"""Scenario 1: the quickstart flow a Python user follows from docs/python-binding.md."""

import re
from pathlib import Path

import pytest

import neograph_engine as ng

from .support import (JOURNAL, TESTS_DIR, TRIAGE, admit, budget, build_registry, channel,
                      define_graph, describe, last_json_line, make_host, run_python, slow, source,
                      trace_channel)


_GUIDE = Path(TESTS_DIR).parents[2] / "docs" / "python-binding.md"


def _doc_example():
    """The first Python block of the guide's "Program and QuickJS" section, read
    from the guide itself so that the test follows the published text."""
    if not _GUIDE.is_file():
        pytest.skip(f"{_GUIDE} is not part of this checkout")
    text = _GUIDE.read_text(encoding="utf-8")
    section = text[text.index("\n## Program and QuickJS\n"):]
    match = re.search(r"^```python\n(.*?)^```", section, re.MULTILINE | re.DOTALL)
    assert match, "the Program and QuickJS section has no Python example"
    return match.group(1)


def _run_doc_example(code):
    scope = {}
    exec(compile(code, "python-binding.md", "exec"), scope)
    return scope["result"]


def test_docs_program_example_runs_verbatim():
    result = _run_doc_example(_doc_example())
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    # main() returns the callCore outcome: canonical channels plus value aliases.
    assert result.output["channels"]["value"]["value"] == 1
    assert result.output["value"] == 1
    assert result.execution_trace == ["work"]


@pytest.mark.parametrize("topic, branch", [("billing", "billing"), ("shipping", "general")])
def test_define_graph_with_python_nodes_and_condition(topic, branch):
    host = make_host()
    run_budget = budget()
    version = admit(host, TRIAGE, run_budget, "triage.js")

    result = host.run(version, {"topic": topic}, run_budget, "trace-quickstart")

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.failure is None and result.interrupt is None
    assert result.execution_trace == ["intake", "classify", branch]
    assert trace_channel(result) == ["intake", "classify", branch]
    assert channel(result, "topic") == topic
    # Only the routed branch executed.
    assert JOURNAL.names() == ["intake", "classify", branch]
    assert result.usage.core_steps == 3
    assert result.usage.program_operations == 1
    assert result.checkpoint is not None and result.checkpoint.checkpoint_id
    assert result.checkpoint.core_name == "triage"
    assert result.run_id.startswith("run-")
    assert result.program_version_id == version.id
    assert result.bundle_id == version.bundle_id
    parsed = ng.ProgramResult.parse(result.serialize_canonical())
    assert parsed.id == result.id
    assert parsed.status == result.status
    assert parsed.execution_trace == result.execution_trace


def test_identities_link_source_bundle_version_and_result():
    registry = build_registry()
    host = make_host(registry=registry)
    run_budget = budget()
    program = source(TRIAGE, "triage.js")

    version = host.compile_admit(program, run_budget)
    bundle = host.compile(program, run_budget)
    standalone = ng.ProgramCompiler(registry).compile(program, run_budget)

    assert bundle.id == version.bundle_id
    assert standalone.id == bundle.id, "ProgramCompiler and LocalProgramHost share the default build id"
    assert bundle.source_hash == program.source_hash
    assert bundle.source_kind == ng.ProgramSourceKind.JavaScript
    assert [d["name"] for d in bundle.sealed_core_definitions] == ["triage"]
    assert version.owner_scope == "tenant:e2e"
    assert ng.ProgramVersion.parse(version.serialize_canonical()).id == version.id
    assert ng.ProgramBundle.parse(bundle.serialize_canonical()).id == bundle.id
    # Admitting the same source again is idempotent.
    assert host.compile_admit(program, run_budget).id == version.id
    # The compiler build id is part of the bundle identity.
    other = ng.ProgramCompiler(registry, "another-build/v1").compile(program, run_budget)
    assert other.id != bundle.id


_IDENTITY_SCRIPT = r'''
import json
import neograph_engine as ng
from program_e2e import support as s

registry = s.build_registry()
program = s.source(s.TRIAGE, "triage.js")
bundle = ng.ProgramCompiler(registry).compile(program, s.budget())
host = s.make_host(registry=registry)
version = host.compile_admit(program, s.budget())
result = host.run(version, {"topic": "billing"}, s.budget())
print(json.dumps({
    "registry_fingerprint": registry.fingerprint,
    "source_hash": program.source_hash,
    "bundle_id": bundle.id,
    "definition_hash": bundle.sealed_core_definitions[0]["definition_hash"],
    "canonical_bundle": bundle.serialize_canonical(),
    "version_id": version.id,
    "version_bundle_id": version.bundle_id,
    "status": str(result.status),
    "trace": result.execution_trace,
}))
'''


def _identities_in_fresh_process():
    completed = run_python(_IDENTITY_SCRIPT)
    assert completed.returncode == 0, completed.stderr[-2000:]
    return last_json_line(completed.stdout)


@slow
def test_recompile_in_fresh_process_has_identical_identities():
    first = _identities_in_fresh_process()
    second = _identities_in_fresh_process()

    registry = build_registry()
    program = source(TRIAGE, "triage.js")
    here = ng.ProgramCompiler(registry).compile(program, budget())

    for other in (first, second):
        assert other["status"] == "ProgramTerminalStatus.Completed"
        assert other["trace"] == ["intake", "classify", "billing"]
        assert other["registry_fingerprint"] == registry.fingerprint
        assert other["source_hash"] == program.source_hash
        assert other["bundle_id"] == here.id
        assert other["definition_hash"] == here.sealed_core_definitions[0]["definition_hash"]
        assert other["canonical_bundle"] == here.serialize_canonical()
    assert first["version_id"] == second["version_id"]
    assert first["version_bundle_id"] == second["version_bundle_id"] == first["bundle_id"]


def test_activation_deploy_and_rollback():
    host = make_host("tenant:deploy")
    run_budget = budget()
    v1 = admit(host, define_graph('g.node("v1", {type: "e2e.step"}); g.entry("v1"); g.exit("v1");'),
               run_budget, "v1.js")
    v2 = admit(host, define_graph('g.node("v2", {type: "e2e.step"}); g.entry("v2"); g.exit("v2");'),
               run_budget, "v2.js")

    with pytest.raises(RuntimeError, match="activation"):
        host.start_active({}, run_budget)
    assert host.activation() is None

    assert host.activate(v1, 0) == ng.ProgramActivationResult.Activated
    activation, handle = host.start_active({}, run_budget)
    assert (activation.generation, activation.active_version_id) == (1, v1.id)
    assert handle.wait().execution_trace == ["v1"]

    # A deploy against a stale generation is a conflict, not a silent overwrite.
    assert host.activate(v2, 0) == ng.ProgramActivationResult.Conflict
    assert host.activate(v2, 1) == ng.ProgramActivationResult.Activated
    activation, handle = host.start_active({}, run_budget)
    assert (activation.generation, activation.active_version_id) == (2, v2.id)
    assert handle.wait().execution_trace == ["v2"]

    assert host.rollback(v1, 2) == ng.ProgramActivationResult.Activated
    activation, handle = host.start_active({}, run_budget)
    assert (activation.generation, activation.active_version_id) == (3, v1.id)
    assert handle.wait().execution_trace == ["v1"]
    assert ng.ProgramActivation.parse(activation.serialize_canonical()).id == activation.id
