"""Regression tests for the two quickstart code blocks in README.md.

Both blocks ship verbatim in the README — broken samples here mean a
first-time user copy-pastes from the README and gets nothing. v0.1.7
silently shipped a broken ReAct quickstart (the top-level
``conditional_edges`` block was dropped by graph_compiler) for four
patches in a row because the wheel test-command only smoke-imported the
module. These tests close that gap.

If you change the README quickstart code blocks, update the verbatim
copies here too.
"""
from __future__ import annotations

import pytest

import neograph_engine as ng


# --------------------------------------------------------------------- #
# README §"Five-second demo (no API key)"
# --------------------------------------------------------------------- #


def test_readme_five_second_demo_runs_and_produces_documented_output():
    """The decorator + ChannelWrite + RunResult.output trio.

    Proves: ``@ng.node`` register / NodeContext default / GraphEngine.compile /
    engine.run / RunResult.output["channels"][...]["value"] all behave the
    way the README documents.
    """
    @ng.node("greet")
    def greet(state):
        return [
            ng.ChannelWrite(
                "messages",
                [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}],
            )
        ]

    definition = {
        "name": "demo",
        "channels": {
            "name": {"reducer": "overwrite"},
            "messages": {"reducer": "append"},
        },
        "nodes": {"greet": {"type": "greet"}},
        "edges": [
            {"from": ng.START_NODE, "to": "greet"},
            {"from": "greet", "to": ng.END_NODE},
        ],
    }

    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))

    msgs = result.output["channels"]["messages"]["value"]
    assert msgs == [{"role": "assistant", "content": "Hello, NeoGraph!"}]


# --------------------------------------------------------------------- #
# README §"ReAct agent with a real LLM"
# --------------------------------------------------------------------- #
#
# We can't hit OpenAI from cibw, so we drop in an offline @ng.node
# replacement that emits the same assistant-with-tool_calls shape a
# real LLM would. The compiler / scheduler / has_tool_calls condition /
# tool_dispatch path is exactly the same — only the LLM call is
# stubbed. This is the same reproducer that found the v0.1.7 silent
# drop of top-level conditional_edges.


@pytest.fixture
def offline_react_graph():
    """Build the README ReAct graph with a fake LLM emitting one tool_call."""

    @ng.node("fake_llm_with_tool_call")
    def fake_llm(state):
        return [
            ng.ChannelWrite(
                "messages",
                [{
                    "role": "assistant",
                    "content": "",
                    "tool_calls": [{
                        "id": "call_OFFLINE_REGRESSION",
                        "name": "calc",
                        "arguments": '{"x": 21}',
                    }],
                }],
            )
        ]

    class CalcTool(ng.Tool):
        def get_name(self):
            return "calc"

        def get_definition(self):
            return ng.ChatTool(
                name="calc",
                description="multiply by 2",
                parameters={
                    "type": "object",
                    "properties": {"x": {"type": "number"}},
                },
            )

        def execute(self, arguments):
            return str(arguments["x"] * 2)

    ctx = ng.NodeContext(tools=[CalcTool()])

    definition = {
        "name": "react",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
            "llm": {"type": "fake_llm_with_tool_call"},
            "dispatch": {"type": "tool_dispatch"},
        },
        # The README ReAct quickstart uses the top-level
        # `conditional_edges` block. graph_compiler dropped this
        # silently in v0.1.0~0.1.7 — fix in v0.1.8 (commit e23a523).
        "edges": [
            {"from": ng.START_NODE, "to": "llm"},
            {"from": "dispatch", "to": ng.END_NODE},  # short-circuit
        ],
        "conditional_edges": [
            {
                "from": "llm",
                "condition": "has_tool_calls",
                "routes": {"true": "dispatch", "false": ng.END_NODE},
            }
        ],
    }

    return ng.GraphEngine.compile(definition, ctx)


