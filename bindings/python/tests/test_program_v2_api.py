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
        # Every builder method's feature compiles and runs.
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


# ---------------------------------------------------------------------------
# #363: any_of join slots
# ---------------------------------------------------------------------------


def _issue_357(condition, wait_for='[{any_of: ["B", "FIX"]}, "C"]', extra=""):
    return _graph(
        '.node("A", {type: "v2.task"}).node("B", {type: "v2.task"})'
        '.node("C", {type: "v2.task"}).node("FIX", {type: "v2.task"})'
        '.node("D", {type: "v2.task"})'
        '.entry("A").edge("A", "B").edge("A", "C")'
        f'.conditionalEdge("B", "{condition}", {{pass: "D", fail: "FIX"}})'
        f'.edge("FIX", "D").edge("C", "D").barrier("D", {wait_for}).exit("D")' + extra
    )


@pytest.mark.parametrize("condition", ["v2.always_pass", "v2.always_fail"])
def test_issue_357_slot_join_completes_on_both_routes(condition):
    _host, _version, result = _run(_issue_357(condition))
    assert result.status == ng.ProgramTerminalStatus.Completed, result.failure
    assert result.execution_trace[-1] == "D"
    assert result.execution_trace.count("D") == 1
    if condition == "v2.always_fail":
        assert "FIX" in result.execution_trace


def test_issue_357_legacy_join_still_stalls_on_the_fail_route():
    _host, _version, result = _run(_issue_357("v2.always_fail", wait_for='["B", "C"]'))
    assert result.status == ng.ProgramTerminalStatus.Failed
    assert result.failure.code == "P_CORE_BARRIER_STALLED"


def test_slot_join_interrupt_before_then_resume():
    host, _version, first = _run(_issue_357("v2.always_fail", extra='.interruptBefore("D")'))
    assert first.status == ng.ProgramTerminalStatus.Interrupted
    resumed = host.resume(first.run_id, {}).wait()
    assert resumed.status == ng.ProgramTerminalStatus.Completed, resumed.failure
    assert resumed.execution_trace[-1] == "D"


def test_singleton_slot_hashes_like_the_legacy_member():
    legacy = _compile(_issue_357("v2.always_pass", wait_for='["B", "C"]'))
    wrapped = _compile(_issue_357("v2.always_pass", wait_for='[{any_of: ["B"]}, "C"]'))
    assert (
        wrapped.sealed_core_definitions[0]["definition_hash"]
        == legacy.sealed_core_definitions[0]["definition_hash"]
    )


# ---------------------------------------------------------------------------
# X2: witness paths, the any_of suggestion and the route table
# ---------------------------------------------------------------------------


