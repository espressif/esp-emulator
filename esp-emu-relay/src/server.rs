//! Accepts connections: a WebSocket upgrade opens one TCP or UDP flow, or carries the port
//! forwards; `GET /dns` answers a DNS query, and any other GET reports the verdict for a flow.

use std::collections::HashMap;
use std::io::{self, ErrorKind, Read, Write};
use std::net::{IpAddr, Shutdown, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use rustls::ServerConfig;

use crate::conn::{self, Reader, Writer};
use crate::dns;
use crate::http::{self, Request};
use crate::inbound::{self, Forward, Inbound};
use crate::policy::{Policy, Refusal, Resolver};
use crate::udp;
use crate::ws::{self, Frame, FrameReader};

const HEAD_TIMEOUT: Duration = Duration::from_secs(10);
pub(crate) const POLL: Duration = Duration::from_millis(250);
const CHUNK: usize = 16 * 1024;
/// With `credit=1`, the host bytes that the page has not credited yet; the relay stops reading at this.
pub const WINDOW: u64 = 256 * 1024;
const CREDIT_WAIT: Duration = Duration::from_millis(2);

#[derive(Clone)]
pub struct Config {
    pub policy: Policy,
    pub resolver: Resolver,
    pub max_flows_per_ip: usize,
    pub idle: Duration,
    pub connect_timeout: Duration,
    /// Serve `wss://` and `https://` with this setup instead of plain `ws://` and `http://`.
    pub tls: Option<Arc<ServerConfig>>,
    /// Ports on the relay's address whose connections go to the firmware.
    pub forwards: Vec<Forward>,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            policy: Policy::default(),
            resolver: Resolver::default(),
            max_flows_per_ip: 16,
            idle: Duration::from_secs(300),
            connect_timeout: Duration::from_secs(10),
            tls: None,
            forwards: Vec::new(),
        }
    }
}

#[derive(Default)]
pub(crate) struct Flows(Mutex<HashMap<IpAddr, usize>>);

pub(crate) struct FlowSlot {
    flows: Arc<Flows>,
    ip: IpAddr,
}

impl Flows {
    pub(crate) fn acquire(self: &Arc<Self>, ip: IpAddr, max: usize) -> Option<FlowSlot> {
        let mut map = self.0.lock().unwrap();
        let count = map.entry(ip).or_default();
        if *count >= max {
            return None;
        }
        *count += 1;
        Some(FlowSlot { flows: self.clone(), ip })
    }

    fn full(&self, ip: IpAddr, max: usize) -> bool {
        self.0.lock().unwrap().get(&ip).is_some_and(|&count| count >= max)
    }
}

impl Drop for FlowSlot {
    fn drop(&mut self) {
        let mut map = self.flows.0.lock().unwrap();
        if let Some(count) = map.get_mut(&self.ip) {
            *count -= 1;
            if *count == 0 {
                map.remove(&self.ip);
            }
        }
    }
}

fn printable(text: &str) -> String {
    text.chars().take(253).map(|c| if c.is_ascii_alphanumeric() || "_.:-".contains(c) { c } else { '?' }).collect()
}

fn target(req: &Request) -> (&str, u16) {
    (req.param("host").unwrap_or(""), req.param("port").and_then(|p| p.parse().ok()).unwrap_or(0))
}

/// Serves `listener` until it fails; one thread per connection. Each forward listens on the
/// address of `listener`.
pub fn serve(listener: TcpListener, config: Config) -> io::Result<()> {
    let forwards = bind_forwards(listener.local_addr()?.ip(), &config.forwards)?;
    serve_bound(listener, forwards, config)
}

/// A listener for each forward on `ip`; the error names the port that cannot bind.
pub fn bind_forwards(ip: IpAddr, forwards: &[Forward]) -> io::Result<Vec<(TcpListener, Forward)>> {
    forwards
        .iter()
        .map(|f| match TcpListener::bind((ip, f.host_port)) {
            Ok(bound) => Ok((bound, *f)),
            Err(e) => Err(io::Error::new(e.kind(), format!("forward port {}: {e}", f.host_port))),
        })
        .collect()
}

/// `serve` with the forward listeners already bound; they replace `config.forwards`.
pub fn serve_bound(listener: TcpListener, forwards: Vec<(TcpListener, Forward)>, mut config: Config) -> io::Result<()> {
    config.forwards = forwards.iter().map(|(_, f)| *f).collect();
    let config = Arc::new(config);
    let flows = Arc::new(Flows::default());
    let inbound = Arc::new(Inbound::default());
    for (bound, forward) in forwards {
        let inbound = inbound.clone();
        thread::spawn(move || inbound::accept_loop(bound, forward, &inbound));
    }
    loop {
        let (stream, _) = listener.accept()?;
        let (config, flows, inbound) = (config.clone(), flows.clone(), inbound.clone());
        thread::spawn(move || handle(stream, &config, &flows, &inbound));
    }
}

