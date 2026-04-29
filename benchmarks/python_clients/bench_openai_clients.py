"""OpenAI client overhead — NeoGraph (`neograph_engine.llm.OpenAIProvider`)
vs `openai` Python SDK.

Both clients hit the same in-process Python mock OpenAI-compatible
server that returns a canned chat completion in <1 ms, so the model
call itself is constant. The difference we measure is purely client
side — header build, JSON serialise, HTTP exchange, JSON parse.

Run:
    pip install neograph-engine==0.2.2 openai
    python bench_openai_clients.py [N=500]
"""

import json as stdjson
import statistics
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


CANNED = stdjson.dumps({
    "id": "x",
    "object": "chat.completion",
    "created": 0,
    "model": "mock",
    "choices": [{
        "index": 0,
        "message": {"role": "assistant", "content": "ok"},
        "finish_reason": "stop",
    }],
    "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
}).encode()


class MockOpenAI(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        if n:
            self.rfile.read(n)
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(CANNED)))
        self.end_headers()
        self.wfile.write(CANNED)
    def log_message(self, *a, **kw): pass


def start_server():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), MockOpenAI)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{srv.server_address[1]}"


def percentile(s, p):
    s = sorted(s)
    return s[min(len(s) - 1, int(len(s) * p))]


def report(label, us):
    print(f"  {label:30s}"
          f" median={statistics.median(us):8.1f} µs"
          f"  p95={percentile(us,0.95):8.1f} µs"
          f"  mean={statistics.mean(us):8.1f} µs"
          f"  throughput={len(us)/(sum(us)/1e6):8.0f} req/s")


def bench_neograph(base_url: str, n: int) -> list[float]:
    from neograph_engine.llm import OpenAIProvider
    from neograph_engine import CompletionParams, ChatMessage
    p = OpenAIProvider(api_key="sk-mock", base_url=base_url,
                       default_model="mock")
    cp = CompletionParams()
    cp.model = "mock"
    cp.messages = [ChatMessage("user", "ping")]
    for _ in range(20):  # warm
        p.complete(cp)
    out = []
    for _ in range(n):
        t0 = time.perf_counter_ns()
        p.complete(cp)
        out.append((time.perf_counter_ns() - t0) / 1000.0)
    return out


def bench_openai_sdk(base_url: str, n: int) -> list[float]:
    from openai import OpenAI
    client = OpenAI(api_key="sk-mock", base_url=base_url + "/v1")
    for _ in range(20):
        client.chat.completions.create(model="mock",
            messages=[{"role": "user", "content": "ping"}])
    out = []
    for _ in range(n):
        t0 = time.perf_counter_ns()
        client.chat.completions.create(model="mock",
            messages=[{"role": "user", "content": "ping"}])
        out.append((time.perf_counter_ns() - t0) / 1000.0)
    return out


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    srv, url = start_server()
    print(f"[bench] mock OpenAI at {url}, iterations per client = {n}\n")

    print("─ NeoGraph (neograph_engine.llm.OpenAIProvider, native asio) ─")
    ng_us = bench_neograph(url, n)
    report("neograph_engine.llm", ng_us)
    print()

    print("─ openai Python SDK (httpx + pydantic) ─")
    sdk_us = bench_openai_sdk(url, n)
    report("openai SDK", sdk_us)
    print()

    ratio = statistics.median(sdk_us) / statistics.median(ng_us)
    print(f"[bench] median ratio: openai SDK is {ratio:.2f}× of NeoGraph")

    srv.shutdown()


if __name__ == "__main__":
    main()
