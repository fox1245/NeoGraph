"""Python tools follow the same failure contract as C++ tools.

Run control signals stop the run and never become a tool message: a
``NodeInterrupt`` raised by a tool pauses the run, a ``HookBoundaryBlocked``
raised by a tool (for example from a nested engine whose mandatory Hook
blocked) fails it with its invocation id and state, and cancelling the run's
token cancels it. Every other exception becomes a tool result with a typed
status that the model reads. Each case runs for one call and for a batch, and
for both ``Tool`` (serial) and ``AsyncTool`` (offloaded) subclasses. What the
provider reads of a Python tool's failure is its exception type and message,
bounded, and never its traceback (#545).
"""

import json
import logging
import pathlib
import time
import traceback

import neograph_engine as ng
import pytest


_uid = 0


def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


class _EmitCalls(ng.GraphNode):
    def __init__(self, name, calls, delay=0.0):
        super().__init__()
        self._name = name
        self._calls = calls
        self._delay = delay

    def get_name(self):
        return self._name

    def run(self, _input):
        if self._delay:
            time.sleep(self._delay)
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": "",
            "tool_calls": self._calls,
        }])]


def _tool(base, name, body):
    class _Probe(base):
        def get_name(self):
            return name

        def get_definition(self):
            return ng.ChatTool(name=name, description=name,
                               parameters={"type": "object", "properties": {}})

        def execute(self, arguments):
            return body(arguments)

    return _Probe()


def _run(tools, batch, call="target", token=None, hooks=None, metadata=None, delay=0.0):
    emit = _next_type("contract_emit")
    calls = [{"id": "call-target", "name": call, "arguments": "{}"}]
    if batch:
        calls.append({"id": "call-sibling", "name": "sibling", "arguments": "{}"})
    ng.NodeFactory.register_type(emit, lambda name, _config, _ctx: _EmitCalls(name, calls, delay))
    definition = {
        "name": "python_tool_contract",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"model": {"type": emit}, "tools": {"type": "tool_dispatch"}},
        "edges": [
            {"from": ng.START_NODE, "to": "model"},
            {"from": "model", "to": "tools"},
            {"from": "tools", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=tools))
    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    if hooks is not None:
        engine.set_hook_runtime(hooks)
    config = ng.RunConfig(thread_id=_next_type("contract_thread"), input={"messages": []})
    if token is not None:
        config.cancel_token = token
    if metadata is None:
        metadata = ng.RunMetadata(run_id="contract-run", owner_scope="contract-owner")
    return engine.run(config, metadata)


def _seen(result, call_id="call-target"):
    for message in result.output["channels"]["messages"]["value"]:
        if message.get("role") == "tool" and message.get("tool_call_id") == call_id:
            return message
    return None


def _sibling():
    return _tool(ng.Tool, "sibling", lambda _args: "sibling ok")


BASES = [pytest.param(ng.Tool, id="serial"), pytest.param(ng.AsyncTool, id="async")]
BATCH = [pytest.param(False, id="single"), pytest.param(True, id="batch")]


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_python_exception_is_a_typed_tool_result(base, batch):
    def fail(_args):
        raise ValueError("bad input from the model")

    result = _run([_tool(base, "target", fail), _sibling()], batch)
    seen = _seen(result)
    assert seen is not None
    # The default execution policy cannot rule out an effect of a tool that
    # had started, so the model is told the outcome needs reconciling.
    assert seen["tool_status"] == "reconciliation_required"
    body = json.loads(seen["content"])
    assert "bad input from the model" in body["error"]
    assert body["status"] == "reconciliation_required"
    if batch:
        assert _seen(result, "call-sibling")["content"] == "sibling ok"


