"""Contract of the benchmark A/B gate runner and its data file (stdlib only)."""
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gates = load("run_ci_ab_gates", "scripts/run_ci_ab_gates.py")
check = load("check_ci_benchmarks", "scripts/check_ci_benchmarks.py")

NEOGRAPH_OUTPUT = (
    "config\tseq_iters\t10000\nconfig\tpar_iters\t5000\nconfig\twarmup\t10\nconfig\tsamples\t5\n"
    "runtime\tos\tLinux\nruntime\tarch\tx86_64\nruntime\tcompiler\t13.3.0\nruntime\tbuild_type\tRelease\n"
    "runtime\toptimization\toptimized\nruntime\thardware_concurrency\t4\n"
    "seq\t10000\t69.5\t6.95\nseq_sync\t10000\t66.0\t6.6\nconfig\tpar_workers\t1\n"
    "par\t5000\t80.1\t16.02\nseq_stream_idle\t10000\t70.2\t7.02\nseq_events\t10000\t75.0\t7.5\n")
PROGRAM_OUTPUT = (
    "header\tworkload\titerations\ttotal_ms\tper_iter_us\n"
    "result\tcore_seq\t200\t3.1\t15.5\nresult\tprogram_single_call_core\t200\t400.0\t2000.0\n"
    "metric\toverhead_ratio\t129.0\n")
FANOUT_OUTPUT = ("mode=async concur= 50000 rounds=5 lat_ms=50  wall= 0.299s  ops/s= 834754.2  "
                 "rss_mb=   67.5  io_threads=4\n")
CHECKPOINT_OUTPUT = (
    "# bench_checkpoint_store threads=4 iters=200 channels=6 payload=256B\n"
    "backend      wall(s)     ops/sec     p50(us)     p95(us)     p99(us)     blobs\n"
    "memory         0.077       10384          25         343        6996       800\n"
    "sqlite         1.828         438        4249       31612       66720       800\n")