fn handle(stream: TcpStream, config: &Config, flows: &Arc<Flows>, inbound: &Inbound) {
    let Ok((mut reader, writer)) = conn::split(stream, config.tls.as_ref()) else { return };
    let _ = reader.set_read_timeout(HEAD_TIMEOUT);
    let Ok((req, head)) = http::read_request(&mut reader) else { return };
    if req.header("upgrade").is_some() {
        match req.path.as_str() {
            "/listen" => inbound::listen(reader, writer, &req, head, config, inbound),
            "/accept" => inbound::accept(reader, writer, &req, head, config, flows, inbound),
            _ => upgrade(reader, writer, &req, head, config, flows),
        }
        return;
    }
    let full = reader.peer_addr().is_ok_and(|a| flows.full(a.ip(), config.max_flows_per_ip));
    let reply = match req.path.as_str() {
        "/dns" => dns_query(&req, config),
        "/version" => version(),
        _ => verdict(&req, config, full),
    };
    let _ = writer.write_all(&reply);
    writer.shutdown();
}

// For any page: the version says nothing about this computer, and a page checks it before it connects
fn version() -> Vec<u8> {
    let headers = [("Access-Control-Allow-Origin", "*"), ("Content-Type", "application/json")];
    http::response(200, &headers, &format!("{{\"name\":\"esp-emu-relay\",\"version\":\"{}\"}}", crate::VERSION))
}

fn dns_query(req: &Request, config: &Config) -> Vec<u8> {
    let origin = req.header("origin").unwrap_or("");
    let cors = [("Access-Control-Allow-Origin", origin), ("Vary", "Origin")];
    if req.method != "GET" || !config.policy.origin_allowed(req.header("origin")) {
        return http::response(403, &cors, Refusal::ORIGIN.reason);
    }
    match req.param("dns").and_then(dns::base64url_decode).and_then(|q| dns::resolve(&q, &config.policy, &config.resolver)) {
        Some(answer) => http::binary_response(200, &[cors[0], cors[1], ("Content-Type", "application/dns-message")], &answer),
        None => http::response(400, &cors, Refusal::BAD_REQUEST.reason),
    }
}

// Why the relay refuses a flow, for the page; `full` when the page's address has no free flow slot
fn verdict(req: &Request, config: &Config, full: bool) -> Vec<u8> {
    let origin = req.header("origin");
    let flow_path = req.path == "/tcp" || req.path == "/udp";
    let Some(origin) = origin.filter(|_| req.method == "GET" && flow_path) else {
        return http::response(403, &[], "");
    };
    let (host, port) = target(req);
    let result = if !config.policy.origin_allowed(Some(origin)) {
        Err(Refusal::ORIGIN)
    } else if full {
        Err(Refusal::TOO_MANY)
    } else {
        config.policy.check(host, port, &config.resolver).map(|_| ())
    };
    let (status, reason) = match result {
        Ok(()) => (204, ""),
        Err(r) => (r.status, r.reason),
    };
    let cors = [("Access-Control-Allow-Origin", origin), ("Vary", "Origin")];
    http::response(status, &cors, reason)
}

pub(crate) fn refuse(writer: Writer, refusal: Refusal, host: &str, port: u16) {
    println!("refused {}:{port} ({})", printable(host), refusal.reason);
    let _ = writer.write_all(&http::response(refusal.status, &[], ""));
    writer.shutdown();
}

