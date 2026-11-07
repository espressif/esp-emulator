//! The HTTP/1.1 request head and the plain responses that the relay sends.

use std::io::{self, Read};

const MAX_HEAD: usize = 8 * 1024;

#[derive(Debug, Default)]
pub struct Request {
    pub method: String,
    pub path: String,
    query: Vec<(String, String)>,
    headers: Vec<(String, String)>,
}

impl Request {
    /// The value of header `name` (lowercase), if present.
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }

    /// The decoded value of query parameter `name`, if present.
    pub fn param(&self, name: &str) -> Option<&str> {
        self.query.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }
}

/// `%XX` and `+` decoding, as `URLSearchParams` does.
pub fn percent_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    let hex = |c: u8| (c as char).to_digit(16);
    while i < b.len() {
        match b[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < b.len() => match (hex(b[i + 1]), hex(b[i + 2])) {
                (Some(h), Some(l)) => {
                    out.push((h * 16 + l) as u8);
                    i += 2;
                }
                _ => out.push(b'%'),
            },
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn parse(head: &str) -> Option<Request> {
    let mut lines = head.split("\r\n");
    let mut start = lines.next()?.split(' ');
    let method = start.next()?.to_string();
    let target = start.next()?;
    if !start.next()?.starts_with("HTTP/1.") {
        return None;
    }
    let (path, query) = target.split_once('?').unwrap_or((target, ""));
    let query = query
        .split('&')
        .filter(|p| !p.is_empty())
        .map(|p| {
            let (k, v) = p.split_once('=').unwrap_or((p, ""));
            (percent_decode(k), percent_decode(v))
        })
        .collect();
    let headers = lines
        .filter_map(|l| l.split_once(':'))
        .map(|(k, v)| (k.trim().to_ascii_lowercase(), v.trim().to_string()))
        .collect();
    Some(Request { method, path: path.to_string(), query, headers })
}

/// Reads one request head; also returns the bytes that came after it.
pub fn read_request(stream: &mut impl Read) -> io::Result<(Request, Vec<u8>)> {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 1024];
    loop {
        if let Some(end) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = std::str::from_utf8(&buf[..end]).map_err(|_| io::ErrorKind::InvalidData)?;
            let req = parse(head).ok_or(io::ErrorKind::InvalidData)?;
            return Ok((req, buf[end + 4..].to_vec()));
        }
        if buf.len() > MAX_HEAD {
            return Err(io::ErrorKind::InvalidData.into());
        }
        let n = stream.read(&mut chunk)?;
        if n == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        buf.extend_from_slice(&chunk[..n]);
    }
}

pub fn status_text(status: u16) -> &'static str {
    match status {
        101 => "Switching Protocols",
        200 => "OK",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        429 => "Too Many Requests",
        502 => "Bad Gateway",
        _ => "",
    }
}

/// A complete response that closes the connection.
pub fn response(status: u16, headers: &[(&str, &str)], body: &str) -> Vec<u8> {
    binary_response(status, headers, body.as_bytes())
}

pub fn binary_response(status: u16, headers: &[(&str, &str)], body: &[u8]) -> Vec<u8> {
    let mut head = format!("HTTP/1.1 {status} {}\r\nConnection: close\r\n", status_text(status));
    for (k, v) in headers {
        head += &format!("{k}: {v}\r\n");
    }
    if status != 204 {
        head += &format!("Content-Length: {}\r\n", body.len());
    }
    head += "\r\n";
    let mut out = head.into_bytes();
    if status != 204 {
        out.extend_from_slice(body);
    }
    out
}
