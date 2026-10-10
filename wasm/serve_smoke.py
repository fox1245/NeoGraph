"""Serve the genuine Emscripten smoke assets with pthread isolation headers."""

from __future__ import annotations

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class SmokeHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if self.path.split("?", 1)[0] in {"/", "/smoke.html"}:
            content = Path(__file__).with_name("smoke.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        else:
            super().do_GET()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path, help="Generated build-wasm/wasm directory")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    assets = args.assets.resolve(strict=True)
    if not assets.is_dir() or any(not (assets / name).is_file()
                                 for name in ("smoke.js", "smoke.wasm")):
        parser.error("assets must contain genuine generated smoke.js and smoke.wasm")
    handler = partial(SmokeHandler, directory=str(assets))
    with ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"http://127.0.0.1:{server.server_port}/smoke.html", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
