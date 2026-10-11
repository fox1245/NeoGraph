"""Scenario 5: diagnostics a model (or a person) can act on."""

import pytest

import neograph_engine as ng

from .support import (admit, budget, build_registry, ceiling, define_graph, digest,
                      make_host, source)


def _compiler():
    return ng.ProgramCompiler(build_registry())


def _line_of(text, needle):
    for number, line in enumerate(text.splitlines(), start=1):
        if needle in line:
            return number
    raise AssertionError(f"{needle!r} not in source")


def test_syntax_error_reports_source_line_and_column():
    text = (
        "export function define() {\n"
        '  return ng.graph("main")\n'
        '    .node("A", {type: "e2e.step" goal: "x"})\n'
        '    .entry("A").exit("A");\n'
        "}\n"
    )
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compiler().compile(source(text, "agent.js"))
    error = raised.value
    assert isinstance(error, RuntimeError)
    assert "agent.js:3:" in str(error)
    [diagnostic] = error.diagnostics
    assert diagnostic["code"] == "P_JS_EVALUATION"
    assert diagnostic["severity"] == "error"
    assert "SyntaxError" in diagnostic["message"]
    assert diagnostic["source_id"] == "agent.js"
    assert diagnostic["span"]["line_begin"] == 3
    assert diagnostic["span"]["column_begin"] > 1
    assert diagnostic["witness"]["engine"] == "quickjs"


def test_reference_error_in_define_points_at_the_line():
    text = (
        "export function define() {\n"
        '  const g = ng.graph("main");\n'
        '  g.node("A", {type: "e2e.step"});\n'
        "  helperThatDoesNotExist(g);\n"
        "  return g;\n"
        "}\n"
    )
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compiler().compile(source(text, "agent.js"))
    [diagnostic] = raised.value.diagnostics
    assert diagnostic["code"] == "P_JS_EVALUATION"
    assert "helperThatDoesNotExist" in diagnostic["message"]
    assert diagnostic["span"]["line_begin"] == 4
    assert "agent.js:4:" in str(raised.value)


def test_unknown_node_type_points_at_the_node_call():
    text = define_graph("""
        g.node("A", {type: "e2e.step"});
        g.node("B", {type: "e2e.misspelled"});
        g.entry("A");
        g.edge("A", "B");
        g.exit("B");
    """)
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compiler().compile(source(text, "agent.js"))
    [diagnostic] = raised.value.diagnostics
    assert diagnostic["code"] == "P_CORE_GC_REGISTRY_MISS"
    assert diagnostic["json_pointer"] == "/root/definition/nodes/B/type"
    assert diagnostic["witness"]["name"] == "e2e.misspelled"
    line = _line_of(text, '"e2e.misspelled"')
    assert diagnostic["span"]["line_begin"] == line
    assert f"agent.js:{line}:" in str(raised.value)


def test_condition_missing_from_the_program_registry_points_at_the_edge():
    text = define_graph("""
        g.node("A", {type: "e2e.step"});
        g.node("B", {type: "e2e.step"});
        g.entry("A");
        g.conditionalEdge("A", "e2e.not_in_registry", {go: "B"});
        g.exit("B");
    """)
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compiler().compile(source(text, "agent.js"))
    [diagnostic] = raised.value.diagnostics
    assert diagnostic["code"] == "P_CORE_GC_REGISTRY_MISS"
    assert diagnostic["json_pointer"] == "/root/definition/conditional_edges/0/condition"
    assert diagnostic["span"]["line_begin"] == _line_of(text, "e2e.not_in_registry")


def test_unregistered_condition_and_reducer_hints_name_the_python_call():
    builder = ng.ProgramRegistryBuilder()
    with pytest.raises(RuntimeError) as condition:
        builder.add_registered_condition("e2e.never_registered", "1.0.0", digest("c"))
    assert "ng.ConditionRegistry.register_condition(name, fn)" in str(condition.value)
    assert "::instance()" not in str(condition.value)
    assert "e2e.never_registered" in str(condition.value)

    with pytest.raises(RuntimeError) as reducer:
        builder.add_registered_reducer("e2e.never_registered", "1.0.0", digest("r"))
    assert "ng.ReducerRegistry.register_reducer(name, fn)" in str(reducer.value)
    assert "::instance()" not in str(reducer.value)


