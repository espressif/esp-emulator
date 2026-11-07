//! `esp-emu-relay`: gives the browser emulator's firmware TCP and UDP access to hosts, and
//! forwards ports on this computer to the firmware.

use std::io::{self, IsTerminal, Read};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener};
use std::path::PathBuf;
use std::process::exit;
use std::time::Duration;

use esp_emu_relay::conn::tls_config;
use esp_emu_relay::inbound::Forward;
use esp_emu_relay::policy::any_address;
use esp_emu_relay::server::{self, Config};

const USAGE: &str = "\
Usage: esp-emu-relay [OPTIONS]

  --version              print the version and exit
  --port N               listen on port N (default 8787)
  --bind ADDR            listen on ADDR (default 127.0.0.1)
  --allow-origin PATTERN accept only pages from PATTERN, such as https://*.example.com
  --allow-host PATTERN   connect only to hosts that match PATTERN, such as *.example.com
  --allow-port N         connect only to port N
  --allow-private        also connect to loopback, private and link-local addresses
                         (needs --allow-origin and a loopback --bind)
  --forward HPORT:FPORT  give connections to port HPORT on the --bind address to port FPORT
                         of the firmware, such as --forward 8080:80 (needs --allow-origin)
  --max-flows N          open flows per client address (default 16)
  --idle-timeout SECS    close a flow with no data for SECS (default 300)
  --tls-cert FILE        serve wss:// with this PEM certificate chain (needs --tls-key)
  --tls-key FILE         the PEM private key of --tls-cert

Each --allow-* option and --forward repeats or takes a comma-separated list; with no --allow-*
list, that kind is not limited. '*' matches one DNS label. Without --allow-private, the relay
never connects to a private, loopback or link-local address. The firmware reaches this computer's
loopback address through its gateway address.";

// A double-clicked binary's window closes on exit, so wait for Enter to show the error
fn fail(message: &str) -> ! {
    eprintln!("{message}");
    if io::stdin().is_terminal() {
        eprintln!("Press Enter to close.");
        let _ = io::stdin().read(&mut [0u8]);
    }
    exit(1);
}

fn usage_error(message: &str) -> ! {
    eprintln!("esp-emu-relay: {message}\n\n{USAGE}");
    exit(2);
}

fn number<T: std::str::FromStr>(flag: &str, value: &str) -> T {
    value.parse().unwrap_or_else(|_| usage_error(&format!("{flag}: '{value}' is not valid")))
}

fn list(value: &str) -> impl Iterator<Item = String> + '_ {
    value.split(',').map(str::trim).filter(|s| !s.is_empty()).map(String::from)
}

fn parse_args() -> (SocketAddr, Config, bool) {
    let mut port: u16 = 8787;
    let mut bind = IpAddr::V4(Ipv4Addr::LOCALHOST);
    let mut config = Config::default();
    let (mut cert, mut key) = (None::<PathBuf>, None::<PathBuf>);
    let mut allow_private = false;
    let mut args = std::env::args().skip(1);
    while let Some(flag) = args.next() {
        if flag == "-h" || flag == "--help" {
            println!("{USAGE}");
            exit(0);
        }
        if flag == "-V" || flag == "--version" {
            println!("esp-emu-relay {}", esp_emu_relay::VERSION);
            exit(0);
        }
        if flag == "--allow-private" {
            allow_private = true;
            continue;
        }
        let Some(value) = args.next() else { usage_error(&format!("{flag} needs a value")) };
        match flag.as_str() {
            "--port" => port = number(&flag, &value),
            "--bind" => bind = number(&flag, &value),
            "--allow-origin" => config.policy.origins.extend(list(&value)),
            "--allow-host" => config.policy.hosts.extend(list(&value)),
            "--allow-port" => config.policy.ports.extend(list(&value).map(|p| number::<u16>(&flag, &p))),
            "--max-flows" => config.max_flows_per_ip = number(&flag, &value),
            "--idle-timeout" => config.idle = Duration::from_secs(number(&flag, &value)),
            "--tls-cert" => cert = Some(value.into()),
            "--tls-key" => key = Some(value.into()),
            "--forward" => {
                for f in list(&value) {
                    config.forwards.push(f.parse::<Forward>().unwrap_or_else(|e| usage_error(&format!("--forward: {e}"))));
                }
            }
            _ => usage_error(&format!("unknown option {flag}")),
        }
    }
    config.tls = match (cert, key) {
        (Some(cert), Some(key)) => Some(tls_config(&cert, &key).unwrap_or_else(|e| fail(&format!("TLS: {e}")))),
        (None, None) => None,
        _ => usage_error("--tls-cert and --tls-key go together"),
    };
    if !config.forwards.is_empty() && config.policy.origins.is_empty() {
        usage_error("--forward needs --allow-origin, so that other web pages cannot take the connections");
    }
    if allow_private {
        // Any page, or any machine that can reach the relay, could otherwise reach the LAN through it
        if config.policy.origins.is_empty() {
            usage_error("--allow-private needs --allow-origin, so that other web pages cannot reach your network");
        }
        if !bind.is_loopback() {
            usage_error("--allow-private needs a loopback --bind, so that other machines cannot reach your network");
        }
        config.resolver.address_ok = any_address;
    }
    (SocketAddr::new(bind, port), config, allow_private)
}

fn describe<T: ToString>(items: &[T], none: &str) -> String {
    if items.is_empty() {
        none.to_string()
    } else {
        items.iter().map(T::to_string).collect::<Vec<_>>().join(", ")
    }
}

fn main() {
    let (addr, config, allow_private) = parse_args();
    let listener = TcpListener::bind(addr).unwrap_or_else(|e| match e.kind() {
        io::ErrorKind::AddrInUse => fail(&format!(
            "Port {} is in use: a relay may already run. Use --port to pick another port.",
            addr.port()
        )),
        _ => fail(&format!("The relay cannot start: {e}")),
    });
    let forwards = server::bind_forwards(addr.ip(), &config.forwards).unwrap_or_else(|e| match e.kind() {
        io::ErrorKind::AddrInUse => fail(&format!("{e}. Another program uses the port: pick another HPORT in --forward.")),
        _ => fail(&format!("The relay cannot start: {e}")),
    });
    println!("ESP Emulator Relay {} is running.", esp_emu_relay::VERSION);
    if config.tls.is_some() {
        println!("Paste this URL into the emulator page: wss://HOST:{}, where HOST is a name on the certificate", addr.port());
    } else {
        println!("Paste this URL into the emulator page: ws://{addr}");
    }
    println!("Origins: {}", describe(&config.policy.origins, "any"));
    let any_host = if allow_private { "any host" } else { "any public host" };
    println!("Hosts: {}", describe(&config.policy.hosts, any_host));
    println!("Ports: {}", describe(&config.policy.ports, "any"));
    for f in &config.forwards {
        println!("Forward: {}:{} -> firmware port {}", addr.ip(), f.host_port, f.guest_port);
    }
    println!("Keep this window open. Press Ctrl+C to stop.");
    if let Err(e) = server::serve_bound(listener, forwards, config) {
        fail(&format!("The relay stopped: {e}"));
    }
}
