"""A resume value reaches only the node it answers (#393).

``input.ctx.resume_value`` used to stay set for every node of every later
super-step of a resumed run: a later node that asks, or a tool gate that asks
before a tool runs, took the earlier answer as its own and did not ask. Now
the value reaches only the invocation the resume answers, in the step it
re-enters, and a later node that asks pauses the run again.
"""

import pytest

import neograph_engine as ng


class Ask(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        if input.ctx.resume_value is None:
            raise ng.NodeInterrupt({"asks": self._name}, reason=f"approve {self._name}")
        return [ng.ChannelWrite("results", [f"{self._name}={input.ctx.resume_value}"])]


class Plain(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("results", [self._name])]


ng.NodeFactory.register_type("scope.ask", lambda name, _config, _ctx: Ask(name))
ng.NodeFactory.register_type("scope.plain", lambda name, _config, _ctx: Plain(name))


def _definition(nodes, edges, **extra):
    definition = {"name": "resume_value_scope",
                  "channels": {"results": {"reducer": "append", "initial": []}},
                  "nodes": nodes,
                  "edges": [{"from": a, "to": b} for a, b in edges]}
    definition.update(extra)
    return definition


def _engine(definition):
    return ng.GraphEngine.compile(definition, ng.NodeContext(), ng.InMemoryCheckpointStore())


def _results(result):
    return sorted(result.output["channels"]["results"]["value"])


TWO_STEPS = _definition({"first": {"type": "scope.ask"}, "second": {"type": "scope.ask"}},
                        [("__start__", "first"), ("first", "second"), ("second", "__end__")])


def test_a_node_of_a_later_step_asks_its_own_question():
    engine = _engine(TWO_STEPS)
    paused = engine.run(ng.RunConfig(thread_id="two-steps"))
    assert paused.interrupt_node == "first"

    again = engine.resume("two-steps", "F")
    assert again.interrupted, "second took first's answer"
    assert again.interrupt_node == "second"
    assert again.interrupt_value["value"] == {"asks": "second"}

    done = engine.resume("two-steps", "S")
    assert done.status == ng.RunStatus.Completed
    assert _results(done) == ["first=F", "second=S"]


def test_an_interrupt_after_answer_reaches_no_node():
    engine = _engine(_definition({"draft": {"type": "scope.plain"},
                                  "approve": {"type": "scope.ask"}},
                                 [("__start__", "draft"), ("draft", "approve"),
                                  ("approve", "__end__")],
                                 interrupt_after=["draft"]))
    assert engine.run(ng.RunConfig(thread_id="after")).interrupt_node == "draft"
    again = engine.resume("after", "reviewed")
    assert again.interrupted, "approve took the review as its approval"
    assert again.interrupt_node == "approve"
    assert _results(engine.resume("after", "A")) == ["approve=A", "draft"]


def test_a_gate_answer_reaches_the_gated_node_only():
    engine = _engine(_definition({"gated": {"type": "scope.ask"},
                                  "later": {"type": "scope.ask"}},
                                 [("__start__", "gated"), ("gated", "later"),
                                  ("later", "__end__")],
                                 interrupt_before=["gated"]))
    assert engine.run(ng.RunConfig(thread_id="gate")).interrupt_node == "gated"
    again = engine.resume("gate", "G")
    assert again.interrupted, "later took the gate's answer"
    assert again.interrupt_node == "later"
    assert _results(engine.resume("gate", "L")) == ["gated=G", "later=L"]


def test_a_tool_gate_of_a_later_step_asks_again():
    class ToolCaller(ng.GraphNode):
        def get_name(self):
            return "llm"

        def run(self, input):
            call = {"id": "call-1", "name": "shell", "arguments": "{}"}
            return [ng.ChannelWrite("messages", [{"role": "assistant", "content": "",
                                                  "tool_calls": [call]}])]

    ran = []

    class Shell(ng.Tool):
        def get_name(self):
            return "shell"

        def get_definition(self):
            definition = ng.ChatTool()
            definition.name = "shell"
            definition.description = "records its runs"
            return definition

        def execute(self, arguments):
            ran.append(arguments)
            return "{}"

    asked = []

    def gate(call, gctx):
        if gctx.resume_value is None:
            asked.append(call.name)
            return ng.ToolDecision.interrupt("shell needs approval")
        return ng.ToolDecision.allow()

    ng.NodeFactory.register_type("scope.tool_caller", lambda _n, _c, _x: ToolCaller())
    definition = _definition({"ask": {"type": "scope.ask"}, "llm": {"type": "scope.tool_caller"},
                              "tools": {"type": "tool_dispatch"}},
                             [("__start__", "ask"), ("ask", "llm"), ("llm", "tools"),
                              ("tools", "__end__")])
    definition["channels"]["messages"] = {"reducer": "append"}
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=[Shell()]),
                                    ng.InMemoryCheckpointStore())
    engine.set_tool_gate(gate)

    assert engine.run(ng.RunConfig(thread_id="tool-gate")).interrupt_node == "ask"
    again = engine.resume("tool-gate", {"approved": True})
    assert again.interrupted, "the gate took ask's answer as its approval"
    assert again.interrupt_node == "tools"
    assert asked == ["shell"]
    assert ran == []
    done = engine.resume("tool-gate", {"approved": True})
    assert done.status == ng.RunStatus.Completed
    assert len(ran) == 1
