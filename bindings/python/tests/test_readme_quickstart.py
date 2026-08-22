"""Executable contracts for the current README Python claims."""

from __future__ import annotations

from pathlib import Path

import neograph_engine as ng


ROOT = Path(__file__).resolve().parents[3]


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


def test_readme_documented_python_surface_is_exported():
    required = {
        "RetryPolicy",
        "RunMetadata",
        "CacheScope",
        "ProgramSource",
        "ProgramRegistryBuilder",
        "ProgramCompiler",
        "LocalProgramHost",
        "HookRuntime",
        "RuntimeContextRequirements",
        "ContextTransformReceipt",
        "SQLiteContextStore",
        "SQLiteProviderDispatchReceiptStore",
        "StrictRuntimeProfile",
    }
    missing = sorted(name for name in required if not hasattr(ng, name))
    assert not missing, f"README documents unavailable wheel symbols: {missing}"


def test_readme_current_media_and_quickstart_links_exist():
    required_paths = [
        "docs/videos/neograph-promo-v3.mp4",
        "docs/images/neograph-promo-v3.gif",
        "examples/62_core_quickstart.cpp",
        "examples/63_program_quickstart.cpp",
        "docs/python-binding.md",
    ]
    missing = [path for path in required_paths if not (ROOT / path).is_file()]
    assert not missing, f"README links missing repository artifacts: {missing}"


def test_readme_no_longer_advertises_removed_walkthroughs():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    stale_phrases = [
        "ReAct agent with a real LLM",
        "Reading the output",
        "neograph-promo.mp4",
        "neograph-promo.gif",
    ]
    assert not [phrase for phrase in stale_phrases if phrase in readme]