class GatesFile(unittest.TestCase):
    def test_the_shipped_file_loads_with_the_required_floors(self):
        document, cases = gates.load_gates()
        self.assertEqual(document["schema_version"], 1)
        self.assertTrue(gates.REQUIRED_FLOORS <= cases.keys())
        for case_id in gates.REQUIRED_FLOORS:
            self.assertIn("floor", cases[case_id])

    def test_the_four_original_limits_keep_their_values(self):
        _, cases = gates.load_gates()
        limits = {case_id: cases[case_id]["floor"]["limit"] for case_id in gates.REQUIRED_FLOORS}
        self.assertEqual(limits, {"http_ops_per_second": 12000, "fanout_ops_per_second": 300000,
                                  "fanout_rss_mb": 120, "core_seq_us": 60, "core_par_us": 300})

    def test_the_workloads_check_ci_benchmarks_runs_are_in_the_file(self):
        document, _ = gates.load_gates()
        workloads = {workload["id"]: workload for workload in document["workloads"]}
        self.assertEqual(workloads["async_http_loopback"]["target"], "bench_async_http")
        self.assertEqual(workloads["async_fanout"]["target"], "bench_async_fanout")
        self.assertEqual(workloads["core_run_workers_1"]["target"], "bench_neograph")
        self.assertEqual(workloads["core_run_workers_1"]["args"][:3], ["10000", "5000", "1"])

    def test_every_case_leaves_room_above_its_measured_noise(self):
        document, cases = gates.load_gates()
        margin = document["methodology"]["minimum_noise_margin"]
        for case_id, case in cases.items():
            room = abs(case["tolerance"] - 1)
            self.assertGreaterEqual(room + 1e-9, margin * case["a_a_noise"], case_id)

    def write(self, directory, document):
        path = Path(directory) / "gates.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def assert_rejected(self, mutate, message):
        document = copy.deepcopy(gates.load_gates()[0])
        mutate(document)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(gates.ConfigurationError) as raised:
                gates.load_gates(self.write(directory, document))
        self.assertIn(message, str(raised.exception))

    def case(self, document, case_id):
        for workload in document["workloads"]:
            for case in workload["cases"]:
                if case["id"] == case_id:
                    return case
        raise KeyError(case_id)

    def test_a_missing_floor_is_a_failure(self):
        del_floor = lambda document: self.case(document, "core_seq_us").pop("floor")
        self.assert_rejected(del_floor, "without their absolute floor")

    def test_a_tolerance_inside_the_noise_is_rejected(self):
        def mutate(document):
            case = self.case(document, "core_seq_us")
            case["a_a_noise"] = case["tolerance"] - 1
        self.assert_rejected(mutate, "times the noise")

    def test_tolerance_must_lie_on_the_unfavourable_side_of_one(self):
        def lower(document):
            self.case(document, "core_seq_us")["tolerance"] = 0.9
        def higher(document):
            self.case(document, "http_ops_per_second")["tolerance"] = 1.2
        self.assert_rejected(lower, "above 1 when lower is better")
        self.assert_rejected(higher, "below 1 when higher is better")

    def test_a_calibration_must_exist(self):
        self.assert_rejected(
            lambda document: self.case(document, "core_seq_us").update(calibrated_on="nowhere"),
            "calibrated_on")

    def test_duplicate_and_unknown_entries_are_rejected(self):
        def duplicate(document):
            document["workloads"][1]["cases"].append(copy.deepcopy(self.case(document, "core_seq_us")))
        self.assert_rejected(duplicate, "duplicate case")
        self.assert_rejected(lambda document: document["workloads"][0].update(surprise=1), "unknown keys")
        self.assert_rejected(
            lambda document: self.case(document, "core_seq_us").update(surprise=1), "unknown keys")

    def test_an_extractor_is_validated(self):
        self.assert_rejected(
            lambda document: self.case(document, "core_seq_us").update(extract={"kind": "regex"}),
            "extract.kind")
        self.assert_rejected(
            lambda document: self.case(document, "core_seq_us").update(
                extract={"kind": "tsv", "row": ["seq"], "column": 0}), "extract.column")


class Extraction(unittest.TestCase):
    def value(self, spec, output):
        return gates.extract_value(spec, output, "case")

    def test_tab_separated_rows_by_leading_fields_and_column(self):
        self.assertEqual(self.value({"kind": "tsv", "row": ["seq"], "column": 3}, NEOGRAPH_OUTPUT), 6.95)
        self.assertEqual(self.value({"kind": "tsv", "row": ["par"], "column": 3}, NEOGRAPH_OUTPUT), 16.02)
        self.assertEqual(
            self.value({"kind": "tsv", "row": ["result", "program_single_call_core"], "column": 4},
                       PROGRAM_OUTPUT), 2000.0)
        self.assertEqual(self.value({"kind": "tsv", "row": ["metric", "overhead_ratio"], "column": 2},
                                    PROGRAM_OUTPUT), 129.0)

    def test_a_tab_row_must_exist_exactly_once(self):
        with self.assertRaises(gates.SampleError):
            self.value({"kind": "tsv", "row": ["seq_missing"], "column": 3}, NEOGRAPH_OUTPUT)
        with self.assertRaises(gates.SampleError):
            self.value({"kind": "tsv", "row": ["seq"], "column": 3}, NEOGRAPH_OUTPUT + "seq\t1\t2\t3\n")
        with self.assertRaises(gates.SampleError):
            self.value({"kind": "tsv", "row": ["seq"], "column": 7}, NEOGRAPH_OUTPUT)

    def test_key_value_pairs_with_padding_and_units(self):
        self.assertEqual(self.value({"kind": "keyvalue", "key": "ops/s"}, FANOUT_OUTPUT), 834754.2)
        self.assertEqual(self.value({"kind": "keyvalue", "key": "rss_mb"}, FANOUT_OUTPUT), 67.5)
        self.assertEqual(self.value({"kind": "keyvalue", "key": "wall"}, FANOUT_OUTPUT), 0.299)
        with self.assertRaises(gates.SampleError):
            self.value({"kind": "keyvalue", "key": "lat"}, FANOUT_OUTPUT)

    def test_a_negative_value_is_not_a_sample(self):
        with self.assertRaises(gates.SampleError):
            self.value({"kind": "keyvalue", "key": "rss_mb"}, "rss_mb=-1.0\n")

    def test_table_cells_by_header_and_row(self):
        spec = {"kind": "table", "header": "backend", "row": "sqlite", "column": "ops/sec"}
        self.assertEqual(self.value(spec, CHECKPOINT_OUTPUT), 438.0)
        self.assertEqual(self.value({**spec, "row": "memory", "column": "p50(us)"}, CHECKPOINT_OUTPUT), 25.0)
        with self.assertRaises(gates.SampleError):
            self.value({**spec, "row": "postgres"}, CHECKPOINT_OUTPUT)
        with self.assertRaises(gates.SampleError):
            self.value({**spec, "column": "missing"}, CHECKPOINT_OUTPUT)

    def test_json_samples_reduce_to_their_median(self):
        output = json.dumps({"case": "ascii", "samples_us": [5.0, 1.0, 3.0, 400.0, 2.0], "status": "ok"})
        spec = {"kind": "json", "path": ["samples_us"], "reduce": "median"}
        self.assertEqual(self.value(spec, output), 3.0)
        self.assertEqual(self.value({**spec, "reduce": "min"}, output), 1.0)
        with self.assertRaises(gates.SampleError):
            self.value({**spec, "path": ["absent"]}, output)
        with self.assertRaises(gates.SampleError):
            self.value(spec, "not json")


