"""Executable contracts for the current README Python claims."""

from __future__ import annotations


import neograph_engine as ng


def test_readme_five_second_demo_runs_and_produces_documented_output():
    @ng.node("greet")
    def greet(state):
        return [
            ng.ChannelWrite(
                "messages",
                [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}],
            )
        ]

    definition = {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
        "name": "demo",
        "channels": {
            "name": {"reducer": "overwrite"},
            "messages": {"reducer": "append"},
        },
        "nodes": {"greet": {"type": "greet"}},
        "edges": [
            {"from": ng.START_NODE, "to": "greet"},
            {"from": "greet", "to": ng.END_NODE},
        ],
    }

    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
    assert result.output["channels"]["messages"]["value"] == [
        {"role": "assistant", "content": "Hello, NeoGraph!"}
    ]