fn upgrade(reader: Reader, writer: Writer, req: &Request, head: Vec<u8>, config: &Config, flows: &Arc<Flows>) {
    let (host, port) = target(req);
    let udp = req.path == "/udp";
    let Some(key) = ws::upgrade_key(req).filter(|_| udp || req.path == "/tcp") else {
        return refuse(writer, Refusal::BAD_REQUEST, host, port);
    };
    if !config.policy.origin_allowed(req.header("origin")) {
        return refuse(writer, Refusal::ORIGIN, host, port);
    }
    let Some(slot) = reader.peer_addr().ok().and_then(|a| flows.acquire(a.ip(), config.max_flows_per_ip)) else {
        return refuse(writer, Refusal::TOO_MANY, host, port);
    };
    let addr = match config.policy.check(host, port, &config.resolver) {
        Ok(addr) => addr,
        Err(r) => return refuse(writer, r, host, port),
    };
    if udp {
        let unspecified: IpAddr = if addr.is_ipv4() { [0u8; 4].into() } else { [0u16; 8].into() };
        let Ok(sock) = UdpSocket::bind((unspecified, 0)).and_then(|s| s.connect(addr).map(|_| s)) else {
            return refuse(writer, Refusal::CONNECT, host, port);
        };
        if writer.write_all(&ws::accept_response(key)).is_ok() {
            udp::bridge(reader, writer, head, sock, config.idle, &format!("udp {}:{port}", printable(host)));
        }
        drop(slot);
        return;
    }
    let Ok(tcp) = TcpStream::connect_timeout(&addr, config.connect_timeout) else {
        return refuse(writer, Refusal::CONNECT, host, port);
    };
    if writer.write_all(&ws::accept_response(key)).is_err() {
        return;
    }
    let label = format!("{}:{port}", printable(host));
    bridge(reader, writer, head, tcp, config.idle, credit(req), &label);
    drop(slot);
}

pub(crate) struct WsOut {
    writer: Writer,
    pub(crate) closed: bool,
    /// The page sent its FIN (an empty message).
    page_fin: bool,
    /// The host sent its FIN, and the page got it as an empty message.
    host_fin: bool,
}

impl WsOut {
    /// False once the WebSocket is closed or its socket fails.
    pub(crate) fn send(&mut self, opcode: u8, payload: &[u8]) -> bool {
        !self.closed && self.writer.write_all(&ws::encode(opcode, payload)).is_ok()
    }

    /// Record one side's FIN; the flow closes normally when both sides sent one.
    fn fin(&mut self, page: bool) {
        if page {
            self.page_fin = true;
        } else {
            self.host_fin = self.send(ws::OP_BINARY, &[]);
        }
        if self.page_fin && self.host_fin {
            self.close(ws::CLOSE_NORMAL);
        }
    }

    pub(crate) fn close(&mut self, code: u16) {
        if !self.closed {
            self.closed = true;
            let _ = self.writer.write_all(&ws::encode_close(code));
            self.writer.shutdown();
        }
    }
}

/// True when the page asked for flow control with `credit=1`.
pub(crate) fn credit(req: &Request) -> bool {
    req.param("credit") == Some("1")
}

pub(crate) struct Shared {
    pub(crate) out: Mutex<WsOut>,
    /// The page credits the host bytes that it took, in text messages.
    credit: bool,
    acked: AtomicU64,
    pub(crate) started: Instant,
    last_active_ms: AtomicU64,
    pub(crate) up: AtomicU64,
    pub(crate) down: AtomicU64,
    pub(crate) done: AtomicBool,
}

impl Shared {
    pub(crate) fn new(writer: Writer) -> Self {
        Shared {
            out: Mutex::new(WsOut { writer, closed: false, page_fin: false, host_fin: false }),
            credit: false,
            acked: AtomicU64::new(0),
            started: Instant::now(),
            last_active_ms: AtomicU64::new(0),
            up: AtomicU64::new(0),
            down: AtomicU64::new(0),
            done: AtomicBool::new(false),
        }
    }

    pub(crate) fn touch(&self) {
        self.last_active_ms.store(self.started.elapsed().as_millis() as u64, Ordering::Relaxed);
    }

    pub(crate) fn idle_for(&self) -> Duration {
        self.started.elapsed().saturating_sub(Duration::from_millis(self.last_active_ms.load(Ordering::Relaxed)))
    }
}

pub(crate) fn timed_out(e: &io::Error) -> bool {
    matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted)
}