def case(case_id="speed", direction="lower_is_better", tolerance=1.2, floor=None):
    result = {"id": case_id, "extract": {"kind": "tsv", "row": ["speed"], "column": 1}, "unit": "us",
              "direction": direction, "tolerance": tolerance, "a_a_noise": 0.01}
    if floor is not None:
        result["floor"] = {"limit": floor}
    return result


def workload(*cases, samples=5):
    return {"id": "synthetic", "target": "bench_synthetic", "args": ["--fast"], "samples": samples,
            "timeout_seconds": 10, "cases": list(cases or [case()])}


def scripted(head, base, calls=None):
    """A runner whose head and base processes print the next value of each series."""
    series = {"head": list(head), "base": list(base)}

    def run(side, binary, definition):
        if calls is not None:
            calls.append(side)
        item = series[side].pop(0)
        if isinstance(item, Exception):
            raise item
        return f"speed\t{item}\n"
    return run


class Decisions(unittest.TestCase):
    def decide(self, head, base, **options):
        return gates.decide(case(**options), {"head": head, "base": base})

    def test_lower_is_better_fails_above_the_tolerance_only(self):
        self.assertEqual(self.decide([120], [100])["status"], "pass")
        self.assertEqual(self.decide([121], [100])["status"], "fail")
        self.assertEqual(self.decide([50], [100])["status"], "pass")

    def test_higher_is_better_fails_below_the_tolerance_only(self):
        self.assertEqual(self.decide([85], [100], direction="higher_is_better", tolerance=0.85)["status"], "pass")
        self.assertEqual(self.decide([84], [100], direction="higher_is_better", tolerance=0.85)["status"], "fail")
        self.assertEqual(self.decide([400], [100], direction="higher_is_better", tolerance=0.85)["status"], "pass")

    def test_the_median_ignores_one_outlier(self):
        result = self.decide([100, 100, 5000, 100, 101], [100, 99, 100, 101, 100])
        self.assertEqual(result["status"], "pass")
        self.assertAlmostEqual(result["ratio"], 1.0)

    def test_a_zero_base_is_a_regression_only_when_the_head_is_not_zero(self):
        self.assertEqual(self.decide([0], [0])["status"], "pass")
        self.assertEqual(self.decide([1], [0])["status"], "fail")

    def test_the_floor_applies_without_a_base(self):
        low = gates.decide(case(floor=50), {"head": [60], "base": []})
        self.assertEqual((low["status"], low["floor"]["status"]), ("fail", "fail"))
        self.assertNotIn("ratio", low)
        self.assertEqual(gates.decide(case(floor=50), {"head": [40], "base": []})["status"], "pass")
        high = gates.decide(case(direction="higher_is_better", tolerance=0.8, floor=50),
                            {"head": [40], "base": [41]})
        self.assertEqual((high["status"], high["floor"]["status"]), ("fail", "fail"))

    def test_the_floor_holds_even_when_the_ratio_is_fine(self):
        result = gates.decide(case(floor=50), {"head": [60], "base": [58]})
        self.assertEqual(result["status"], "fail")
        self.assertAlmostEqual(result["ratio"], 60 / 58)


