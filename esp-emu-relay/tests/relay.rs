use std::io::{Read, Write};
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::thread;
use std::time::Duration;

use esp_emu_relay::http::{percent_decode, read_request};
use esp_emu_relay::inbound::{Forward, CLOSE_REPLACED, MSG_CONNECTION, MSG_FORWARD};
use esp_emu_relay::policy::{is_public, pattern_matches, Policy, Refusal, Resolver};
use esp_emu_relay::server::{serve, serve_bound, Config, WINDOW};
use esp_emu_relay::ws::{accept_key, Frame, FrameReader, CLOSE_PROTOCOL, CLOSE_UNSUPPORTED};
use tungstenite::client::IntoClientRequest;
use tungstenite::protocol::frame::coding::CloseCode;
use tungstenite::{Error, Message};

fn any_address(_: IpAddr) -> bool {
    true
}

fn loopback_ok() -> Resolver {
    Resolver { address_ok: any_address, ..Resolver::default() }
}

fn fixed_lookup(_: &str, port: u16) -> std::io::Result<Vec<SocketAddr>> {
    Ok(vec![SocketAddr::from(([10, 0, 0, 1], port)), SocketAddr::from(([93, 184, 215, 14], port))])
}

#[test]
fn star_matches_one_label() {
    assert!(pattern_matches("*.cloud.example.com", "api.Cloud.Example.com"));
    assert!(!pattern_matches("*.cloud.example.com", "cloud.example.com"));
    assert!(!pattern_matches("*.cloud.example.com", "a.b.cloud.example.com"));
    assert!(pattern_matches("*.iot.*.example.net", "a1b2-ats.iot.region-1.example.net"));
    assert!(pattern_matches("https://*.*.previews.example.com", "https://mr-1.app.previews.example.com"));
    assert!(!pattern_matches("http://localhost:5173", "http://localhost:51730"));
}

#[test]
fn public_address_table() {
    for private in ["0.1.2.3", "10.1.1.1", "100.64.0.1", "127.0.0.1", "169.254.169.254", "172.31.0.1",
        "192.168.1.1", "198.19.0.1", "224.0.0.251", "255.255.255.255", "168.63.129.16", "::", "::1",
        "::a00:1", "::ffff:8.8.8.8", "64:ff9b::808:808", "64:ff9b:1::a00:1", "2001:db8::1", "2002::1",
        "fd00::1", "fe80::1", "ff02::fb"]
    {
        assert!(!is_public(private.parse().unwrap()), "{private} counted as public");
    }
    for public in ["8.8.8.8", "1.1.1.1", "172.32.0.1", "100.128.0.1", "2606:4700:4700::1111"] {
        assert!(is_public(public.parse().unwrap()), "{public} counted as private");
    }
}

#[test]
fn no_allowlist_allows_any_public_host() {
    let resolver = Resolver { lookup: fixed_lookup, ..Resolver::default() };
    let policy = Policy::default();
    assert!(policy.origin_allowed(None));
    assert!(policy.origin_allowed(Some("https://anything.example")));
    let addr = policy.check("Example.COM.", 4242, &resolver).unwrap();
    assert_eq!(addr, SocketAddr::from(([93, 184, 215, 14], 4242)), "the first public address wins");
}

#[test]
fn allowlists_limit_origins_hosts_and_ports() {
    let resolver = Resolver { lookup: fixed_lookup, ..Resolver::default() };
    let policy = Policy {
        origins: vec!["https://*.example.com".into()],
        hosts: vec!["*.example.com".into()],
        ports: vec![443],
    };
    assert!(policy.origin_allowed(Some("https://app.example.com")));
    assert!(!policy.origin_allowed(Some("https://evil.test")));
    assert!(!policy.origin_allowed(None));
    assert!(policy.check("mqtt.example.com", 443, &resolver).is_ok());
    assert_eq!(policy.check("mqtt.example.org", 443, &resolver), Err(Refusal::HOST));
    assert_eq!(policy.check("93.184.215.14", 443, &resolver), Err(Refusal::HOST));
    assert_eq!(policy.check("mqtt.example.com", 8883, &resolver), Err(Refusal::PORT));
}

#[test]
fn check_refuses_bad_and_private_targets() {
    let policy = Policy::default();
    let resolver = Resolver::default();
    assert_eq!(policy.check("", 443, &resolver), Err(Refusal::BAD_HOST));
    assert_eq!(policy.check("a b", 443, &resolver), Err(Refusal::BAD_HOST));
    assert_eq!(policy.check("example.com", 0, &resolver), Err(Refusal::BAD_PORT));
    assert_eq!(policy.check("127.0.0.1", 443, &resolver), Err(Refusal::NOT_PUBLIC));
    assert_eq!(policy.check("::1", 443, &resolver), Err(Refusal::NOT_PUBLIC));
    assert_eq!(policy.check("localhost", 443, &resolver), Err(Refusal::NOT_PUBLIC));
    let failing = Resolver { lookup: |_, _| Err(std::io::ErrorKind::NotFound.into()), ..Resolver::default() };
    assert_eq!(policy.check("example.com", 443, &failing), Err(Refusal::LOOKUP));
}

