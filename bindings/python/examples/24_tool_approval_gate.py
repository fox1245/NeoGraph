"""24 — Tool approval gate: "the agent wants to run `rm -rf build/`. Allow?"

Offline. No API key: a stub node stands in for the model, so you can run this
and watch the whole loop.

    python 24_tool_approval_gate.py

This is the shape every coding agent has, and until issue #89 it was
unimplementable on NeoGraph without forking the engine — there was no hook
anywhere between "the model asked for tool X" and "tool X runs".

The gate is one function, consulted for every tool call, returning one of three
verdicts:

    ToolDecision.allow()             run it
    ToolDecision.allow({...})        run it, with these arguments instead
    ToolDecision.deny(reason)        do not; the model is told why, and adapts
    ToolDecision.interrupt(...)      do not; pause the run and ask a human

Permission, audit, argument rewriting and the per-call interrupt are not four
features. They are one primitive wearing four hats: observe and intervene at
the tool-call boundary.

THE ONE THING WORTH UNDERSTANDING

The gate sees every call BEFORE any tool runs. Watch the output: when the run
pauses for approval, `list_files` — which the gate was perfectly happy to allow
— has NOT run either.

That is deliberate. Suppose the gate ran the harmless tools first and only then
asked. The run pauses; the human approves; the engine resumes — and resume
re-enters the node from the top, because a node that interrupted recorded no
writes. `list_files` runs a *second* time. Swap `list_files` for `git commit`
and the approval prompt for `rm -rf` has just double-committed.

Worse, if the human says NO, the harmless tools have already had their effects
and there is nothing to undo them. A permission system in which "denied" does
not mean "nothing happened" is not a permission system.

So: decide everything while nothing has happened yet, then act.
"""

import neograph_engine as ng


# ── Tools ────────────────────────────────────────────────────────────────────

class ListFiles(ng.Tool):
    """Harmless. The gate always allows it — and it still must not run early."""

    def __init__(self):
        super().__init__()
        self.runs = 0

    def get_definition(self):
        d = ng.ChatTool()
        d.name = "list_files"
        d.description = "List files in the working directory"
        return d

    def execute(self, arguments):
        self.runs += 1
        print(f"      [tool] list_files ran  (call #{self.runs})")
        return '{"files": ["README.md", "src/", "build/"]}'

    def get_name(self):
        return "list_files"


class Shell(ng.Tool):
    """The dangerous one."""

    def __init__(self):
        super().__init__()
        self.runs = 0

    def get_definition(self):
        d = ng.ChatTool()
        d.name = "shell"
        d.description = "Run a shell command"
        return d

    def execute(self, arguments):
        self.runs += 1
        print(f"      [tool] shell ran: {arguments.get('cmd')!r}  (call #{self.runs})")
        return '{"exit_code": 0}'

    def get_name(self):
        return "shell"


# ── A stand-in for the model ────────────────────────────────────────────────

class PretendModel(ng.GraphNode):
    """Emits an assistant message asking for two tools, as a real model would."""

    def run(self, _input):
        assistant = {
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {"id": "1", "name": "list_files", "arguments": "{}"},
                {"id": "2", "name": "shell",
                 "arguments": '{"cmd": "rm -rf build/"}'},
            ],
        }
        return [ng.ChannelWrite("messages", [assistant])]

    def get_name(self):
        return "model"


DEFINITION = {
    "name": "approval_gate",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {
        "model": {"type": "approval_model"},
        "tools": {"type": "tool_dispatch"},
    },
    "edges": [
        {"from": "__start__", "to": "model"},
        {"from": "model", "to": "tools"},
        {"from": "tools", "to": "__end__"},
    ],
}

DANGEROUS = {"shell", "write_file", "delete"}


def main():
    ng.NodeFactory.register_type(
        "approval_model", lambda _n, _c, _x: PretendModel())

    list_files, shell = ListFiles(), Shell()
    engine = ng.GraphEngine.compile(
        DEFINITION,
        ng.NodeContext(tools=[list_files, shell]),
        ng.InMemoryCheckpointStore(),   # required: an interrupt has to be resumable
    )

    def gate(call, gctx):
        """Called once per tool call, before any tool runs."""
        if call.name not in DANGEROUS:
            return ng.ToolDecision.allow()

        # gctx.resume_value is None until a human has actually answered — which
        # is how the gate tells "nobody has been asked yet" from "the answer was
        # no", and therefore how it avoids asking the same question forever.
        if gctx.resume_value is None:
            return ng.ToolDecision.interrupt(
                f"{call.name} needs approval",
                {"tool": call.name, "arguments": call.arguments},
            )

        if gctx.resume_value.get("approved"):
            return ng.ToolDecision.allow()

        # A denial is a result, not silence: the model sees it and can adapt,
        # instead of asking for the same tool again on the next turn.
        return ng.ToolDecision.deny("the operator refused this command")

    # The gate lives on the engine, not on RunConfig — resume() builds its own
    # RunConfig, so a per-run gate would vanish the moment the human answered
    # the very prompt it raised, and the dangerous tool would run unchecked.
    engine.set_tool_gate(gate)

    cfg = ng.RunConfig()
    cfg.thread_id = "approval-demo"

    print("\n1. Run — the model asks for list_files + shell\n")
    result = engine.run(cfg)

    assert result.interrupted
    payload = result.interrupt_value["value"]
    print(f"   PAUSED at node {result.interrupt_node!r}")
    print(f"   reason : {result.interrupt_value['reason']}")
    print(f"   tool   : {payload['tool']}")
    print(f"   args   : {payload['arguments']}")
    print()
    print(f"   list_files runs so far: {list_files.runs}"
          "   <- zero. It was allowed, and still has not run.")
    print(f"   shell      runs so far: {shell.runs}")

    # ── The refusal ─────────────────────────────────────────────────────────
    print("\n2. Operator refuses\n")
    refused = engine.resume("approval-demo", {"approved": False})
    print(f"   list_files runs: {list_files.runs}   <- the harmless tool ran, once")
    print(f"   shell      runs: {shell.runs}   <- the refused one never ran")

    tool_msgs = [m for m in refused.output["channels"]["messages"]["value"]
                 if m.get("role") == "tool"]
    for m in tool_msgs:
        print(f"   -> model sees: {m['tool_name']}: {m['content']}")

    # ── And the approval, on a fresh thread ─────────────────────────────────
    list_files.runs = shell.runs = 0
    cfg2 = ng.RunConfig()
    cfg2.thread_id = "approval-demo-2"

    print("\n3. Same run again, but this time the operator approves\n")
    assert engine.run(cfg2).interrupted
    engine.resume("approval-demo-2", {"approved": True})

    print(f"\n   list_files runs: {list_files.runs}"
          "   <- once, not twice. The pause did not make it re-run.")
    print(f"   shell      runs: {shell.runs}   <- approved, so it ran")

    assert list_files.runs == 1, "the approval double-applied a harmless tool"
    assert shell.runs == 1
    print("\nOK\n")


if __name__ == "__main__":
    main()
