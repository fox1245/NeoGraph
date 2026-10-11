import pytest

import neograph_engine as ng


CASES = [
    pytest.param(
        field,
        valid_empty,
        wrong_empty,
        schema_version,
        expected,
        schema_version is not None or expected == "object",
        id=f"{mode}-{field}",
    )
    for mode, schema_version in (("legacy", None), ("strict", 1))
    for field, valid_empty, wrong_empty, expected in (
        ("channels", {}, [], "object"),
        ("nodes", {}, [], "object"),
        ("edges", [], {}, "array"),
        ("conditional_edges", [], {}, "array"),
    )
]


@pytest.mark.parametrize(
    "field,valid_empty,wrong_empty,schema_version,expected,rejects_wrong", CASES
)
def test_compile_rejects_wrong_top_level_container(
    field, valid_empty, wrong_empty, schema_version, expected, rejects_wrong
):
    definition = {field: wrong_empty}
    if schema_version is not None:
        definition["schema_version"] = schema_version

    if rejects_wrong:
        with pytest.raises(RuntimeError, match=rf"\$\.{field}.*{expected}"):
            ng.GraphEngine.compile(definition, ng.NodeContext())
    else:
        ng.GraphEngine.compile(definition, ng.NodeContext())


@pytest.mark.parametrize(
    "field,valid_empty,wrong_empty,schema_version,expected,rejects_wrong", CASES
)
def test_compile_accepts_valid_empty_top_level_container(
    field, valid_empty, wrong_empty, schema_version, expected, rejects_wrong
):
    definition = {field: valid_empty}
    if schema_version is not None:
        definition["schema_version"] = schema_version

    ng.GraphEngine.compile(definition, ng.NodeContext())


@pytest.mark.parametrize("schema_version", [None, 1], ids=["legacy", "strict"])
@pytest.mark.parametrize("field", ["edges", "conditional_edges"])
@pytest.mark.parametrize("value", [None, "not a container", 7, True])
def test_edge_fields_reject_scalar_values(field, value, schema_version):
    definition = {field: value}
    if schema_version is not None:
        definition["schema_version"] = schema_version

    with pytest.raises(RuntimeError, match=rf"\$\.{field}.*array"):
        ng.GraphEngine.compile(definition, ng.NodeContext())


@pytest.mark.parametrize(
    "field,entry",
    [
        ("edges", {"direct": {"from": ng.START_NODE, "to": ng.END_NODE}}),
        ("conditional_edges", {
            "branch": {
                "from": ng.START_NODE,
                "condition": "route",
                "routes": {"done": ng.END_NODE},
            }
        }),
    ],
)
def test_legacy_edge_fields_keep_keyed_object_compatibility(field, entry):
    definition = {
        "nodes": {},
        field: entry,
    }
    ng.GraphEngine.compile(definition, ng.NodeContext())


LENIENT_HINT = "Remove 'schema_version'"


@pytest.mark.parametrize("schema_version", [None, 1], ids=["legacy", "strict"])
@pytest.mark.parametrize(
    "case,nodes,edges,fix",
    [
        ("reserved-node", {"__start__": {"type": "a"}, "a": {"type": "a"}},
         [{"from": "__start__", "to": "a"}], "Rename the node"),
        ("no-entry", {"a": {"type": "a"}},
         [{"from": "a", "to": "__end__"}], 'Add an edge from "__start__"'),
    ],
)
def test_every_version_errors_say_what_to_fix(case, nodes, edges, fix, schema_version):
    # Issue #375: lenient parsing does not relax these rules, so removing
    # schema_version cannot help; the error says what to change instead.
    definition = {"nodes": nodes, "edges": edges}
    if schema_version is not None:
        definition["schema_version"] = schema_version
    with pytest.raises(RuntimeError) as raised:
        ng.GraphEngine.compile(definition, ng.NodeContext())
    message = str(raised.value)
    assert fix in message, message
    assert LENIENT_HINT not in message, message


def test_unknown_keys_keep_the_lenient_hint():
    definition = {
        "schema_version": 1,
        "nodes": {},
        "edgez": [],
    }
    with pytest.raises(RuntimeError, match=LENIENT_HINT):
        ng.GraphEngine.compile(definition, ng.NodeContext())
