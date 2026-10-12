"""Recording reverse proxy for Streamable HTTP interop tests.

`HttpRecordProxy(upstream_port, log_path)` listens on 127.0.0.1 and forwards
every request verbatim to the upstream, appending JSONL records:
{"t", "dir": "c2s", "method", "path", "headers", "raw"} and
{"t", "dir": "s2c", "status", "headers", "raw"} (SSE bodies are recorded as the
`data:` payloads, one record per event). Usable as a module or a CLI:
`http_record_proxy.py LISTEN_PORT UPSTREAM_PORT LOG`.
"""

import http.client
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOP = {"connection", "keep-alive", "transfer-encoding", "te", "trailer", "upgrade",
       "proxy-authorization", "proxy-authenticate", "content-length", "host"}


class HttpRecordProxy:
    def __init__(self, upstream_port: int, log_path: str, listen_port: int = 0):
        self.upstream_port = upstream_port
        self.log_path = log_path
        self.lock = threading.Lock()
        proxy = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_a):
                pass

            def _forward(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP}
                proxy.record({"dir": "c2s", "method": self.command, "path": self.path,
                              "headers": dict(self.headers.items()),
                              "raw": body.decode("utf-8", "replace")})
                conn = http.client.HTTPConnection("127.0.0.1", proxy.upstream_port, timeout=120)
                try:
                    conn.request(self.command, self.path, body=body or None, headers=headers)
                    resp = conn.getresponse()
                    ctype = resp.getheader("Content-Type", "") or ""
                    rheaders = [(k, v) for k, v in resp.getheaders() if k.lower() not in HOP]
                    if ctype.startswith("text/event-stream"):
                        self.send_response(resp.status)
                        for k, v in rheaders:
                            self.send_header(k, v)
                        self.send_header("Transfer-Encoding", "chunked")
                        self.end_headers()
                        buf = b""
                        while True:
                            chunk = resp.read1(65536) if hasattr(resp, "read1") else resp.read(1)
                            if not chunk:
                                break
                            self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
                            self.wfile.flush()
                            buf += chunk
                            while b"\n\n" in buf:
                                event, buf = buf.split(b"\n\n", 1)
                                data = b"\n".join(l[5:].lstrip() for l in event.split(b"\n")
                                                  if l.startswith(b"data:"))
                                proxy.record({"dir": "s2c", "status": resp.status, "sse": True,
                                              "headers": dict(rheaders),
                                              "raw": data.decode("utf-8", "replace")})
                        self.wfile.write(b"0\r\n\r\n")
                        self.wfile.flush()
                        return
                    data = resp.read()
                    proxy.record({"dir": "s2c", "status": resp.status, "headers": dict(rheaders),
                                  "raw": data.decode("utf-8", "replace")})
                    self.send_response(resp.status)
                    for k, v in rheaders:
                        self.send_header(k, v)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    if data:
                        self.wfile.write(data)
                except (ConnectionError, OSError) as exc:
                    proxy.record({"dir": "proxy", "error": repr(exc)})
                    try:
                        self.send_response(502)
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                    except OSError:
                        pass
                finally:
                    conn.close()

            do_GET = do_POST = do_DELETE = do_PUT = do_OPTIONS = _forward

        self.server = ThreadingHTTPServer(("127.0.0.1", listen_port), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def record(self, entry: dict) -> None:
        entry["t"] = time.time()
        with self.lock, open(self.log_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")

    def start(self) -> "HttpRecordProxy":
        self.thread.start()
        return self

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()


if __name__ == "__main__":
    p = HttpRecordProxy(int(sys.argv[2]), sys.argv[3], int(sys.argv[1])).start()
    print(f"proxy 127.0.0.1:{p.port} -> {sys.argv[2]}", flush=True)
    try:
        p.thread.join()
    except KeyboardInterrupt:
        p.stop()