class Measuring(unittest.TestCase):
    def measure(self, runner, samples=4, base=Path("base")):
        definition = workload(samples=samples)
        return gates.measure_workload(definition, definition["cases"], Path("head"), base, samples, runner)

    def test_head_and_base_alternate_first_position(self):
        calls = []
        measured = self.measure(scripted([1, 2, 3, 4], [10, 20, 30, 40], calls))
        self.assertEqual(calls, ["head", "base", "base", "head", "head", "base", "base", "head"])
        self.assertEqual(measured["values"]["speed"], {"head": [1, 2, 3, 4], "base": [10, 20, 30, 40]})
        self.assertIsNone(measured["no_base"])

    def test_a_base_that_cannot_run_the_first_sample_has_no_base(self):
        calls = []
        runner = scripted([1, 2, 3], [gates.SampleError("unknown flag")] * 2, calls)
        measured = self.measure(runner, samples=3)
        self.assertIn("unknown flag", measured["no_base"])
        self.assertEqual(measured["values"]["speed"], {"head": [1, 2, 3], "base": []})
        self.assertEqual(calls.count("base"), 2)

    def test_a_failed_sample_is_tried_once_more(self):
        measured = self.measure(scripted([1, gates.SampleError("busy"), 2, 3, 4], [10, 20, 30, 40]))
        self.assertEqual(measured["values"]["speed"]["head"], [1, 2, 3, 4])

    def test_a_second_failure_is_an_error_not_a_regression(self):
        runner = scripted([1, gates.SampleError("busy"), gates.SampleError("busy")], [10, 20, 30])
        with self.assertRaises(gates.BenchmarkError):
            self.measure(runner, samples=3)

    def test_a_failing_head_on_the_first_sample_is_an_error(self):
        runner = scripted([gates.SampleError("broken"), gates.SampleError("broken")], [10, 20, 30])
        with self.assertRaises(gates.BenchmarkError):
            self.measure(runner, samples=3)

    def test_without_a_base_only_the_head_runs(self):
        calls = []
        measured = self.measure(scripted([1, 2, 3], [], calls), samples=3, base=None)
        self.assertEqual(calls, ["head"] * 3)
        self.assertEqual(measured["values"]["speed"]["base"], [])


