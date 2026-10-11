"""Node failure classification for Python node bodies (#364).

A Python exception raised by a node is an ordinary failure. When it carries a
string ``code`` attribute that is an author code (``app.compile_error``; the
``NG.`` prefix is reserved) the failure uses that code, and a boolean
``retryable`` attribute of ``False`` stops the retry loop. The caller still
receives the original exception object.
"""

import itertools

import pytest

import neograph_engine as ng


_ids = itertools.count()


class _Prep(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, _input):
        return [ng.ChannelWrite("result", "prep")]


ng.NodeFactory.register_type("node_failure_prep", lambda name, _config, _ctx: _Prep(name))


def _engine(node_class, max_retries=2, checkpoints=None, after_prep=False):
    type_name = f"node_failure_{next(_ids)}"
    ng.NodeFactory.register_type(type_name, lambda name, _config, _ctx: node_class(name))
    nodes = {"work": {"type": type_name}}
    edges = [{"from": "work", "to": ng.END_NODE}]
    if after_prep:
        nodes["prep"] = {"type": "node_failure_prep"}
        edges += [{"from": ng.START_NODE, "to": "prep"}, {"from": "prep", "to": "work"}]
    else:
        edges.append({"from": ng.START_NODE, "to": "work"})
    definition = {
        "name": "node_failure",
        "channels": {"result": {"reducer": "overwrite"}},
        "nodes": nodes,
        "edges": edges,
        "retry_policy": {"max_retries": max_retries, "initial_delay_ms": 1,
                         "backoff_multiplier": 1.0, "max_delay_ms": 1},
    }
    if checkpoints is None:
        return ng.GraphEngine.compile(definition, ng.NodeContext())
    return ng.GraphEngine.compile(definition, ng.NodeContext(), checkpoints)


def _failing_node(make_error, succeed_on=None):
    calls = []

    class Node(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, _input):
            calls.append(len(calls))
            if succeed_on is not None and len(calls) - 1 == succeed_on:
                return [ng.ChannelWrite("result", "ok")]
            raise make_error()

    return Node, calls


class CompileError(Exception):
    code = "app.compile_error"
    retryable = False


class FlakyCheck(Exception):
    code = "app.flaky_check"
    retryable = True


class HttpLikeError(Exception):
    """A library error whose ``code`` is an HTTP status, not an author code."""

    def __init__(self):
        super().__init__("service unavailable")
        self.code = 503


class VendorCodedError(Exception):
    code = "E42"


class GiveUp(Exception):
    retryable = False


def test_non_retryable_author_code_runs_once_and_keeps_the_exception():
    error = CompileError("the patch does not compile")
    node, calls = _failing_node(lambda: error)
    engine = _engine(node)

    with pytest.raises(CompileError) as info:
        engine.run(ng.RunConfig(thread_id="compile-error", input={}))

    assert len(calls) == 1
    assert info.value is error
    assert info.value.node_name == "work"
    assert info.value.attempts == 1
    error.__traceback__ = None


def test_retryable_author_code_is_retried():
    node, calls = _failing_node(lambda: FlakyCheck("flaky"), succeed_on=2)
    result = _engine(node).run(ng.RunConfig(thread_id="flaky-check", input={}))

    assert len(calls) == 3
    assert result.output["channels"]["result"]["value"] == "ok"


def test_retryable_false_without_a_code_still_stops_the_retry_loop():
    node, calls = _failing_node(lambda: GiveUp("no point retrying"))
    with pytest.raises(GiveUp) as info:
        _engine(node).run(ng.RunConfig(thread_id="give-up", input={}))
    assert len(calls) == 1
    assert info.value.attempts == 1


@pytest.mark.parametrize("error_class", [HttpLikeError, VendorCodedError, RuntimeError])
def test_other_python_errors_are_ordinary_and_retried(error_class):
    node, calls = _failing_node(lambda: error_class())
    with pytest.raises(error_class) as info:
        _engine(node).run(ng.RunConfig(thread_id=f"ordinary-{error_class.__name__}", input={}))
    assert len(calls) == 3
    assert info.value.attempts == 3


# ── Effect-aware retry: a resume continues the attempt that was in flight ──

