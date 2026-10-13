"""Channel versions: one write counter shared by all channels (#416).

A channel's `version` is the value of the run's single write counter at the
channel's last write, and `global_version` is the counter after the last write.
They order writes; they are not a count of one channel's writes and not a
super-step number. The run input's writes count. A checkpoint stores both
numbers, and a resumed or forked run goes on counting from them.

The graph is the one of example 79: three nodes in turn, each writing `last`
(overwrite) and `all` (append).
"""

import neograph_engine as ng
import pytest

_uid = 0


def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


class _Writer(ng.GraphNode):
    """Writes its own name to `last` (overwrite) and appends it to `all` (append)."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("last", self._name), ng.ChannelWrite("all", [self._name])]


class _Probe(ng.GraphNode):
    """Writes the counters it sees to `seen`."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        state = input.state
        return [ng.ChannelWrite("seen", [state.channel_version("last"), state.channel_version("all"),
                                         state.global_version(), state.channel_version("seen")])]


def _chain(nodes, channels, interrupt_before=()):
    edges, previous = [], ng.START_NODE
    for name, _ in nodes:
        edges.append({"from": previous, "to": name})
        previous = name
    edges.append({"from": previous, "to": ng.END_NODE})
    definition = {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
        "name": "channel_versions",
        "channels": channels,
        "nodes": {name: {"type": node_type} for name, node_type in nodes},
        "edges": edges,
    }
    if interrupt_before:
        definition["interrupt_before"] = list(interrupt_before)
    return definition


def _three_writers(interrupt_before=()):
    writer = _next_type("channel_versions_writer")
    ng.NodeFactory.register_type(writer, lambda name, config, ctx: _Writer(name))
    return _chain(
        [("first", writer), ("second", writer), ("third", writer)],
        {"last": {"reducer": "overwrite"},
         "all": {"reducer": "append"},
         "budget": {"reducer": "overwrite", "initial": 100}},
        interrupt_before)


def _engine(definition):
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())
    store = ng.InMemoryCheckpointStore()
    engine.set_checkpoint_store(store)
    return engine, store


def _versions(output):
    channels = output["channels"]
    return {name: channels[name]["version"] for name in ("last", "all", "budget") if name in channels}


def test_three_writers_leave_one_counter_shared_by_every_channel():
    engine = ng.GraphEngine.compile(_three_writers(), ng.NodeContext())
    result = engine.run(ng.RunConfig())

    # Six writes in order: last 1, all 2, last 3, all 4, last 5, all 6.
    assert _versions(result.output) == {"last": 5, "all": 6, "budget": 0}
    assert result.output["global_version"] == 6
    assert result.output["channels"]["all"]["value"] == ["first", "second", "third"]
    # `last` was written three times, but its version is not 3. A channel nobody
    # wrote keeps version 0, also with an initial value.
    assert result.output["channels"]["last"]["value"] == "third"
    assert result.output["channels"]["budget"]["value"] == 100


def test_a_node_sees_the_shared_counter_not_a_sum_of_versions():
    writer = _next_type("channel_versions_writer")
    probe = _next_type("channel_versions_probe")
    ng.NodeFactory.register_type(writer, lambda name, config, ctx: _Writer(name))
    ng.NodeFactory.register_type(probe, lambda name, config, ctx: _Probe(name))
    definition = _chain(
        [("first", writer), ("second", writer), ("probe", probe)],
        {"last": {"reducer": "overwrite"}, "all": {"reducer": "append"}, "seen": {"reducer": "overwrite"}})
    result = ng.GraphEngine.compile(definition, ng.NodeContext()).run(ng.RunConfig())

    # The probe runs after four writes (last 3, all 4): the counter is 4, not the
    # sum of the channels' versions (7). `seen` was not written yet.
    assert result.output["channels"]["seen"]["value"] == [3, 4, 4, 0]
    assert result.output["channels"]["seen"]["version"] == 5
    assert result.output["global_version"] == 5


def test_the_run_input_counts_as_writes_and_unknown_keys_do_not():
    engine = ng.GraphEngine.compile(_three_writers(), ng.NodeContext())
    result = engine.run(ng.RunConfig(input={"last": "seed", "not_a_channel": 1}))

    # The input is one write (counter 1); the nodes' six writes follow.
    assert _versions(result.output) == {"last": 6, "all": 7, "budget": 0}
    assert result.output["global_version"] == 7
    assert "not_a_channel" not in result.output["channels"]


def test_versions_rise_across_checkpoints_and_no_two_channels_share_one():
    engine, _store = _engine(_three_writers())
    engine.run(ng.RunConfig(thread_id="versions-history"))

    history = sorted(engine.get_state_history("versions-history"), key=lambda cp: cp.step)
    assert len(history) >= 3
    previous_global, previous = 0, {"last": 0, "all": 0}
    for checkpoint in history:
        values = checkpoint.channel_values
        assert values["global_version"] >= previous_global, f"step {checkpoint.step}"
        seen = set()
        for name in ("last", "all"):
            version = values["channels"][name]["version"]
            assert previous[name] <= version <= values["global_version"], f"{name} at step {checkpoint.step}"
            if version:
                assert version not in seen, f"two channels share version {version}"
                seen.add(version)
            previous[name] = version
        previous_global = values["global_version"]
    assert (previous_global, previous) == (6, {"last": 5, "all": 6})
    # The engine keeps the numbers in channel_values; it never sets channel_versions.
    assert all(not checkpoint.channel_versions for checkpoint in history)


def test_a_resumed_run_goes_on_counting_from_the_checkpoint():
    engine, _store = _engine(_three_writers(interrupt_before=["third"]))
    paused = engine.run(ng.RunConfig(thread_id="versions-resume"))

    assert paused.interrupted
    # `first` and `second` ran: four writes.
    assert _versions(paused.output) == {"last": 3, "all": 4, "budget": 0}
    assert paused.output["global_version"] == 4
    state = engine.get_state("versions-resume")
    assert (state["global_version"], state["channels"]["last"]["version"]) == (4, 3)

    # The resumed run restores them and counts on: the numbers are those of a run
    # that never paused.
    resumed = engine.resume("versions-resume")
    assert not resumed.interrupted
    assert _versions(resumed.output) == {"last": 5, "all": 6, "budget": 0}
    assert resumed.output["global_version"] == 6


def test_a_forked_run_goes_on_counting_from_the_checkpoint():
    engine, _store = _engine(_three_writers(interrupt_before=["third"]))
    assert engine.run(ng.RunConfig(thread_id="versions-source")).interrupted

    engine.fork("versions-source", "versions-branch")
    branch = engine.resume("versions-branch")
    assert not branch.interrupted
    assert _versions(branch.output) == {"last": 5, "all": 6, "budget": 0}
    assert branch.output["global_version"] == 6

    # The source is untouched and still resumes to the same numbers.
    assert engine.get_state("versions-source")["global_version"] == 4
    assert engine.resume("versions-source").output["global_version"] == 6


def test_a_new_input_on_a_resumed_thread_counts_after_the_checkpoint():
    engine, _store = _engine(_three_writers())
    first = engine.run(ng.RunConfig(thread_id="versions-continue"))
    assert first.output["global_version"] == 6

    # resume_if_exists restores the counter (6) before it applies the input, so
    # the input is write 7 and the second pass's six writes are 8 to 13.
    second = engine.run(ng.RunConfig(thread_id="versions-continue", resume_if_exists=True,
                                     input={"last": "again"}))
    assert _versions(second.output) == {"last": 12, "all": 13, "budget": 0}
    assert second.output["global_version"] == 13
