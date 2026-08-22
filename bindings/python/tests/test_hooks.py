"""Mandatory lifecycle Hook parity with the C++ HookRuntime."""

import neograph_engine as ng
import pytest


class _WriteNode(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, _input):
        return [ng.ChannelWrite("value", "done")]


def _engine():
    ng.NodeFactory.register_type(
        "python_hook_write", lambda name, _config, _ctx: _WriteNode(name)
    )
    definition = {
        "name": "hook-python",
        "channels": {"value": {"reducer": "overwrite"}},
        "nodes": {"work": {"type": "python_hook_write"}},
        "edges": [
            {"from": ng.START_NODE, "to": "work"},
            {"from": "work", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
    return engine


def _definition(target="capture"):
    data = ng.HookDefinitionData()
    data.phase = ng.HookPhase.CheckpointPublished
    data.target_id = target
    data.delivery = ng.HookDelivery.BlockingMandatory
    data.failure_mode = ng.HookFailureMode.FailClosed
    data.idempotency = ng.HookIdempotency.Idempotent
    data.effect = ng.ToolEffectClass.ReadOnly
    mapper = ng.HookInputMapper()
    mapper.kind = ng.HookInputMapperKind.Template
    mapper.value_template = {"kind": "checkpoint"}
    data.input_mapper = mapper
    return ng.HookDefinition.create(data)


def test_python_callback_runs_from_mandatory_checkpoint_hook():
    observed = []

    def capture(arguments, event_type, event_data):
        observed.append((arguments, event_type, event_data))

    runtime = ng.create_hook_runtime([_definition()], {"capture": capture})
    engine = _engine()
    engine.set_hook_runtime(runtime)

    metadata = ng.RunMetadata(run_id="hook-python-run", owner_scope="python-owner")
    result = engine.run(ng.RunConfig(thread_id="hook-python-run"), metadata)

    assert result.output["channels"]["value"]["value"] == "done"
    assert observed
    assert all(item[0] == {"kind": "checkpoint"} for item in observed)
    assert all(item[1] == "checkpoint_published" for item in observed)


def test_hook_definition_exposes_immutable_definition_data():
    definition = _definition("inspect")
    assert definition.data.target_id == "inspect"
    assert definition.data.phase == ng.HookPhase.CheckpointPublished


def test_fail_closed_python_hook_blocks_the_runtime_boundary():
    def fail(*_args):
        raise RuntimeError("audit sink failed")

    runtime = ng.create_hook_runtime([_definition("fail")], {"fail": fail})
    engine = _engine()
    engine.set_hook_runtime(runtime)

    with pytest.raises(Exception, match="Hook|hook"):
        metadata = ng.RunMetadata(run_id="hook-python-fail", owner_scope="python-owner")
        engine.run(ng.RunConfig(thread_id="hook-python-fail"), metadata)