FAN_IN = define_graph("""
    g.node("A", {type: "e2e.step"});
    g.node("B", {type: "e2e.step"});
    g.node("C", {type: "e2e.step"});
    g.entry("A");
    g.edge("A", "B");
    g.edge("A", "C");
    g.edge("B", "C");
    g.exit("C");
""")


def test_lint_warnings_carry_call_sites_while_the_sealed_copy_is_spanless():
    compiler = _compiler()
    bundle = compiler.compile(source(FAN_IN, "fan_in.js"))

    returned = {(d["code"], d["json_pointer"]): d for d in bundle.compile_diagnostics}
    e9 = returned[("P_CORE_E9", "/root/definition/nodes/C")]
    assert e9["severity"] == "warning"
    assert e9["span"]["line_begin"] == _line_of(FAN_IN, 'g.node("C"')

    # The sealed copy keeps the rules bundles were first sealed under, so ids do
    # not change (#358, #361): it still reports E6 for the channels that only
    # Python nodes write, which the returned diagnostics no longer do.
    sealed = bundle.diagnostics
    sealed_codes = {d["code"] for d in sealed}
    returned_codes = {d["code"] for d in bundle.compile_diagnostics}
    assert "P_CORE_E9" in sealed_codes
    assert "P_CORE_E6" in sealed_codes and "P_CORE_E6" not in returned_codes
    assert all(d["span"] is None and d["json_pointer"] == "" for d in sealed)

    serialized = bundle.serialize_canonical()
    assert "line_begin" not in serialized
    parsed = ng.ProgramBundle.parse(serialized)
    assert parsed.id == bundle.id
    assert parsed.compile_diagnostics == sealed
    # Call-site spans are not part of the identity: a recompile is identical.
    assert compiler.compile(source(FAN_IN, "fan_in.js")).id == bundle.id


def test_control_character_in_a_config_string_warns_and_string_raw_fixes_it():
    regex_by_mistake = define_graph(r"""
        g.node("A", {type: "e2e.step", pattern: "\b[1-9]\d* failed"});
        g.entry("A");
        g.exit("A");
    """)
    bundle = _compiler().compile(source(regex_by_mistake, "agent.js"))

    warnings = [d for d in bundle.compile_diagnostics if d["code"] == "P_JS_CONTROL_CHARACTER"]
    assert len(warnings) == 1, bundle.compile_diagnostics
    [warning] = warnings
    assert warning["severity"] == "warning"
    assert warning["json_pointer"] == "/root/definition/nodes/A/pattern"
    assert warning["span"]["line_begin"] == _line_of(regex_by_mistake, "pattern:")
    assert "P_JS_CONTROL_CHARACTER" not in {d["code"] for d in bundle.diagnostics}
    stored = bundle.sealed_core_definitions[0]["definition"]["nodes"]["A"]["pattern"]
    assert stored.startswith("\x08"), repr(stored)

    fixed = regex_by_mistake.replace(r'"\b[1-9]\d* failed"', r'String.raw`\b[1-9]\d* failed`')
    clean = _compiler().compile(source(fixed, "agent.js"))
    assert "P_JS_CONTROL_CHARACTER" not in {d["code"] for d in clean.compile_diagnostics}
    assert clean.sealed_core_definitions[0]["definition"]["nodes"]["A"]["pattern"] == r"\b[1-9]\d* failed"


def test_admission_prints_no_lint_lines_to_stderr(capfd):
    host = make_host()
    capfd.readouterr()
    admit(host, FAN_IN)
    captured = capfd.readouterr()
    assert "[neograph] lint" not in captured.err, captured.err


def test_admission_failure_exposes_its_diagnostics():
    # The Program compiles, but its run budget asks for more Core steps than
    # the host's policy ceiling allows; only admission can see that.
    host = make_host(budget_ceiling=ceiling(max_core_steps=10))
    text = define_graph('g.node("A", {type: "e2e.step"}); g.entry("A"); g.exit("A");')

    with pytest.raises(RuntimeError) as raised:
        host.compile_admit(source(text), budget(max_core_steps=50))
    error = raised.value
    diagnostics = getattr(error, "diagnostics", None)
    assert diagnostics, f"no diagnostics on {type(error).__name__}: {error}"
    assert any("P_ADMIT_POLICY" in str(d) and "max_core_steps" in str(d) for d in diagnostics), \
        diagnostics
