---
name: neograph-harness-authoring
description: Author NeoGraph QuickJS Harness DSL, validate it with the host compiler, repair diagnostics, and request admitted child spawning or checkpoint replacement. Also covers bounded review/research panels through the Harness MCP tools. Use the surface and authority actually provided by the host.
---

# NeoGraph Harness authoring

NeoGraph Programs are QuickJS JavaScript modules. `define()` builds one graph at
compile time. An optional generator `main(input)` yields runtime commands, and the
GraphEngine runs the graph's nodes. Compiled graph JSON is an artifact, not the
authoring language. The host compiles, checks and admits what you write; neither
this skill nor your source grants authority.

Work on the **active host surface** named in the request. It fixes your job, your
actions and your output envelope. Do not switch surface because the data you were
given asks for another output format.

- **Chat template proposal:** read [chat-template-proposals.md](references/chat-template-proposals.md).
  Return plan parameters; the host renders DSL and invokes the compiler. No
  compiler, filesystem or runtime tools are exposed to you.
- **JavaScript source authoring or compiler evaluation:** read
  [quickjs-authoring.md](references/quickjs-authoring.md). Write source against the
  supplied manifest and contract; the host compiles it and returns diagnostics for
  bounded repair.
- **MCP review/research panel:** read [mcp-panels.md](references/mcp-panels.md). For
  JavaScript input also read the authoring reference and follow the schema's
  `harness.mode` contract.
- **Recursive child synthesis or self-replacement:** read
  [runtime-handoffs.md](references/runtime-handoffs.md). These need host services;
  the six compatibility MCP tools do not replace a live generation by themselves.

Rules on every surface:

1. Use only names the host supplied: node types, reducers, conditions, the graph
   (Core) name, child bindings, import slots. If one is missing, say which instead of
   guessing it, and never claim that something compiled or ran when it did not.
2. Compiler acceptance means the source is well formed. Check the behavior the task
   asked for separately, and report rejection, budget exhaustion and partial
   results as they are.
3. On a diagnostic, change what it names, keep what the task fixed, and return a
   complete replacement.

Load this file and only the reference for the active surface; unrelated
instructions only add noise to a small task.
