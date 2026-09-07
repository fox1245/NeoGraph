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
- **MCP review/research panel:** follow the procedure below. For JavaScript input,
  also read the authoring reference and use the schema's `harness.mode` contract.
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

## MCP panel procedure

1. Call `neograph_schema`; use only presets and fields returned by this build.
2. Build one request with a precise objective, acceptance criteria, bounded
   budgets, and a JSON output schema for every worker.
3. For review work, use `pr_review_panel`, set `policy.read_only` to true, and
   set `policy.evidence_required` to the evidence fields required in every
   finding schema.
4. Give each worker only the tool IDs it needs. Mark read-only tools and list
   path-bearing string arguments in `path_arguments`. Set explicit
   `policy.workspace_roots` whenever such arguments exist.
5. Call `neograph_compile`. If `ok` is false, fix diagnostics by `phase`,
   `path`, and `source`; never call start with a rejected request.
6. Call `neograph_start` with the retained `artifact_id`.
7. Poll `neograph_get` with `run_id`. If status is `awaiting_tool_results` or
   `input_required`, fulfill only the returned `pending` call, then call
   `neograph_resume` with the same `run_id`, exact `call_id`, and a result that
   conforms to `result_schema`. Treat an identical duplicate as acknowledged;
   never substitute a different call ID.
8. Continue polling until status is terminal. Keep the compact result in the
   main context.
9. Dereference a returned `neograph://runs/...` URI through `neograph_get`
   with its `run_id` only when the final answer needs worker details or the
   execution trace.
10. Report partial, zero-findings, timeout, cancelled, expired, max-step, and failed
   outcomes exactly; do not turn them into a generic success.

## Anti-Patterns

- Do not skip `neograph_compile` for an inline request.
- Do not attach a broad tool catalog to every worker.
- Do not configure path-bearing tools without workspace roots.
- Do not treat malformed or empty worker output as an empty findings list.
- Do not fetch detailed traces before the compact result proves they are needed.
- Do not add write-capable tools to a read-only review.
- Do not retry a host result with modified data after its call ID was consumed.
- Do not assume MCP Tasks is core protocol support. Use stable `neograph_get`
  polling unless the server and the individual request explicitly opt into the
  experimental `io.modelcontextprotocol/tasks` extension.

## Example

For a PR review, collect the diff with the host's repository tools, place it in
the task objective, and use two workers with distinct correctness and security
instructions. Require `file`, `line`, and `evidence` in each finding. See
[HARNESS_MCP.md](../../docs/HARNESS_MCP.md) in the repository for the complete
request and host setup commands. A host integrating this skill must actually load
the relevant text into the authoring agent's context; merely storing the file
does not make a runtime model read it.
