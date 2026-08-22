"""Python parity for NeoGraph's optional Program / QuickJS control plane."""

import neograph_engine as ng


_DIGEST_NODE = "sha256:" + "1" * 64
_DIGEST_REDUCER = "sha256:" + "2" * 64


class _ProgramNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, _input):
        return [ng.ChannelWrite("value", 42)]


def _registry():
    ng.NodeFactory.register_type(
        "python_program_node",
        lambda name, _config, _ctx: _ProgramNode(name),
    )
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node(
        "python_program_node", "1.0.0", _DIGEST_NODE
    )
    builder.add_registered_reducer("overwrite", "1.0.0", _DIGEST_REDUCER)
    return builder.build()


def _budget():
    value = ng.ProgramRunBudget()
    value.wall_time_ms = 10_000
    value.model_tokens = 1_000
    value.monetary_microunits = 1_000
    value.max_concurrency = 2
    value.max_program_operations = 32
    value.max_core_steps = 20
    value.max_dynamic_compiles = 0
    value.max_child_depth = 1
    value.max_total_children = 4
    return value


def _ceiling():
    value = _budget()
    value.max_dynamic_compiles = 2
    return value


def _source():
    return ng.ProgramSource.from_javascript(
        "python-program.js",
        """
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "python_program_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}

export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
""",
    )


def test_program_symbols_and_native_capability_manifest():
    assert hasattr(ng, "ProgramCompiler")
    assert hasattr(ng, "LocalProgramHost")
    manifest = ng.javascript_authoring_capability_manifest()
    assert manifest["javascript_profile"] == "sealed-v1"
    command_names = {
        method["name"] for method in manifest["main"]["command_methods"]
    }
    assert {"callCore", "spawn", "all", "checkpoint"} <= command_names


def test_python_compiles_the_same_quickjs_program_bundle_as_cpp():
    registry = _registry()
    source = _source()
    compiler = ng.ProgramCompiler(registry, "python-program-test/v1")

    bundle = compiler.compile(source, _budget())

    assert bundle.source_kind == ng.ProgramSourceKind.JavaScript
    assert bundle.source_hash == source.source_hash
    assert bundle.sealed_core_definitions[0]["name"] == "main"
    assert ng.ProgramBundle.parse(bundle.serialize_canonical()).id == bundle.id


def test_local_program_host_compiles_admits_and_executes_program_runtime():
    host = ng.LocalProgramHost(
        _registry(), "python-owner", _ceiling(), "python-program-host/v1"
    )
    version = host.compile_admit(_source(), _budget())

    result = host.run(version, {}, _budget(), "python-program-run")

    assert result.status == ng.ProgramTerminalStatus.Completed
    assert result.program_version_id == version.id
    assert result.output["channels"]["value"]["value"] == 42
    assert result.execution_trace == ["work"]
    assert ng.ProgramResult.parse(result.serialize_canonical()).id == result.id


def test_program_handle_start_wait_uses_the_same_runtime():
    host = ng.LocalProgramHost(
        _registry(), "python-owner-handle", _ceiling(), "python-program-host/v1"
    )
    version = host.compile_admit(_source(), _budget())

    handle = host.start(version, {}, _budget())
    result = handle.wait()

    assert handle.run_id == result.run_id
    assert result.status == ng.ProgramTerminalStatus.Completed