@pytest.mark.parametrize("batch", BATCH)
def test_unknown_tool_is_a_rejected_tool_result(batch):
    result = _run([_sibling()], batch, call="missing")
    seen = _seen(result)
    assert seen["tool_status"] == "rejected"
    assert json.loads(seen["content"])["error"] == "Tool not found: missing"


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_node_interrupt_from_a_tool_pauses_the_run(base, batch):
    def ask(_args):
        raise ng.NodeInterrupt({"cmd": "deploy"}, reason="target needs approval")

    result = _run([_tool(base, "target", ask), _sibling()], batch)
    assert result.interrupted, "the interrupt became a tool message the model could ignore"
    assert result.interrupt_node == "tools"
    assert result.interrupt_value["reason"] == "target needs approval"
    assert result.interrupt_value["value"] == {"cmd": "deploy"}


def _nested_engine_with_blocking_hook():
    """An inner engine whose FailClosed checkpoint Hook fails."""
    class _Write(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, _input):
            return [ng.ChannelWrite("value", "done")]

    node_type = _next_type("contract_inner")
    ng.NodeFactory.register_type(node_type, lambda name, _c, _x: _Write(name))
    inner = ng.GraphEngine.compile({
        "name": "inner",
        "channels": {"value": {"reducer": "overwrite"}},
        "nodes": {"work": {"type": node_type}},
        "edges": [{"from": ng.START_NODE, "to": "work"},
                  {"from": "work", "to": ng.END_NODE}],
    }, ng.NodeContext())
    inner.set_checkpoint_store(ng.InMemoryCheckpointStore())

    data = ng.HookDefinitionData()
    data.phase = ng.HookPhase.CheckpointPublished
    data.target_id = "audit"
    data.delivery = ng.HookDelivery.BlockingMandatory
    data.failure_mode = ng.HookFailureMode.FailClosed
    data.idempotency = ng.HookIdempotency.Idempotent
    data.effect = ng.ToolEffectClass.ReadOnly
    mapper = ng.HookInputMapper()
    mapper.kind = ng.HookInputMapperKind.Template
    mapper.value_template = {"kind": "checkpoint"}
    data.input_mapper = mapper

    def refuse(*_args):
        raise RuntimeError("audit sink unavailable")

    inner.set_hook_runtime(ng.create_hook_runtime(
        [ng.HookDefinition.create(data)], {"audit": refuse}))
    return inner


def test_hook_boundary_blocked_carries_invocation_id_and_state():
    inner = _nested_engine_with_blocking_hook()
    with pytest.raises(ng.HookBoundaryBlocked) as raised:
        inner.run(ng.RunConfig(thread_id=_next_type("inner")),
                  ng.RunMetadata(run_id="inner-run", owner_scope="owner"))
    assert raised.value.invocation_id.startswith("sha256:")
    # A failed attempt with none left is failed; only an elapsed deadline is
    # timed_out.
    assert raised.value.state == "failed"


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_hook_block_from_a_tool_stops_the_run(base, batch):
    inner = _nested_engine_with_blocking_hook()

    def sub_agent(_args):
        # A tool that runs its own engine: the inner Hook block escapes it.
        inner.run(ng.RunConfig(thread_id=_next_type("inner")),
                  ng.RunMetadata(run_id="inner-run", owner_scope="owner"))
        return "unreachable"

    with pytest.raises(ng.HookBoundaryBlocked) as raised:
        _run([_tool(base, "target", sub_agent), _sibling()], batch)
    assert raised.value.invocation_id.startswith("sha256:")
    # A failed attempt with none left is failed; only an elapsed deadline is
    # timed_out.
    assert raised.value.state == "failed"


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_cancelling_the_run_from_a_tool_cancels_the_run(base, batch):
    token = ng.CancelToken()

    def cancel(_args):
        token.cancel()
        return "finished after the cancel"

    with pytest.raises(RuntimeError, match="run cancelled"):
        _run([_tool(base, "target", cancel), _sibling()], batch, token=token)


# ── The engine's tool Hooks reach every graph tool call ─────────────────────

