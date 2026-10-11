"""Python parity for NeoGraph's optional Program / QuickJS control plane."""

import time

import pytest

import neograph_engine as ng


_DIGEST_NODE = "sha256:" + "1" * 64
_DIGEST_REDUCER = "sha256:" + "2" * 64


class _ProgramNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, _input):
        return [ng.ChannelWrite("value", 42)]


def _registry():
    ng.NodeFactory.register_type(
        "python_program_node",
        lambda name, _config, _ctx: _ProgramNode(name),
    )
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node(
        "python_program_node", "1.0.0", _DIGEST_NODE
    )
    builder.add_registered_reducer("overwrite", "1.0.0", _DIGEST_REDUCER)
    return builder.build()


def _budget():
    value = ng.ProgramRunBudget()
    value.wall_time_ms = 10_000
    value.model_tokens = 1_000
    value.monetary_microunits = 1_000
    value.max_concurrency = 2
    value.max_program_operations = 32
    value.max_core_steps = 20
    value.max_dynamic_compiles = 0
    value.max_child_depth = 1
    value.max_total_children = 4
    return value


def _ceiling():
    value = _budget()
    value.max_dynamic_compiles = 2
    return value


def _source():
    return ng.ProgramSource.from_javascript(
        "python-program.js",
        """
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "python_program_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}

export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
""",
    )


def test_python_compiles_the_same_quickjs_program_bundle_as_cpp():
    registry = _registry()
    source = _source()
    compiler = ng.ProgramCompiler(registry, "python-program-test/v1")

    bundle = compiler.compile(source, _budget())

    assert bundle.source_kind == ng.ProgramSourceKind.JavaScript
    assert bundle.source_hash == source.source_hash
    assert bundle.sealed_core_definitions[0]["name"] == "main"
    assert ng.ProgramBundle.parse(bundle.serialize_canonical()).id == bundle.id


def test_local_program_host_compiles_admits_and_executes_program_runtime():
    host = ng.LocalProgramHost(
        _registry(), "python-owner", _ceiling(), "python-program-host/v1"
    )
    version = host.compile_admit(_source(), _budget())

    result = host.run(version, {}, _budget(), "python-program-run")

    assert result.status == ng.ProgramTerminalStatus.Completed
    assert result.program_version_id == version.id
    assert result.output["channels"]["value"]["value"] == 42
    assert result.execution_trace == ["work"]
    assert ng.ProgramResult.parse(result.serialize_canonical()).id == result.id


def test_program_handle_start_wait_uses_the_same_runtime():
    host = ng.LocalProgramHost(
        _registry(), "python-owner-handle", _ceiling(), "python-program-host/v1"
    )
    version = host.compile_admit(_source(), _budget())

    handle = host.start(version, {}, _budget())
    result = handle.wait()

    assert handle.run_id == result.run_id
    assert result.status == ng.ProgramTerminalStatus.Completed

def test_local_program_host_active_start_pins_admitted_version():
    host = ng.LocalProgramHost(
        _registry(), "python-owner-active", _ceiling(), "python-program-host/v1"
    )
    version = host.compile_admit(_source(), _budget())
    with pytest.raises(RuntimeError, match="activation"):
        host.start_active({}, _budget(), "python-unactivated-run")
    assert host.activation() is None
    assert host.activate(version, 0) == ng.ProgramActivationResult.Activated
    activation, handle = host.start_active({}, _budget(), "python-active-run")
    assert activation.owner_scope == "python-owner-active"
    assert activation.active_version_id == version.id
    assert activation.generation == 1
    assert ng.ProgramActivation.parse(activation.serialize_canonical()).id == activation.id
    assert handle.program_version_id == activation.active_version_id
    result = handle.wait()
    assert result.status == ng.ProgramTerminalStatus.Completed
    assert result.output["channels"]["value"]["value"] == 42
    assert host.rollback(version, 1) == ng.ProgramActivationResult.AlreadyPresent
    assert host.activation().id == activation.id


def test_program_preserves_core_failure_in_terminal_result():
    class FailingNode(ng.GraphNode):
        def get_name(self):
            return "work"

        def run(self, _input):
            raise ValueError("local Program node rejected execution")

    ng.NodeFactory.register_type(
        "python_program_node", lambda _n, _c, _ctx: FailingNode())
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node("python_program_node", "1.0.0", _DIGEST_NODE)
    builder.add_registered_reducer("overwrite", "1.0.0", _DIGEST_REDUCER)
    host = ng.LocalProgramHost(
        builder.build(), "python-owner-failure", _ceiling(), "python-program-host/v1")
    version = host.compile_admit(_source(), _budget())
    result = host.run(version, {}, _budget(), "python-program-failure")
    assert result.status == ng.ProgramTerminalStatus.Failed
    assert result.failure.core_node == "work"
    parsed = ng.ProgramResult.parse(result.serialize_canonical())
    assert parsed.status == ng.ProgramTerminalStatus.Failed
    assert parsed.failure.code == result.failure.code
    assert parsed.failure.core_node == result.failure.core_node
    assert parsed.failure.provider_outcome is None


_DIGEST_TRACE_NODE = "sha256:" + "3" * 64
_DIGEST_APPEND = "sha256:" + "4" * 64


class _TraceNode(ng.GraphNode):
    def __init__(self, name, config):
        super().__init__()
        self._name = name
        self._fail = bool(config.get("fail"))

    def get_name(self):
        return self._name

    def run(self, _input):
        if self._fail:
            raise RuntimeError("trace node failed on purpose")
        return [ng.ChannelWrite("trace", [self._name])]


