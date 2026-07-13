"""25 — Concurrent tools: three I/O-bound tools in the time of one.

Offline. No API key, no network: the tools sleep instead of calling out.

    python 25_async_tools.py

When the model asks for several tools in one turn, NeoGraph dispatches them
together. Whether they actually *overlap* is the tool's own choice:

    class Slow(ng.Tool):        ->  runs to completion, then the next one starts
    class Slow(ng.AsyncTool):   ->  overlaps with its siblings

This program runs the identical workload both ways and prints the wall clock, so
the difference is a number rather than a claim.

WHY IT IS OPT-IN

A sync `Tool` never overlaps, even inside the dispatcher's parallel group. That
is not a limitation, it is a guarantee: an existing tool that keeps state cannot
suddenly find itself racing a copy of itself. You declare concurrency; it does
not happen to you.

The flip side is that two calls to the same AsyncTool can be in flight at once —
the model is free to ask for the same tool twice in one turn. Keep per-call state
on the stack, not on `self`.

THE BOUNDARY

A Python function holds the GIL while it runs. Your tool overlaps only while it
is *not* holding it — that is, while it is blocked on I/O, which is when CPython
lets go. HTTP calls, socket reads, database queries, `time.sleep`: all release
it. Pure-Python CPU work does not, and no number of threads will change that.
The last section measures exactly that, so the promise stays honest.
"""

import time

import neograph_engine as ng


LATENCY = 0.3   # what one "network call" costs
N = 3


def _definition(names):
    return {
        "name": "async_tools_demo",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"model": {"type": "at_model"}, "tools": {"type": "tool_dispatch"}},
        "edges": [
            {"from": "__start__", "to": "model"},
            {"from": "model", "to": "tools"},
            {"from": "tools", "to": "__end__"},
        ],
    }


class PretendModel(ng.GraphNode):
    """Asks for every tool at once, as a real model does when it can."""

    def __init__(self, names):
        super().__init__()
        self._names = names

    def run(self, _input):
        return [ng.ChannelWrite("messages", [{
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {"id": str(i), "name": n, "arguments": '{"url": "https://example.com"}'}
                for i, n in enumerate(self._names)
            ],
        }])]

    def get_name(self):
        return "model"


def make_tool(base, name):
    """Same body, same latency. The only difference is the base class."""

    class _Tool(base):
        def get_definition(self):
            d = ng.ChatTool()
            d.name = name
            d.description = "Fetch a URL"
            return d

        def execute(self, arguments):
            time.sleep(LATENCY)      # stands in for the network; releases the GIL
            return '{"status": 200}'

        def get_name(self):
            return name

    return _Tool()


def run(base, label):
    tools = [make_tool(base, f"fetch_{i}") for i in range(N)]
    names = [t.get_name() for t in tools]

    ng.NodeFactory.register_type("at_model", lambda _n, _c, _x: PretendModel(names))
    engine = ng.GraphEngine.compile(_definition(names), ng.NodeContext(tools=tools))

    cfg = ng.RunConfig()
    cfg.thread_id = label
    started = time.perf_counter()
    engine.run(cfg)
    elapsed = time.perf_counter() - started

    print(f"  {label:<24} {elapsed:5.2f}s")
    return elapsed


def cpu_bound_boundary():
    """The honest part: CPU-bound Python tools do NOT overlap."""

    def make_burner(name):
        class _Burn(ng.AsyncTool):
            def get_definition(self):
                d = ng.ChatTool()
                d.name = name
                d.description = "burns cpu"
                return d

            def execute(self, arguments):
                total = 0
                for i in range(6_000_000):   # pure Python: the GIL is held throughout
                    total += i
                return '{"ok": true}'

            def get_name(self):
                return name

        return _Burn()

    def timed(count, label):
        tools = [make_burner(f"burn_{i}") for i in range(count)]
        names = [t.get_name() for t in tools]
        ng.NodeFactory.register_type("at_model", lambda _n, _c, _x: PretendModel(names))
        engine = ng.GraphEngine.compile(_definition(names), ng.NodeContext(tools=tools))
        cfg = ng.RunConfig()
        cfg.thread_id = label
        started = time.perf_counter()
        engine.run(cfg)
        elapsed = time.perf_counter() - started
        print(f"  {label:<24} {elapsed:5.2f}s")
        return elapsed

    one = timed(1, "1 cpu-bound AsyncTool")
    three = timed(3, "3 cpu-bound AsyncTools")
    return one, three


def main():
    print(f"\n{N} tools, each waiting {LATENCY}s on \"the network\"\n")

    serial = run(ng.Tool, "ng.Tool")
    concurrent = run(ng.AsyncTool, "ng.AsyncTool")

    print(f"\n  -> {serial / concurrent:.1f}x. Serial is {N} x {LATENCY}s; "
          f"concurrent is bounded by the slowest single call.\n")

    print("And the boundary — the same trick on CPU-bound work:\n")
    one, three = cpu_bound_boundary()
    print(f"\n  -> {three / one:.1f}x, not 1.0x. A Python function holds the GIL")
    print("     while it runs, so pure-Python CPU work cannot overlap. Threads")
    print("     do not help; declaring such a tool AsyncTool buys nothing.")
    print("     (numpy / C extensions / subprocesses release the GIL, and do overlap.)\n")


if __name__ == "__main__":
    main()
