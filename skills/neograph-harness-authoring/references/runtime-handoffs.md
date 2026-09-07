# Runtime child synthesis and replacement

This reference is for a host that actually exposes these services. A skill does
not turn ng.compile, ng.replace or an imagined MCP replacement tool into an API.
The compatibility Harness MCP surface compiles/starts workflows; live replacement
requires the native host integration described here.

## Required host packet

Obtain the effective owner/run/version, current lineage head, available registry,
proposal schema (template parameters or full source), input/output contracts,
remaining limits, and result/diagnostic format from the provided host service.
Inspect available configuration if these are not already in the request. Identify
a missing contract rather than guessing a binding or claiming an operation ran.

The model proposes source or parameters. The host supplies authority and performs
the atomic state changes. Provider credentials and trusted grants stay with the host.

## Creating a child

1. Propose source or template parameters for a concrete child task and required
   output. Request only the parent's permitted capabilities and remaining limits.
2. At the required boundary, submit the host's proposal envelope. The host binds
   the actual parent/head, reserves resources, compiles, validates semantics,
   admits, publishes, and binds the child. A model-supplied grant is not authority.
3. Read the result. On rejection, use the returned diagnostic and remaining repair
   allowance. On a bound result, use the exact child binding and input contract.
4. Yield an admitted spawn command, wrapped in await when a result is needed.
   Record the child result or the precise failure; do not report a queued proposal
   as an executed child.

For illustration, the chatbot's reviewed review template emits a checkpoint with
kind="review", turn and source. Its host derives the name "review-" + turn and
verifies source against a reviewed template before binding it. Those field names
are specific to that example, not a universal proposal schema.

## Replacing the current Harness

1. Construct explicit serializable state containing what the successor needs:
   conversation/task progress and any exact inputs needed to rejoin retained work.
2. Yield checkpoint(state, stableSite). The host retains the lease at that boundary.
3. The host durably records synthesis intent, reserves a dynamic compilation,
   compiles/validates/admits the successor, and selects that exact version for
   replacement by generation CAS.
4. The successor starts a new generator. Restore the declared handoff state from
   input.handoff before performing new work; do not initialize counters/history
   from scratch when a handoff is present. The host also binds previous_run_id.
5. Keep the logical parent, child relations, output contract and granted authority
   compatible. Remaining nonrenewable resources transfer to the successor.

The JSON state crosses the boundary, not a QuickJS heap, generator stack, pointer,
promise or native handle. A published graph/version is immutable.

## Retained children and recovery

A retained child's binding and original input identify the existing work. Under
the current native runtime, spawning an inherited binding with its exact recorded
input rejoins that child; changing the input is a conflict. A new task needs a new
host-admitted binding and a newly reserved, attenuated child budget. Replacements
must not repeat completed external work or restore already-spent limits.

Reload the active generation on a stale-head conflict. An uncertain provider or
compiler outcome requires reconciliation through the supplied host path; do not
redispatch it merely because there is no usable answer. Cancellation follows the
logical family. Report the returned execution guarantee accurately.

## Native host implementation entry points

- RuntimeConfig::checkpoint_handler: enqueue the handle/lease, then return promptly.
  Retaining the lease pauses the generation; do host work outside the scheduler.
- ProgramRuntime::prepare_child_synthesis: bind a proposal and an independent host
  grant to the held parent checkpoint.
- ProgramSynthesisGateway: compile, semantic validation and Catalog admission.
- ProgramRuntime::reserve_synthesis: debit one unallocated compile by expected-head
  CAS; refresh the lease's journal head while retaining its checkpoint identity.
- ProgramRuntime::replace: consume the held checkpoint and admitted target, using
  explicit handoff/predecessor input and the source remainder.
- reconnect/recover_child_synthesis: recover the recorded generation/stages or
  return the reconciliation requirement.

These names are C++ host APIs, not JavaScript commands or automatic model tools.
Use the actual repository headers/service schema for signatures and the full
invocation. Generator control retains its Unmanaged guarantee; compiler acceptance
does not prove behavioral quality.
