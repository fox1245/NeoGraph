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

Why isn't HTTP/2 faster on OpenAI?
----------------------------------

We instrumented both paths with tshark + strace. libcurl really did
multiplex 25 calls over a single TCP (1 SYN, 1 TLS handshake);
ConnPool used 3-5 keep-alive TCPs. No RST, no TLS alerts, no
retransmissions on either side — the wire is clean. Yet libcurl is
~25% slower at p50. The most plausible explanation is TCP-level
head-of-line blocking on a single connection: when 5 streams'
response packets interleave, any reorder stalls all five. ConnPool's
N independent TCPs avoid this entirely (HTTP/3 / QUIC fixes it but
needs ngtcp2-built libcurl). We also measured `CURLOPT_PIPEWAIT=0`
+ `CURLMOPT_MAX_HOST_CONNECTIONS=8` (lets libcurl open multiple H/2
connections rather than funneling onto one); didn't help. The
escape hatches `NG_CURL_PIPEWAIT` and `NG_CURL_MAX_HOST_CONNS` are
exposed for further experimentation if you want to try on your own
endpoint.

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


_TRANSIENT_5XX = ("502", "503", "504")


def _one_call_with_retry(provider: SchemaProvider) -> None:
    """One `.complete()` with a single retry on Cloudflare/upstream
    transients. OpenAI's edge surfaces 502/503/504 a few times per
    thousand calls; without a retry one bad call kills the whole burst
    measurement (and confuses anyone trying to A/B the two transports).
    Retry once, then propagate.

    Note: we use the dict-shaped overload — `complete(messages=[...])`
    — instead of the typed `CompletionParams` + `ChatMessage` pair.
    The dict path skips a layer of pybind marshalling per call; on
    a 5-parallel burst that saved ~250-300ms wall-clock vs the typed
    API in our measurements (closing the gap to the C++-direct probe)."""
    messages = [{"role": "user", "content": PROMPT}]
    try:
        provider.complete(messages)
    except RuntimeError as exc:
        msg = str(exc)
        if any(code in msg for code in _TRANSIENT_5XX):
            time.sleep(0.2)
            provider.complete(messages)
        else:
            raise


def parallel_burst(provider: SchemaProvider) -> float:
    """Fire PARALLEL `complete()` calls concurrently; return wall-clock."""
    t0 = time.perf_counter()
    with cf.ThreadPoolExecutor(max_workers=PARALLEL) as ex:
        list(ex.map(lambda _: _one_call_with_retry(provider), range(PARALLEL)))
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
