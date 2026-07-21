---
name: neograph-harness-authoring
description: Compile and run bounded PR review, bug triage, or research synthesis panels through the NeoGraph Harness MCP tools. Use when a task benefits from multiple schema-constrained workers, strict preflight validation, read-only workspace policy, durable host-brokered resume, compact polling, and URI-linked detailed artifacts.
---

# NeoGraph Harness

## Goal

Run a multi-worker panel without giving the main conversation every worker
trace or allowing an unvalidated workflow to execute.

## Procedure

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
`docs/HARNESS_MCP.md` for the complete request and host setup commands.