def _tool_hook(phase, target):
    data = ng.HookDefinitionData()
    data.phase = phase
    data.target_id = target
    data.delivery = ng.HookDelivery.BlockingMandatory
    data.failure_mode = ng.HookFailureMode.FailClosed
    data.idempotency = ng.HookIdempotency.Idempotent
    data.effect = ng.ToolEffectClass.ReadOnly
    mapper = ng.HookInputMapper()
    mapper.kind = ng.HookInputMapperKind.Template
    mapper.value_template = {"tool": {"$event": "/tool_name"},
                             "call": {"$event": "/tool_call_id"}}
    data.input_mapper = mapper
    return ng.HookDefinition.create(data)


@pytest.mark.parametrize("batch", BATCH)
@pytest.mark.parametrize("base", BASES)
def test_engine_tool_hooks_run_for_graph_tool_calls(base, batch):
    seen = []

    def audit(arguments, event_type, _event_data):
        seen.append((event_type, arguments["tool"], arguments["call"]))

    hooks = ng.create_hook_runtime(
        [_tool_hook(ng.HookPhase.BeforeToolExecution, "audit"),
         _tool_hook(ng.HookPhase.AfterToolExecution, "audit")], {"audit": audit})
    result = _run([_tool(base, "target", lambda _a: "target ok"), _sibling()], batch,
                  hooks=hooks)
    assert _seen(result)["content"] == "target ok"
    calls = ["call-target"] + (["call-sibling"] if batch else [])
    for call in calls:
        assert sum(1 for item in seen if item[2] == call) == 2, seen
    assert ("tool_execution", "target", "call-target") in seen


@pytest.mark.parametrize("batch", BATCH)
def test_a_failing_tool_hook_blocks_the_graph_call_before_it_runs(batch):
    ran = []

    def refuse(*_args):
        raise RuntimeError("audit sink unavailable")

    hooks = ng.create_hook_runtime(
        [_tool_hook(ng.HookPhase.BeforeToolExecution, "audit")], {"audit": refuse})
    with pytest.raises(ng.HookBoundaryBlocked) as raised:
        _run([_tool(ng.Tool, "target", lambda _a: ran.append(1) or "ran"), _sibling()],
             batch, hooks=hooks)
    assert raised.value.state == "failed"
    assert raised.value.invocation_id.startswith("sha256:")
    assert not ran


@pytest.mark.parametrize("batch", BATCH)
def test_no_tool_starts_after_the_run_deadline(batch):
    ran = []
    result = _run([_tool(ng.Tool, "target", lambda _a: ran.append(1) or "ran"), _sibling()],
                  batch, metadata=ng.RunMetadata(timeout_ms=50, run_id="late",
                                                 owner_scope="contract-owner"),
                  delay=0.2)
    seen = _seen(result)
    assert seen["tool_status"] == "expired"
    assert json.loads(seen["content"])["error"] == \
        "run deadline elapsed before the tool started"
    assert not ran


def test_a_hook_block_from_a_python_node_stops_the_run_unretried():
    inner = _nested_engine_with_blocking_hook()
    attempts = []

    class _Nested(ng.GraphNode):
        def get_name(self):
            return "nested"

        def run(self, _input):
            attempts.append(1)
            inner.run(ng.RunConfig(thread_id=_next_type("inner")),
                      ng.RunMetadata(run_id="inner-run", owner_scope="owner"))
            return []

    node_type = _next_type("contract_nested_node")
    ng.NodeFactory.register_type(node_type, lambda _n, _c, _x: _Nested())
    engine = ng.GraphEngine.compile({
        "name": "nested_hook_block",
        "channels": {"value": {"reducer": "overwrite"}},
        "nodes": {"nested": {"type": node_type}},
        "edges": [{"from": ng.START_NODE, "to": "nested"},
                  {"from": "nested", "to": ng.END_NODE}],
    }, ng.NodeContext())
    policy = ng.RetryPolicy()
    policy.max_retries = 2
    policy.initial_delay_ms = 1
    engine.set_node_retry_policy("nested", policy)
    with pytest.raises(ng.HookBoundaryBlocked) as raised:
        engine.run(ng.RunConfig(thread_id=_next_type("outer")))
    assert raised.value.state == "failed"
    assert len(attempts) == 1, "a Hook block from a Python node was retried"


