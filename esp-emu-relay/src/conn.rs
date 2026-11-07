//! The page's connection, plain TCP or TLS: one reader thread, writers from any thread.

use std::collections::VecDeque;
use std::fs::File;
use std::io::{self, BufReader, ErrorKind, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpStream};
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use rustls::{ServerConfig, ServerConnection};
use rustls_pki_types::pem::PemObject;
use rustls_pki_types::{CertificateDer, PrivateKeyDer};

/// The TLS server setup from a PEM certificate chain and its PEM private key.
pub fn tls_config(cert: &Path, key: &Path) -> Result<Arc<ServerConfig>, String> {
    let open = |p: &Path| File::open(p).map(BufReader::new).map_err(|e| format!("{}: {e}", p.display()));
    let certs = CertificateDer::pem_reader_iter(open(cert)?)
        .collect::<Result<Vec<_>, _>>()
        .map_err(|e| format!("{}: {e}", cert.display()))?;
    if certs.is_empty() {
        return Err(format!("{}: no certificate", cert.display()));
    }
    let key = PrivateKeyDer::from_pem_reader(open(key)?).map_err(|e| format!("{}: {e}", key.display()))?;
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut config = ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .with_no_client_auth()
        .with_single_cert(certs, key)
        .map_err(|e| format!("{}: {e}", cert.display()))?;
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(Arc::new(config))
}

struct Out {
    sock: TcpStream,
    tls: Option<ServerConnection>,
}

impl Out {
    fn flush_tls(&mut self) -> io::Result<()> {
        if let Some(tls) = &mut self.tls {
            while tls.wants_write() {
                tls.write_tls(&mut self.sock)?;
            }
        }
        Ok(())
    }
}

/// The writing side; clones share the connection.
#[derive(Clone)]
pub struct Writer(Arc<Mutex<Out>>);

impl Writer {
    pub fn write_all(&self, data: &[u8]) -> io::Result<()> {
        let mut out = self.0.lock().unwrap();
        match &mut out.tls {
            Some(tls) => {
                tls.writer().write_all(data)?;
                out.flush_tls()
            }
            None => out.sock.write_all(data),
        }
    }

    /// Ends the page's side: a TLS close_notify, then a TCP FIN.
    pub fn shutdown(&self) {
        let mut out = self.0.lock().unwrap();
        if let Some(tls) = &mut out.tls {
            tls.send_close_notify();
            let _ = out.flush_tls();
        }
        let _ = out.sock.shutdown(Shutdown::Write);
    }
}

/// The reading side; only one thread reads.
pub struct Reader {
    sock: TcpStream,
    out: Writer,
    /// Fixed at `split`, so a plain read takes no lock.
    tls: bool,
    plain: VecDeque<u8>,
    tls_eof: bool,
}

impl Reader {
    pub fn set_read_timeout(&self, timeout: Duration) -> io::Result<()> {
        self.sock.set_read_timeout(Some(timeout))
    }

    pub fn peer_addr(&self) -> io::Result<SocketAddr> {
        self.sock.peer_addr()
    }

    pub fn set_nodelay(&self) -> io::Result<()> {
        self.sock.set_nodelay(true)
    }

    // Decrypts `raw`, keeps the plaintext, and sends what TLS wants to send back
    fn take_tls(&mut self, raw: &[u8]) -> io::Result<()> {
        let mut out = self.out.0.lock().unwrap();
        let Some(tls) = &mut out.tls else { return Ok(()) };
        let mut rest = raw;
        while !rest.is_empty() {
            tls.read_tls(&mut rest)?;
            let state = tls.process_new_packets().map_err(|e| io::Error::new(ErrorKind::InvalidData, e))?;
            let mut chunk = vec![0u8; state.plaintext_bytes_to_read()];
            tls.reader().read_exact(&mut chunk)?;
            self.plain.extend(chunk);
            self.tls_eof |= state.peer_has_closed();
        }
        out.flush_tls()
    }
}

impl Read for Reader {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if !self.tls {
            return self.sock.read(buf);
        }
        let mut raw = [0u8; 16 * 1024];
        while self.plain.is_empty() && !self.tls_eof {
            let n = self.sock.read(&mut raw)?;
            if n == 0 {
                return Ok(0);
            }
            self.take_tls(&raw[..n])?;
        }
        let n = buf.len().min(self.plain.len());
        for (dst, src) in buf.iter_mut().zip(self.plain.drain(..n)) {
            *dst = src;
        }
        Ok(n)
    }
}

/// Splits an accepted socket into its reader and writer, with TLS when `tls` is set.
pub fn split(sock: TcpStream, tls: Option<&Arc<ServerConfig>>) -> io::Result<(Reader, Writer)> {
    let tls = tls.map(|c| ServerConnection::new(c.clone())).transpose().map_err(io::Error::other)?;
    let is_tls = tls.is_some();
    let out = Writer(Arc::new(Mutex::new(Out { sock: sock.try_clone()?, tls })));
    Ok((Reader { sock, out: out.clone(), tls: is_tls, plain: VecDeque::new(), tls_eof: false }, out))
}
