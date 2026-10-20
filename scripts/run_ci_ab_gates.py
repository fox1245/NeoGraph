#!/usr/bin/env python3
"""Compare benchmark binaries of a base and a head build on one machine (stdlib only).

The cases, workloads, tolerances and the noise they were calibrated against are in
benchmarks/ci_ab_gates.json. Every workload runs on the base and the head build,
alternating which goes first by sample index, and each case compares the medians.
A failing workload is measured once more, and only the rerun decides.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_quickjs_performance import (  # noqa: E402
    BenchmarkError,
    ConfigurationError,
    binary_metadata,
    cpu_model,
    finite_nonnegative,
    git_metadata,
    sha256_file,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GATES = ROOT / "benchmarks" / "ci_ab_gates.json"
NUMBER = r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?"
DIRECTIONS = {"lower_is_better", "higher_is_better"}
EXTRACT_KINDS = {"tsv", "keyvalue", "table", "json"}
REDUCTIONS = {"median", "mean", "min", "max"}
IDENTIFIER = re.compile(r"[a-z][a-z0-9_]*")
TARGET = re.compile(r"[A-Za-z][A-Za-z0-9_]*")
# The absolute limits that stay a last resort beside the A/B tolerances. A file without
# one of them does not load: a missing limit is a gate failure.
REQUIRED_FLOORS = {
    "http_ops_per_second",
    "fanout_ops_per_second",
    "fanout_rss_mb",
    "core_seq_us",
    "core_par_us",
}


class SampleError(BenchmarkError):
    """One benchmark process did not produce a usable sample."""


def positive_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def real(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def validate_extract(case_id: str, spec: Any) -> dict[str, Any]:
    if not isinstance(spec, dict) or spec.get("kind") not in EXTRACT_KINDS:
        raise ConfigurationError(f"{case_id}: extract.kind must be one of {sorted(EXTRACT_KINDS)}")
    kind = spec["kind"]
    allowed = {
        "tsv": {"kind", "row", "column"},
        "keyvalue": {"kind", "key"},
        "table": {"kind", "header", "row", "column"},
        "json": {"kind", "path", "reduce"},
    }[kind]
    if spec.keys() - allowed:
        raise ConfigurationError(f"{case_id}: unknown extract keys {sorted(spec.keys() - allowed)}")

    def text(key: str) -> str:
        if not isinstance(spec.get(key), str) or not spec[key]:
            raise ConfigurationError(f"{case_id}: extract.{key} must be a nonempty string")
        return spec[key]

    if kind == "tsv":
        row = spec.get("row")
        if not isinstance(row, list) or not row or not all(isinstance(item, str) and item for item in row):
            raise ConfigurationError(f"{case_id}: extract.row must be a nonempty array of strings")
        if not isinstance(spec.get("column"), int) or isinstance(spec["column"], bool) or spec["column"] < len(row):
            raise ConfigurationError(f"{case_id}: extract.column must follow the matched fields")
    elif kind == "keyvalue":
        text("key")
    elif kind == "table":
        text("header"), text("row"), text("column")
    else:
        path = spec.get("path")
        if not isinstance(path, list) or not path or not all(isinstance(item, (str, int)) for item in path):
            raise ConfigurationError(f"{case_id}: extract.path must be a nonempty array")
        if spec.get("reduce") not in REDUCTIONS:
            raise ConfigurationError(f"{case_id}: extract.reduce must be one of {sorted(REDUCTIONS)}")
    return spec


def validate_case(case: Any, workload_id: str, methodology: dict[str, Any],
                  calibrations: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(case, dict) or not isinstance(case.get("id"), str) or not IDENTIFIER.fullmatch(case["id"]):
        raise ConfigurationError(f"{workload_id}: every case needs a lower-case identifier id")
    case_id = case["id"]
    allowed = {"id", "extract", "unit", "direction", "tolerance", "floor", "a_a_noise", "calibrated_on", "note"}
    if case.keys() - allowed:
        raise ConfigurationError(f"{case_id}: unknown keys {sorted(case.keys() - allowed)}")
    validate_extract(case_id, case.get("extract"))
    if not isinstance(case.get("unit"), str) or not case["unit"]:
        raise ConfigurationError(f"{case_id}: unit must be nonempty")
    if case.get("direction") not in DIRECTIONS:
        raise ConfigurationError(f"{case_id}: direction must be one of {sorted(DIRECTIONS)}")
    tolerance, noise = case.get("tolerance"), case.get("a_a_noise")
    if not real(tolerance) or tolerance <= 0:
        raise ConfigurationError(f"{case_id}: tolerance must be a positive number")
    if not real(noise) or noise < 0:
        raise ConfigurationError(f"{case_id}: a_a_noise must be a nonnegative number")
    if case.get("calibrated_on") not in calibrations:
        raise ConfigurationError(f"{case_id}: calibrated_on must name an entry of calibrations")
    lower = case["direction"] == "lower_is_better"
    if lower and tolerance <= 1 or not lower and tolerance >= 1:
        raise ConfigurationError(
            f"{case_id}: tolerance must be above 1 when lower is better and below 1 when higher is better"
        )
    room = tolerance - 1 if lower else 1 - tolerance
    margin = methodology["minimum_noise_margin"]
    if room < margin * noise:
        raise ConfigurationError(
            f"{case_id}: tolerance leaves {room:.3f} above the measured noise {noise:.3f}; "
            f"it must leave at least {margin:g} times the noise"
        )
    if "floor" in case:
        floor = case["floor"]
        if not isinstance(floor, dict) or floor.keys() != {"limit"} or not real(floor["limit"]) or floor["limit"] <= 0:
            raise ConfigurationError(f"{case_id}: floor must be an object with a positive limit")
    return case


def load_gates(path: Path = DEFAULT_GATES) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    """The validated file and its cases by id, each with its workload in case["workload"]."""
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigurationError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise ConfigurationError("schema_version must be 1")
    methodology = document.get("methodology")
    if not isinstance(methodology, dict):
        raise ConfigurationError("methodology must be an object")
    if not isinstance(methodology.get("minimum_noise_margin"), (int, float)) or not methodology["minimum_noise_margin"] >= 1:
        raise ConfigurationError("methodology.minimum_noise_margin must be a number >= 1")
    if methodology.get("statistic") != "median":
        raise ConfigurationError("methodology.statistic must be median")
    calibrations = document.get("calibrations")
    if not isinstance(calibrations, dict) or not calibrations:
        raise ConfigurationError("calibrations must be a nonempty object")
    for name, calibration in calibrations.items():
        if not isinstance(calibration, dict) or not isinstance(calibration.get("machine"), str) \
                or not positive_int(calibration.get("runs")) or not isinstance(calibration.get("date"), str):
            raise ConfigurationError(f"calibrations.{name} needs machine, date and runs")
    workloads = document.get("workloads")
    if not isinstance(workloads, list) or not workloads:
        raise ConfigurationError("workloads must be a nonempty array")
    cases: dict[str, dict[str, Any]] = {}
    seen: set[str] = set()
    for workload in workloads:
        if not isinstance(workload, dict) or not isinstance(workload.get("id"), str) \
                or not IDENTIFIER.fullmatch(workload["id"]) or workload["id"] in seen:
            raise ConfigurationError("every workload needs a unique lower-case identifier id")
        workload_id = workload["id"]
        seen.add(workload_id)
        allowed = {"id", "target", "args", "environment", "samples", "timeout_seconds", "cases", "note"}
        if workload.keys() - allowed:
            raise ConfigurationError(f"{workload_id}: unknown keys {sorted(workload.keys() - allowed)}")
        if not isinstance(workload.get("target"), str) or not TARGET.fullmatch(workload["target"]):
            raise ConfigurationError(f"{workload_id}: target must be a benchmark target name")
        if not isinstance(workload.get("args"), list) or not all(isinstance(item, str) for item in workload["args"]):
            raise ConfigurationError(f"{workload_id}: args must be an array of strings")
        environment = workload.get("environment", {})
        if not isinstance(environment, dict) or not all(
                isinstance(key, str) and isinstance(value, str) for key, value in environment.items()):
            raise ConfigurationError(f"{workload_id}: environment must map strings to strings")
        if not positive_int(workload.get("samples")) or workload["samples"] < 3:
            raise ConfigurationError(f"{workload_id}: samples must be an integer >= 3")
        if not positive_int(workload.get("timeout_seconds")):
            raise ConfigurationError(f"{workload_id}: timeout_seconds must be a positive integer")
        if not isinstance(workload.get("cases"), list) or not workload["cases"]:
            raise ConfigurationError(f"{workload_id}: cases must be a nonempty array")
        for case in workload["cases"]:
            case = validate_case(case, workload_id, methodology, calibrations)
            if case["id"] in cases:
                raise ConfigurationError(f"duplicate case: {case['id']}")
            cases[case["id"]] = {**case, "workload": workload}
    missing = REQUIRED_FLOORS - {case_id for case_id, case in cases.items() if "floor" in case}
    if missing:
        raise ConfigurationError(f"cases without their absolute floor: {sorted(missing)}")
    return document, cases


def number(text: str, label: str) -> float:
    if re.fullmatch(NUMBER, text) is None:
        raise SampleError(f"{label}: malformed number {text!r}")
    value = float(text)
    if not finite_nonnegative(value) or text.startswith("-"):
        raise SampleError(f"{label}: expected a finite nonnegative number, got {text!r}")
    return value


def reduce_values(values: list[float], reduction: str) -> float:
    if not values:
        raise SampleError("the output holds no samples")
    return {
        "median": statistics.median,
        "mean": statistics.fmean,
        "min": min,
        "max": max,
    }[reduction](values)


def extract_value(spec: dict[str, Any], output: str, label: str) -> float:
    """One number from a benchmark's standard output, or a SampleError."""
    kind = spec["kind"]
    if kind == "tsv":
        row = spec["row"]
        found = [fields for fields in (line.split("\t") for line in output.splitlines())
                 if fields[: len(row)] == row]
        if len(found) != 1:
            raise SampleError(f"{label}: expected one {row!r} row, found {len(found)}")
        if len(found[0]) <= spec["column"]:
            raise SampleError(f"{label}: the {row!r} row has no column {spec['column']}")
        return number(found[0][spec["column"]], label)
    if kind == "keyvalue":
        found = re.findall(rf"(?<![\w/]){re.escape(spec['key'])}=\s*({NUMBER})", output)
        if len(found) != 1:
            raise SampleError(f"{label}: expected one {spec['key']}= value, found {len(found)}")
        return number(found[0], label)
    if kind == "table":
        lines = [line.split() for line in output.splitlines() if line.strip()]
        headers = [fields for fields in lines if fields[0] == spec["header"]]
        if len(headers) != 1 or spec["column"] not in headers[0]:
            raise SampleError(f"{label}: no {spec['header']!r} header with column {spec['column']!r}")
        rows = [fields for fields in lines if fields[0] == spec["row"] and fields is not headers[0]]
        if len(rows) != 1 or len(rows[0]) != len(headers[0]):
            raise SampleError(f"{label}: expected one {spec['row']!r} row with a value per column")
        return number(rows[0][headers[0].index(spec["column"])], label)
    try:
        node: Any = json.loads(output)
        for step in spec["path"]:
            node = node[step]
    except (json.JSONDecodeError, KeyError, IndexError, TypeError) as error:
        raise SampleError(f"{label}: cannot read {spec['path']!r} from the output: {error}") from error
    values = node if isinstance(node, list) else [node]
    if not all(isinstance(item, (int, float)) and not isinstance(item, bool) and finite_nonnegative(item)
               for item in values):
        raise SampleError(f"{label}: {spec['path']!r} is not a list of finite nonnegative numbers")
    return float(reduce_values([float(item) for item in values], spec["reduce"]))