#[test]
fn request_head_and_query() {
    let raw = b"GET /tcp?host=a%2Eexample.com&port=443 HTTP/1.1\r\nOrigin: https://x\r\nUpgrade: websocket\r\n\r\nextra";
    let (req, rest) = read_request(&mut &raw[..]).unwrap();
    assert_eq!((req.method.as_str(), req.path.as_str()), ("GET", "/tcp"));
    assert_eq!(req.param("host"), Some("a.example.com"));
    assert_eq!(req.param("port"), Some("443"));
    assert_eq!(req.header("origin"), Some("https://x"));
    assert_eq!(rest, b"extra");
    assert_eq!(percent_decode("a+b%2"), "a b%2");
}

fn masked(opcode: u8, payload: &[u8]) -> Vec<u8> {
    masked_frame(0x80 | opcode, payload)
}

// `b0` carries the FIN bit and the opcode
fn masked_frame(b0: u8, payload: &[u8]) -> Vec<u8> {
    let mask = [0x11, 0x22, 0x33, 0x44];
    let mut out = vec![b0];
    if payload.len() < 126 {
        out.push(0x80 | payload.len() as u8);
    } else {
        out.push(0x80 | 126);
        out.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    }
    out.extend_from_slice(&mask);
    out.extend(payload.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
    out
}

#[test]
fn websocket_frames() {
    assert_eq!(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    let mut reader = FrameReader::default();
    let big = vec![7u8; 300];
    let mut bytes = masked(2, b"hello");
    bytes.extend(masked(2, &big));
    bytes.extend(masked(9, b"p"));
    bytes.extend(masked(8, &1001u16.to_be_bytes()));
    let (first, second) = bytes.split_at(9);
    reader.push(first);
    assert_eq!(reader.next(), Ok(None), "a partial frame waits for more bytes");
    reader.push(second);
    assert_eq!(reader.next(), Ok(Some(Frame::Data(b"hello".to_vec()))));
    assert_eq!(reader.next(), Ok(Some(Frame::Data(big))));
    assert_eq!(reader.next(), Ok(Some(Frame::Ping(b"p".to_vec()))));
    assert_eq!(reader.next(), Ok(Some(Frame::Close(1001))));
    assert_eq!(reader.next(), Ok(None));

    let mut text = FrameReader::default();
    text.push(&masked(1, b"65536"));
    assert_eq!(text.next(), Ok(Some(Frame::Text(b"65536".to_vec()))));
    let mut long = FrameReader::default();
    long.push(&masked(1, &[b'1'; 33]));
    assert_eq!(long.next(), Err(CLOSE_UNSUPPORTED), "a credit is a short text message");
    let mut split = FrameReader::default();
    split.push(&masked_frame(0x01, b"1"));
    assert_eq!(split.next(), Err(CLOSE_UNSUPPORTED), "a credit comes in one frame");
    let mut unmasked = FrameReader::default();
    unmasked.push(&[0x82, 0x01, 0x00]);
    assert_eq!(unmasked.next(), Err(CLOSE_PROTOCOL));
}

#[test]
fn fragmented_message_is_one_data_frame() {
    let mut reader = FrameReader::default();
    let mut bytes = masked_frame(0x02, b"one ");
    bytes.extend(masked(9, b"p"));
    bytes.extend(masked_frame(0x00, b"two "));
    bytes.extend(masked_frame(0x80, b"three"));
    reader.push(&bytes);
    assert_eq!(reader.next(), Ok(Some(Frame::Ping(b"p".to_vec()))), "a control frame can come between fragments");
    assert_eq!(reader.next(), Ok(Some(Frame::Data(b"one two three".to_vec()))));
    assert_eq!(reader.next(), Ok(None));

    let mut stray = FrameReader::default();
    stray.push(&masked_frame(0x80, b"x"));
    assert_eq!(stray.next(), Err(CLOSE_PROTOCOL), "a continuation needs a first fragment");
    let mut interleaved = FrameReader::default();
    interleaved.push(&masked_frame(0x02, b"a"));
    interleaved.push(&masked(2, b"b"));
    assert_eq!(interleaved.next(), Err(CLOSE_PROTOCOL), "a new message cannot start inside another");
}

#[test]
fn forward_parses_hport_fport() {
    assert_eq!("8080:80".parse(), Ok(Forward { host_port: 8080, guest_port: 80 }));
    for bad in ["80", "0:80", "8080:0", "a:80", "8080:80:1", "70000:80"] {
        assert!(bad.parse::<Forward>().is_err(), "{bad} parsed");
    }
}

fn start_relay(config: Config) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    thread::spawn(move || serve(listener, config));
    port
}

/// A host that sends `greeting`, echoes until EOF, then closes.
fn start_echo(greeting: &'static [u8]) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    thread::spawn(move || {
        for stream in listener.incoming() {
            let mut stream = stream.unwrap();
            thread::spawn(move || {
                stream.write_all(greeting).unwrap();
                let mut buf = [0u8; 4096];
                while let Ok(n) = stream.read(&mut buf) {
                    if n == 0 || stream.write_all(&buf[..n]).is_err() {
                        break;
                    }
                }
            });
        }
    });
    port
}

