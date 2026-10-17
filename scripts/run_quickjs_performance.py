#!/usr/bin/env python3
"""Run the preregistered QuickJS performance gate with stdlib only."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PREREGISTRATION = ROOT / "benchmarks" / "quickjs_performance_preregistration.json"
THRESHOLD_KEYS = {
    "median_max",
    "p95_max",
    "mad_max",
    "peak_allocated_bytes_max",
}
EXPECTED_CASES = {
    "runtime_creation_cold",
    "source_compilation_cold",
    "module_compilation_cold",
    "define_lowering",
    "generator_first_command",
    "generator_warm_command",
    "host_bridge_round_trip",
    "runtime_builder_peak_memory",
    "structured_join_sequence",
    "structured_join_parallel",
    "structured_join_race",
    "structured_join_quorum",
    "structured_join_await",
    "javascript_wrapped_core",
    "javascript_wrapped_core_ratio",
    "enabled_unused_core_ratio",
    "replay_growth",
}


class ConfigurationError(RuntimeError):
    pass


class BenchmarkError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def finite_nonnegative(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0

def validate_thresholds(case_id: str, thresholds: Any) -> dict[str, Any]:
    if not isinstance(thresholds, dict):
        raise ConfigurationError(f"{case_id}: thresholds are required")
    missing = THRESHOLD_KEYS - thresholds.keys()
    extra = thresholds.keys() - THRESHOLD_KEYS
    if missing or extra:
        raise ConfigurationError(
            f"{case_id}: threshold keys must be exactly {sorted(THRESHOLD_KEYS)}; "
            f"missing={sorted(missing)} extra={sorted(extra)}"
        )
    for key in THRESHOLD_KEYS:
        if not finite_nonnegative(thresholds[key]):
            raise ConfigurationError(f"{case_id}: {key} must be finite and nonnegative")
    return thresholds


def load_preregistration(path: Path) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigurationError(f"cannot read preregistration {path}: {error}") from error

    if document.get("schema_version") != 1:
        raise ConfigurationError("preregistration schema_version must be 1")
    methodology = document.get("methodology")
    if not isinstance(methodology, dict):
        raise ConfigurationError("preregistration methodology must be an object")
    minimum = methodology.get("minimum_samples_per_case")
    samples = methodology.get("samples_per_case")
    if not isinstance(minimum, int) or isinstance(minimum, bool) or minimum < 30:
        raise ConfigurationError("minimum_samples_per_case must be an integer >= 30")
    if not isinstance(samples, int) or isinstance(samples, bool) or samples < minimum:
        raise ConfigurationError("samples_per_case must be >= minimum_samples_per_case")
    if methodology.get("p95_method") != "nearest-rank":
        raise ConfigurationError("p95_method must be nearest-rank")
    core_pair_rerun_samples = methodology.get("core_pair_failure_rerun_samples")
    if (
        not isinstance(core_pair_rerun_samples, int)
        or isinstance(core_pair_rerun_samples, bool)
        or core_pair_rerun_samples != 20
    ):
        raise ConfigurationError("core_pair_failure_rerun_samples must be exactly 20")
    tolerance = methodology.get("threshold_tolerance")
    if not isinstance(tolerance, float) or tolerance != 0.1:
        raise ConfigurationError("threshold_tolerance must be exactly 0.1")
    comparison = methodology.get("ab_comparison")
    if not isinstance(comparison, dict):
        raise ConfigurationError("methodology.ab_comparison must be an object")
    ab_samples = comparison.get("samples_per_case")
    if not isinstance(ab_samples, int) or isinstance(ab_samples, bool) or ab_samples < 10:
        raise ConfigurationError("ab_comparison.samples_per_case must be an integer >= 10")
    warning_ratio = comparison.get("warning_ratio")
    failure_ratio = comparison.get("failure_ratio")
    if not (
        isinstance(warning_ratio, float)
        and isinstance(failure_ratio, float)
        and 1.0 < warning_ratio < failure_ratio
    ):
        raise ConfigurationError(
            "ab_comparison needs 1 < warning_ratio < failure_ratio, both floats"
        )

    entries = document.get("cases")
    if not isinstance(entries, list) or not entries:
        raise ConfigurationError("preregistration cases must be a nonempty array")
    cases: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
            raise ConfigurationError("every preregistration case requires a string id")
        case_id = entry["id"]
        if case_id in cases:
            raise ConfigurationError(f"duplicate preregistration case: {case_id}")
        if entry.get("driver") not in {"primitive", "control", "core_pair"}:
            raise ConfigurationError(f"{case_id}: unknown driver")
        if not isinstance(entry.get("blocking"), bool):
            raise ConfigurationError(f"{case_id}: blocking must be boolean")
        if not isinstance(entry.get("iterations"), int) or entry["iterations"] <= 0:
            raise ConfigurationError(f"{case_id}: iterations must be positive")
        if not isinstance(entry.get("unit"), str) or not entry["unit"]:
            raise ConfigurationError(f"{case_id}: unit must be nonempty")
        if case_id == "replay_growth":
            replay_counts = entry.get("replay_command_counts")
            if not isinstance(replay_counts, list) or not replay_counts:
                raise ConfigurationError("replay_growth must declare nonempty replay_command_counts")
            seen_counts: set[int] = set()
            for replay_count in replay_counts:
                if not isinstance(replay_count, dict):
                    raise ConfigurationError("replay_growth entries must be objects")
                count = replay_count.get("count")
                if not isinstance(count, int) or isinstance(count, bool) or count < 0:
                    raise ConfigurationError("replay_growth counts must be nonnegative integers")
                if count in seen_counts:
                    raise ConfigurationError(f"replay_growth count is duplicated: {count}")
                seen_counts.add(count)
                validate_thresholds(f"replay_growth[{count}]", replay_count.get("thresholds"))
            if "thresholds" in entry:
                raise ConfigurationError("replay_growth thresholds belong to each replay count")
        else:
            validate_thresholds(case_id, entry.get("thresholds"))
        cases[case_id] = entry

    missing_cases = EXPECTED_CASES - cases.keys()
    extra_cases = cases.keys() - EXPECTED_CASES
    if missing_cases or extra_cases:
        raise ConfigurationError(
            f"case inventory mismatch: missing={sorted(missing_cases)} extra={sorted(extra_cases)}"
        )
    replay = cases["replay_growth"]
    if replay.get("status") != "active" or not replay["blocking"]:
        raise ConfigurationError("replay_growth must be an active blocking gate")
    return document, cases


def run_process(command: list[str], expected_returncode: int = 0) -> dict[str, Any]:
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=120,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise BenchmarkError(f"cannot run {' '.join(command)}: {error}") from error
    if completed.returncode != expected_returncode:
        raise BenchmarkError(
            f"{' '.join(command)} exited {completed.returncode}; stderr={completed.stderr.strip()!r}"
        )
    output = completed.stdout.strip()
    try:
        parsed = json.loads(output)
    except json.JSONDecodeError as error:
        raise BenchmarkError(f"invalid JSON from {' '.join(command)}: {output!r}") from error
    if not isinstance(parsed, dict):
        raise BenchmarkError(f"benchmark output must be an object: {' '.join(command)}")
    return parsed


def require_binary(path: Path | None, name: str) -> Path:
    if path is None:
        raise ConfigurationError(f"--{name.replace('_', '-')} is required for the selected cases")
    resolved = path.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ConfigurationError(f"{name} is not an executable file: {resolved}")
    return resolved


def validate_sample(sample: dict[str, Any], case: dict[str, Any], required_build_type: str) -> None:
    case_id = case["id"]
    if sample.get("schema_version") != 1 or sample.get("case") != case_id:
        raise BenchmarkError(f"{case_id}: sample identity mismatch: {sample}")
    if sample.get("status") != "ok":
        raise BenchmarkError(f"{case_id}: sample did not report ok: {sample}")
    if sample.get("unit") != case["unit"]:
        raise BenchmarkError(f"{case_id}: expected unit {case['unit']}, got {sample.get('unit')}")
    if sample.get("build_type") != required_build_type:
        raise BenchmarkError(
            f"{case_id}: expected build_type {required_build_type}, got {sample.get('build_type')}"
        )
    if not finite_nonnegative(sample.get("value")):
        raise BenchmarkError(f"{case_id}: value must be finite and nonnegative")
    peak = sample.get("peak_allocated_bytes")
    if not isinstance(peak, int) or isinstance(peak, bool) or peak <= 0:
        raise BenchmarkError(f"{case_id}: peak_allocated_bytes must be a positive integer")


def single_sample(
    case: dict[str, Any],
    primitive_binary: Path | None,
    control_binary: Path | None,
    required_build_type: str,
) -> dict[str, Any]:
    driver = case["driver"]
    if driver == "primitive":
        binary = require_binary(primitive_binary, "primitive_binary")
        sample = run_process([str(binary), "--case", case["id"]])
    elif driver == "control":
        binary = require_binary(control_binary, "control_binary")
        sample = run_process(
            [
                str(binary),
                "--case",
                case["id"],
                "--iterations",
                str(case["iterations"]),
            ]
        )
    else:
        raise BenchmarkError(f"{case['id']}: core_pair requires paired_sample")
    validate_sample(sample, case, required_build_type)
    return sample

def replay_sample(
    case: dict[str, Any],
    replay_count: int,
    control_binary: Path | None,
    required_build_type: str,
) -> dict[str, Any]:
    binary = require_binary(control_binary, "control_binary")
    sample = run_process(
        [
            str(binary),
            "--case",
            case["id"],
            "--iterations",
            str(case["iterations"]),
            "--replay-count",
            str(replay_count),
        ]
    )
    validate_sample(sample, case, required_build_type)
    components = sample.get("components")
    if (
        not isinstance(components, dict)
        or components.get("replay_command_count") != replay_count
        or components.get("recorded_command_count") != replay_count
    ):
        raise BenchmarkError(f"replay_growth[{replay_count}]: command stream mismatch: {sample}")
    return sample


def core_process_sample(binary: Path, iterations: int, expected_label: str, required_build_type: str) -> dict[str, Any]:
    sample = run_process([str(binary), "--iterations", str(iterations)])
    if sample.get("case") != "enabled_unused_core" or sample.get("status") != "ok":
        raise BenchmarkError(f"Core pair sample identity mismatch: {sample}")
    if sample.get("quickjs_build") != expected_label:
        raise BenchmarkError(
            f"Core pair binary expected {expected_label}, got {sample.get('quickjs_build')}"
        )
    if sample.get("build_type") != required_build_type:
        raise BenchmarkError(
            f"Core pair expected build_type {required_build_type}, got {sample.get('build_type')}"
        )
    if sample.get("unit") != "us" or not finite_nonnegative(sample.get("value")) or sample["value"] <= 0:
        raise BenchmarkError(f"Core pair emitted invalid timing: {sample}")
    peak = sample.get("peak_allocated_bytes")
    if not isinstance(peak, int) or isinstance(peak, bool) or peak <= 0:
        raise BenchmarkError(f"Core pair emitted invalid peak allocation: {sample}")
    return sample


def paired_sample(
    case: dict[str, Any],
    sample_index: int,
    enabled_binary: Path,
    disabled_binary: Path,
    required_build_type: str,
) -> dict[str, Any]:
    order = (
        ((enabled_binary, "enabled_unused"), (disabled_binary, "disabled"))
        if sample_index % 2 == 0
        else ((disabled_binary, "disabled"), (enabled_binary, "enabled_unused"))
    )
    measured: dict[str, dict[str, Any]] = {}
    for binary, label in order:
        measured[label] = core_process_sample(
            binary, case["iterations"], label, required_build_type
        )
    enabled = measured["enabled_unused"]
    disabled = measured["disabled"]
    ratio = enabled["value"] / disabled["value"]
    sample = {
        "schema_version": 1,
        "case": case["id"],
        "status": "ok",
        "value": ratio,
        "unit": "ratio",
        "peak_allocated_bytes": max(
            enabled["peak_allocated_bytes"], disabled["peak_allocated_bytes"]
        ),
        "memory_scope": "maximum_of_paired_process_peak_resident_sets",
        "build_type": required_build_type,
        "components": {
            "enabled_unused_core_us": enabled["value"],
            "disabled_core_us": disabled["value"],
            "first_process": order[0][1],
        },
    }
    validate_sample(sample, case, required_build_type)
    return sample


def nearest_rank_p95(values: list[float]) -> float:
    ordered = sorted(values)
    rank = max(1, math.ceil(0.95 * len(ordered)))
    return ordered[rank - 1]


def summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    values = [float(sample["value"]) for sample in samples]
    median = float(statistics.median(values))
    deviations = [abs(value - median) for value in values]
    return {
        "sample_count": len(samples),
        "unit": samples[0]["unit"],
        "median": median,
        "p95": nearest_rank_p95(values),
        "median_absolute_deviation": float(statistics.median(deviations)),
        "peak_allocated_bytes": max(sample["peak_allocated_bytes"] for sample in samples),
    }


def evaluate_gate(
    summary: dict[str, Any], thresholds: dict[str, Any], tolerance: float
) -> dict[str, Any]:
    comparisons = {
        "median": (summary["median"], thresholds["median_max"]),
        "p95": (summary["p95"], thresholds["p95_max"]),
        "median_absolute_deviation": (
            summary["median_absolute_deviation"],
            thresholds["mad_max"],
        ),
        "peak_allocated_bytes": (
            summary["peak_allocated_bytes"],
            thresholds["peak_allocated_bytes_max"],
        ),
    }
    # A statistic above its ceiling but within the tolerance band passes as a
    # warning; hosted runners alone move some cases a few percent across it.
    failures = []
    warnings = []
    for metric, (observed, maximum) in comparisons.items():
        if observed <= maximum:
            continue
        tolerated = maximum * (1 + tolerance)
        entry = {
            "metric": metric,
            "observed": observed,
            "maximum": maximum,
            "tolerated_maximum": tolerated,
        }
        (failures if observed > tolerated else warnings).append(entry)
    return {
        "status": "fail" if failures else "pass",
        "failures": failures,
        "warnings": warnings,
    }


def git_metadata() -> dict[str, Any]:
    def git(*arguments: str) -> str:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return completed.stdout.strip()

    try:
        return {
            "commit": git("rev-parse", "HEAD"),
            "dirty": bool(git("status", "--porcelain")),
        }
    except (OSError, subprocess.CalledProcessError) as error:
        raise BenchmarkError(f"cannot record git metadata: {error}") from error


def cpu_model() -> str | None:
    """The CPU a report was measured on; its microsecond ceilings assume CI's runners."""
    try:
        text = Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    fields: dict[str, str] = {}
    for line in text.splitlines():
        key, _, value = line.partition(":")
        fields.setdefault(key.strip(), value.strip())
    if fields.get("model name"):
        return fields["model name"]
    if fields.get("CPU implementer") and fields.get("CPU part"):
        return f"CPU implementer {fields['CPU implementer']}, part {fields['CPU part']}"
    return None


