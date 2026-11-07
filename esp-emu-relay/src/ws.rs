//! Server side of RFC 6455 for binary messages: handshake, framing, ping and close. A short text
//! message carries flow-control credit.

use sha1::{Digest, Sha1};

use crate::http::Request;

const GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const MAX_FRAME: usize = 1 << 20;
const MAX_TEXT: usize = 32;

pub const OP_CONTINUATION: u8 = 0;
pub const OP_TEXT: u8 = 1;
pub const OP_BINARY: u8 = 2;
pub const OP_CLOSE: u8 = 8;
pub const OP_PING: u8 = 9;
pub const OP_PONG: u8 = 10;

pub const CLOSE_NORMAL: u16 = 1000;
/// A close frame with no code; never sent.
pub const CLOSE_NO_STATUS: u16 = 1005;
pub const CLOSE_PROTOCOL: u16 = 1002;
pub const CLOSE_UNSUPPORTED: u16 = 1003;
pub const CLOSE_ERROR: u16 = 1011;

/// The `Sec-WebSocket-Key` of a valid upgrade request, or `None`.
pub fn upgrade_key(req: &Request) -> Option<&str> {
    let upgrade = req.header("upgrade").unwrap_or("");
    (req.method == "GET" && upgrade.eq_ignore_ascii_case("websocket") && req.header("sec-websocket-version") == Some("13"))
        .then(|| req.header("sec-websocket-key"))
        .flatten()
        .filter(|k| !k.is_empty())
}

fn base64(data: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let n = chunk.iter().enumerate().fold(0u32, |n, (i, &b)| n | (b as u32) << (16 - 8 * i));
        for i in 0..4 {
            out.push(if i <= chunk.len() { ALPHABET[(n >> (18 - 6 * i) & 63) as usize] as char } else { '=' });
        }
    }
    out
}

/// The `Sec-WebSocket-Accept` value for `key`.
pub fn accept_key(key: &str) -> String {
    base64(&Sha1::new().chain_update(key.as_bytes()).chain_update(GUID.as_bytes()).finalize())
}

/// The 101 response that accepts the upgrade for `key`.
pub fn accept_response(key: &str) -> Vec<u8> {
    format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        accept_key(key)
    )
    .into_bytes()
}

/// One unmasked server frame.
pub fn encode(opcode: u8, payload: &[u8]) -> Vec<u8> {
    let len = payload.len();
    let mut out = Vec::with_capacity(len + 10);
    out.push(0x80 | opcode);
    if len < 126 {
        out.push(len as u8);
    } else if len < 65536 {
        out.push(126);
        out.extend_from_slice(&(len as u16).to_be_bytes());
    } else {
        out.push(127);
        out.extend_from_slice(&(len as u64).to_be_bytes());
    }
    out.extend_from_slice(payload);
    out
}

pub fn encode_close(code: u16) -> Vec<u8> {
    encode(OP_CLOSE, &code.to_be_bytes())
}

#[derive(Debug, PartialEq, Eq)]
pub enum Frame {
    Data(Vec<u8>),
    /// A text message in one frame, at most 32 bytes.
    Text(Vec<u8>),
    Ping(Vec<u8>),
    Pong,
    Close(u16),
}

/// Splits the client's byte stream into frames, and joins a fragmented message into one `Data`.
#[derive(Default)]
pub struct FrameReader {
    buf: Vec<u8>,
    /// The fragments of a message that is not complete.
    partial: Option<Vec<u8>>,
}

impl FrameReader {
    pub fn push(&mut self, bytes: &[u8]) {
        self.buf.extend_from_slice(bytes);
    }

    /// The next complete frame or message, `None` for more bytes, or `Err(code)` to close with.
    pub fn next(&mut self) -> Result<Option<Frame>, u16> {
        loop {
            let Some((fin, opcode, payload)) = self.next_frame()? else { return Ok(None) };
            let frame = match opcode {
                OP_BINARY | OP_CONTINUATION => {
                    let mut data = match (opcode, self.partial.take()) {
                        (OP_BINARY, None) => payload,
                        (OP_CONTINUATION, Some(mut data)) => {
                            data.extend_from_slice(&payload);
                            data
                        }
                        _ => return Err(CLOSE_PROTOCOL),
                    };
                    if data.len() > MAX_FRAME {
                        return Err(CLOSE_PROTOCOL);
                    }
                    if !fin {
                        self.partial = Some(std::mem::take(&mut data));
                        continue;
                    }
                    Frame::Data(data)
                }
                OP_PING => Frame::Ping(payload),
                OP_PONG => Frame::Pong,
                OP_CLOSE if payload.len() >= 2 => Frame::Close(u16::from_be_bytes([payload[0], payload[1]])),
                OP_CLOSE => Frame::Close(CLOSE_NO_STATUS),
                OP_TEXT if fin && self.partial.is_none() && payload.len() <= MAX_TEXT => Frame::Text(payload),
                OP_TEXT => return Err(CLOSE_UNSUPPORTED),
                _ => return Err(CLOSE_PROTOCOL),
            };
            return Ok(Some(frame));
        }
    }

    // One unmasked frame as (FIN, opcode, payload)
    fn next_frame(&mut self) -> Result<Option<(bool, u8, Vec<u8>)>, u16> {
        let b = &self.buf;
        if b.len() < 2 {
            return Ok(None);
        }
        let fin = b[0] & 0x80 != 0;
        let opcode = b[0] & 0x0f;
        let masked = b[1] & 0x80 != 0;
        let (len, at) = match b[1] & 0x7f {
            126 if b.len() >= 4 => (u16::from_be_bytes([b[2], b[3]]) as u64, 4),
            127 if b.len() >= 10 => (u64::from_be_bytes(b[2..10].try_into().unwrap()), 10),
            126 | 127 => return Ok(None),
            n => (n as u64, 2),
        };
        if !masked || len > MAX_FRAME as u64 {
            return Err(CLOSE_PROTOCOL);
        }
        let len = len as usize;
        if b.len() < at + 4 + len {
            return Ok(None);
        }
        let mask = [b[at], b[at + 1], b[at + 2], b[at + 3]];
        let payload: Vec<u8> = b[at + 4..at + 4 + len].iter().enumerate().map(|(i, &c)| c ^ mask[i & 3]).collect();
        self.buf.drain(..at + 4 + len);
        Ok(Some((fin, opcode, payload)))
    }
}
