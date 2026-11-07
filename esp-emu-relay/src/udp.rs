//! One UDP flow: each binary WebSocket message is one datagram, in both directions.

use std::io::{ErrorKind, Read};
use std::net::UdpSocket;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use crate::conn::{Reader, Writer};
use crate::server::{timed_out, Shared, POLL};
use crate::ws::{self, Frame, FrameReader};

const MAX_DATAGRAM: usize = 65_535;

/// Carries datagrams between the page's WebSocket and `sock`, a socket connected to the host.
pub(crate) fn bridge(ws: Reader, writer: Writer, head: Vec<u8>, sock: UdpSocket, idle: Duration, label: &str) {
    let Ok(sock_in) = sock.try_clone() else { return };
    let _ = ws.set_nodelay();
    let _ = ws.set_read_timeout(POLL);
    let _ = sock.set_read_timeout(Some(POLL));
    let shared = Arc::new(Shared::new(writer));
    let upstream = {
        let shared = shared.clone();
        thread::spawn(move || ws_to_udp(ws, head, sock, &shared))
    };
    let was_idle = udp_to_ws(sock_in, &shared, idle);
    shared.done.store(true, Ordering::Relaxed);
    let _ = upstream.join();
    println!(
        "{label} up {} B, down {} B, {:.1} s{}",
        shared.up.load(Ordering::Relaxed),
        shared.down.load(Ordering::Relaxed),
        shared.started.elapsed().as_secs_f64(),
        if was_idle { ", idle" } else { "" }
    );
}

fn ws_to_udp(mut ws: Reader, head: Vec<u8>, sock: UdpSocket, shared: &Shared) {
    let mut reader = FrameReader::default();
    reader.push(&head);
    let mut buf = vec![0u8; 16 * 1024];
    loop {
        loop {
            match reader.next() {
                Ok(Some(Frame::Data(data))) => {
                    shared.up.fetch_add(data.len() as u64, Ordering::Relaxed);
                    shared.touch();
                    // A lost datagram is normal for UDP, so a send error does not end the flow
                    let _ = sock.send(&data);
                }
                Ok(Some(Frame::Ping(payload))) => {
                    shared.out.lock().unwrap().send(ws::OP_PONG, &payload);
                }
                Ok(Some(Frame::Pong)) => {}
                Ok(Some(Frame::Text(_))) => {
                    shared.out.lock().unwrap().close(ws::CLOSE_UNSUPPORTED);
                    return;
                }
                Ok(Some(Frame::Close(code))) | Err(code) => {
                    let reply = if code == ws::CLOSE_NO_STATUS { ws::CLOSE_NORMAL } else { code };
                    shared.out.lock().unwrap().close(reply);
                    return;
                }
                Ok(None) => break,
            }
        }
        if shared.done.load(Ordering::Relaxed) {
            return;
        }
        match ws.read(&mut buf) {
            Ok(n) if n > 0 => reader.push(&buf[..n]),
            Err(e) if timed_out(&e) => {}
            _ => {
                shared.out.lock().unwrap().closed = true;
                return;
            }
        }
    }
}

/// Returns true when the flow ended on the idle timeout.
fn udp_to_ws(sock: UdpSocket, shared: &Shared, idle: Duration) -> bool {
    let mut buf = vec![0u8; MAX_DATAGRAM];
    loop {
        match sock.recv(&mut buf) {
            Ok(n) => {
                shared.down.fetch_add(n as u64, Ordering::Relaxed);
                shared.touch();
                if !shared.out.lock().unwrap().send(ws::OP_BINARY, &buf[..n]) {
                    return false;
                }
            }
            Err(e) if timed_out(&e) => {
                let closed = shared.out.lock().unwrap().closed;
                let was_idle = shared.idle_for() > idle;
                if closed || was_idle {
                    shared.out.lock().unwrap().close(ws::CLOSE_NORMAL);
                    return was_idle;
                }
            }
            // An ICMP port unreachable for an earlier datagram; the host can still answer later ones
            Err(e) if matches!(e.kind(), ErrorKind::ConnectionRefused | ErrorKind::ConnectionReset) => {}
            Err(_) => {
                shared.out.lock().unwrap().close(ws::CLOSE_ERROR);
                return false;
            }
        }
    }
}
