"""A Python node can pause the graph and be resumed (issue #94).

The static form — `interrupt_before` in the graph definition — has always
worked from Python. The dynamic form did not: `NodeInterrupt` was not bound, so
a node that wanted to stop had nothing better to raise than `RuntimeError`, and
that aborts the run instead of pausing it. No checkpoint, nothing to resume.

Which rules out the one case dynamic interrupts exist for — the approval
prompt. You cannot put `interrupt_before` on "the node that is about to do
something dangerous", because whether it is dangerous depends on what the model
just asked for:

    "The agent wants to run `rm -rf build/`. Allow?"

So the pause has to carry a payload out (what needs approving) and the resume
has to carry an answer back in (what the human said). Both directions are
tested here; a binding that only did the first would let a Python user stop a
graph and never start it again.
"""

import neograph_engine as ng
import pytest


REASON = "shell command needs approval"
PAYLOAD = {"tool": "shell", "cmd": "rm -rf build/"}


class _ApprovalNode(ng.GraphNode):
    """Pauses on the first visit; acts on the human's answer on the second."""

    def __init__(self, name="risky"):
        super().__init__()
        self._name = name
        self.visits = 0

    def run(self, input):
        self.visits += 1
        answer = input.ctx.resume_value
        if answer is None:
            raise ng.NodeInterrupt(PAYLOAD, reason=REASON)
        verdict = "paid" if answer.get("approved") else "refused"
        return [ng.ChannelWrite("result", verdict)]

    def get_name(self):
        return self._name


def _definition(name="py_dynamic_interrupt"):
    return {
        "name": name,
        "channels": {"result": {"reducer": "overwrite"}},
        "nodes": {"risky": {"type": "py_approval"}},
        "edges": [
            {"from": "__start__", "to": "risky"},
            {"from": "risky", "to": "__end__"},
        ],
    }


@pytest.fixture
def engine():
    node = _ApprovalNode()
    ng.NodeFactory.register_type(
        "py_approval", lambda _name, _config, _ctx: node)
    store = ng.InMemoryCheckpointStore()
    compiled = ng.GraphEngine.compile(_definition(), ng.NodeContext(), store)
    return compiled, node


def test_the_exception_type_exists():
    """The whole issue in one line: there was nothing to raise."""
    assert hasattr(ng, "NodeInterrupt")


def test_a_python_node_pauses_the_graph(engine):
    compiled, _node = engine

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-pause"
    result = compiled.run(cfg)

    assert result.interrupted, "raising NodeInterrupt must pause, not fail"
    assert result.interrupt_node == "risky"


def test_the_payload_reaches_the_caller(engine):
    """The caller has to be able to render the prompt without parsing prose."""
    compiled, _node = engine

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-payload"
    result = compiled.run(cfg)

    assert result.interrupt_value["reason"] == REASON
    assert result.interrupt_value["value"] == PAYLOAD


def test_the_answer_reaches_the_node_and_the_run_completes(engine):
    compiled, node = engine

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-resume"
    assert compiled.run(cfg).interrupted
    assert node.visits == 1

    done = compiled.resume("py-di-resume", {"approved": True})

    assert not done.interrupted
    assert done.output["channels"]["result"]["value"] == "paid"
    assert node.visits == 2


def test_the_node_acts_on_what_the_human_said(engine):
    """A refusal is an answer. The node branches on the content, not the arrival."""
    compiled, _node = engine

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-refuse"
    assert compiled.run(cfg).interrupted

    done = compiled.resume("py-di-refuse", {"approved": False})

    assert not done.interrupted
    assert done.output["channels"]["result"]["value"] == "refused"


