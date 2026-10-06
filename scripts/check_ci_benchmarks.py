#!/usr/bin/env python3
"""Run the four original CI benchmark commands and enforce their gates."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import re
import subprocess
import sys


class BenchmarkError(RuntimeError):
    pass


NUMBER = r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?"
TIMING_ROWS = {"seq", "seq_sync", "par", "seq_stream_idle", "seq_events"}
CONFIG_ROWS = {"seq_iters", "par_iters", "warmup", "samples", "par_workers"}
RUNTIME_ROWS = {
    "os", "arch", "compiler", "build_type", "optimization", "hardware_concurrency"
}


def number(text: str, label: str) -> float:
    if re.fullmatch(NUMBER, text) is None:
        raise BenchmarkError(f"{label}: malformed number {text!r}")
    value = float(text)
    if not math.isfinite(value) or value < 0 or text.startswith("-"):
        raise BenchmarkError(f"{label}: expected a finite nonnegative number, got {text!r}")
    return value


def positive_integer(text: str, label: str) -> int:
    if re.fullmatch(r"[0-9]+", text) is None or int(text) <= 0:
        raise BenchmarkError(f"{label}: expected a positive integer, got {text!r}")
    return int(text)


def parse_async(output: str, *, http: bool) -> dict[str, str]:
    # Match the complete printf record, not a substring that could conceal a
    # second metric, a truncated value, or another benchmark's output.
    threads = (
        r"client_threads=(?P<client_threads>[0-9]+) +server_threads=(?P<server_threads>[0-9]+)"
        if http else r"io_threads=(?P<io_threads>[0-9]+)"
    )
    pattern = (
        r"mode=(?P<mode>[a-z_]+) +concur= *(?P<concur>[0-9]+) "
        r"rounds=(?P<rounds>[0-9]+) +lat_ms=(?P<lat_ms>[0-9]+) +"
        rf"wall= *(?P<wall>{NUMBER})s +ops/s= *(?P<ops>{NUMBER}) +"
        rf"rss_mb= *(?P<rss>{NUMBER}) +" + threads
    )
    match = re.fullmatch(pattern, output.strip())
    if match is None:
        raise BenchmarkError("expected exactly one complete async benchmark record")
    values = match.groupdict()
    expected = (
        {"mode": "async_pool", "concur": "1000", "rounds": "5", "lat_ms": "5"}
        if http else
        {"mode": "async", "concur": "50000", "rounds": "5", "lat_ms": "50"}
    )
    for key, value in expected.items():
        if values[key] != value:
            raise BenchmarkError(f"async {key}: expected {value!r}, got {values[key]!r}")
    for key in ("wall", "ops", "rss"):
        number(values[key], f"async {key}")
    for key in (("client_threads", "server_threads") if http else ("io_threads",)):
        positive_integer(values[key], f"async {key}")
    return values


def parse_neograph(output: str, seq_iters: int, par_iters: int, workers: str) -> dict[str, float]:
    config: dict[str, str] = {}
    runtime: dict[str, str] = {}
    timings: dict[str, float] = {}
    for line in output.splitlines():
        fields = line.split("\t")
        kind = fields[0]
        if kind in ("config", "runtime"):
            if len(fields) != 3 or not fields[2].strip():
                raise BenchmarkError(f"malformed metadata row: {line!r}")
            rows, allowed = (config, CONFIG_ROWS) if kind == "config" else (runtime, RUNTIME_ROWS)
            key, value = fields[1:]
            if key not in allowed or key in rows:
                raise BenchmarkError(f"unknown or duplicate {kind} row: {line!r}")
            rows[key] = value
        elif kind in TIMING_ROWS:
            if len(fields) != 4 or kind in timings:
                raise BenchmarkError(f"malformed or duplicate timing row: {line!r}")
            count = positive_integer(fields[1], f"{kind} count")
            expected_count = par_iters if kind == "par" else seq_iters
            if count != expected_count:
                raise BenchmarkError(f"{kind}: expected {expected_count} iterations, got {count}")
            number(fields[2], f"{kind} total_ms")
            timings[kind] = number(fields[3], f"{kind} per_op_us")
        else:
            raise BenchmarkError(f"unknown benchmark output row: {line!r}")
    for label, rows, required in (
        ("config", config, CONFIG_ROWS),
        ("runtime", runtime, RUNTIME_ROWS),
        ("timing", timings, TIMING_ROWS),
    ):
        missing = required - rows.keys()
        if missing:
            raise BenchmarkError(f"missing {label} rows: {', '.join(sorted(missing))}")
    for key in ("seq_iters", "par_iters", "warmup", "samples"):
        positive_integer(config[key], f"config {key}")
    if int(config["seq_iters"]) != seq_iters or int(config["par_iters"]) != par_iters:
        raise BenchmarkError("iteration configuration does not match the requested workload")
    hardware_threads = positive_integer(runtime["hardware_concurrency"], "hardware_concurrency")
    if workers == "auto":
        # configure_par_workers prints the same max(hardware_concurrency, 1)
        # used by the runtime row; a prefix-only 'auto(' is not evidence.
        expected_workers = f"auto({hardware_threads})"
    else:
        expected_workers = workers
    if config["par_workers"] != expected_workers:
        raise BenchmarkError(
            f"par_workers: expected {expected_workers!r}, got {config['par_workers']!r}"
        )
    return timings


def run_benchmark(
    binary: Path, arguments: list[str], extra_environment: dict[str, str] | None = None
) -> str:
    command = [str(binary), *arguments]
    print(f"Running: {command!r}", flush=True)
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    environment.update(extra_environment or {})
    completed = subprocess.run(command, capture_output=True, text=True, env=environment)
    sys.stdout.write(completed.stdout)
    sys.stdout.flush()
    sys.stderr.write(completed.stderr)
    sys.stderr.flush()
    if completed.returncode:
        # Preserve ordinary child exit statuses and report signal termination
        # with the conventional 128 + signal shell exit status.
        raise SystemExit(
            completed.returncode if completed.returncode > 0 else 128 - completed.returncode
        )
    if completed.stderr.strip():
        # HTTP workers can print call failures but still return zero. Do not
        # certify their requested-operation throughput as successful work.
        raise BenchmarkError(f"{binary.name}: benchmark emitted stderr diagnostics")
    return completed.stdout


def floor(value: float, minimum: float, label: str) -> None:
    if value < minimum:
        raise BenchmarkError(f"{label}: {value:g} < {minimum:g}")
    print(f"ok: {label} {value:g} >= {minimum:g}")


def ceiling(value: float, maximum: float, label: str) -> None:
    if value > maximum:
        raise BenchmarkError(f"{label}: {value:g} > {maximum:g}")
    print(f"ok: {label} {value:g} <= {maximum:g}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="directory containing the three benchmark executables")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    suffix = ".exe" if os.name == "nt" else ""
    binaries = {
        name: build_dir / (name + suffix)
        for name in ("bench_async_http", "bench_async_fanout", "bench_neograph")
    }
    for binary in binaries.values():
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise BenchmarkError(f"missing benchmark executable: {binary}")

    http = parse_async(run_benchmark(binaries["bench_async_http"], [
        "--mode", "async_pool", "--concur", "1000", "--rounds", "5", "--latency-ms", "5"
    ]), http=True)
    floor(number(http["ops"], "HTTP ops/s"), 12000, "HTTP ops/s")

    fanout = parse_async(run_benchmark(binaries["bench_async_fanout"], [
        "--concur", "50000"
    ]), http=False)
    floor(number(fanout["ops"], "fanout ops/s"), 300000, "fanout ops/s")
    ceiling(number(fanout["rss"], "fanout rss_mb"), 120, "fanout RSS MB")

    # worker=1 deliberately measures serial fan-out, so the engine always prints its
    # informational "fan-out is serial" notice; the engine documents this switch for it.
    # Every other stderr diagnostic still fails the gate, including in the auto run below.
    timings = parse_neograph(run_benchmark(binaries["bench_neograph"], [
        "10000", "5000", "1"
    ], {"NEOGRAPH_SUPPRESS_FANOUT_WARNING": "1"}), 10000, 5000, "1")
    ceiling(timings["seq"], 60, "seq us/op")
    ceiling(timings["par"], 300, "par us/op")

    parse_neograph(run_benchmark(binaries["bench_neograph"], [
        "100", "50", "auto"
    ]), 100, 50, "auto")
    print("ok: explicit auto worker configuration; all CI benchmark gates passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BenchmarkError, OSError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