def test_readme_react_top_level_conditional_edges_route_to_dispatch(
    offline_react_graph,
):
    """The trace must include `dispatch` — i.e., the conditional fired.

    Pre-v0.1.8 fail mode: trace == ['llm'] (compiler dropped the
    conditional_edges block, so llm fell through to __end__).
    """
    result = offline_react_graph.run(
        ng.RunConfig(
            thread_id="t1",
            input={"messages": [{"role": "user", "content": "What is 21 * 2?"}]},
            max_steps=10,
        )
    )

    assert "dispatch" in result.execution_trace, (
        f"top-level conditional_edges block was silently dropped — "
        f"the README ReAct quickstart will not work for users. "
        f"trace={result.execution_trace}"
    )


def test_readme_react_tool_dispatch_appends_tool_result(offline_react_graph):
    """The dispatch node must execute the tool and append its result."""
    result = offline_react_graph.run(
        ng.RunConfig(
            thread_id="t1",
            input={"messages": [{"role": "user", "content": "What is 21 * 2?"}]},
            max_steps=10,
        )
    )

    msgs = result.output["channels"]["messages"]["value"]
    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    assert tool_msgs, f"no tool result in messages: {msgs}"
    assert tool_msgs[0]["content"] == "42", tool_msgs[0]


# --------------------------------------------------------------------- #
# README §"Reading the output" — RunResult surface
# --------------------------------------------------------------------- #


def test_runresult_documented_attributes_exist():
    """Every RunResult attribute the README's table promises must exist.

    If a future refactor renames one of these, the README docs go
    stale silently — until a user follows the table and gets
    AttributeError. Lock it down.
    """

    @ng.node("noop")
    def noop(state):
        return []

    definition = {
        "name": "n",
        "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {"noop": {"type": "noop"}},
        "edges": [
            {"from": ng.START_NODE, "to": "noop"},
            {"from": "noop", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1", input={"x": 1}))

    documented_attrs = [
        "output",
        "interrupted",
        "interrupt_node",
        "interrupt_value",
        "checkpoint_id",
        "execution_trace",
    ]
    for attr in documented_attrs:
        assert hasattr(result, attr), (
            f"RunResult missing documented attribute: {attr}. "
            f"README §'Reading the output' will break."
        )


def test_runconfig_documented_fields_accept_documented_types():
    """RunConfig fields the README tabulates must accept the documented types."""
    cfg = ng.RunConfig(
        thread_id="t1",
        input={"x": 1},
        max_steps=10,
        stream_mode=ng.StreamMode.EVENTS,
    )
    assert cfg.thread_id == "t1"
    assert cfg.input == {"x": 1}
    assert cfg.max_steps == 10
    # StreamMode is an IntEnum — equality with the original value.
    assert cfg.stream_mode == ng.StreamMode.EVENTS


# --------------------------------------------------------------------- #
# README §"Built-in reducers" — the catalog must match reality
# --------------------------------------------------------------------- #


@pytest.mark.parametrize("reducer_name", ["overwrite", "append"])
def test_documented_reducers_compile(reducer_name):
    """Both reducer names the README documents must work in compile()."""
    @ng.node("noop")
    def noop(state):
        return []

    initial = [] if reducer_name == "append" else 0
    definition = {
        "name": "r",
        "channels": {"ch": {"reducer": reducer_name}},
        "nodes": {"noop": {"type": "noop"}},
        "edges": [
            {"from": ng.START_NODE, "to": "noop"},
            {"from": "noop", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1", input={"ch": initial}))
    assert result.output is not None  # compile + run succeeded


def test_undocumented_reducer_raises_clearly():
    """If someone guesses 'last_value' (LangGraph alias) we want a
    clear error — not silent fallthrough."""
    @ng.node("noop")
    def noop(state):
        return []

    definition = {
        "name": "r",
        "channels": {"ch": {"reducer": "last_value"}},  # not registered
        "nodes": {"noop": {"type": "noop"}},
        "edges": [
            {"from": ng.START_NODE, "to": "noop"},
            {"from": "noop", "to": ng.END_NODE},
        ],
    }
    with pytest.raises(RuntimeError, match=r"[Uu]nknown reducer"):
        engine = ng.GraphEngine.compile(definition, ng.NodeContext())
        engine.run(ng.RunConfig(thread_id="t1", input={"ch": 0}))
