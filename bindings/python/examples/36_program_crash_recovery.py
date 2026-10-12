"""36 - Crash recovery: a new process finishes a run whose process died, and the model call is not repeated.

A host created with `sqlite_directory=` keeps its Program versions, run records, checkpoints
and a journal of provider calls on disk. Process A runs a Program to a question, is answered,
makes its model call and then dies (`os._exit`) before the run ends. Process B opens the same
directory with the same registry and calls `host.reconnect(run_id)`. The run continues from its
last checkpoint, the journaled reply is replayed instead of being sent again, and the call is
charged once. The scripted model lives in this parent process, so it counts every request.

A host that binds a provider also needs `native_history_archive=`: the journal keeps every
reply under the archive's custody. Child budgets (`max_child_depth`) are what let a run be
recovered from its checkpoint; without them a lease holds the run until its wall time ends.

Run:
    python 36_program_crash_recovery.py

smoke-os: linux, darwin
smoke-expect: process A: paused, answered, then crashed (exit 9)
smoke-expect: model calls after the crash: 1
smoke-expect: process B: Completed on attempt 2, trace ['ask', 'after']
smoke-expect: model calls after recovery: 1
smoke-expect: charged once: 30 tokens, reply replayed: True
smoke-forbid: [neograph] lint
"""

import json
import os
import subprocess
import sys
import tempfile

import neograph_engine as ng
from _program import admit, budget, ceiling, make_model_host, model_url, native_archive, node
from _scripted import ScriptedModel, say


@node("crash.after")
def after(name, input, config):
    if os.environ.get("EXAMPLE_CRASH") == "1":
        os._exit(9)                                   # the process dies after the model call
    return [ng.ChannelWrite("trace", [name])]


PROGRAM = r"""
export function define() {
  return ng.graph("chat")
    .channel("messages", {reducer: "append", initial: [{role: "user", content: "Name a river."}]})
    .channel("trace", {reducer: "append", initial: []})
    .node("ask", {type: "llm_call"})
    .node("after", {type: "crash.after"})
    .entry("ask").edge("ask", "after").exit("after")
    .interruptBefore("ask");
}
"""
LIMITS = dict(model_tokens=1_000, monetary_microunits=1_000_000, max_child_depth=1, max_total_children=1)


def open_host(url, directory, first):
    return make_model_host(
        url, ["crash.after"], host_ceiling=ceiling(model_tokens=10_000_000,
                                                   monetary_microunits=10_000_000,
                                                   max_child_depth=1, max_total_children=1),
        sqlite_directory=directory, native_history_archive=native_archive(directory, url, first=first))


def child(phase, url, directory, run_id=""):
    host = open_host(url, directory, first=phase == "first")
    if phase == "first":                              # process A
        version = admit(host, PROGRAM, budget(**LIMITS), "chat.js")
        paused = host.run(version, {}, budget(**LIMITS))
        print(json.dumps({"run_id": paused.run_id, "status": paused.status.name}), flush=True)
        os.environ["EXAMPLE_CRASH"] = "1"
        host.resume(paused.run_id, {}).wait()         # never returns: the node exits the process
    else:                                             # process B
        result = host.reconnect(run_id).wait()
        print(json.dumps({"status": result.status.name, "attempt": result.attempt,
                          "trace": result.execution_trace, "tokens": result.usage.model_tokens,
                          "replayed": [r.replayed for r in result.monetary_records]}), flush=True)


if len(sys.argv) > 1:
    child(*sys.argv[1:])
    sys.exit(0)

with ScriptedModel([say("The Seine.").with_usage(20, 10), say("never asked for")]) as model, \
        tempfile.TemporaryDirectory() as temp:
    directory = os.path.realpath(temp)     # the native archive wants the canonical path (macOS: /var -> /private/var)
    def spawn(*args):
        return subprocess.run([sys.executable, __file__, *args], capture_output=True, text=True)

    a = spawn("first", model_url(model), directory)
    if not a.stdout.strip():
        sys.exit(f"process A printed nothing (exit {a.returncode}):\n{a.stderr}")
    paused = json.loads(a.stdout.splitlines()[0])
    print(f"process A: paused, answered, then crashed (exit {a.returncode})")
    print("model calls after the crash:", model.call_count)

    b = spawn("recover", model_url(model), directory, paused["run_id"])
    done = json.loads(b.stdout.splitlines()[-1])
    print(f"process B: {done['status']} on attempt {done['attempt']}, trace {done['trace']}")
    print("model calls after recovery:", model.call_count)
    print(f"charged once: {done['tokens']} tokens, reply replayed: {done['replayed'][0]}")

    assert a.returncode == 9 and paused["status"] == "Interrupted", a.stderr
    assert b.returncode == 0 and done["status"] == "Completed" and done["trace"] == ["ask", "after"], b.stderr
    assert model.call_count == 1 and done["tokens"] == 30 and done["replayed"] == [True]
