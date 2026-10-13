"""RunResult.execution_trace lists every node execution, Send-started ones included (#424).

One entry per execution that completed, in the order the engine applied the
results. A super-step lists its ready nodes in ready order, then its Sends in
the order they were emitted, whatever order the executions finished in. A Send
execution is listed as "<target>[send]", so the same node run by a Send and by
an edge shows two kinds of entry. A step that paused lists nothing, and a run
that resumed it lists the whole step.
"""

import asyncio
import time

import pytest

import neograph_engine as ng


class _Body(ng.GraphNode):
    def __init__(self, name, body):
        super().__init__()
        self._name, self._body = name, body

    def get_name(self):
        return self._name

    def run(self, input):
        return self._body(input)


def _register(type_name, body):
    ng.NodeFactory.register_type(type_name, lambda name, _config, _ctx: _Body(name, body))


def _sends(*targets):
    return [ng.Send(target, {"item": index}) for index, target in enumerate(targets)]


def _record_item(input):
    item = input.state.get("item")
    if item == 0:
        time.sleep(0.06)  # with a pool, the first Send finishes last
    return [ng.ChannelWrite("results", [item])]


def _ask(input):
    if input.ctx.resume_value is None:
        raise ng.NodeInterrupt("approve", {"ask": True})
    return []


_register("trace.planner", lambda _input: _sends("worker", "worker", "worker"))
_register("trace.fanout", lambda _input: _sends("alpha", "beta", "gamma"))
_register("trace.ask_planner", lambda _input: _sends("asker", "worker"))
_register("trace.goto_planner", lambda _input: ng.NodeResult(
    writes=[], command=ng.Command(goto_node="after"), sends=_sends("worker", "worker", "worker")))
_register("trace.worker", _record_item)
_register("trace.asker", _ask)
_register("trace.noop", lambda _input: [])
_register("trace.nested_worker", lambda input: ng.NodeResult(
    writes=_record_item(input), sends=[ng.Send("leaf", {"item": input.state.get("item")})]))

PLANNER_AND_THREE_WORKERS = ["planner"] + ["worker[send]"] * 3


def _definition(nodes, edges=()):
    """`nodes` maps a node name to its type; the first one is the entry."""
    entry = next(iter(nodes))
    return {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
        "channels": {"item": {"reducer": "overwrite"},
                     "results": {"reducer": "append", "initial": []}},
        "nodes": {name: {"type": f"trace.{kind}"} for name, kind in nodes.items()},
        "edges": [{"from": ng.START_NODE, "to": entry}]
        + [{"from": source, "to": target} for source, target in edges],
    }


def _fanout_graph(edges=()):
    """planner Sends worker three times; no edge leads to worker."""
    return _definition({"planner": "planner", "worker": "worker"}, edges)


def _compile(definition, workers=1, store=None):
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(), store, stderr_notices=False)
    if workers != 1:
        engine.set_worker_count(workers)
    return engine


def _run(engine, config):
    return engine.run(config)


def _run_stream(engine, config):
    return engine.run_stream(config, lambda _event: None)


def _run_async(engine, config):
    async def go():
        return await engine.run_async(config)
    return asyncio.run(go())


def _run_stream_async(engine, config):
    async def go():
        return await engine.run_stream_async(config, lambda _event: None)
    return asyncio.run(go())


@pytest.mark.parametrize("workers", [1, 3])
@pytest.mark.parametrize("entry", [_run, _run_stream, _run_async, _run_stream_async],
                         ids=lambda entry: entry.__name__.lstrip("_"))
def test_send_fan_out_lists_the_planner_and_every_worker(entry, workers):
    result = entry(_compile(_fanout_graph(), workers), ng.RunConfig())

    assert result.execution_trace == PLANNER_AND_THREE_WORKERS


@pytest.mark.parametrize("workers", [1, 3])
def test_sends_are_listed_in_the_order_they_were_emitted_whatever_finishes_first(workers):
    definition = _definition(
        {"planner": "fanout", "alpha": "worker", "beta": "worker", "gamma": "worker"})

    result = _compile(definition, workers).run(ng.RunConfig())

    assert result.execution_trace == ["planner", "alpha[send]", "beta[send]", "gamma[send]"]
    assert result.output["channels"]["results"]["value"] == [0, 1, 2]


def test_ready_nodes_come_before_the_sends_of_their_step():
    definition = _definition({"planner": "planner", "worker": "worker", "side": "noop"},
                             [("__start__", "side")])

    result = _compile(definition, 3).run(ng.RunConfig())

    assert result.execution_trace == ["planner", "side"] + ["worker[send]"] * 3


def test_a_command_and_sends_of_one_node_are_all_listed():
    definition = _definition({"planner": "goto_planner", "worker": "worker", "after": "noop"})

    result = _compile(definition).run(ng.RunConfig())

    assert result.execution_trace == ["planner"] + ["worker[send]"] * 3 + ["after"]


def test_a_node_run_by_send_and_by_an_edge_shows_both_kinds():
    # The static edge makes `worker` run once more, after the Sends, on the
    # shared state, where no Send payload ever landed (#415).
    result = _compile(_fanout_graph([("planner", "worker")])).run(ng.RunConfig())

    assert result.execution_trace == PLANNER_AND_THREE_WORKERS + ["worker"]
    assert result.output["channels"]["results"]["value"] == [0, 1, 2, None]


def test_a_checkpoint_holds_the_writes_of_every_execution_the_trace_lists():
    store = ng.InMemoryCheckpointStore()
    engine = _compile(_fanout_graph(), store=store)

    result = engine.run(ng.RunConfig(thread_id="trace-checkpoint"))

    assert result.execution_trace == PLANNER_AND_THREE_WORKERS
    saved = store.load_by_id(result.checkpoint_id)
    assert saved.step == 0
    assert saved.channel_values["channels"]["results"]["value"] == [0, 1, 2]


def test_a_paused_step_lists_nothing_and_the_resume_lists_the_whole_step():
    definition = _definition({"planner": "ask_planner", "worker": "worker", "asker": "asker"})
    engine = _compile(definition, store=ng.InMemoryCheckpointStore())

    paused = engine.run(ng.RunConfig(thread_id="trace-pause"))
    assert paused.interrupted
    assert paused.execution_trace == []

    done = engine.resume("trace-pause", "yes")
    assert not done.interrupted
    assert done.execution_trace == ["planner", "asker[send]", "worker[send]"]


@pytest.mark.parametrize("workers", [1, 3])
def test_a_send_execution_that_returns_sends_fails_the_run(workers):
    # #432: the Sends of a Send execution used to be dropped without a word.
    engine = _compile(_definition({"planner": "planner", "worker": "nested_worker", "leaf": "worker"}),
                      workers)
    with pytest.raises(Exception) as raised:
        engine.run(ng.RunConfig(thread_id="nested"))
    assert "'worker' was started by a Send and returned Sends to 'leaf'" in str(raised.value)

