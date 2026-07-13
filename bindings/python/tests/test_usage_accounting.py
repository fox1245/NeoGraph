"""Token usage comes out of a run, from Python too (issue #88).

Cost visibility is not a C++ concern. A Python user running a graph against a
paid API has exactly the same question — what did that cost? — and before this
there was no way to ask it: the providers parsed the usage and the graph threw
it away.
"""

import neograph_engine as ng


class _UsageProvider(ng.Provider):
    def __init__(self, prompt=10, completion=5):
        super().__init__()
        self._prompt = prompt
        self._completion = completion

    def complete(self, params):
        c = ng.ChatCompletion()
        c.message = ng.ChatMessage("assistant", "ok")
        c.usage.prompt_tokens = self._prompt
        c.usage.completion_tokens = self._completion
        c.usage.total_tokens = self._prompt + self._completion
        return c

    def complete_stream(self, params, on_chunk):
        return self.complete(params)

    def get_name(self):
        return "usage-stub"


def _engine():
    definition = {
        "name": "py_usage",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "llm_call"}},
        "edges": [
            {"from": "__start__", "to": "llm"},
            {"from": "llm", "to": "__end__"},
        ],
    }
    return ng.GraphEngine.compile(definition, ng.NodeContext(provider=_UsageProvider()))


def test_run_reports_token_usage():
    result = _engine().run(ng.RunConfig(thread_id="usage-single"))

    assert result.usage.prompt_tokens == 10
    assert result.usage.completion_tokens == 5
    assert result.usage.total_tokens == 15


def test_a_graph_with_no_llm_reports_zero():
    definition = {
        "name": "py_no_llm",
        "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {},
        "edges": [{"from": "__start__", "to": "__end__"}],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())

    result = engine.run(ng.RunConfig(thread_id="usage-empty"))

    assert result.usage.total_tokens == 0


def test_an_accumulator_totals_across_runs():
    """A multi-turn chat is N runs on one thread_id.

    The number people want is what the conversation cost, not what the last turn
    cost — so the accumulator can outlive a single run.
    """
    engine = _engine()
    accumulator = ng.UsageAccumulator()

    for turn in range(3):
        cfg = ng.RunConfig(thread_id=f"usage-turn-{turn}")
        cfg.usage = accumulator
        engine.run(cfg)

    assert accumulator.snapshot().total_tokens == 45
    assert accumulator.snapshot().prompt_tokens == 30
