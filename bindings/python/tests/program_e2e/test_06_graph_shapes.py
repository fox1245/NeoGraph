"""Scenario 6: graph shapes built with the define() builder."""

import pytest

import neograph_engine as ng

from .support import (JOURNAL, budget, build_registry, channel, define_graph, describe,
                      make_host, run_program, source, trace_channel)


def test_fan_out_and_barrier_join():
    text = define_graph("""
        g.node("plan", {type: "e2e.step"});
        g.node("search_web", {type: "e2e.step", delay_ms: 30});
        g.node("search_docs", {type: "e2e.step"});
        g.node("merge", {type: "e2e.step"});
        g.entry("plan");
        g.edge("plan", "search_web");
        g.edge("plan", "search_docs");
        g.edge("search_web", "merge");
        g.edge("search_docs", "merge");
        g.barrier("merge", ["search_web", "search_docs"]);
        g.exit("merge");
    """)
    host, version, result = run_program(text)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    trace = result.execution_trace
    assert trace[0] == "plan" and trace[-1] == "merge"
    assert sorted(trace[1:3]) == ["search_docs", "search_web"]
    assert JOURNAL.names().count("merge") == 1, "the barrier join runs exactly once"
    bundle = host.compile(source(text), budget())
    merge = bundle.sealed_core_definitions[0]["definition"]["nodes"]["merge"]
    # The sealed definition is canonical: wait_for is stored sorted.
    assert merge["barrier"] == {"wait_for": ["search_docs", "search_web"]}


