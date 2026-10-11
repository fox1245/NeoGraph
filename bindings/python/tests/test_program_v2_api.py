"""Program API v2 through the Python binding (#362-#365).

Each section is one work package: the self-repair diagnostic contract (X1),
the capability manifest (X4), graph metadata (#362), any_of join slots (#363),
witness paths (X2) and the failure-routing Core keys (#364).
"""

import hashlib
import json
import pathlib

import pytest

import neograph_engine as ng

pytestmark = pytest.mark.skipif(
    not getattr(ng, "_HAVE_PROGRAM", True), reason="Program component not built"
)

_REPO = pathlib.Path(__file__).resolve().parents[3]


def _digest(text):
    return "sha256:" + hashlib.sha256(text.encode()).hexdigest()


class _Task(ng.GraphNode):
    def __init__(self, name, config):
        super().__init__()
        self._name = name
        self._config = dict(config)

    def get_name(self):
        return self._name

    def run(self, _input):
        if self._config.get("boom"):
            raise RuntimeError("boom")
        return [ng.ChannelWrite("trace", [self._name])]


def _registry():
    ng.NodeFactory.register_type(
        "v2.task", lambda name, config, _ctx: _Task(name, config)
    )
    ng.ConditionRegistry.register_condition("v2.always_fail", lambda _s: "fail")
    ng.ConditionRegistry.register_condition("v2.always_pass", lambda _s: "pass")
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node("v2.task", "1.0.0", _digest("v2.task"))
    builder.add_registered_condition("v2.always_fail", "1.0.0", _digest("fail"))
    builder.add_registered_condition("v2.always_pass", "1.0.0", _digest("pass"))
    builder.add_registered_reducer("append", "1.0.0", _digest("append"))
    builder.add_registered_reducer("overwrite", "1.0.0", _digest("overwrite"))
    return builder.build()


_BASE = dict(
    max_child_depth=4,
    max_concurrency=4,
    max_core_steps=100,
    max_dynamic_compiles=0,
    max_program_operations=32,
    max_total_children=4,
    model_tokens=10000,
    monetary_microunits=1000000,
    wall_time_ms=60000,
)


def _budget(**overrides):
    value = ng.ProgramRunBudget()
    for key, item in {**_BASE, **overrides}.items():
        setattr(value, key, item)
    return value


def _compile(js, **kwargs):
    compiler = ng.ProgramCompiler(_registry(), "python-v2-test/v1", **kwargs)
    return compiler.compile(ng.ProgramSource.from_javascript("v2.js", js))


def _compile_error(js, **kwargs):
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compile(js, **kwargs)
    return raised.value.diagnostics


def _run(js):
    host = ng.LocalProgramHost(
        _registry(), "owner:v2", _budget(max_dynamic_compiles=1), "python-v2-host/v1"
    )
    version = host.compile_admit(ng.ProgramSource.from_javascript("v2.js", js), _budget())
    return host, version, host.run(version, {}, _budget())


def _graph(body):
    return (
        "export function define() {\n"
        '  return ng.graph("g").channel("trace", {reducer: "append", initial: []})'
        + body
        + ";\n}\n"
    )


# ---------------------------------------------------------------------------
# X1: self-repair diagnostic contract
# ---------------------------------------------------------------------------


def test_compile_errors_carry_help_with_candidates_and_a_suggestion():
    diagnostics = _compile_error(
        _graph(
            '.node("Work", {type: "v2.task"}).node("Done", {type: "v2.task"})'
            '.entry("Work").edge("Work", "Dnoe").exit("Done")'
        )
    )
    unknown = [d for d in diagnostics if d["code"] == "P_CORE_E3"]
    assert len(unknown) == 1
    help = unknown[0]["help"]
    assert help["doc"] == "docs/diagnostics.md#p_core_e3"
    assert help["candidates"] == ["Done"]
    suggestion = help["suggestions"][0]
    assert suggestion["applicability"] == "maybe"
    assert suggestion["edits"] == [
        {"op": "replace", "path": "/root/definition/edges/1/to", "value": "Done"}
    ]


def test_returned_diagnostics_have_help_and_sealed_ones_do_not():
    bundle = _compile(
        _graph(
            '.node("A", {type: "v2.task"}).node("B", {type: "v2.task"})'
            '.node("C", {type: "v2.task"}).node("D", {type: "v2.task"})'
            '.entry("A").edge("A", "B").edge("A", "C").edge("B", "D").edge("C", "D")'
            '.exit("D")'
        )
    )
    returned = {d["code"]: d for d in bundle.compile_diagnostics}
    fan_in = returned["P_CORE_E9"]["help"]
    assert fan_in["suggestions"][0]["edits"][0]["value"] == {"wait_for": ["B", "C"]}
    assert all(d["help"] is None for d in bundle.diagnostics)
    assert ng.ProgramBundle.parse(bundle.serialize_canonical()).id == bundle.id


