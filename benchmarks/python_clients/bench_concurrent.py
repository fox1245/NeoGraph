"""Concurrent A2A throughput — GIL behavior under load.

NeoGraph's `A2AClient.send_message` releases the GIL during the HTTP
exchange (every wrapped C++ call uses `gil_scoped_release`), so
`ThreadPoolExecutor` scales linearly until the network or the server
saturates. The `a2a-sdk` Python client is asyncio-native, also non-
blocking in its event loop. We compare both at K=1, 4, 16, 64
concurrent in-flight requests against the same mock server.

Run:
    python bench_concurrent.py [N=500]
"""

import asyncio
import json as stdjson
import statistics
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from uuid import uuid4

# Reuse mock from the sequential bench.
sys.path.insert(0, "/root/Coding/NeoGraph/benchmarks/python_clients")
from bench_a2a_clients import start_server  # noqa: E402


def neograph_concurrent(url: str, total: int, k: int) -> tuple[float, float]:
    """Return (wall_seconds, throughput_req_per_s)."""
    import neograph_engine as ng
    client = ng.a2a.A2AClient(url)
    client.set_timeout(30)
    client.fetch_agent_card()
    for _ in range(20):
        client.send_message("warm")  # warm

    def one(_): client.send_message("ping")
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=k) as ex:
        list(ex.map(one, range(total)))
    wall = time.perf_counter() - t0
    return wall, total / wall


async def _sdk_one(client):
    from a2a.types import (Message, Part, Role, SendMessageRequest,
                           SendMessageConfiguration)
    req = SendMessageRequest(
        message=Message(message_id=str(uuid4()), role=Role.ROLE_USER,
                        parts=[Part(text="ping")]),
        configuration=SendMessageConfiguration())
    async for _ in client.send_message(req):
        break


def a2a_sdk_concurrent(url: str, total: int, k: int) -> tuple[float, float]:
    import httpx
    from a2a.client import A2ACardResolver
    from a2a.client.client_factory import ClientFactory, ClientConfig

    async def go():
        # Bigger pool so the SDK isn't bottlenecked on httpx connection limits.
        limits = httpx.Limits(max_connections=256, max_keepalive_connections=128)
        async with httpx.AsyncClient(timeout=30, limits=limits) as h:
            resolver = A2ACardResolver(httpx_client=h, base_url=url)
            card = await resolver.get_agent_card()
            factory = ClientFactory(ClientConfig(httpx_client=h))
            client = factory.create(card)
            for _ in range(20):  # warm
                await _sdk_one(client)

            sem = asyncio.Semaphore(k)
            async def gated():
                async with sem:
                    await _sdk_one(client)
            t0 = time.perf_counter()
            await asyncio.gather(*(gated() for _ in range(total)))
            return time.perf_counter() - t0

    wall = asyncio.run(go())
    return wall, total / wall


def main():
    total = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    srv, url = start_server()
    print(f"[bench] mock A2A at {url}, total requests = {total} per row\n")
    print(f"  {'K':>4}  {'NeoGraph req/s':>16}  {'a2a-sdk req/s':>16}  speedup")
    for k in (1, 4, 16, 64):
        ng_wall, ng_rps = neograph_concurrent(url, total, k)
        sdk_wall, sdk_rps = a2a_sdk_concurrent(url, total, k)
        print(f"  {k:>4}  {ng_rps:>16,.0f}  {sdk_rps:>16,.0f}  {ng_rps/sdk_rps:>4.2f}×")
    srv.shutdown()


if __name__ == "__main__":
    main()
