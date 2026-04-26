"""Production-stack memory probe.

Question: in a realistic FastAPI + DB + observability deployment, how
much extra RSS does each engine cost per worker process?

For each tier we measure RSS in a fresh subprocess after the imports.
This is the per-uvicorn-worker baseline a real service would carry.
"""
from __future__ import annotations

import json
import subprocess
import sys

PY = sys.executable


def _run(snippet: str) -> dict:
    out = subprocess.check_output([PY, "-c", snippet], stderr=subprocess.STDOUT)
    return json.loads(out.strip().decode().splitlines()[-1])


# --- Stack tiers ---------------------------------------------------------

WEB = """
import fastapi, uvicorn, starlette, pydantic
"""

WEB_DB = WEB + """
import sqlalchemy
import redis
"""

WEB_DB_OBS = WEB_DB + """
import prometheus_client
import opentelemetry
from opentelemetry import trace, metrics
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.metrics import MeterProvider
import structlog
"""

LG = """
import langgraph, langchain_core, langchain_openai
from langgraph.graph import StateGraph, START, END
"""

NG = """
import neograph_engine as ng
"""


def measure(label: str, body: str) -> dict:
    snippet = f"""
import json, psutil
p = psutil.Process()
baseline = p.memory_info().rss / 1024**2
{body}
post = p.memory_info().rss / 1024**2
print(json.dumps({{"label": "{label}", "baseline_mb": round(baseline,1),
                   "post_mb": round(post,1),
                   "delta_mb": round(post - baseline,1)}}))
"""
    return _run(snippet)


def measure_concurrent_capacity(stack_imports: str, engine_label: str) -> dict:
    """How many concurrent in-flight 'worker' tasks fit in 100 MB on top
    of the stack baseline? We approximate by measuring per-task delta."""
    snippet = f"""
import json, psutil, time, asyncio
p = psutil.Process()
{stack_imports}
post_imports = p.memory_info().rss / 1024**2

# Workload: hold N concurrent fan-out branches doing sleep(0.5).
async def task():
    await asyncio.sleep(0.5)

async def main(n):
    await asyncio.gather(*[task() for _ in range(n)])

# Measure with N=256
N = 256
loop = asyncio.new_event_loop()
asyncio.set_event_loop(loop)
peak = post_imports
def snap():
    global peak
    rss = p.memory_info().rss / 1024**2
    if rss > peak: peak = rss

# Just gather and let it run
fut = asyncio.gather(*[task() for _ in range(N)])
loop.run_until_complete(fut)
snap()

print(json.dumps({{"label": "{engine_label}", "post_imports_mb": round(post_imports,1),
                   "peak_mb": round(peak,1),
                   "delta_mb": round(peak - post_imports,1)}}))
"""
    return _run(snippet)


if __name__ == "__main__":
    tiers = [
        ("FastAPI+uvicorn+pydantic",       WEB),
        ("+sqlalchemy+redis",              WEB_DB),
        ("+prometheus+opentelemetry+log",  WEB_DB_OBS),
    ]

    print("=" * 78)
    print("Per-worker-process baseline (RSS post-import in fresh subprocess)")
    print("=" * 78)
    print(f"  {'tier':<42s}  {'post_mb':>8s}  {'+ NG':>10s}  {'+ LG':>10s}")
    rows = []
    for label, body in tiers:
        bare = measure(label, body)
        ng = measure(f"{label}+NG", body + NG)
        lg = measure(f"{label}+LG", body + LG)
        rows.append((label, bare, ng, lg))
        print(f"  {label:<42s}  {bare['post_mb']:>8.1f}  "
              f"{ng['post_mb']:>6.1f} (+{ng['post_mb']-bare['post_mb']:.1f})  "
              f"{lg['post_mb']:>6.1f} (+{lg['post_mb']-bare['post_mb']:.1f})")

    # Headroom math: how many uvicorn workers fit in 1 GB / 4 GB?
    print()
    print("=" * 78)
    print("Worker count fit per RAM budget (full stack tier-3)")
    print("=" * 78)
    label, bare, ng, lg = rows[-1]
    for budget_gb in [1, 2, 4, 8]:
        budget_mb = budget_gb * 1024
        ng_workers = int(budget_mb // ng["post_mb"])
        lg_workers = int(budget_mb // lg["post_mb"])
        diff = ng_workers - lg_workers
        print(f"  RAM={budget_gb} GB  (= {budget_mb} MB):  "
              f"NG fits {ng_workers:3d} workers  ·  LG fits {lg_workers:3d} workers  "
              f"(+{diff} = +{diff/lg_workers*100:.0f}%)")
