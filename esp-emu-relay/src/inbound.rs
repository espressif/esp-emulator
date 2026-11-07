//! Port forwards to the firmware. A page holds one `/listen` WebSocket, which gets a message
//! for each connection to a forward port; the page claims the connection with `/accept?id=N`.

use std::collections::HashMap;
use std::io::Read;
use std::net::{TcpListener, TcpStream};
use std::str::FromStr;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::conn::{Reader, Writer};
use crate::http::Request;
use crate::policy::Refusal;
use crate::server::{bridge, credit, refuse, timed_out, Config, Flows, POLL};
use crate::ws::{self, Frame, FrameReader};

/// `[1][host port: u16 BE][firmware port: u16 BE]`, one per forward when `/listen` opens.
pub const MSG_FORWARD: u8 = 1;
/// `[2][id: u32 BE][firmware port: u16 BE]`, one per connection to a forward port.
pub const MSG_CONNECTION: u8 = 2;
/// The close code for a `/listen` WebSocket that a newer one replaced.
pub const CLOSE_REPLACED: u16 = 4000;
/// A connection that no page claims in this time is closed.
pub const CLAIM_TIMEOUT: Duration = Duration::from_secs(10);

/// Connections to `host_port` on the relay's address go to `guest_port` on the firmware.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Forward {
    pub host_port: u16,
    pub guest_port: u16,
}

impl FromStr for Forward {
    type Err = String;

    /// `HPORT:FPORT`, such as `8080:80`.
    fn from_str(s: &str) -> Result<Self, String> {
        let port = |p: &str| p.trim().parse::<u16>().ok().filter(|&p| p != 0);
        match s.split_once(':').map(|(h, g)| (port(h), port(g))) {
            Some((Some(host_port), Some(guest_port))) => Ok(Forward { host_port, guest_port }),
            _ => Err(format!("'{s}' is not HPORT:FPORT")),
        }
    }
}

struct Pending {
    stream: TcpStream,
    forward: Forward,
    since: Instant,
}

/// The page that takes the connections, and the connections that it did not claim yet.
#[derive(Default)]
pub(crate) struct Inbound {
    page: Mutex<Option<(u64, Writer)>>,
    pending: Mutex<HashMap<u32, Pending>>,
    next_id: AtomicU32,
    next_page: AtomicU64,
}

impl Inbound {
    fn expire(&self) {
        self.pending.lock().unwrap().retain(|_, p| p.since.elapsed() < CLAIM_TIMEOUT);
    }

    // Tell the page about `stream`; with no page, dropping `stream` closes it
    fn offer(&self, stream: TcpStream, forward: Forward) {
        self.expire();
        let page = self.page.lock().unwrap();
        let Some((_, writer)) = page.as_ref() else {
            println!("inbound :{} closed: no page listens", forward.host_port);
            return;
        };
        let id = self.next_id.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
        let mut msg = vec![MSG_CONNECTION];
        msg.extend_from_slice(&id.to_be_bytes());
        msg.extend_from_slice(&forward.guest_port.to_be_bytes());
        self.pending.lock().unwrap().insert(id, Pending { stream, forward, since: Instant::now() });
        if writer.write_all(&ws::encode(ws::OP_BINARY, &msg)).is_err() {
            self.pending.lock().unwrap().remove(&id);
        }
    }
}

pub(crate) fn accept_loop(listener: TcpListener, forward: Forward, inbound: &Inbound) {
    for stream in listener.incoming().flatten() {
        inbound.offer(stream, forward);
    }
}

/// The page's `/listen` WebSocket: it replaces an earlier one, and stays until the page closes it.
pub(crate) fn listen(mut reader: Reader, writer: Writer, req: &Request, head: Vec<u8>, config: &Config, inbound: &Inbound) {
    let Some(key) = ws::upgrade_key(req) else { return refuse(writer, Refusal::BAD_REQUEST, "listen", 0) };
    if !config.policy.origin_allowed(req.header("origin")) {
        return refuse(writer, Refusal::ORIGIN, "listen", 0);
    }
    if writer.write_all(&ws::accept_response(key)).is_err() {
        return;
    }
    for f in &config.forwards {
        let mut msg = vec![MSG_FORWARD];
        msg.extend_from_slice(&f.host_port.to_be_bytes());
        msg.extend_from_slice(&f.guest_port.to_be_bytes());
        if writer.write_all(&ws::encode(ws::OP_BINARY, &msg)).is_err() {
            return;
        }
    }
    let id = inbound.next_page.fetch_add(1, Ordering::Relaxed);
    if let Some((_, old)) = inbound.page.lock().unwrap().replace((id, writer.clone())) {
        let _ = old.write_all(&ws::encode_close(CLOSE_REPLACED));
        old.shutdown();
    }
    println!("A page takes the inbound connections");
    let _ = reader.set_read_timeout(POLL);
    let mut frames = FrameReader::default();
    frames.push(&head);
    let mut buf = [0u8; 1024];
    'page: loop {
        loop {
            match frames.next() {
                Ok(Some(Frame::Ping(payload))) => {
                    let _ = writer.write_all(&ws::encode(ws::OP_PONG, &payload));
                }
                Ok(Some(Frame::Data(_) | Frame::Text(_) | Frame::Pong)) => {}
                Ok(Some(Frame::Close(code))) | Err(code) => {
                    let reply = if code == ws::CLOSE_NO_STATUS { ws::CLOSE_NORMAL } else { code };
                    let _ = writer.write_all(&ws::encode_close(reply));
                    writer.shutdown();
                    break 'page;
                }
                Ok(None) => break,
            }
        }
        match reader.read(&mut buf) {
            Ok(n) if n > 0 => frames.push(&buf[..n]),
            Err(e) if timed_out(&e) => inbound.expire(),
            _ => break,
        }
    }
    let mut page = inbound.page.lock().unwrap();
    if page.as_ref().is_some_and(|(p, _)| *p == id) {
        *page = None;
    }
}

/// The page's `/accept?id=N` WebSocket: carries connection N as a TCP flow, as `/tcp` does.
pub(crate) fn accept(
    reader: Reader,
    writer: Writer,
    req: &Request,
    head: Vec<u8>,
    config: &Config,
    flows: &Arc<Flows>,
    inbound: &Inbound,
) {
    let Some(key) = ws::upgrade_key(req) else { return refuse(writer, Refusal::BAD_REQUEST, "accept", 0) };
    if !config.policy.origin_allowed(req.header("origin")) {
        return refuse(writer, Refusal::ORIGIN, "accept", 0);
    }
    let id = req.param("id").and_then(|id| id.parse::<u32>().ok());
    let Some(pending) = id.and_then(|id| inbound.pending.lock().unwrap().remove(&id)) else {
        return refuse(writer, Refusal::GONE, "accept", 0);
    };
    let host_port = pending.forward.host_port;
    let Some(slot) = reader.peer_addr().ok().and_then(|a| flows.acquire(a.ip(), config.max_flows_per_ip)) else {
        return refuse(writer, Refusal::TOO_MANY, "inbound", host_port);
    };
    if writer.write_all(&ws::accept_response(key)).is_err() {
        return;
    }
    let label = format!("inbound :{host_port} -> firmware :{}", pending.forward.guest_port);
    let _ = pending.stream.set_nodelay(true);
    bridge(reader, writer, head, pending.stream, config.idle, credit(req), &label);
    drop(slot);
}
