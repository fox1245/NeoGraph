#!/usr/bin/env python3
"""Measure Program cost scopes serially; preserve raw runs without imposing gates.

Postgres is opt-in. The caller names a disposable local Docker container and
sets NEOGRAPH_COST_POSTGRES_URL to its published localhost endpoint. Each sample
gets a newly created database, which is dropped after the sample. Existing
databases are never used or removed. SQLite files are retained in output-dir.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
from urllib.parse import urlsplit, urlunsplit
import uuid


def stats(values):
    if not values or any(not math.isfinite(x) or x < 0 for x in values):
        raise ValueError("expected nonempty finite nonnegative measurements")
    ordered = sorted(values)
    median = statistics.median(ordered)
    return {"n": len(values), "median": median,
            "p95": ordered[math.ceil(len(ordered) * .95) - 1],
            "mad": statistics.median(abs(x - median) for x in ordered),
            "min": ordered[0], "max": ordered[-1]}


def summarize(records):
    # A process is the independent repetition. Do not pretend that the inner
    # invocations from the same process are independent process samples.
    metrics = {}
    for record in records:
        values = {k: v for k, v in record.items()
                  if (k.endswith("_us") or k.endswith("_bytes"))
                  and isinstance(v, (float, int))}
        for k, v in record.get("cold", {}).items():
            values["cold." + k] = v
        for k, v in record.get("first_run", {}).items():
            values["first_run." + k] = v
        samples = record.get("samples", [])
        if samples:
            for k in samples[0]:
                distribution = stats([sample[k] for sample in samples])
                values["invocation_median." + k] = distribution["median"]
                values["invocation_p95." + k] = distribution["p95"]
                values["invocation_mean." + k] = statistics.mean(sample[k] for sample in samples)
        if "value" in record:
            values["value"] = record["value"]
        for k, v in values.items():
            metrics.setdefault(k, []).append(v)
    return {k: stats(v) for k, v in sorted(metrics.items())}


def run(command, *, env=None, timeout=180):
    result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=timeout)
    if result.returncode:
        # Avoid including provider/database connection strings in diagnostics.
        raise RuntimeError(f"{Path(command[0]).name} exited {result.returncode}")
    return result.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--postgres-container")
    args = parser.parse_args()
    if not 3 <= args.repeats <= 30 or not 10 <= args.iterations <= 1000 or not 1 <= args.warmup <= 100:
        parser.error("repeats: 3..30, iterations: 10..1000, warmup: 1..100")
    build = args.build_dir.resolve()
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "raw").mkdir()
    pg_parts = None
    if args.postgres_container:
        pg_parts = urlsplit(os.environ.get("NEOGRAPH_COST_POSTGRES_URL", ""))
        if pg_parts.scheme not in {"postgres", "postgresql"} or pg_parts.hostname not in {"localhost", "127.0.0.1"}:
            parser.error("Postgres needs a localhost NEOGRAPH_COST_POSTGRES_URL")
        ports = run(["docker", "port", args.postgres_container, "5432/tcp"])
        if not any(line.rsplit(":", 1)[-1] == str(pg_parts.port or 5432) for line in ports.splitlines()):
            parser.error("URL port does not match the specified disposable container")

    executables = {name: build / name for name in
                   ["bench_program_cost", "bench_quickjs_primitives", "bench_quickjs_control"]}
    metadata = {"schema_version": 1, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "platform": platform.platform(), "cpu_count": os.cpu_count(),
                "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                "repeats": args.repeats, "iterations": args.iterations, "warmup": args.warmup,
                "git_commit": run(["git", "-C", str(root), "rev-parse", "HEAD"]).strip(),
                "git_diff_sha256": hashlib.sha256(run(["git", "-C", str(root), "diff", "HEAD"]).encode()).hexdigest(),
                "binary_sha256": {k: digest(v) for k, v in executables.items()},
                "source_sha256": {p: digest(root / p) for p in [
                    "CMakeLists.txt", "benchmarks/bench_program_cost.cpp", "scripts/run_program_costs.py",
                    "benchmarks/bench_quickjs_primitives.cpp", "benchmarks/bench_quickjs_control.cpp"]},
                "method": "serial fresh processes; alternating forward/reverse case order; independent unit = process",
                "p95_method": "nearest rank; process p95 at n=7 is the maximum",
                "scope": "diagnostic baseline, not the preregistered QuickJS gate; no LLM calls; no runtime changes"}
    metadata["cmake_cache_sha256"] = digest(build / "CMakeCache.txt")
    metadata["cmake_options"] = [line for line in (build / "CMakeCache.txt").read_text().splitlines()
                                 if line.startswith(("NEOGRAPH_", "CMAKE_BUILD_TYPE:", "CMAKE_CXX_FLAGS_RELEASE:"))]
    if platform.system() == "Linux":
        metadata["cpu"] = run(["lscpu"])
    write(out / "metadata.json", metadata)

    cases = []
    def add(name, binary, *options):
        cases.append((name, [str(executables[binary]), *map(str, options)]))

    for payload in [0, 4096, 65536]:
        common = ["--payload-bytes", payload, "--iterations", args.iterations, "--warmup", args.warmup]
        add(f"direct-{payload}", "bench_program_cost", "--case", "direct", *common)
        for backend in ["memory", "sqlite"] + (["postgres"] if pg_parts else []):
            for mode in ["cpp", "javascript"]:
                add(f"{backend}-{mode}-{payload}", "bench_program_cost", "--case", "lifecycle",
                    "--backend", backend, "--mode", mode, *common)
        add(f"generator-{payload}", "bench_program_cost", "--case", "generator",
            "--payload-bytes", payload, "--iterations", 1000)
    for commands in [4, 16]:
        for backend in ["memory", "sqlite"] + (["postgres"] if pg_parts else []):
            add(f"{backend}-javascript-commands-{commands}", "bench_program_cost", "--case", "lifecycle",
                "--backend", backend, "--commands", commands,
                "--iterations", args.iterations, "--warmup", args.warmup)
    for count in [1, 32, 128]:
        add(f"resident-{count}", "bench_program_cost", "--case", "resident", "--residents", count)
    for name in ["runtime_creation_cold", "source_compilation_cold", "module_compilation_cold"]:
        add(name, "bench_quickjs_primitives", "--case", name)
    for name in ["define_lowering", "generator_first_command", "generator_warm_command", "host_bridge_round_trip"]:
        add(name, "bench_quickjs_control", "--case", name, "--iterations", 1000)
    for count in [0, 10, 100, 1000]:
        add(f"replay-{count}", "bench_quickjs_control", "--case", "replay_growth", "--replay-count", count)
    write(out / "cases.json", [{"id": name, "command": command} for name, command in cases])

    all_records = {name: [] for name, _ in cases}
    sample_number = 0
    session = uuid.uuid4().hex[:12]
    try:
        for repeat in range(args.repeats):
            for name, original in (cases if repeat % 2 == 0 else list(reversed(cases))):
                command = list(original)
                env = os.environ.copy()
                db = None
                if name.startswith("sqlite-"):
                    command += ["--storage", str(out / f"{name}-{repeat}.sqlite")]
                if name.startswith("postgres-"):
                    db = f"ng_cost_{session}_{sample_number}"
                    run(["docker", "exec", args.postgres_container, "createdb", "-U", "postgres", db])
                    env["NEOGRAPH_COST_POSTGRES_URL"] = urlunsplit(pg_parts._replace(path="/" + db))
                try:
                    started = time.monotonic()
                    completed = subprocess.run(command, text=True, capture_output=True, env=env, timeout=180)
                    # A failed run is retained and ends the suite; never omit it from a success summary.
                    stderr = completed.stderr
                    secret = env.get("NEOGRAPH_COST_POSTGRES_URL")
                    if secret:
                        stderr = stderr.replace(secret, "[redacted]")
                    record_path = out / "raw" / f"{name}-{repeat}.json"
                    if completed.returncode:
                        write(record_path, {"status": "failed", "returncode": completed.returncode, "stderr": stderr})
                        raise RuntimeError(f"{name} failed; see {record_path}")
                    record = json.loads(completed.stdout)
                    if record.get("status") != "ok" or record.get("build_type") != "Release":
                        raise RuntimeError(f"{name}: expected successful Release measurement")
                    record["process_wall_seconds"] = time.monotonic() - started
                    record["repeat"] = repeat
                    record["suite_id"] = name
                    write(record_path, record)
                    all_records[name].append(record)
                    print(f"{repeat + 1}/{args.repeats} {name}: {record['process_wall_seconds']:.2f}s", flush=True)
                finally:
                    if db:
                        run(["docker", "exec", args.postgres_container, "dropdb", "-U", "postgres", db])
                sample_number += 1
        write(out / "summary.json", {name: summarize(records) for name, records in all_records.items()})
        metadata["status"] = "complete"
    except BaseException as error:
        metadata["status"] = "incomplete"
        metadata["failure_type"] = type(error).__name__
        raise
    finally:
        metadata["completed_samples"] = sample_number
        metadata["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        write(out / "metadata.json", metadata)


if __name__ == "__main__":
    main()
