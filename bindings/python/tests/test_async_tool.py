"""Concurrent tool dispatch reaches Python (issue #96).

#87 made tool calls overlap: three 300 ms tools went 900 ms -> 300 ms, twenty
went 6002 ms -> 300 ms. None of it reached Python, because the speedup only pays
for tools that *suspend*, and every Python tool was a sync `Tool`.

That was not an oversight in #87 — it was the property that made #87 safe to
land. `Tool::execute_async`'s default body runs the sync `execute()` straight
through, so a sync tool never overlaps with its siblings even inside a parallel
group, and existing stateful sync tools therefore cannot be raced. Concurrency
has to be opted into, by saying so.

`ng.AsyncTool` is how a Python tool says so. Subclass it and your tool overlaps;
subclass plain `ng.Tool` and it does not, exactly as before.

WHAT THE SPEEDUP DEPENDS ON, AND THE HONEST BOUNDARY

A Python tool body holds the GIL while it runs. Overlap is real only while a
tool is *not* holding it — i.e. while it is blocked on I/O (a socket read, an
HTTP call, `time.sleep`), which is when CPython releases it. That covers the
case that matters: MCP tools, HTTP tools, database calls.

A Python tool that burns CPU holds the GIL the whole time and will NOT overlap,
no matter how many threads it is handed. `TheBoundaryCpuBoundToolsDoNotOverlap`
pins that, so the docs cannot quietly start promising otherwise.
"""

import time

import neograph_engine as ng
import pytest


SLEEP = 0.3
TOLERANCE = 0.25   # generous: CI machines are noisy, and the claim is 3x, not 3.0x


class _SleepTool(ng.Tool):
    """Sync. Must NOT overlap — that is the compatibility guarantee."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_definition(self):
        d = ng.ChatTool()
        d.name = self._name
        d.description = "sleeps"
        return d

    def execute(self, arguments):
        time.sleep(SLEEP)          # releases the GIL, and still must not overlap
        return '{"ok": true}'

    def get_name(self):
        return self._name


class _AsyncSleepTool(ng.AsyncTool):
    """Declares itself safe to run concurrently."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_definition(self):
        d = ng.ChatTool()
        d.name = self._name
        d.description = "sleeps"
        return d

    def execute(self, arguments):
        time.sleep(SLEEP)
        return '{"ok": true}'

    def get_name(self):
        return self._name


class _ToolCallingNode(ng.GraphNode):
    def __init__(self, names):
        super().__init__()
        self._names = names

    def run(self, _input):
        assistant = {
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {"id": str(i), "name": n, "arguments": "{}"}
                for i, n in enumerate(self._names)
            ],
        }
        return [ng.ChannelWrite("messages", [assistant])]

    def get_name(self):
        return "llm"


def _time_dispatch(tools):
    """Wall clock for one super-step that calls every tool once."""
    names = [t.get_name() for t in tools]
    ng.NodeFactory.register_type(
        "at_llm", lambda _n, _c, _x: _ToolCallingNode(names))

    definition = {
        "name": "async_tool_bench",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "at_llm"}, "tools": {"type": "tool_dispatch"}},
        "edges": [
            {"from": "__start__", "to": "llm"},
            {"from": "llm", "to": "tools"},
            {"from": "tools", "to": "__end__"},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(tools=tools))

    cfg = ng.RunConfig()
    cfg.thread_id = "at"
    started = time.perf_counter()
    result = engine.run(cfg)
    elapsed = time.perf_counter() - started

    tool_msgs = [m for m in result.output["channels"]["messages"]["value"]
                 if m.get("role") == "tool"]
    assert len(tool_msgs) == len(tools), "not every tool produced a result"
    return elapsed


def test_the_base_class_exists():
    """The whole issue in one line: there was nothing to subclass."""
    assert hasattr(ng, "AsyncTool")
    assert issubclass(ng.AsyncTool, ng.Tool)


def test_sync_tools_still_do_not_overlap():
    """The compatibility anchor, and it is the load-bearing one.

    An existing Python tool holding state must not suddenly find itself racing
    a copy of itself. Three 300 ms sync tools take ~900 ms, as they always did.
    """
    tools = [_SleepTool(f"sync{i}") for i in range(3)]

    elapsed = _time_dispatch(tools)

    assert elapsed > 3 * SLEEP * (1 - TOLERANCE), (
        f"sync tools overlapped ({elapsed:.2f}s for 3x{SLEEP}s) — existing "
        "stateful tools can now race each other")


def test_async_tools_overlap():
    """Three 300 ms async tools take ~300 ms, not ~900 ms."""
    tools = [_AsyncSleepTool(f"async{i}") for i in range(3)]

    elapsed = _time_dispatch(tools)

    assert elapsed < 2 * SLEEP, (
        f"async tools did not overlap: {elapsed:.2f}s for 3x{SLEEP}s "
        "(serial would be ~0.9s, concurrent ~0.3s)")


def test_twenty_async_tools_still_take_about_one_sleep():
    """The number #87 measured in C++, now asked of Python.

    Serial: 20 x 300 ms = 6.0 s. Concurrent: ~300 ms.
    """
    tools = [_AsyncSleepTool(f"a{i}") for i in range(20)]

    elapsed = _time_dispatch(tools)
    print(f"\n[MEASURE] 20 async Python tools x {SLEEP}s: {elapsed:.2f}s "
          f"(serial would be {20 * SLEEP:.1f}s, speedup {20 * SLEEP / elapsed:.1f}x)")

    assert elapsed < 4 * SLEEP, (
        f"20 async tools took {elapsed:.2f}s; serial is {20 * SLEEP:.1f}s")


def test_the_boundary_cpu_bound_tools_do_not_overlap():
    """The honest boundary, so the docs cannot quietly overpromise.

    A Python tool that burns CPU holds the GIL for its whole body. It will not
    overlap however many threads it is given — declaring it async buys nothing.
    Only tools that block on I/O (and so release the GIL) get the speedup.
    """

    class _BurnTool(ng.AsyncTool):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_definition(self):
            d = ng.ChatTool()
            d.name = self._name
            d.description = "burns cpu"
            return d

        def execute(self, arguments):
            # Pure-Python arithmetic: the GIL is held throughout.
            total = 0
            for i in range(6_000_000):
                total += i
            return '{"ok": true}'

        def get_name(self):
            return self._name

    one = _time_dispatch([_BurnTool("burn0")])
    three = _time_dispatch([_BurnTool(f"burn{i}") for i in range(3)])

    print(f"\n[MEASURE] cpu-bound: 1 tool {one:.2f}s, 3 tools {three:.2f}s "
          f"(ratio {three / one:.1f}x — overlap would make this ~1.0x)")

    assert three > 2 * one * (1 - TOLERANCE), (
        "CPU-bound Python tools appear to have overlapped, which would mean the "
        "GIL was released somewhere it should not have been")
