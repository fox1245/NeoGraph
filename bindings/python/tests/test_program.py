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


def test_compile_diagnostics_carry_call_sites_while_sealed_ones_do_not():
    compiler = ng.ProgramCompiler(_registry(), "python-program-test/v1")
    bundle = compiler.compile(
        ng.ProgramSource.from_javascript(
            "python-call-sites.js",
            "export function define() {\n"
            '  const graph = ng.graph("main");\n'
            '  graph.channel("value", {reducer: "overwrite", initial: 0});\n'
            '  graph.node("work", {type: "python_program_node", pattern: "\\b"});\n'
            '  graph.entry("work").exit("work");\n'
            "  return graph;\n"
            "}\n",
        )
    )
    returned = {d["code"]: d for d in bundle.compile_diagnostics}
    control = returned["P_JS_CONTROL_CHARACTER"]
    assert control["json_pointer"] == "/root/definition/nodes/work/pattern"
    assert control["span"]["line_begin"] == 4
    sealed = {d["code"]: d for d in bundle.diagnostics}
    assert sealed["P_CORE_E6"]["span"] is None
    assert "P_JS_CONTROL_CHARACTER" not in sealed
    parsed = ng.ProgramBundle.parse(bundle.serialize_canonical())
    assert parsed.id == bundle.id
    assert parsed.compile_diagnostics == bundle.diagnostics


def _stall_registry():
    _trace_host("python-owner-stall-registry")  # registers the trace node type
    ng.ConditionRegistry.register_condition("python_program_fail", lambda _s: "fail")
    ng.ConditionRegistry.register_condition("python_program_pass", lambda _s: "pass")
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node(
        "python_program_trace_node", "1.0.0", _DIGEST_TRACE_NODE
    )
    builder.add_registered_reducer("append", "1.0.0", _DIGEST_APPEND)
    builder.add_registered_condition("python_program_fail", "1.0.0", "sha256:" + "5" * 64)
    builder.add_registered_condition("python_program_pass", "1.0.0", "sha256:" + "6" * 64)
    return builder.build()


def _stall_host(owner, **options):
    return ng.LocalProgramHost(
        _stall_registry(), owner, _ceiling(), "python-program-host/v1", **options
    )


def _barrier_source(condition, fail_target, control=False):
    text = (
        'export function define() { return ng.graph("main")'
        '.channel("trace", {reducer: "append", initial: []})'
        '.node("A", {type: "python_program_trace_node"})'
        '.node("B", {type: "python_program_trace_node"})'
        '.node("C", {type: "python_program_trace_node"})'
        '.node("D", {type: "python_program_trace_node"})'
        '.node("FIX", {type: "python_program_trace_node"})'
        '.entry("A").edge("A", "B").edge("A", "C")'
        f'.conditionalEdge("B", "{condition}", {{pass: "D", fail: "{fail_target}"}})'
        '.edge("FIX", "D").edge("C", "D")'
        '.barrier("D", ["B", "C"])'
        '.exit("D"); }'
    )
    if control:
        text += '\nexport function* main(input) { return yield ng.callCore("main", input, "stall"); }\n'
    return ng.ProgramSource.from_javascript("python-barrier.js", text)


def test_program_barrier_stall_fails_instead_of_completing():
    host = _stall_host("python-owner-barrier-stall")
    version = host.compile_admit(_barrier_source("python_program_fail", "FIX"), _budget())
    result = host.run(version, {}, _budget())

    assert result.status == ng.ProgramTerminalStatus.Failed
    assert result.failure.code == "P_CORE_BARRIER_STALLED"
    assert result.failure.core_node == "D"
    stall = result.failure.witness["stalled_barriers"][0]
    assert stall["missing"] == ["B"]
    assert stall["received"] == ["C", "FIX"]
    assert result.execution_trace == ["A", "B", "C", "FIX"]


def test_program_barrier_routes_that_join_or_end_still_complete():
    host = _stall_host("python-owner-barrier-pass")
    joined = host.run(
        host.compile_admit(_barrier_source("python_program_pass", "FIX"), _budget()),
        {}, _budget())
    assert joined.status == ng.ProgramTerminalStatus.Completed
    assert joined.execution_trace[-1] == "D"

    ended = host.run(
        host.compile_admit(_barrier_source("python_program_fail", "__end__"), _budget()),
        {}, _budget())
    assert ended.status == ng.ProgramTerminalStatus.Completed