def _trace_host(owner):
    ng.NodeFactory.register_type(
        "python_program_trace_node",
        lambda name, config, _ctx: _TraceNode(name, config),
    )
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node(
        "python_program_trace_node", "1.0.0", _DIGEST_TRACE_NODE
    )
    builder.add_registered_reducer("append", "1.0.0", _DIGEST_APPEND)
    return ng.LocalProgramHost(
        builder.build(), owner, _ceiling(), "python-program-host/v1"
    )


def _trace_source(body):
    return ng.ProgramSource.from_javascript(
        "python-trace.js",
        'export function define() { return ng.graph("main")'
        '.channel("trace", {reducer: "append", initial: []})' + body + "; }",
    )


def test_resume_defaults_to_the_single_awaiting_pending_call():
    host = _trace_host("python-owner-resume")
    version = host.compile_admit(
        _trace_source(
            '.node("A", {type: "python_program_trace_node"})'
            '.node("B", {type: "python_program_trace_node"})'
            '.entry("A").edge("A", "B").interruptBefore("B").exit("B")'
        ),
        _budget(),
    )
    interrupted = host.run(version, {}, _budget())
    assert interrupted.status == ng.ProgramTerminalStatus.Interrupted
    call_id = interrupted.interrupt.pending_input.call_id

    # An explicit id keeps the exact-match contract.
    with pytest.raises(RuntimeError, match="exact pending call"):
        host.resume(interrupted.run_id, {"ok": True}, "not-" + call_id)

    resumed = host.resume(interrupted.run_id, {"ok": True}).wait()
    assert resumed.status == ng.ProgramTerminalStatus.Completed
    assert resumed.output["channels"]["trace"]["value"] == ["A", "B"]


def test_failed_result_keeps_the_trace_of_nodes_that_completed():
    host = _trace_host("python-owner-failed-trace")
    version = host.compile_admit(
        _trace_source(
            '.node("A", {type: "python_program_trace_node"})'
            '.node("B", {type: "python_program_trace_node"})'
            '.node("C", {type: "python_program_trace_node", fail: true})'
            '.entry("A").edge("A", "B").edge("B", "C").exit("C")'
        ),
        _budget(),
    )
    result = host.run(version, {}, _budget())
    assert result.status == ng.ProgramTerminalStatus.Failed
    assert result.failure.core_node == "C"
    assert result.execution_trace == ["A", "B"]
    parsed = ng.ProgramResult.parse(result.serialize_canonical())
    assert parsed.execution_trace == ["A", "B"]


_SPIN = "export function define() { let i = 0; while (true) { i++; } }"


def test_compile_time_javascript_stops_at_the_budget_wall_time():
    compiler = ng.ProgramCompiler(_registry(), "python-program-test/v1")
    budget = _budget()
    budget.wall_time_ms = 300
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="P_JS_TIMEOUT"):
        compiler.compile(ng.ProgramSource.from_javascript("spin.js", _SPIN), budget)
    assert time.monotonic() - started < 5


def test_program_compilers_accept_javascript_compile_limits():
    limits = ng.ProgramJavaScriptCompileLimits()
    assert limits.max_wall_time_ms == 60_000
    limits.max_wall_time_ms = 300
    source = ng.ProgramSource.from_javascript("spin.js", _SPIN)

    compiler = ng.ProgramCompiler(_registry(), "python-program-test/v1", limits)
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="P_JS_TIMEOUT"):
        compiler.compile(source)
    assert time.monotonic() - started < 5

    host = ng.LocalProgramHost(
        _registry(), "python-owner-limits", _ceiling(), "python-program-host/v1",
        javascript_limits=limits,
    )
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="P_JS_TIMEOUT"):
        host.compile(source, _budget())
    assert time.monotonic() - started < 5


def test_compile_error_reports_the_javascript_source_location():
    compiler = ng.ProgramCompiler(_registry(), "python-program-test/v1")
    source = ng.ProgramSource.from_javascript(
        "python-syntax.js",
        "export function define() {\n"
        '  return ng.graph("main")\n'
        '    .node("A", {type: "python_program_node" goal: "x"})\n'
        '    .entry("A").exit("A");\n'
        "}\n",
    )
    with pytest.raises(RuntimeError) as raised:
        compiler.compile(source)
    assert "python-syntax.js:3:" in str(raised.value)
    assert isinstance(raised.value, ng.ProgramCompileError)
    diagnostic = raised.value.diagnostics[0]
    assert diagnostic["code"] == "P_JS_EVALUATION"
    assert diagnostic["source_id"] == "python-syntax.js"
    assert diagnostic["span"]["line_begin"] == 3
    assert diagnostic["witness"]["engine"] == "quickjs"


def test_registry_miss_hints_name_the_python_registration_call():
    builder = ng.ProgramRegistryBuilder()
    with pytest.raises(RuntimeError) as condition:
        builder.add_registered_condition(
            "python_program_unregistered", "1.0.0", _DIGEST_NODE
        )
    assert "ng.ConditionRegistry.register_condition(name, fn)" in str(condition.value)
    assert "::instance()" not in str(condition.value)

    with pytest.raises(RuntimeError) as reducer:
        builder.add_registered_reducer(
            "python_program_unregistered", "1.0.0", _DIGEST_REDUCER
        )
    assert "ng.ReducerRegistry.register_reducer(name, fn)" in str(reducer.value)
    assert "::instance()" not in str(reducer.value)