# ── What the provider reads of a failed Python tool (#545) ───────────────────

TOOL_LOGGER = "neograph_engine.tools"

# A real model loop: the first answer asks for the tool, the second ends the turn.
AGENT = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "python_tool_error_text",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}, "tools": {"type": "tool_dispatch"}},
    "edges": [{"from": ng.START_NODE, "to": "llm"}, {"from": "tools", "to": "llm"}],
    "conditional_edges": [{"from": "llm", "condition": "has_tool_calls",
                           "routes": {"true": "tools", "false": ng.END_NODE}}],
}


def _agent_reply(request):
    body = request["body"]
    if any(message.get("role") == "tool" for message in body["messages"]):
        message, finish = {"role": "assistant", "content": "done"}, "stop"
    else:
        message, finish = {"role": "assistant", "content": None, "tool_calls": [
            {"id": "call-target", "type": "function",
             "function": {"name": "target", "arguments": "{}"}}]}, "tool_calls"
    return 200, {"id": "chat-local", "object": "chat.completion", "model": body["model"],
                 "created": 1, "usage": {"prompt_tokens": 2, "completion_tokens": 3, "total_tokens": 5},
                 "choices": [{"index": 0, "message": message, "finish_reason": finish}]}, \
        "application/json"


def _provider_read(typed_peer, tool, policy=None):
    """Run the model loop over a local provider.

    Returns the content of the tool message in the provider's second request, and that
    request's raw body: everything the provider was sent.
    """
    typed_peer.responder = _agent_reply
    context = ng.NodeContext(provider=typed_peer.provider("openai.chat"), model="fixture-model",
                             tools=[tool])
    engine = ng.GraphEngine.compile(AGENT, context)
    if policy is not None:
        policies = ng.ToolExecutionPolicyRegistry()
        policies.upsert("target", policy)
        engine.set_tool_execution_controller(ng.ToolExecutionController(policies))
    result = engine.run(ng.RunConfig(thread_id=_next_type("provider_read"),
                                     input={"messages": [{"role": "user", "content": "go"}]}))
    assert result.status == ng.RunStatus.Completed
    _asked, answered = typed_peer.requests
    [message] = [m for m in answered["body"]["messages"] if m.get("role") == "tool"]
    return message["content"], answered["raw_body"].decode()


class _LedgerBusy(Exception):
    pass


def _load_account():
    raise ValueError("bad input from the model")


def _call_through_helpers():
    _load_account()


def _chained():
    try:
        raise KeyError("account")
    except KeyError as error:
        raise RuntimeError("could not load the ledger") from error


def _custom():
    raise _LedgerBusy("the ledger is busy; call again")


def _without_message():
    raise LookupError()


FAILURES = [
    pytest.param(_call_through_helpers, "ValueError: bad input from the model", id="plain"),
    pytest.param(_chained, "RuntimeError: could not load the ledger", id="chained"),
    pytest.param(_custom, "_LedgerBusy: the ledger is busy; call again", id="custom-class"),
    pytest.param(_without_message, "LookupError", id="no-message"),
]


@pytest.mark.parametrize("raiser, expected", FAILURES)
@pytest.mark.parametrize("base", BASES)
def test_the_provider_reads_the_exception_type_and_message_not_the_traceback(
        typed_peer, base, raiser, expected):
    content, request = _provider_read(typed_peer, _tool(base, "target", lambda _a: raiser()))
    error = json.loads(content)
    assert error["error"] == expected
    assert error["status"] == "reconciliation_required"
    # Nothing of the traceback reached the provider: not its frames (functions, files)
    # and not a path of the host.
    here = pathlib.Path(__file__).resolve()
    for leaked in ("Traceback", "At:", 'File "', "<lambda>", raiser.__name__, ".py",
                   here.name, str(here.parent)):
        assert leaked not in request, f"{leaked!r} reached the provider: {request}"


