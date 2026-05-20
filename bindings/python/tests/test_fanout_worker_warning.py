"""Run-time warning when a multi-Send fan-out runs without an
engine-owned worker pool.

Reproduces the contract introduced for issue #62 — `compile()`'s
default is `set_worker_count(1)` (no engine-owned pool, fan-out
branches dispatch inline on the caller's executor and run serially).
Without a louder hint the user discovers this only via timing-sensitive
integration tests; the engine now emits a one-shot stderr warning the
first time a fan-out of width >= 2 dispatches against a worker=1
default, with an env-var opt-out for benchmarks and CI assertions.

NOTE: Windows is skipped at module level. pytest's `capfd` redirects
fd 2 via `dup2`, but the MSVC CRT inside the loaded wheel binary
caches the original `fd 2 -> HANDLE` mapping inside its `FILE*
stderr`, so writes via `std::cerr` / `fputs(..., stderr)` / `fflush`
land on the pre-redirect HANDLE that `capfd` never observes. The
warning itself still reaches a Windows user's real console — only
pytest's fd-capture machinery is the limitation. The Linux + macOS
jobs in this PR's CI exercise the same code path and cover the
behavior.
"""

import sys

import pytest

if sys.platform == "win32":
    pytest.skip(
        "capfd does not observe wheel-binary stderr on Windows MSVC; "
        "Linux + macOS CI cover the warning behavior.",
        allow_module_level=True,
    )

import neograph_engine as neograph


_uid = 0
def _next_type(prefix):
    global _uid
    _uid += 1
    return f"{prefix}_{_uid}"


def _build_fanout_graph(width):
    """Build a simple `fan -> N x worker` graph that emits `width`
    Sends and joins on a list channel."""
    fan_type = _next_type("fan")
    worker_type = _next_type("worker")

    class FanNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def run(self, inp):
            return [neograph.Send("worker", {"item": i}) for i in range(width)]

    class WorkerNode(neograph.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._n = name

        def get_name(self):
            return self._n

        def run(self, inp):
            item = inp.state.get("item")
            return [neograph.ChannelWrite("results", [item])]

    neograph.NodeFactory.register_type(
        fan_type,    lambda n, c, ctx: FanNode(n))
    neograph.NodeFactory.register_type(
        worker_type, lambda n, c, ctx: WorkerNode(n))

    definition = {
        "name": "fanout_probe",
        "channels": {
            "item":    {"reducer": "overwrite"},
            "results": {"reducer": "append"},
        },
        "nodes": {
            "fan":    {"type": fan_type},
            "worker": {"type": worker_type},
        },
        "edges": [
            {"from": neograph.START_NODE, "to": "fan"},
            {"from": "worker",            "to": neograph.END_NODE},
        ],
    }
    return definition


def test_default_compile_emits_warning_on_multi_send_fanout(capfd, monkeypatch):
    monkeypatch.delenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", raising=False)
    defn = _build_fanout_graph(width=3)
    engine = neograph.GraphEngine.compile(defn, neograph.NodeContext())
    engine.run(neograph.RunConfig(thread_id="t1", input={}))
    _, err = capfd.readouterr()
    assert "[neograph]" in err
    assert "fan-out of width 3" in err
    assert "set_worker_count" in err


def test_warning_is_emitted_at_most_once_per_engine(capfd, monkeypatch):
    monkeypatch.delenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", raising=False)
    defn = _build_fanout_graph(width=2)
    engine = neograph.GraphEngine.compile(defn, neograph.NodeContext())

    engine.run(neograph.RunConfig(thread_id="t1", input={}))
    _, first = capfd.readouterr()
    assert first.count("[neograph]") == 1

    engine.run(neograph.RunConfig(thread_id="t2", input={}))
    _, second = capfd.readouterr()
    assert "[neograph]" not in second


def test_explicit_worker_pool_silences_warning(capfd, monkeypatch):
    monkeypatch.delenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", raising=False)
    defn = _build_fanout_graph(width=4)

    engine = neograph.GraphEngine.compile(defn, neograph.NodeContext())
    engine.set_worker_count(4)
    engine.run(neograph.RunConfig(thread_id="t1", input={}))
    _, err = capfd.readouterr()
    assert "[neograph]" not in err

    engine_auto = neograph.GraphEngine.compile(defn, neograph.NodeContext())
    engine_auto.set_worker_count_auto()
    engine_auto.run(neograph.RunConfig(thread_id="t2", input={}))
    _, err_auto = capfd.readouterr()
    assert "[neograph]" not in err_auto


def test_suppress_env_var_silences_warning(capfd, monkeypatch):
    monkeypatch.setenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1")
    defn = _build_fanout_graph(width=3)
    engine = neograph.GraphEngine.compile(defn, neograph.NodeContext())
    engine.run(neograph.RunConfig(thread_id="t1", input={}))
    _, err = capfd.readouterr()
    assert "[neograph]" not in err


def test_single_send_does_not_warn(capfd, monkeypatch):
    monkeypatch.delenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", raising=False)
    defn = _build_fanout_graph(width=1)
    engine = neograph.GraphEngine.compile(defn, neograph.NodeContext())
    engine.run(neograph.RunConfig(thread_id="t1", input={}))
    _, err = capfd.readouterr()
    assert "[neograph]" not in err