type Client = tungstenite::WebSocket<tungstenite::stream::MaybeTlsStream<TcpStream>>;

fn connect(relay: u16, host: &str, port: u16, origin: &str) -> Result<Client, Error> {
    ws_connect(relay, &format!("/tcp?host={host}&port={port}"), origin)
}

fn ws_connect(relay: u16, path: &str, origin: &str) -> Result<Client, Error> {
    let mut req = format!("ws://127.0.0.1:{relay}{path}").into_client_request().unwrap();
    req.headers_mut().insert("Origin", origin.parse().unwrap());
    tungstenite::connect(req).map(|(ws, _)| ws)
}

fn refused_status(result: Result<Client, Error>) -> u16 {
    match result {
        Err(Error::Http(response)) => response.status().as_u16(),
        Err(e) => panic!("expected an HTTP refusal, got {e}"),
        Ok(_) => panic!("expected an HTTP refusal, got a WebSocket"),
    }
}

fn read_binary(ws: &mut Client) -> Vec<u8> {
    let mut got = Vec::new();
    while let Message::Binary(data) = ws.read().unwrap() {
        got.extend_from_slice(&data);
        if !got.is_empty() {
            return got;
        }
    }
    panic!("expected binary data")
}

#[test]
fn relays_bytes_both_ways_and_closes_on_page_close() {
    let echo = start_echo(b"hi");
    let relay = start_relay(Config { resolver: loopback_ok(), ..Config::default() });
    let mut ws = connect(relay, "127.0.0.1", echo, "https://any.example").unwrap();
    assert_eq!(read_binary(&mut ws), b"hi");
    ws.send(Message::Binary(b"ping-pong".to_vec().into())).unwrap();
    assert_eq!(read_binary(&mut ws), b"ping-pong");
    ws.close(None).unwrap();
    loop {
        match ws.read() {
            Ok(Message::Close(_)) | Err(Error::ConnectionClosed) => break,
            Ok(_) => {}
            Err(e) => panic!("unexpected error after close: {e}"),
        }
    }
}

fn expect_fin(ws: &mut Client) {
    match ws.read().unwrap() {
        Message::Binary(data) => assert!(data.is_empty(), "expected the host's FIN, got {data:?}"),
        other => panic!("expected an empty binary message, got {other:?}"),
    }
}

fn expect_normal_close(ws: &mut Client) {
    match ws.read().unwrap() {
        Message::Close(Some(frame)) => assert_eq!(frame.code, CloseCode::Normal),
        other => panic!("expected a close frame, got {other:?}"),
    }
}

fn send_fin(ws: &mut Client) {
    ws.send(Message::Binary(Vec::new().into())).unwrap();
}

#[test]
fn host_fin_is_an_empty_message_and_both_fins_close_normally() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let host = listener.local_addr().unwrap().port();
    let (tx, rx) = std::sync::mpsc::channel();
    thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream.write_all(b"bye").unwrap();
        stream.shutdown(std::net::Shutdown::Write).unwrap();
        let mut after = Vec::new();
        stream.read_to_end(&mut after).unwrap();
        tx.send(after).unwrap();
    });
    let relay = start_relay(Config { resolver: loopback_ok(), ..Config::default() });
    let mut ws = connect(relay, "127.0.0.1", host, "https://any.example").unwrap();
    assert_eq!(read_binary(&mut ws), b"bye");
    expect_fin(&mut ws);
    ws.send(Message::Binary(b"after host fin".to_vec().into())).unwrap();
    send_fin(&mut ws);
    assert_eq!(rx.recv_timeout(Duration::from_secs(5)).unwrap(), b"after host fin", "the page can send after the host's FIN");
    expect_normal_close(&mut ws);
}

