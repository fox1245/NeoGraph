"""Shared measurement code of the burst harnesses (record schema 2).

One burst submits N runs of the 3-node counter chain at one instant and waits
until all of them finish (a closed single burst, not a steady-state load).
Every harness reports the same record:

  service_*_us   time from the moment a run started executing to its completion
  response_*_us  time from the ONE common submit instant (taken before the first
                 task is created or posted) to the completion of the run
  total_wall_us  common submit instant to the last completion, in microseconds

A cell is valid only when every run returned the right answer. Each run result
is checked (the chain must end with counter == EXPECTED) after the service-time
clock stops; a mismatch or an exception counts as a failure. A cell with no
success or with any failure has status "error", null percentiles, the first
error text in "first_error", and the process exits with status 2.

Percentiles use the nearest-rank method, ceil(p * n) - 1 on the sorted samples.
p95 is published from n >= 100 and p99 from n >= 1000 samples; otherwise the
field is null. p50, p90, max and n are always present for a valid cell.

Memory: "peak_rss_kb" has a scope given by "rss_scope". "process" is the peak of
this one process (VmHWM). "process_tree" is the parent's peak plus the sum of
the pool workers' peaks (each worker's VmHWM read from /proc before the pool
is closed, so workers that received no task are included; the largest ru_maxrss
a worker reported with a result is the fallback); pages shared between a parent and its forked workers are counted once
per process, so the tree figure is an upper bound of the memory in use.
"""

from __future__ import annotations

import asyncio
import json
import math
import multiprocessing as mp
import os
import resource
import sys
import time
from typing import Any, Awaitable, Callable

SCHEMA = 2
EXPECTED = 3  # the chain a -> b -> c adds one per node, starting from 0
FIRST_ERROR_LIMIT = 400
P95_MIN_SAMPLES = 100
P99_MIN_SAMPLES = 1000

Clock = time.clock_gettime_ns


def now_ns() -> int:
    """CLOCK_MONOTONIC in ns: one clock for every process on the machine."""
    return Clock(time.CLOCK_MONOTONIC)