def binary_path(directory: Path, target: str) -> Path:
    return Path(directory) / (target + (".exe" if os.name == "nt" else ""))


def run_process(binary: Path, workload: dict[str, Any]) -> str:
    """Standard output of one benchmark process, run in a directory of its own."""
    with tempfile.TemporaryDirectory(prefix="neograph-bench-ab-") as scratch:
        environment = dict(os.environ)
        environment["LC_ALL"] = "C"
        # The checkpoint benchmark puts its SQLite database in the temporary directory.
        environment["TMPDIR"] = scratch
        environment.update(workload.get("environment", {}))
        command = [str(binary), *workload["args"]]
        try:
            completed = subprocess.run(command, capture_output=True, text=True, env=environment,
                                       cwd=scratch, timeout=workload["timeout_seconds"], check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise SampleError(f"cannot run {' '.join(command)}: {error}") from error
    if completed.returncode != 0:
        raise SampleError(
            f"{binary.name} exited {completed.returncode}; stderr={completed.stderr.strip()[:300]!r}"
        )
    if completed.stderr.strip():
        # A benchmark can report failed calls on stderr and still exit 0; its throughput is then not valid.
        raise SampleError(f"{binary.name} wrote diagnostics: {completed.stderr.strip()[:300]!r}")
    return completed.stdout


Runner = Callable[[str, Path, dict[str, Any]], str]


def default_runner(side: str, binary: Path, workload: dict[str, Any]) -> str:
    return run_process(binary, workload)


def sample_values(runner: Runner, side: str, binary: Path, workload: dict[str, Any],
                  cases: list[dict[str, Any]]) -> dict[str, float]:
    output = runner(side, binary, workload)
    return {case["id"]: extract_value(case["extract"], output, f"{workload['id']}.{case['id']} ({side})")
            for case in cases}


def measure_workload(workload: dict[str, Any], cases: list[dict[str, Any]], head: Path,
                     base: Path | None, samples: int, runner: Runner) -> dict[str, Any]:
    """Interleaved samples of every case of a workload.

    Head and base alternate first position by sample index. A base build that cannot give its
    first sample, twice, while the head can lacks the workload (an absent flag or output): the
    rest of the samples are the head's alone and "no_base" says why. Any other sample that fails
    twice is an error.
    """
    values: dict[str, dict[str, list[float]]] = {case["id"]: {"head": [], "base": []} for case in cases}
    no_base: str | None = None
    base_started = False
    for index in range(samples):
        for side in ("head", "base") if index % 2 == 0 else ("base", "head"):
            binary = head if side == "head" else base
            if binary is None or (side == "base" and no_base):
                continue
            try:
                row = sample_values(runner, side, binary, workload, cases)
            except SampleError as error:
                try:
                    row = sample_values(runner, side, binary, workload, cases)
                except SampleError as second:
                    if side == "base" and not base_started:
                        no_base = f"the base build cannot run it: {second}"
                        continue
                    raise BenchmarkError(f"{workload['id']} ({side}): {second}") from error
            base_started = base_started or side == "base"
            for case_id, value in row.items():
                values[case_id][side].append(value)
    return {"no_base": no_base, "values": values}


def floor_status(case: dict[str, Any], head_median: float) -> dict[str, Any] | None:
    if "floor" not in case:
        return None
    limit = case["floor"]["limit"]
    ok = head_median <= limit if case["direction"] == "lower_is_better" else head_median >= limit
    return {"limit": limit, "status": "pass" if ok else "fail"}


def ratio_of(head_median: float, base_median: float) -> float:
    if base_median > 0:
        return head_median / base_median
    return 1.0 if head_median == 0 else math.inf


def decide(case: dict[str, Any], values: dict[str, list[float]]) -> dict[str, Any]:
    """The verdict of one case from its head and base samples."""
    head_median = float(statistics.median(values["head"]))
    result: dict[str, Any] = {
        "id": case["id"],
        "unit": case["unit"],
        "direction": case["direction"],
        "tolerance": case["tolerance"],
        "a_a_noise": case["a_a_noise"],
        "head_median": head_median,
        "head_values": values["head"],
    }
    floor = floor_status(case, head_median)
    if floor is not None:
        result["floor"] = floor
    status = "pass"
    if values["base"]:
        base_median = float(statistics.median(values["base"]))
        ratio = ratio_of(head_median, base_median)
        regressed = ratio > case["tolerance"] if case["direction"] == "lower_is_better" else ratio < case["tolerance"]
        result.update({"base_median": base_median, "ratio": ratio, "base_values": values["base"]})
        status = "fail" if regressed else "pass"
    if floor is not None and floor["status"] == "fail":
        status = "fail"
    result["status"] = status
    return result


def executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def run_workload(workload: dict[str, Any], cases: list[dict[str, Any]], head_dir: Path,
                 base_dir: Path | None, runner: Runner, samples: int | None = None,
                 confirm: bool = True) -> dict[str, Any]:
    """Measure one workload and decide its cases; a workload with a failing case is measured once more."""
    head = binary_path(head_dir, workload["target"])
    if not executable(head):
        raise ConfigurationError(f"the head build has no executable {head}")
    base: Path | None = None
    no_base: str | None = "no base build was given" if base_dir is None else None
    if base_dir is not None:
        if executable(binary_path(base_dir, workload["target"])):
            base = binary_path(base_dir, workload["target"])
        else:
            no_base = f"the base build has no {workload['target']}"
    count = samples or workload["samples"]

    def measure(use: Path | None) -> dict[str, Any]:
        return measure_workload(workload, cases, head, use, count, runner)

    measured = measure(base)
    no_base = no_base or measured["no_base"]
    entries = [decide(case, measured["values"][case["id"]]) for case in cases]
    if confirm and any(entry["status"] == "fail" for entry in entries):
        rerun = measure(None if no_base else base)
        if rerun["no_base"]:
            raise BenchmarkError(f"{workload['id']}: the base build stopped running the workload")
        final = [decide(case, rerun["values"][case["id"]]) for case in cases]
        entries = [
            {**again, "initial_run": entry} if entry["status"] == "fail" else entry
            for entry, again in zip(entries, final)
        ]
    result: dict[str, Any] = {
        "id": workload["id"],
        "target": workload["target"],
        "args": workload["args"],
        "samples": count,
        "comparison": "no_base" if no_base else "ab",
        "cases": entries,
        "status": "fail" if any(entry["status"] == "fail" for entry in entries) else "pass",
    }
    if no_base:
        result["no_base"] = no_base
    return result


def calibrate_workload(workload: dict[str, Any], cases: list[dict[str, Any]], head_dir: Path,
                       base_dir: Path, runner: Runner, rounds: int, samples: int | None = None,
                       log: Callable[[str], None] = lambda message: None) -> dict[str, Any]:
    """The head/base ratio of each case in several independent comparisons, without reruns."""
    head = binary_path(head_dir, workload["target"])
    base = binary_path(base_dir, workload["target"])
    for binary in (head, base):
        if not executable(binary):
            raise ConfigurationError(f"no executable {binary}")
    count = samples or workload["samples"]
    ratios: dict[str, list[float]] = {case["id"]: [] for case in cases}
    for index in range(rounds):
        log(f"{workload['id']}: round {index + 1} of {rounds}")
        measured = measure_workload(workload, cases, head, base, count, runner)
        if measured["no_base"]:
            raise BenchmarkError(f"{workload['id']}: {measured['no_base']}")
        for case in cases:
            values = measured["values"][case["id"]]
            ratios[case["id"]].append(ratio_of(statistics.median(values["head"]),
                                               statistics.median(values["base"])))
    entries = []
    for case in cases:
        series = ratios[case["id"]]
        entries.append({
            "id": case["id"],
            "unit": case["unit"],
            "direction": case["direction"],
            "ratios": series,
            "noise": max(abs(item - 1.0) for item in series),
            "tolerance": case["tolerance"],
            "a_a_noise": case["a_a_noise"],
        })
    return {"id": workload["id"], "target": workload["target"], "samples": count, "rounds": rounds,
            "cases": entries}


def display_path(path: Path) -> str:
    """A path relative to the repository when it is inside it."""
    resolved = path.resolve()
    return resolved.relative_to(ROOT).as_posix() if resolved.is_relative_to(ROOT) else str(resolved)


def machine_metadata() -> dict[str, Any]:
    try:
        load = list(os.getloadavg())
    except (AttributeError, OSError):
        load = None
    return {
        "platform": platform.platform(),
        "architecture": platform.machine(),
        "cpu_model": cpu_model(),
        "logical_cpu_count": os.cpu_count(),
        "runner_environment": os.environ.get("RUNNER_ENVIRONMENT"),
        "python": platform.python_version(),
        "load_average_at_start": load,
    }


def annotate(kind: str, message: str, stream: Any) -> None:
    text = f"::{kind}::{message}" if os.environ.get("GITHUB_ACTIONS") else f"{kind}: {message}"
    print(text, file=stream, flush=True)


def describe(entry: dict[str, Any]) -> str:
    if "ratio" in entry:
        return (f"{entry['id']}: head {entry['head_median']:.6g} {entry['unit']}, base "
                f"{entry['base_median']:.6g}, ratio {entry['ratio']:.3f} "
                f"(tolerance {entry['tolerance']:g}, {entry['direction'].replace('_', ' ')})")
    return f"{entry['id']}: head {entry['head_median']:.6g} {entry['unit']}"


def announce(workloads: list[dict[str, Any]], stream: Any) -> None:
    """Name every failing case, every case a rerun cleared, and every workload without a base."""
    for workload in workloads:
        if workload.get("no_base"):
            annotate("notice", f"A/B: {workload['id']} has no base: {workload['no_base']}", stream)
        for entry in workload["cases"]:
            first = entry.get("initial_run")
            if entry["status"] == "fail":
                message = f"A/B: {describe(entry)}"
                if "floor" in entry and entry["floor"]["status"] == "fail":
                    message += f"; the head is beyond its absolute limit {entry['floor']['limit']:g}"
                if first:
                    message += f" (first comparison {first.get('ratio', float('nan')):.3f})"
                annotate("error", message, stream)
            elif first:
                annotate("warning", f"A/B: {describe(first)} in the first comparison and "
                         f"{entry.get('ratio', float('nan')):.3f} in the rerun", stream)


def summary_table(report: dict[str, Any]) -> str:
    lines = ["| case | head | base | ratio | tolerance | status |", "|---|---:|---:|---:|---:|---|"]
    for workload in report["workloads"]:
        for entry in workload["cases"]:
            base = f"{entry['base_median']:.6g}" if "base_median" in entry else "-"
            ratio = f"{entry['ratio']:.3f}" if "ratio" in entry else "-"
            status = entry["status"] if "ratio" in entry or workload["comparison"] == "ab" else \
                f"{entry['status']} (no base)"
            lines.append(f"| `{entry['id']}` ({entry['unit']}) | {entry['head_median']:.6g} | {base} | "
                         f"{ratio} | {entry['tolerance']:g} | {status} |")
    return "\n".join(lines)


def calibration_table(report: dict[str, Any]) -> str:
    lines = ["| case | ratios | noise | recorded a_a_noise | tolerance |", "|---|---|---:|---:|---:|"]
    for workload in report["workloads"]:
        for entry in workload["cases"]:
            ratios = ", ".join(f"{ratio:.3f}" for ratio in entry["ratios"])
            lines.append(f"| `{entry['id']}` | {ratios} | {entry['noise']:.3f} | "
                         f"{entry['a_a_noise']:g} | {entry['tolerance']:g} |")
    return "\n".join(lines)


def write_step_summary(report: dict[str, Any]) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    title = f"### Benchmark A/B gates: {report['status']} ({report['mode']}"
    if report.get("base_commit"):
        title += f", base {report['base_commit'][:12]}"
    table = calibration_table(report) if report["status"] == "calibration" else summary_table(report)
    try:
        with open(path, "a", encoding="utf-8") as summary:
            summary.write(title + ")\n\n" + table + "\n")
    except OSError:
        pass


def binary_paths(chosen: list[tuple[dict[str, Any], list[dict[str, Any]]]], head_dir: Path,
                 base_dir: Path | None) -> dict[str, Path | None]:
    """The benchmark executables a report names, by side and target."""
    paths: dict[str, Path | None] = {}
    for workload, _ in chosen:
        for side, directory in (("head", head_dir), ("base", base_dir if base_dir != head_dir else None)):
            if directory is not None and executable(binary_path(directory, workload["target"])):
                paths[f"{side}/{workload['target']}"] = binary_path(directory, workload["target"])
    return paths


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gates", type=Path, default=DEFAULT_GATES)
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--head-dir", type=Path, help="build directory of the benchmarks under test")
    parser.add_argument("--base-dir", type=Path,
                        help="build directory of the base benchmarks; the head directory itself makes an A/A run")
    parser.add_argument("--base-commit", help="recorded in the report")
    parser.add_argument("--case", action="append", dest="selected_cases", help="run only the workloads of this case")
    parser.add_argument("--samples", type=int, help="override the samples per workload (calibration, tests)")
    parser.add_argument("--calibrate", type=int, metavar="ROUNDS",
                        help="compare ROUNDS times without reruns and report each ratio (needs --base-dir)")
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def selected_workloads(document: dict[str, Any], cases: dict[str, dict[str, Any]],
                       selected: list[str] | None) -> list[tuple[dict[str, Any], list[dict[str, Any]]]]:
    if selected:
        unknown = set(selected) - cases.keys()
        if unknown:
            raise ConfigurationError(f"unknown cases: {sorted(unknown)}")
    chosen = []
    for workload in document["workloads"]:
        members = [cases[case["id"]] for case in workload["cases"]]
        if selected and not any(case["id"] in selected for case in members):
            continue
        chosen.append((workload, members))
    return chosen


def main() -> int:
    arguments = parse_arguments()
    document, cases = load_gates(arguments.gates)
    if arguments.validate_only:
        print(json.dumps({"status": "valid", "id": document["id"], "case_count": len(cases),
                          "workload_count": len(document["workloads"])}, sort_keys=True))
        return 0
    if arguments.head_dir is None:
        raise ConfigurationError("--head-dir is required")
    if arguments.calibrate is not None and (arguments.calibrate < 1 or arguments.base_dir is None):
        raise ConfigurationError("--calibrate needs a positive round count and --base-dir")
    if arguments.samples is not None and arguments.samples < 3:
        raise ConfigurationError("--samples must be at least 3")
    chosen = selected_workloads(document, cases, arguments.selected_cases)
    head_dir = arguments.head_dir.resolve()
    base_dir = arguments.base_dir.resolve() if arguments.base_dir else None
    mode = "head_only" if base_dir is None else "aa" if base_dir == head_dir else "ab"
    report: dict[str, Any] = {
        "schema_version": 1,
        "gates": {"id": document["id"], "path": display_path(arguments.gates),
                  "sha256": sha256_file(arguments.gates.resolve())},
        "mode": mode,
        "source": git_metadata(),
        "base_commit": arguments.base_commit,
        "machine": machine_metadata(),
        "binaries": binary_metadata(binary_paths(chosen, head_dir, base_dir)),
        "sample_process_isolation": "one subprocess per raw sample",
        "order": "head and base alternate first position by sample index",
        "confirmation": "a workload with a failing case is measured once more and the rerun decides",
    }
    stream = sys.stdout if arguments.output else sys.stderr
    if arguments.calibrate is not None:
        report["calibration_rounds"] = arguments.calibrate
        report["workloads"] = [
            calibrate_workload(workload, members, head_dir, base_dir, default_runner,
                               arguments.calibrate, arguments.samples,
                               lambda message: print(f"calibration: {message}", file=stream, flush=True))
            for workload, members in chosen
        ]
        failed = False
        report["status"] = "calibration"
        print(calibration_table(report), file=stream, flush=True)
        write_step_summary(report)
    else:
        report["workloads"] = []
        for workload, members in chosen:
            print(f"A/B: {workload['id']} ({workload['target']})", file=stream, flush=True)
            report["workloads"].append(
                run_workload(workload, members, head_dir, base_dir, default_runner, arguments.samples))
        failed = any(workload["status"] == "fail" for workload in report["workloads"])
        report["status"] = "fail" if failed else "pass"
        announce(report["workloads"], stream)
        print(summary_table(report), file=stream, flush=True)
        write_step_summary(report)
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(serialized, encoding="utf-8")
    else:
        sys.stdout.write(serialized)
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ConfigurationError, BenchmarkError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