#[test]
fn host_answers_after_the_page_fin() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let host = listener.local_addr().unwrap().port();
    thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = Vec::new();
        stream.read_to_end(&mut request).unwrap();
        stream.write_all(&[b"re: ", &request[..]].concat()).unwrap();
    });
    let relay = start_relay(Config { resolver: loopback_ok(), ..Config::default() });
    let mut ws = connect(relay, "127.0.0.1", host, "https://any.example").unwrap();
    ws.send(Message::Binary(b"req".to_vec().into())).unwrap();
    send_fin(&mut ws);
    assert_eq!(read_binary(&mut ws), b"re: req");
    expect_fin(&mut ws);
    expect_normal_close(&mut ws);
}

#[test]
fn default_relay_refuses_loopback_targets() {
    let echo = start_echo(b"");
    let relay = start_relay(Config::default());
    assert_eq!(refused_status(connect(relay, "127.0.0.1", echo, "https://any.example")), 403);
}

#[test]
fn origin_allowlist_and_flow_cap() {
    let echo = start_echo(b"x");
    let mut config = Config { resolver: loopback_ok(), max_flows_per_ip: 1, ..Config::default() };
    config.policy.origins = vec!["https://ok.example".into()];
    let relay = start_relay(config);
    assert_eq!(refused_status(connect(relay, "127.0.0.1", echo, "https://bad.example")), 403);
    let mut first = connect(relay, "127.0.0.1", echo, "https://ok.example").unwrap();
    assert_eq!(read_binary(&mut first), b"x");
    assert_eq!(refused_status(connect(relay, "127.0.0.1", echo, "https://ok.example")), 429);
    drop(first);
    // The slot frees once the relay sees the page's socket close
    for _ in 0..50 {
        if connect(relay, "127.0.0.1", echo, "https://ok.example").is_ok() {
            return;
        }
        thread::sleep(Duration::from_millis(100));
    }
    panic!("the flow slot was not released");
}

/// A host that sends `size` bytes, then closes.
fn start_source(size: usize) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    thread::spawn(move || {
        for stream in listener.incoming() {
            let mut stream = stream.unwrap();
            thread::spawn(move || {
                let _ = stream.write_all(&vec![0x5a; size]);
            });
        }
    });
    port
}

// Host bytes that arrive before `quiet` passes with none
fn read_until_quiet(ws: &mut Client, quiet: Duration) -> usize {
    let tungstenite::stream::MaybeTlsStream::Plain(sock) = ws.get_ref() else { panic!("plain socket") };
    sock.set_read_timeout(Some(quiet)).unwrap();
    let mut got = 0;
    loop {
        match ws.read() {
            Ok(Message::Binary(data)) => got += data.len(),
            Ok(_) => {}
            Err(Error::Io(e)) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => return got,
            Err(e) => panic!("{e}"),
        }
    }
}

#[test]
fn credit_holds_the_host_to_the_window() {
    let source = start_source(4 * WINDOW as usize);
    let relay = start_relay(Config { resolver: loopback_ok(), ..Config::default() });
    let mut ws = ws_connect(relay, &format!("/tcp?host=127.0.0.1&port={source}&credit=1"), "https://any.example").unwrap();
    let first = read_until_quiet(&mut ws, Duration::from_millis(500));
    assert!(first >= WINDOW as usize && first < WINDOW as usize + 16 * 1024, "the relay stops at the window: {first} B");
    ws.send(Message::Text(first.to_string().into())).unwrap();
    let second = read_until_quiet(&mut ws, Duration::from_millis(500));
    assert!(second >= WINDOW as usize && second < WINDOW as usize + 16 * 1024, "a credit opens the window again: {second} B");
}

#[test]
fn text_without_credit_closes_the_flow() {
    let echo = start_echo(b"hi");
    let relay = start_relay(Config { resolver: loopback_ok(), ..Config::default() });
    let mut ws = connect(relay, "127.0.0.1", echo, "https://any.example").unwrap();
    assert_eq!(read_binary(&mut ws), b"hi");
    ws.send(Message::Text("100".into())).unwrap();
    loop {
        match ws.read() {
            Ok(Message::Close(Some(frame))) => return assert_eq!(u16::from(frame.code), CLOSE_UNSUPPORTED),
            Ok(_) => {}
            Err(e) => panic!("expected a close frame, got {e}"),
        }
    }
}

fn verdict(relay: u16, query: &str, origin: &str) -> String {
    let mut stream = TcpStream::connect(("127.0.0.1", relay)).unwrap();
    write!(stream, "GET /tcp?{query} HTTP/1.1\r\nHost: x\r\nOrigin: {origin}\r\n\r\n").unwrap();
    let mut reply = String::new();
    stream.read_to_string(&mut reply).unwrap();
    reply
}

