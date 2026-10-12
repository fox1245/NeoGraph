"""A tiny HTTP server for test_server_ports.py: every request is answered with its label.

    port_stub_server.py --port N [--label L] [--delay S] [--die CODE]

--port 0 binds a port the OS picks; the stub prints `READY <port>` either way,
as a server that reports its own port does. --delay waits before binding (a server that is slow
to start); --die exits with CODE and a message instead of serving.
"""

import argparse
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--label", default="real")
    parser.add_argument("--delay", type=float, default=0.0)
    parser.add_argument("--die", type=int)
    args = parser.parse_args()
    if args.die is not None:
        print("the stub cannot start for a reason of its own", file=sys.stderr, flush=True)
        return args.die
    time.sleep(args.delay)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            body = args.label.encode()
            self.send_response(200)
            self.send_header("X-Identity", args.label)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    # Binding fails like a real server's does when the number is taken:
    # OSError "Address already in use" on stderr, exit status 1.
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"READY {server.server_address[1]}", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
