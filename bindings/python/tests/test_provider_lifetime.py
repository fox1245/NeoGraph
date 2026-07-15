"""A Python Provider subclass must survive being passed as a temporary (#98).

    engine = ng.GraphEngine.compile(defn, ng.NodeContext(provider=MyProvider()))

Every argument there is a temporary — which is how anyone writes it. Before the
keep_alive chain, this segfaulted: the C++ side holds a shared_ptr<Provider>,
which keeps the *C++* trampoline alive, but the Python instance carrying the
overrides is a separate refcount. Once it was collected, the trampoline could no
longer find `complete`, fell through to the base Provider::complete, which
bridges to complete_async, which bridges back to complete — infinite recursion,
stack overflow, SIGSEGV.

It went unnoticed because every existing test that subclasses Provider drives the
graph through run_stream(), and holds the provider in a local variable.
"""

import gc
import weakref

import neograph_engine as ng
import pytest


class _Provider(ng.Provider):
    def __init__(self, reply="ok"):
        super().__init__()
        self._reply = reply

    def complete(self, params):
        c = ng.ChatCompletion()
        c.message = ng.ChatMessage("assistant", self._reply)
        c.usage.prompt_tokens = 10
        c.usage.completion_tokens = 5
        c.usage.total_tokens = 15
        return c

    def complete_stream(self, params, on_chunk):
        return self.complete(params)

    def get_name(self):
        return "test-provider"


def _definition():
    return {
        "name": "provider_lifetime",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "llm_call"}},
        "edges": [
            {"from": "__start__", "to": "llm"},
            {"from": "llm", "to": "__end__"},
        ],
    }


def test_provider_passed_as_a_temporary_survives_the_run():
    # Nothing here holds a reference to the provider or the NodeContext.
    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext(provider=_Provider()))
    gc.collect()  # and make sure Python really does try to collect them

    cfg = ng.RunConfig()
    cfg.thread_id = "temp-provider"
    result = engine.run(cfg)

    assert result.output["channels"]["messages"]["value"][-1]["content"] == "ok"


def test_provider_held_in_a_local_still_works():
    provider = _Provider("held")
    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext(provider=provider))

    cfg = ng.RunConfig()
    cfg.thread_id = "held-provider"
    result = engine.run(cfg)

    assert result.output["channels"]["messages"]["value"][-1]["content"] == "held"


@pytest.mark.parametrize("drive", ["run", "run_stream"])
def test_provider_assigned_after_construction_survives_collection(drive):
    """The current provider stays alive; the replaced provider does not."""
    initial = _Provider("initial")
    initial_ref = weakref.ref(initial)
    ctx = ng.NodeContext(provider=initial)
    del initial
    gc.collect()

    assert initial_ref() is not None

    replacement = _Provider("reassigned")
    replacement_ref = weakref.ref(replacement)
    ctx.provider = replacement
    del replacement
    gc.collect()

    assert initial_ref() is None
    assert replacement_ref() is not None

    engine = ng.GraphEngine.compile(_definition(), ctx)
    del ctx
    gc.collect()

    assert replacement_ref() is not None

    cfg = ng.RunConfig()
    cfg.thread_id = f"reassigned-{drive}"
    if drive == "run":
        result = engine.run(cfg)
    else:
        result = engine.run_stream(cfg, lambda _event: None)

    assert result.output["channels"]["messages"]["value"][-1]["content"] == "reassigned"


def test_provider_property_none_releases_the_current_provider():
    provider = _Provider()
    provider_ref = weakref.ref(provider)
    ctx = ng.NodeContext(provider=provider)
    del provider
    gc.collect()

    assert provider_ref() is not None

    ctx.provider = None
    gc.collect()

    assert provider_ref() is None
    assert ctx.provider is None


def test_repeated_provider_assignment_keeps_only_the_current_provider():
    ctx = ng.NodeContext()
    refs = []

    for i in range(10):
        provider = _Provider(f"provider-{i}")
        refs.append(weakref.ref(provider))
        ctx.provider = provider

    del provider
    gc.collect()

    assert [ref() is not None for ref in refs] == [False] * 9 + [True]

    ctx.provider = None
    gc.collect()

    assert all(ref() is None for ref in refs)


@pytest.mark.parametrize("drive", ["run", "run_stream"])
def test_every_entry_point_reaches_the_python_override(drive):
    """run() reaches complete(); run_stream() reaches complete_stream().

    Only the second was ever exercised, which is why the first could crash for
    the entire life of the binding with a green suite.
    """
    engine = ng.GraphEngine.compile(_definition(), ng.NodeContext(provider=_Provider()))

    cfg = ng.RunConfig()
    cfg.thread_id = f"entry-{drive}"

    if drive == "run":
        result = engine.run(cfg)
    else:
        result = engine.run_stream(cfg, lambda _event: None)

    assert result.output["channels"]["messages"]["value"][-1]["content"] == "ok"
