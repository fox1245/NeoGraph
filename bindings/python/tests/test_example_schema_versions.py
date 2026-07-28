"""Recommended and generated topology examples must opt into strict mode."""

from __future__ import annotations

import json
from pathlib import Path

import neograph_engine as ng


ROOT = Path(__file__).resolve().parents[3]


def test_python_examples_that_compile_topologies_declare_schema_version():
    roots = [ROOT / "bindings/python/examples", ROOT / "examples/cookbook"]
    sources = [path for root in roots for path in root.rglob("*.py")]

    offenders = []
    for path in sources:
        text = path.read_text(encoding="utf-8")
        if "GraphEngine.compile(" not in text:
            continue
        if path.name == "15_graph_from_json.py":
            continue  # Loads the versioned output written by example 14.
        if '"schema_version":' not in text:
            offenders.append(path.relative_to(ROOT).as_posix())

    assert not offenders, f"unversioned Python topology examples: {offenders}"


def test_cpp_examples_that_build_topologies_declare_schema_version():
    sources = [
        path
        for path in (ROOT / "examples").rglob("*.cpp")
        if path.is_file() and "third_party" not in path.parts
    ]
    externally_versioned = {
        "examples/cookbook/jarvis/src/main.cpp",  # Loads jarvis_graph.json.
    }
    generated_and_gated = {
        "examples/cookbook/the-beast/the_beast_apex.cpp",
        "examples/cookbook/the-beast/the_beast_forge.cpp",
        "examples/cookbook/the-beast/the_beast_live.cpp",
    }
    minimum_markers = {
        "examples/09_all_features.cpp": 5,
        "examples/cookbook/multi_tenant_chatbot/server.cpp": 3,
        "examples/cookbook/multi_tenant_chatbot/server_live_llm.cpp": 3,
        "examples/cookbook/self_evolving_chatbot/server.cpp": 3,
        "examples/cookbook/self_evolving_chatbot/server_multi.cpp": 3,
        "examples/cookbook/the-beast/the_beast_gate_eval.cpp": 5,
    }

    offenders = []
    for path in sources:
        text = path.read_text(encoding="utf-8")
        if "GraphEngine::build(" not in text:
            continue
        relative = path.relative_to(ROOT).as_posix()
        if relative in externally_versioned:
            continue
        if relative in generated_and_gated:
            if "schema_version must match TOPOLOGY_SCHEMA_VERSION" not in text:
                offenders.append(f"{relative} (generated schema gate missing)")
            continue
        marker_count = text.count('{"schema_version"') + text.count('"schema_version":')
        required = minimum_markers.get(relative, 1)
        if marker_count < required:
            offenders.append(f"{relative} ({marker_count} < {required})")

    assert not offenders, f"unversioned C++ topology examples: {offenders}"


def test_generated_factories_and_shipped_json_use_current_schema_version():
    generated_sources = [
        ROOT / "src/core/react_graph.cpp",
        ROOT / "src/core/plan_execute_graph.cpp",
        ROOT / "src/core/deep_research_graph.cpp",
    ]
    for path in generated_sources:
        text = path.read_text(encoding="utf-8")
        assert "TOPOLOGY_SCHEMA_VERSION" in text, path.relative_to(ROOT)

    graph_files = list(
        (ROOT / "examples/cookbook/jarvis").rglob("jarvis_graph.json")
    )
    assert graph_files
    for path in graph_files:
        definition = json.loads(path.read_text(encoding="utf-8"))
        assert definition["schema_version"] == getattr(
            ng, "TOPOLOGY_SCHEMA_VERSION"
        ), path


def test_unversioned_compatibility_boundary_is_documented():
    policy = (ROOT / "docs/troubleshooting.md").read_text(encoding="utf-8")
    assert "Every `0.x` release" in policy
    assert "planned `1.0.0` boundary" in policy
    assert "absent or zero versions" in policy