@pytest.mark.parametrize("after_prep", [False, True], ids=["entry-node", "later-step"])
def test_a_resume_continues_the_interrupted_retry_attempt(after_prep):
    """A node that pauses during its second attempt resumes in that attempt.

    The attempt keeps its effect namespace (so a host effect broker replays
    what it already committed) and the retry budget is not reset: with
    max_retries=1 the resumed attempt is the last one.
    """
    calls = []
    attempts_seen = []

    class Work(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            calls.append(input.ctx.resume_value)
            if len(calls) == 1:
                raise RuntimeError("the first attempt fails")
            if input.ctx.resume_value is None:
                raise ng.NodeInterrupt("check with a human during the retry")
            raise RuntimeError("still failing after the human answered")

    engine = _engine(Work, max_retries=1, checkpoints=ng.InMemoryCheckpointStore(),
                     after_prep=after_prep)
    thread = f"resume-attempt-{after_prep}"

    first = engine.run_stream(
        ng.RunConfig(thread_id=thread, input={}),
        lambda event: attempts_seen.append(event.data.get("retry_attempt", 0))
        if event.type == ng.GraphEvent.Type.NODE_START and event.node_name == "work" else None)
    assert first.interrupted
    assert first.interrupt_node == "work"

    with pytest.raises(RuntimeError, match="still failing") as info:
        engine.resume(thread, {"approved": True})

    # Attempt 0 failed, attempt 1 paused, the resume ran attempt 1 again and
    # stopped: three node calls, two attempts, no retry budget handed back.
    assert len(calls) == 3
    assert info.value.attempts == 2
    assert attempts_seen == [0, 1]
    info.value.__traceback__ = None


# ── The same through a Program run on LocalProgramHost ──────────────────────

_DIGEST_STEP = "sha256:" + "5" * 64
_DIGEST_FLAKY = "sha256:" + "6" * 64
_DIGEST_APPEND = "sha256:" + "7" * 64


def _program_budget(max_dynamic_compiles=0):
    budget = ng.ProgramRunBudget()
    budget.wall_time_ms = 20_000
    budget.model_tokens = 1_000
    budget.monetary_microunits = 1_000
    budget.max_concurrency = 2
    budget.max_program_operations = 32
    budget.max_core_steps = 20
    budget.max_dynamic_compiles = max_dynamic_compiles
    budget.max_child_depth = 1
    budget.max_total_children = 4
    return budget


def test_a_program_resumes_a_paused_retry_attempt_with_the_budget_left():
    """Program e2e: a node fails, pauses for a human in its retry, and the
    resumed run continues that retry: it does not get a fresh retry budget."""
    calls = []

    class Step(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, _input):
            return [ng.ChannelWrite("trace", [self._name])]

    class Flaky(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            calls.append(input.ctx.resume_value)
            if len(calls) == 2 and input.ctx.resume_value is None:
                raise ng.NodeInterrupt("confirm the retry")
            raise FlakyCheck("the check failed again")

    ng.NodeFactory.register_type("node_failure_program_step", lambda name, _c, _x: Step(name))
    ng.NodeFactory.register_type("node_failure_program_flaky", lambda name, _c, _x: Flaky(name))
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node("node_failure_program_step", "1.0.0", _DIGEST_STEP)
    builder.add_registered_node("node_failure_program_flaky", "1.0.0", _DIGEST_FLAKY)
    builder.add_registered_reducer("append", "1.0.0", _DIGEST_APPEND)
    host = ng.LocalProgramHost(builder.build(), "node-failure-owner",
                               _program_budget(2), "node-failure-host/v1")
    source = ng.ProgramSource.from_javascript(
        "node-failure-retry.js",
        'export function define() { return ng.graph("main")'
        '.channel("trace", {reducer: "append", initial: []})'
        '.node("prepare", {type: "node_failure_program_step"})'
        '.node("check", {type: "node_failure_program_flaky"})'
        '.entry("prepare").edge("prepare", "check").exit("check")'
        '.retryPolicy({max_retries: 1, initial_delay_ms: 10, backoff_multiplier: 1,'
        ' max_delay_ms: 10}); }',
    )
    version = host.compile_admit(source, _program_budget())

    paused = host.run(version, {}, _program_budget())
    assert paused.status == ng.ProgramTerminalStatus.Interrupted
    assert paused.interrupt.core_node == "check"

    finished = host.resume(paused.run_id, {"retry": True}).wait()
    # Attempt 0 failed, attempt 1 paused and, resumed, failed: the retry
    # budget (max_retries 1) is spent. A fresh budget would have run attempt 0
    # and attempt 1 again.
    assert finished.status == ng.ProgramTerminalStatus.Failed
    assert finished.failure.core_node == "check"
    assert finished.failure.attempts == 2
    assert len(calls) == 3


# ---------------------------------------------------------------------------
# Failure routing through the Core engine (#364)
# ---------------------------------------------------------------------------


def _routed_engine(node_class, policy=None, exits=None, redactor=None):
    type_name = f"node_failure_{next(_ids)}"
    ng.NodeFactory.register_type(type_name, lambda name, _config, _ctx: node_class(name))
    definition = {
        "schema_version": 1,
        "name": "node_failure_routing",
        "channels": {"result": {"reducer": "overwrite"},
                     "failures": {"reducer": "append", "initial": []}},
        "nodes": {"work": {"type": type_name}, "handler": {"type": "node_failure_prep"}},
        "edges": [{"from": ng.START_NODE, "to": "work"}, {"from": "work", "to": ng.END_NODE},
                  {"from": "handler", "to": ng.END_NODE}],
        "node_retry_policies": {"work": policy or {"max_retries": 1, "initial_delay_ms": 10,
                                                   "backoff_multiplier": 1.0,
                                                   "max_delay_ms": 10}},
        "failure_edges": [{"from": "work", "to": "handler", "error_channel": "failures"}],
    }
    if exits:
        definition["failure_exits"] = exits
        definition["edges"] = [edge for edge in definition["edges"] if edge["from"] not in exits]
    return ng.GraphEngine.compile(definition, ng.NodeContext(),
                                  failure_text_redactor=redactor)


def test_a_routed_python_failure_completes_through_its_handler():
    node, calls = _failing_node(lambda: CompileError("does not build"))
    result = _routed_engine(node).run(ng.RunConfig())
    assert result.status == ng.RunStatus.Completed
    assert len(calls) == 1  # app.compile_error is not retryable
    assert result.execution_trace == ["work", "handler"]
    failures = result.output["channels"]["failures"]["value"]
    assert failures == [{"node": "work", "kind": "non_retryable", "code": "app.compile_error",
                         "attempts": 1, "retryable": False, "step": 0, "routed_to": "handler",
                         "effects": []}]


def test_the_redactor_sees_public_message_only():
    class Secret(Exception):
        public_message = "token=hunter2"

    seen = []

    def redact(failure):
        seen.append(failure)
        return failure["public_message"].replace("hunter2", "***")

    node, _calls = _failing_node(lambda: Secret("what() is never a source"))
    engine = _routed_engine(node, policy={"error_text": True}, redactor=redact)
    failures = engine.run(ng.RunConfig()).output["channels"]["failures"]["value"]
    assert failures[0]["detail"] == {"untrusted_text": "token=***"}
    assert seen[0]["public_message"] == "token=hunter2"


def test_a_failure_routed_to_a_failure_exit_reports_failure_exit():
    node, _calls = _failing_node(lambda: RuntimeError("fails"))
    engine = _routed_engine(node, exits={"handler": {"code": "plan.stop"}})
    result = engine.run(ng.RunConfig())
    assert result.status == ng.RunStatus.FailureExit
    assert result.failure_exited
    assert result.output["_neograph"]["failure_exit"] == {"node": "handler", "code": "plan.stop"}


def test_resolved_retry_policy_only_tightens_a_sealed_node():
    node, _calls = _failing_node(lambda: RuntimeError("x"))
    engine = _routed_engine(node, policy={"max_retries": 3, "initial_delay_ms": 50})
    host = ng.RetryPolicy()
    host.max_retries = 8
    host.initial_delay_ms = 10
    engine.set_node_retry_policy("work", host)
    resolved = engine.resolved_retry_policy("work")
    assert resolved.max_retries == 3
    assert resolved.initial_delay_ms == 50


def test_a_program_diagnostic_raised_by_a_node_is_not_retried():
    if not getattr(ng, "_HAVE_PROGRAM", False):
        pytest.skip("neograph_engine was built without Program")
    node, calls = _failing_node(lambda: ng.ProgramDiagnosticError("a definition verdict"))
    with pytest.raises(ng.ProgramDiagnosticError):
        _engine(node).run(ng.RunConfig())
    assert len(calls) == 1
