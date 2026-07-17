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
