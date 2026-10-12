"""A GraphEngine is released while its fan-out workers still unwind failed branches.

A failed fan-out branch keeps its exception until the branch's coroutine frame
unwinds on a worker, which can be after the run has returned and the test has
finished with the engine. The exception's traceback holds the test's frame, so
the worker can be the thread that drops the last reference to the engine:

* destroying the engine's thread pool on a thread of that pool freed the pool
  under the worker, which went on to use it (an intermittent segmentation fault
  of this suite under load; the deterministic C++ regression test is
  tests/test_engine_pool_owner_thread.cpp);
* the thread that dropped the last reference while holding the GIL joined the
  workers, one of which was waiting for the GIL to release a failed branch's
  Python exception: the join never returned.

Both depend on which thread finishes first, so the runs repeat in a child
process, which a deadlock cannot take the test run down with.
"""

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

import neograph_engine as ng

ROUNDS = 300
TIMEOUT_SECONDS = 240

CHILD = textwrap.dedent('''
    import os
    import sys

    import neograph_engine as ng

    os.environ["NEOGRAPH_SUPPRESS_FANOUT_WARNING"] = "1"


    class Act(ng.GraphNode):
        def __init__(self, name, does):
            super().__init__()
            self._name = name
            self._does = does

        def get_name(self):
            return self._name

        def run(self, input):
            if self._does == "ask" and input.ctx.resume_value is None:
                raise ng.NodeInterrupt({"asks": self._name}, reason="approve " + self._name)
            if self._does == "fail":
                raise RuntimeError("boom " + self._name)
            return [ng.ChannelWrite("results", [self._name])]


    ng.NodeFactory.register_type(
        "lifetime.act", lambda name, config, _ctx: Act(name, config["does"]))


    def definition(first, second):
        return {
            "name": "engine_pool_lifetime",
            "channels": {"results": {"reducer": "append", "initial": []}},
            "nodes": {"n1": {"type": "lifetime.act", "does": first},
                      "n2": {"type": "lifetime.act", "does": second}},
            "edges": [{"from": "__start__", "to": "n1"}, {"from": "__start__", "to": "n2"},
                      {"from": "n1", "to": "__end__"}, {"from": "n2", "to": "__end__"}]}


    def run_and_drop(first, second):
        engine = ng.GraphEngine.compile(
            definition(first, second), ng.NodeContext(), ng.InMemoryCheckpointStore())
        engine.set_worker_count(4)
        try:
            engine.run(ng.RunConfig(thread_id="lifetime"))
        except Exception as error:
            # The traceback keeps this frame, and so the engine, alive for as
            # long as a worker still holds the exception.
            assert "boom n" in str(error), error
        else:
            raise AssertionError("the failed branch did not fail the run")


    for _ in range(int(sys.argv[1])):
        for shape in (("fail", "fail"), ("ask", "fail"), ("fail", "ask")):
            run_and_drop(*shape)
    print("dropped", int(sys.argv[1]) * 3, "engines")
''')


def _child_env():
    env = os.environ.copy()
    package_root = str(Path(ng.__file__).resolve().parents[1])
    env["PYTHONPATH"] = os.pathsep.join(
        [package_root] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    return env


def test_dropping_an_engine_after_failed_fan_outs_neither_crashes_nor_deadlocks():
    try:
        result = subprocess.run(
            [sys.executable, "-X", "faulthandler", "-c", CHILD, str(ROUNDS)],
            env=_child_env(), capture_output=True, text=True, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as expired:
        stderr = expired.stderr or b""
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        pytest.fail(
            "dropping an engine deadlocked: a fan-out worker waited for the GIL "
            "while the thread dropping the engine joined it\n" + stderr[-2000:])
    assert result.returncode == 0, (result.returncode, result.stdout[-2000:], result.stderr[-4000:])
    assert result.stdout.strip() == f"dropped {ROUNDS * 3} engines"
