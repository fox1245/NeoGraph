"""Local HTTP Chat client overhead — SchemaProvider vs the OpenAI Python SDK.

Both clients request the same model/messages from an in-process loopback peer
that returns a canned Chat response. This measures client/protocol overhead,
not a hosted model, SDK correctness qualification or saved historical results.

Run with the current neograph-engine build and openai installed:
    python benchmarks/python_clients/bench_openai_clients.py [N=500]
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
        request = stdjson.loads(self.rfile.read(n))
        if (self.path != "/v1/chat/completions" or request.get("model") != "mock"
                or request.get("messages") != [
                    {"role": "user", "content": [{"type": "text", "text": "ping"}]}]
                or request.get("stream", False)):
            self.send_error(400, "expected the buffered benchmark Chat request")
            return
        with self.server.count_lock:
            self.server.request_count += 1
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(CANNED)))
        self.end_headers()
        self.wfile.write(CANNED)
    def log_message(self, *a, **kw): pass


def start_server():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), MockOpenAI)
    srv.request_count = 0
    srv.count_lock = threading.Lock()
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
    import neograph_engine as ng
    from neograph_engine.llm import SchemaProvider

    descriptor = ng.load_provider_descriptor(stdjson.dumps({
        "descriptor_version": 1,
        "revision": 1,
        "id": "local-client-benchmark",
        "family": "openai.chat",
        "connection": {
            "base_url": base_url,
            "paths": {"buffered": "/v1/chat/completions",
                      "streaming": "/v1/chat/completions"},
        },
        "bindings": {"model": "model", "messages": "messages", "stream": "stream",
                     "max_output_tokens": "max_tokens", "usage": ["usage"]},
        "stop_reasons": {"stop": "EndTurn", "length": "MaxTokens",
                         "tool_calls": "ToolUse", "content_filter": "ContentFilter"},
    }))
    provider = SchemaProvider(descriptor, options=ng.ProviderRuntimeOptions())
    request = ng.make_provider_request(
        provider, "mock",
        [ng.ProviderMessage(role=ng.ProviderRole.User, parts=[ng.Text("ping")])],
        mode=ng.ProviderMode.Collect,
    )

    def invoke():
        outcome = provider.invoke(request)
        if outcome.failure is not None:
            raise RuntimeError(f"Local Chat request failed: {outcome.failure}")
        completion = outcome.completion
        if completion is None:
            raise RuntimeError("Local Chat request returned no completion")
        text = "".join(part.value for message in completion.messages
                       for part in message.parts if isinstance(part, ng.Text))
        if text != "ok":
            raise RuntimeError(f"Unexpected local Chat text: {text!r}")

    for _ in range(20):  # warm
        invoke()
    out = []
    for _ in range(n):
        t0 = time.perf_counter_ns()
        invoke()
        out.append((time.perf_counter_ns() - t0) / 1000.0)
    return out


def bench_openai_sdk(base_url: str, n: int) -> list[float]:
    from openai import OpenAI
    out = []
    with OpenAI(api_key="local-benchmark", base_url=base_url + "/v1",
                max_retries=0) as client:
        def invoke():
            completion = client.chat.completions.create(
                model="mock", messages=[
                    {"role": "user", "content": [{"type": "text", "text": "ping"}]}])
            if completion.choices[0].message.content != "ok":
                raise RuntimeError("Unexpected local Chat text from OpenAI SDK")

        for _ in range(20):
            invoke()
        for _ in range(n):
            t0 = time.perf_counter_ns()
            invoke()
            out.append((time.perf_counter_ns() - t0) / 1000.0)
    return out


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    if n <= 0:
        raise SystemExit("N must be positive")
    srv, url = start_server()
    try:
        print(f"[bench] canned local HTTP Chat peer at {url}, iterations per client = {n}\n")

        print("─ NeoGraph (SchemaProvider, libcurl HTTP Chat, typed outcomes) ─")
        ng_us = bench_neograph(url, n)
        report("SchemaProvider", ng_us)
        print()

        print("─ openai Python SDK (httpx + pydantic) ─")
        sdk_us = bench_openai_sdk(url, n)
        report("openai SDK", sdk_us)
        print()

        ratio = statistics.median(sdk_us) / statistics.median(ng_us)
        print(f"[bench] median ratio: openai SDK is {ratio:.2f}× of NeoGraph")
        expected = 2 * (20 + n)
        if srv.request_count != expected:
            raise RuntimeError(f"Local peer accepted {srv.request_count}, expected {expected}")
        print(f"[bench] verified local Chat responses: {srv.request_count}")
    finally:
        srv.shutdown()
        srv.server_close()


if __name__ == "__main__":
    main()
