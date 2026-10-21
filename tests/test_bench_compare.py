#!/usr/bin/env python3
"""Contract tests for the cross-framework benchmark tooling (standard library only).

* benchmarks/compare/common.py: the cell-validity rule. A cell with a wrong answer, a failed run or no
  completion is an error cell: no timing, status "error", the first error kept, exit status 3.
* benchmarks/compare/driver.py: a cell whose process exits non-zero, or prints no record, is stored as an
  error and never as a timing.
* scripts/render_bench_report.py: every number of a rendered table comes from the record, the output is
  deterministic, and --check notices a document that no longer matches its record.
"""

import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPARE = ROOT / "benchmarks" / "compare"
sys.path.insert(0, str(COMPARE))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


common = load("common", COMPARE / "common.py")
driver = load("compare_driver", COMPARE / "driver.py")
report = load("render_bench_report", ROOT / "scripts" / "render_bench_report.py")


def fake_port(kind="sync", answer=3, fail_at=None):
    """A stand-in framework port: a counter chain whose answer and failure point are controlled."""
    calls = {"n": 0}
    port = types.SimpleNamespace(PACKAGES=[], VARIANTS={"v": {"kind": kind}})
    port.load = lambda: None

    def build(variant, workload):
        if kind == "async":
            async def call():
                calls["n"] += 1
                if fail_at is not None and calls["n"] >= fail_at:
                    raise RuntimeError("boom")
                return answer
        else:
            def call():
                calls["n"] += 1
                if fail_at is not None and calls["n"] >= fail_at:
                    raise RuntimeError("boom")
                return answer
        return call

    def check(workload, result):
        assert result == 3, f"expected 3, got {result!r}"

    port.build, port.check = build, check
    return port