@pytest.mark.parametrize("base", BASES)
def test_the_traceback_stays_with_the_host_in_the_log(typed_peer, caplog, base):
    with caplog.at_level(logging.WARNING, logger=TOOL_LOGGER):
        _provider_read(typed_peer, _tool(base, "target", lambda _a: _call_through_helpers()))
    [record] = [r for r in caplog.records if r.name == TOOL_LOGGER]
    assert record.levelno == logging.WARNING
    assert "target" in record.getMessage()
    assert "ValueError: bad input from the model" in record.getMessage()
    assert record.exc_info[0] is ValueError
    written = "".join(traceback.format_exception(*record.exc_info))
    assert "Traceback" in written and "_load_account" in written and "_call_through_helpers" in written
    assert pathlib.Path(__file__).name in written


def test_a_tool_that_succeeds_logs_nothing(typed_peer, caplog):
    with caplog.at_level(logging.DEBUG, logger=TOOL_LOGGER):
        content, _ = _provider_read(typed_peer, _tool(ng.Tool, "target", lambda _a: "target ok"))
    assert content == "target ok"
    assert not [r for r in caplog.records if r.name == TOOL_LOGGER]


@pytest.mark.parametrize("base", BASES)
def test_a_long_message_is_cut_to_the_default_limit(typed_peer, base):
    limit = ng.ToolExecutionPolicy().error_limit_bytes

    def fail(_args):
        raise ValueError("x" * (50 * limit))

    content, _ = _provider_read(typed_peer, _tool(base, "target", fail))
    error = json.loads(content)
    assert len(error["error"].encode()) == limit
    assert error["error"].startswith("ValueError: xxxx")
    assert error["error"].endswith("[truncated]")


def test_the_policy_sets_the_limit_and_the_cut_keeps_whole_characters(typed_peer):
    def fail(_args):
        raise ValueError("\u00e9" * 5000)

    policy = ng.ToolExecutionPolicy()
    policy.error_limit_bytes = 64
    content, _ = _provider_read(typed_peer, _tool(ng.Tool, "target", fail), policy)
    error = json.loads(content)
    sent = error["error"].encode()
    assert 63 <= len(sent) <= 64
    assert error["error"].startswith("ValueError: \u00e9\u00e9")
    assert error["error"].endswith("[truncated]")
    assert "\ufffd" not in error["error"], "a character was split"


def test_an_error_limit_outside_its_range_is_refused():
    policy = ng.ToolExecutionPolicy()
    assert policy.error_limit_bytes == 2048
    for refused in (0, 1024 * 1024 + 1):
        policy.error_limit_bytes = refused
        with pytest.raises(ValueError, match="error_limit_bytes"):
            policy.validate()
    policy.error_limit_bytes = 1
    policy.validate()


# ── Text crosses the Python boundary as UTF-8, U+FFFD for ill-formed bytes ──

@pytest.mark.parametrize("result, expected", [
    pytest.param("ok \udcff end", "ok \ufffd end", id="lone-surrogate-str"),
    pytest.param(b"ok \xff end", "ok \ufffd end", id="ill-formed-bytes"),
])
def test_ill_formed_tool_text_becomes_replacement_characters(result, expected):
    seen = _seen(_run([_tool(ng.Tool, "target", lambda _a: result)], False))
    assert seen["tool_status"] == "succeeded"
    assert seen["content"] == expected


def test_ill_formed_message_text_reads_back_with_replacement_characters():
    message = ng.ChatMessage()
    message.content = b"caf\xc3 \xff"
    assert message.content == "caf\ufffd \ufffd"