pub(crate) fn bridge(ws: Reader, writer: Writer, head: Vec<u8>, tcp: TcpStream, idle: Duration, credit: bool, label: &str) {
    let Ok(tcp_in) = tcp.try_clone() else { return };
    let _ = ws.set_nodelay();
    let _ = ws.set_read_timeout(POLL);
    let _ = tcp.set_nodelay(true);
    let _ = tcp.set_read_timeout(Some(POLL));
    let shared = Arc::new(Shared { credit, ..Shared::new(writer) });
    let upstream = {
        let shared = shared.clone();
        thread::spawn(move || ws_to_tcp(ws, head, tcp, &shared))
    };
    let was_idle = tcp_to_ws(tcp_in, &shared, idle);
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

fn ws_to_tcp(mut ws: Reader, head: Vec<u8>, mut tcp: TcpStream, shared: &Shared) {
    let mut reader = FrameReader::default();
    reader.push(&head);
    let mut buf = vec![0u8; CHUNK];
    loop {
        loop {
            match reader.next() {
                Ok(Some(Frame::Data(data))) if data.is_empty() => {
                    let _ = tcp.shutdown(Shutdown::Write);
                    shared.out.lock().unwrap().fin(true);
                }
                Ok(Some(Frame::Data(data))) => {
                    shared.up.fetch_add(data.len() as u64, Ordering::Relaxed);
                    shared.touch();
                    if tcp.write_all(&data).is_err() {
                        let _ = tcp.shutdown(Shutdown::Both);
                        return;
                    }
                }
                Ok(Some(Frame::Ping(payload))) => {
                    shared.out.lock().unwrap().send(ws::OP_PONG, &payload);
                }
                Ok(Some(Frame::Pong)) => {}
                Ok(Some(Frame::Text(text))) => {
                    let Some(n) = std::str::from_utf8(&text).ok().and_then(|t| t.parse::<u64>().ok()).filter(|_| shared.credit) else {
                        shared.out.lock().unwrap().close(ws::CLOSE_UNSUPPORTED);
                        let _ = tcp.shutdown(Shutdown::Both);
                        return;
                    };
                    shared.acked.fetch_add(n, Ordering::Relaxed);
                    shared.touch();
                }
                Ok(Some(Frame::Close(ws::CLOSE_NORMAL))) => {
                    shared.out.lock().unwrap().close(ws::CLOSE_NORMAL);
                    let _ = tcp.shutdown(Shutdown::Write);
                    return;
                }
                Ok(Some(Frame::Close(code))) | Err(code) => {
                    // Any other close aborts the flow
                    let reply = if code == ws::CLOSE_NO_STATUS { ws::CLOSE_NORMAL } else { code };
                    shared.out.lock().unwrap().close(reply);
                    let _ = tcp.shutdown(Shutdown::Both);
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
                // The page vanished without a close frame: reset the host side
                shared.out.lock().unwrap().closed = true;
                let _ = tcp.shutdown(Shutdown::Both);
                return;
            }
        }
    }
}

/// Returns true when the flow ended on the idle timeout.
fn tcp_to_ws(mut tcp: TcpStream, shared: &Shared, idle: Duration) -> bool {
    let mut buf = vec![0u8; CHUNK];
    loop {
        if let Some(was_idle) = wait_credit(shared, idle) {
            let _ = tcp.shutdown(Shutdown::Both);
            return was_idle;
        }
        match tcp.read(&mut buf) {
            Ok(0) => {
                shared.out.lock().unwrap().fin(false);
                return wait_close(shared, idle);
            }
            Ok(n) => {
                shared.down.fetch_add(n as u64, Ordering::Relaxed);
                shared.touch();
                if !shared.out.lock().unwrap().send(ws::OP_BINARY, &buf[..n]) {
                    let _ = tcp.shutdown(Shutdown::Both);
                    return false;
                }
            }
            Err(e) if timed_out(&e) => {
                let closed = shared.out.lock().unwrap().closed;
                let was_idle = shared.idle_for() > idle;
                if closed || was_idle {
                    shared.out.lock().unwrap().close(ws::CLOSE_ERROR);
                    let _ = tcp.shutdown(Shutdown::Both);
                    return was_idle;
                }
            }
            Err(_) => {
                shared.out.lock().unwrap().close(ws::CLOSE_ERROR);
                return false;
            }
        }
    }
}

// Wait until the page's credit leaves room in the window; `Some(was_idle)` when the flow ends instead
fn wait_credit(shared: &Shared, idle: Duration) -> Option<bool> {
    while shared.credit && shared.down.load(Ordering::Relaxed) >= shared.acked.load(Ordering::Relaxed) + WINDOW {
        let mut out = shared.out.lock().unwrap();
        if out.closed {
            return Some(false);
        }
        if shared.idle_for() > idle {
            out.close(ws::CLOSE_ERROR);
            return Some(true);
        }
        drop(out);
        thread::sleep(CREDIT_WAIT);
    }
    None
}

// After the host's FIN: wait for the page's FIN or close; true on the idle timeout
fn wait_close(shared: &Shared, idle: Duration) -> bool {
    loop {
        let mut out = shared.out.lock().unwrap();
        if out.closed {
            return false;
        }
        if shared.idle_for() > idle {
            out.close(ws::CLOSE_ERROR);
            return true;
        }
        drop(out);
        thread::sleep(POLL);
    }
}
