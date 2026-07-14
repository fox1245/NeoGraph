"""Capabilities that existed in C++ and were simply unreachable from Python (#97).

Three of them, and they are not the same kind of thing:

  - **Store** is a missing *subsystem*. Checkpoints remember one conversation;
    the Store remembers the user across all of them. There was no symbol for any
    of it in Python — not the interface, not the in-memory implementation, not
    the item type, and no way for a node body to reach one.

  - **RateLimitedProvider** is what stands between a demo and something you
    would leave running. Without it a Python user wraps `engine.run()` in their
    own retry loop, which retries *the whole graph* rather than the one HTTP
    call that got a 429.

  - **GraphValidator** turns "it threw at compile()" into a report you can read
    before you run anything.

NOT here, deliberately: `RetryPolicy`. The issue listed it, and the issue was
wrong — a graph definition already carries `"retry_policy": {...}` and the engine
already honours it from Python. Binding a RetryPolicy *class* would give one
concept two homes. `test_retry_policy_already_works_from_the_definition` pins
that it works, so nobody re-files this.
"""

import time

import neograph_engine as ng
import neograph_engine.llm as nglm
import pytest


# ── Store: the long-term memory subsystem ───────────────────────────────────

def test_the_store_symbols_exist():
    assert hasattr(ng, "Store")
    assert hasattr(ng, "InMemoryStore")
    assert hasattr(ng, "StoreItem")


def test_put_get_round_trip():
    store = ng.InMemoryStore()

    store.put(["users", "u1"], "prefs", {"theme": "dark"})
    item = store.get(["users", "u1"], "prefs")

    assert item is not None
    assert item.value == {"theme": "dark"}
    assert item.ns == ["users", "u1"]
    assert item.key == "prefs"
    assert item.created_at > 0


def test_a_missing_item_is_none_not_an_exception():
    store = ng.InMemoryStore()

    assert store.get(["users", "nobody"], "prefs") is None


def test_search_by_namespace_prefix():
    store = ng.InMemoryStore()
    store.put(["users", "u1"], "a", {"n": 1})
    store.put(["users", "u1"], "b", {"n": 2})
    store.put(["users", "u2"], "c", {"n": 3})

    found = store.search(["users", "u1"])

    assert sorted(i.key for i in found) == ["a", "b"]
    assert len(store.search(["users"])) == 3


def test_delete_and_list_namespaces():
    store = ng.InMemoryStore()
    store.put(["users", "u1"], "a", {"n": 1})
    store.put(["orgs", "o1"], "b", {"n": 2})

    assert sorted(store.list_namespaces()) == [["orgs", "o1"], ["users", "u1"]]

    store.delete_item(["users", "u1"], "a")

    assert store.get(["users", "u1"], "a") is None


def test_a_node_can_reach_the_store_through_its_context():
    """The point of the subsystem: a node remembering something across runs.

    Without `ctx.store` a Python node has to capture the store in the factory
    closure — which works, and which means the store cannot be swapped without
    recompiling the graph.
    """

    class Remembering(ng.GraphNode):
        def run(self, input):
            store = input.ctx.store
            assert store is not None, "the node cannot see the engine's store"

            seen = store.get(["visits"], "count")
            count = (seen.value["n"] if seen else 0) + 1
            store.put(["visits"], "count", {"n": count})

            return [ng.ChannelWrite("count", count)]

        def get_name(self):
            return "remember"

    ng.NodeFactory.register_type("remember", lambda *_a: Remembering())
    definition = {
        "name": "store_graph",
        "channels": {"count": {"reducer": "overwrite"}},
        "nodes": {"remember": {"type": "remember"}},
        "edges": [
            {"from": "__start__", "to": "remember"},
            {"from": "remember", "to": "__end__"},
        ],
    }

    store = ng.InMemoryStore()
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.set_store(store)

    counts = []
    for turn in range(3):
        cfg = ng.RunConfig()
        cfg.thread_id = f"visit-{turn}"     # a DIFFERENT conversation each time
        counts.append(engine.run(cfg).output["channels"]["count"]["value"])

    assert counts == [1, 2, 3], (
        "the store did not survive across threads — which is the one thing it "
        "does that a checkpoint does not")


# ── RateLimitedProvider: back off on a 429, not on the whole graph ───────────

class _RateLimited(ng.Provider):
    """Fails with a rate-limit error N times, then succeeds."""

    def __init__(self, fail_times):
        super().__init__()
        self.calls = 0
        self._fail_times = fail_times

    def complete(self, params):
        self.calls += 1
        if self.calls <= self._fail_times:
            raise ng.RateLimitError("429 Too Many Requests", retry_after_seconds=0)
        completion = ng.ChatCompletion()
        completion.message = ng.ChatMessage("assistant", "ok")
        return completion

    def complete_stream(self, params, on_chunk):
        return self.complete(params)

    def get_name(self):
        return "rate-limited-stub"


