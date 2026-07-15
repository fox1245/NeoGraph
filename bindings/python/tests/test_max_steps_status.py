"""RunResult.max_steps_exhausted reports limit stops without false positives."""

import neograph_engine as ng


@ng.node("max_steps_status_noop")
def noop(_state):
    return []


def make_engine(loop: bool):
    target = "max_steps_status_noop" if loop else ng.END_NODE
    definition = {
        "name": "max_steps_status",
        "channels": {"value": {"reducer": "overwrite"}},
        "nodes": {"max_steps_status_noop": {"type": "max_steps_status_noop"}},
        "edges": [
            {"from": ng.START_NODE, "to": "max_steps_status_noop"},
            {"from": "max_steps_status_noop", "to": target},
        ],
    }
    return ng.GraphEngine.compile(definition, ng.NodeContext())


def test_self_loop_reports_max_steps_exhausted():
    result = make_engine(loop=True).run(ng.RunConfig(max_steps=3))

    assert result.execution_trace == ["max_steps_status_noop"] * 3
    assert result.max_steps_exhausted is True
    assert result.output["_neograph"]["max_steps_exhausted"] is True


def test_exact_boundary_completion_is_not_exhausted():
    result = make_engine(loop=False).run(ng.RunConfig(max_steps=1))

    assert result.execution_trace == ["max_steps_status_noop"]
    assert result.max_steps_exhausted is False
    assert "_neograph" not in result.output


def test_zero_max_steps_with_pending_work_is_exhausted():
    result = make_engine(loop=False).run(ng.RunConfig(max_steps=0))

    assert result.execution_trace == []
    assert result.max_steps_exhausted is True
