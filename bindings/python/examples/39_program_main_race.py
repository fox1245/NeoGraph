"""39 - Racing runs from main(): the first to finish wins and the others are cancelled.

`main()` can start several Core runs at once. `ng.race([...])` returns the first member to
finish and cancels the rest; `ng.quorum([...], {required_successes: n})` returns once n
members have succeeded; `ng.await(command, timeout_ms)` gives one command a deadline and
ends the run TimedOut (P_AWAIT_TIMEOUT) when it passes. A cancelled Core run sees its
`cancel_token` flip; a node that polls the token stops early. The three modes below are one
Program started with different inputs.

Run:
    python 39_program_main_race.py

smoke-expect: race: winner mirror-b
smoke-expect: mirror-a: cancelled
smoke-expect: quorum: ['mirror-b', 'mirror-c']
smoke-expect: await: TimedOut P_AWAIT_TIMEOUT
smoke-forbid: [neograph] lint
"""

import time

import neograph_engine as ng
from _program import admit, budget, make_host, node

EVENTS = []                                  # (what happened, which mirror), in order


@node("lookup.search")
def search(name, input, config):
    who, token = input.state.get("who"), input.ctx.cancel_token
    deadline = time.monotonic() + input.state.get("delay") / 1000
    while time.monotonic() < deadline:
        if token.is_cancelled():             # the race or quorum no longer needs this run
            EVENTS.append(("cancelled", who))
            raise RuntimeError(f"{who} was cancelled")
        time.sleep(0.005)
    EVENTS.append(("finished", who))
    return [ng.ChannelWrite("trace", [who])]


PROGRAM = r"""
export function define() {
  return ng.graph("lookup")
    .channel("trace", {reducer: "append", initial: []})
    .channel("who", {reducer: "overwrite", initial: ""})
    .channel("delay", {reducer: "overwrite", initial: 0})
    .node("search", {type: "lookup.search"})
    .entry("search").exit("search");
}
const ask = (who, delay) => ng.callCore("lookup", {who, delay}, "lookup:" + who);

export function* main(input) {
  if (input.mode === "race") {
    const winner = yield ng.race([ask("mirror-a", 5000), ask("mirror-b", 30)], {max_in_flight: 2}, "race");
    return {winner: winner.who};
  }
  if (input.mode === "quorum") {
    const done = yield ng.quorum([ask("mirror-a", 5000), ask("mirror-b", 30), ask("mirror-c", 40)],
                                 {required_successes: 2, max_in_flight: 3}, "quorum");
    return {done: done.map((r) => r.who).sort()};
  }
  yield ng.await(ask("mirror-a", 5000), 150, "deadline");     // gives up after 150 ms
  return {unreachable: true};
}
"""

host = make_host(["lookup.search"], scheduler_threads=2)
run_budget = budget()
version = admit(host, PROGRAM, run_budget, "lookup.js")


def fate(who):
    """What the node of `who` did, once it has done it (a cancelled node polls every 5 ms)."""
    for _ in range(200):
        for what, name in EVENTS:
            if name == who:
                return what
        time.sleep(0.01)
    return "still running"


def run(mode):
    EVENTS.clear()
    started = time.monotonic()
    result = host.run(version, {"mode": mode}, run_budget)
    return result, time.monotonic() - started


raced, seconds = run("race")
print(f"race: winner {raced.output['winner']}, {seconds:.1f} s (the slow mirror needed 5 s)")
print("  mirror-a:", fate("mirror-a"))
assert raced.output == {"winner": "mirror-b"} and seconds < 3

quorum, seconds = run("quorum")
print("quorum:", quorum.output["done"], f"{seconds:.1f} s")
print("  mirror-a:", fate("mirror-a"))
assert quorum.output == {"done": ["mirror-b", "mirror-c"]} and seconds < 3

late, seconds = run("await")
print("await:", late.status.name, late.failure.code, f"{seconds:.1f} s")
print("  mirror-a:", fate("mirror-a"))
assert late.status == ng.ProgramTerminalStatus.TimedOut and late.failure.code == "P_AWAIT_TIMEOUT"
assert seconds < 3
