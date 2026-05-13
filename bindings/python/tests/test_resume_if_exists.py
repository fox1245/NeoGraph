"""v0.3.1: RunConfig.resume_if_exists — multi-turn-chat semantics.

Closes the LangGraph parity gap noted in TODO_v0.3.md item #1: same
``thread_id`` no longer requires the caller to thread prior state
through the input dict themselves. Opt-in flag keeps existing callers
unaffected.
"""

import neograph_engine as neograph


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


def _build_chat_engine(node_type):
    """Single-node graph: append one assistant message echoing the last user msg.

    Uses an APPEND-reduced ``messages`` channel so each turn grows the
    history rather than replacing it — same shape as the canonical
    LangGraph chat agent.
    """
    class EchoNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def run(self, input):
            state = input.state
            msgs = state.get("messages") or []
            last_user = ""
            for m in msgs:
                if isinstance(m, dict) and m.get("role") == "user":
                    last_user = m.get("content", "")
            reply = {"role": "assistant", "content": f"echo: {last_user}"}
            return [neograph.ChannelWrite("messages", [reply])]

    neograph.NodeFactory.register_type(
        node_type,
        lambda name, config, ctx: EchoNode(name),
    )

    definition = {
        "name": "chat",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"echo": {"type": node_type}},
        "edges": [
            {"from": neograph.START_NODE, "to": "echo"},
            {"from": "echo",              "to": neograph.END_NODE},
        ],
    }

    store = neograph.InMemoryCheckpointStore()
    engine = neograph.GraphEngine.compile(definition, neograph.NodeContext())
    engine.set_checkpoint_store(store)
    return engine


def _user(content):
    return [{"role": "user", "content": content}]


def _msgs(result):
    return result.output["channels"]["messages"]["value"]


def test_default_off_starts_fresh_each_run():
    """Back-compat: same thread_id without the flag still starts fresh."""
    engine = _build_chat_engine(_next_type("echo_default"))

    r1 = engine.run(neograph.RunConfig(thread_id="t1", input={"messages": _user("hi")}))
    assert len(_msgs(r1)) == 2

    # Second call, same thread_id, default resume_if_exists=False → fresh start.
    r2 = engine.run(neograph.RunConfig(thread_id="t1", input={"messages": _user("again")}))
    msgs = _msgs(r2)
    assert len(msgs) == 2, f"default-off must not carry prior turn, got {msgs!r}"
    assert msgs[0]["content"] == "again"


def test_opt_in_carries_prior_state():
    """Two-turn chat: prior turn's user+assistant survive into turn 2."""
    engine = _build_chat_engine(_next_type("echo_opt_in"))

    engine.run(neograph.RunConfig(thread_id="chat-1", input={"messages": _user("hi")}))

    cfg = neograph.RunConfig(
        thread_id="chat-1",
        input={"messages": _user("how are you")},
        resume_if_exists=True,
    )
    r2 = engine.run(cfg)
    msgs = _msgs(r2)
    assert len(msgs) == 4
    assert msgs[0]["content"] == "hi"
    assert msgs[1]["content"] == "echo: hi"
    assert msgs[2]["content"] == "how are you"
    assert msgs[3]["content"] == "echo: how are you"


def test_opt_in_no_prior_checkpoint_starts_fresh():
    """Setting the flag on a brand-new thread_id is a no-op (no error)."""
    engine = _build_chat_engine(_next_type("echo_no_prior"))

    cfg = neograph.RunConfig(
        thread_id="never-seen-before",
        input={"messages": _user("first")},
        resume_if_exists=True,
    )
    r = engine.run(cfg)
    msgs = _msgs(r)
    assert len(msgs) == 2
    assert msgs[0]["content"] == "first"


def test_opt_in_via_attribute_assignment():
    """The flag is also settable as an attribute, not just a kwarg."""
    engine = _build_chat_engine(_next_type("echo_attr"))

    engine.run(neograph.RunConfig(thread_id="attr-1", input={"messages": _user("one")}))

    cfg = neograph.RunConfig(thread_id="attr-1", input={"messages": _user("two")})
    cfg.resume_if_exists = True
    r2 = engine.run(cfg)
    msgs = _msgs(r2)
    assert len(msgs) == 4
    assert msgs[2]["content"] == "two"


def test_three_turn_conversation():
    """Flag is durable across multiple opt-in resumes — each turn appends."""
    engine = _build_chat_engine(_next_type("echo_three"))

    def turn(content):
        return engine.run(neograph.RunConfig(
            thread_id="three",
            input={"messages": _user(content)},
            resume_if_exists=True,
        ))

    turn("turn one")
    turn("turn two")
    r3 = turn("turn three")
    msgs = _msgs(r3)
    assert len(msgs) == 6
    assert msgs[0]["content"] == "turn one"
    assert msgs[2]["content"] == "turn two"
    assert msgs[4]["content"] == "turn three"
    assert msgs[5]["content"] == "echo: turn three"


def test_default_value_is_false():
    """Constructor default must keep historical fresh-start behaviour."""
    cfg = neograph.RunConfig(thread_id="t", input={})
    assert cfg.resume_if_exists is False