#[test]
fn verdict_reports_why_a_flow_fails() {
    let mut config = Config::default();
    config.policy.origins = vec!["https://ok.example".into()];
    let relay = start_relay(config);
    let reply = verdict(relay, "host=127.0.0.1&port=80", "https://ok.example");
    assert!(reply.starts_with("HTTP/1.1 403"), "{reply}");
    assert!(reply.contains("Access-Control-Allow-Origin: https://ok.example"), "{reply}");
    assert!(reply.ends_with("address not public"), "{reply}");
    let reply = verdict(relay, "host=example.com&port=443", "https://bad.example");
    assert!(reply.ends_with("origin not allowed"), "{reply}");
}

#[test]
fn verdict_reports_a_full_flow_cap() {
    let echo = start_echo(b"x");
    let relay = start_relay(Config { resolver: loopback_ok(), max_flows_per_ip: 1, ..Config::default() });
    let mut first = connect(relay, "127.0.0.1", echo, "https://ok.example").unwrap();
    assert_eq!(read_binary(&mut first), b"x");
    let reply = verdict(relay, &format!("host=127.0.0.1&port={echo}"), "https://ok.example");
    assert!(reply.starts_with("HTTP/1.1 429") && reply.ends_with("too many flows"), "{reply}");
}

fn dns_query(name: &str, qtype: u16) -> Vec<u8> {
    let mut q = vec![0xab, 0xcd, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0];
    for label in name.split('.') {
        q.push(label.len() as u8);
        q.extend_from_slice(label.as_bytes());
    }
    q.push(0);
    q.extend_from_slice(&qtype.to_be_bytes());
    q.extend_from_slice(&[0, 1]);
    q
}

/// The rcode and the A record addresses of `reply` to a query for `name`.
fn dns_reply(reply: &[u8], name: &str) -> (u8, Vec<[u8; 4]>) {
    assert_eq!(&reply[..2], &[0xab, 0xcd], "the ID is echoed");
    assert_eq!(reply[2] & 0x81, 0x81, "QR and RD set");
    let count = u16::from_be_bytes([reply[6], reply[7]]) as usize;
    let mut at = 12 + name.len() + 2 + 4;
    let mut addrs = Vec::new();
    for _ in 0..count {
        assert_eq!(&reply[at..at + 12], &[0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4]);
        addrs.push(reply[at + 12..at + 16].try_into().unwrap());
        at += 16;
    }
    assert_eq!(at, reply.len());
    (reply[3] & 0x0f, addrs)
}

#[test]
fn dns_answers_a_with_public_addresses_only() {
    use esp_emu_relay::dns::{resolve, RCODE_NOTIMP, RCODE_REFUSED, TYPE_A, TYPE_AAAA};
    let resolver = Resolver { lookup: fixed_lookup, ..Resolver::default() };
    let open = Policy::default();
    let a = dns_query("mqtt.example.com", TYPE_A);
    assert_eq!(dns_reply(&resolve(&a, &open, &resolver).unwrap(), "mqtt.example.com"), (0, vec![[93, 184, 215, 14]]));
    let aaaa = dns_query("mqtt.example.com", TYPE_AAAA);
    assert_eq!(dns_reply(&resolve(&aaaa, &open, &resolver).unwrap(), "mqtt.example.com"), (0, vec![]));
    let txt = dns_query("mqtt.example.com", 16);
    assert_eq!(dns_reply(&resolve(&txt, &open, &resolver).unwrap(), "mqtt.example.com").0, RCODE_NOTIMP);
    let limited = Policy { hosts: vec!["*.example.org".into()], ..Policy::default() };
    assert_eq!(dns_reply(&resolve(&a, &limited, &resolver).unwrap(), "mqtt.example.com").0, RCODE_REFUSED);
    assert!(resolve(&a[..20], &open, &resolver).is_none(), "a truncated query gets no answer");
}

#[test]
fn dns_endpoint_over_http() {
    let relay = start_relay(Config::default());
    let query = dns_query("localhost", esp_emu_relay::dns::TYPE_A);
    let b64: String = {
        const URL: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        let bits: Vec<bool> = query.iter().flat_map(|b| (0..8).rev().map(move |i| b >> i & 1 == 1)).collect();
        bits.chunks(6).map(|c| URL[c.iter().fold(0, |n, &b| n << 1 | b as usize) << (6 - c.len())] as char).collect()
    };
    let mut stream = TcpStream::connect(("127.0.0.1", relay)).unwrap();
    write!(stream, "GET /dns?dns={b64} HTTP/1.1\r\nHost: x\r\nOrigin: https://page.example\r\n\r\n").unwrap();
    let mut reply = Vec::new();
    stream.read_to_end(&mut reply).unwrap();
    let split = reply.windows(4).position(|w| w == b"\r\n\r\n").unwrap();
    let head = String::from_utf8_lossy(&reply[..split]);
    assert!(head.starts_with("HTTP/1.1 200"), "{head}");
    assert!(head.contains("Content-Type: application/dns-message"), "{head}");
    assert!(head.contains("Access-Control-Allow-Origin: https://page.example"), "{head}");
    assert_eq!(dns_reply(&reply[split + 4..], "localhost"), (0, vec![]), "loopback answers are left out");
}

