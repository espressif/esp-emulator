//! Answers a DNS query in wire format with the system resolver's IPv4 addresses.

use std::net::IpAddr;

use crate::policy::{Policy, Refusal, Resolver};

pub const TYPE_A: u16 = 1;
pub const TYPE_AAAA: u16 = 28;
const CLASS_IN: u16 = 1;
const TTL: u32 = 60;

pub const RCODE_SERVFAIL: u8 = 2;
pub const RCODE_NOTIMP: u8 = 4;
pub const RCODE_REFUSED: u8 = 5;

/// The question of a one-question query.
#[derive(Debug, PartialEq, Eq)]
pub struct Question {
    pub name: String,
    pub qtype: u16,
    /// Offset of the byte after the question.
    end: usize,
}

/// The single question of `query`, or `None` for a malformed query or a response.
pub fn parse_question(query: &[u8]) -> Option<Question> {
    if query.len() < 12 || query[2] & 0x80 != 0 || u16::from_be_bytes([query[4], query[5]]) != 1 {
        return None;
    }
    let mut labels = Vec::new();
    let mut at = 12;
    loop {
        let len = *query.get(at)? as usize;
        at += 1;
        if len == 0 {
            break;
        }
        if len > 63 {
            return None;
        }
        labels.push(std::str::from_utf8(query.get(at..at + len)?).ok()?);
        at += len;
    }
    let qtype = u16::from_be_bytes([*query.get(at)?, *query.get(at + 1)?]);
    Some(Question { name: labels.join("."), qtype, end: at + 4 }).filter(|q| q.end <= query.len())
}

/// The response to `query` with `rcode` and one A record per address.
pub fn answer(query: &[u8], question: &Question, rcode: u8, addrs: &[[u8; 4]]) -> Vec<u8> {
    let mut out = Vec::with_capacity(question.end + addrs.len() * 16);
    out.extend_from_slice(&query[..2]);
    out.push(0x80 | (query[2] & 0x79));
    out.push(0x80 | rcode);
    out.extend_from_slice(&[0, 1]);
    out.extend_from_slice(&(addrs.len() as u16).to_be_bytes());
    out.extend_from_slice(&[0, 0, 0, 0]);
    out.extend_from_slice(&query[12..question.end]);
    for addr in addrs {
        out.extend_from_slice(&[0xc0, 0x0c]);
        out.extend_from_slice(&TYPE_A.to_be_bytes());
        out.extend_from_slice(&CLASS_IN.to_be_bytes());
        out.extend_from_slice(&TTL.to_be_bytes());
        out.extend_from_slice(&[0, 4]);
        out.extend_from_slice(addr);
    }
    out
}

/// The response for `query`, or `None` when it is not a valid query.
pub fn resolve(query: &[u8], policy: &Policy, resolver: &Resolver) -> Option<Vec<u8>> {
    let q = parse_question(query)?;
    let reply = |rcode, addrs: &[[u8; 4]]| Some(answer(query, &q, rcode, addrs));
    match q.qtype {
        TYPE_A => {}
        // The browser NAT carries IPv4 only, so the firmware falls back to A
        TYPE_AAAA => return reply(0, &[]),
        _ => return reply(RCODE_NOTIMP, &[]),
    }
    match policy.host(&q.name) {
        Ok(_) => {}
        Err(Refusal::BAD_HOST) => return reply(RCODE_SERVFAIL, &[]),
        Err(_) => return reply(RCODE_REFUSED, &[]),
    }
    let Ok(found) = (resolver.lookup)(&q.name, 0) else { return reply(RCODE_SERVFAIL, &[]) };
    let mut addrs: Vec<[u8; 4]> = Vec::new();
    for a in found {
        if let IpAddr::V4(v4) = a.ip() {
            if (resolver.address_ok)(a.ip()) && !addrs.contains(&v4.octets()) {
                addrs.push(v4.octets());
            }
        }
    }
    reply(0, &addrs)
}

/// Decodes base64url without padding, as RFC 8484 GET uses.
pub fn base64url_decode(text: &str) -> Option<Vec<u8>> {
    let value = |c: u8| match c {
        b'A'..=b'Z' => Some(c - b'A'),
        b'a'..=b'z' => Some(c - b'a' + 26),
        b'0'..=b'9' => Some(c - b'0' + 52),
        b'-' => Some(62),
        b'_' => Some(63),
        _ => None,
    };
    let mut out = Vec::with_capacity(text.len() * 3 / 4);
    let (mut acc, mut bits) = (0u32, 0);
    for c in text.trim_end_matches('=').bytes() {
        acc = (acc << 6 | value(c)? as u32) & 0x3fff;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}
