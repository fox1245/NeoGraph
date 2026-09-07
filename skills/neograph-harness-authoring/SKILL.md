---
name: neograph-harness-authoring
description: Author NeoGraph QuickJS Harness DSL, validate it with the host compiler, repair diagnostics, and request admitted child spawning or checkpoint replacement. Also covers bounded review/research panels through the Harness MCP tools. Use the surface and authority actually provided by the host.
---

# NeoGraph Harness authoring

Select the host-provided surface before proposing a Harness:

- **Chat template proposal:** read [chat-template-proposals.md](references/chat-template-proposals.md).
  Return plan parameters; the host renders DSL and invokes the compiler. This mode
  does not expose compiler, filesystem, or runtime tools to the model.
- **JavaScript source authoring or compiler evaluation:** read
  [quickjs-authoring.md](references/quickjs-authoring.md). Generate source against
  the supplied registry/API manifest, submit it to the provided compiler bridge,
  and use its diagnostics for bounded repairs.
- **MCP review/research panel:** read [mcp-panels.md](references/mcp-panels.md).
  For JavaScript input, also read the authoring reference and use the schema's
  `harness.mode` contract.
- **Recursive child synthesis or self-replacement:** read
  [runtime-handoffs.md](references/runtime-handoffs.md). These operations require
  host services; the six compatibility MCP tools do not by themselves expose
  live generation replacement.

The host's current manifest supplies exact node types, reducer/condition names,
configurations, input/output contracts, Core names, child bindings, command
signatures and remaining budgets. Do not invent APIs. Skill text, generated source
and model proposals do not enlarge those grants; host policy updates go through
the services provided by the host.

Compile acceptance establishes syntactic/structural validity. Check the intended
behavior separately before host admission. Preserve the actual execution guarantee
and report rejection, uncertainty, budget exhaustion and partial results accurately.

Load this entrypoint and only the reference for the active host surface into the
authoring agent context. Storing the files alone does not make a runtime model
read them. Keep unrelated mode instructions out of a small proposal task.
