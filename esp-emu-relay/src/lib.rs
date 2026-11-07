//! WebSocket relay for the browser emulator: each WebSocket on `/tcp?host=H&port=P` or
//! `/udp?host=H&port=P` carries one TCP or UDP flow of the firmware to `H:P`, which must resolve
//! to a public address unless the relay allows private ones. `/dns` answers the firmware's DNS
//! queries with the system resolver, and `/listen` with `/accept` carry the port forwards.

/// The release version. A page accepts a relay of the MAJOR it was built against.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

pub mod conn;
pub mod dns;
pub mod http;
pub mod inbound;
pub mod policy;
pub mod server;
pub mod udp;
pub mod ws;