def test_a_reason_only_interrupt_works(engine):
    """`raise NodeInterrupt("why")` — the shape the C++ docs have always shown."""

    class ReasonOnly(ng.GraphNode):
        def run(self, _input):
            raise ng.NodeInterrupt("plain old reason")

        def get_name(self):
            return "plain"

    ng.NodeFactory.register_type(
        "py_reason_only", lambda _n, _c, _x: ReasonOnly())
    definition = _definition("py_reason_only_graph")
    definition["nodes"] = {"risky": {"type": "py_reason_only"}}

    store = ng.InMemoryCheckpointStore()
    compiled = ng.GraphEngine.compile(definition, ng.NodeContext(), store)

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-reason-only"
    result = compiled.run(cfg)

    assert result.interrupted
    assert result.interrupt_value["reason"] == "plain old reason"
    assert "value" not in result.interrupt_value


def test_an_ordinary_exception_is_still_an_error():
    """The translation must be narrow.

    If it swallowed every exception into a pause, a genuine bug in a node would
    look like "waiting for a human" and hang forever instead of failing loudly.
    """

    class Broken(ng.GraphNode):
        def run(self, _input):
            raise ValueError("this is a bug, not a question")

        def get_name(self):
            return "broken"

    ng.NodeFactory.register_type("py_broken", lambda _n, _c, _x: Broken())
    definition = _definition("py_broken_graph")
    definition["nodes"] = {"risky": {"type": "py_broken"}}

    compiled = ng.GraphEngine.compile(definition, ng.NodeContext())

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-broken"
    with pytest.raises(Exception, match="this is a bug"):
        compiled.run(cfg)


def test_resume_value_is_none_on_a_fresh_run(engine):
    """Otherwise a node would take the approved branch with nobody approving."""
    compiled, _node = engine

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-fresh"
    assert compiled.run(cfg).interrupted


class _UsageProvider(ng.Provider):
    def complete(self, params):
        completion = ng.ChatCompletion()
        completion.message = ng.ChatMessage("assistant", "ok")
        completion.usage.prompt_tokens = 10
        completion.usage.completion_tokens = 5
        completion.usage.total_tokens = 15
        return completion

    def complete_stream(self, params, on_chunk):
        return self.complete(params)

    def get_name(self):
        return "usage-stub"


def test_an_interrupted_run_still_reports_what_it_spent():
    """Where token accounting (#88) meets the dynamic interrupt (#94).

    A run that calls the model and *then* pauses for a human has already spent
    money. If the pause dropped the usage on the floor, every approval-gated
    agent would under-report its bill by exactly the work it did before asking.

    On the resume the count is zero, and that is correct rather than a second
    bug: the LLM node's write is replayed from the checkpoint, not re-executed,
    so no new tokens are bought. It is the same per-run contract documented on
    RunResult.usage — for the conversation total, supply RunConfig.usage.
    """
    provider = _UsageProvider()

    class Gate(ng.GraphNode):
        def run(self, input):
            if input.ctx.resume_value is None:
                raise ng.NodeInterrupt("needs approval")
            return [ng.ChannelWrite("done", True)]

        def get_name(self):
            return "gate"

    ng.NodeFactory.register_type("py_usage_gate", lambda _n, _c, _x: Gate())
    definition = {
        "name": "py_interrupt_usage",
        "channels": {"messages": {"reducer": "append"},
                     "done": {"reducer": "overwrite"}},
        "nodes": {"llm": {"type": "llm_call"},
                  "gate": {"type": "py_usage_gate"}},
        "edges": [
            {"from": "__start__", "to": "llm"},
            {"from": "llm", "to": "gate"},
            {"from": "gate", "to": "__end__"},
        ],
    }
    compiled = ng.GraphEngine.compile(
        definition, ng.NodeContext(provider=provider),
        ng.InMemoryCheckpointStore())

    cfg = ng.RunConfig()
    cfg.thread_id = "py-di-usage"
    paused = compiled.run(cfg)

    assert paused.interrupted
    assert paused.usage.total_tokens == 15, "the call made before the pause vanished from the bill"

    done = compiled.resume("py-di-usage", {"approved": True})
    assert done.output["channels"]["done"]["value"] is True
    assert done.usage.total_tokens == 0, "the resumed run replays; it must not re-buy the tokens"
