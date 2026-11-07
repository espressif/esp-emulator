//! Which origins, hosts, ports and addresses the relay accepts.

use std::io;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, ToSocketAddrs};

/// A refused request: the HTTP status and the reason that the page reads.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Refusal {
    pub status: u16,
    pub reason: &'static str,
}

impl Refusal {
    pub const BAD_REQUEST: Refusal = Refusal { status: 400, reason: "bad request" };
    pub const BAD_HOST: Refusal = Refusal { status: 400, reason: "bad host" };
    pub const BAD_PORT: Refusal = Refusal { status: 400, reason: "bad port" };
    pub const ORIGIN: Refusal = Refusal { status: 403, reason: "origin not allowed" };
    pub const HOST: Refusal = Refusal { status: 403, reason: "host not allowed" };
    pub const PORT: Refusal = Refusal { status: 403, reason: "port not allowed" };
    pub const NOT_PUBLIC: Refusal = Refusal { status: 403, reason: "address not public" };
    pub const TOO_MANY: Refusal = Refusal { status: 429, reason: "too many flows" };
    pub const LOOKUP: Refusal = Refusal { status: 502, reason: "lookup failed" };
    pub const CONNECT: Refusal = Refusal { status: 502, reason: "connect failed" };
    pub const GONE: Refusal = Refusal { status: 404, reason: "no such connection" };
}

/// True when `text` matches `pattern`, where `*` stands for exactly one DNS label. Case-insensitive.
pub fn pattern_matches(pattern: &str, text: &str) -> bool {
    fn label_char(c: u8) -> bool {
        c.is_ascii_alphanumeric() || c == b'-'
    }
    fn go(p: &[u8], t: &[u8]) -> bool {
        match p.split_first() {
            None => t.is_empty(),
            Some((b'*', rest)) => {
                let n = t.iter().take_while(|&&c| label_char(c)).count();
                (1..=n).any(|k| go(rest, &t[k..]))
            }
            Some((c, rest)) => t.first() == Some(c) && go(rest, &t[1..]),
        }
    }
    go(pattern.to_ascii_lowercase().as_bytes(), text.to_ascii_lowercase().as_bytes())
}

const V4_BLOCKED: [(Ipv4Addr, u8); 15] = [
    (Ipv4Addr::new(0, 0, 0, 0), 8),
    (Ipv4Addr::new(10, 0, 0, 0), 8),
    (Ipv4Addr::new(100, 64, 0, 0), 10),
    (Ipv4Addr::new(127, 0, 0, 0), 8),
    (Ipv4Addr::new(169, 254, 0, 0), 16),
    (Ipv4Addr::new(172, 16, 0, 0), 12),
    (Ipv4Addr::new(192, 0, 0, 0), 24),
    (Ipv4Addr::new(192, 0, 2, 0), 24),
    (Ipv4Addr::new(192, 168, 0, 0), 16),
    (Ipv4Addr::new(198, 18, 0, 0), 15),
    (Ipv4Addr::new(198, 51, 100, 0), 24),
    (Ipv4Addr::new(203, 0, 113, 0), 24),
    (Ipv4Addr::new(224, 0, 0, 0), 4),
    (Ipv4Addr::new(240, 0, 0, 0), 4),
    (Ipv4Addr::new(168, 63, 129, 16), 32),
];

const V6_BLOCKED: [(Ipv6Addr, u8); 10] = [
    (Ipv6Addr::new(0, 0, 0, 0, 0, 0, 0, 0), 96),
    (Ipv6Addr::new(0, 0, 0, 0, 0, 0xffff, 0, 0), 96),
    (Ipv6Addr::new(0x64, 0xff9b, 0, 0, 0, 0, 0, 0), 96),
    (Ipv6Addr::new(0x64, 0xff9b, 1, 0, 0, 0, 0, 0), 48),
    (Ipv6Addr::new(0x100, 0, 0, 0, 0, 0, 0, 0), 64),
    (Ipv6Addr::new(0x2001, 0xdb8, 0, 0, 0, 0, 0, 0), 32),
    (Ipv6Addr::new(0x2002, 0, 0, 0, 0, 0, 0, 0), 16),
    (Ipv6Addr::new(0xfc00, 0, 0, 0, 0, 0, 0, 0), 7),
    (Ipv6Addr::new(0xfe80, 0, 0, 0, 0, 0, 0, 0), 10),
    (Ipv6Addr::new(0xff00, 0, 0, 0, 0, 0, 0, 0), 8),
];

