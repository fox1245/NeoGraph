# QuickJS source authoring

## Establish the host contract

Identify these bindings before writing code. Use supplied configuration or inspect
the available schema; do not fill missing names with plausible guesses.

| Contract | What you need |
|---|---|
| JS API manifest | Exact builder and command signatures for this build |
| Registry | Node/reducer/condition/import names, node config schemas and effects |
| Invocation | Input JSON shape and the Core name available to callCore |
| Results | Core channel names and the generator's required terminal output |
| Children | Admitted binding names, child inputs/outputs and granted limits |
| Compiler bridge | Accepted source envelope, diagnostics and remaining repair allowance |

The built-in JS manifest lists syntax, not application nodes. For example,
probe.node belongs to the capability test palette; it is not an application LLM
node. The fixed capability probe checks its case-specific graph/command contract,
not arbitrary production behavior.

## Compile-time graph versus runtime Program

**define()** is a synchronous, compile-time function returning one open builder.
Create nodes and edges with that builder. Its methods return the builder, not a
node handle. Node behavior comes from the host's registered C++ implementation;
ordinary JS callbacks are not graph nodes.

**main(input)** is an optional synchronous generator running later. Pure JS can
choose branches, build arrays and loops, and transform JSON. Runtime effects use
sealed ng commands yielded from the generator. Do not call runtime commands from
define(), mutate a published graph, return a graph-shaped object, or use async
functions, promises, timers, require(), ambient I/O, eval or dynamic imports.

This plumbing example assumes the host has registered the probe palette and
requires numeric input.value. Its node is a no-op, so it illustrates data flow,
not an LLM action or a named capability-case answer:

~~~javascript
export function define() {
  const g = ng.graph("example");
  g.channel("value", {reducer: "probe.overwrite", initial: 0});
  g.node("step", {type: "probe.node"});
  g.entry("step");
  g.exit("step");
  return g;
}

export function* main(input) {
  const result = yield ng.callCore("example", {value: input.value}, "copy:value");
  return {value: result.channels.value.value};
}
~~~

Use exactly the graph name provided by the host in callCore. Neither "main",
"example", "capability", nor a node's name is a universal alias.

## Data flow and topology

A normal callCore input is a map of channel names to incoming values. Each
declared channel applies its registered reducer. A normal Core result contains
serialized channels; read the channel's value, not its wrapper.

For example, the chatbot's existing template invokes its answer node with:

~~~javascript
const reply = yield ng.callCore(
  "main", {payload: {phase: "answer", task: task}}, "answer"
);
const answer = reply.channels.result.value;
~~~

Here payload/result are declared channels, and chat.step reads payload.phase.
Those names and behavior come from that host's registry/template. Copying this
fragment into a different registry does not create those bindings.

A declaration-only module retains the Core result shape. With main(), the
generator's return must match the separately admitted Program output contract.
The capability evaluator sometimes supplies synthetic command responses such as
{accepted: true}; use that case's stated response contract rather than adding a
Core channel wrapper to a synthetic response.

| Intent | Construction and important detail |
|---|---|
| Linear path | Add nodes, entry/exit, and every connecting edge |
| Conditional routing | conditionalEdge(from, registeredCondition, routes); map every condition label to a node |
| Static fan-out/fan-in | Add both outgoing branches and incoming join edges; add barrier(joinNode, branchNames) when the join requires all branches |
| Parallel writes | Choose host reducers/channels that handle concurrent writes; edges alone do not define merging |
| Graph interrupts/retry | Use the manifest's interruptBefore/After and retryPolicy keys; these are separate from JS loops or logical retries |

## Compose runtime commands

- callCore(coreName, input, site) produces a command; yielding it performs the
  Core invocation and returns its result.
- all(commands, {max_in_flight: N}, site) accepts sealed commands. Build the list
  with ordinary JS and yield the all command once. Do not yield the raw array,
  turn the commands into Promises, or use yield* on a sealed command.
- spawn(binding, input, site) selects a host-admitted child binding. To wait for
  its result, wrap the command in await(spawnCommand, timeoutMs, site).
- checkpoint(state, site) publishes explicit JSON state. It does not compile,
  admit or replace anything on its own.
- emit and cancelScope have the manifest's declared semantics. hostCapability
  requires an admitted import slot; it is not a route to an arbitrary native API.

For example, given an admitted binding and its exact original input:

~~~javascript
const result = yield ng.await(
  ng.spawn(binding, originalChildInput, "child:spawn"),
  timeoutMs,
  "child:wait"
);
~~~

An emitted child ID is not itself a sealed await command. Use the host's actual
join/recovery contract. See [runtime-handoffs.md](runtime-handoffs.md) when retaining children across a
replacement.

Source-site labels participate in durable coordinates. Derive them from stable
task identifiers or deterministic indices; avoid clock/random values. Existing
completed operations must retain their original inputs on replay. A JS loop,
retry, parallel branch or new child does not replenish a budget.

A loop based on a successful result such as accepted=false is a logical retry.
A failed Core command is a Program outcome; do not assume an ordinary JS
try/catch can resume it. Use the host's failure/resume/reconciliation contract.

## Output, compile and repair

Use the envelope requested by the active surface:
- Source evaluation: exactly one JSON object whose source string is the complete
  module. No Markdown fences, patches, ProgramBundle JSON or explanatory prose.
- Harness MCP: first discover neograph_schema. Put the module under
  harness.mode="javascript", source_id and source, together with all required
  task/worker/budget/policy fields. Source alone is not a complete MCP request.
- A host-native proposal: follow that host's schema. This skill does not define a
  universal free-form compilation or replacement RPC.

Submit through the provided compiler bridge. In evaluation mode the host submits
your returned source and returns the diagnostic; no model-callable tool is implied.
In MCP mode use neograph_compile only when that tool is available.

On failure, locate the reported code/path/source site and repair the violated
contract. For example:
- unknown node/reducer/condition: match the registered binding and config;
- wrong Core binding: match the admitted graph name throughout the commands;
- non-command yield: produce one sealed command or an admitted structured join;
- wrong output/input: correct the channel mapping or Program result contract;
- missing edge/barrier: fix the lowered topology, not just the source wording.

Return a complete replacement source within the existing repair allowance.
Do not erase diagnostics, widen grants, add unrelated operations, or claim success
before the compiler and task-specific checks accept it.

The checkout's capability bridge can be inspected and used as follows:

~~~text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
~~~

Build target: program_dsl_capability_probe. Named cases require their own supplied
contract; successful compilation alone does not mean a case passed, and probe
success does not grant production execution. The runner
scripts/run_dsl_capability_eval.ts performs bounded model/diagnostic iterations.
