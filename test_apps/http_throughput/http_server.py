#!/usr/bin/env python3
"""Local HTTP server that streams a fixed-size payload for emulator throughput
tests. The guest reaches it at http://192.168.4.1:<PORT>/bigfile — esp-emu's
user-net redirects the gateway IP to host 127.0.0.1 (slirp-style)."""
import http.server
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8070
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 100 * 1024 * 1024
CHUNK = b"\xab" * 65536


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(SIZE))
        self.end_headers()
        remaining = SIZE
        while remaining > 0:
            n = min(len(CHUNK), remaining)
            try:
                self.wfile.write(CHUNK[:n])
            except (BrokenPipeError, ConnectionResetError):
                return
            remaining -= n

    def log_message(self, *args):  # silence per-request logging
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    with Server(("127.0.0.1", PORT), Handler) as httpd:
        print(f"serving {SIZE} bytes on 127.0.0.1:{PORT}/bigfile", flush=True)
        httpd.serve_forever()