class CellValidity(unittest.TestCase):
    def run_cell(self, port, iters=20, samples=2, warmup=2):
        return common.run_loop(port, "v", "seq", iters, samples, warmup, {"framework": "fake"})

    def test_a_valid_cell_reports_every_completion_and_a_timing(self):
        for kind in ("sync", "async"):
            record = self.run_cell(fake_port(kind))
            self.assertEqual(record["status"], "ok", record)
            self.assertEqual(record["completions"], 40)
            self.assertEqual(record["failures"], 0)
            self.assertEqual(len(record["sample_s"]), 2)
            self.assertGreater(record["per_run_us"], 0)
            self.assertEqual(record["checked_runs"], 4)

    def test_a_wrong_answer_makes_an_error_cell_without_a_timing(self):
        for kind in ("sync", "async"):
            record = self.run_cell(fake_port(kind, answer=4))
            self.assertEqual(record["status"], "error")
            self.assertIsNone(record["per_run_us"])
            self.assertEqual(record["sample_s"], [])
            self.assertIn("expected 3", record["first_error"])

    def test_a_failed_run_makes_an_error_cell_and_names_the_exception(self):
        for kind in ("sync", "async"):
            record = self.run_cell(fake_port(kind, fail_at=15))
            self.assertEqual(record["status"], "error")
            self.assertGreaterEqual(record["failures"], 1)
            self.assertIsNone(record["per_run_us"])
            self.assertIn("boom", record["first_error"])

    def test_an_error_cell_exits_non_zero_and_prints_the_first_error(self):
        record = self.run_cell(fake_port(answer=4))
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = common.finish(record)
        self.assertEqual(code, 3)
        self.assertIn("expected 3", err.getvalue())
        self.assertTrue(out.getvalue().startswith("record\t"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(common.finish(self.run_cell(fake_port())), 0)

    def test_the_median_helper(self):
        self.assertEqual(common.median([3.0, 1.0, 2.0]), 2.0)
        self.assertEqual(common.median([1.0, 2.0, 3.0, 10.0]), 2.5)


class DriverCells(unittest.TestCase):
    def spec(self, script):
        return {"id": "t/x", "phase": "loop", "framework": "t", "source": "head", "variant": "v", "workload": "seq",
                "roles": ["matched_single"], "cmd": [sys.executable, "-c", script], "kind": "py_loop", "iters": 10,
                "timeout": 60}

    def test_a_good_record_becomes_a_timing(self):
        script = ("import json;print('record\\t'+json.dumps({'status':'ok','iters':10,'sample_s':[0.001,0.003,0.002],"
                  "'completions':30,'failures':0,'checked_runs':6}))")
        result = driver.execute(self.spec(script), Path("."))
        self.assertEqual(result["status"], "ok")
        self.assertAlmostEqual(result["per_run_us"], 200.0)

    def test_a_cell_that_exits_non_zero_is_an_error_even_with_a_record(self):
        script = ("import json,sys;print('record\\t'+json.dumps({'status':'ok','iters':10,'sample_s':[0.001],"
                  "'completions':10,'failures':0}));sys.exit(3)")
        result = driver.execute(self.spec(script), Path("."))
        self.assertEqual(result["status"], "error")

    def test_a_cell_without_a_record_is_an_error(self):
        result = driver.execute(self.spec("print('nothing')"), Path("."))
        self.assertEqual(result["status"], "error")
        self.assertIn("no record", result["first_error"])

    def test_the_first_error_is_kept_from_an_error_record(self):
        script = ("import json,sys;print('record\\t'+json.dumps({'status':'error','first_error':'boom','iters':10,"
                  "'sample_s':[],'completions':0,'failures':1}));sys.exit(3)")
        result = driver.execute(self.spec(script), Path("."))
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["first_error"], "boom")

    def test_host_paths_are_removed_from_stored_text(self):
        self.assertEqual(driver.sanitize("see /home/someone/x and /w/y", Path("/w")), "see <home>/x and <work>/y")


def synthetic_record():
    """A small record with the shape the driver writes."""
    plan = {
        "frameworks": {
            "fw": {"label": "Fw", "variants": {
                "fast": {"roles": ["matched_single", "idiomatic"], "label": "fast path", "iters": {"seq": 1, "par": 1}},
                "pool": {"roles": ["matched_pool"], "label": "pool path", "iters": {"seq": 1, "par": 1}},
            }},
        },
        "neograph": {"builds": {"hardened": {}, "unhardened": {}}},
    }
    cells = []

    def add(cid, phase, framework, source, variant, workload, value, rnd, **extra):
        cell = {"id": cid, "phase": phase, "framework": framework, "source": source, "variant": variant,
                "workload": workload, "roles": [], "status": "ok", "round": rnd, "order": 1, "per_run_us": value}
        cell.update(extra)
        cells.append(cell)

    for rnd, factor in ((1, 1.0), (2, 1.1), (3, 0.9)):
        for build, scale in (("hardened", 1.0), ("unhardened", 0.8)):
            add(f"neograph/{build}/workers_1/seq", "loop", "neograph", build, "workers_1", "seq", 5 * factor * scale, rnd,
                process={"hwm_kb_end": 6000}, build={"glibcxx_assertions": build == "hardened", "fortify_source": 2,
                                                       "stack_protector": "strong", "pie": True})
            add(f"neograph/{build}/workers_1/par", "loop", "neograph", build, "workers_1", "par", 12 * factor * scale, rnd,
                process={"hwm_kb_end": 6500})
            add(f"neograph/{build}/workers_auto/par", "loop", "neograph", build, "workers_auto", "par", 270 * factor * scale, rnd,
                workers="auto(4)")
        add("neograph/hardened/workers_auto/seq", "loop", "neograph", "hardened", "workers_auto", "seq", 9 * factor, rnd)
        for source in ("head", "release"):
            for variant, base in (("fast", 100), ("pool", 400)):
                for workload, mult in (("seq", 1), ("par", 3)):
                    add(f"fw/{source}/{variant}/{workload}", "loop", "fw", source, variant, workload,
                        base * mult * factor, rnd, versions={"fw-pkg": "1.0.0"}, process={"hwm_kb_end": 60000})
    return {
        "schema": 1, "kind": "cross_framework_cohort", "plan": plan, "cells": cells, "sizes": {}, "probes": {},
        "provenance": {
            "harness_commit": "a" * 40, "neograph_engine_commit": "b" * 40, "script_sha256": {"x": "0" * 64},
            "session": {"utc_start": "2026-10-22T00:00:00Z", "utc_end": "2026-10-22T01:00:00Z",
                        "loadavg_start": [0.1, 0.1, 0.1], "loadavg_end": [0.2, 0.2, 0.2]},
            "host": {"cpu": {"model": "Test CPU", "count": 4, "governor": "not exposed by the host"}, "kernel": "6.0.0-test"},
            "builds": {"hardened": {}}, "packages": {"fw-head": {"python": "3.12.3", "pip_freeze": []}},
            "requirements_head": "", "requirements_release": "",
        },
    }


class Report(unittest.TestCase):
    def setUp(self):
        self.record = synthetic_record()
        self.record["provenance"]["record_name"] = "synthetic.json"
        self.agg = report.Agg(self.record)

    def test_medians_and_ranges_come_from_the_rounds(self):
        stat = self.agg.stat("neograph/hardened/workers_1/seq")
        self.assertAlmostEqual(stat.median, 5.0)
        self.assertAlmostEqual(stat.low, 4.5)
        self.assertAlmostEqual(stat.high, 5.5)
        self.assertEqual(stat.n, 3)

    def test_the_matched_block_forms_ratios_against_the_matching_neograph_row(self):
        single = report.block_matched_single(self.agg, "en")
        pool = report.block_matched_pool(self.agg, "en")
        # single thread: fw seq 100 against neograph 5.0 = 20x; pool: fw seq 400 against 9.0 = 44x
        self.assertIn("20.0×", single)
        self.assertNotIn("44.4×", single)
        self.assertIn("44.4×", pool)
        self.assertIn("12", pool)  # the worker=1 par figure of the range line

    def test_the_idiomatic_block_has_no_ratio(self):
        text = report.block_idiomatic(self.agg, "en")
        self.assertNotIn("×", text)

    def test_the_hardening_delta_is_a_ratio_of_the_two_builds(self):
        text = report.block_ceiling(self.agg, "en")
        self.assertIn("1.25×", text)

    def test_all_four_languages_render_the_same_table_shape(self):
        shapes = set()
        for locale in report.LOCALES:
            for name in ("matched_single", "matched_pool", "idiomatic", "ceiling"):
                text = report.BLOCKS[name](self.agg, locale)
                shapes.add((name, text.count("\n|"), sum(1 for line in text.splitlines() if line.startswith("```"))))
        self.assertEqual(len(shapes), 4, shapes)

    def test_the_rendering_is_deterministic_and_a_stale_document_is_noticed(self):
        with tempfile.TemporaryDirectory() as scratch:
            doc = Path(scratch) / "doc.md"
            doc.write_text("intro\n<!-- bench-record:begin matched_single -->\nstale\n<!-- bench-record:end matched_single -->\ntail\n",
                           encoding="utf-8")
            first = report.render_doc(doc, self.agg, "en")
            doc.write_text(first, encoding="utf-8")
            self.assertEqual(report.render_doc(doc, self.agg, "en"), first)
            self.assertNotIn("stale", first)
            self.assertTrue(first.startswith("intro\n") and first.endswith("tail\n"))
            changed = json.loads(json.dumps(self.record))
            changed["cells"][0]["per_run_us"] = 999.0
            second = report.render_doc(doc, report.Agg(changed), "en")
            self.assertNotEqual(first, second)

    def test_an_error_cell_is_listed_and_excluded_from_every_figure(self):
        record = json.loads(json.dumps(self.record))
        record["cells"].append({"id": "fw/head/fast/seq", "phase": "loop", "framework": "fw", "source": "head",
                                "variant": "fast", "workload": "seq", "roles": [], "status": "error", "round": 4,
                                "order": 1, "per_run_us": 5.0, "first_error": "boom"})
        agg = report.Agg(record)
        self.assertEqual(agg.stat("fw/head/fast/seq").n, 3)
        self.assertIn("boom", report.block_probes(agg, "en"))

    def test_a_record_without_provenance_is_refused(self):
        record = synthetic_record()
        del record["provenance"]["host"]
        with self.assertRaises(SystemExit):
            report.validate_record(record)


if __name__ == "__main__":
    os.chdir(ROOT)
    unittest.main()
