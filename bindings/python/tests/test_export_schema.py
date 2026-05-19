"""Python side of the topology schema export (issue #56).

`neograph_engine.export_schema()` returns the same document the C++
`NodeFactory::export_schema()` / `example_export_schema` emits — the
drift-proof palette source the visual block editor consumes. These
tests pin the shape, the built-in catalog, and that custom node types
registered from Python show up (so an embedder's palette is complete).
"""
from __future__ import annotations

import neograph_engine as ng


def test_document_shape():
    doc = ng.export_schema()
    assert isinstance(doc, dict)

    assert isinstance(doc["neograph_version"], str)
    assert doc["neograph_version"]                       # non-empty
    assert doc["neograph_version"] != "unknown"          # CMake stamp reached the TU
    assert doc["$schema"] == "https://json-schema.org/draft/2020-12/schema"

    topo = doc["topology"]
    assert topo["type"] == "object"
    assert "nodes" in topo["required"]
    for k in ("name", "channels", "nodes", "edges",
              "conditional_edges", "interrupt_before",
              "interrupt_after", "retry_policy"):
        assert k in topo["properties"], k

    for t in ("llm_call", "tool_dispatch", "intent_classifier", "subgraph"):
        assert t in doc["node_types"], t

    assert "overwrite" in doc["reducers"]
    assert "append" in doc["reducers"]
    assert "has_tool_calls" in doc["conditions"]
    assert "route_channel" in doc["conditions"]


def test_builtin_intent_classifier_schema():
    nt = ng.export_schema()["node_types"]
    assert "routes" in nt["intent_classifier"]["required"]
    assert "prompt" in nt["intent_classifier"]["properties"]
    assert "definition" in nt["subgraph"]["required"]


def test_matches_cpp_version():
    # The dict is produced by the same C++ call as __version__'s source
    # (both read pyproject.toml via CMake). They must agree.
    assert ng.export_schema()["neograph_version"] == ng.__version__


def test_python_registered_node_appears_in_palette():
    # A custom node registered from Python must surface in the editor
    # palette — otherwise an embedder's blocks would be invisible.
    class _Probe(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            return []

    ng.NodeFactory.register_type(
        "py_palette_probe",
        lambda name, config, ctx: _Probe(name),
    )

    nt = ng.export_schema()["node_types"]
    assert "py_palette_probe" in nt
    # Registered without a declared schema -> permissive default object.
    assert nt["py_palette_probe"]["type"] == "object"