def vm_kb(field: str) -> int:
    """A kB field of /proc/self/status (VmRSS, VmHWM), or 0 where unavailable."""
    try:
        with open("/proc/self/status", encoding="ascii") as handle:
            for line in handle:
                if line.startswith(field + ":"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def proc_hwm_kb(pid: int) -> int:
    """VmHWM of another process, or 0 where /proc is unavailable."""
    try:
        with open(f"/proc/{pid}/status", encoding="ascii") as handle:
            for line in handle:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def peak_rss_kb() -> int:
    # ru_maxrss is kB on Linux. Prefer VmHWM, which is the same quantity.
    return vm_kb("VmHWM") or resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def nearest_rank(sorted_values: list[float], p: float) -> float:
    return sorted_values[max(0, math.ceil(p * len(sorted_values)) - 1)]


def percentile_fields(prefix: str, values_us: list[float], valid: bool) -> dict[str, Any]:
    """p50/p90/p95/p99/max of one latency kind, null where not publishable."""
    names = ("p50", "p90", "p95", "p99", "max")
    if not valid or not values_us:
        return {f"{prefix}_{name}_us": None for name in names}
    ordered = sorted(values_us)
    count = len(ordered)
    return {
        f"{prefix}_p50_us": round(nearest_rank(ordered, 0.50), 1),
        f"{prefix}_p90_us": round(nearest_rank(ordered, 0.90), 1),
        f"{prefix}_p95_us": round(nearest_rank(ordered, 0.95), 1) if count >= P95_MIN_SAMPLES else None,
        f"{prefix}_p99_us": round(nearest_rank(ordered, 0.99), 1) if count >= P99_MIN_SAMPLES else None,
        f"{prefix}_max_us": round(ordered[-1], 1),
    }


def describe_error(error: BaseException) -> str:
    text = f"{type(error).__name__}: {error}"
    return text if len(text) <= FIRST_ERROR_LIMIT else text[: FIRST_ERROR_LIMIT - 3] + "..."


class Collector:
    """Accumulates the outcome of the runs of one burst (single event loop or one process)."""

    def __init__(self) -> None:
        self.service_us: list[float] = []
        self.response_us: list[float] = []
        self.errors = 0
        self.first_error: str | None = None

    def success(self, t_start_ns: int, t_end_ns: int, t_submit_ns: int) -> None:
        self.service_us.append((t_end_ns - t_start_ns) / 1000.0)
        self.response_us.append((t_end_ns - t_submit_ns) / 1000.0)

    def failure(self, reason: str) -> None:
        self.errors += 1
        if self.first_error is None:
            self.first_error = reason


def check_equal(label: str, actual: Any, expected: Any = EXPECTED) -> None:
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


# ── asyncio burst ──────────────────────────────────────────────────────

async def burst_async(
    concurrency: int,
    call: Callable[[], Awaitable[Any]],
    check: Callable[[Any], None],
) -> tuple[Collector, int]:
    """N tasks created at one instant; returns the collector and the wall in ns.

    `call` runs one chain and returns the raw framework result; `check` raises
    when the result is wrong. The check runs after the service clock stopped.
    """
    collector = Collector()

    async def one(t_submit: int) -> None:
        t_start = now_ns()
        try:
            raw = await call()
            t_end = now_ns()
            check(raw)
        except Exception as error:  # noqa: BLE001 - every failure is recorded
            collector.failure(describe_error(error))
            return
        collector.success(t_start, t_end, t_submit)

    t_submit = now_ns()
    tasks = [asyncio.create_task(one(t_submit)) for _ in range(concurrency)]
    await asyncio.gather(*tasks)
    return collector, now_ns() - t_submit


# ── multiprocessing burst ──────────────────────────────────────────────

# Worker side. Each task returns (t_start_ns, t_end_ns, error_or_None, pid, maxrss_kb).

def timed_call(run_once: Callable[[], Any], check: Callable[[Any], None]) -> tuple:
    t_start = now_ns()
    try:
        raw = run_once()
        t_end = now_ns()
        check(raw)
        error = None
    except Exception as exc:  # noqa: BLE001
        t_end = now_ns()
        error = describe_error(exc)
    return (t_start, t_end, error, os.getpid(),
            resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)


_ready = None


def _worker_init(ready: Any, user_init: Callable[[], None] | None) -> None:
    global _ready
    _ready = ready
    if user_init is not None:
        try:
            user_init()
        except Exception:  # noqa: BLE001
            # A failing warm-up must not kill the worker (the pool would respawn
            # it forever). The runs themselves fail and report the error.
            pass
    with ready.get_lock():
        ready.value += 1


def burst_mp(
    concurrency: int,
    pool_size: int,
    task: Callable[[int], tuple],
    worker_init: Callable[[], None] | None = None,
) -> tuple[Collector, int, dict[str, Any]]:
    """N tasks on a process pool that is started and warmed before the clock.

    `worker_init` runs once in every worker (build the graph, one warm-up call).
    The common submit instant is taken after all workers are ready, so pool
    start-up is reported (pool_startup_us) but not part of the wall or latencies.
    """
    ready = mp.Value("i", 0)
    startup_begin = now_ns()
    collector = Collector()
    with mp.Pool(pool_size, initializer=_worker_init, initargs=(ready, worker_init)) as pool:
        deadline = time.monotonic() + 300.0
        while ready.value < pool_size:
            if time.monotonic() > deadline:
                raise RuntimeError(f"only {ready.value} of {pool_size} pool workers became ready")
            time.sleep(0.005)
        t_submit = now_ns()
        pool_startup_ns = t_submit - startup_begin
        rows = pool.map(task, range(concurrency))
        wall_ns = now_ns() - t_submit
        # Every worker, including those that received no task: its VmHWM, read
        # from /proc while the pool is still alive.
        worker_peak: dict[int, int] = {}
        for child in mp.active_children():
            if child.pid is not None:
                worker_peak[child.pid] = proc_hwm_kb(child.pid)
    for t_start, t_end, error, pid, maxrss in rows:
        worker_peak[pid] = max(worker_peak.get(pid, 0), maxrss)
        if error is None:
            collector.success(t_start, t_end, t_submit)
        else:
            collector.failure(error)
    children_max = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return collector, wall_ns, {
        "pool_size": pool_size,
        "pool_startup_us": pool_startup_ns // 1000,
        "worker_pids": sorted(worker_peak),
        "peak_rss_workers_kb": [worker_peak[pid] for pid in sorted(worker_peak)],
        "peak_rss_children_max_kb": children_max,
    }


# ── record ─────────────────────────────────────────────────────────────

def build_record(
    engine: str,
    mode: str,
    concurrency: int,
    collector: Collector,
    wall_ns: int,
    *,
    note: str,
    rss_after_import_kb: int,
    rss_before_burst_kb: int,
    mp_info: dict[str, Any] | None = None,
) -> dict[str, Any]:
    ok = len(collector.service_us)
    err = collector.errors
    valid = ok > 0 and err == 0 and ok == concurrency
    first_error = collector.first_error
    if first_error is None and not valid:
        first_error = f"{ok} of {concurrency} runs completed"
    wall_us = wall_ns // 1000
    parent_peak = peak_rss_kb()
    record: dict[str, Any] = {
        "schema": SCHEMA,
        "engine": engine,
        "mode": mode,
        "concurrency": concurrency,
        "status": "ok" if valid else "error",
        "ok": ok,
        "err": err,
        "first_error": None if valid else first_error,
        "check": f"counter == {EXPECTED} on every run, checked after the service clock stopped",
        "note": note,
        "clock": "CLOCK_MONOTONIC",
        "percentile_method": "nearest-rank",
        "samples": ok,
        "total_wall_us": wall_us,
        "total_wall_ms": round(wall_us / 1000.0, 3),
    }
    record.update(percentile_fields("service", collector.service_us, valid))
    record.update(percentile_fields("response", collector.response_us, valid))
    if mp_info is None:
        record.update({
            "rss_scope": "process",
            "peak_rss_kb": parent_peak,
            "peak_rss_parent_kb": parent_peak,
            "peak_rss_workers_kb": [],
        })
    else:
        record.update({
            "rss_scope": "process_tree",
            "peak_rss_kb": parent_peak + sum(mp_info["peak_rss_workers_kb"]),
            "peak_rss_parent_kb": parent_peak,
            **{key: value for key, value in mp_info.items() if key != "pool_size"},
        })
    record["rss_after_import_kb"] = rss_after_import_kb
    record["rss_before_burst_kb"] = rss_before_burst_kb
    return record


def guard_warmup(
    engine: str,
    mode: str,
    concurrency: int,
    warm: Callable[[], Any],
    rss_after_import_kb: int,
) -> dict[str, Any] | None:
    """Run the warm-up; None when it succeeded, else an error record for the cell.

    A warm-up that fails or returns a wrong answer means the workload cannot
    run, so the cell is an error with the first exception recorded.
    """
    try:
        warm()
    except Exception as error:  # noqa: BLE001
        collector = Collector()
        collector.failure("warm-up failed: " + describe_error(error))
        collector.errors = concurrency
        return build_record(
            engine, mode, concurrency, collector, 0,
            note="the warm-up before the burst failed; no burst was run",
            rss_after_import_kb=rss_after_import_kb,
            rss_before_burst_kb=vm_kb("VmRSS"),
        )
    return None


def finish(record: dict[str, Any]) -> None:
    """Print the record on stdout and exit 0 for a valid cell, 2 otherwise."""
    print(json.dumps(record))
    sys.stdout.flush()
    if record["status"] != "ok":
        print(f"{record['engine']} {record['mode']} N={record['concurrency']}: "
              f"{record['err']} failed, {record['ok']} ok; first error: {record['first_error']}",
              file=sys.stderr)
        sys.exit(2)
    sys.exit(0)


def parse_args(name: str) -> tuple[str, int]:
    if len(sys.argv) != 3 or sys.argv[1] not in ("async", "mp"):
        print(f"usage: {name} {{async|mp}} <concurrency>", file=sys.stderr)
        sys.exit(1)
    try:
        concurrency = int(sys.argv[2])
    except ValueError:
        concurrency = 0
    if concurrency < 1:
        print(f"{name}: concurrency must be a positive integer, got {sys.argv[2]!r}", file=sys.stderr)
        sys.exit(1)
    return sys.argv[1], concurrency


def inject_failure(label: str) -> None:
    """Test hook: BURST_INJECT_FAILURE=<label> makes the node of that harness raise.

    It exists so a harness can be shown to report status "error", a null
    percentile row, its first exception and exit status 2.
    """
    if os.environ.get("BURST_INJECT_FAILURE") in (label, "all"):
        raise RuntimeError(f"injected failure in {label}")