def test_core_engine_reports_and_resumes_a_barrier_stall():
    _trace_host("python-owner-core-stall")
    ng.ConditionRegistry.register_condition("python_program_fail", lambda _s: "fail")
    definition = {
        "name": "barrier_stall",
        "channels": {"trace": {"reducer": "append", "initial": []}},
        "nodes": {
            "A": {"type": "python_program_trace_node"},
            "B": {"type": "python_program_trace_node"},
            "C": {"type": "python_program_trace_node"},
            "FIX": {"type": "python_program_trace_node"},
            "D": {"type": "python_program_trace_node", "barrier": {"wait_for": ["B", "C"]}},
        },
        "edges": [
            {"from": ng.START_NODE, "to": "A"},
            {"from": "A", "to": "B"},
            {"from": "A", "to": "C"},
            {"from": "C", "to": "D"},
            {"from": "FIX", "to": "D"},
            {"from": "D", "to": ng.END_NODE},
        ],
        "conditional_edges": [
            {"from": "B", "condition": "python_program_fail",
             "routes": {"pass": "D", "fail": "FIX"}},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(), ng.InMemoryCheckpointStore())
    cfg = ng.RunConfig()
    cfg.thread_id = "python-barrier-stall"
    result = engine.run(cfg)
    assert result.status == ng.RunStatus.BarrierStalled
    assert result.barrier_stalled is True
    assert result.output["_neograph"]["barrier_stalled"][0]["missing"] == ["B"]

    resumed = engine.resume("python-barrier-stall", None)
    assert resumed.status == ng.RunStatus.BarrierStalled


def test_program_barrier_stall_fails_through_a_javascript_control_call():
    host = _stall_host("python-owner-barrier-main")
    source = _barrier_source("python_program_fail", "FIX", control=True)
    result = host.run(host.compile_admit(source, _budget()), {}, _budget())

    assert result.status == ng.ProgramTerminalStatus.Failed
    assert result.failure.code == "P_CORE_BARRIER_STALLED"
    assert result.failure.witness["stalled_barriers"][0]["missing"] == ["B"]


def test_reserved_node_names_and_missing_entry_fail_compilation():
    compiler = ng.ProgramCompiler(_registry(), "python-program-test/v1")
    reserved = ng.ProgramSource.from_javascript(
        "python-reserved.js",
        'export function define() { return ng.graph("g")'
        '.node("A", {type: "python_program_node"}).node("__end__", {type: "python_program_node"})'
        '.entry("A").edge("A", "__end__").exit("__end__"); }',
    )
    with pytest.raises(ng.ProgramCompileError) as raised:
        compiler.compile(reserved)
    assert raised.value.diagnostics[0]["code"] == "P_JS_GRAPH_ARGUMENT"
    assert "reserved" in raised.value.diagnostics[0]["message"]

    no_entry = ng.ProgramSource.from_javascript(
        "python-no-entry.js",
        'export function define() { return ng.graph("g").node("A", {type: "python_program_node"}); }',
    )
    with pytest.raises(ng.ProgramCompileError) as raised:
        compiler.compile(no_entry)
    assert raised.value.diagnostics[0]["code"] == "P_JS_GRAPH_ENTRY"


def _graph_source(body):
    return ng.ProgramSource.from_javascript(
        "python-routing.js",
        'export function define() { return ng.graph("main")'
        '.channel("trace", {reducer: "append", initial: []})'
        '.node("A", {type: "python_program_trace_node"})'
        '.node("B", {type: "python_program_trace_node"})'
        '.node("D", {type: "python_program_trace_node"})'
        '.node("X", {type: "python_program_trace_node"})' + body + "; }",
    )


def _codes(diagnostics):
    return [d["code"] for d in diagnostics]


def test_routing_lints_are_returned_but_the_seal_keeps_its_original_rules():
    bundle = ng.ProgramCompiler(_stall_registry()).compile(
        _barrier_source("python_program_fail", "FIX"))
    returned = {d["code"]: d for d in bundle.compile_diagnostics}
    bypass = returned["P_CORE_E15"]
    assert bypass["json_pointer"] == "/root/definition/nodes/D/barrier"
    assert bypass["witness"]["routes"] == {"fail": "FIX"}
    # Python node types declare no effects, so E6 cannot tell a channel is unused.
    assert "P_CORE_E6" not in returned
    sealed = _codes(bundle.diagnostics)
    assert "P_CORE_E15" not in sealed
    assert "P_CORE_E6" in sealed


def test_static_edge_shadowed_by_a_conditional_edge_fails_compilation():
    source = _graph_source(
        '.entry("A").edge("A", "B").edge("B", "D")'
        '.conditionalEdge("B", "python_program_pass", {pass: "D", fail: "X"})'
        '.edge("X", "D").exit("D")')
    with pytest.raises(ng.ProgramCompileError) as raised:
        ng.ProgramCompiler(_stall_registry()).compile(source)
    shadowed = [d for d in raised.value.diagnostics if d["code"] == "P_CORE_E13"]
    assert shadowed[0]["json_pointer"] == "/root/definition/edges/2"


def test_second_conditional_edge_from_a_node_fails_compilation():
    source = _graph_source(
        '.entry("A").edge("A", "B")'
        '.conditionalEdge("B", "python_program_pass", {pass: "D", fail: "X"})'
        '.conditionalEdge("B", "python_program_fail", {pass: "D", fail: "X"})'
        '.edge("X", "D").exit("D")')
    with pytest.raises(ng.ProgramCompileError) as raised:
        ng.ProgramCompiler(_stall_registry()).compile(source)
    assert "P_CORE_E14" in _codes(raised.value.diagnostics)


def test_require_acyclic_is_an_opt_in_compile_option():
    cyclic = _graph_source('.entry("A").edge("A", "B").edge("B", "A").edge("B", "D").exit("D")'
                           '.edge("X", "D")')
    ng.ProgramCompiler(_stall_registry()).compile(cyclic)  # loops stay valid by default
    with pytest.raises(ng.ProgramCompileError) as raised:
        ng.ProgramCompiler(_stall_registry(), require_acyclic=True).compile(cyclic)
    cycle = [d for d in raised.value.diagnostics if d["code"] == "P_CORE_E16"][0]
    assert cycle["witness"]["nodes"] == ["A", "B"]
    host = _stall_host("python-owner-acyclic", require_acyclic=True)
    with pytest.raises(ng.ProgramCompileError):
        host.compile(cyclic, _budget())


def test_program_path_keeps_lint_and_fan_out_notices_off_stderr(capfd):
    host = _trace_host("python-owner-quiet")
    version = host.compile_admit(
        _trace_source(
            '.node("A", {type: "python_program_trace_node"})'
            '.node("B", {type: "python_program_trace_node"})'
            '.node("C", {type: "python_program_trace_node"})'
            '.node("D", {type: "python_program_trace_node"})'
            '.entry("A").edge("A", "B").edge("A", "C").edge("B", "D").edge("C", "D").exit("D")'
        ),
        _budget(),
    )
    result = host.run(version, {}, _budget())
    assert result.status == ng.ProgramTerminalStatus.Completed
    err = capfd.readouterr().err
    assert "[neograph] lint" not in err
    assert "executing serially" not in err


class _SleepNode(ng.GraphNode):
    """Sleeps for the "delay" channel (ms): cooperatively, polling the cancel
    token, or blocking (a node that ignores cancellation)."""

    def __init__(self, name, config):
        super().__init__()
        self._name = name
        self._blocking = bool(config.get("blocking"))

    def get_name(self):
        return self._name

    def run(self, input):
        delay = (input.state.get("delay") or 0) / 1000.0
        if self._blocking:
            time.sleep(delay)
        else:
            token = input.ctx.cancel_token
            deadline = time.monotonic() + delay
            while time.monotonic() < deadline:
                if token is not None and token.is_cancelled():
                    raise RuntimeError("observed cancellation")
                time.sleep(0.005)
        return [ng.ChannelWrite("topic", input.state.get("topic"))]


def _join_budget():
    value = _budget()
    value.max_concurrency = 4
    return value


def _sleep_host(owner, **options):
    ng.NodeFactory.register_type(
        "python_program_sleep_node", lambda name, config, _ctx: _SleepNode(name, config))
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node("python_program_sleep_node", "1.0.0", "sha256:" + "7" * 64)
    builder.add_registered_reducer("overwrite", "1.0.0", _DIGEST_REDUCER)
    ceiling = _join_budget()
    ceiling.max_dynamic_compiles = 2
    return ng.LocalProgramHost(builder.build(), owner, ceiling, "python-program-host/v1", **options)


def _sleep_program(main_body, blocking=False):
    flag = ", blocking: true" if blocking else ""
    return ng.ProgramSource.from_javascript(
        "python-sleep.js",
        'export function define() { const g = ng.graph("work");'
        ' g.channel("delay", {reducer: "overwrite", initial: 0});'
        ' g.channel("topic", {reducer: "overwrite", initial: ""});'
        f' g.node("sleep", {{type: "python_program_sleep_node"{flag}}});'
        ' g.entry("sleep"); g.exit("sleep"); return g; }\n'
        "export function* main(input) {\n" + main_body + "\n}\n",
    )


def _timed_run(host, source):
    version = host.compile_admit(source, _join_budget())
    started = time.monotonic()
    result = host.run(version, {}, _join_budget())
    elapsed = time.monotonic() - started
    assert result.program_version_id == version.id
    assert ng.ProgramResult.parse(result.serialize_canonical()).id == result.id
    return result, elapsed


@pytest.mark.parametrize("threads", [1, 4])
def test_join_members_with_python_nodes_run_concurrently(threads):
    host = _sleep_host("python-owner-join-all", scheduler_threads=threads)
    result, elapsed = _timed_run(host, _sleep_program(
        'return (yield ng.all([ng.callCore("work", {delay: 800}, "a"),'
        ' ng.callCore("work", {delay: 800}, "b")], {max_in_flight: 2}, "fan")).length;'))
    assert result.status == ng.ProgramTerminalStatus.Completed
    assert result.output == 2
    assert result.usage.peak_concurrency == 2
    assert elapsed < 1.3, f"two 800 ms members took {elapsed:.2f}s"


@pytest.mark.parametrize("threads", [1, 4])
def test_race_settles_on_the_first_member_to_finish(threads):
    host = _sleep_host("python-owner-join-race", scheduler_threads=threads)
    result, elapsed = _timed_run(host, _sleep_program(
        'const winner = yield ng.race([ng.callCore("work", {topic: "slow", delay: 1500}, "slow"),'
        ' ng.callCore("work", {topic: "fast", delay: 20}, "fast")], {max_in_flight: 2}, "race");'
        " return winner.topic;"))
    assert result.status == ng.ProgramTerminalStatus.Completed
    assert result.output == "fast"
    assert elapsed < 1.0, f"race took {elapsed:.2f}s"


@pytest.mark.parametrize("threads", [1, 4])
def test_quorum_settles_at_the_required_successes(threads):
    host = _sleep_host("python-owner-join-quorum", scheduler_threads=threads)
    result, elapsed = _timed_run(host, _sleep_program(
        'const ok = yield ng.quorum([ng.callCore("work", {topic: "slow", delay: 1500}, "slow"),'
        ' ng.callCore("work", {topic: "f1", delay: 10}, "f1"),'
        ' ng.callCore("work", {topic: "f2", delay: 10}, "f2")],'
        ' {required_successes: 2, max_in_flight: 3}, "quorum");'
        " return ok.map((r) => r.topic);"))
    assert result.status == ng.ProgramTerminalStatus.Completed
    assert sorted(result.output) == ["f1", "f2"]
    assert elapsed < 1.0, f"quorum took {elapsed:.2f}s"


def test_await_timeout_is_enforced_with_the_default_host():
    host = _sleep_host("python-owner-await-default")  # scheduler_threads=1
    result, elapsed = _timed_run(host, _sleep_program(
        'return yield ng.await(ng.callCore("work", {delay: 3000}, "slow"), 150, "deadline");'))
    assert result.status == ng.ProgramTerminalStatus.TimedOut
    assert result.failure.code == "P_AWAIT_TIMEOUT"
    assert elapsed < 1.5, f"await(..., 150) returned after {elapsed:.2f}s"


def test_await_does_not_accept_a_completion_after_its_deadline():
    # The node ignores cancellation and finishes after the deadline.
    host = _sleep_host("python-owner-await-late")
    result, _ = _timed_run(host, _sleep_program(
        'return yield ng.await(ng.callCore("work", {delay: 400}, "late"), 100, "deadline");',
        blocking=True))
    assert result.status == ng.ProgramTerminalStatus.TimedOut
    assert result.failure.code == "P_AWAIT_TIMEOUT"


def _simple_trace_source():
    return _trace_source('.node("A", {type: "python_program_trace_node"}).entry("A").exit("A")')


def test_admission_errors_are_typed_and_carry_diagnostics():
    host = _trace_host("python-owner-admission-error")
    above_ceiling = _budget()
    above_ceiling.wall_time_ms = 20_000  # the host ceiling is 10 000 ms
    with pytest.raises(RuntimeError) as raised:
        host.compile_admit(_simple_trace_source(), above_ceiling)
    error = raised.value
    assert isinstance(error, ng.ProgramAdmissionError)
    assert error.diagnostics
    assert all(d["code"].startswith("P_ADMIT_") for d in error.diagnostics)
    assert error.diagnostics[0]["code"] in str(error)
    assert "wall_time_ms maximum 20000 exceeds the policy ceiling 10000" in str(error)


def test_runtime_diagnostic_errors_are_typed_and_name_the_bound():
    host = _trace_host("python-owner-runtime-error")
    version = host.compile_admit(_simple_trace_source(), _budget())
    other = _budget()
    other.wall_time_ms = 9_000  # compile_admit pinned 10 000 ms exactly
    with pytest.raises(RuntimeError) as raised:
        host.run(version, {}, other)
    error = raised.value
    assert isinstance(error, ng.ProgramDiagnosticError)
    diagnostic = error.diagnostics[0]
    assert diagnostic["code"] == "P_START_BUDGET"
    assert diagnostic["witness"]["resource"] == "wall_time_ms"
    assert diagnostic["witness"]["granted"] == 9_000
    message = str(error)
    assert "P_START_BUDGET" in message
    assert "wall_time_ms" in message and "9000" in message and "10000" in message


def test_budget_ceiling_errors_name_the_field():
    ceiling = _ceiling()
    ceiling.model_tokens = 0
    with pytest.raises(ValueError, match="model_tokens"):
        ng.LocalProgramHost(_registry(), "python-owner-zero-ceiling", ceiling)


_LLM_GRAPH = {
    "name": "no_provider",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}},
    "edges": [{"from": ng.START_NODE, "to": "llm"}, {"from": "llm", "to": ng.END_NODE}],
}