def binary_metadata(paths: dict[str, Path | None]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, path in paths.items():
        if path is not None:
            resolved = path.resolve()
            result[name] = {"path": str(resolved), "sha256": sha256_file(resolved)}
    return result


def replay_growth_result(
    case: dict[str, Any],
    control_binary: Path | None,
    sample_count: int,
    minimum_sample_count: int,
    required_build_type: str,
    tolerance: float,
) -> dict[str, Any]:
    replay_counts = []
    failures = []
    warnings = []
    for replay_case in case["replay_command_counts"]:
        replay_count = replay_case["count"]
        samples = [
            replay_sample(case, replay_count, control_binary, required_build_type)
            for _ in range(sample_count)
        ]
        summary = summarize(samples)
        if summary["sample_count"] < minimum_sample_count:
            raise BenchmarkError(
                f"replay_growth[{replay_count}]: fewer than {minimum_sample_count} valid samples"
            )
        gate = evaluate_gate(summary, replay_case["thresholds"], tolerance)
        replay_counts.append(
            {
                "replay_command_count": replay_count,
                "summary": summary,
                "thresholds": replay_case["thresholds"],
                "gate": gate,
                "raw_samples": samples,
            }
        )
        failures.extend(
            {"replay_command_count": replay_count, **failure} for failure in gate["failures"]
        )
        warnings.extend(
            {"replay_command_count": replay_count, **warning} for warning in gate["warnings"]
        )
    return {
        "id": case["id"],
        "blocking": case["blocking"],
        "scope": case["scope"],
        "iterations_per_sample": case["iterations"],
        "replay_command_counts": replay_counts,
        "gate": {
            "status": "fail" if failures else "pass",
            "failures": failures,
            "warnings": warnings,
        },
    }


def ab_measure(
    case: dict[str, Any],
    head: tuple[Path | None, Path | None],
    base: tuple[Path | None, Path | None],
    sample_count: int,
    required_build_type: str,
) -> dict[str, list[float]] | None:
    """Head and base samples of one case, alternating; None if the base build lacks the case."""
    values: dict[str, list[float]] = {"head": [], "base": []}
    for index in range(sample_count):
        for side in ("head", "base") if index % 2 == 0 else ("base", "head"):
            primitive, control = head if side == "head" else base
            try:
                sample = single_sample(case, primitive, control, required_build_type)
            except BenchmarkError as error:
                # An older base build may not have the case yet: its driver
                # rejects it as an "unknown ... benchmark case".
                message = str(error)
                if side == "base" and not values["base"] and "unknown" in message and "benchmark case" in message:
                    return None
                raise
            values[side].append(float(sample["value"]))
    return values


def ab_verdict(
    case: dict[str, Any], values: dict[str, list[float]], settings: dict[str, Any]
) -> dict[str, Any]:
    head_median = float(statistics.median(values["head"]))
    base_median = float(statistics.median(values["base"]))
    if base_median > 0:
        ratio = head_median / base_median
    else:
        ratio = 1.0 if head_median == 0 else math.inf
    if ratio > settings["failure_ratio"] and case["blocking"]:
        status = "fail"
    elif ratio > settings["warning_ratio"]:
        status = "warn"
    else:
        status = "pass"
    return {
        "head_median": head_median,
        "base_median": base_median,
        "ratio": ratio,
        "status": status,
        "head_values": values["head"],
        "base_values": values["base"],
    }


def ab_comparison(
    cases: dict[str, dict[str, Any]],
    selected: list[str],
    head: tuple[Path | None, Path | None],
    base: tuple[Path | None, Path | None],
    base_commit: str | None,
    settings: dict[str, Any],
    required_build_type: str,
) -> dict[str, Any]:
    """Each primitive and control case on the head and the base build, alternating, on one machine.

    Absolute ceilings cannot see a regression smaller than the spread between
    hosted runner types (#476); the ratio of two builds measured side by side
    can. replay_growth and core_pair cases are not compared. A blocking case
    above the failure ratio is compared once more with fresh samples and
    fails only if that comparison fails too, as a core_pair case reruns.
    """
    results = []
    sample_count = settings["samples_per_case"]
    for case_id in selected:
        case = cases[case_id]
        if case["driver"] not in {"primitive", "control"} or case_id == "replay_growth":
            continue
        values = ab_measure(case, head, base, sample_count, required_build_type)
        if values is None:
            results.append(
                {"id": case_id, "status": "skipped", "reason": "the base build does not have this case"}
            )
            continue
        identity = {"id": case_id, "blocking": case["blocking"], "unit": case["unit"]}
        verdict = ab_verdict(case, values, settings)
        if verdict["status"] == "fail":
            rerun = ab_measure(case, head, base, sample_count, required_build_type)
            if rerun is None:
                raise BenchmarkError(f"{case_id}: the base build stopped running the case")
            results.append({**identity, **ab_verdict(case, rerun, settings), "initial_run": verdict})
        else:
            results.append({**identity, **verdict})
    return {
        "base_commit": base_commit,
        "samples_per_case": settings["samples_per_case"],
        "warning_ratio": settings["warning_ratio"],
        "failure_ratio": settings["failure_ratio"],
        "order": "head and base alternate first position by sample index",
        "failure_rerun": "a blocking case above failure_ratio is compared again and fails only if the rerun fails",
        "status": "fail" if any(entry["status"] == "fail" for entry in results) else "pass",
        "cases": results,
    }


def announce_comparison(comparison: dict[str, Any], stream: Any) -> None:
    """Name every case slower than its base build beyond the warning ratio."""
    for entry in comparison["cases"]:
        initial = entry.get("initial_run")
        if initial and entry["status"] != "fail":
            message = (
                f"QuickJS A/B: {entry['id']} was {initial['ratio']:.3f} times its base build, "
                f"above the failure ratio {comparison['failure_ratio']:g}, and "
                f"{entry['ratio']:.3f} times in the rerun"
            )
            print(
                f"::warning::{message}" if os.environ.get("GITHUB_ACTIONS") else f"warning: {message}",
                file=stream,
                flush=True,
            )
            continue
        if entry["status"] not in {"warn", "fail"}:
            continue
        kind = "error" if entry["status"] == "fail" else "warning"
        limit = comparison["failure_ratio"] if kind == "error" else comparison["warning_ratio"]
        message = (
            f"QuickJS A/B: {entry['id']} median {entry['head_median']:.6g} {entry['unit']} "
            f"against {entry['base_median']:.6g} on the base build, {entry['ratio']:.3f} times, "
            f"above the {'failure' if kind == 'error' else 'warning'} ratio {limit:g}"
        )
        if entry["status"] == "warn" and entry["ratio"] > comparison["failure_ratio"]:
            message += "; the case is not blocking"
        if initial:
            message += f" (first comparison {initial['ratio']:.3f} times)"
        print(
            f"::{kind}::{message}" if os.environ.get("GITHUB_ACTIONS") else f"{kind}: {message}",
            file=stream,
            flush=True,
        )


def announce(results: list[dict[str, Any]], tolerance: float, stream: Any) -> None:
    """Name every statistic above its ceiling in the log, not only in the report."""
    for result in results:
        gate = result["gate"]
        for kind, entries in (("warning", gate["warnings"]), ("error", gate["failures"])):
            for entry in entries:
                case = result["id"]
                if "replay_command_count" in entry:
                    case = f"{case}[{entry['replay_command_count']}]"
                band = "within" if kind == "warning" else "beyond"
                message = (
                    f"QuickJS performance: {case} {entry['metric']} {entry['observed']:.6g} "
                    f"is above its ceiling {entry['maximum']:.6g} and {band} the "
                    f"{tolerance:.0%} tolerance ({entry['tolerated_maximum']:.6g})"
                )
                # A non-blocking case cannot fail the job, so it never logs an error.
                if not result["blocking"]:
                    kind = "warning"
                    message += "; the case is not blocking"
                print(
                    f"::{kind}::{message}" if os.environ.get("GITHUB_ACTIONS") else f"{kind}: {message}",
                    file=stream,
                    flush=True,
                )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preregistration", type=Path, default=DEFAULT_PREREGISTRATION)
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--case", action="append", dest="selected_cases")
    parser.add_argument("--primitive-binary", type=Path)
    parser.add_argument("--control-binary", type=Path)
    parser.add_argument("--core-enabled-binary", type=Path)
    parser.add_argument("--core-disabled-binary", type=Path)
    parser.add_argument("--base-primitive-binary", type=Path)
    parser.add_argument("--base-control-binary", type=Path)
    parser.add_argument("--base-commit")
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    preregistration, cases = load_preregistration(arguments.preregistration)
    if arguments.validate_only:
        print(
            json.dumps(
                {
                    "status": "valid",
                    "id": preregistration["id"],
                    "case_count": len(cases),
                    "samples_per_case": preregistration["methodology"]["samples_per_case"],
                },
                sort_keys=True,
            )
        )
        return 0

    selected = arguments.selected_cases or list(cases)
    unknown = set(selected) - cases.keys()
    if unknown:
        raise ConfigurationError(f"unknown selected cases: {sorted(unknown)}")
    if len(selected) != len(set(selected)):
        raise ConfigurationError("selected cases must not contain duplicates")

    required_build_type = preregistration["methodology"]["required_build_type"]
    tolerance = preregistration["methodology"]["threshold_tolerance"]
    sample_count = preregistration["methodology"]["samples_per_case"]
    primitive_binary = arguments.primitive_binary
    control_binary = arguments.control_binary
    enabled_binary = arguments.core_enabled_binary
    disabled_binary = arguments.core_disabled_binary

    results = []
    failed = False
    for case_id in selected:
        case = cases[case_id]
        if case_id == "replay_growth":
            result = replay_growth_result(
                case,
                control_binary,
                sample_count,
                preregistration["methodology"]["minimum_samples_per_case"],
                required_build_type,
                tolerance,
            )
            failed = failed or (case["blocking"] and result["gate"]["status"] == "fail")
            results.append(result)
            continue
        samples = []
        if case["driver"] == "core_pair":
            enabled = require_binary(enabled_binary, "core_enabled_binary")
            disabled = require_binary(disabled_binary, "core_disabled_binary")
            for sample_index in range(sample_count):
                samples.append(
                    paired_sample(case, sample_index, enabled, disabled, required_build_type)
                )
        else:
            for _ in range(sample_count):
                samples.append(
                    single_sample(case, primitive_binary, control_binary, required_build_type)
                )
        summary = summarize(samples)
        if summary["sample_count"] < preregistration["methodology"]["minimum_samples_per_case"]:
            raise BenchmarkError(f"{case_id}: fewer than 30 valid samples")
        initial_gate = evaluate_gate(summary, case["thresholds"], tolerance)
        gate = initial_gate
        effective_summary = summary
        effective_samples = samples
        failure_rerun = None
        if (
            case["driver"] == "core_pair"
            and case["blocking"]
            and initial_gate["status"] == "fail"
        ):
            rerun_samples = []
            rerun_sample_count = preregistration["methodology"][
                "core_pair_failure_rerun_samples"
            ]
            for sample_index in range(rerun_sample_count):
                rerun_samples.append(
                    paired_sample(case, sample_index, enabled, disabled, required_build_type)
                )
            rerun_summary = summarize(rerun_samples)
            gate = evaluate_gate(rerun_summary, case["thresholds"], tolerance)
            effective_summary = rerun_summary
            effective_samples = rerun_samples
            failure_rerun = {
                "sample_count": rerun_sample_count,
                "summary": rerun_summary,
                "gate": gate,
                "raw_samples": rerun_samples,
            }
        failed = failed or (case["blocking"] and gate["status"] == "fail")
        result = {
            "id": case_id,
            "blocking": case["blocking"],
            "scope": case["scope"],
            "iterations_per_sample": case["iterations"],
            "summary": effective_summary,
            "thresholds": case["thresholds"],
            "gate": gate,
            "raw_samples": effective_samples,
        }
        if failure_rerun is not None:
            result["initial_run"] = {
                "summary": summary,
                "gate": initial_gate,
                "raw_samples": samples,
            }
            result["failure_rerun"] = failure_rerun
        results.append(result)

    comparison = None
    base_binaries = (arguments.base_primitive_binary, arguments.base_control_binary)
    if any(base_binaries):
        if not all(base_binaries):
            raise ConfigurationError(
                "--base-primitive-binary and --base-control-binary go together"
            )
        comparison = ab_comparison(
            cases,
            selected,
            (primitive_binary, control_binary),
            base_binaries,
            arguments.base_commit,
            preregistration["methodology"]["ab_comparison"],
            required_build_type,
        )
        failed = failed or comparison["status"] == "fail"

    binaries = {
        "primitive": primitive_binary,
        "control": control_binary,
        "core_enabled": enabled_binary,
        "core_disabled": disabled_binary,
        "base_primitive": arguments.base_primitive_binary,
        "base_control": arguments.base_control_binary,
    }
    used_drivers = {cases[case_id]["driver"] for case_id in selected}
    if "primitive" not in used_drivers:
        binaries["primitive"] = None
    if "control" not in used_drivers:
        binaries["control"] = None
    if "core_pair" not in used_drivers:
        binaries["core_enabled"] = None
        binaries["core_disabled"] = None

    report = {
        "schema_version": 1,
        "preregistration": {
            "id": preregistration["id"],
            "path": str(arguments.preregistration.resolve()),
            "sha256": sha256_file(arguments.preregistration.resolve()),
            "source_baseline": preregistration["source_baseline"],
            "threshold_tolerance": tolerance,
        },
        "source": git_metadata(),
        "machine": {
            "platform": platform.platform(),
            "architecture": platform.machine(),
            "processor": platform.processor(),
            "cpu_model": cpu_model(),
            "logical_cpu_count": os.cpu_count(),
            "runner_environment": os.environ.get("RUNNER_ENVIRONMENT"),
            "python": platform.python_version(),
        },
        "binaries": binary_metadata(binaries),
        "sample_process_isolation": "one subprocess per raw sample",
        "status": "fail" if failed else "pass",
        "cases": results,
    }
    if comparison is not None:
        report["ab_comparison"] = comparison
    # Without --output the report itself is written to stdout.
    stream = sys.stdout if arguments.output else sys.stderr
    announce(results, tolerance, stream)
    if comparison is not None:
        announce_comparison(comparison, stream)
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