def test_the_rate_limited_provider_exists():
    assert hasattr(nglm, "RateLimitedProvider")


def test_it_retries_the_call_and_succeeds():
    from neograph_engine.llm import RateLimitedProvider

    inner = _RateLimited(fail_times=2)
    provider = RateLimitedProvider(
        inner, max_retries=3, default_wait_seconds=0, max_wait_seconds=0)

    params = ng.CompletionParams()
    params.messages = [ng.ChatMessage("user", "hi")]
    result = provider.complete(params)

    assert result.message.content == "ok"
    assert inner.calls == 3, "the wrapper did not retry the failing call"


def test_it_gives_up_after_max_retries():
    from neograph_engine.llm import RateLimitedProvider

    inner = _RateLimited(fail_times=99)
    provider = RateLimitedProvider(
        inner, max_retries=2, default_wait_seconds=0, max_wait_seconds=0)

    params = ng.CompletionParams()
    params.messages = [ng.ChatMessage("user", "hi")]
    with pytest.raises(Exception):
        provider.complete(params)

    assert inner.calls == 3, "expected the original call plus two retries"


# ── GraphValidator: read the report instead of catching the throw ────────────

def test_the_validator_exists():
    assert hasattr(ng, "validate")
    assert hasattr(ng, "ValidationReport")


def test_a_good_graph_has_no_errors():
    definition = {
        "name": "fine",
        "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {},
        "edges": [{"from": "__start__", "to": "__end__"}],
    }

    report = ng.validate(definition)

    assert not report.has_errors()
    assert report.summary() == "" or "error" not in report.summary().lower()


def test_a_dangling_edge_is_reported_not_thrown():
    """The whole point: you get told what is wrong, before anything runs."""
    definition = {
        "name": "broken",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"a": {"type": "llm_call"}},
        "edges": [
            {"from": "__start__", "to": "a"},
            {"from": "a", "to": "ghost"},        # <- no such node
        ],
    }

    report = ng.validate(definition)

    assert report.has_errors()
    codes = [d.code for d in report.errors()]
    assert "E3" in codes, f"expected a dangling-reference error, got {codes}"

    offending = [d for d in report.errors() if d.code == "E3"]
    assert "ghost" in offending[0].message


def test_an_unknown_node_type_still_throws():
    """The honest edge of validate(), pinned rather than papered over.

    validate() compiles the definition first, and compiling instantiates the
    nodes — so a node type nobody registered surfaces as an exception, not as a
    diagnostic. Same on the C++ side. Register your node types before you
    validate, exactly as you would before you compile.
    """
    definition = {
        "name": "unknown_type",
        "channels": {"x": {"reducer": "overwrite"}},
        "nodes": {"a": {"type": "no_such_type_anywhere"}},
        "edges": [
            {"from": "__start__", "to": "a"},
            {"from": "a", "to": "__end__"},
        ],
    }

    with pytest.raises(RuntimeError, match="Unknown node type"):
        ng.validate(definition)


# ── The one the issue got wrong ─────────────────────────────────────────────

def test_retry_policy_already_works_from_the_definition():
    """#97 listed RetryPolicy as a gap. It is not one.

    A graph definition carries `"retry_policy": {...}` and the engine honours it
    — from Python, today, with no class bound. Binding a RetryPolicy class would
    give one concept two homes, so this test exists instead: it proves the
    capability is there, so nobody re-files the gap.
    """
    attempts = {"n": 0}

    class Flaky(ng.GraphNode):
        def run(self, _input):
            attempts["n"] += 1
            if attempts["n"] < 3:
                raise RuntimeError("transient")
            return [ng.ChannelWrite("result", "ok")]

        def get_name(self):
            return "flaky"

    ng.NodeFactory.register_type("flaky_parity", lambda *_a: Flaky())
    definition = {
        "name": "retry_from_definition",
        "channels": {"result": {"reducer": "overwrite"}},
        "nodes": {"flaky": {"type": "flaky_parity"}},
        "edges": [
            {"from": "__start__", "to": "flaky"},
            {"from": "flaky", "to": "__end__"},
        ],
        "retry_policy": {"max_retries": 5, "initial_delay_ms": 1},
    }

    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    cfg = ng.RunConfig()
    cfg.thread_id = "retry-def"
    result = engine.run(cfg)

    assert attempts["n"] == 3
    assert result.output["channels"]["result"]["value"] == "ok"