def test_llm_call_without_provider_fails_the_core_run_cleanly():
    engine = ng.GraphEngine.compile(_LLM_GRAPH, ng.NodeContext())
    with pytest.raises(Exception, match="has no provider"):
        engine.run(ng.RunConfig())


def test_llm_call_without_provider_is_rejected_at_program_admission():
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node("llm_call", "1.0.0", "sha256:" + "8" * 64)
    builder.add_registered_reducer("append", "1.0.0", _DIGEST_APPEND)
    host = ng.LocalProgramHost(builder.build(), "python-owner-llm", _ceiling())
    source = ng.ProgramSource.from_javascript(
        "python-llm.js",
        'export function define() { return ng.graph("g")'
        '.channel("messages", {reducer: "append", initial: []})'
        '.node("llm", {type: "llm_call"}).entry("llm").exit("llm"); }',
    )
    with pytest.raises(ng.ProgramAdmissionError) as raised:
        host.compile_admit(source, _budget())
    provider = [d for d in raised.value.diagnostics if d["code"] == "P_ADMIT_PROVIDER"]
    assert provider and provider[0]["witness"]["node"] == "llm"


def test_program_barrier_stall_is_reported_after_an_implicit_end_elsewhere():
    host = _stall_host("python-owner-barrier-implicit-end")
    # X has no outgoing edges: it ends implicitly while D still waits for B.
    source = ng.ProgramSource.from_javascript(
        "python-barrier-implicit-end.js",
        'export function define() { return ng.graph("main")'
        '.channel("trace", {reducer: "append", initial: []})'
        '.node("A", {type: "python_program_trace_node"})'
        '.node("B", {type: "python_program_trace_node"})'
        '.node("C", {type: "python_program_trace_node"})'
        '.node("D", {type: "python_program_trace_node"})'
        '.node("FIX", {type: "python_program_trace_node"})'
        '.node("X", {type: "python_program_trace_node"})'
        '.entry("A").edge("A", "B").edge("A", "C").edge("A", "X")'
        '.conditionalEdge("B", "python_program_fail", {pass: "D", fail: "FIX"})'
        '.edge("FIX", "D").edge("C", "D").barrier("D", ["B", "C"]).exit("D"); }',
    )
    result = host.run(host.compile_admit(source, _budget()), {}, _budget())
    assert result.status == ng.ProgramTerminalStatus.Failed
    assert result.failure.code == "P_CORE_BARRIER_STALLED"