def test_fan_in_without_barrier_is_flagged_by_e9():
    text = define_graph("""
        g.node("plan", {type: "e2e.step"});
        g.node("left", {type: "e2e.step"});
        g.node("right", {type: "e2e.step"});
        g.node("merge", {type: "e2e.step"});
        g.entry("plan");
        g.edge("plan", "left");
        g.edge("plan", "right");
        g.edge("left", "merge");
        g.edge("right", "merge");
        g.exit("merge");
    """)
    bundle = ng.ProgramCompiler(build_registry()).compile(source(text))
    e9 = [d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_E9"]
    assert [d["json_pointer"] for d in e9] == ["/root/definition/nodes/merge"]
    assert "barrier" in e9[0]["message"]


@pytest.mark.parametrize("topic, branch", [("billing", "billing"), ("other", "general")])
def test_conditional_routing_then_common_tail(topic, branch):
    text = define_graph("""
        g.node("classify", {type: "e2e.step"});
        g.node("billing", {type: "e2e.step"});
        g.node("general", {type: "e2e.step"});
        g.node("reply", {type: "e2e.step"});
        g.entry("classify");
        g.conditionalEdge("classify", "e2e.route_by_topic", {billing: "billing", general: "general"});
        g.edge("billing", "reply");
        g.edge("general", "reply");
        g.exit("reply");
    """)
    _host, _version, result = run_program(text, {"topic": topic})
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.execution_trace == ["classify", branch, "reply"]
    assert JOURNAL.names() == ["classify", branch, "reply"]


def test_send_map_reduce_from_a_python_node():
    text = """
export function define() {
  const g = ng.graph("map_reduce");
  g.channel("items", {reducer: "overwrite", initial: []});
  g.channel("item", {reducer: "overwrite", initial: 0});
  g.channel("results", {reducer: "append", initial: []});
  g.node("fan", {type: "e2e.fan"});
  g.node("square", {type: "e2e.square"});
  g.entry("fan");
  g.exit("square");
  return g;
}
"""
    _host, _version, result = run_program(text, {"items": [1, 2, 3, 4]})
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert sorted(channel(result, "results")) == [1, 4, 9, 16]
    assert result.execution_trace[0] == "fan"
    assert sorted(result.execution_trace[1:]) == ["square[send]"] * 4
    assert sorted(JOURNAL.names()) == ["fan", "square:1", "square:2", "square:3", "square:4"]


def test_a_completed_program_run_reports_the_core_trace_of_its_sends():
    text = """
export function define() {
  const g = ng.graph("map_reduce");
  g.channel("items", {reducer: "overwrite", initial: []});
  g.channel("item", {reducer: "overwrite", initial: 0});
  g.channel("results", {reducer: "append", initial: []});
  g.node("fan", {type: "e2e.fan"});
  g.node("square", {type: "e2e.square"});
  g.entry("fan");
  g.exit("square");
  return g;
}
"""
    _host, _version, program = run_program(text, {"items": [1, 2, 3, 4]})
    assert program.status == ng.ProgramTerminalStatus.Completed, describe(program)

    core = ng.GraphEngine.compile({
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
        "channels": {"items": {"reducer": "overwrite", "initial": []},
                     "item": {"reducer": "overwrite", "initial": 0},
                     "results": {"reducer": "append", "initial": []}},
        "nodes": {"fan": {"type": "e2e.fan"}, "square": {"type": "e2e.square"}},
        "edges": [{"from": ng.START_NODE, "to": "fan"}, {"from": "square", "to": ng.END_NODE}],
    }, ng.NodeContext(), stderr_notices=False).run(ng.RunConfig(input={"items": [1, 2, 3, 4]}))

    assert core.execution_trace == ["fan"] + ["square[send]"] * 4
    assert program.execution_trace == core.execution_trace


def test_a_failed_program_attempt_names_its_send_executions_as_the_core_trace_does():
    # #433: a failing Core rethrows without its engine trace, so a failed attempt
    # builds its trace from node-end events. It lists the executions that
    # completed before the failure, in the order they completed, and names a Send
    # execution "<target>[send]" as the engine's trace does.
    text = """
export function define() {
  const g = ng.graph("map_reduce");
  g.channel("items", {reducer: "overwrite", initial: []});
  g.channel("item", {reducer: "overwrite", initial: 0});
  g.channel("results", {reducer: "append", initial: []});
  g.node("fan", {type: "e2e.fan"});
  g.node("square", {type: "e2e.square"});
  g.entry("fan");
  g.exit("square");
  return g;
}
"""
    _host, _version, program = run_program(text, {"items": [1, 2, -3]})
    assert program.status == ng.ProgramTerminalStatus.Failed, describe(program)

    trace = list(program.execution_trace)
    assert trace[0] == "fan", trace
    assert trace[1:] and all(entry == "square[send]" for entry in trace[1:]), trace
    assert len(trace) <= 3, trace


def test_subgraph_node_runs_an_inline_child_graph():
    registry = build_registry(extra_node_types=("subgraph",))
    host = make_host(registry=registry)
    text = """
export function define() {
  const inner = {
    schema_version: 1,
    name: "review",
    channels: {trace: {reducer: "append"}},
    nodes: {critique: {type: "e2e.step"}, revise: {type: "e2e.step"}},
    edges: [
      {from: "__start__", to: "critique"},
      {from: "critique", to: "revise"},
      {from: "revise", to: "__end__"}
    ]
  };
  return ng.graph("outer")
    .channel("trace", {reducer: "append", initial: []})
    .node("draft", {type: "e2e.step"})
    .node("review", {type: "subgraph", definition: inner,
                     input_map: {trace: "trace"}, output_map: {trace: "trace"}})
    .node("publish", {type: "e2e.step"})
    .entry("draft").edge("draft", "review").edge("review", "publish").exit("publish");
}
"""
    _host, _version, result = run_program(text, host=host)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.execution_trace == ["draft", "review", "publish"]
    assert trace_channel(result) == ["draft", "critique", "revise", "publish"]


ISSUE_357 = """
export function define() {
  return ng.graph("g")
    .channel("trace", {reducer: "append", initial: []})
    .node("A", {type: "e2e.step"}).node("B", {type: "e2e.step"}).node("C", {type: "e2e.step"})
    .node("D", {type: "e2e.step"}).node("FIX", {type: "e2e.step"})
    .entry("A").edge("A", "B").edge("A", "C")
    .conditionalEdge("B", "%s", {pass: "D", fail: "FIX"})
    .edge("FIX", "D").edge("C", "D")
    .barrier("D", ["B", "C"])
    .exit("D");
}
"""


def test_barrier_bypassed_by_a_conditional_fail_route_does_not_report_completed():
    _host, _version, result = run_program(ISSUE_357 % "e2e.always_fail")
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "P_CORE_BARRIER_STALLED"


def test_barrier_with_a_conditional_pass_route_completes():
    _host, _version, result = run_program(ISSUE_357 % "e2e.always_pass")
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.execution_trace[-1] == "D"
    assert JOURNAL.names().count("D") == 1
    assert "FIX" not in JOURNAL.names()


def _compile(text):
    return ng.ProgramCompiler(build_registry()).compile(source(text, "shape.js"))


@pytest.mark.parametrize("reserved", ["__end__", "__start__"])
def test_reserved_node_names_are_rejected(reserved):
    if reserved == "__end__":
        body = f'g.node("A", {{type: "e2e.step"}}); g.node("{reserved}", {{type: "e2e.step"}}); ' \
               f'g.entry("A"); g.edge("A", "{reserved}"); g.exit("{reserved}");'
    else:
        body = f'g.node("{reserved}", {{type: "e2e.step"}}); g.node("A", {{type: "e2e.step"}}); ' \
               f'g.entry("{reserved}"); g.edge("{reserved}", "A"); g.exit("A");'
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compile(define_graph(body))
    assert reserved in str(raised.value)


def test_missing_entry_is_rejected():
    text = define_graph('g.node("A", {type: "e2e.step"}); g.exit("A");')
    with pytest.raises(ng.ProgramCompileError):
        _compile(text)


def test_static_edge_overlapping_a_conditional_route_is_rejected():
    text = define_graph("""
        g.node("A", {type: "e2e.step"});
        g.node("B", {type: "e2e.step"});
        g.node("D", {type: "e2e.step"});
        g.node("X", {type: "e2e.step"});
        g.entry("A");
        g.edge("A", "B");
        g.edge("B", "D");
        g.conditionalEdge("B", "e2e.always_pass", {pass: "D", fail: "X"});
        g.edge("X", "D");
        g.exit("D");
    """)
    with pytest.raises(ng.ProgramCompileError) as raised:
        _compile(text)
    codes = [d["code"] for d in raised.value.diagnostics]
    assert "P_CORE_E13" in codes, codes
