"""v0.3.2: TODO_v0.3.md item #6 — flat StateView for engine.get_state.

Pre-fix users had to write
``state["channels"]["messages"]["value"]`` for the most common
read. The new ``engine.get_state_view(thread_id)`` returns a
Pydantic-backed ``StateView`` where channels are top-level
attributes (``view.messages``), with ``.raw`` preserved for
metadata (versions, etc.). Subclassing ``StateView`` with declared
fields gives full Pydantic typing.
"""

import pytest

import neograph_engine as neograph
from neograph_engine import StateView


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


class _PassthroughNode(neograph.GraphNode):
    """Trivial node so the engine actually saves a checkpoint."""
    def __init__(self, name):
        super().__init__()
        self._n = name
    def get_name(self):
        return self._n
    def run(self, input):
        state = input.state
        return []


def _build_engine():
    type_name = _next_type("state_view_passthrough")
    neograph.NodeFactory.register_type(
        type_name, lambda name, c, ctx: _PassthroughNode(name))
    store = neograph.InMemoryCheckpointStore()
    definition = {
        "name": "state_view_test",
        "channels": {
            "messages": {"reducer": "append"},
            "scratch":  {"reducer": "overwrite"},
        },
        "nodes": {"n": {"type": type_name}},
        "edges": [
            {"from": neograph.START_NODE, "to": "n"},
            {"from": "n",                 "to": neograph.END_NODE},
        ],
    }
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    engine.set_checkpoint_store(store)
    engine.run(neograph.RunConfig(thread_id="t1", input={
        "messages": [{"role": "user", "content": "hello"}],
        "scratch": "scratch-init",
    }))
    return engine


def test_get_state_view_returns_StateView_with_flat_access():
    """The whole point: skip the nested ['channels'][n]['value'] indirection."""
    engine = _build_engine()
    view = engine.get_state_view("t1")
    assert isinstance(view, StateView)
    assert view.messages == [{"role": "user", "content": "hello"}]
    assert view.scratch  == "scratch-init"


def test_get_state_view_returns_None_for_unknown_thread():
    """Mirrors get_state's None return for missing checkpoint."""
    engine = _build_engine()
    assert engine.get_state_view("nonexistent-thread") is None


def test_state_view_raw_preserves_unflattened_dict():
    """Callers who DO need version / metadata can still reach it via .raw."""
    engine = _build_engine()
    view = engine.get_state_view("t1")
    raw = view.raw
    assert isinstance(raw, dict)
    assert "channels" in raw
    assert raw["channels"]["messages"]["value"] == \
        [{"role": "user", "content": "hello"}]
    # Versions live in the per-channel dict alongside 'value'.
    assert "version" in raw["channels"]["messages"]


def test_state_view_channel_names_lists_channels():
    engine = _build_engine()
    view = engine.get_state_view("t1")
    names = set(view.channel_names())
    # At least our two declared channels should be present.
    assert {"messages", "scratch"}.issubset(names)


def test_state_view_get_with_default():
    """Dict-like .get(name, default) for dynamic channel name access."""
    engine = _build_engine()
    view = engine.get_state_view("t1")
    assert view.get("messages") == [{"role": "user", "content": "hello"}]
    assert view.get("does_not_exist", "fallback") == "fallback"
    assert view.get("does_not_exist") is None


def test_typed_subclass_validates_fields():
    """User-declared fields get full Pydantic validation."""

    class ChatState(StateView):
        messages: list[dict] = []
        scratch: str = ""

    engine = _build_engine()
    typed = engine.get_state_view("t1", model=ChatState)
    assert isinstance(typed, ChatState)
    assert typed.messages == [{"role": "user", "content": "hello"}]
    assert typed.scratch == "scratch-init"
    # Subclass instances are also StateView, so .raw still works.
    assert "channels" in typed.raw


def test_typed_subclass_rejects_wrong_type():
    """If the user declares a field type that doesn't match the actual
    channel value, Pydantic raises — better than a silent type mismatch."""
    from pydantic import ValidationError

    class WrongTypes(StateView):
        # messages is actually a list, but declare it as int — Pydantic
        # should reject the validation.
        messages: int = 0

    engine = _build_engine()
    with pytest.raises(ValidationError):
        engine.get_state_view("t1", model=WrongTypes)


def test_StateView_from_state_rejects_unflattened_input():
    """Defensive: from_state expects the engine's get_state shape, not
    a RunResult.output slice. Feed it a wrong shape and it should
    raise loudly — silent acceptance would mask bugs."""
    with pytest.raises(ValueError):
        StateView.from_state({"messages": []})  # missing 'channels' wrapper


def test_StateView_from_state_with_explicit_dict():
    """Direct construction (without going through engine) works for tests
    and one-off transformations."""
    raw = {
        "channels": {
            "foo": {"value": 42, "version": 1},
            "bar": {"value": "hello", "version": 1},
        },
        "global_version": 1,
    }
    view = StateView.from_state(raw)
    assert view.foo == 42
    assert view.bar == "hello"
    assert view.raw is raw  # same reference, not a copy


def test_StateView_tolerates_channel_entry_without_value_wrapper():
    """Some test fixtures or older serializations may lack the
    {'value': ...} wrapper. Don't bail — just pass the entry through.
    Better than blowing up on legitimate-looking shape variation."""
    raw = {
        "channels": {
            "wrapped":   {"value": "wrapped-val"},
            "unwrapped": "raw-string-no-wrapper",
        },
    }
    view = StateView.from_state(raw)
    assert view.wrapped == "wrapped-val"
    assert view.unwrapped == "raw-string-no-wrapper"


def test_StateView_module_export():
    """StateView re-exported at the package root for ergonomics."""
    assert hasattr(neograph, "StateView")
    assert "StateView" in neograph.__all__


def test_engine_get_state_view_method_exists():
    """The monkey-patched method is present on every GraphEngine."""
    engine = _build_engine()
    assert hasattr(engine, "get_state_view")
    assert callable(engine.get_state_view)