@unittest.skipIf(os.name == "nt", "the stand-in benchmarks are POSIX scripts")
class Running(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.head = Path(self.directory.name) / "head"
        self.base = Path(self.directory.name) / "base"
        for build in (self.head, self.base):
            build.mkdir()
            self.install(build)

    def install(self, build, target="bench_synthetic"):
        path = build / target
        path.write_text("#!/bin/sh\n", encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)

    def run_workload(self, runner, **options):
        definition = workload(case(floor=options.pop("floor", None)), samples=3)
        return gates.run_workload(definition, definition["cases"], self.head,
                                  options.pop("base", self.base), runner, **options)

    def test_a_regression_that_the_rerun_repeats_fails(self):
        runner = scripted([150] * 3 + [160] * 3, [100] * 3 + [100] * 3)
        result = self.run_workload(runner)
        self.assertEqual(result["status"], "fail")
        entry = result["cases"][0]
        self.assertAlmostEqual(entry["ratio"], 1.6)
        self.assertAlmostEqual(entry["initial_run"]["ratio"], 1.5)

    def test_a_regression_that_the_rerun_does_not_repeat_passes(self):
        runner = scripted([150] * 3 + [101] * 3, [100] * 6)
        result = self.run_workload(runner)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["cases"][0]["initial_run"]["status"], "fail")

    def test_a_passing_workload_is_not_measured_again(self):
        calls = []
        result = self.run_workload(scripted([100] * 3, [100] * 3, calls))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(len(calls), 6)
        self.assertNotIn("initial_run", result["cases"][0])

    def test_only_a_failing_workload_is_rerun(self):
        calls = []
        self.run_workload(scripted([150] * 6, [100] * 6, calls))
        self.assertEqual(len(calls), 12)

    def test_without_the_confirmation_the_first_comparison_decides(self):
        calls = []
        result = self.run_workload(scripted([150] * 3, [100] * 3, calls), confirm=False)
        self.assertEqual((result["status"], len(calls)), ("fail", 6))

    def test_a_target_the_base_lacks_has_no_base_and_does_not_fail(self):
        (self.base / "bench_synthetic").unlink()
        calls = []
        result = self.run_workload(scripted([5000] * 3, [], calls))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["comparison"], "no_base")
        self.assertIn("no bench_synthetic", result["no_base"])
        self.assertEqual(calls, ["head"] * 3)

    def test_a_base_that_rejects_the_workload_has_no_base(self):
        result = self.run_workload(scripted([100] * 3, [gates.SampleError("unknown option --fast")] * 2))
        self.assertEqual((result["status"], result["comparison"]), ("pass", "no_base"))

    def test_the_floor_still_fails_a_case_with_no_base(self):
        (self.base / "bench_synthetic").unlink()
        result = self.run_workload(scripted([90] * 6, []), floor=60)
        self.assertEqual(result["status"], "fail")

    def test_a_missing_head_binary_is_a_configuration_error(self):
        (self.head / "bench_synthetic").unlink()
        with self.assertRaises(gates.ConfigurationError):
            self.run_workload(scripted([], []))

    def test_calibration_reports_each_ratio(self):
        definition = workload(samples=3)
        report = gates.calibrate_workload(definition, definition["cases"], self.head, self.base,
                                          scripted([100] * 3 + [110] * 3, [100] * 6), rounds=2)
        entry = report["cases"][0]
        self.assertEqual(entry["ratios"], [1.0, 1.1])
        self.assertAlmostEqual(entry["noise"], 0.1)


