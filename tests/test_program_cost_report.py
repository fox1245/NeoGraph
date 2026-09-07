"""Statistical contract of the diagnostic cost runner (stdlib only)."""
import importlib.util
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "program_costs", Path(__file__).resolve().parents[1] / "scripts/run_program_costs.py")
costs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(costs)


class CostStatistics(unittest.TestCase):
    def test_nearest_rank_and_mad_keep_outlier_visible(self):
        result = costs.stats([1, 2, 3, 4, 5, 6, 1000])
        self.assertEqual(result["median"], 4)
        self.assertEqual(result["p95"], 1000)
        self.assertEqual(result["mad"], 2)

    def test_invalid_samples_cannot_be_a_successful_summary(self):
        for values in [[], [math.nan], [math.inf], [-1]]:
            with self.subTest(values=values), self.assertRaises(ValueError):
                costs.stats(values)

    def test_processes_are_the_independent_unit(self):
        # A process with many inner runs must not outweigh another process.
        records = [{"samples": [{"total_us": 1}] * 1000},
                   {"samples": [{"total_us": 9}]}]
        summary = costs.summarize(records)["invocation_median.total_us"]
        self.assertEqual(summary["n"], 2)
        self.assertEqual(summary["median"], 5)


if __name__ == "__main__":
    unittest.main()