fn in_net(addr: u128, net: u128, prefix: u8, bits: u8) -> bool {
    let shift = bits - prefix;
    shift >= bits || (addr >> shift) == (net >> shift)
}

/// True for a public unicast address; false for loopback, private, link-local, CGNAT, metadata and the like.
pub fn is_public(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(a) => !V4_BLOCKED
            .iter()
            .any(|&(net, p)| in_net(u32::from(a) as u128, u32::from(net) as u128, p, 32)),
        IpAddr::V6(a) => !V6_BLOCKED.iter().any(|&(net, p)| in_net(u128::from(a), u128::from(net), p, 128)),
    }
}

/// Accepts every address, for `--allow-private`.
pub fn any_address(_: IpAddr) -> bool {
    true
}

/// The system resolver, IPv4 addresses first.
pub fn system_lookup(host: &str, port: u16) -> io::Result<Vec<SocketAddr>> {
    let mut addrs: Vec<SocketAddr> = (host, port).to_socket_addrs()?.collect();
    addrs.sort_by_key(|a| a.is_ipv6());
    Ok(addrs)
}

/// How the relay resolves a host and which addresses it connects to.
#[derive(Clone, Copy)]
pub struct Resolver {
    pub lookup: fn(&str, u16) -> io::Result<Vec<SocketAddr>>,
    pub address_ok: fn(IpAddr) -> bool,
}

impl Default for Resolver {
    fn default() -> Self {
        Resolver { lookup: system_lookup, address_ok: is_public }
    }
}

/// The optional allowlists. An empty list allows everything of its kind.
#[derive(Debug, Clone, Default)]
pub struct Policy {
    pub origins: Vec<String>,
    pub hosts: Vec<String>,
    pub ports: Vec<u16>,
}

impl Policy {
    pub fn origin_allowed(&self, origin: Option<&str>) -> bool {
        self.origins.is_empty() || origin.is_some_and(|o| self.origins.iter().any(|p| pattern_matches(p, o)))
    }

    /// `host` in lowercase without its trailing dot, if it is valid and the host allowlist accepts it.
    pub fn host(&self, host: &str) -> Result<String, Refusal> {
        let name = host.strip_suffix('.').unwrap_or(host).to_ascii_lowercase();
        let literal = name.parse::<IpAddr>().is_ok();
        let valid = |c: char| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_');
        if name.is_empty() || name.len() > 253 || !(literal || name.chars().all(valid)) {
            return Err(Refusal::BAD_HOST);
        }
        if !self.hosts.is_empty() && (literal || !self.hosts.iter().any(|p| pattern_matches(p, &name))) {
            return Err(Refusal::HOST);
        }
        Ok(name)
    }

    /// The address to connect to for `host:port`, or why the relay refuses it.
    pub fn check(&self, host: &str, port: u16, resolver: &Resolver) -> Result<SocketAddr, Refusal> {
        if port == 0 {
            return Err(Refusal::BAD_PORT);
        }
        let name = self.host(host)?;
        if !self.ports.is_empty() && !self.ports.contains(&port) {
            return Err(Refusal::PORT);
        }
        let addrs = (resolver.lookup)(&name, port).map_err(|_| Refusal::LOOKUP)?;
        if addrs.is_empty() {
            return Err(Refusal::LOOKUP);
        }
        addrs.into_iter().find(|a| (resolver.address_ok)(a.ip())).ok_or(Refusal::NOT_PUBLIC)
    }
}