def test_admission_and_runtime_exceptions_link_their_documentation():
    host = ng.LocalProgramHost(
        _registry(), "owner:v2-help", _budget(max_dynamic_compiles=1), "python-v2-host/v1"
    )
    with pytest.raises(ng.ProgramDiagnosticError) as raised:
        host.resume("no-such-run", {})
    diagnostic = raised.value.diagnostics[0]
    assert diagnostic["help"]["doc"].startswith("docs/diagnostics.md#")


# ---------------------------------------------------------------------------
# X4: capability manifest and schemas
# ---------------------------------------------------------------------------


def _schema(name):
    with open(_REPO / "schemas" / name, encoding="utf-8") as handle:
        return json.load(handle)


def test_capability_manifest_validates_against_its_schema():
    jsonschema = pytest.importorskip("jsonschema")
    manifest = ng.javascript_authoring_capability_manifest()
    jsonschema.Draft202012Validator(
        _schema("javascript-capability-manifest-v1.schema.json")
    ).validate(manifest)
    for method in manifest["define"]["graph_builder_methods"]:
        assert manifest["features"][method["feature"]] is True, method["name"]
        for argument in method["arguments"]:
            jsonschema.Draft202012Validator.check_schema(argument["schema"])


def test_compiled_definitions_validate_against_the_core_schema():
    jsonschema = pytest.importorskip("jsonschema")
    validator = jsonschema.Draft202012Validator(_schema("core-definition-v1.schema.json"))
    bundle = _compile(
        _graph(
            '.node("A", {type: "v2.task"}).node("B", {type: "v2.task"})'
            '.entry("A").conditionalEdge("A", "v2.always_pass", {pass: "B", fail: "__end__"})'
            '.exit("B").retryPolicy({max_retries: 1})'
        )
    )
    definition = bundle.sealed_core_definitions[0]["definition"]
    validator.validate(definition)
    golden = json.loads(
        (_REPO / "tests" / "fixtures" / "identity_corpus" / "golden.json").read_text("utf-8")
    )
    for entry in golden["entries"]:
        validator.validate(entry["definition"])
    broken = dict(definition, on_exhausted="B")
    assert not validator.is_valid(broken)


# ---------------------------------------------------------------------------
# #362: graph metadata
# ---------------------------------------------------------------------------


_METADATA_JS = _graph(
    '.metadata({owner: "agent:planner", requirements: ["REQ-12"]})'
    '.node("A", {type: "v2.task"}).entry("A").exit("A")'
)


def test_issue_362_repro_compiles_and_hosts_read_the_metadata():
    bundle = _compile(_METADATA_JS)
    definition = bundle.sealed_core_definitions[0]["definition"]
    assert definition["metadata"] == {"owner": "agent:planner", "requirements": ["REQ-12"]}

    host = ng.LocalProgramHost(
        _registry(), "owner:v2-meta", _budget(max_dynamic_compiles=1), "python-v2-host/v1"
    )
    hosted = host.compile(ng.ProgramSource.from_javascript("v2.js", _METADATA_JS), _budget())
    assert hosted.sealed_core_definitions[0]["definition"]["metadata"]["owner"] == "agent:planner"


def test_metadata_is_never_part_of_run_state_or_output():
    _host, _version, result = _run(_METADATA_JS)
    assert result.status == ng.ProgramTerminalStatus.Completed
    text = json.dumps(result.output)
    assert "agent:planner" not in text and "REQ-12" not in text


def test_metadata_errors_name_the_rule_they_break():
    diagnostics = _compile_error(
        _graph('.metadata({owner: "a\\u200bb"}).node("A", {type: "v2.task"}).entry("A").exit("A")')
    )
    hidden = [d for d in diagnostics if d["code"] == "P_CORE_GC_METADATA_TEXT"]
    assert hidden and hidden[0]["witness"]["code_points"] == ["U+200B"]
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compile(_graph('.metadata({a: 1}).metadata({b: 2}).node("A", {type: "v2.task"}).entry("A")'))
    assert raised.value.diagnostics[0]["code"] == "P_JS_GRAPH_ARGUMENT"

