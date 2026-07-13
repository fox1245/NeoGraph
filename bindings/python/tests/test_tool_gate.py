"""The tool gate reaches Python (issue #89).

A gate that only exists in C++ is half a feature — the permission prompt is the
reason anyone wants this, and most people building an agent on top of NeoGraph
are writing Python.

The gate is consulted for every tool call BEFORE any tool runs. That ordering is
the design, not a tidiness preference: a gate asked after execution would let
the siblings of an approval-pending call take effect, and the resume — which
re-enters the node from the top — would then run them a second time. Approving
an `rm -rf` would double-commit the `git commit` beside it.
"""

import neograph_engine as ng
import pytest


class _SpyTool(ng.Tool):
    """Records that it ran, and with what."""

    def __init__(self, name):
        super().__init__()
        self._name = name
        self.runs = 0
        self.last_args = None

    def get_definition(self):
        definition = ng.ChatTool()
        definition.name = self._name
        definition.description = "spy"
        return definition

    def execute(self, arguments):
        self.runs += 1
        self.last_args = arguments
        return '{"ok": true}'

    def get_name(self):
        return self._name


class _ToolCallingNode(ng.GraphNode):
    """Stands in for a model that asked for two tools in one turn."""

    def run(self, _input):
        assistant = {
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {"id": "1", "name": "read", "arguments": "{}"},
                {"id": "2", "name": "shell", "arguments": "{}"},
            ],
        }
        return [ng.ChannelWrite("messages", [assistant])]

    def get_name(self):
        return "llm"


DEFINITION = {
    "name": "py_tool_gate",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "tg_llm"}, "tools": {"type": "tool_dispatch"}},
    "edges": [
        {"from": "__start__", "to": "llm"},
        {"from": "llm", "to": "tools"},
        {"from": "tools", "to": "__end__"},
    ],
}


@pytest.fixture
def agent():
    ng.NodeFactory.register_type("tg_llm", lambda _n, _c, _x: _ToolCallingNode())
    read, shell = _SpyTool("read"), _SpyTool("shell")
    ctx = ng.NodeContext(tools=[read, shell])
    engine = ng.GraphEngine.compile(
        DEFINITION, ctx, ng.InMemoryCheckpointStore())
    return engine, read, shell


def _messages(result):
    return result.output["channels"]["messages"]["value"]


def test_no_gate_means_every_tool_runs(agent):
    """The anchor: every existing graph sets no gate and must be unaffected."""
    engine, read, shell = agent

    cfg = ng.RunConfig()
    cfg.thread_id = "tg-none"
    engine.run(cfg)

    assert read.runs == 1
    assert shell.runs == 1


def test_deny_stops_the_tool_and_tells_the_model(agent):
    """A denial is a result, not silence — otherwise the model just asks again."""
    engine, read, shell = agent

    def gate(call, _gctx):
        if call.name == "shell":
            return ng.ToolDecision.deny("shell is disabled in this workspace")
        return ng.ToolDecision.allow()

    cfg = ng.RunConfig()
    cfg.thread_id = "tg-deny"
    engine.set_tool_gate(gate)
    result = engine.run(cfg)

    assert shell.runs == 0, "a denied tool must not execute"
    assert read.runs == 1, "denying one call must not block the others"

    denied = [m for m in _messages(result)
              if m.get("role") == "tool" and m.get("tool_name") == "shell"]
    assert denied, "the denial never reached the message history"
    assert "shell is disabled" in denied[0]["content"]


def test_allow_can_rewrite_the_arguments(agent):
    """Ambient values get injected here, not in every tool."""
    engine, read, _shell = agent

    def gate(call, _gctx):
        if call.name == "read":
            return ng.ToolDecision.allow({"tenant_id": "acme"})
        return ng.ToolDecision.allow()

    cfg = ng.RunConfig()
    cfg.thread_id = "tg-rewrite"
    engine.set_tool_gate(gate)
    engine.run(cfg)

    assert read.runs == 1
    assert read.last_args == {"tenant_id": "acme"}


def test_interrupt_pauses_before_any_tool_runs(agent):
    """The load-bearing case. Neither tool may have run when we stop."""
    engine, read, shell = agent

    def gate(call, _gctx):
        if call.name == "shell":
            return ng.ToolDecision.interrupt(
                "shell needs approval", {"cmd": "rm -rf build/"})
        return ng.ToolDecision.allow()

    cfg = ng.RunConfig()
    cfg.thread_id = "tg-interrupt"
    engine.set_tool_gate(gate)
    paused = engine.run(cfg)

    assert paused.interrupted
    assert paused.interrupt_node == "tools"
    assert paused.interrupt_value["reason"] == "shell needs approval"
    assert paused.interrupt_value["value"] == {"cmd": "rm -rf build/"}
    assert read.runs == 0, "the harmless tool ran while we were still asking"
    assert shell.runs == 0


def test_resume_after_approval_runs_each_tool_exactly_once(agent):
    """Twice would mean the approval double-applied the siblings' effects."""
    engine, read, shell = agent

    def gate(call, gctx):
        if call.name != "shell":
            return ng.ToolDecision.allow()
        if gctx.resume_value is None:
            return ng.ToolDecision.interrupt("shell needs approval")
        if gctx.resume_value.get("approved"):
            return ng.ToolDecision.allow()
        return ng.ToolDecision.deny("the human said no")

    cfg = ng.RunConfig()
    cfg.thread_id = "tg-resume"
    engine.set_tool_gate(gate)

    assert engine.run(cfg).interrupted
    assert read.runs == 0 and shell.runs == 0

    done = engine.resume("tg-resume", {"approved": True})

    assert not done.interrupted
    assert read.runs == 1, "the harmless tool ran twice — effects double-applied"
    assert shell.runs == 1


def test_a_refusal_is_an_answer_too(agent):
    """The gate branches on the content of the decision, not its arrival."""
    engine, read, shell = agent

    def gate(call, gctx):
        if call.name != "shell":
            return ng.ToolDecision.allow()
        if gctx.resume_value is None:
            return ng.ToolDecision.interrupt("shell needs approval")
        if gctx.resume_value.get("approved"):
            return ng.ToolDecision.allow()
        return ng.ToolDecision.deny("the human said no")

    cfg = ng.RunConfig()
    cfg.thread_id = "tg-refuse"
    engine.set_tool_gate(gate)
    assert engine.run(cfg).interrupted

    done = engine.resume("tg-refuse", {"approved": False})

    assert not done.interrupted
    assert shell.runs == 0, "a refused tool must not execute"
    assert read.runs == 1, "refusing one call must not block the others"
