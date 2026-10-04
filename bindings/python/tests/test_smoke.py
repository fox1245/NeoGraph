"""Smoke test for the pybind11 binding.

Run from the build dir (where libneograph_core.so + neograph/ live):

    PYTHONPATH=$PWD pytest -q ../bindings/python/tests

Doesn't hit any LLM API — uses a JSON-defined graph that compiles
cleanly without invoking a Provider. The point is to prove the
binding boundary works end-to-end: dict-shaped graph definition →
GraphEngine.compile → GraphEngine.run → result.output.
"""


import neograph_engine as neograph  # PyPI dist name is `neograph-engine`;
                                     # bare `neograph` was already taken




def test_stream_mode_bitfield():
    # StreamMode is bound with py::arithmetic so | and & should compose.
    combined = neograph.StreamMode.EVENTS | neograph.StreamMode.TOKENS
    # Either result is fine; both must register as containing EVENTS.
    assert int(combined) & int(neograph.StreamMode.EVENTS)
    assert int(combined) & int(neograph.StreamMode.TOKENS)


def test_compile_minimal_graph():
    """The simplest valid graph: one channel, no nodes, START → END."""
    definition = {
        "name": "passthrough",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {},
        "edges": [
            {"from": neograph.START_NODE, "to": neograph.END_NODE},
        ],
    }
    ctx = neograph.NodeContext()
    engine = neograph.GraphEngine.compile(definition, ctx)
    assert engine.name == "passthrough"

    cfg = neograph.RunConfig(
        thread_id="t1",
        input={"messages": [{"role": "user", "content": "hello"}]},
    )
    result = engine.run(cfg)
    assert result.output["channels"]["messages"]["value"] == [
        {"role": "user", "content": "hello"},
    ]