#[test]
fn version_endpoint_for_any_origin() {
    let relay = start_relay(Config::default());
    let mut stream = TcpStream::connect(("127.0.0.1", relay)).unwrap();
    write!(stream, "GET /version HTTP/1.1\r\nHost: x\r\nOrigin: https://page.example\r\n\r\n").unwrap();
    let mut reply = String::new();
    stream.read_to_string(&mut reply).unwrap();
    let (head, body) = reply.split_once("\r\n\r\n").unwrap();
    assert!(head.starts_with("HTTP/1.1 200"), "{head}");
    assert!(head.contains("Access-Control-Allow-Origin: *"), "{head}");
    assert_eq!(body, format!("{{\"name\":\"esp-emu-relay\",\"version\":\"{}\"}}", esp_emu_relay::VERSION));
    let core = esp_emu_relay::VERSION.split('-').next().unwrap();
    assert!(core.split('.').count() == 3 && core.split('.').all(|n| n.parse::<u32>().is_ok()), "MAJOR.MINOR.PATCH[-pre]: {}", esp_emu_relay::VERSION);
}

struct TestTls {
    server: std::sync::Arc<rustls::ServerConfig>,
    client: std::sync::Arc<rustls::ClientConfig>,
}

/// A self-signed certificate for `localhost`, written to PEM files and loaded as the relay does.
fn test_tls() -> TestTls {
    let rcgen::CertifiedKey { cert, signing_key } = rcgen::generate_simple_self_signed(vec!["localhost".into()]).unwrap();
    let dir = std::env::temp_dir().join(format!("esp-emu-relay-tls-{}-{:?}", std::process::id(), thread::current().id()));
    std::fs::create_dir_all(&dir).unwrap();
    let (cert_path, key_path) = (dir.join("cert.pem"), dir.join("key.pem"));
    std::fs::write(&cert_path, cert.pem()).unwrap();
    std::fs::write(&key_path, signing_key.serialize_pem()).unwrap();
    let server = esp_emu_relay::conn::tls_config(&cert_path, &key_path).unwrap();
    std::fs::remove_dir_all(&dir).unwrap();

    let mut roots = rustls::RootCertStore::empty();
    roots.add(cert.der().clone()).unwrap();
    let provider = std::sync::Arc::new(rustls::crypto::ring::default_provider());
    let client = rustls::ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .unwrap()
        .with_root_certificates(roots)
        .with_no_client_auth();
    TestTls { server, client: std::sync::Arc::new(client) }
}

type TlsStream = rustls::StreamOwned<rustls::ClientConnection, TcpStream>;

fn tls_stream(relay: u16, client: &std::sync::Arc<rustls::ClientConfig>) -> TlsStream {
    let name = rustls::pki_types::ServerName::try_from("localhost").unwrap();
    let conn = rustls::ClientConnection::new(client.clone(), name).unwrap();
    rustls::StreamOwned::new(conn, TcpStream::connect(("127.0.0.1", relay)).unwrap())
}

#[test]
fn tls_relays_bytes_both_ways() {
    let tls = test_tls();
    let echo = start_echo(b"hi");
    let relay = start_relay(Config { resolver: loopback_ok(), tls: Some(tls.server.clone()), ..Config::default() });
    let mut req = format!("wss://localhost:{relay}/tcp?host=127.0.0.1&port={echo}").into_client_request().unwrap();
    req.headers_mut().insert("Origin", "https://page.example".parse().unwrap());
    let (mut ws, _) = tungstenite::client(req, tls_stream(relay, &tls.client)).unwrap();
    let mut read = || match ws.read().unwrap() {
        Message::Binary(data) => data.to_vec(),
        other => panic!("expected binary data, got {other:?}"),
    };
    assert_eq!(read(), b"hi");
    let big: Vec<u8> = (0..40_000u32).map(|i| i as u8).collect();
    ws.send(Message::Binary(big.clone().into())).unwrap();
    let mut got = Vec::new();
    while got.len() < big.len() {
        match ws.read().unwrap() {
            Message::Binary(data) => got.extend_from_slice(&data),
            other => panic!("expected binary data, got {other:?}"),
        }
    }
    assert_eq!(got, big, "40 kB crosses several TLS records each way");
}

