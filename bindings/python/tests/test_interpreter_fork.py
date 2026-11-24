"""A child made by fork exits normally after the binding's threads ran in its parent.

The binding keeps threads of its own (the one that drives `run_async`, the pools that run Python tools) and
bookkeeping about them: the runtime's worker, the pools, the count of node attempts that are running. A child made by
`os.fork()` inherits that bookkeeping and none of those threads. When the child exits normally (its script ends, or it
calls `sys.exit`), the interpreter runs the binding's exit hook there; the hook must neither wait for what only the
parent's threads can finish nor join threads that are not the child's, and the static destructors that run after it
must not either. The parent's exit does not change, and the work the child starts after the fork runs on threads of
its own, which the child's exit hook stops.

Every case runs as a script in a subprocess; the script forks. No test waits for a bounded time: a child (or a
script) that hangs is ended by the test runner's timeout, and the processes carry PR_SET_PDEATHSIG so that none of
them outlives it.
"""

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

import neograph_engine as ng
from sanitizer_env import without_leak_detection

pytestmark = pytest.mark.skipif(not hasattr(os, "fork") or not sys.platform.startswith("linux"),
                                reason="a child made by fork, ended with its parent (Linux)")

# The status the forked child exits with: a value no signal or crash produces, so a normal exit is told from the rest.
CHILD_STATUS = 7

PRELUDE = textwrap.dedent('''
    import asyncio
    import atexit
    import ctypes
    import os
    import signal
    import sys
    import threading
    import warnings

    # A process with threads that forks is what these scripts do on purpose.
    warnings.simplefilter("ignore", DeprecationWarning)

    CHILD_STATUS = %d


    def die_with_parent():
        # A process whose parent has gone is killed (Linux), so that a hung one never outlives the test run.
        ctypes.CDLL(None).prctl(1, int(signal.SIGKILL), 0, 0, 0)


    die_with_parent()
    PARENT_PID = os.getpid()


    def in_child():
        return os.getpid() != PARENT_PID


    # Registered first of all, so it runs last: the child leaves once every exit hook has run, before the destructors of
    # static objects run (see the test that sets the variable).
    if os.environ.get("CHILD_LEAVES_AFTER_THE_EXIT_HOOKS"):
        atexit.register(lambda: os._exit(CHILD_STATUS) if in_child() else None)

    # Registered before the module is imported, so it runs after the module's own exit hook, in whichever process
    # exits: the binding refuses new runs once its hook has stopped its runtime.
    def after_the_hook():
        if not in_child():
            return
        async def attempt():
            try:
                engine.run_async(ng.RunConfig(thread_id="late"))
            except RuntimeError as error:
                print("child after the hook: refused", flush=True)
            else:
                print("child after the hook: accepted", flush=True)
        asyncio.run(attempt())

    atexit.register(after_the_hook)

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
    engine = ng.GraphEngine.compile({
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "g",
        "channels": {"seed": {"reducer": "overwrite"}, "doubled": {"reducer": "overwrite"}},
        "nodes": {"d": {"type": "doubler"}},
        "edges": [{"from": ng.START_NODE, "to": "d"}, {"from": "d", "to": ng.END_NODE}],
    }, ng.NodeContext())


    def runs_on_the_runtime(tag):
        """Four runs on the binding's runtime thread; returns what they computed."""
        async def go():
            results = await asyncio.gather(*[
                engine.run_async(ng.RunConfig(thread_id=f"{tag}{i}", input={"seed": i})) for i in range(4)])
            return [r.output["channels"]["doubled"]["value"] for r in results]
        return asyncio.run(go())


    class AsyncSleep(ng.AsyncTool):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_definition(self):
            d = ng.ChatTool()
            d.name = self._name
            d.description = "returns at once"
            return d

        def execute(self, arguments):
            return '{"ok": true}'

        def get_name(self):
            return self._name


    class CallsTools(ng.GraphNode):
        def run(self, _input):
            calls = [{"id": str(i), "name": f"t{i}", "arguments": "{}"} for i in range(3)]
            return [ng.ChannelWrite("messages", [{"role": "assistant", "content": "", "tool_calls": calls}])]

        def get_name(self):
            return "llm"


    ng.NodeFactory.register_type("calls_tools", lambda name, config, ctx: CallsTools())


    def runs_on_the_tool_pool(tag):
        """One run whose three AsyncTool calls go to the binding's tool pool; returns how many answered."""
        tools = [AsyncSleep(f"t{i}") for i in range(3)]
        tool_engine = ng.GraphEngine.compile({
            "name": f"tools_{tag}",
            "channels": {"messages": {"reducer": "append"}},
            "nodes": {"llm": {"type": "calls_tools"}, "tools": {"type": "tool_dispatch"}},
            "edges": [{"from": "__start__", "to": "llm"}, {"from": "llm", "to": "tools"},
                      {"from": "tools", "to": "__end__"}],
        }, ng.NodeContext(tools=tools))
        result = tool_engine.run(ng.RunConfig(thread_id=tag))
        return len([m for m in result.output["channels"]["messages"]["value"] if m.get("role") == "tool"])


    def reap(pid):
        _, status = os.waitpid(pid, 0)
        return os.waitstatus_to_exitcode(status)
''' % CHILD_STATUS)


