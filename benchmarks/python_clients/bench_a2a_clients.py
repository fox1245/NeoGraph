"""A2A client overhead — NeoGraph (`neograph_engine.a2a`) vs `a2a-sdk` Python.

Both clients hit the same in-process Python mock A2A server that
returns a canned Task on every message/send. The server cost is a
flat constant; the difference we measure is purely client-side
overhead — JSON-RPC envelope build, HTTP write, parse.

Run:
    pip install neograph-engine==0.2.2 a2a-sdk httpx
    python bench_a2a_clients.py [N=500]
"""

import asyncio
import json as stdjson
import statistics
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from uuid import uuid4


# ── Mock A2A server ─────────────────────────────────────────────────────
AGENT_CARD = {
    "name": "bench-mock",
    "description": "mock A2A endpoint for client overhead bench",
    "url": "http://127.0.0.1:0/",
    "version": "0.0.1",
    "protocolVersion": "0.3.0",
    "preferredTransport": "JSONRPC",
    "capabilities": {"streaming": False, "pushNotifications": False},
    "defaultInputModes": ["text/plain"],
    "defaultOutputModes": ["text/plain"],
    "skills": [{"id": "echo", "name": "echo", "description": "echo"}],
}

CANNED_TASK = {
    "kind": "task",
    "id": "t-bench",
    "contextId": "c-bench",
    "status": {"state": "completed"},
    "history": [{
        "kind": "message", "messageId": "m-bench", "role": "agent",
        "parts": [{"kind": "text", "text": "ok"}],
    }],
}


class MockA2A(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/.well-known/agent-card.json":
            body = stdjson.dumps(AGENT_CARD).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404); self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        raw = self.rfile.read(n) if n else b""
        try:
            req = stdjson.loads(raw)
            rid = req.get("id", 0)
        except Exception:
            rid = 0
        body = stdjson.dumps({"jsonrpc": "2.0", "id": rid,
                              "result": CANNED_TASK}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a, **kw): pass


def start_server():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), MockA2A)
    srv.daemon_threads = True
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    base = f"http://127.0.0.1:{srv.server_address[1]}"
    # The AgentCard's `url` field is what a2a-sdk's transport uses for
    # subsequent JSON-RPC posts; rewrite the placeholder to the real
    # bound URL so the SDK doesn't try to dial port 0.
    AGENT_CARD["url"] = base + "/"
    return srv, base


# ── Helpers ─────────────────────────────────────────────────────────────
def percentile(s, p):
    s = sorted(s)
    return s[min(len(s) - 1, int(len(s) * p))]


def report(label, us):
    print(f"  {label:24s}"
          f" median={statistics.median(us):8.1f} µs"
          f"  p95={percentile(us,0.95):8.1f} µs"
          f"  mean={statistics.mean(us):8.1f} µs"
          f"  throughput={len(us)/(sum(us)/1e6):8.0f} req/s")


# ── Clients ─────────────────────────────────────────────────────────────
def bench_neograph(url: str, n: int) -> list[float]:
    import neograph_engine as ng
    c = ng.a2a.A2AClient(url)
    c.set_timeout(30)
    c.fetch_agent_card()  # warm
    for _ in range(20):  # warm tcp + jit
        c.send_message("ping")
    out = []
    for _ in range(n):
        t0 = time.perf_counter_ns()
        c.send_message("ping")
        out.append((time.perf_counter_ns() - t0) / 1000.0)
    return out


def bench_a2a_sdk(url: str, n: int) -> list[float]:
    import httpx
    from a2a.client import A2ACardResolver
    from a2a.client.client_factory import ClientFactory, ClientConfig
    from a2a.types import (
        Message, Part, Role, SendMessageRequest, SendMessageConfiguration,
    )
    out = []

    async def go():
        async with httpx.AsyncClient(timeout=30) as h:
            resolver = A2ACardResolver(httpx_client=h, base_url=url)
            card = await resolver.get_agent_card()
            factory = ClientFactory(ClientConfig(httpx_client=h))
            client = factory.create(card)

            async def one():
                req = SendMessageRequest(
                    message=Message(message_id=str(uuid4()),
                                    role=Role.ROLE_USER,
                                    parts=[Part(text="ping")]),
                    configuration=SendMessageConfiguration(),
                )
                async for _ in client.send_message(req):
                    break

            for _ in range(20):  # warm
                await one()
            for _ in range(n):
                t0 = time.perf_counter_ns()
                await one()
                out.append((time.perf_counter_ns() - t0) / 1000.0)

    asyncio.run(go())
    return out


# ── Driver ──────────────────────────────────────────────────────────────
def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    srv, url = start_server()
    print(f"[bench] mock A2A at {url}, iterations per client = {n}\n")

    print("─ NeoGraph (neograph_engine.a2a, sync, native asio+OpenSSL) ─")
    ng_us = bench_neograph(url, n)
    report("neograph_engine.a2a", ng_us)
    print()

    print("─ a2a-sdk Python (asyncio + httpx) ─")
    sdk_us = bench_a2a_sdk(url, n)
    report("a2a-sdk", sdk_us)
    print()

    ratio = statistics.median(sdk_us) / statistics.median(ng_us)
    print(f"[bench] median ratio: a2a-sdk is {ratio:.1f}× of NeoGraph")

    srv.shutdown()


if __name__ == "__main__":
    main()
