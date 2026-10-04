"""21 — Compare HTTP/1.1 and HTTP/2 with the typed SDK's libcurl runtime.

Both paths use SchemaProvider and the same admitted Chat descriptor. This
measures this endpoint/workload, not a universal protocol ranking. HTTP/2
needs a TLS endpoint and a libcurl build with HTTP/2 support; an HTTP/1-only
local peer cannot qualify it. No HTTP/3 or WebSocket capability is implied.

Run:
    OPENAI_API_KEY=... OPENAI_MODEL=gpt-4.1-mini python 21_http2_transport.py

For a no-key run, route OPENAI_API_BASE to a local TLS peer that negotiates
both h2 and http/1.1, and set NG_EXAMPLE_CA_FILE to its trusted CA file.
Each protocol sends 20 Chat requests by default: one warmup and three
measured bursts of five. NG_TRANSPORT_PARALLEL and NG_TRANSPORT_ITERS change
those counts. Every request must return a real Chat protocol text completion.
"""

from __future__ import annotations

import concurrent.futures
import os
import statistics
import time

from _common import ask_text, ng, schema_provider

PARALLEL = int(os.getenv("NG_TRANSPORT_PARALLEL", "5"))
ITERS = int(os.getenv("NG_TRANSPORT_ITERS", "3"))
PROMPT = "Reply with a single short factual sentence about apples."


def measure(label, version):
    provider = schema_provider(http_version=version)
    messages = [{"role": "user", "content": PROMPT}]

    def burst():
        started = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=PARALLEL) as executor:
            replies = list(executor.map(lambda _: ask_text(provider, messages), range(PARALLEL)))
        return time.perf_counter() - started, replies

    burst()  # warm the runtime without including initialization in the summary
    walls = []
    for index in range(ITERS):
        wall, replies = burst()
        walls.append(wall)
        print(f"{label} burst {index + 1}/{ITERS}: {wall:.3f}s; response: {replies[0]}")
    return walls


def main():
    if PARALLEL < 1 or ITERS < 1:
        raise ValueError("NG_TRANSPORT_PARALLEL and NG_TRANSPORT_ITERS must be positive")
    h1 = measure("HTTP/1.1", ng.ProviderHttpVersion.Http1_1)
    h2 = measure("HTTP/2", ng.ProviderHttpVersion.Http2PriorKnowledge)
    for label, walls in (("HTTP/1.1", h1), ("HTTP/2", h2)):
        print(f"{label}: median={statistics.median(walls):.3f}s; mean={statistics.fmean(walls):.3f}s")
    print("Confirm negotiated protocol in the peer/transport trace before interpreting the comparison.")


if __name__ == "__main__":
    main()
