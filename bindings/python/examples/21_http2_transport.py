"""21 — HTTP/2 transport (libcurl) as an opt-in alternative to the
default HTTP/1.1 keep-alive ConnPool.

When to flip the switch
-----------------------

The default `SchemaProvider` transport is `async::ConnPool` —
HTTP/1.1 with keep-alive, asio-native coroutines, one warm TCP per
in-flight request. It's measurably faster than HTTP/2 multiplexing
on the OpenAI endpoint we benchmarked. Don't flip the switch
unless one of these applies to you:

  1. **Cloudflare-WAF protected endpoints** — some providers
     fingerprint plain httplib's TLS+H1 signature and 400 it.
     libcurl's signature passes (it IS curl). Symptom: every call
     fails with a 400 or 403 and curl from the shell works fine.

  2. **Corporate proxy / API gateway** — the infra team complains
     that your service is holding hundreds of warm TCP connections
     in the gateway's session table. HTTP/2 multiplexes streams
     over one TCP per upstream, so the session count drops by a
     factor of `worker_count`.

  3. **High-churn networks** — flaky connectivity, frequent
     idle-timeout disconnects. HTTP/2's one-handshake-many-streams
     pays off when the per-call cost of re-establishing the pool
     starts to bite.

  4. **HTTP/3 (QUIC) needed** — if your runtime libcurl was built
     with quictls/ngtcp2, flipping `prefer_libcurl=True` enables
     it transparently.

If none of the above apply, keep the default. We measured
ConnPool ~23% faster at p50 on a 5-way parallel-burst probe to
api.openai.com.

Run:
    pip install neograph-engine python-dotenv
    cp .env.example .env   # fill in OPENAI_API_KEY
    python 21_http2_transport.py
"""

from __future__ import annotations

import concurrent.futures as cf
import os
import statistics
import sys
import time

import _common  # side-effect: loads .env  # noqa: F401
from neograph_engine import ChatMessage, CompletionParams
from neograph_engine.llm import SchemaProvider

API_KEY = os.getenv("OPENAI_API_KEY", "")
MODEL   = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
PROMPT  = "Reply with a single short factual sentence about apples."
PARALLEL = 5
ITERS    = 3

if not API_KEY:
    print("OPENAI_API_KEY not set in environment or .env file.")
    print("Skipping the live transport comparison.")
    sys.exit(0)


def make_provider(*, http2: bool) -> SchemaProvider:
    """Build a SchemaProvider with either transport.

    `prefer_libcurl=True` switches the underlying HTTP path from
    `async::ConnPool` (HTTP/1.1 keep-alive) to `async::CurlH2Pool`
    (libcurl-driven HTTP/2 with stream multiplexing). The Provider
    surface — `complete()` shape, message format, error handling —
    is identical either way; the swap is purely transport-layer.
    """
    return SchemaProvider(
        schema_path="openai",
        api_key=API_KEY,
        default_model=MODEL,
        use_websocket=False,        # libcurl path is HTTP-only.
        prefer_libcurl=http2,       # ← the only knob that matters here.
    )


def parallel_burst(provider: SchemaProvider) -> float:
    """Fire PARALLEL `complete()` calls concurrently; return wall-clock."""
    def one() -> None:
        provider.complete(CompletionParams(
            model=MODEL,
            messages=[ChatMessage("user", PROMPT)],
        ))
    t0 = time.perf_counter()
    with cf.ThreadPoolExecutor(max_workers=PARALLEL) as ex:
        list(ex.map(lambda _: one(), range(PARALLEL)))
    return time.perf_counter() - t0


def measure(label: str, *, http2: bool) -> list[float]:
    print(f"\n[{label}] prefer_libcurl={http2}")
    provider = make_provider(http2=http2)
    # Warmup so the first burst doesn't pay TLS / pool-fill cost.
    parallel_burst(provider)
    walls = []
    for i in range(ITERS):
        wall = parallel_burst(provider)
        walls.append(wall)
        print(f"  burst {i + 1}/{ITERS}: {wall:.2f}s wall "
              f"({PARALLEL} parallel calls)")
    return walls


def summary(label: str, walls: list[float]) -> None:
    p50 = statistics.median(walls)
    print(f"  → {label:8s} p50 {p50:.2f}s  mean {statistics.fmean(walls):.2f}s")


print(f"Comparing transports against {os.getenv('OPENAI_API_BASE', 'api.openai.com')}")
print(f"  model: {MODEL}")
print(f"  workload: {PARALLEL} parallel POST per burst, {ITERS} bursts each")

connpool_walls = measure("ConnPool HTTP/1.1", http2=False)
libcurl_walls  = measure("libcurl  HTTP/2",   http2=True)

print("\n" + "=" * 60)
print("Results")
print("=" * 60)
summary("connpool", connpool_walls)
summary("libcurl",  libcurl_walls)

cp_med = statistics.median(connpool_walls)
lc_med = statistics.median(libcurl_walls)
if cp_med < lc_med:
    pct = (lc_med / cp_med - 1) * 100
    print(f"\nOn YOUR endpoint, ConnPool is {pct:.0f}% faster at p50.")
    print("Recommendation: keep the default. Flip prefer_libcurl=True only")
    print("for the WAF / proxy / churn / HTTP/3 reasons in the docstring.")
else:
    pct = (cp_med / lc_med - 1) * 100
    print(f"\nOn YOUR endpoint, libcurl HTTP/2 is {pct:.0f}% faster at p50.")
    print("Recommendation: pass prefer_libcurl=True when constructing")
    print("SchemaProvider, or set NG_PREFER_LIBCURL=1 if you're using")
    print("the bench / examples helpers in _common.py.")
