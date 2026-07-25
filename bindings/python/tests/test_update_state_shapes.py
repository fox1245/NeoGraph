"""v0.3.2: TODO_v0.3.md item #5 — update_state accepts both
dict and list[ChannelWrite] forms.

Pre-fix the binding silently no-op'd when channel_writes was a
list (the C++ engine checks `is_object()` and skips otherwise),
which is exactly the shape a caller would build by collecting
ChannelWrite from a node body. The dispatch preserves that list's
ChannelWrite modes and order:

- dict             {channel: value}        → existing path
- list of CW       [ChannelWrite(...), ...] → apply writes in order
- anything else                              → TypeError (no silent no-op)
"""

import pytest

import neograph_engine as neograph


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


class _PassthroughNode(neograph.GraphNode):
    """Trivial node so the engine actually runs a super-step and
    saves a checkpoint that update_state can write *over*."""

    def __init__(self, name):
        super().__init__()
        self._n = name

    def get_name(self):
        return self._n

    def run(self, input):
        state = input.state
        return []


def _build_engine_with_messages():
    type_name = _next_type("update_state_passthrough")
    neograph.NodeFactory.register_type(
        type_name,
        lambda name, c, ctx: _PassthroughNode(name),
    )
    store = neograph.InMemoryCheckpointStore()
    definition = {
        "name": "update_state_shapes",
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
    # update_state needs a checkpoint to write *over*. Run once to
    # establish the base checkpoint.
    engine.run(neograph.RunConfig(thread_id="t1", input={}))
    return engine


def _channel(state, name):
    return state["channels"][name]["value"]


def test_dict_form_writes_through_reducer():
    """The historical dict shape — `{channel: value}` — must keep working."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        {"messages": [{"role": "user", "content": "hi"}]},
    )
    state = engine.get_state("t1")
    assert _channel(state, "messages") == [{"role": "user", "content": "hi"}]


def test_list_of_channelwrite_form_writes_through_reducer():
    """The new list form — symmetric with what node bodies emit."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        [neograph.ChannelWrite("messages",
                               [{"role": "user", "content": "from list"}])],
    )
    state = engine.get_state("t1")
    assert _channel(state, "messages") == [{"role": "user", "content": "from list"}]


def test_list_form_preserves_overwrite_mode():
    """An explicit overwrite must bypass the append reducer."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        {"messages": [{"role": "user", "content": "old"}]},
    )
    engine.update_state(
        "t1",
        [neograph.ChannelWrite(
            "messages",
            [{"role": "assistant", "content": "replacement"}],
            neograph.ChannelWrite.Mode.OVERWRITE,
        )],
    )
    assert _channel(engine.get_state("t1"), "messages") == [
        {"role": "assistant", "content": "replacement"},
    ]


def test_list_form_preserves_reduce_and_overwrite_order():
    """Writes to one channel retain list order instead of collapsing to a dict."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        {"messages": [{"role": "user", "content": "seed"}]},
    )
    engine.update_state(
        "t1",
        [
            neograph.ChannelWrite("messages", [{"role": "assistant", "content": "append"}]),
            neograph.ChannelWrite(
                "messages",
                [{"role": "assistant", "content": "replace"}],
                neograph.ChannelWrite.Mode.OVERWRITE,
            ),
            neograph.ChannelWrite("messages", [{"role": "user", "content": "after"}]),
        ],
    )
    assert _channel(engine.get_state("t1"), "messages") == [
        {"role": "assistant", "content": "replace"},
        {"role": "user", "content": "after"},
    ]


def test_list_form_multiple_distinct_channels():
    """Multiple ChannelWrites covering different channels — all applied."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        [
            neograph.ChannelWrite("messages", [{"role": "user", "content": "m"}]),
            neograph.ChannelWrite("scratch",  "scratch-value"),
        ],
    )
    state = engine.get_state("t1")
    assert _channel(state, "messages") == [{"role": "user", "content": "m"}]
    assert _channel(state, "scratch")  == "scratch-value"


def test_list_form_duplicate_channel_writes_apply_in_order():
    """Repeated reducer writes keep their individual list positions."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        [
            neograph.ChannelWrite("messages", [{"role": "user", "content": "first"}]),
            neograph.ChannelWrite("messages", [{"role": "assistant", "content": "second"}]),
        ],
    )
    state = engine.get_state("t1")
    assert _channel(state, "messages") == [
        {"role": "user", "content": "first"},
        {"role": "assistant", "content": "second"},
    ]


def test_list_form_accepts_bundled_values_for_append_reducer():
    """One ChannelWrite may still carry multiple reducer values."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        [
            neograph.ChannelWrite("messages", [
                {"role": "user", "content": "m1"},
                {"role": "assistant", "content": "m2"},
            ]),
        ],
    )
    state = engine.get_state("t1")
    assert _channel(state, "messages") == [
        {"role": "user", "content": "m1"},
        {"role": "assistant", "content": "m2"},
    ]


def test_tuple_form_also_accepted():
    """list and tuple are both iterable sequences — accept both."""
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        (neograph.ChannelWrite("scratch", "via-tuple"),),
    )
    assert _channel(engine.get_state("t1"), "scratch") == "via-tuple"


def test_invalid_type_raises_type_error_not_silent_noop():
    """A naked string / int / etc. used to silently no-op (because the
    C++ engine's is_object() check rejected it). The whole point of
    item #5 is to make THAT silent failure mode noisy."""
    engine = _build_engine_with_messages()
    with pytest.raises(TypeError) as excinfo:
        engine.update_state("t1", "not-a-valid-shape")
    assert "channel_writes" in str(excinfo.value)


def test_list_with_non_channelwrite_item_raises():
    """A list containing something that isn't ChannelWrite gets a
    typed error pointing at .channel / .value, not a silent skip."""
    engine = _build_engine_with_messages()
    with pytest.raises(TypeError) as excinfo:
        engine.update_state("t1", ["just-a-string"])
    msg = str(excinfo.value)
    assert ".channel" in msg or "ChannelWrite" in msg


def test_duck_typed_object_with_channel_and_value_works():
    """The list form documents .channel/.value duck-typing — a
    SimpleNamespace or any object with those attrs is accepted.
    Useful in tests / REPL flows where users haven't imported
    ChannelWrite."""
    from types import SimpleNamespace
    engine = _build_engine_with_messages()
    engine.update_state(
        "t1",
        [SimpleNamespace(channel="scratch", value="duck")],
    )
    assert _channel(engine.get_state("t1"), "scratch") == "duck"


def test_empty_list_is_a_noop_not_an_error():
    """Empty list = no writes to apply. Must not error — common shape
    when a caller filters down their writes and ends up with nothing."""
    engine = _build_engine_with_messages()
    # Should not raise.
    engine.update_state("t1", [])


def test_empty_dict_is_a_noop_not_an_error():
    """Symmetric with the list form."""
    engine = _build_engine_with_messages()
    engine.update_state("t1", {})