#[test]
fn tls_verdict_over_https() {
    let tls = test_tls();
    let relay = start_relay(Config { tls: Some(tls.server.clone()), ..Config::default() });
    let mut stream = tls_stream(relay, &tls.client);
    write!(stream, "GET /tcp?host=127.0.0.1&port=80 HTTP/1.1\r\nHost: localhost\r\nOrigin: https://page.example\r\n\r\n").unwrap();
    let mut reply = Vec::new();
    let _ = stream.read_to_end(&mut reply);
    let reply = String::from_utf8_lossy(&reply);
    assert!(reply.starts_with("HTTP/1.1 403"), "{reply}");
    assert!(reply.ends_with("address not public"), "{reply}");
}

#[test]
fn plain_client_on_a_tls_relay_gets_no_reply() {
    let tls = test_tls();
    let relay = start_relay(Config { tls: Some(tls.server.clone()), ..Config::default() });
    let mut stream = TcpStream::connect(("127.0.0.1", relay)).unwrap();
    write!(stream, "GET /tcp?host=example.com&port=443 HTTP/1.1\r\nHost: x\r\nOrigin: https://page.example\r\n\r\n").unwrap();
    let mut reply = Vec::new();
    // The relay closes with our bytes unread, so the close can come as a reset
    let _ = stream.read_to_end(&mut reply);
    assert!(!reply.starts_with(b"HTTP/1.1"), "{}", String::from_utf8_lossy(&reply));
}

#[test]
fn tls_config_reports_bad_files() {
    let missing = std::path::Path::new("/nonexistent/cert.pem");
    let err = esp_emu_relay::conn::tls_config(missing, missing).unwrap_err();
    assert!(err.contains("/nonexistent/cert.pem"), "{err}");
}

/// A UDP host that answers each datagram with `re: ` and the datagram.
fn start_udp_echo() -> u16 {
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let port = sock.local_addr().unwrap().port();
    thread::spawn(move || {
        let mut buf = [0u8; 2048];
        while let Ok((n, from)) = sock.recv_from(&mut buf) {
            let _ = sock.send_to(&[b"re: ", &buf[..n]].concat(), from);
        }
    });
    port
}

fn next_message(ws: &mut Client) -> Vec<u8> {
    loop {
        match ws.read().unwrap() {
            Message::Binary(data) => return data.to_vec(),
            Message::Ping(_) | Message::Pong(_) => {}
            other => panic!("expected a binary message, got {other:?}"),
        }
    }
}

#[test]
fn udp_keeps_datagram_boundaries() {
    let echo = start_udp_echo();
    let relay = start_relay(Config { resolver: loopback_ok(), ..Config::default() });
    let mut ws = ws_connect(relay, &format!("/udp?host=127.0.0.1&port={echo}"), "https://any.example").unwrap();
    ws.send(Message::Binary(b"first".to_vec().into())).unwrap();
    ws.send(Message::Binary(b"second".to_vec().into())).unwrap();
    ws.send(Message::Binary(Vec::new().into())).unwrap();
    assert_eq!(next_message(&mut ws), b"re: first");
    assert_eq!(next_message(&mut ws), b"re: second");
    assert_eq!(next_message(&mut ws), b"re: ", "an empty message is an empty datagram, not a FIN");
    ws.close(None).unwrap();
}

#[test]
fn udp_flow_survives_a_closed_port_and_ends_when_idle() {
    let closed = UdpSocket::bind("127.0.0.1:0").unwrap();
    let port = closed.local_addr().unwrap().port();
    drop(closed);
    let relay = start_relay(Config { resolver: loopback_ok(), idle: Duration::from_secs(1), ..Config::default() });
    let mut ws = ws_connect(relay, &format!("/udp?host=127.0.0.1&port={port}"), "https://any.example").unwrap();
    ws.send(Message::Binary(b"nobody".to_vec().into())).unwrap();
    ws.send(Message::Binary(b"still nobody".to_vec().into())).unwrap();
    expect_normal_close(&mut ws);
}

#[test]
fn udp_obeys_the_address_policy_and_has_a_verdict() {
    let echo = start_udp_echo();
    let relay = start_relay(Config::default());
    let refused = ws_connect(relay, &format!("/udp?host=127.0.0.1&port={echo}"), "https://any.example");
    assert_eq!(refused_status(refused), 403);
    let mut stream = TcpStream::connect(("127.0.0.1", relay)).unwrap();
    write!(stream, "GET /udp?host=127.0.0.1&port={echo} HTTP/1.1\r\nHost: x\r\nOrigin: https://p.example\r\n\r\n").unwrap();
    let mut reply = String::new();
    stream.read_to_string(&mut reply).unwrap();
    assert!(reply.starts_with("HTTP/1.1 403") && reply.ends_with("address not public"), "{reply}");
}

