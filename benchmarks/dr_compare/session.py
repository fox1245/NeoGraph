#!/usr/bin/env python3
"""Run the mock-model dr_compare matrix in one session and write one record.

    session.py --python VENV/bin/python --pythonpath DIR_WITH_neograph_engine --out RECORD.json \
               [--rounds 5] [--no-load-check] [--harness-commit SHA] [--engine-commit SHA]

Every cell is a fresh `bench_mock.py` process (mock model: a sleep per model call, canned evidence, in-memory
checkpoints; no key, no network). Both sides get the same pool width: NG_WORKER_COUNT for NeoGraph and
LG_MAX_CONCURRENCY for LangGraph. The configurations run in interleaved rounds, in a shuffled order that
follows from the seed below.

  A  FANOUT=5   LLM_MOCK_MS=0    width 4    the engine floor
  B  FANOUT=5   LLM_MOCK_MS=100  width 4    the usual fan-out
  C  FANOUT=50  LLM_MOCK_MS=100  width 50   the historical wide fan-out, widths matched
  D  FANOUT=50  LLM_MOCK_MS=100  width 4    the same fan-out through a narrow pool on both sides

A cell whose process fails, whose JSON is missing or reports any failed run is stored with status "error", no
timings and its first error; the session still runs the other cells and exits non-zero at the end.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "compare"))
import driver  # noqa: E402  (helpers shared with the cross-framework session)

SEED = 20261022
WARMUP = 3
ITERS = 20
CONFIGS = {
    "A": {"fanout": 5, "mock_ms": 0, "width": 4, "iters": 50,
          "note": "engine floor: no model latency"},
    "B": {"fanout": 5, "mock_ms": 100, "width": 4, "iters": ITERS,
          "note": "five researchers, 100 ms per model call"},
    "C": {"fanout": 50, "mock_ms": 100, "width": 50, "iters": ITERS,
          "note": "fifty researchers, pool as wide as the fan-out on both sides"},
    "D": {"fanout": 50, "mock_ms": 100, "width": 4, "iters": ITERS,
          "note": "fifty researchers through a four-wide pool on both sides"},
}
SIDES = ("neograph", "langgraph")
KNOBS = ("LLM_MOCK_MS", "FANOUT", "MOCK_SEARCH", "USE_INMEMORY_CP", "NG_WORKER_COUNT", "LG_MAX_CONCURRENCY")
SCRIPTS = ("bench_mock.py", "dr_neograph.py", "dr_langgraph.py", "session.py", "_run_single.py")


def cell_env(config: dict, pythonpath: str, extra: dict) -> dict:
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("OPENAI", "ANTHROPIC", "GOOGLE", "NG_", "LG_", "LLM_", "FANOUT", "MOCK_", "CRAWL4AI",
                                "NEOGRAPH_", "LANGGRAPH_", "DR_", "PYTHONPATH", "HAYSTACK"))}
    env.update({
        "LC_ALL": "C", "PYTHONPATH": pythonpath, "LLM_MOCK_MS": str(config["mock_ms"]), "FANOUT": str(config["fanout"]),
        "MOCK_SEARCH": "1", "USE_INMEMORY_CP": "1", "NG_WORKER_COUNT": str(config["width"]),
        "LG_MAX_CONCURRENCY": str(config["width"]),
    })
    env.update(extra)
    return env


def run_cell(spec: dict, python: str, pythonpath: str, bench: Path, extra_env: dict, scale: float) -> dict:
    config = CONFIGS[spec["config"]]
    warmup = max(0, round(WARMUP * scale))
    iters = max(1, round(config["iters"] * scale))
    out = {"id": spec["config"], "round": spec["round"], "order": spec["order"], "config": dict(config),
           "warmup": warmup, "iters": iters}
    with tempfile.TemporaryDirectory(prefix="neograph-dr-") as scratch:
        json_path = Path(scratch) / "bench.json"
        cmd = [python, str(bench), "--warmup", str(warmup), "--iters", str(iters), "--json", str(json_path)]
        started = time.perf_counter()
        try:
            done = subprocess.run(cmd, capture_output=True, text=True, env=cell_env(config, pythonpath, extra_env),
                                  cwd=scratch, timeout=1800)
            code, stderr = done.returncode, done.stderr
        except subprocess.TimeoutExpired:
            code, stderr = 124, "timeout after 1800 s"
        out["wall_s"] = round(time.perf_counter() - started, 3)
        out["exit"] = code
        report = None
        if json_path.exists():
            try:
                report = json.loads(json_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                report = None
    status, first_error = "ok", None
    if report is None:
        status = "error"
        tail = [line for line in stderr.strip().splitlines() if line.strip()]
        first_error = driver.sanitize(tail[-1][:400] if tail else f"no report; exit {code}", pythonpath_path(pythonpath))
    else:
        out["widths"] = report.get("widths")
        out["widths_matched"] = report.get("widths_matched")
        out["env"] = report.get("env")
        out["failures"] = report.get("failures", 0)
        if code != 0 or report.get("failures"):
            status = "error"
            errors = report.get("errors") or []
            first_error = (f"{errors[0]['side']}: {errors[0]['message']}" if errors
                           else f"exit {code}")
        if report.get("widths_matched") is not True:
            status = "error"
            first_error = first_error or "pool widths are not matched"
        if status == "ok":
            out["samples_s"] = report["samples_s"]
            out["median_s"] = {side: statistics.median(v) for side, v in report["samples_s"].items() if v}
            for side in SIDES:
                if len(report["samples_s"].get(side, [])) != iters:
                    status, first_error = "error", f"{side}: {len(report['samples_s'].get(side, []))} of {iters} samples"
    out["status"] = status
    out["first_error"] = first_error
    if status != "ok":
        out.pop("samples_s", None)
        out.pop("median_s", None)
    return out


def pythonpath_path(pythonpath: str) -> Path:
    return Path(pythonpath).resolve()


def summarize(cells: list) -> dict:
    summary = {}
    for name in CONFIGS:
        rows = [c for c in cells if c["id"] == name and c["status"] == "ok"]
        if not rows:
            continue
        entry = {"rounds_ok": len(rows)}
        for side in SIDES:
            values = [c["median_s"][side] for c in rows if side in c["median_s"]]
            if values:
                entry[side] = {"median_s": statistics.median(values), "min_s": min(values), "max_s": max(values)}
        if all(side in entry for side in SIDES):
            entry["ratio_langgraph_over_neograph"] = entry["langgraph"]["median_s"] / entry["neograph"]["median_s"]
        summary[name] = entry
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--python", required=True, help="interpreter of the venv that has langgraph and psutil")
    parser.add_argument("--pythonpath", required=True, help="directory that contains the neograph_engine package")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--configs", default=",".join(CONFIGS), help="comma list of configurations to run")
    parser.add_argument("--scale", type=float, default=1.0, help="scale warmup and iterations (tests)")
    parser.add_argument("--bench", type=Path, default=HERE / "bench_mock.py", help=argparse.SUPPRESS)
    parser.add_argument("--cell-env", action="append", default=[], metavar="KEY=VALUE",
                        help="extra environment for every cell (tests)")
    parser.add_argument("--build-dir", type=Path,
                        help="build directory of the NeoGraph Python module (build-<name>); its CMake cache is recorded")
    parser.add_argument("--harness-commit", default="")
    parser.add_argument("--harness-dirty", default="unknown")
    parser.add_argument("--engine-commit", default="")
    parser.add_argument("--no-load-check", action="store_true")
    args = parser.parse_args()

    names = [n for n in args.configs.split(",") if n]
    unknown = [n for n in names if n not in CONFIGS]
    if unknown:
        parser.error(f"unknown configuration(s): {', '.join(unknown)}")
    extra_env = dict(item.split("=", 1) for item in args.cell_env)
    pp = pythonpath_path(args.pythonpath)

    start_time, start_load = driver.utc_now(), driver.loadavg()
    rng = random.Random(f"{SEED}/dr_compare")
    cells, rounds_info = [], []
    for rnd in range(1, args.rounds + 1):
        load = driver.loadavg() if args.no_load_check else driver.wait_for_idle(driver.LOAD_LIMIT, f"round {rnd}")
        order = list(names)
        rng.shuffle(order)
        for position, name in enumerate(order, 1):
            cell = run_cell({"config": name, "round": rnd, "order": position}, args.python, args.pythonpath, args.bench,
                            extra_env, args.scale)
            cells.append(cell)
            shown = ""
            if cell["status"] == "ok":
                shown = " ".join(f"{s}={cell['median_s'][s] * 1000:.1f}ms" for s in SIDES)
            print(f"{driver.utc_now()} [round {rnd}/{args.rounds} {position}/{len(order)}] {name}: {cell['status']} "
                  f"{shown}{' -- ' + cell['first_error'] if cell['first_error'] else ''}", flush=True)
        rounds_info.append({"round": rnd, "loadavg_start": load, "loadavg_end": driver.loadavg()})
    end_time, end_load = driver.utc_now(), driver.loadavg()

    def run_python(code: str) -> str:
        env = dict(os.environ, PYTHONPATH=args.pythonpath)
        try:
            return subprocess.run([args.python, "-c", code], capture_output=True, text=True, env=env,
                                  timeout=120).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            return ""

    freeze = driver.sanitize(subprocess.run([args.python, "-m", "pip", "freeze"], capture_output=True, text=True)
                             .stdout, pp).splitlines()
    module_version = run_python("import neograph_engine; print(neograph_engine.__version__)")
    extension = run_python("import neograph_engine._neograph as m, os; print(os.path.basename(m.__file__))")
    scripts = {f"benchmarks/dr_compare/{name}": driver.sha256_file(HERE / name) for name in SCRIPTS if (HERE / name).exists()}
    scripts["benchmarks/compare/driver.py"] = driver.sha256_file(HERE.parent / "compare" / "driver.py")
    record = {
        "schema": 1,
        "kind": "dr_compare_session",
        "plan": {"configs": {n: CONFIGS[n] for n in names}, "rounds": args.rounds, "seed": SEED, "warmup": WARMUP,
                 "scale": args.scale, "sides": list(SIDES),
                 "mock": "a sleep of LLM_MOCK_MS per model call, canned search evidence, in-memory checkpoints; "
                         "no key, no network"},
        "provenance": {
            "harness_commit": args.harness_commit,
            "harness_tree_dirty": args.harness_dirty,
            "neograph_engine_commit": args.engine_commit,
            "script_sha256": scripts,
            "session": {"utc_start": start_time, "utc_end": end_time, "loadavg_start": start_load,
                        "loadavg_end": end_load, "rounds": rounds_info},
            "host": {"cpu": driver.cpu_facts(), "kernel": driver.command_output(["uname", "-r"])},
            "python": {"version": run_python("import sys; print(sys.version.split()[0])"), "pip_freeze": freeze},
            "neograph_module": {"version": module_version, "extension_file": extension,
                                "build": (driver.build_facts(args.build_dir.resolve().parent,
                                                              args.build_dir.name.removeprefix("build-"))
                                          if args.build_dir else {})},
            "env_knobs_per_config": {n: {"LLM_MOCK_MS": CONFIGS[n]["mock_ms"], "FANOUT": CONFIGS[n]["fanout"],
                                         "MOCK_SEARCH": 1, "USE_INMEMORY_CP": 1,
                                         "NG_WORKER_COUNT": CONFIGS[n]["width"],
                                         "LG_MAX_CONCURRENCY": CONFIGS[n]["width"]} for n in names},
        },
        "cells": cells,
        "summary": summarize(cells),
    }
    args.out.write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8")
    errors = [c for c in cells if c["status"] != "ok"]
    print(f"{driver.utc_now()} wrote {args.out.name}: {len(cells)} cells, {len(errors)} with errors", flush=True)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
