"""A process exits cleanly while an engine with Python nodes is still alive.

A script that ends while an engine it ran with `run_async` is still referenced
(a module global, say) leaves the engine's worker thread finishing the run: it
releases the Python objects the run held, which needs the GIL. When the main
thread has already started finalizing the interpreter by then, taking the GIL
makes the worker exit (`PyThread_exit_thread`), the forced unwind meets a
`noexcept` frame, and the process dies with SIGABRT or SIGSEGV instead of
exiting with status 0. Which thread is first is a matter of timing, so the
failure shows only on some runs and mostly under load; this test starts the
reproduction as several child processes at once, round after round, and
requires every one of them to exit with status 0.
"""

import os
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import neograph_engine as ng

PARALLEL = 8
MAX_RUNS = 120
BUDGET_SECONDS = 50
RUN_TIMEOUT_SECONDS = 60

CHILD = textwrap.dedent('''
    import asyncio

    import neograph_engine as ng


    class Doubler(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            return [ng.ChannelWrite("doubled", (input.state.get("seed") or 0) * 2)]


    ng.NodeFactory.register_type("doubler", lambda name, config, ctx: Doubler(name))
    definition = {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "g",
        "channels": {"seed": {"reducer": "overwrite"}, "doubled": {"reducer": "overwrite"}},
        "nodes": {"d": {"type": "doubler"}},
        "edges": [{"from": ng.START_NODE, "to": "d"}, {"from": "d", "to": ng.END_NODE}],
    }
    # Still referenced when the interpreter exits.
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())


    async def go():
        await asyncio.gather(*[engine.run_async(ng.RunConfig(thread_id=f"t{i}", input={"seed": i}))
                               for i in range(8)])


    asyncio.run(go())
    print("done")
''')


def _child_env():
    env = os.environ.copy()
    package_root = str(Path(ng.__file__).resolve().parents[1])
    env["PYTHONPATH"] = os.pathsep.join(
        [package_root] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    return env


def _describe(returncode):
    # A negative status is the signal that ended the process.
    return f"signal {-returncode}" if returncode < 0 else f"status {returncode}"


def test_a_process_exits_cleanly_with_a_live_engine_and_python_nodes():
    env = _child_env()
    deadline = time.monotonic() + BUDGET_SECONDS
    runs = 0
    failures = []
    while runs < MAX_RUNS and time.monotonic() < deadline:
        children = [
            subprocess.Popen([sys.executable, "-X", "faulthandler", "-c", CHILD], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            for _ in range(PARALLEL)]
        for child in children:
            try:
                stdout, stderr = child.communicate(timeout=RUN_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                child.kill()
                stdout, stderr = child.communicate()
                failures.append(f"hung at exit\n{stderr[-1000:]}")
                continue
            runs += 1
            if child.returncode != 0 or stdout.strip() != "done":
                failures.append(f"{_describe(child.returncode)}\n{stderr[-1000:]}")
    assert runs >= 3 * PARALLEL, f"only {runs} runs fit in {BUDGET_SECONDS} s"
    assert not failures, (
        f"{len(failures)} of {runs} runs did not exit cleanly:\n" + "\n---\n".join(failures[:3]))


IN_FLIGHT = textwrap.dedent('''
    import asyncio
    import time

    import neograph_engine as ng


    class Slow(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            time.sleep(0.4)
            print("node finished", flush=True)
            return [ng.ChannelWrite("out", 1)]


    ng.NodeFactory.register_type("slow", lambda name, config, ctx: Slow(name))
    definition = {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "g",
        "channels": {"out": {"reducer": "overwrite"}},
        "nodes": {"s": {"type": "slow"}},
        "edges": [{"from": ng.START_NODE, "to": "s"}, {"from": "s", "to": ng.END_NODE}],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext())


    async def go():
        engine.run_async(ng.RunConfig(thread_id="t"))   # started, never awaited
        await asyncio.sleep(0.1)                         # the node is now sleeping


    asyncio.run(go())
    print("main done", flush=True)
''')

LATE_RUN = textwrap.dedent('''
    import asyncio
    import atexit

    # Registered before the module is imported, so it runs after the module's
    # own exit hook: the binding no longer accepts runs by then.
    def late():
        async def go():
            engine = ng.GraphEngine.compile(definition, ng.NodeContext())
            try:
                engine.run_async(ng.RunConfig(thread_id="late"))
            except RuntimeError as error:
                print("refused:", error)
            else:
                print("accepted")
        asyncio.run(go())

    atexit.register(late)

    import neograph_engine as ng

    definition = {
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "g",
        "channels": {"out": {"reducer": "overwrite"}},
        "nodes": {}, "edges": [{"from": ng.START_NODE, "to": ng.END_NODE}],
    }
    print("main done", flush=True)
''')


def _run_child(source):
    return subprocess.run([sys.executable, "-X", "faulthandler", "-c", source], env=_child_env(),
                          capture_output=True, text=True, timeout=RUN_TIMEOUT_SECONDS)


def test_a_run_still_executing_python_at_exit_finishes_before_the_interpreter_shuts_down():
    # Deterministic: the node is asleep in Python when the script ends, and it
    # needs the GIL when it wakes. The exit hook waits for it.
    result = _run_child(IN_FLIGHT)
    assert result.returncode == 0, (_describe(result.returncode), result.stderr[-1500:])
    assert result.stdout.split() == ["main", "done", "node", "finished"], result.stdout


def test_the_binding_accepts_no_run_once_its_exit_hook_has_stopped_the_runtime():
    result = _run_child(LATE_RUN)
    assert result.returncode == 0, (_describe(result.returncode), result.stderr[-1500:])
    assert "main done" in result.stdout
    assert "refused:" in result.stdout, result.stdout
