"""A provider bound to Program llm_call nodes from Python (#372).

These tests cover the API surface: registry entries, the host's validation
of its binding, the identity rule and the durable host. The end-to-end
scenarios that call a model and settle money are in
program_e2e/test_09_bound_llm.py.
"""

import hashlib
import json

import pytest

import neograph_engine as ng

pytestmark = pytest.mark.skipif(
    not (getattr(ng, "_HAVE_PROGRAM", False) and getattr(ng, "_HAVE_LLM", False)),
    reason="neograph_engine was built without Program or the LLM target")


def digest(label):
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def provider():
    from neograph_engine.llm import SchemaProvider
    descriptor = ng.load_provider_descriptor(json.dumps({
        "descriptor_version": 1, "revision": 1, "id": "unit-chat", "family": "openai.chat",
        "connection": {"base_url": "https://127.0.0.1:9",
                       "paths": {"buffered": "/v1/chat/completions",
                                 "streaming": "/v1/chat/completions"}},
    }))
    return SchemaProvider(descriptor, ng.ProviderRuntimeOptions(api_key="unit-key"))


class Noop(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return []


ng.NodeFactory.register_type("unit.noop", lambda name, config, ctx: Noop(name))


def registry(bound=True):
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_reducer("append", "1.0.0", digest("reducer:append"))
    builder.add_registered_node("unit.noop", "1.0.0", digest("node:unit.noop"))
    if bound:
        builder.add_provider("chat", "1.0.0", digest("provider:chat"))
        builder.add_registered_node("llm_call", "1.0.0", digest("node:llm_call"), provider="chat")
    else:
        builder.add_registered_node("llm_call", "1.0.0", digest("node:llm_call"))
    return builder.build()


def ceiling():
    value = ng.ProgramRunBudget()
    for field, amount in dict(wall_time_ms=10_000, model_tokens=1_000_000,
                              monetary_microunits=1_000_000, max_concurrency=2,
                              max_program_operations=8, max_core_steps=20,
                              max_dynamic_compiles=1).items():
        setattr(value, field, amount)
    return value


def run_budget():
    value = ceiling()
    value.max_dynamic_compiles = 0
    return value


CHAT = """
export function define() {
  const g = ng.graph("chat");
  g.channel("messages", {reducer: "append", initial: []});
  g.node("llm", {type: "llm_call"});
  g.entry("llm");
  g.exit("llm");
  return g;
}
"""


def bound_host(**kwargs):
    kwargs.setdefault("node_context", ng.NodeContext(provider=provider(), model="unit-model"))
    kwargs.setdefault("binding_label", "unit-binding")
    return ng.LocalProgramHost(registry(), "tenant:unit", ceiling(), **kwargs)


def test_provider_entries_are_validated():
    builder = ng.ProgramRegistryBuilder()
    builder.add_provider("chat", "1.0.0", digest("provider:chat"))
    with pytest.raises(ValueError, match="already registered"):
        builder.add_provider("chat", "1.0.0", digest("provider:chat"))
    with pytest.raises(ValueError, match="add_provider"):
        builder.add_registered_node("llm_call", "1.0.0", digest("node:llm_call"), provider="other")
    with pytest.raises(ValueError, match="llm_call"):
        builder.add_registered_node("intent_classifier", "1.0.0", digest("node:ic"),
                                    provider="chat")


def test_a_provider_requirement_is_part_of_the_registry_identity():
    assert registry(bound=True).fingerprint != registry(bound=False).fingerprint
    assert registry(bound=True).fingerprint == registry(bound=True).fingerprint
    entries = registry(bound=True).manifest["entries"]
    llm = next(e for e in entries if e["kind"] == "node" and e["name"] == "llm_call")
    assert [d["name"] for d in llm["required_executables"]] == ["chat"]


def test_node_context_carries_provider_controls():
    controls = ng.ProviderControls()
    controls.max_output_tokens = 64
    context = ng.NodeContext(provider_controls=controls)
    assert context.provider_controls.max_output_tokens == 64
    context.provider_controls.max_output_tokens = 32
    assert context.provider_controls.max_output_tokens == 32
    assert ng.NodeContext().provider_controls.max_output_tokens is None


@pytest.mark.parametrize("label", ["", "Upper", "-x", "with space", "sk-or-v1-abcdef",
                                   "sk_live_1", "a" * 65, "slash/label"])
def test_binding_label_must_be_a_short_non_secret_name(label):
    with pytest.raises(ValueError, match="binding_label"):
        bound_host(binding_label=label)


def test_a_binding_needs_a_label_a_provider_and_no_tools():
    with pytest.raises(ValueError, match="binding_label"):
        bound_host(binding_label=None)
    with pytest.raises(ValueError, match="provider"):
        bound_host(node_context=ng.NodeContext(model="unit-model"))
    with pytest.raises(ValueError, match="node_context"):
        ng.LocalProgramHost(registry(), "tenant:unit", ceiling(), binding_label="unit-binding")

    class Echo(ng.Tool):
        def get_name(self):
            return "echo"

        def get_definition(self):
            return ng.ChatTool(name="echo", description="echo", parameters={"type": "object"})

        def execute(self, arguments):
            return "echo"

    with pytest.raises(ValueError, match="tools"):
        bound_host(node_context=ng.NodeContext(provider=provider(), tools=[Echo()]))


def test_the_label_enters_the_version_identity_the_model_does_not():
    source = ng.ProgramSource.from_javascript("chat.js", CHAT)
    a = bound_host().compile_admit(source, run_budget())
    b = bound_host(node_context=ng.NodeContext(provider=provider(), model="another-model"))\
        .compile_admit(source, run_budget())
    c = bound_host(binding_label="other-binding").compile_admit(source, run_budget())
    assert a.bundle_id == b.bundle_id == c.bundle_id
    assert a.id == b.id, "the model is not identity"
    assert a.id != c.id, "the binding label is identity"
    assert "unit-model" not in a.serialize_canonical()
    assert "unit-key" not in a.serialize_canonical()


def test_without_a_binding_the_program_is_rejected_at_admission():
    host = ng.LocalProgramHost(registry(), "tenant:unit", ceiling())
    with pytest.raises(ng.ProgramAdmissionError) as raised:
        host.compile_admit(ng.ProgramSource.from_javascript("chat.js", CHAT), run_budget())
    assert [d["code"] for d in raised.value.diagnostics] == ["P_ADMIT_PROVIDER"]


def test_monetary_records_are_empty_without_priced_calls():
    host = ng.LocalProgramHost(registry(), "tenant:unit", ceiling())
    text = """
export function define() {
  const g = ng.graph("noop");
  g.channel("messages", {reducer: "append", initial: []});
  g.node("noop", {type: "unit.noop"});
  g.entry("noop");
  g.exit("noop");
  return g;
}
"""
    version = host.compile_admit(ng.ProgramSource.from_javascript("noop.js", text), run_budget())
    result = host.run(version, {}, run_budget())
    assert result.status == ng.ProgramTerminalStatus.Completed
    assert result.monetary_records == []
    assert result.usage.monetary_microunits == 0
    assert ng.ProgramMonetaryRecord is not None


def test_an_in_memory_host_cannot_reconnect_a_run_it_did_not_start():
    host = ng.LocalProgramHost(registry(), "tenant:unit", ceiling())
    with pytest.raises(ng.ProgramDiagnosticError, match="P_RUN_NOT_FOUND"):
        host.reconnect("run-unknown")


def test_a_sqlite_host_keeps_its_runs_for_a_new_host(tmp_path):
    text = """
export function define() {
  const g = ng.graph("noop");
  g.channel("messages", {reducer: "append", initial: []});
  g.node("noop", {type: "unit.noop"});
  g.entry("noop");
  g.exit("noop");
  return g;
}
"""
    source = ng.ProgramSource.from_javascript("noop.js", text)
    first = ng.LocalProgramHost(registry(), "tenant:unit", ceiling(),
                                sqlite_directory=str(tmp_path / "runs"))
    version = first.compile_admit(source, run_budget())
    result = first.run(version, {}, run_budget())
    assert result.status == ng.ProgramTerminalStatus.Completed
    del first
    second = ng.LocalProgramHost(registry(), "tenant:unit", ceiling(),
                                 sqlite_directory=str(tmp_path / "runs"))
    again = second.reconnect(result.run_id).wait()
    assert again.status == ng.ProgramTerminalStatus.Completed
    assert again.id == result.id
    assert (tmp_path / "runs" / "program.db").exists()
    assert (tmp_path / "runs" / "provider_calls.db").exists()


def test_a_durable_binding_needs_native_custody(tmp_path):
    with pytest.raises(ValueError, match="native_history_archive"):
        bound_host(sqlite_directory=str(tmp_path / "runs"))
