#!/usr/bin/env python3
"""Run the cross-framework cohort in one session and write one record.

    driver.py run   --work DIR --out RECORD.json [--rounds N] [--only-loop] ...
    driver.py pilot --work DIR                      (one short round, nothing is recorded as a result)

The work directory holds what a session needs: `venvs/<framework>-<source>/` (one venv per framework and per
source, `head` or `release`) and `build-<config>/` (NeoGraph build directories: hardened, unhardened, minsize,
shared). The driver never installs or builds anything; scripts/setup of those is `setup_venvs.sh` and the CMake
commands in benchmarks/README.md.

A session is a set of phases, each in interleaved rounds (every cell once per round, in a shuffled order that is
reproducible from the seed in plan.json):

  loop       timed loops of every variant (release sources: the matched variants only)
  cold       process start to the first completed `seq` run, 20 fresh processes per framework
  idle       memory of the interpreter alone, of the framework imported, and of the NeoGraph binary doing nothing
  inflight   memory with N runs parked in flight (N = 1, 64, 256)
  sizes      stripped binary sizes, install footprints (on-disk size and package count of a fresh venv)

Every cell is one fresh process and prints one record (see common.py). A cell whose record is missing, whose
status is not "ok" or whose exit status is not zero is stored with status "error" and its first error, and the
driver exits non-zero at the end; it never leaves an error cell out of the record.

The record carries its provenance: the commit of the harness tree and the engine commit, the compiler, the
flags the binaries were built with, the CPU, the kernel, every package version and `pip freeze` per venv, UTC
start and end, the load average before and after each phase, and the sha256 of the harness and aggregation
scripts.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECORD_SCHEMA = 1
LOAD_LIMIT = 0.30


# ── small helpers ────────────────────────────────────────────────────────────────────────────────────────────────

def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def loadavg() -> list[float]:
    with open("/proc/loadavg", encoding="ascii") as handle:
        return [float(x) for x in handle.read().split()[:3]]


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def median(values: list[float]) -> float:
    ordered = sorted(values)
    mid = len(ordered) // 2
    return ordered[mid] if len(ordered) % 2 else (ordered[mid - 1] + ordered[mid]) / 2


def wait_for_idle(limit: float, label: str, patience_s: int = 900) -> list[float]:
    """Block until the 1-minute load average is under `limit`; returns the load. Raises when it never calms down."""
    deadline = time.time() + patience_s
    while True:
        load = loadavg()
        if load[0] < limit:
            return load
        if time.time() > deadline:
            raise SystemExit(f"{label}: the load average stayed at {load[0]:.2f} (limit {limit}); not timing")
        time.sleep(15)


def sanitize(text: str, work: Path) -> str:
    """Remove host paths and user names from a string that goes into the record."""
    text = text.replace(str(work), "<work>")
    text = re.sub(r"/home/[^/\s]+", "<home>", text)
    text = re.sub(r"/root\b", "<home>", text)
    return text


# ── running one cell ─────────────────────────────────────────────────────────────────────────────────────────────

def parse_record(stdout: str) -> dict | None:
    record = None
    for line in stdout.splitlines():
        if line.startswith("record\t"):
            try:
                record = json.loads(line.split("\t", 1)[1])
            except json.JSONDecodeError:
                record = None
    return record


def run_process(cmd: list[str], env: dict, timeout: float) -> tuple[int, str, str, float, int]:
    """Run a cell; returns (exit status, stdout, stderr, seconds, CLOCK_MONOTONIC ns taken just before the spawn)."""
    scratch = tempfile.mkdtemp(prefix="neograph-cell-")
    try:
        spawn_ns = time.monotonic_ns()
        started = time.perf_counter()
        try:
            done = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=scratch, timeout=timeout)
            return done.returncode, done.stdout, done.stderr, time.perf_counter() - started, spawn_ns
        except subprocess.TimeoutExpired as expired:
            out = expired.stdout.decode() if isinstance(expired.stdout, bytes) else (expired.stdout or "")
            return 124, out, f"timeout after {timeout}s", time.perf_counter() - started, spawn_ns
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def normalize_python_loop(record: dict, spec: dict) -> dict:
    iters = record.get("iters") or spec.get("iters")
    samples_us = [s * 1e6 / iters for s in record.get("sample_s", [])] if iters else []
    return {
        "iters": iters,
        "samples": len(samples_us),
        "sample_us_per_run": samples_us,
        "per_run_us": median(samples_us) if samples_us and record.get("status") == "ok" else None,
        "completions": record.get("completions", 0),
        "failures": record.get("failures", 0),
        "checked_runs": record.get("checked_runs", 0),
        "versions": record.get("versions"),
        "process": record.get("process"),
        "import_s": record.get("import_s"),
        "build_s": record.get("build_s"),
    }


def normalize_neograph_loop(record: dict, spec: dict) -> dict:
    rows = record.get("rows", [])
    wanted = spec["row"]
    row = next((r for r in rows if r.get("row") == wanted), None)
    if row is None:
        return {"per_run_us": None, "completions": 0, "failures": 1, "first_error": f"no {wanted} row in the record"}
    iters = row["iters"]
    samples_us = [ms * 1000.0 / iters for ms in row.get("sample_ms", [])]
    return {
        "iters": iters,
        "samples": len(samples_us),
        "sample_us_per_run": samples_us,
        "per_run_us": median(samples_us) if samples_us and row.get("status") == "ok" else None,
        "completions": row.get("completions", 0),
        "failures": row.get("failures", 0),
        "checked_runs": row.get("checked_runs", 0),
        "workers": row.get("workers"),
        "first_error": row.get("first_error") or None,
        "process": record.get("process"),
        "build": record.get("build"),
    }


def execute(spec: dict, work: Path) -> dict:
    """Run one cell spec and return its stored result."""
    env = dict(os.environ)
    env.update({"LC_ALL": "C", "PYTHONHASHSEED": "0", "HAYSTACK_TELEMETRY_ENABLED": "False",
                "NEOGRAPH_SUPPRESS_FANOUT_WARNING": "1"})
    env.pop("PYTHONPATH", None)
    code, out, err, seconds, spawn_ns = run_process(spec["cmd"], env, spec.get("timeout", 900))
    record = parse_record(out)
    result = {k: spec[k] for k in ("id", "phase", "framework", "source", "variant", "workload", "roles") if k in spec}
    result["wall_s"] = round(seconds, 3)
    result["exit"] = code
    status, first_error = "ok", None
    if record is None:
        status, first_error = "error", f"no record; exit {code}; stderr: {sanitize(err.strip()[:400], work)}"
    elif code != 0 or record.get("status") not in (None, "ok"):
        status = "error"
        first_error = record.get("first_error") or sanitize(err.strip()[:400], work) or f"exit {code}"
    if record is not None:
        kind = spec["kind"]
        if kind == "py_loop":
            result.update(normalize_python_loop(record, spec))
        elif kind == "ng_loop":
            result.update(normalize_neograph_loop(record, spec))
            first_error = first_error or result.get("first_error")
        elif kind in ("py_cold", "ng_cold"):
            done_ns = record.get("first_run_done_ns")
            result["cold_start_ms"] = (done_ns - spawn_ns) / 1e6 if done_ns else None
            for key in ("import_us", "build_us", "first_run_us", "compile_us"):
                if key in record:
                    result[key] = record[key]
        elif kind in ("py_inflight", "ng_inflight"):
            result["n"] = record.get("n")
            for key in ("before", "plateau", "after_release", "parked", "finished"):
                if key in record:
                    result[key] = record[key]
        elif kind in ("py_idle", "ng_idle"):
            for key in ("at_start", "after_import", "after_compile", "versions"):
                if key in record:
                    result[key] = record[key]
        if "versions" in record:
            result.setdefault("versions", record["versions"])
        if kind in ("py_loop", "ng_loop") and result.get("per_run_us") is None:
            status = "error"
            first_error = first_error or "no timing"
        if spec["kind"] == "ng_loop" and result.get("failures"):
            status = "error"
        if spec["kind"] == "py_loop" and result.get("failures"):
            status = "error"
    result["status"] = status
    result["first_error"] = first_error
    return result


# ── building the cell lists ──────────────────────────────────────────────────────────────────────────────────────

def venv_python(work: Path, framework: str, source: str) -> str:
    return str(work / "venvs" / f"{framework}-{source}" / "bin" / "python")


def py_cell(work: Path, plan: dict, framework: str, source: str, variant: str, workload: str, mode: str,
            roles: list[str], extra: list[str] | None = None, phase: str = "loop") -> dict:
    spec_v = plan["frameworks"][framework]["variants"][variant]
    iters = spec_v["iters"][workload]
    cmd = [venv_python(work, framework, source), str(HERE / "cell.py"), "--framework", framework,
           "--variant", variant, "--workload", workload, "--mode", mode]
    if mode == "loop":
        cmd += ["--iters", str(iters), "--samples", str(plan["loop"]["samples"]), "--warmup", str(plan["loop"]["warmup"])]
    cmd += extra or []
    kind = {"loop": "py_loop", "cold": "py_cold", "inflight": "py_inflight", "idle": "py_idle"}[mode]
    return {"id": f"{framework}/{source}/{variant}/{workload}" + (f"/{mode}" if mode != "loop" else ""),
            "phase": phase, "framework": framework, "source": source, "variant": variant, "workload": workload,
            "roles": roles, "cmd": cmd, "kind": kind, "iters": iters, "timeout": 1200}


def ng_binary(work: Path, config: str, name: str) -> str:
    return str(work / f"build-{config}" / name)


def ng_cell(work: Path, plan: dict, config: str, entry: dict) -> dict:
    variant, workload = entry["variant"], entry["workload"]
    rows = {"seq": "seq", "par": "par"}[workload]
    args = []
    if variant == "workers_1":
        workers = "1"
    elif variant == "workers_auto":
        workers = "auto"
        if workload == "seq":
            args.append("--all-workers")
    elif variant == "workers_1_events":
        workers, rows = "1", "seq_events"
    else:
        raise SystemExit(f"unknown NeoGraph variant {variant}")
    seq_iters = entry["iters"] if workload == "seq" else 1000
    par_iters = entry["iters"] if workload == "par" else 1000
    cmd = [ng_binary(work, config, "bench_neograph"), str(seq_iters), str(par_iters), workers, "10",
           str(entry["samples"]), f"--rows={rows}", "--record"] + args
    return {"id": f"neograph/{config}/{variant}/{workload}", "phase": "loop", "framework": "neograph",
            "source": config, "variant": variant, "workload": workload, "roles": entry["roles"], "cmd": cmd,
            "kind": "ng_loop", "row": rows, "timeout": 1200}


SKIPPED: list[str] = []  # environments or builds the plan asked for that the work directory does not have


def loop_cells(work: Path, plan: dict, sources: list[str], only: set[str] | None) -> list[dict]:
    cells = []
    for config, spec in ((c, None) for c in plan["neograph"]["builds"]):
        for entry in plan["neograph"]["loop_cells"]:
            if config in entry["builds"] and (only is None or "neograph" in only):
                if (work / f"build-{config}" / "bench_neograph").exists():
                    cells.append(ng_cell(work, plan, config, entry))
                elif f"build-{config}" not in SKIPPED:
                    SKIPPED.append(f"build-{config}")
    for framework, fspec in plan["frameworks"].items():
        if only is not None and framework not in only:
            continue
        for source in sources:
            if not (work / "venvs" / f"{framework}-{source}").exists():
                SKIPPED.append(f"venvs/{framework}-{source}")
                continue
            for variant, vspec in fspec["variants"].items():
                if source == "release" and not vspec.get("release"):
                    continue
                for workload in ("seq", "par"):
                    cells.append(py_cell(work, plan, framework, source, variant, workload, "loop", vspec["roles"]))
    return cells


def matched_variant(plan: dict, framework: str) -> str:
    for variant, vspec in plan["frameworks"][framework]["variants"].items():
        if "matched_single" in vspec["roles"]:
            return variant
    raise SystemExit(f"{framework} has no matched_single variant")


def footprint_cells(work: Path, plan: dict, phase: str, only: set[str] | None) -> list[dict]:
    cells = []
    fp = plan["footprint"]
    if phase == "cold":
        if (work / "build-hardened" / "bench_footprint").exists() and (only is None or "neograph" in only):
            cells.append({"id": "neograph/hardened/cold", "phase": phase, "framework": "neograph", "source": "hardened",
                          "variant": "cold", "workload": "seq", "roles": ["cold"], "kind": "ng_cold",
                          "cmd": [ng_binary(work, "hardened", "bench_footprint"), "cold"], "timeout": 120})
        for framework in plan["frameworks"]:
            if only is not None and framework not in only:
                continue
            if (work / "venvs" / f"{framework}-head").exists():
                variant = matched_variant(plan, framework)
                cells.append(py_cell(work, plan, framework, "head", variant, "seq", "cold", ["cold"], phase=phase))
    elif phase == "idle":
        if (work / "build-hardened" / "bench_footprint").exists() and (only is None or "neograph" in only):
            cells.append({"id": "neograph/hardened/idle", "phase": phase, "framework": "neograph", "source": "hardened",
                          "variant": "idle", "workload": None, "roles": ["idle"], "kind": "ng_idle",
                          "cmd": [ng_binary(work, "hardened", "bench_footprint"), "idle"], "timeout": 120})
        cells.append({"id": "python/bare/idle", "phase": phase, "framework": "pass", "source": "interpreter",
                      "variant": "idle", "workload": None, "roles": ["idle"], "kind": "py_idle",
                      "cmd": [sys.executable, str(HERE / "cell.py"), "--framework", "pass", "--mode", "idle"],
                      "timeout": 120})
        for framework in plan["frameworks"]:
            if only is not None and framework not in only:
                continue
            if (work / "venvs" / f"{framework}-head").exists():
                cells.append({"id": f"{framework}/head/idle", "phase": phase, "framework": framework, "source": "head",
                              "variant": "idle", "workload": None, "roles": ["idle"], "kind": "py_idle",
                              "cmd": [venv_python(work, framework, "head"), str(HERE / "cell.py"), "--framework",
                                      framework, "--mode", "idle"], "timeout": 120})
    elif phase == "inflight":
        for n in fp["inflight_ns"]:
            for workload in ("seq", "par"):
                if (work / "build-hardened" / "bench_footprint").exists() and (only is None or "neograph" in only):
                    cells.append({"id": f"neograph/hardened/inflight/{workload}/{n}", "phase": phase,
                                  "framework": "neograph", "source": "hardened", "variant": "inflight",
                                  "workload": workload, "roles": ["inflight"], "kind": "ng_inflight",
                                  "cmd": [ng_binary(work, "hardened", "bench_footprint"), "inflight", str(n), workload],
                                  "timeout": 300})
                for framework in plan["frameworks"]:
                    if only is not None and framework not in only:
                        continue
                    if (work / "venvs" / f"{framework}-head").exists():
                        variant = matched_variant(plan, framework)
                        cell = py_cell(work, plan, framework, "head", variant, workload, "inflight", ["inflight"],
                                       extra=["--n", str(n)], phase=phase)
                        cell["id"] += f"/{n}"
                        cells.append(cell)
    return cells


# ── sizes and installed footprints ───────────────────────────────────────────────────────────────────────────────

def file_size(path: Path) -> int | None:
    return path.stat().st_size if path.exists() else None


def stripped_size(path: Path) -> int | None:
    if not path.exists():
        return None
    with tempfile.TemporaryDirectory(prefix="neograph-strip-") as scratch:
        target = Path(scratch) / "stripped"
        subprocess.run(["strip", "-o", str(target), str(path)], check=True, capture_output=True)
        return target.stat().st_size


def dir_bytes(path: Path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            full = os.path.join(root, name)
            if not os.path.islink(full):
                total += os.path.getsize(full)
    return total


def site_packages(venv: Path) -> Path:
    found = sorted(venv.glob("lib/python3*/site-packages"))
    return found[0]


def ldd_entries(path: Path) -> list[tuple[str, str]]:
    """(library name, resolved path) for every shared library the binary needs."""
    done = subprocess.run(["ldd", str(path)], capture_output=True, text=True)
    entries = []
    for line in done.stdout.splitlines():
        if "=>" in line:
            name, _, rest = line.strip().partition(" => ")
            entries.append((name, rest.split(" ")[0]))
    return sorted(set(entries))


def ldd_names(path: Path) -> list[str]:
    return [name for name, _ in ldd_entries(path)]


def measure_sizes(work: Path, plan: dict) -> dict:
    sizes: dict = {"neograph": {}, "python": {}}
    for config in ("hardened", "unhardened", "minsize", "shared"):
        build = work / f"build-{config}"
        if not build.exists():
            continue
        entry = {}
        for name in ("bench_footprint", "bench_neograph"):
            path = build / name
            if path.exists():
                entry[name] = {"as_built_bytes": file_size(path), "stripped_bytes": stripped_size(path),
                               "shared_libraries": ldd_names(path)}
                # The shared libraries the build itself produced (NeoGraph and the SDK), stripped: what a shared
                # deployment of the minimal app ships besides the system libraries.
                own = [(n, Path(p)) for n, p in ldd_entries(path) if Path(p).exists() and build in Path(p).resolve().parents]
                if own:
                    entry[name]["own_shared_libraries_stripped_bytes"] = {
                        n: stripped_size(p) for n, p in own}
        for name in ("libneograph_core.a", "libneograph_core.so"):
            path = build / name
            if path.exists():
                entry[name] = {"as_built_bytes": file_size(path),
                               "stripped_bytes": stripped_size(path) if name.endswith(".so") else None}
        sizes["neograph"][config] = entry
    baseline = work / "venvs" / "_empty"
    base_bytes = dir_bytes(site_packages(baseline)) if baseline.exists() else None
    for framework in plan["frameworks"]:
        for source in ("head", "release"):
            venv = work / "venvs" / f"{framework}-{source}"
            if not venv.exists():
                continue
            freeze = subprocess.run([str(venv / "bin" / "python"), "-m", "pip", "freeze"], capture_output=True,
                                    text=True).stdout.splitlines()
            total = dir_bytes(site_packages(venv))
            sizes["python"][f"{framework}-{source}"] = {
                "site_packages_bytes": total,
                "empty_venv_site_packages_bytes": base_bytes,
                "installed_bytes": total - base_bytes if base_bytes is not None else None,
                "packages": len([line for line in freeze if line and not line.startswith("pip==")]),
            }
    return sizes


# ── provenance ───────────────────────────────────────────────────────────────────────────────────────────────────

def command_output(cmd: list[str], work: Path | None = None) -> str:
    try:
        text = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return sanitize(text, work) if work else text


def cpu_facts() -> dict:
    model, count = "", os.cpu_count()
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith(("model name", "Model name")):
                    model = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    lscpu = command_output(["lscpu"])
    vendor = ""
    for line in lscpu.splitlines():
        if line.startswith("Model name:") and not model:
            model = line.split(":", 1)[1].strip()
        if line.startswith("Vendor ID:"):
            vendor = line.split(":", 1)[1].strip()
    if model and vendor and vendor.lower() not in model.lower() and vendor in ("ARM", "Apple", "Qualcomm"):
        model = f"{vendor} {model}"
    governor = "not exposed by the host"
    gov_path = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    if gov_path.exists():
        governor = gov_path.read_text().strip()
    return {"model": model or "unknown", "count": count, "governor": governor,
            "architecture": command_output(["uname", "-m"])}


def build_facts(work: Path, config: str) -> dict:
    build = work / f"build-{config}"
    if not build.exists():
        return {}
    facts = {"config": config}
    cache = build / "CMakeCache.txt"
    wanted = ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS", "NEOGRAPH_ENABLE_HARDENING",
              "CMAKE_INTERPROCEDURAL_OPTIMIZATION", "BUILD_SHARED_LIBS")
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            key = line.split(":", 1)[0]
            if key in wanted and "=" in line:
                facts[key.lower()] = sanitize(line.split("=", 1)[1], work)
    facts["compiler_version"] = command_output(["g++", "--version"]).splitlines()[0] if command_output(["g++", "--version"]) else ""
    compile_db = build / "compile_commands.json"
    if compile_db.exists():
        for entry in json.loads(compile_db.read_text()):
            if entry["file"].endswith("benchmarks/bench_neograph.cpp"):
                facts["bench_neograph_compile_command"] = sanitize(
                    re.sub(r"^\S+g\+\+", "g++", entry["command"]), work).replace(str(work), "<work>")
            if entry["file"].endswith("src/core/graph_engine.cpp"):
                facts["engine_compile_command"] = sanitize(re.sub(r"^\S+g\+\+", "g++", entry["command"]), work)
    link = command_output(["ninja", "-C", str(build), "-t", "commands", "bench_neograph"], work)
    if link:
        facts["bench_neograph_link_command"] = link.splitlines()[-1][:1500]
    return facts


def package_facts(work: Path, plan: dict) -> dict:
    facts = {}
    for framework in plan["frameworks"]:
        for source in ("head", "release"):
            venv = work / "venvs" / f"{framework}-{source}"
            if not venv.exists():
                continue
            py = str(venv / "bin" / "python")
            freeze = command_output([py, "-m", "pip", "freeze"], work).splitlines()
            facts[f"{framework}-{source}"] = {
                "python": command_output([py, "-c", "import sys;print(sys.version.split()[0])"]),
                "pip_freeze": freeze,
            }
    return facts


def script_hashes() -> dict:
    root = HERE.parents[1]
    names = sorted(str(p.relative_to(root)) for p in HERE.glob("*.py")) + ["benchmarks/compare/plan.json"]
    names += ["benchmarks/bench_neograph.cpp", "benchmarks/bench_footprint.cpp", "scripts/render_bench_report.py"]
    return {name: sha256_file(root / name) for name in sorted(set(names)) if (root / name).exists()}


# ── probes ───────────────────────────────────────────────────────────────────────────────────────────────────────

PROBES = [
    ("llamaindex_reuse", "llamaindex-head", ["probes.py", "llamaindex_reuse"]),
    ("threads_langgraph", "langgraph-head", ["probes.py", "threads", "langgraph"]),
    ("threads_haystack", "haystack-head", ["probes.py", "threads", "haystack"]),
    ("pydantic_async", "pydantic_graph-head", ["probes.py", "pydantic_async"]),
    ("pydantic_legacy", "pydantic_graph-legacy", ["probe_pydantic_legacy.py", "3000"]),
    ("pydantic_legacy_logfire", "pydantic_graph-legacy-logfire", ["probe_pydantic_legacy.py", "3000"]),
]


def run_probes(work: Path, log) -> dict:
    results = {}
    env = dict(os.environ)
    env.update({"LC_ALL": "C", "HAYSTACK_TELEMETRY_ENABLED": "False"})
    env.pop("PYTHONPATH", None)
    for name, venv, argv in PROBES:
        python = work / "venvs" / venv / "bin" / "python"
        if not python.exists():
            log(f"[probe {name}] skipped: no venv {venv}")
            continue
        code, out, err, seconds, _ = run_process([str(python), str(HERE / argv[0]), *argv[1:]], env, 900)
        record = parse_record(out)
        if code != 0 or record is None:
            results[name] = {"status": "error", "first_error": sanitize((err or out).strip()[:400], work)}
            log(f"[probe {name}] ERROR {results[name]['first_error']}")
        else:
            record["status"] = "ok"
            results[name] = record
            log(f"[probe {name}] ok")
    return results


# ── the session ──────────────────────────────────────────────────────────────────────────────────────────────────

def run_phase(name: str, cells: list[dict], rounds: int, seed: int, work: Path, out_lines: Path,
              results: list, check_load: bool, log) -> dict:
    """Interleaved rounds: every cell once per round, in a shuffled order."""
    summary = {"phase": name, "rounds": rounds, "cells_per_round": len(cells), "loadavg_start": None,
               "loadavg_end": None, "load_per_round": []}
    summary["loadavg_start"] = wait_for_idle(LOAD_LIMIT, name) if check_load else loadavg()
    rng = random.Random(f"{seed}/{name}")
    for rnd in range(1, rounds + 1):
        order = list(cells)
        rng.shuffle(order)
        round_start = loadavg()
        for position, spec in enumerate(order, 1):
            result = execute(spec, work)
            result["round"], result["order"] = rnd, position
            results.append(result)
            with out_lines.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(result) + "\n")
            shown = result.get("per_run_us") or result.get("cold_start_ms")
            log(f"[{name} {rnd}/{rounds} {position}/{len(order)}] {result['id']}: {result['status']}"
                + (f" {shown:.1f}" if isinstance(shown, (int, float)) else "")
                + (f" -- {result['first_error']}" if result["first_error"] else ""))
        summary["load_per_round"].append({"round": rnd, "start": round_start, "end": loadavg()})
    summary["loadavg_end"] = loadavg()
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "pilot"):
        p = sub.add_parser(name)
        p.add_argument("--work", type=Path, required=True)
        p.add_argument("--out", type=Path)
        p.add_argument("--rounds", type=int)
        p.add_argument("--phases", default="loop,cold,idle,inflight,probes,sizes")
        p.add_argument("--sources", default="head,release")
        p.add_argument("--only", help="comma list of frameworks (and neograph) to run")
        p.add_argument("--harness-commit", default="")
        p.add_argument("--harness-dirty", default="unknown")
        p.add_argument("--engine-commit", default="")
        p.add_argument("--engine-source-note", default="")
        p.add_argument("--wasm-json", type=Path, help="WebAssembly sizes from the CI size gate (merged into the record)")
        p.add_argument("--no-load-check", action="store_true")
        p.add_argument("--scale", type=float, default=1.0, help="scale the iteration counts (tests, pilots)")
    args = parser.parse_args()

    work = args.work.resolve()
    plan = json.loads((HERE / "plan.json").read_text())
    if args.scale != 1.0:
        for fspec in plan["frameworks"].values():
            for vspec in fspec["variants"].values():
                vspec["iters"] = {k: max(5, int(v * args.scale)) for k, v in vspec["iters"].items()}
        for entry in plan["neograph"]["loop_cells"]:
            entry["iters"] = max(50, int(entry["iters"] * args.scale))
    rounds = args.rounds or (1 if args.command == "pilot" else plan["rounds"])
    only = set(args.only.split(",")) if args.only else None
    phases = args.phases.split(",")
    sources = args.sources.split(",")
    out = args.out or (work / ("pilot.json" if args.command == "pilot" else "record.json"))
    lines = out.with_suffix(".jsonl")
    if lines.exists():
        lines.unlink()

    def log(message: str) -> None:
        print(f"{utc_now()} {message}", flush=True)

    start_time, start_load = utc_now(), loadavg()
    results: list = []
    phase_summaries: list = []
    check_load = not args.no_load_check
    for phase in phases:
        if phase == "loop":
            cells = loop_cells(work, plan, sources, only)
            phase_summaries.append(run_phase("loop", cells, rounds, plan["seed"], work, lines, results, check_load, log))
        elif phase in ("cold", "idle", "inflight"):
            fp = plan["footprint"]
            n = {"cold": fp["cold_start_runs"], "idle": fp["idle_rounds"], "inflight": fp["inflight_rounds"]}[phase]
            if args.command == "pilot":
                n = 1
            cells = footprint_cells(work, plan, phase, only)
            phase_summaries.append(run_phase(phase, cells, n, plan["seed"], work, lines, results, check_load, log))
    probes = run_probes(work, log) if "probes" in phases else {}
    sizes = measure_sizes(work, plan) if "sizes" in phases else {}
    wasm = json.loads(args.wasm_json.read_text()) if args.wasm_json else None
    end_time, end_load = utc_now(), loadavg()

    record = {
        "schema": RECORD_SCHEMA,
        "kind": "cross_framework_cohort",
        "plan": plan,
        "provenance": {
            "harness_commit": args.harness_commit,
            "harness_tree_dirty": args.harness_dirty,
            "neograph_engine_commit": args.engine_commit,
            "neograph_engine_source_note": args.engine_source_note,
            "script_sha256": script_hashes(),
            "session": {"utc_start": start_time, "utc_end": end_time, "loadavg_start": start_load,
                        "loadavg_end": end_load, "phases": phase_summaries, "skipped": sorted(set(SKIPPED))},
            "host": {
                "cpu": cpu_facts(),
                "kernel": command_output(["uname", "-r"]),
                "python_driver": sys.version.split()[0],
            },
            "builds": {config: build_facts(work, config) for config in ("hardened", "unhardened", "minsize", "shared")
                       if (work / f"build-{config}").exists()},
            "packages": package_facts(work, plan),
            "requirements_release": (HERE.parent / "requirements-compare.txt").read_text(encoding="utf-8"),
            "requirements_head": (HERE.parent / "requirements-compare-head.txt").read_text(encoding="utf-8"),
            "environment_set_for_cells": {"HAYSTACK_TELEMETRY_ENABLED": "False", "NEOGRAPH_SUPPRESS_FANOUT_WARNING": "1",
                                          "LC_ALL": "C", "PYTHONHASHSEED": "0"},
        },
        "cells": results,
        "sizes": sizes,
        "probes": probes,
    }
    if wasm:
        record["wasm"] = wasm
    out.write_text(json.dumps(record, indent=1, sort_keys=False) + "\n")
    errors = [r for r in results if r["status"] != "ok"]
    log(f"wrote {out.name}: {len(results)} cells, {len(errors)} with errors")
    for r in errors[:20]:
        log(f"  ERROR {r['id']} (round {r.get('round')}): {r['first_error']}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
