"""No-provider regression tests for benchmark result/exit accounting."""

import __future__
import ast
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch
from pathlib import Path

import bench
import bench_mock
import session


class BenchmarkAccountingTests(unittest.TestCase):
    def run_cohort(self, harness, replies, warmup=0, iters=2, environment=None):
        calls = iter(replies)

        def run_query(query, thread_id):
            reply = next(calls)
            if isinstance(reply, Exception):
                raise reply
            return reply

        runner = types.SimpleNamespace(run_query=run_query, LLM_MOCK_MS=0, MOCK_SEARCH=True)
        output = io.StringIO()
        environment = {"LLM_MOCK_MS": "0"} if environment is None else environment
        with patch.dict(os.environ, environment), patch.dict(
                sys.modules, {"dr_neograph": runner}), patch.object(
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

    def test_mock_defaults_select_only_local_work(self):
        with patch.dict(os.environ, {}, clear=True):
            code, output = self.run_cohort(bench_mock, ["report"], iters=1, environment={})
        self.assertEqual(code, 0)
        self.assertIn("LLM_MOCK_MS:  0", output)
        self.assertIn("MOCK_SEARCH:  1", output)
        self.assertIn("USE_INMEMORY: 1", output)

    def test_mock_rejects_inherited_paid_mode_before_dispatch(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            self.run_cohort(bench_mock, [], iters=1, environment={"LLM_MOCK_MS": "-1"})
        self.assertEqual(error.exception.code, 2)

    def test_empty_cohorts_and_negative_warmups_are_rejected(self):
        for harness in (bench, bench_mock):
            for warmup, iters in ((0, 0), (0, -1), (-1, 1)):
                with self.subTest(harness=harness.__name__, warmup=warmup, iters=iters):
                    with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                        self.run_cohort(harness, [], warmup=warmup, iters=iters)
                    self.assertEqual(error.exception.code, 2)

    def test_cached_non_mock_runner_is_not_dispatched(self):
        for mock_ms, mock_search in ((-1, True), (0, False)):
            dispatch = Mock(side_effect=AssertionError("must not dispatch"))
            runner = types.SimpleNamespace(run_query=dispatch,
                                           LLM_MOCK_MS=mock_ms, MOCK_SEARCH=mock_search)
            with patch.dict(os.environ, {"LLM_MOCK_MS": "0"}), patch.dict(
                    sys.modules, {"dr_neograph": runner}), patch.object(
                    sys, "argv", ["bench_mock", "--only", "neograph", "--warmup", "0",
                                  "--iters", "1"]), contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                bench_mock.main()
            self.assertEqual(error.exception.code, 2)
            dispatch.assert_not_called()


class MatchedWidthAndJsonTests(unittest.TestCase):
    """Pool widths of the two sides, and the raw-sample report. Runners are stubs; no engine is imported."""

    def run_both(self, environment, extra_args=(), replies=None, warmup=0, iters=2):
        calls = {"neograph": iter(replies or ["report"] * (warmup + iters)),
                 "langgraph": iter(replies or ["report"] * (warmup + iters))}
        dispatched = []

        def runner(side):
            def run_query(query, thread_id):
                dispatched.append(side)
                reply = next(calls[side])
                if isinstance(reply, Exception):
                    raise reply
                return reply
            return types.SimpleNamespace(run_query=run_query, LLM_MOCK_MS=0, MOCK_SEARCH=True)

        output = io.StringIO()
        modules = {"dr_neograph": runner("neograph"), "dr_langgraph": runner("langgraph")}
        with patch.dict(os.environ, environment, clear=True), patch.dict(sys.modules, modules), patch.object(
                sys, "argv", ["bench_mock", "--warmup", str(warmup), "--iters", str(iters), *extra_args]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            code = bench_mock.main()
        return code, output.getvalue(), dispatched

    def test_default_widths_are_matched_and_printed(self):
        code, output, _ = self.run_both({})
        self.assertEqual(code, 0)
        self.assertIn("NeoGraph worker_count=4, LangGraph max_concurrency=4", output)
        self.assertNotIn("UNMATCHED", output)

    def test_langgraph_follows_the_neograph_worker_count_unless_told_otherwise(self):
        self.assertEqual(bench_mock._widths({"NG_WORKER_COUNT": "7"}), (7, 7))
        self.assertEqual(bench_mock._widths({"NG_WORKER_COUNT": "7", "LG_MAX_CONCURRENCY": "3"}), (7, 3))
        self.assertEqual(bench_mock._widths({}), (4, 4))

    def test_unmatched_widths_are_refused_before_any_dispatch(self):
        dispatched = []
        with self.assertRaises(SystemExit) as error:
            _, _, dispatched = self.run_both({"NG_WORKER_COUNT": "4", "LG_MAX_CONCURRENCY": "8"})
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(dispatched, [])

    def test_unmatched_widths_run_only_when_allowed_and_are_labelled(self):
        code, output, dispatched = self.run_both({"NG_WORKER_COUNT": "4", "LG_MAX_CONCURRENCY": "8"},
                                                 ["--allow-unmatched-widths"])
        self.assertEqual(code, 0)
        self.assertIn("(UNMATCHED)", output)
        self.assertEqual(sorted(set(dispatched)), ["langgraph", "neograph"])

    def test_one_side_alone_needs_no_matching(self):
        code, _, dispatched = self.run_both({"NG_WORKER_COUNT": "4", "LG_MAX_CONCURRENCY": "8"},
                                            ["--only", "neograph"])
        self.assertEqual(code, 0)
        self.assertEqual(set(dispatched), {"neograph"})

    def test_non_positive_widths_are_rejected(self):
        for environment in ({"NG_WORKER_COUNT": "0"}, {"LG_MAX_CONCURRENCY": "abc"}):
            with self.subTest(environment=environment), self.assertRaises(SystemExit) as error:
                self.run_both(environment)
            self.assertEqual(error.exception.code, 2)

    def test_json_holds_raw_samples_widths_and_env_knobs(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = os.path.join(scratch, "report.json")
            code, _, _ = self.run_both({"FANOUT": "5", "NG_WORKER_COUNT": "4"}, ["--json", path], iters=3)
            self.assertEqual(code, 0)
            with open(path, encoding="utf-8") as handle:
                report = json.load(handle)
        self.assertEqual(report["schema"], 1)
        self.assertEqual(report["kind"], "dr_compare_mock")
        self.assertEqual({side: len(values) for side, values in report["samples_s"].items()},
                         {"neograph": 3, "langgraph": 3})
        self.assertTrue(all(isinstance(v, float) and v >= 0 for v in report["samples_s"]["neograph"]))
        self.assertEqual(report["widths"], {"neograph": 4, "langgraph": 4})
        self.assertTrue(report["widths_matched"])
        self.assertEqual(report["failures"], 0)
        self.assertEqual(report["exit"], 0)
        self.assertEqual(report["env"]["FANOUT"], "5")
        self.assertEqual(report["env"]["MOCK_SEARCH"], "1")
        self.assertEqual(report["env"]["LG_MAX_CONCURRENCY"], None)
        self.assertEqual(report["env"]["NG_WORKER_COUNT"], "4")

    def test_json_keeps_the_first_error_of_a_failed_run(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = os.path.join(scratch, "report.json")
            code, _, _ = self.run_both({}, ["--json", path, "--only", "neograph"],
                                       replies=[RuntimeError("dispatch failed"), "report"], iters=2)
            with open(path, encoding="utf-8") as handle:
                report = json.load(handle)
        self.assertEqual(code, 1)
        self.assertEqual(report["failures"], 1)
        self.assertEqual(report["errors"][0]["side"], "neograph")
        self.assertIn("dispatch failed", report["errors"][0]["message"])
        self.assertEqual(report["exit"], 1)
        self.assertEqual(len(report["samples_s"]["neograph"]), 1)


class SessionTests(unittest.TestCase):
    """session.py with a stub in place of bench_mock.py."""

    STUB = r'''
import json, os, sys
args = sys.argv[1:]
path = args[args.index("--json") + 1]
mode = os.environ.get("STUB_MODE", "ok")
iters = int(args[args.index("--iters") + 1])
if mode == "crash":
    sys.stderr.write("Traceback\nValueError: FANOUT must be positive\n")
    sys.exit(1)
width = int(os.environ["NG_WORKER_COUNT"]), int(os.environ["LG_MAX_CONCURRENCY"])
report = {"schema": 1, "widths": {"neograph": width[0], "langgraph": width[1]}, "widths_matched": width[0] == width[1],
          "env": {k: os.environ.get(k) for k in ("LLM_MOCK_MS", "FANOUT", "NG_WORKER_COUNT", "LG_MAX_CONCURRENCY")},
          "samples_s": {"neograph": [0.01] * iters, "langgraph": [0.03] * iters}, "failures": 0, "errors": []}
if mode == "failed-run":
    report["failures"] = 1
    report["errors"] = [{"side": "langgraph", "message": "RuntimeError: boom"}]
    report["samples_s"]["langgraph"] = report["samples_s"]["langgraph"][1:]
json.dump(report, open(path, "w"))
sys.exit(1 if report["failures"] else 0)
'''

    def run_session(self, *extra, rounds="2", configs="A,C", environment=None):
        scratch = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(scratch, ignore_errors=True))
        stub = Path(scratch) / "stub.py"
        stub.write_text(self.STUB, encoding="utf-8")
        out = Path(scratch) / "record.json"
        command = [sys.executable, str(Path(session.__file__)), "--python", sys.executable, "--pythonpath", scratch,
                   "--out", str(out), "--rounds", rounds, "--configs", configs, "--no-load-check", "--scale", "0.1",
                   "--bench", str(stub), *extra]
        env = dict(os.environ, **(environment or {}))
        done = subprocess.run(command, capture_output=True, text=True, env=env)
        record = json.loads(out.read_text(encoding="utf-8")) if out.exists() else None
        return done, record

    def test_a_clean_session_records_every_cell_with_provenance(self):
        done, record = self.run_session()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual((record["schema"], record["kind"]), (1, "dr_compare_session"))
        self.assertEqual(len(record["cells"]), 4)
        self.assertEqual({c["id"] for c in record["cells"]}, {"A", "C"})
        self.assertTrue(all(c["status"] == "ok" and c["widths_matched"] for c in record["cells"]))
        prov = record["provenance"]
        for key in ("script_sha256", "session", "host", "python", "neograph_module", "env_knobs_per_config"):
            self.assertIn(key, prov)
        self.assertIn("benchmarks/dr_compare/session.py", prov["script_sha256"])
        self.assertEqual(prov["env_knobs_per_config"]["C"]["NG_WORKER_COUNT"], 50)
        self.assertEqual(prov["env_knobs_per_config"]["C"]["LG_MAX_CONCURRENCY"], 50)
        self.assertEqual(record["summary"]["A"]["rounds_ok"], 2)
        self.assertAlmostEqual(record["summary"]["A"]["ratio_langgraph_over_neograph"], 3.0)
        text = json.dumps(record)
        self.assertNotIn("/home/", text)

    def test_the_order_is_shuffled_per_round_and_reproducible(self):
        _, first = self.run_session(rounds="3", configs="A,B,C,D")
        _, second = self.run_session(rounds="3", configs="A,B,C,D")
        order = lambda rec: [(c["round"], c["id"]) for c in rec["cells"]]  # noqa: E731
        self.assertEqual(order(first), order(second))
        per_round = {}
        for c in first["cells"]:
            per_round.setdefault(c["round"], []).append(c["id"])
        self.assertTrue(all(sorted(ids) == ["A", "B", "C", "D"] for ids in per_round.values()))

    def test_a_failed_run_makes_an_error_cell_without_timings_and_a_nonzero_exit(self):
        done, record = self.run_session(environment={"STUB_MODE": "failed-run"})
        self.assertNotEqual(done.returncode, 0)
        cell = record["cells"][0]
        self.assertEqual(cell["status"], "error")
        self.assertNotIn("samples_s", cell)
        self.assertIn("RuntimeError: boom", cell["first_error"])
        self.assertEqual(record["summary"], {})

    def test_a_process_that_writes_no_report_is_an_error_cell_with_its_last_stderr_line(self):
        done, record = self.run_session(environment={"STUB_MODE": "crash"})
        self.assertNotEqual(done.returncode, 0)
        self.assertTrue(all(c["status"] == "error" for c in record["cells"]))
        self.assertEqual(record["cells"][0]["first_error"], "ValueError: FANOUT must be positive")

    def test_unmatched_widths_in_a_cell_are_an_error(self):
        done, record = self.run_session("--cell-env", "LG_MAX_CONCURRENCY=9", configs="B", rounds="1")
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(record["cells"][0]["status"], "error")
        self.assertIn("not matched", record["cells"][0]["first_error"])

    def test_unknown_configuration_is_a_usage_error(self):
        done, record = self.run_session(configs="Z")
        self.assertEqual(done.returncode, 2)
        self.assertIsNone(record)


class ProviderProjectionPolicyTests(unittest.TestCase):
    # Execute the actual projection functions without importing/constructing a
    # graph or SDK client. These shape fixtures test consumer policy, NOT provider
    # decoding, transport, billing, native seals or hosted-model qualification.
    def function(self, filename, name, **namespace):
        path = Path(__file__).with_name(filename)
        tree = ast.parse(path.read_text(encoding="utf-8"))
        function = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == name)
        namespace["os"] = os
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), "exec",
                     flags=__future__.annotations.compiler_flag), namespace)
        return namespace[name]

    def neograph_function(self, outcome):
        tool_type = type("ToolPart", (), {})
        invalid_type = type("InvalidToolPart", (), {})
        ng = types.SimpleNamespace(
            ProviderControls=types.SimpleNamespace,
            make_provider_request=Mock(return_value=object()),
            ProviderStopKind=types.SimpleNamespace(EndTurn="EndTurn", StopSequence="StopSequence"),
            ProviderToolCall=tool_type, InvalidToolCall=invalid_type)
        dispatch = Mock(return_value=outcome)
        function = self.function("dr_neograph.py", "_workload_text", ng=ng,
                                 LLM_MOCK_MS=-1, DR_MODEL="policy-fixture",
                                 provider_messages=lambda messages: messages,
                                 PROVIDER=types.SimpleNamespace(invoke=dispatch))
        return function, dispatch, ng

    def test_ng_rejects_partial_refused_unknown_and_empty_outcomes_without_reask(self):
        for stop, text in (("MaxTokens", "partial"), ("Refusal", "refused"),
                           ("Unknown", "visible"), ("ToolUse", "commentary"), ("EndTurn", " \n")):
            outcome = types.SimpleNamespace(
                completion=types.SimpleNamespace(stop=types.SimpleNamespace(kind=stop), messages=[]),
                failure=None, text=text)
            function, dispatch, _ = self.neograph_function(outcome)
            with self.subTest(stop=stop), self.assertRaises(RuntimeError) as error:
                function([])
            self.assertIs(error.exception.outcome, outcome)
            self.assertEqual(dispatch.call_count, 1)

    def test_ng_failure_retains_partial_outcome_instead_of_projecting_it_to_success(self):
        outcome = types.SimpleNamespace(completion=None, text="partial",
                                        failure=types.SimpleNamespace(
                                            error=types.SimpleNamespace(safe_message="remote failure")))
        function, dispatch, _ = self.neograph_function(outcome)
        with self.assertRaises(RuntimeError) as error:
            function([])
        self.assertIs(error.exception.outcome, outcome)
        self.assertEqual(dispatch.call_count, 1)

    def test_ng_honors_explicit_cap_and_projects_only_completed_text(self):
        outcome = types.SimpleNamespace(
            completion=types.SimpleNamespace(stop=types.SimpleNamespace(kind="EndTurn"), messages=[]),
            failure=None, text="report")
        function, dispatch, ng = self.neograph_function(outcome)
        with patch.dict(os.environ, {"NG_EXAMPLE_MAX_TOKENS": "220"}):
            self.assertEqual(function([]), "report")
        self.assertEqual(ng.make_provider_request.call_args.kwargs["controls"].max_output_tokens, 220)
        self.assertEqual(dispatch.call_count, 1)

    def test_ng_tool_parts_never_become_a_text_only_sample(self):
        for part_name in ("ProviderToolCall", "InvalidToolCall"):
            outcome = types.SimpleNamespace(
                completion=types.SimpleNamespace(stop=types.SimpleNamespace(kind="EndTurn"), messages=[]),
                failure=None, text="commentary")
            function, dispatch, ng = self.neograph_function(outcome)
            outcome.completion.messages = [types.SimpleNamespace(parts=[getattr(ng, part_name)()])]
            with self.assertRaises(RuntimeError) as error:
                function([])
            self.assertIs(error.exception.outcome, outcome)
            self.assertEqual(dispatch.call_count, 1)

    def langgraph_function(self, response):
        dispatch = Mock(return_value=response)
        function = self.function("dr_langgraph.py", "_llm_text", LLM_MOCK_MS=-1,
                                 _LLM=types.SimpleNamespace(invoke=dispatch),
                                 HumanMessage=lambda **kwargs: kwargs)
        return function, dispatch

    def test_lg_uses_real_text_blocks_and_never_stringifies_reasoning_only_content(self):
        for content, expected in (([{"type": "text", "text": "report"}], "report"),
                                  ([{"type": "reasoning", "reasoning": "not an answer"}], None),
                                  ([], None), (" \n", None)):
            response = types.SimpleNamespace(content=content, response_metadata={"finish_reason": "stop"},
                                             tool_calls=[], invalid_tool_calls=[])
            function, dispatch = self.langgraph_function(response)
            if expected is None:
                with self.assertRaises(RuntimeError) as error:
                    function("question")
                self.assertIs(error.exception.response, response)
            else:
                self.assertEqual(function("question"), expected)
            self.assertEqual(dispatch.call_count, 1)

    def test_lg_rejects_partial_filtered_tool_and_unknown_termination(self):
        for finish in ("length", "content_filter", "tool_calls", None):
            response = types.SimpleNamespace(content="retained text",
                                             response_metadata={"finish_reason": finish},
                                             tool_calls=[], invalid_tool_calls=[])
            function, dispatch = self.langgraph_function(response)
            with self.assertRaises(RuntimeError) as error:
                function("question")
            self.assertIs(error.exception.response, response)
            self.assertEqual(dispatch.call_count, 1)

    def test_lg_tool_parts_do_not_become_a_text_only_sample_even_with_stop(self):
        for valid, invalid in (([{"name": "lookup"}], []), ([], [{"name": "lookup"}])):
            response = types.SimpleNamespace(content="commentary",
                                             response_metadata={"finish_reason": "stop"},
                                             tool_calls=valid, invalid_tool_calls=invalid)
            function, dispatch = self.langgraph_function(response)
            with self.assertRaises(RuntimeError) as error:
                function("question")
            self.assertIs(error.exception.response, response)
            self.assertEqual(dispatch.call_count, 1)


if __name__ == "__main__":
    unittest.main()
