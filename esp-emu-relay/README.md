# esp-emu-relay

The WebSocket relay that gives the browser emulator real networking. The page
runs the firmware inside the tab; this program runs on your computer and
carries each of the firmware's TCP and UDP flows to the internet, answers its
DNS queries with your resolver, and can forward ports of your computer to a
server in the firmware. Usage, options and the wire protocol:
[docs/guides/BROWSER.md](../docs/guides/BROWSER.md#browser-networking).

Most people install a release instead of building: the page's **Network**
panel shows the command for your OS, or on Linux and macOS

```sh
curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh -s -- --relay
```

## Build and test

The relay is its own Cargo workspace with no emulator code, so it builds on
any host with Rust and a C compiler:

```sh
cargo build --release --manifest-path esp-emu-relay/Cargo.toml
./esp-emu-relay/target/release/esp-emu-relay --help
cargo test --manifest-path esp-emu-relay/Cargo.toml
```

Each release ships it for Linux x86_64 and aarch64, Windows x86_64 and
macOS x86_64 and arm64, built with `cargo build --release --target <target>`
and the target's own linker; the Linux and Windows binaries link the C
runtime statically, so on Linux names resolve through `/etc/hosts` and DNS
only, not through NSS plugins such as mDNS.

## Version and release

The relay has its own version in `Cargo.toml` and its own GitHub release,
`relay-vX.Y.Z`, apart from the emulator's. `MAJOR` is the wire protocol: a
page works with every relay of the `MAJOR` it was built for, and says so
when it finds another. The page and the installers name the version they
were built with.
