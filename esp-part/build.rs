//! Generates the message types from `proto/esp_cosim.proto`, the contract a
//! part and the emulator speak, into `OUT_DIR/esp_cosim.v1.rs`.

/// Where `esp_cosim.proto` is: the repository's `proto/`. A copy of this crate
/// kept outside the repository sets `ESP_COSIM_PROTO_DIR` to the folder that
/// holds its own copy of the file.
const PROTO_DIR: &str = "../proto";

fn main() {
    let dir = std::env::var("ESP_COSIM_PROTO_DIR").unwrap_or_else(|_| PROTO_DIR.into());
    let files = protox::compile(["esp_cosim.proto"], [dir.as_str()]).expect("compile esp_cosim.proto");
    // Maps as BTreeMap: a part encodes the same bytes twice, and carries no
    // hasher.
    prost_build::Config::new().btree_map(["."]).compile_fds(files).expect("generate the Rust types");
    println!("cargo:rerun-if-changed={dir}/esp_cosim.proto");
    println!("cargo:rerun-if-env-changed=ESP_COSIM_PROTO_DIR");
}