fn forward_relay(origins: &[&str]) -> (u16, u16) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let relay = listener.local_addr().unwrap().port();
    let forward = TcpListener::bind("127.0.0.1:0").unwrap();
    let host_port = forward.local_addr().unwrap().port();
    let mut config = Config::default();
    config.policy.origins = origins.iter().map(|o| o.to_string()).collect();
    let forwards = vec![(forward, Forward { host_port, guest_port: 80 })];
    thread::spawn(move || serve_bound(listener, forwards, config));
    (relay, host_port)
}

/// The `/listen` WebSocket, after its forward message.
fn listen(relay: u16) -> Client {
    let mut control = ws_connect(relay, "/listen", "https://ok.example").unwrap();
    let forward = next_message(&mut control);
    assert_eq!(forward[0], MSG_FORWARD);
    assert_eq!(u16::from_be_bytes([forward[3], forward[4]]), 80);
    control
}

/// The connection ID of the next `MSG_CONNECTION`, which must name firmware port 80.
fn next_connection(control: &mut Client) -> u32 {
    let msg = next_message(control);
    assert_eq!((msg[0], msg.len()), (MSG_CONNECTION, 7));
    assert_eq!(u16::from_be_bytes([msg[5], msg[6]]), 80);
    u32::from_be_bytes(msg[1..5].try_into().unwrap())
}

#[test]
fn forward_gives_a_connection_to_the_listening_page() {
    let (relay, host_port) = forward_relay(&["https://ok.example"]);
    assert_eq!(refused_status(ws_connect(relay, "/listen", "https://bad.example")), 403);
    let mut control = listen(relay);
    let mut client = TcpStream::connect(("127.0.0.1", host_port)).unwrap();
    client.write_all(b"GET / HTTP/1.0\r\n\r\n").unwrap();
    let id = next_connection(&mut control);

    assert_eq!(refused_status(ws_connect(relay, &format!("/accept?id={id}"), "https://bad.example")), 403);
    let mut flow = ws_connect(relay, &format!("/accept?id={id}"), "https://ok.example").unwrap();
    assert_eq!(read_binary(&mut flow), b"GET / HTTP/1.0\r\n\r\n");
    flow.send(Message::Binary(b"HTTP/1.0 200 OK\r\n\r\nhello".to_vec().into())).unwrap();
    send_fin(&mut flow);
    let mut answer = Vec::new();
    client.read_to_end(&mut answer).unwrap();
    assert_eq!(answer, b"HTTP/1.0 200 OK\r\n\r\nhello", "the page's FIN reaches the client");
    drop(client);
    expect_fin(&mut flow);
    expect_normal_close(&mut flow);
    assert_eq!(refused_status(ws_connect(relay, &format!("/accept?id={id}"), "https://ok.example")), 404, "a claimed connection is gone");
}

#[test]
fn forward_without_a_page_closes_the_connection() {
    let (_, host_port) = forward_relay(&[]);
    let mut client = TcpStream::connect(("127.0.0.1", host_port)).unwrap();
    client.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let mut buf = [0u8; 1];
    assert!(matches!(client.read(&mut buf), Ok(0) | Err(_)), "the client sees the connection end");
}

#[test]
fn newer_listen_replaces_the_older() {
    let (relay, host_port) = forward_relay(&["https://ok.example"]);
    let mut old = listen(relay);
    let mut new = listen(relay);
    match old.read().unwrap() {
        Message::Close(Some(frame)) => assert_eq!(u16::from(frame.code), CLOSE_REPLACED),
        other => panic!("expected a close frame, got {other:?}"),
    }
    let _client = TcpStream::connect(("127.0.0.1", host_port)).unwrap();
    next_connection(&mut new);
}

#[test]
fn allow_private_needs_an_origin_and_a_loopback_bind() {
    let relay = env!("CARGO_BIN_EXE_esp-emu-relay");
    let run = |args: &[&str]| std::process::Command::new(relay).args(args).output().unwrap();
    let no_origin = run(&["--allow-private"]);
    assert_eq!(no_origin.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&no_origin.stderr).contains("needs --allow-origin"));
    let open_bind = run(&["--allow-private", "--allow-origin", "https://ok.example", "--bind", "0.0.0.0"]);
    assert_eq!(open_bind.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&open_bind.stderr).contains("needs a loopback --bind"));
}

#[test]
fn forward_needs_an_origin() {
    let relay = env!("CARGO_BIN_EXE_esp-emu-relay");
    let out = std::process::Command::new(relay).args(["--forward", "18080:80"]).output().unwrap();
    assert_eq!(out.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&out.stderr).contains("--forward needs --allow-origin"));
}