def _env():
    env = without_leak_detection()
    package_root = str(Path(ng.__file__).resolve().parents[1])
    env["PYTHONPATH"] = os.pathsep.join(
        [package_root] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    return env


def _describe(returncode):
    return f"signal {-returncode}" if returncode < 0 else f"status {returncode}"


def _run_script(body, **extra_env):
    # No `timeout=`: the runner's timeout bounds a script that hangs.
    return subprocess.run([sys.executable, "-X", "faulthandler", "-c", PRELUDE + textwrap.dedent(body)],
                          env={**_env(), **extra_env}, capture_output=True, text=True)


def _lines(result):
    assert result.returncode == 0, (_describe(result.returncode), result.stdout, result.stderr[-1500:])
    return result.stdout.splitlines()


RAN_THEN_FORK = '''
    assert runs_on_the_runtime("p") == [0, 2, 4, 6]
    assert runs_on_the_tool_pool("p") == 3
    print("parent ran", flush=True)
    pid = os.fork()
    if pid == 0:
        die_with_parent()
        %s
    print("child status", reap(pid), flush=True)
    # The parent's threads and bookkeeping are untouched by the child's exit.
    assert runs_on_the_runtime("q") == [0, 2, 4, 6]
    assert runs_on_the_tool_pool("q") == 3
    print("parent ran again", flush=True)
'''

# A child that returns from its script must not run the rest of the parent's script, so that case keeps the rest under
# a test of the process id.
RAN_THEN_FORK_END_OF_SCRIPT = '''
    assert runs_on_the_runtime("p") == [0, 2, 4, 6]
    assert runs_on_the_tool_pool("p") == 3
    print("parent ran", flush=True)
    pid = os.fork()
    if pid == 0:
        die_with_parent()
    else:
        print("child status", reap(pid), flush=True)
        assert runs_on_the_runtime("q") == [0, 2, 4, 6]
        assert runs_on_the_tool_pool("q") == 3
        print("parent ran again", flush=True)
'''


@pytest.mark.parametrize("script,child_status", [
    (RAN_THEN_FORK % "sys.exit(CHILD_STATUS)", CHILD_STATUS),
    (RAN_THEN_FORK_END_OF_SCRIPT, 0)],
    ids=["child_calls_sys_exit", "child_returns_from_its_script"])
def test_a_child_made_by_fork_exits_normally_after_the_binding_ran_work_and_the_parent_goes_on(
        script, child_status):
    result = _run_script(script)
    lines = _lines(result)
    # "refused" is printed by an exit handler that runs after the binding's hook, in the child.
    assert lines == ["parent ran", "child after the hook: refused", f"child status {child_status}",
                     "parent ran again"], (
        lines, result.stderr[-1500:])


IN_FLIGHT_ATTEMPT = '''
    entered = threading.Event()
    release = threading.Event()


    class Blocked(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, input):
            entered.set()
            release.wait()          # ends when the parent says so, after the child has gone
            return [ng.ChannelWrite("out", "finished")]


    ng.NodeFactory.register_type("blocked", lambda name, config, ctx: Blocked(name))
    blocked = ng.GraphEngine.compile({
        "schema_version": ng.TOPOLOGY_SCHEMA_VERSION, "name": "b",
        "channels": {"out": {"reducer": "overwrite"}},
        "nodes": {"w": {"type": "blocked"}},
        "edges": [{"from": ng.START_NODE, "to": "w"}, {"from": "w", "to": ng.END_NODE}],
        # An attempt with a timeout is dispatched off the run's orchestrator and counted as running in the process.
        "node_retry_policies": {"w": {"attempt_timeout_ms": 3600000}},
    }, ng.NodeContext())

    outcome = {}
    thread = threading.Thread(target=lambda: outcome.update(
        status=blocked.run(ng.RunConfig(thread_id="b")).status))
    thread.start()
    entered.wait()                  # the attempt is running on a thread that only this process has
    print("attempt running", flush=True)

    pid = os.fork()
    if pid == 0:
        die_with_parent()
        sys.exit(CHILD_STATUS)
    print("child status", reap(pid), flush=True)

    release.set()
    thread.join()
    print("attempt run", outcome["status"] == ng.RunStatus.Completed, flush=True)
    assert runs_on_the_runtime("q") == [0, 2, 4, 6]
    print("parent ran again", flush=True)
'''


def test_the_exit_hook_of_a_child_made_while_a_node_attempt_runs_in_its_parent_does_not_wait_for_it():
    # The count of running attempts is inherited, and only the parent's thread can lower it: a hook that waited for the
    # count to reach zero would wait for ever. The child leaves after the exit hooks have run (the variable): the
    # destructors of static objects of the core library that run after them are another unit's (a node attempt that has
    # a timeout runs on a pool of that library, whose destructor joins threads the child does not have).
    lines = _lines(_run_script(IN_FLIGHT_ATTEMPT, CHILD_LEAVES_AFTER_THE_EXIT_HOOKS="1"))
    assert lines == ["attempt running", "child after the hook: refused", f"child status {CHILD_STATUS}",
                     "attempt run True", "parent ran again"], lines


CHILD_WORK = '''
    assert runs_on_the_runtime("p") == [0, 2, 4, 6]
    assert runs_on_the_tool_pool("p") == 3
    print("parent ran", flush=True)

    pid = os.fork()
    if pid == 0:
        die_with_parent()
        # Work of the child's own, started after the fork: its own runtime thread and its own tool pool.
        print("child ran", runs_on_the_runtime("c"), runs_on_the_tool_pool("c"), flush=True)
        sys.exit(CHILD_STATUS)
    print("child status", reap(pid), flush=True)
    assert runs_on_the_runtime("q") == [0, 2, 4, 6]
    print("parent ran again", flush=True)
'''


def test_a_child_runs_work_of_its_own_after_the_fork_and_its_exit_hook_stops_its_threads():
    result = _run_script(CHILD_WORK)
    lines = _lines(result)
    # "refused" is printed by an exit handler that runs after the binding's hook, in the child: the hook has stopped
    # the runtime that the child itself started.
    assert lines == ["parent ran", "child ran [0, 2, 4, 6] 3", "child after the hook: refused",
                     f"child status {CHILD_STATUS}", "parent ran again"], (lines, result.stderr[-1500:])


NO_WORK_BEFORE_THE_FORK = '''
    pid = os.fork()
    if pid == 0:
        die_with_parent()
        print("child ran", runs_on_the_runtime("c"), flush=True)
        sys.exit(CHILD_STATUS)
    print("child status", reap(pid), flush=True)
    assert runs_on_the_runtime("p") == [0, 2, 4, 6]
    print("parent ran", flush=True)
'''


def test_a_binding_that_started_no_thread_before_the_fork_starts_its_threads_in_the_parent_and_the_child_alike():
    lines = _lines(_run_script(NO_WORK_BEFORE_THE_FORK))
    assert lines == ["child ran [0, 2, 4, 6]", "child after the hook: refused", f"child status {CHILD_STATUS}",
                     "parent ran"], lines
