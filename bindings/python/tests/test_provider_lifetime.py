"""A delegating Python provider retains real SDK execution through collection."""

import gc
import weakref

import pytest
import neograph_engine as ng


class _Provider(ng.Provider):
    def __init__(self, inner):
        super().__init__("openai.chat")
        self.inner = inner

    def get_name(self):
        return "python-delegating-provider"

    def prepare(self, request):
        return self.inner.prepare(request)


def _definition():
    return {
        "name": "provider_lifetime", "schema_version": 1,
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "llm_call"}},
        "edges": [{"from": ng.START_NODE, "to": "llm"},
                  {"from": "llm", "to": ng.END_NODE}],
    }


@pytest.mark.parametrize("drive", ["run", "run_stream"])
def test_provider_passed_as_a_temporary_survives_the_run(provider_peer, drive):
    engine = ng.GraphEngine.compile(
        _definition(), ng.NodeContext(provider=_Provider(provider_peer.provider()),
                                      model="local-model"))
    gc.collect()
    cfg = ng.RunConfig(thread_id=f"temp-provider-{drive}")
    cfg.input = {"messages": [{"role": "user", "content": "local request"}]}
    result = (engine.run(cfg) if drive == "run" else
              engine.run_stream(cfg, lambda _event: None))
    assert result.output["channels"]["messages"]["value"][-1]["content"] == "ok"


@pytest.mark.parametrize("drive", ["run", "run_stream"])
def test_provider_reassignment_releases_old_and_retains_compiled_snapshot(provider_peer, drive):
    initial = _Provider(provider_peer.provider())
    initial_ref = weakref.ref(initial)
    ctx = ng.NodeContext(provider=initial, model="local-model")
    del initial
    gc.collect()
    assert initial_ref() is not None
    replacement = _Provider(provider_peer.provider())
    replacement_ref = weakref.ref(replacement)
    ctx.provider = replacement
    del replacement
    gc.collect()
    assert initial_ref() is None
    assert replacement_ref() is not None
    engine = ng.GraphEngine.compile(_definition(), ctx)
    ctx.provider = None
    del ctx
    gc.collect()
    assert replacement_ref() is not None
    cfg = ng.RunConfig(thread_id=f"reassigned-{drive}")
    cfg.input = {"messages": [{"role": "user", "content": "local request"}]}
    result = (engine.run(cfg) if drive == "run" else
              engine.run_stream(cfg, lambda _event: None))
    assert result.output["channels"]["messages"]["value"][-1]["content"] == "ok"


def test_provider_property_none_releases_the_current_provider(provider_peer):
    provider = _Provider(provider_peer.provider())
    provider_ref = weakref.ref(provider)
    ctx = ng.NodeContext(provider=provider)
    del provider
    gc.collect()
    assert provider_ref() is not None
    ctx.provider = None
    gc.collect()
    assert provider_ref() is None
    assert ctx.provider is None


def test_repeated_provider_assignment_keeps_only_the_current_provider(provider_peer):
    ctx = ng.NodeContext()
    refs = []
    for _ in range(10):
        provider = _Provider(provider_peer.provider())
        refs.append(weakref.ref(provider))
        ctx.provider = provider
    del provider
    gc.collect()
    assert [ref() is not None for ref in refs] == [False] * 9 + [True]
    ctx.provider = None
    gc.collect()
    assert all(ref() is None for ref in refs)