def test_e15_suggests_the_slot_that_lets_the_fix_stand_in():
    bundle = _compile(_issue_357("v2.always_fail", wait_for='["B", "C"]'))
    bypass = [d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_E15"][0]
    assert bypass["witness"]["paths"] == {"fail": ["B", "FIX", "D"]}
    suggestion = bypass["help"]["suggestions"][0]
    assert suggestion["applicability"] == "maybe"
    assert suggestion["edits"][0]["value"] == [{"any_of": ["B", "FIX"]}, "C"]
    assert suggestion["message"].startswith("Let 'FIX' stand in for 'B': graph.barrier(")


def test_stalled_run_witness_carries_the_route_from_the_trace():
    _host, _version, result = _run(_issue_357("v2.always_fail", wait_for='["B", "C"]'))
    assert result.failure.code == "P_CORE_BARRIER_STALLED"
    stall = result.failure.witness["stalled_barriers"][0]
    assert stall["missing"] == ["B"]
    assert stall["paths"] == {"B": ["B", "FIX"]}
    assert stall["suggested_wait_for"] == [{"any_of": ["B", "FIX"]}, "C"]


def test_echo_routes_returns_the_routed_successor_table():
    bundle = _compile(_issue_357("v2.always_pass"), echo_routes=True)
    notes = [d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_ROUTES"]
    assert len(notes) == 1 and notes[0]["severity"] == "note"
    assert notes[0]["witness"]["routes"]["B"] == ["D", "FIX"]
    assert all(d["code"] != "P_CORE_ROUTES" for d in bundle.diagnostics)


# ---------------------------------------------------------------------------
# #364: failure routing keys (compile time; program_e2e/test_09 runs them)
# ---------------------------------------------------------------------------


_FIX_LOOP = (
    "export function define() {\n"
    '  return ng.graph("fix").channel("failures", {reducer: "append", initial: []})'
    '.node("IMPLEMENT", {type: "v2.task"}).node("TEST", {type: "v2.task"})'
    '.node("ASK_USER", {type: "v2.task"}).node("ABORT", {type: "v2.task"})'
    '.entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")'
    '.conditionalEdge("ASK_USER", "v2.always_pass", {pass: "IMPLEMENT", fail: "ABORT"})'
    '.retryPolicy("TEST", {max_retries: 2, non_retryable: ["app.compile_error"], max_visits: 3,'
    ' on_failure: "ASK_USER", error_channel: "failures"})'
    '.interruptBefore("ASK_USER").failureExit("ABORT", {code: "plan.user_abort"})%s;\n'
    "}\n"
)


def test_retry_policy_sugar_lowers_to_the_failure_routing_keys():
    bundle = _compile(_FIX_LOOP % "")
    definition = bundle.sealed_core_definitions[0]["definition"]
    assert definition["node_retry_policies"] == {
        "TEST": {"max_retries": 2, "max_visits": 3, "non_retryable": ["app.compile_error"]}
    }
    assert definition["failure_edges"] == [
        {"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}
    ]
    assert definition["failure_exits"] == {"ABORT": {"code": "plan.user_abort"}}
    assert bundle.diagnostics == []
    features = ng.javascript_authoring_capability_manifest()["features"]
    assert features["retry_policy.per_node"] is True
    assert features["failure_edges"] is True
    assert features["failure_exits"] is True


def test_failure_routing_errors_reach_python_with_their_codes():
    diagnostics = _compile_error(_FIX_LOOP % '.retryPolicy("ASK_USER", {max_retries: 11})')
    bound = [d for d in diagnostics if d["code"] == "P_CORE_GC_FIELD_VALUE"]
    assert bound and bound[0]["witness"]["max"] == 10
    diagnostics = _compile_error(
        _FIX_LOOP % '.retryPolicy("IMPLEMENT", {on_failure: "__end__"})'
    )
    edge = [d for d in diagnostics if d["code"] == "P_CORE_GC_FAILURE_EDGE"]
    assert edge and edge[0]["witness"]["reason"] == "sentinel_target"


# ---------------------------------------------------------------------------
# X3: the opt-in soundness check
# ---------------------------------------------------------------------------


def _findings(bundle, code):
    return [d for d in bundle.compile_diagnostics if d["code"] == code]


def test_soundness_check_returns_witness_traces_and_never_seals_them():
    source = _issue_357("v2.always_fail", wait_for='["B", "C"]')
    plain = _compile(source)
    checked = _compile(source, soundness_check=True)
    assert checked.id == plain.id
    assert checked.serialize_canonical() == plain.serialize_canonical()
    assert not any(d["code"].startswith("P_CORE_S") for d in checked.diagnostics)
    assert not _findings(plain, "P_CORE_SOUNDNESS")

    stall = _findings(checked, "P_CORE_S2")
    assert len(stall) == 1 and stall[0]["severity"] == "warning"
    witness = stall[0]["witness"]
    assert witness["kind"] == "stalled"
    assert witness["trace"] == [
        {"ready": ["A"], "routes": {}},
        {"ready": ["B", "C"], "routes": {"B": "fail"}},
        {"ready": ["FIX"], "routes": {}},
    ]
    assert witness["received"] == ["C", "FIX"] and witness["missing"] == ["B"]
    assert witness["suggested_wait_for"] == [{"any_of": ["B", "FIX"]}, "C"]
    help = stall[0]["help"]
    assert help["doc"] == "docs/diagnostics.md#p_core_s2"
    assert help["suggestions"][0]["edits"][0]["value"] == [{"any_of": ["B", "FIX"]}, "C"]
    assert help["related"][0]["label"] == "'B' takes route 'fail' here"

    dead_end = _findings(checked, "P_CORE_S1")
    assert len(dead_end) == 1
    assert dead_end[0]["witness"]["state"] == {"ready": ["FIX"], "pending": {"D": ["C"]}}

    summary = _findings(checked, "P_CORE_SOUNDNESS")
    assert len(summary) == 1 and summary[0]["severity"] == "note"
    assert summary[0]["witness"]["status"] == "checked"
    assert summary[0]["witness"]["outcomes"] == ["completed", "stalled"]
    assert ng.javascript_authoring_capability_manifest()["features"]["diagnostics.soundness_check"]


def test_soundness_check_finds_nothing_once_the_slot_lets_fix_stand_in():
    bundle = _compile(_issue_357("v2.always_fail"), soundness_check=True)
    assert not [d for d in bundle.compile_diagnostics if d["code"].startswith("P_CORE_S") and
                d["code"] != "P_CORE_SOUNDNESS"]
    summary = _findings(bundle, "P_CORE_SOUNDNESS")[0]
    assert summary["severity"] == "note" and summary["witness"]["outcomes"] == ["completed"]


def test_soundness_stall_finding_matches_the_run_through_local_program_host():
    # The S2 witness is the run the runtime takes on the fail route.
    host = ng.LocalProgramHost(
        _registry(), "owner:v2-soundness", _budget(max_dynamic_compiles=1),
        "python-v2-host/v1", soundness_check=True,
    )
    source = ng.ProgramSource.from_javascript(
        "v2.js", _issue_357("v2.always_fail", wait_for='["B", "C"]')
    )
    bundle = host.compile(source, _budget())
    stall = _findings(bundle, "P_CORE_S2")[0]["witness"]
    result = host.run(host.admit(bundle), {}, _budget())
    assert result.failure.code == "P_CORE_BARRIER_STALLED"
    stalled = result.failure.witness["stalled_barriers"][0]
    assert stalled["node"] == stall["barrier"] == "D"
    assert stalled["received"] == stall["received"]
    assert stalled["missing"] == stall["missing"]
    ran = [node for step in stall["trace"] for node in step["ready"]]
    assert sorted(result.execution_trace) == sorted(ran)


def test_soundness_check_models_failure_edges_and_failure_exits():
    bundle = _compile(_FIX_LOOP % "", soundness_check=True)
    assert not [d for d in bundle.compile_diagnostics if d["code"].startswith("P_CORE_S") and
                d["code"] != "P_CORE_SOUNDNESS"]
    summary = _findings(bundle, "P_CORE_SOUNDNESS")[0]["witness"]
    assert summary["status"] == "checked"
    assert summary["outcomes"] == ["completed", "failure_exit", "step_limit"]


def test_soundness_state_cap_reports_not_checked():
    bundle = _compile(
        _issue_357("v2.always_fail", wait_for='["B", "C"]'),
        soundness_check=True,
        soundness_state_cap=2,
    )
    summary = _findings(bundle, "P_CORE_SOUNDNESS")
    assert len(summary) == 1 and summary[0]["severity"] == "warning"
    assert summary[0]["witness"]["status"] == "not_checked"
    assert not _findings(bundle, "P_CORE_S1")
