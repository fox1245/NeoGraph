"""24 — Tool approval: pause, refuse, and approve a scripted shell request.

Run:
    python 24_tool_approval_gate.py

No API key or real shell action. The model and tools are deliberately scripted:
their counters make it possible to inspect the engine's approval behavior.

The gate receives every tool call before dispatch begins and returns allow,
allow with rewritten arguments, deny, or interrupt. If a call interrupts the
batch, neither sibling tool runs before the pause. On refusal, allowed siblings
can run but the denied shell call does not; approval executes each tool once.
This avoids double-applying allowed effects when dispatch resumes from the
interrupted node.
"""

import sys

import neograph_engine as ng

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")


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
    """Simulates a dangerous action without executing a shell command."""

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
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
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