@unittest.skipIf(os.name == "nt", "the stand-in benchmarks are POSIX scripts")
class Process(unittest.TestCase):
    """The runner and the check script against small stand-in benchmarks."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def build(self, name, value, *, banner=None):
        build = self.root / name
        build.mkdir()
        path = build / "bench_synthetic"
        stand_in = f"import sys\nprint('speed\\t{value}')\n"
        if banner:
            stand_in += f"print({banner!r}, file=sys.stderr)\n"
        path.write_text(f"#!{sys.executable}\n{stand_in}", encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return build

    def write_gates(self):
        document = copy.deepcopy(gates.load_gates()[0])
        document["workloads"] = [{
            "id": "synthetic", "target": "bench_synthetic", "args": [], "samples": 3, "timeout_seconds": 20,
            "cases": [dict(case(case_id), floor={"limit": 1000})
                      for case_id in ("http_ops_per_second", "fanout_ops_per_second", "fanout_rss_mb",
                                      "core_seq_us", "core_par_us")]}]
        for entry in document["workloads"][0]["cases"]:
            entry["direction"] = "lower_is_better"
            entry["tolerance"] = 1.2
            entry["calibrated_on"] = next(iter(document["calibrations"]))
        path = self.root / "gates.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def run_script(self, *arguments):
        return subprocess.run([sys.executable, str(ROOT / "scripts/run_ci_ab_gates.py"), *arguments],
                              capture_output=True, text=True)

    def test_validate_only_accepts_the_shipped_file(self):
        completed = self.run_script("--validate-only")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["status"], "valid")

    def test_a_comparison_writes_a_report_and_exits_zero(self):
        head, base = self.build("head", 100), self.build("base", 100)
        output = self.root / "report.json"
        completed = self.run_script("--gates", str(self.write_gates()), "--head-dir", str(head),
                                    "--base-dir", str(base), "--base-commit", "abc", "--output", str(output))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(output.read_text())
        self.assertEqual((report["status"], report["mode"], report["base_commit"]), ("pass", "ab", "abc"))
        self.assertEqual(len(report["workloads"][0]["cases"]), 5)
        self.assertIn("head/bench_synthetic", report["binaries"])

    def test_a_regression_exits_one(self):
        head, base = self.build("head", 200), self.build("base", 100)
        completed = self.run_script("--gates", str(self.write_gates()), "--head-dir", str(head),
                                    "--base-dir", str(base), "--output", str(self.root / "report.json"))
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertIn("A/B: ", completed.stdout + completed.stderr)

    def test_the_head_directory_as_base_is_an_a_a_run(self):
        head = self.build("head", 100)
        output = self.root / "report.json"
        completed = self.run_script("--gates", str(self.write_gates()), "--head-dir", str(head),
                                    "--base-dir", str(head), "--output", str(output))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(output.read_text())["mode"], "aa")

    def test_a_benchmark_that_writes_diagnostics_is_an_error(self):
        head = self.build("head", 100, banner="call failed")
        completed = self.run_script("--gates", str(self.write_gates()), "--head-dir", str(head),
                                    "--output", str(self.root / "report.json"))
        self.assertEqual(completed.returncode, 2)
        self.assertIn("diagnostics", completed.stderr)

    def test_calibration_reports_ratios_and_never_fails(self):
        head = self.build("head", 100)
        output = self.root / "report.json"
        completed = self.run_script("--gates", str(self.write_gates()), "--head-dir", str(head),
                                    "--base-dir", str(head), "--calibrate", "2", "--output", str(output))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report["status"], "calibration")
        self.assertIn("recorded a_a_noise", completed.stdout)
        self.assertEqual(len(report["workloads"][0]["cases"][0]["ratios"]), 2)


class CheckScript(unittest.TestCase):
    def test_async_records_are_matched_against_the_workload_arguments(self):
        arguments = ["--mode", "async", "--concur", "50000", "--rounds", "5", "--latency-ms", "50"]
        record = check.parse_async(FANOUT_OUTPUT, arguments, http=False)
        self.assertEqual(record["ops"], "834754.2")
        with self.assertRaises(check.BenchmarkError):
            check.parse_async(FANOUT_OUTPUT, arguments[:3] + ["1000"] + arguments[4:], http=False)

    def test_the_workload_arguments_must_name_what_the_record_is_checked_against(self):
        with self.assertRaises(check.BenchmarkError):
            check.parse_async(FANOUT_OUTPUT, ["--concur", "50000"], http=False)

    def test_core_timings_are_read_for_the_requested_workload(self):
        timings = check.parse_neograph(NEOGRAPH_OUTPUT, 10000, 5000, "1")
        self.assertEqual((timings["seq"], timings["par"]), (6.95, 16.02))

    def test_limits_follow_the_direction_of_the_case(self):
        _, cases = gates.load_gates()
        with contextlib.redirect_stdout(io.StringIO()):
            check.limit_of(cases, "core_seq_us", 59.0, "seq")
            check.limit_of(cases, "http_ops_per_second", 12001.0, "http")
            with self.assertRaises(check.BenchmarkError):
                check.limit_of(cases, "core_seq_us", 61.0, "seq")
            with self.assertRaises(check.BenchmarkError):
                check.limit_of(cases, "http_ops_per_second", 11999.0, "http")


if __name__ == "__main__":
    unittest.main()
