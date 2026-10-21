"""Shared pieces of the cross-framework cells: memory readings, the cell-validity rule and the timed loops.

A cell is one process that times (or measures) one variant of one framework on one workload. Its single JSON
record on stdout says how many runs completed, how many failed, whether the answer was right and what the
timings were. The rules every cell obeys:

* The answer of the workload is checked: the last run of every sample, and one extra run after the clock
  has stopped (so the checks cost the timing nothing).
* With zero completions, any failure or a wrong answer the record has status "error", no timings, the first
  error text, and the process exits non-zero. No timing is published for such a cell.
* Warm-up runs the very object that is timed, for both workloads.

The module imports nothing heavy at import time, so the cold-start cell measures the framework and not this file.
"""

from __future__ import annotations

import sys
import time

SCHEMA = 1
WORKLOADS = ("seq", "par")


class CellError(Exception):
    """A cell that did not produce a valid answer."""


def proc_status_kb(key: str, path: str = "/proc/self/status") -> int:
    """A kB figure from /proc (VmRSS, VmHWM, Pss); 0 where unavailable."""
    try:
        with open(path, encoding="ascii") as handle:
            for line in handle:
                if line.startswith(key + ":"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def memory_snapshot() -> dict:
    return {
        "vm_rss_kb": proc_status_kb("VmRSS"),
        "vm_hwm_kb": proc_status_kb("VmHWM"),
        "pss_kb": proc_status_kb("Pss", "/proc/self/smaps_rollup"),
    }


def malloc_trim() -> None:
    """Ask glibc to return free heap to the kernel, so a reading is not inflated by earlier garbage."""
    try:
        import ctypes

        ctypes.CDLL("libc.so.6").malloc_trim(0)
    except (OSError, AttributeError):
        pass


def package_versions(packages: list[str]) -> dict:
    from importlib import metadata

    found = {}
    for name in packages:
        try:
            found[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            found[name] = None
    return found


def monotonic_ns() -> int:
    return time.monotonic_ns()


def median(values: list[float]) -> float:
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


class Gate:
    """Parks runs in flight: a node awaits `wait()` until the harness opens the gate."""

    def __init__(self) -> None:
        import asyncio

        self.event = asyncio.Event()
        self.parked = 0
        self.hold = True

    async def wait(self) -> None:
        if not self.hold:
            return
        self.parked += 1
        await self.event.wait()


def error_record(base: dict, error: str) -> dict:
    base = dict(base)
    base["status"] = "error"
    base["first_error"] = error[:600]
    base["sample_s"] = []
    base["median_s"] = None
    base["per_run_us"] = None
    return base


async def _loop_async(call, check, iters: int, samples: int, warmup: int, record: dict) -> None:
    import gc

    last = None
    for _ in range(warmup):
        last = await call()
    if warmup:
        check(last)
    for _ in range(samples):
        gc.collect()
        started = time.perf_counter()
        try:
            for _ in range(iters):
                last = await call()
        except Exception as error:  # a failed run invalidates the cell
            record["failures"] += 1
            raise CellError(f"{type(error).__name__}: {error}") from error
        elapsed = time.perf_counter() - started
        record["completions"] += iters
        check(last)
        check(await call())
        record["checked_runs"] += 2
        record["sample_s"].append(elapsed)


def _loop_sync(call, check, iters: int, samples: int, warmup: int, record: dict) -> None:
    import gc

    last = None
    for _ in range(warmup):
        last = call()
    if warmup:
        check(last)
    for _ in range(samples):
        gc.collect()
        started = time.perf_counter()
        try:
            for _ in range(iters):
                last = call()
        except Exception as error:
            record["failures"] += 1
            raise CellError(f"{type(error).__name__}: {error}") from error
        elapsed = time.perf_counter() - started
        record["completions"] += iters
        check(last)
        check(call())
        record["checked_runs"] += 2
        record["sample_s"].append(elapsed)


def run_loop(port, variant: str, workload: str, iters: int, samples: int, warmup: int, base: dict) -> dict:
    """Time `iters` runs per sample, `samples` samples, on one built graph."""
    record = dict(base)
    record.update({"completions": 0, "failures": 0, "checked_runs": 0, "sample_s": [], "first_error": None})
    kind = port.VARIANTS[variant]["kind"]
    before_import = memory_snapshot()
    started = time.perf_counter()
    port.load()
    record["import_s"] = time.perf_counter() - started
    after_import = memory_snapshot()
    started = time.perf_counter()
    call = port.build(variant, workload)
    record["build_s"] = time.perf_counter() - started
    check = lambda result: port.check(workload, result)  # noqa: E731
    try:
        if kind == "async":
            import asyncio

            started = time.perf_counter()
            asyncio.run(_loop_async(call, check, iters, samples, warmup, record))
        else:
            started = time.perf_counter()
            _loop_sync(call, check, iters, samples, warmup, record)
    except (CellError, AssertionError, Exception) as error:  # noqa: BLE001
        text = str(error) or type(error).__name__
        if not isinstance(error, CellError):
            record["failures"] += 1
            text = f"{type(error).__name__}: {text}"
        return error_record(_with_memory(record, before_import, after_import), text)
    record["status"] = "ok"
    record["median_s"] = median(record["sample_s"])
    record["per_run_us"] = record["median_s"] * 1e6 / iters
    return _with_memory(record, before_import, after_import)


def _with_memory(record: dict, before_import: dict, after_import: dict) -> dict:
    record["process"] = {
        "rss_kb_before_import": before_import["vm_rss_kb"],
        "rss_kb_after_import": after_import["vm_rss_kb"],
        "hwm_kb_end": proc_status_kb("VmHWM"),
        "rss_kb_end": proc_status_kb("VmRSS"),
    }
    return record


def run_inflight(port, variant: str, workload: str, n: int, base: dict) -> dict:
    """Memory with n runs parked in flight on one event loop (async variants only)."""
    import asyncio
    import gc

    record = dict(base)
    record.update({"n": n, "completions": 0, "failures": 0, "first_error": None})
    port.load()
    if port.VARIANTS[variant]["kind"] != "async":
        return error_record(record, "in-flight measurement needs an async variant")

    async def main() -> None:
        gate = Gate()
        make_call = port.make_inflight(variant, workload, gate)
        # Warm-up: one run end to end with the gate open, so first-use allocations are not charged to the parked runs.
        gate.hold = False
        warm = make_call()
        port.check(workload, await warm())
        gate.hold = True
        gate.parked = 0
        gc.collect()
        malloc_trim()
        record["before"] = memory_snapshot()
        tasks = []
        for _ in range(n):
            tasks.append(asyncio.create_task(make_call()()))
        deadline = time.perf_counter() + 120
        while gate.parked < n:
            if time.perf_counter() > deadline:
                raise CellError(f"only {gate.parked} of {n} runs parked in flight")
            for task in tasks:
                if task.done():
                    task.result()  # raises the run's own error
            await asyncio.sleep(0.001)
        record["parked"] = gate.parked
        record["plateau"] = memory_snapshot()
        gate.event.set()
        results = await asyncio.gather(*tasks)
        for result in results:
            port.check(workload, result)
        record["completions"] = len(results)
        record["after_release"] = memory_snapshot()

    try:
        asyncio.run(main())
    except Exception as error:  # noqa: BLE001
        record["failures"] += 1
        return error_record(record, f"{type(error).__name__}: {error}")
    record["status"] = "ok"
    return record


def run_cold(port, variant: str, workload: str, base: dict) -> dict:
    """Import, build and one run; reports CLOCK_MONOTONIC at completion for the parent to subtract its spawn time."""
    record = dict(base)
    record.update({"completions": 0, "failures": 0, "first_error": None})
    t_start = monotonic_ns()
    port.load()
    t_imported = monotonic_ns()
    call = port.build(variant, workload)
    t_built = monotonic_ns()
    try:
        if port.VARIANTS[variant]["kind"] == "async":
            import asyncio

            result = asyncio.run(call())
        else:
            result = call()
        t_done = monotonic_ns()
        port.check(workload, result)
    except Exception as error:  # noqa: BLE001
        record["failures"] += 1
        return error_record(record, f"{type(error).__name__}: {error}")
    record.update({
        "status": "ok",
        "completions": 1,
        "cell_entered_ns": t_start,
        "first_run_done_ns": t_done,
        "import_us": (t_imported - t_start) / 1000,
        "build_us": (t_built - t_imported) / 1000,
        "first_run_us": (t_done - t_built) / 1000,
    })
    return record


def run_idle(port, base: dict) -> dict:
    """Memory of a process that imported the framework and did nothing else."""
    record = dict(base)
    record.update({"completions": 0, "failures": 0, "first_error": None})
    record["at_start"] = memory_snapshot()
    if port is not None:
        port.load()
    record["after_import"] = memory_snapshot()
    record["status"] = "ok"
    return record


def finish(record: dict) -> int:
    """Print the record as the last stdout line; non-zero exit for an error cell, with the first error on stderr."""
    import json

    print("record\t" + json.dumps(record, separators=(",", ":")))
    sys.stdout.flush()
    if record.get("status") != "ok":
        print(f"error: {record.get('first_error')}", file=sys.stderr)
        return 3
    return 0
