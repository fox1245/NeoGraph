"""No-provider regression tests for benchmark result/exit accounting."""

import contextlib
import io
import sys
import types
import unittest
from unittest.mock import patch

import bench
import bench_mock


class BenchmarkAccountingTests(unittest.TestCase):
    def run_cohort(self, harness, replies, warmup=0, iters=2):
        calls = iter(replies)

        def run_query(query, thread_id):
            reply = next(calls)
            if isinstance(reply, Exception):
                raise reply
            return reply

        runner = types.SimpleNamespace(run_query=run_query)
        output = io.StringIO()
        with patch.dict(sys.modules, {"dr_neograph": runner}), patch.object(
                sys, "argv", ["bench", "--only", "neograph", "--warmup", str(warmup),
                              "--iters", str(iters)]), contextlib.redirect_stdout(output):
            code = harness.main()
        return code, output.getvalue()

    def test_failed_measurement_is_not_a_successful_cohort(self):
        for harness in (bench, bench_mock):
            with self.subTest(harness=harness.__name__):
                code, output = self.run_cohort(harness, [RuntimeError("dispatch failed"), "report"])
                self.assertEqual(code, 1)
                self.assertIn("dispatch failed", output)
                self.assertIn("n=1", output)
                self.assertIn("Failed runs: 1", output)

    def test_empty_report_cannot_count_as_a_successful_sample(self):
        for harness in (bench, bench_mock):
            with self.subTest(harness=harness.__name__):
                code, output = self.run_cohort(harness, ["  ", "report"])
                self.assertEqual(code, 1)
                self.assertIn("no visible report", output)
                self.assertIn("n=1", output)

    def test_failed_real_warmup_is_retained_in_exit_status(self):
        code, output = self.run_cohort(bench, [RuntimeError("warmup failed"), "report"],
                                      warmup=1, iters=1)
        self.assertEqual(code, 1)
        self.assertIn("Failed runs: 1", output)

    def test_failed_mock_warmup_aborts(self):
        code, output = self.run_cohort(bench_mock, [RuntimeError("warmup failed")],
                                      warmup=1, iters=1)
        self.assertEqual(code, 1)
        self.assertIn("FAILED — abort", output)

    def test_successful_cohort_preserves_all_samples(self):
        for harness in (bench, bench_mock):
            with self.subTest(harness=harness.__name__):
                code, output = self.run_cohort(harness, ["report", "report"])
                self.assertEqual(code, 0)
                self.assertIn("n=2", output)
                self.assertIn("Failed runs: 0", output)


if __name__ == "__main__":
    unittest.main()
