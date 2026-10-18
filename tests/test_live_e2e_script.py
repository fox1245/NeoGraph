#!/usr/bin/env python3
"""Self-test of scripts/live_e2e.py: report parsing, key scrubbing and the release verification, in a scratch
git repository. Needs no key and no network."""
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "scripts" / "live_e2e.py"


class Repository:
    def __init__(self, directory):
        self.root = Path(directory)
        (self.root / "scripts").mkdir()
        shutil.copy(SCRIPT, self.root / "scripts" / "live_e2e.py")
        (self.root / "cmake").mkdir()
        (self.root / "cmake" / "NeoGraphSchemaProvider.cmake").write_text(
            'set(PIN\n    "' + "a" * 40 + '")\n', encoding="utf-8")
        (self.root / "pyproject.toml").write_text('[project]\nversion = "9.8.7"\n', encoding="utf-8")
        (self.root / "tests" / "live").mkdir(parents=True)
        self.write_manifest(["py:test_a::test_one", "py:test_a::test_gap"],
                            {"py:test_a::test_gap": "a known gap"})
        (self.root / "source.txt").write_text("one\n", encoding="utf-8")
        (self.root / ".gitignore").write_text("__pycache__/\n", encoding="utf-8")
        self.git("init", "-q")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "add", "-A")
        self.commit("start")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True, text=True).stdout

    def commit(self, message):
        self.git("add", "-A")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false",
                 "commit", "-q", "-m", message)

    def write_manifest(self, cases, gaps):
        (self.root / "tests" / "live" / "manifest.json").write_text(json.dumps(
            {"format": "neograph.live-e2e-manifest/v1", "known_gaps": gaps, "cases": cases}), encoding="utf-8")

    def module(self):
        spec = importlib.util.spec_from_file_location("live_e2e_copy", self.root / "scripts" / "live_e2e.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def write_report(self, **changes):
        module = self.module()
        report = {"format": module.FORMAT, "neograph": "9.8.7", "commit": self.git("rev-parse", "HEAD").strip(),
                  "tree": module.tree_hash(), "dirty": False,
                  "cases": {"py:test_a::test_one": {"status": "passed", "seconds": 1, "message": ""},
                            "py:test_a::test_gap": {"status": "xfailed", "seconds": 1, "message": ""}}}
        report.update(changes)
        (self.root / "tests" / "live" / "report.json").write_text(json.dumps(report), encoding="utf-8")

    def verify(self):
        return subprocess.run([sys.executable, "scripts/live_e2e.py", "verify"], cwd=self.root,
                              capture_output=True, text=True)


class VerifyTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.repo = Repository(self.scratch.name)

    def tearDown(self):
        self.scratch.cleanup()

    def test_a_report_of_this_tree_with_expected_results_passes(self):
        self.repo.write_report()
        self.repo.commit("report")
        result = self.repo.verify()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("verified", result.stdout)

    def test_a_missing_report_fails(self):
        self.assertNotEqual(self.repo.verify().returncode, 0)

    def test_a_change_after_the_run_fails(self):
        self.repo.write_report()
        (self.repo.root / "source.txt").write_text("two\n", encoding="utf-8")
        self.repo.commit("report and an untested change")
        result = self.repo.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not describe this tree", result.stdout)

    def test_failed_missing_and_unexpected_results_fail(self):
        cases = {
            "failed": {"py:test_a::test_one": {"status": "failed", "seconds": 1, "message": "x"},
                       "py:test_a::test_gap": {"status": "xfailed", "seconds": 1, "message": ""}},
            "missing": {"py:test_a::test_gap": {"status": "xfailed", "seconds": 1, "message": ""}},
            "gap passed": {"py:test_a::test_one": {"status": "passed", "seconds": 1, "message": ""},
                           "py:test_a::test_gap": {"status": "passed", "seconds": 1, "message": ""}},
            "skipped": {"py:test_a::test_one": {"status": "skipped", "seconds": 0, "message": "no key"},
                        "py:test_a::test_gap": {"status": "xfailed", "seconds": 1, "message": ""}},
        }
        for name, value in cases.items():
            with self.subTest(name):
                self.repo.write_report(cases=value)
                self.repo.commit(name)
                self.assertNotEqual(self.repo.verify().returncode, 0, name)

    def test_an_extra_failed_case_fails(self):
        self.repo.write_report(cases={
            "py:test_a::test_one": {"status": "passed", "seconds": 1, "message": ""},
            "py:test_a::test_gap": {"status": "xfailed", "seconds": 1, "message": ""},
            "py:test_b::test_new": {"status": "failed", "seconds": 1, "message": "x"}})
        self.repo.commit("extra failure")
        self.assertNotEqual(self.repo.verify().returncode, 0)

    def test_a_dirty_or_other_version_report_fails(self):
        for changes in ({"dirty": True}, {"neograph": "9.8.6"}, {"format": "other"}):
            with self.subTest(changes):
                self.repo.write_report(**changes)
                self.repo.commit(str(changes))
                self.assertNotEqual(self.repo.verify().returncode, 0)


class ParseTest(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("live_e2e", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_junit_results_and_key_scrubbing(self):
        with tempfile.TemporaryDirectory() as scratch:
            junit = Path(scratch, "r.xml")
            junit.write_text(textwrap.dedent("""\
                <testsuites><testsuite>
                <testcase classname="live.test_live_matrix" name="test_text[openai.chat-collect]" time="1.5"/>
                <testcase classname="live.test_live_matrix" name="test_gap[tts]" time="0.4">
                  <skipped type="pytest.xfail" message="known gap"/></testcase>
                <testcase classname="test_live_provider" name="test_x[gemini]" time="0.1">
                  <skipped type="pytest.skip" message="live: set GEMINI_API_KEY"/></testcase>
                <testcase classname="test_live_provider" name="test_y[openai]" time="2">
                  <failure message="key sk-secret-value-123 leaked"/></testcase>
                </testsuite></testsuites>"""), encoding="utf-8")
            cases = self.module.python_cases(junit, ["sk-secret-value-123"])
        self.assertEqual(cases["py:test_live_matrix::test_text[openai.chat-collect]"]["status"], "passed")
        self.assertEqual(cases["py:test_live_matrix::test_gap[tts]"]["status"], "xfailed")
        self.assertEqual(cases["py:test_live_provider::test_x[gemini]"]["status"], "skipped")
        failed = cases["py:test_live_provider::test_y[openai]"]
        self.assertEqual(failed["status"], "failed")
        self.assertNotIn("sk-secret-value-123", failed["message"])

    def test_gtest_results(self):
        with tempfile.TemporaryDirectory() as scratch:
            report = Path(scratch, "g.json")
            report.write_text(json.dumps({"testsuites": [{"name": "Mode/LiveAdapter", "testsuite": [
                {"name": "InteractionsText/Collect", "result": "COMPLETED", "time": "1.2s"},
                {"name": "InteractionsText/Stream", "result": "SKIPPED", "time": "0s"},
                {"name": "GeminiGeneratedImage/Stream", "result": "COMPLETED", "time": "3s",
                 "failures": [{"failure": "http 400"}]}]}]}), encoding="utf-8")
            cases = self.module.cxx_cases(report, [])
        self.assertEqual(cases["cpp:Mode/LiveAdapter.InteractionsText/Collect"]["status"], "passed")
        self.assertEqual(cases["cpp:Mode/LiveAdapter.InteractionsText/Stream"]["status"], "skipped")
        self.assertEqual(cases["cpp:Mode/LiveAdapter.GeminiGeneratedImage/Stream"]["status"], "failed")


if __name__ == "__main__":
    unittest.main()
