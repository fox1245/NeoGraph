#!/usr/bin/env python3
"""Run a test command with an isolated, trusted HTTPS signed-video peer."""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading


class SignedVideoPeer(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # A credential leak must produce a consumer-visible failure, not a mock echo.
        if self.headers.get("x-goog-api-key") or self.headers.get("Authorization"):
            self.send_error(403)
            return
        if self.path.startswith("/downgrade"):
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:9/untrusted")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/loop"):
            self.send_response(302)
            self.send_header("Location", "/loop")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if not self.path.startswith("/signed?X-Goog-Signature="):
            self.send_error(404)
            return
        video = b"\x00\x00\x00\x18ftypmp42" + bytes(12)
        self.send_response(200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(video)))
        self.end_headers()
        self.wfile.write(video)

    def log_message(self, *_args):
        pass


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: typed_veo_peer.py TEST_COMMAND [ARGS...]")
    with tempfile.TemporaryDirectory(prefix="neograph-veo-peer-") as directory:
        cert = Path(directory) / "cert.pem"
        key = Path(directory) / "key.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", str(key), "-out", str(cert), "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SignedVideoPeer)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        environment = os.environ.copy()
        environment["SSL_CERT_FILE"] = str(cert)
        environment["NEOGRAPH_VEO_SIGNED_ORIGIN"] = f"https://localhost:{server.server_port}"
        try:
            result = subprocess.run(sys.argv[1:], env=environment, check=False)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
