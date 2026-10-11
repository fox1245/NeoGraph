"""No-provider regression tests for benchmark result/exit accounting."""

import __future__
import ast
import contextlib
import io
import os
import sys
import types
import unittest
from unittest.mock import Mock, patch
from pathlib import Path

import bench
import bench_mock


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
