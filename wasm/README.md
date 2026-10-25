# NeoGraph on WebAssembly

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This directory holds the WebAssembly programs that CI builds and runs.

- `module.cpp`, `programs.cpp` and `neograph.mjs` are the JavaScript module: compile a graph from JSON and run it, or
  compile, admit and run a Program (with pauses, forks and a record of every run), with the model calls sent through the
  host's `fetch`. `neograph.d.ts` has its types. `examples/` has Node programs and a Cloudflare Workers-style handler that use it,
  and `test/` has the offline tests that CI runs (a local HTTP server stands in for the model).
- `smoke.cpp` compiles a graph with one `DoubleNode`, writes `doubled = seed * 2`, and runs it with
  `InMemoryCheckpointStore`; with `seed = 21` it prints `doubled = 42` and `trace = d`.
- `checks.cpp` runs the checks that differ on a 32-bit host without threads (below).

The plan is in [#535](https://github.com/fox1245/NeoGraph/issues/535), and this is not a browser SDK.

## Status

| What | State |
|---|---|
| Toolchain | Emscripten 6.0.13, pinned in CI to the emsdk release commit. |
| Build modes | Single-thread (the default on WebAssembly) and pthread, both built and run in CI. The JavaScript module is single-thread. |
| Runtime | Node.js 22 and 24, in CI. The Workers example runs in Node, called with a `Request`; it has not run on workerd. The graph smoke also runs in Chromium through `smoke.html` and `serve_smoke.py` (below); the JavaScript module in a browser, Deno and WASI are not verified. |
| SDK | SchemaProvider 0.4.0, interface revision 7, at the commit that `cmake/NeoGraphSchemaProvider.cmake` pins. It builds without libcurl and without threads, and its JavaScript host (`sp_js_host`) is the executor and the `fetch` transport of the module. |
| Components | Core, LLM (model calls through the host), and Program (the runtime on the host's loop; the stores are in memory or the host's own). |

## Build

The build needs Emscripten 6.0.13 (C++20 coroutines, `<stop_token>` and `std::bit_cast` are in its libc++), CMake 3.20
or newer and Ninja. ASYNC, MCP client, A2A, gRPC and the SQLite and PostgreSQL stores need sockets, TLS or system
libraries that a WebAssembly host does not provide, so CMake refuses them there and they are switched off below. LLM is
built for the host (below).

Single-thread, the default on WebAssembly:

```bash
source /path/to/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON -DNEOGRAPH_BUILD_WASM_JS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF -DNEOGRAPH_BUILD_MCP_SERVER=OFF -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke neograph_wasm_checks neograph_js
node build-wasm/wasm/smoke.js
node build-wasm/wasm/checks.js
node wasm/test/llm_host_test.mjs build-wasm/wasm
node wasm/test/workers_test.mjs build-wasm/wasm
node wasm/test/program_test.mjs build-wasm/wasm
```

To also build the Program store tests (the conformance suites against the JavaScript stores and the store checks, see
*Program stores in JavaScript* and *Tests*), add `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON` to the configure line; it fetches
googletest. Then:

```bash
cmake --build build-wasm --target neograph_wasm_store_conformance neograph_wasm_store_checks
node wasm/test/store_conformance_test.mjs build-wasm/wasm
node wasm/test/js_stores_test.mjs build-wasm/wasm
```

`NEOGRAPH_BUILD_WASM_JS=ON` builds the JavaScript module into `build-wasm/wasm/`: `neograph_module.wasm`,
`neograph_module.mjs` (the Emscripten loader), `neograph.mjs` (the API, copied from this directory), `stores.mjs` (the
bridge to the host's stores, which `neograph.mjs` imports) and `neograph.d.ts` (its types). Copy the first four into a
project to use it. With `NEOGRAPH_BUILD_PROGRAM=ON` the module has the Program
API ([below](#programs-in-the-module)); `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` adds the QuickJS control runtime that a
Program written in JavaScript needs, and without it only a Program given as a document is accepted.
`NEOGRAPH_WASM_JS_PROGRAMS=OFF` builds a module that runs graphs only, which is about half the size (see
[Measured sizes](#measured-sizes)). Pthread: add `-DNEOGRAPH_THREADS=ON` to the configure line, leave
`NEOGRAPH_BUILD_WASM_JS` and `NEOGRAPH_BUILD_LLM` off, and use another build directory.

`NEOGRAPH_THREADS` is the one switch between the two modes. It is `OFF` on Emscripten and WASI and `ON` elsewhere.

- **Single-thread (`OFF`).** No `-pthread` and no `Threads::Threads`. The `asio` target exports `ASIO_DISABLE_THREADS`,
  and the SDK is built the same way (`SP_THREADS=OFF`, no libcurl transport). Work runs on an executor the host
  supplies.
- **pthread (`ON`).** `-pthread` everywhere and a pool of four workers for the smoke programs. The SDK is built with
  `SP_THREADS=ON`; it still has no libcurl transport on WebAssembly.
- **Exceptions.** `-fexceptions` (JavaScript-based exceptions) is set for every NeoGraph target, C sources included,
  and the SDK takes the same flag. A configuration that already names `-fexceptions` or `-fwasm-exceptions` is kept,
  and configuration fails when exceptions are disabled.
- **One setting per program.** Asio is header-only, so two translation units that disagree about threads are a
  one-definition violation that nothing diagnoses. The generated `neograph/build_config.h` records the setting, every
  public header reaches it through `neograph/api.h`, and a translation unit that disagrees stops compiling. The SDK
  records its own setting the same way (`sp/build_config.h`), and configuration fails when the two differ.

## The JavaScript module

```js
import { load } from './neograph.mjs';

const neograph = await load();                      // a Worker passes its imported module: load({ wasmModule })
const result = await neograph.run({
  graph: {                                          // a graph definition, as JSON text or an object
    channels: { messages: { reducer: 'append' } },
    nodes: { llm: { type: 'llm_call' } },
    edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
  },
  provider: {                                       // given at run time; nothing of it is in the module
    baseUrl: 'https://api.example.com',             // the origin of an OpenAI-compatible Chat Completions endpoint
    path: '/v1/chat/completions',
    apiKey: env.API_KEY,
    model: 'a-model',
  },
  input: { messages: [{ role: 'user', content: 'Hello' }] },
  onEvent: (event) => console.log(event),           // optional: streams the run
});
console.log(result.text);
```

- **`load(options)`** instantiates the module once. In Node and a browser it finds `neograph_module.wasm` beside
  `neograph_module.mjs`. A Worker cannot compile bytes at run time, so it passes the `WebAssembly.Module` that its
  bundler imports as `wasmModule`. `wasmBinary` and `locateFile` are the Emscripten options.
- **`neograph.compile(graph, provider)`** returns a `Graph` that runs any number of times (`graph.run(input, run)`) until
  `graph.release()`. **`neograph.run({ graph, provider, input, ...run })`** compiles, runs once and releases.
- **`provider`** is `{ baseUrl, path, apiKey, model }` for a Chat Completions endpoint. `family` names another wire
  family and `paths: { buffered, streaming }` another route, and `descriptor` takes a complete SchemaProvider
  descriptor. `instructions`, `maxOutputTokens` and `temperature` are optional. The key is sent as the descriptor's
  family sends a key.
- **`run`** options: `onEvent` (streams the run: the model is called in streaming mode), `signal` (an `AbortSignal`
  that cancels the run and the model call) and `timeoutMs`.
- **The result** is `{ status, text, output, trace, usage }`: `status` is `completed`, `interrupted`, `step_limit` and so
  on; `text` is the last assistant message; `output` is the final state (`output.channels.<name>.value`); `trace` lists
  the nodes that ran; `usage` has `input_tokens`, `output_tokens` and `total_tokens` when the provider reports them.
- **Events** are `{ type, node, data }` with `type` one of `node_start`, `node_end`, `llm_token` (`data` is the text
  of the delta), `channel_write`, `interrupt` and `error`, in the order the engine produced them.
- **Errors** are a `NeoGraphError`: `type` is `provider` (the model call failed: `kind`, `reason` and `httpStatus` say
  how), `cancelled` or `error` (an invalid graph or provider, or a failing node). A graph or provider that is invalid
  throws from `compile`.

Graphs use the built-in node types that need no tool: `llm_call`, `intent_classifier` and `subgraph`. Tools and nodes
written in JavaScript are not part of this milestone; a Program has its own surface, below.

## Programs in the module

A Program is a small JavaScript module that the host compiles and admits, and that runs a graph under a budget: a run
can pause at an interrupt and wait for an answer, be resumed or forked, and leaves a record of every step in the
stores ([the Program guide](../docs/program-guide.md)). The module compiles, admits and runs one from JavaScript, on
the one thread of the host's event loop:

```js
import { load } from './neograph.mjs';

const SOURCE = `
export function define() {
  return ng.graph("two-turns")
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "llm_call"})
    .node("second", {type: "llm_call"})
    .entry("first").edge("first", "second").exit("second")
    .interruptBefore("second");                      // the run stops here and waits for an answer
}
export function* main(input) {
  const reply = yield ng.callCore("two-turns", input, "run");
  return {messages: reply.messages};
}
`;

const neograph = await load();
const programs = neograph.programs({
  provider: {
    baseUrl: 'https://api.example.com', path: '/v1/chat/completions', apiKey: env.API_KEY, model: 'a-model',
    catalogue: { version: 1, models: [{ id: 'a-model', input_limit: 4000, output_limit: 1000 }] },
  },
});
const version = await programs.admit(SOURCE);                  // compile and admit
const run = programs.start(version, {
  input: { messages: [{ role: 'user', content: 'Hello' }] },
  onEvent: (event) => console.log(event.kind),                 // optional: the run's events, in order
});
const paused = await run.result;                               // status 'interrupted'; paused.interrupt says where
const done = await run.resume('Go on').result;                 // the answer is the next message; 'completed'
console.log(done.output.messages.at(-1).content);
programs.release();
```

- **`neograph.programs({ provider, owner, limits, budget, stores, javascript })`** makes a `Programs` object over one provider and
  one set of stores, and throws a `NeoGraphError` when the provider is invalid or the module was built without Program
  support. `provider` is as for `compile`, with one addition: `catalogue`, the SDK's model catalogue (`{ version: 1,
  models: [...] }`), which is laid over the embedded one. `owner` scopes the Programs and runs of the object.
  `stores` are the host's own stores ([Program stores in JavaScript](#program-stores-in-javascript)); a store that is left
  out is in memory. `limits` is the host's ceiling, the most any admitted Program may be given (its defaults allow one
  dynamic compile, a child depth of four, and four children per run: `maxDynamicCompiles: 1`, `maxChildDepth: 4`,
  `maxTotalChildren: 4`; a host that synthesizes children or builds deeper hierarchies raises `maxDynamicCompiles`,
  `maxChildDepth` and `maxTotalChildren` there), and `budget` is the budget of a run that gives none, both written as
  below. `javascript` is `{ stackBytes, defineTimeMs }`, the stack that a control source may use and the time that
  `define()` may run, described after the budgets.
- **`programs.admit(source)`** compiles a Program and admits it, and resolves with `{ id, bundleId, name }`. `source` is
  the text of a JavaScript control source, `{ javascript, name, budget }` or `{ javascript, name, budgetBounds: {
  minimum, maximum } }` (`budgetBounds` takes precedence if both are present), or `{ document, name }` for a Program
  already in the document form. `budget` is the exact budget runs of this version are granted; `budgetBounds` is the
  range a grant may take, which a version that will be replaced needs, because a replacement carries the source's
  remaining budget, below the first grant. Members left out keep the host's default budget (`budget` and
  `budgetBounds.maximum`) or nothing (`0`, in `budgetBounds.minimum`, so `minimum` must set the structural floor
  members). The minimum must meet the Program's structural floor (at least one concurrent attempt, Program operation and
  Core step; at least one level and one child when the Program dispatches children), and the compiler refuses one that
  does not. Without either, the source's own declaration applies. A source the compiler or the admission policy refuses
  rejects with a `NeoGraphError` of type `compile` or `admission`, whose `diagnostics` list `{ code, message, pointer,
  witness }`. Admitting the same source and budget bounds again gives the same version; a different `budget` or
  `budgetBounds` compiles a distinct bundle and version.
- **`programs.start(version, { input, budget, runId, onEvent, signal })`** returns a `ProgramRun` at once.
  **`programs.run(version, options)`** is `start(...).result`.
- **`ProgramRun`** has `id`, `versionId`, `result`, `resume(value, options)`, `fork(value, options)`, `handoff()`,
  `releaseHandoff()`, `replace(version, options)`, `migrateGraph(version, options)`, `synthesizeChild(binding, source, options)` and
  `cancel()` (`programs.handoff(run?)`, `programs.releaseHandoff(run)`, `programs.replace(run, version, options)`,
  `programs.migrateGraph(run, version, options)` and `programs.synthesizeChild(run, binding, source, options)` or `programs.synthesizeChild(run, options)` take a run id or
  `{ id }` instead, and `programs.handoff()` with no run waits for the next checkpoint handoff of any run on the object).
  `result` resolves when the run ends or waits for an answer; a run that fails or is cancelled resolves with that
  status, and it rejects only when the call itself could not be made (an unknown version, a budget outside what the
  version admits, a run id that was used before). `resume` answers the interrupt the run waits at and returns a `ProgramRun` with
  the same id. `fork` continues the same pause as a new run (`options.runId` names it) with its own answer, takes half
  of what the paused run has left of its budget (or what `options.budget` asks for, which must leave the paused run some wall time and Core steps), and leaves
  the paused run resumable; the finished node is not run again. The forks of one pause may be made together: they take their turns, and each reads what the paused run has left after the forks
  before it, so each takes half of what the forks before it left. A fork that would take all the wall time or Core steps the paused run has left is refused
  (`P_FORK_SOURCE_BUDGET`), and so is one that asks for more than it has left (`P_FORK_BUDGET`); neither is a conflict. A fork made again under the same `runId` is that fork, with the amounts it was first given. A fork that is cancelled after it was sent, while it waits for its turn, still takes its turn and its share and then ends cancelled. `cancel()` resolves with whether this call cancelled
  the run. A resume or a fork that has not been sent yet, because it waits for the run it continues, is withdrawn by
  `cancel()` or an aborted `signal` instead, and its `result` rejects with a `cancelled` error; a `signal` that is
  aborted already means the call is not made.
- **`handoff`, `releaseHandoff`, `replace`, `migrateGraph` and `synthesizeChild`.** `handoff()` waits until the run
  reaches an `ng.checkpoint(value)` safe point, holds it there, and resolves with `{ runId, journalHead, coordinateId,
  entryId, value }` (`releaseHandoff()` releases the hold so the run continues without replacing it, resolving with a
  boolean). `replace(version, { input, budget, runId, journalHead, coordinateId, entryId, traceId, onEvent, signal })`
  replaces a run held at a checkpoint handoff with another admitted `version` (starting from the source lineage's
  remaining budget, with `options.budget` overriding specified fields and `journalHead`, `coordinateId` and `entryId`
  overriding the handoff reference fields; when `input` is an object or omitted, starting from `{}` without copying the
  source run's initial `input`, `input.handoff` defaults to the checkpoint `value` and `input.previous_run_id` defaults
  to the source `runId`), keeps the source run's children so the replacement generation can attach to them, and returns
  the replacement `ProgramRun`. `replace` refuses with the C++
  codes (`P_REPLACEMENT_AUTHORITY`, `P_REPLACEMENT_BUDGET`, `P_REPLACEMENT_TARGET`, `P_REPLACEMENT_INPUT`,
  `P_REPLACEMENT_HANDOFF`), rejecting with a `NeoGraphError` carrying that `code` and `witness`, and leaves the source
  run and its hold as they were. `migrateGraph(version, { runId, semanticAdapter?, onEvent, signal })` migrates a live
  root Core graph run on the in-memory transition store and returns the migrated `ProgramRun`; it attaches a semantic
  migration adapter only when `semanticAdapter: true`, and without one, the target must be the run's own admitted
  version or a compatible one (`P_GRAPH_MIGRATION_*` on refusal; a JavaScript transition store answers `"conflict"`, as
  noted below). `synthesizeChild(binding, source, { budget, capabilities?, effects?, grant, semanticDecision })` (or a
  single options object with `binding` and `source`) prepares a child version while the parent run is held at a
  checkpoint handoff and resolves with `{ id, binding, state, versionId, bundleId, proposalId, grantId }`. It needs the
  child's `budget` and the host's `grant` (`{ templateIdentity, validatorIdentity, contractIdentity,
  allowedCapabilities?, allowedEffects?, childBudget }`) and `semanticDecision` (`{ validatorIdentity, contractIdentity,
  accepted, evidence? }`); without them it is refused; the module makes neither from the child's request; the gateway
  accepts the decision only when it names the grant's validator and contract (`P_CHILD_SYNTHESIS_AUTHORITY` when the
  requested authority exceeds the parent run's or grant's envelope). While `synthesizeChild` uses a run's held handoff,
  `releaseHandoff`, `replace` and `cancel` of that run are refused until it returns, and a second `synthesizeChild` on
  the same hold is refused. `modelTokens` must cover a model call's input and
  output ceilings, because the run reserves that bound before each call.

  ```js
  // The host computes or stores the grant's identities ("sha256:" + 64 lowercase hex).
  const TEMPLATE_ID = await hostIdentity('child template');
  const VALIDATOR_ID = await hostIdentity('semantic validator');
  const CONTRACT_ID = await hostIdentity('semantic contract');
  const hold = await parentRun.handoff();
  const child = await parentRun.synthesizeChild('worker', CHILD_SOURCE, {
    budget: {
      wallTimeMs: 20000, modelTokens: 5000, monetaryMicrounits: 1000,
      maxConcurrency: 1, maxProgramOperations: 8, maxCoreSteps: 20,
      maxDynamicCompiles: 0, maxChildDepth: 0, maxTotalChildren: 0,
    },
    grant: {
      templateIdentity: TEMPLATE_ID,
      validatorIdentity: VALIDATOR_ID,
      contractIdentity: CONTRACT_ID,
      childBudget: {
        wallTimeMs: 20000, modelTokens: 5000, monetaryMicrounits: 1000,
        maxConcurrency: 1, maxProgramOperations: 8, maxCoreSteps: 20,
        maxDynamicCompiles: 0, maxChildDepth: 0, maxTotalChildren: 0,
      },
    },
    semanticDecision: {
      validatorIdentity: VALIDATOR_ID,
      contractIdentity: CONTRACT_ID,
      accepted: true,
    },
  });
  await parentRun.releaseHandoff();
  ```
- **`programs.result(runId)`** reads what the stores hold of a run: its latest result (the pause of a run that waits is
  one), `{ runId, status: 'running' }` while it has none, or `null` when they know no such run. **`programs.release()`** cancels the runs that are executing and frees the object, and calls already made finish
  first. A run that waits at an interrupt stays as it is in the stores.
- **A result** is `{ runId, versionId, status, output, trace, usage, remainingBudget, checkpointId?, interrupt?,
  failure? }`. `status` is `completed`, `interrupted`, `cancelled`, `budget_exhausted`, `timed_out`, `failed`,
  `ambiguous_effect` or `checkpoint_incompatible`. `output` is what `main()` returned (the graph's state, under
  `output.channels`, while a run waits). `trace` lists the nodes that ran since the run started or resumed. An
  `interrupt` is `{ node, value, kind, pendingId }`, and a `failure` is `{ code, message, node, attempts, witness }`.
- **Events** are the run's journal entries, `{ sequence, kind, run_id, operation_id, payload, ... }`, in the order the
  runtime wrote them. A model reply arrives as events with `payload.type === 'llm_token'`. An error thrown by `onEvent`
  cancels that run and rejects its `result` with the error; `signal` cancels the call when it aborts while the call is
  pending, and has no effect once the call has settled (use `cancel()` for a run that paused). A resumed run that is
  cancelled, or whose `onEvent` throws, before its Core call has started ends `cancelled`, and the stores keep it as
  `cancelled`. The resume reserved wall time, model tokens, money and Core steps for the call, and a run that ends before
  the call has used any of it gives none of it back: those four read 0 in its `remainingBudget`. A run that ends once
  the call has started gets back what the call did not use.
- **Budgets.** A budget is `{ wallTimeMs, modelTokens, monetaryMicrounits, maxConcurrency, maxProgramOperations,
  maxCoreSteps, ... }`; a member that is left out keeps the default: 60 s of wall time, 10,000 model tokens, 4 concurrent
  branches, 32 Program operations and 100 Core steps, which are also the largest values a JavaScript source can declare.
  An amount is a whole number from 0 to 2^53 and a name must be a member of the budget, otherwise the call is refused.
  A call to the model is sent only when its worst case fits what is left: the model's input limit plus its output limit,
  read from the catalogue (or `maxOutputTokens` for the output). A model that no catalogue gives limits for is not called,
  and the run ends `failed` with `NG.ConfigError`. `catalogue` states them. Give `modelTokens` a positive amount: a run
  given 0 is not bounded before its calls, because the runtime charges them afterwards.
- **Errors.** Besides `compile` and `admission`, a refusal of the runtime is a `NeoGraphError` of type `program` with a
  `code` (`P_START_BUDGET` for a budget outside what the version admits, for example) and its `witness`.

Everything runs on the host's event loop: the runtime awaits its stores, the timers and the model calls there, and none of
those blocks it. A control source is the exception: `define()` and each step of `main()` run in QuickJS on the same
thread, and a source that never ends holds the loop until its time limit stops it: 5 seconds for `define()`, which runs
when the Program is admitted (`javascript.defineTimeMs` chooses another time, from 1 to 3,600,000 milliseconds), and the
run's wall-time budget (60 seconds by default, `budget.wallTimeMs`) for the steps of `main()`. The stack limit of QuickJS
is on in this build, so a source that recurses without end, or nests deeper than the parser can follow, fails with the
diagnostic `P_JS_RESOURCE_LIMIT` instead of overflowing the host's stack: `admit` rejects with a `compile` error, and a
`main()` that does it ends its run `failed`. The limit counts the module's own stack in linear memory, for `define()` and
`main()` alike, and the `max_stack_bytes` in the witness of the diagnostic reports the size in force. That is 48 KiB
unless the developer chooses another size with `javascript.stackBytes`. How deep a control source can go grows with it,
at about four nested calls per KiB:

| `stackBytes` | nested calls | through the comparator of `sort` | through the callback of `map` | parentheses, array or object literals or calls in a source |
| --- | --- | --- | --- | --- |
| 48 KiB (49152, the default) | 190 | 32 | 57 | 131 |
| 96 KiB | 382 | 64 | 115 | 265 |
| 128 KiB | 510 | 85 | 154 | 354 |

A native build, with 512 KiB of the C stack, allows about four times as many nested calls (about 700 with GCC on x86-64;
it depends on the compiler and its flags) and nested literals in a source (about 500) as the module does at its default.
The limit does not see a chain of Proxy objects without traps that `Object.getPrototypeOf` or `Object.isExtensible`
follows: a chain of about 5,000 of them still overflows the host's stack. Functions nested deeper than the parser can
follow are refused too, but not always with this diagnostic: function expressions (about 40 deep at the default) end
with `P_JS_RESOURCE_LIMIT`, while function declarations (about 180 deep) and, at the default size, arrow functions
(about 85 deep) end with `P_JS_EVALUATION` and the message `SyntaxError: missing formal parameter`, because the engine
reports the overflow of the parser in that position as a syntax error. `admit` rejects with a `compile` error in every
case.

The size is the developer's choice, and the module enforces one bound on it: `stackBytes` is a whole number from 16384 up
to what the module's own stack holds, 983,040 in the module that this directory builds (its stack of 1 MiB less 64 KiB,
which are the engine's own reserve and the frames already on the stack when it starts). A larger value, a value that is not
a whole number, and a member that does not exist are refused when `programs()` is called, with an error that states the
bounds. What the host's own call stack carries is not something the module can read, and it is what runs out first: the
host's stack is used up faster than the module's, about 1.5 times as fast by plain calls and about 6 to 7 times as fast
by `JSON.stringify` of deeply nested data, which is the worst shape that was tried. Measured on Node 22 and 24, whose
host stack is about 984 KiB, with the optimizing and with the baseline compiler (`node --liftoff-only`), every one of the
17 shapes that `wasm/test/program_test.mjs` refuses (recursion through calls, getters, Proxy traps and the callbacks of
`sort`, `map` and `JSON.parse`, constructors and generators, a constructor reached through 5,000 bound functions,
`JSON.stringify` and `Array.prototype.flat` of data nested 100,000 deep, `JSON.parse` of text nested 100,000 deep, and
source nested 2,000 deep) ends with `P_JS_RESOURCE_LIMIT` at each of the sizes 48, 64, 96, 128 and 144 KiB. At 160 KiB
`JSON.stringify` of nested data overflows the host's stack, at 192 KiB `Array.prototype.flat` of it does too, at 256 KiB
seven of the shapes do, and at 512 KiB fourteen. With the host stack halved (`node --stack-size=492`) the largest size
at which every shape is caught is 64 KiB, and at 96 KiB the same two shapes overflow. An eighth of the host's stack
(about 123 KiB of Node's) is therefore caught for all of these shapes, and a quarter is not. The default of 48 KiB is a
third of the first size that overflowed. Raise `stackBytes` only as far as your sources need, keep it within an eighth of the call stack of the host
that runs them (a Worker, a browser tab and a Node process have different stacks), and try a source that recurses
without end against the setting on that host before relying on it: a size that the host's stack cannot carry ends in
the trap described below. The witness of the diagnostic reports the size in force in `max_stack_bytes`.

A source that runs both natively and in the module should stay within the module's limit. A native build counts the C
stack of the thread that runs the source, 512 KiB by default (`JavaScriptCompileLimits::max_stack_bytes` for `define()`
and `RuntimeConfig::javascript_stack_bytes` for `main()`), the module counts its own stack, 48 KiB by default, and a call
uses a different number of bytes in each. The same source can therefore be refused in one and run in the other: write a
source meant for both for the smaller figures of the table above (190 nested calls, 131 nested literals), or set both
limits to depths that were measured.

A host that admits control sources it does not trust should do so in a Worker that it can discard. The limit above stops
the shapes of recursion that were tried, but a trap in WebAssembly cannot be recovered in the same module instance: if the
host's stack overflows, the call never settles, the instance is left unusable, and in Node the uncaught `RangeError` ends
the process unless the host handles it. Run in the module only control sources that the host trusts or has vetted.

The checkpoints of a paused graph are kept in the module's memory, so a run can be resumed or forked only while the
`Programs` object that started it is alive.

## How it works

### The two event loops

NeoGraph's graph engine is a set of coroutines on an Asio `io_context`. A WebAssembly host owns the thread and its
event loop, and nothing in the module may block it: no Asyncify, no JSPI, no loop of the module's own. SchemaProvider
has the same problem and solves it with a host seam: an `Executor` that the host runs and a `HostTransport` that sends
HTTP, which `sp_js_host` implements over `queueMicrotask`, `setTimeout` and `fetch`. The module joins the engine to that
loop in four steps.

- **Who pumps the `io_context`.** `async::HostLoop` owns it and never calls `run()`. A turn is a task posted to a
  SchemaProvider executor of the loop's own (a Client shuts its executor down when it ends, so they cannot share one);
  it calls `poll_one()` until no handler is ready, at most 256 a turn, and then yields to the host and posts another turn
  if work is left. A turn is scheduled when work is executed on the loop's executor: `HostLoop::executor()` wraps the
  `io_context` executor so that executing a handler on it posts a turn, one at a time and never from inside a turn.
  Starting a run, cancelling it, and an SDK completion all do that. An idle loop schedules nothing.
- **How an SDK completion reaches the graph's coroutines.** `llm_call` starts the SDK operation and suspends on a
  wake channel. The SDK delivers a fetch chunk or the outcome in a task of its own executor, a JavaScript microtask. The
  callback keeps the event and signals the channel, which posts the coroutine's continuation to the loop's executor. That
  schedules a turn, and in it the coroutine resumes, hands the events to the run's observers in order and, on the
  outcome, finishes the node. Nothing runs inline in an SDK callback, and no thread or lock is involved.
- **How `run_async` is driven without blocking.** `HostLoop::spawn(awaitable, done)` is `co_spawn` plus a turn: it
  returns at once, and `done(exception, result)` is called from a later turn. The module's `ng_run` returns, the host
  event loop goes on, and the JavaScript `Promise` is resolved from `ng_js_done` when the run ends.
- **Asio timers.** A retry backoff or a resource-wait poll is an Asio timer, and the host's loop cannot see Asio's timer
  queue. The engine waits on `async::HostTimer` (`src/core/host_timer.h`), which arms the Asio timer and tells the loop
  its deadline (`async::note_deadline`) in one call, and the loop sets one host timer for the earliest. That timer can
  run before Asio dispatches the timer that expires then (Asio's Windows reactor learns from a thread of its own that a
  timer is due, and a host's clock can run ahead of Asio's), so the loop keeps a deadline until Asio has dispatched the
  timers up to it and looks again a millisecond later while it has not. A raw Asio timer in `src/` or `include/` would
  never fire on a host, so the `host_timer_guard` test refuses it; `scripts/host_timer_allowlist.txt` lists the code
  that never runs on a host loop. Code of your own that waits on an Asio timer under a `HostLoop` calls
  `async::note_deadline` itself.

The synchronous entry points keep their symbols and refuse. `GraphEngine::run`, `resume`, `Provider::dispatch` and
`invoke`, and the synchronous endpoint calls drive a private `io_context` until the call is done, which would wait for
a host loop that only the waiting thread can run. When a provider on a host-pumped executor (`Provider::host_pumped()`,
true for `SchemaProvider` built with an executor that says so) is reached from such a call, the call fails with the SDK's
`runtime.blocking.host_pumped` reason (`ErrorKind::Misuse`, retry safety `NotSent`) before anything is sent. The message
names the `_async` form to use. A run that never reaches a host-pumped provider still works through `run()`, as in the
smoke program.

### TLS and OpenSSL

TLS belongs to the host: `fetch` does it. Where the SDK has no libcurl transport (WebAssembly, or a build with
`NEOGRAPH_THREADS=OFF`), `neograph::llm` is built host-only: CMake does not look for OpenSSL, `neograph::llm` links
neither `neograph::async` nor `SchemaProvider::transport`, and `NEOGRAPH_BUILD_ASYNC` (the `asio::ssl` HTTP client
that LLM, MCP client and A2A use on native hosts) stays refused. The libcurl constructor of `SchemaProvider` then
throws the SDK's `ConfigError` at `/transport`. Native builds are unchanged: the same libraries, the same link lines, the
same constructors.

### Building a provider over the host seam

`SchemaProvider` has a second constructor, `SchemaProvider(descriptor, options, executor, transport, defaults)`, and a
matching `create`. It forwards the SDK's public `Client(descriptor, options, executor, transport)`. It needs no libcurl
and no OpenSSL, and takes the `host_pumped()` declaration of the executor. The module makes one executor and one fetch
transport for each compiled graph:

```cpp
auto executor  = sp::runtime::js::make_executor();
auto transport = sp::runtime::js::make_fetch_transport(executor);
auto provider  = neograph::llm::SchemaProvider::create(descriptor, sp::runtime::Options::with_api_key(key),
                                                       executor, transport);
```

### The JavaScript surface

The module exports a C API and `neograph.mjs` wraps it. A C API with JSON strings needs no binding library; Embind
would add one and a registration of every type, for a surface of a few functions: `ng_compile`, `ng_run`, `ng_cancel`,
`ng_release` and `ng_error` for graphs, and `ng_programs_create`, `ng_programs_admit`, `ng_programs_start`,
`ng_programs_resume`, `ng_programs_fork`, `ng_programs_handoff`, `ng_programs_release_handoff`, `ng_programs_replace`,
`ng_programs_migrate_graph`, `ng_programs_synthesize_child`, `ng_programs_cancel`, `ng_programs_result` and
`ng_programs_release` for Programs. Results come back through callbacks, `Module.ngOnEvent` and `Module.ngOnDone` for a graph and
`Module.ngProgramEvent` and `Module.ngProgramDone` for a Program, called from a later turn of the host's event loop. The key is part of the provider description that `compile` is given and is
held by the SDK client of that graph; it is not in the binary, not in any result, and not in an error message.

## Programs on the host loop

`program::ProgramRuntime` takes the loop's executor, and runs everything on it: a Program's attempts, its deadline timers
and the waiters of `ProgramHandle::wait_async`. It starts no thread. The stores it reads and writes are awaitable, and the
in-memory ones are the stores this build provides.

```cpp
async::HostLoop loop(host_executor);                  // the sp::runtime::Executor the host runs
program::RuntimeConfig config{catalog, checkpoints, /*state_store=*/{}, transitions, /*scheduler_threads=*/1};
config.executor = loop.executor();                    // required here: a build without threads has no pool to fall back to
program::ProgramRuntime runtime(std::move(config));

loop.spawn(runtime.run_async(owner, version, invocation),
           [](std::exception_ptr error, program::ProgramResult result) { /* from a later turn */ });
```

- **Required.** Without `RuntimeConfig::executor`, a build without threads throws `ProgramExecutorRequired`, a
  `graph::ConfigurationFailure`, from the constructor. On a native build the runtime's own pools are used when it is
  unset, as they always were.
- **Awaitable forms.** Every operation has one: `start_async`, `reconnect_async`, `resume_handle_async`, `fork_async`,
  `replace_async`, `migrate_graph_async` and the rest. They run on the loop and complete on the awaiting coroutine's
  executor. `run_async` and `resume_async` wait for the result.
- **The synchronous calls refuse.** `start`, `resume`, `fork`, `run` and the other synchronous calls, and
  `ProgramHandle::wait` while the result is not in, would block the thread that has to run the Program. They throw
  `program::ProgramBlockingCallRefused` (a `graph::ConfigurationFailure`) before they do anything. `operation()` names the
  call and `alternative()` the awaitable form. A `hook_runtime` is refused by the constructor for the same reason.
- **Timers.** The runtime waits on `async::HostTimer` only, which tells the loop each deadline and takes it back when the
  timer is cancelled, re-armed or destroyed, so a finished run leaves no host timer armed.
- **Node bodies.** With an executor, a node attempt that needs its own worker runs on a strand of it instead of the
  32-thread process-wide pool, so a node body must not block its thread.
- **Locks.** A critical section that reaches a store holds an `OperationLock`, which a coroutine waits for without
  blocking; `std::mutex` guards memory only.
- **Awaitable stores.** The five Program store interfaces return `asio::awaitable`, and every operation of the runtime
  awaits them on the loop: the entry points, the terminal publication, the child launch, the Core tool grant resolver
  (`RuntimeConfig::core_tool_grant_awaitable_resolver`), `ProgramCatalog`'s `_async` forms and
  `ModuleResolver::resolve_async`. A store whose operations complete at once, as the in-memory ones do, costs no turn,
  and one that completes in a later turn of the host's loop works. The synchronous forms run the same coroutines to
  completion on the calling thread and refuse, as above, instead of waiting for a loop that only the waiting thread could
  turn.
- **Destroying the runtime** cancels its runs and does not wait for them. The attempts keep the runtime's state alive until
  they finish, so keep running the loop until the handles are terminal.

## Program stores in JavaScript

A host that runs NeoGraph in WebAssembly supplies the stores of the Program runtime as a JavaScript object. The C++ side
adapts that object to the five awaitable store interfaces, so the runtime and the catalog use it like any other store.
The contract moves values and decisions; what a store does with them stays in C++, so a host writes no state machine.

```js
const neograph = await load();
const programs = neograph.programs({
  provider,
  stores: { transitions, programs: programStore, fragments, modules },    // each member is optional
});
```

A member that is left out is the module's own in-memory store, and a name that is not a member (a misspelt one) is
refused. A `transitions` store must have `loadJavascriptCommands`, `loadMigrationPlan`, `loadRunLineage` and `loadGeneration` as well
as the methods marked required below: a run reads its command history, a fork its migration proof, and a resume and a fork the
lineage of a run. The `Programs` object keeps the object registered until the
runtime has dropped every store it made from it, which is after the runs that were cancelled by `programs.release()` have
published their end, so releasing the object does not make those publications fail. The runtime of the module uses the
`transitions`, `programs`, `fragments` and `modules` stores. A Program of the module has no tools to grant, so `grants` is
for C++ code that links the adapters:

```js
import { installStoreBridge, registerStores, releaseStores, memoryStores } from './stores.mjs';

installStoreBridge(module);                       // once per module instance; load() does it for neograph.mjs
const handle = registerStores(module, {           // each member is optional
  transitions, programs, grants, fragments, modules,
});
// C++: neograph::wasm::JsStoreSet stores = neograph::wasm::adapt_js_stores(handle);
releaseStores(module, handle);
```

`wasm/js_stores.h` declares `adapt_js_stores(handle, lease)`, which returns a `JsStoreSet` of five `shared_ptr`s (a member is null
when the object has no store of that kind). Each call makes new adapters over the same object, so two sets are two writers
on the same data. `lease`, when given, is held by every adapter and released with the last of them; the Programs object of the
module ends the registration from its deleter. `stores.mjs` is the JavaScript half and is copied next to `neograph.mjs`. A
module that links `neograph_wasm_js_stores` exports the functions the bridge calls (`ng_store_resolve`, `ng_store_reject`,
`ng_store_alloc`, `ng_store_free`) and the ones of the reference store (`ng_memory_*`).

**Calls.** A store member is an object whose methods are named after the C++ operations, in camelCase, and take the
arguments of the C++ operation in its order. A method returns its value or a Promise of it.

- A value that is not a thenable completes the call at once, without suspending the awaiting coroutine and without a turn
  of the host's loop. A synchronous caller (`Blocking...Store`) can then wait for it.
- A Promise suspends the coroutine. When it settles, the bridge posts a task, and the coroutine goes on in a turn of the
  loop, on its own executor. `set_js_store_executor(executor)` names another executor for the completions; by default
  they go to the executor the awaiting coroutine runs on.
- The arguments are copied into the call before the coroutine can suspend.
- If the awaiting coroutine is destroyed while a Promise is pending (its loop is closed, its run is cancelled), its wait
  goes with it. The outcome that arrives afterwards finds no one and is dropped.
- Calls are not serialized: another call may be made while one is pending.

**Failures.** A thrown exception or a rejected Promise becomes a C++ exception thrown from the awaiting call, which the
runtime treats as a failed store (it fails closed). It is a `std::runtime_error` that carries the message of the JavaScript
error, or of the rejection value converted to a string. An error with a `kind` property names the C++ class instead:
`invalid_argument`, `out_of_range`, `domain_error`, `length_error`, `logic_error` or `runtime_error`; any other `kind` is
ignored. A store that refuses a call for a reason of its own, as the in-memory stores refuse an empty owner scope with
`std::invalid_argument`, says so this way. A value that does not parse, or has the wrong shape, is the same failure with a
message that names the method (`JavaScript store method 'transitions.load' returned a malformed value: ...`). The adapter
never returns a default value for a failed call.

**Values.**

| C++ | JavaScript |
|---|---|
| `std::string`, `std::string_view` | string |
| `std::uint64_t` (a sequence, a generation, a count) | number: a non-negative integer below 2^53 |
| `bool` | boolean |
| `std::optional<T>` | the value of `T`, or `null` (`undefined` reads as `null`) |
| `std::vector<T>` | array |
| `ProgramTransitionPublishResult` | `"published"`, `"already_present"`, `"conflict"` |
| `ProgramActivationResult` | `"activated"`, `"already_present"`, `"conflict"` |
| `ProgramCoreToolGrantAdmission` | `"admitted"`, `"already_present"`, `"conflict"` |
| `TaskGraphPublishResult` | `"published"`, `"already_present"`, `"conflict"`, `"missing"` |
| `ModuleLifecycle` | `"active"`, `"deprecated"`, `"quarantined"`, `"revoked"` |
| `ProgramRunRecord`, `ProgramJournalRecord`, `ProgramEvent`, `ProgramEffectOutboxEntry`, `ProgramJavaScriptCommandJournalEntry`, `HookOutboxEntry`, `ProgramChildSynthesisRecord`, `MigrationPlan`, `ProgramRunLineage`, `ProgramRunGeneration`, `ProgramTransitionPublication`, `GraphMigrationCapsule`, `ProgramExecutionLease`, `ProgramRuntimeStateTransferReceipt`, `ContextEpoch`, `ContextArtifact`, `ProgramBundle`, `ProgramVersion`, `ProgramActivation`, `TaskGraphFragmentRecord`, `ProgramModule` | string: the canonical text, `serialize_canonical()`, which the type's `parse(bytes)` reads back. A host stores and returns it as it is. |
| `ProgramRunPublicationHead` | `{ run_record, journal_record }` |
| `ProgramCommandPublicationHead` | `{ run_record, journal_record, latest_command }`; `latest_command` is a command journal entry or `null` |
| `ProgramContextPublication` | `{ epoch, artifacts: [...], assembly_receipt }`, each a canonical text |
| `ProgramEffectiveRuntimeState` | `{ context_publications: [ProgramContextPublication...], hook_outbox_entries: [...], inherited_hook_outbox_entries: [...], transfer_receipt }`; `transfer_receipt` is a canonical text or `null` |
| `ProgramActivationBinding` | `{ activation, version }`, each a canonical text |
| `ProgramRetentionReport` | `{ versions_removed, bundles_removed, references: [{ version_id, reason }...] }` |
| `ProgramCoreToolGrantRecord` | `{ owner_scope, program_version_id, run_id, operation_id, attempt, binding_fingerprint, grant_id }` |
| `ProgramCoreToolGrantContext` | the same without `grant_id` |

An object has exactly the members listed; a missing or extra member is a malformed value.

**Methods.** `ownerScope`, `runId` and the other names are strings. A method marked required must be present: `adapt_js_stores`
throws `std::invalid_argument`, naming it, when it is not. An optional method that is absent keeps the default of the C++
interface, given in the last column.

| Member | Method | Result | If absent |
|---|---|---|---|
| `transitions` | `load(ownerScope, runId)` | run record or `null` | required |
| | `latest(ownerScope, runId)` | journal record or `null` | required |
| | `loadRunPublicationHead(ownerScope, runId)` | publication head or `null` | read from `load` and `latest`, which are read again to see that the head did not move |
| | `loadEvents(ownerScope, runId, afterSequence)` | list of events | required |
| | `loadEffects(ownerScope, runId, afterSequence)` | list of effect outbox entries | required |
| | `loadJavascriptCommands(ownerScope, runId, afterSequence)` | list of command journal entries | fails |
| | `loadCommandPublicationHead(ownerScope, runId)` | command publication head or `null` | read from `load`, `latest` and `loadJavascriptCommands` (so it fails with them) |
| | `loadContextPublications(ownerScope, runId, afterSequence)` | list of context publications | fails |
| | `loadHookOutboxEntries(ownerScope, runId)` | list of hook outbox entries | fails |
| | `loadEffectiveRuntimeState(ownerScope, runId)` | effective runtime state | read from the context, hook, run, lineage and generation reads |
| | `loadChildSyntheses(ownerScope, runId)` | list of child synthesis records | fails |
| | `loadMigrationPlan(ownerScope, runId)` | migration plan or `null` | `null` |
| | `loadLineage(ownerScope, lineageId)` | lineage head or `null` | `null` |
| | `loadRunLineage(ownerScope, runId)` | lineage head or `null` | `null` |
| | `loadLineageHead(ownerScope, lineageId, headId)` | lineage head or `null` | `null` |
| | `loadGeneration(ownerScope, lineageId, generation)` | generation or `null` | `null` |
| | `loadGenerationInitialPublication(ownerScope, lineageId, generation)` | publication or `null` | fails |
| | `loadGraphMigrationCapsule(ownerScope, runId, lineageHeadId)` | capsule or `null` | `null` |
| | `loadExecutionLease(ownerScope, runId)` | lease or `null` | `null` |
| | `comparePublish(ownerScope, expectedJournalHead, publication)` | publish result | required |
| | `comparePublishExecution(ownerScope, expectedJournalHead, publication, expectedLease, nextLease)` | publish result; the leases are canonical texts or `null` | `"conflict"` |
| `programs` | `publishAdmitted(bundle, version)` | nothing | required |
| | `getBundle(id)`, `getBundle(ownerScope, id)` | bundle or `null` | required |
| | `getVersion(id)`, `getVersion(ownerScope, id)` | version or `null` | required |
| | `getActivation(ownerScope)` | activation or `null` | fails |
| | `getActiveBinding(ownerScope)` | activation binding or `null` | read from `getActivation` and `getVersion` |
| | `compareActivate(ownerScope, expectedGeneration, versionId, policySnapshotHash)` | activation result | fails |
| | `listVersions(ownerScope)` | list of versions | fails |
| | `collectGarbage(ownerScope, pinnedVersionIds)` | retention report | fails |
| `grants` | `admit(record)` | admission | required |
| | `load(context)` | grant record or `null` | required |
| | `revoke(context)` | boolean: whether a grant was revoked | required |
| `fragments` | `publish(record)` | publish result | required |
| | `load(fragmentId)` | fragment record or `null` | required |
| | `findPublishedLineage(ownerScope, parentRunId, expansionOperationId)` | fragment record or `null` | `null` |
| | `findPublished(ownerScope, parentRunId, expansionOperationId, proposalHash)` | fragment record or `null` | `null` |
| | `compareUpdate(fragmentId, expectedRevision, record)` | publish result | required |
| `modules` | `publish(module)` | nothing | required |
| | `get(contentIdentity)`, `get(ownerScope, contentIdentity)` | module or `null` | required |
| | `setLifecycle(contentIdentity, lifecycle)` | nothing | required |

- **Two forms of one method.** `getBundle`, `getVersion` and `get` each have an unqualified form and an owner-qualified one in
  C++, and the catalog uses both. One JavaScript method serves both, and the number of arguments tells them apart: one is
  the identity, two are the owner scope and then the identity. A method written for one argument is wrong for the other.
- **A publication is checked before it crosses.** `comparePublish` and `comparePublishExecution` begin as every backend of
  the transition store begins: the checks that need no stored state (the owner of the publication, the shape of the
  expected head, the effect outbox, the canonical text of the publication). A publication that fails them is `"conflict"`
  and never reaches the object. What the stored state says about it is the store's decision.
- **Not supported.** `compare_publish_graph_safe_point` carries evidence that is never serialized, so a JavaScript store
  does not take part in live graph migration: it answers `"conflict"`, as the base class does.

**The reference store.** `neograph.memoryStores()` (`memoryStores(module)` of `stores.mjs`) returns `{ transitions, programs, grants, fragments, modules }` that
implement this contract by calling the in-memory C++ stores of the module (`wasm/memory_stores.cpp`): the arguments are
decoded with the same table the adapters encode with, the store's own operation runs, and the result is encoded with the
table the adapters decode with. Its methods return values, not Promises, and a refusal of the in-memory store reaches the
adapter as the same C++ exception class, through `kind`. The grant store has no in-memory form in the library, so the
reference keeps a small one with the decisions of the SQLite grant store. `stores.release()` drops the set; do that when
the runs on it have ended (their `result` has settled), because a call on a dropped set fails.

A host that wants a durable store wraps the reference store instead of writing the rules again. Every method delegates to
it; the arguments of the calls that change something are written to the host's own storage; and on start the wrapper
replays them in order into a fresh reference store. The reference store decides each call from its arguments and the state
before it, so the replay reaches the state the log describes. The wrapper below appends to the log after the reference
store has decided and before it returns the result; a host that cannot lose a call to a crash in between writes the log
entry first and applies the call after it. `test/program_stores_test.mjs` does this for a whole Program run.

```js
const CHANGES = new Set(['transitions.comparePublish', 'transitions.comparePublishExecution', 'programs.publishAdmitted',
  'programs.compareActivate', 'programs.collectGarbage', 'grants.admit', 'grants.revoke', 'fragments.publish',
  'fragments.compareUpdate', 'modules.publish', 'modules.setLifecycle']);

function durable(reference, log) {                          // log.append(entry) is the host's storage
  const wrapped = {};
  for (const [member, target] of Object.entries(reference)) {
    wrapped[member] = {};
    for (const [method, call] of Object.entries(target)) {
      wrapped[member][method] = async (...args) => {
        const result = call(...args);                       // the reference store decides
        if (CHANGES.has(`${member}.${method}`)) await log.append({ member, method, args });
        return result;
      };
    }
  }
  return wrapped;
}
// On start: for (const { member, method, args } of log.entries()) fresh[member][method](...args);
```

## Examples

- `examples/node/chat.mjs`: one call from Node,
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`.
- `examples/node/program.mjs`: a Program with two model calls and a pause between them, from Node,
  `API_KEY=... node wasm/examples/node/program.mjs <base url> <path> <model> "<prompt>" [--answer <text>]`. It prints the
  first reply token by token, waits for an answer (the flag, or a line on stdin; an empty answer cancels the run) and
  continues the conversation with it.
- `examples/workers/worker.mjs` and `wrangler.toml`: a request handler that answers `POST {"message": "..."}` with the
  model's reply, as JSON or as server-sent events. The template holds no secret: the key is a Worker secret
  (`wrangler secret put API_KEY`), and `neograph_module.wasm` is imported as a `WebAssembly.Module`.

## Tests

CI builds the module and runs, under Node 22 and 24, with no network and no key:

- `test/llm_host_test.mjs`: a local HTTP server answers like a Chat Completions endpoint, buffered and as server-sent
  events with a character cut across two chunks. The test runs the one-node graph through the module and checks the
  final state, the order of the streamed events, the request that went out (the key and model given at run time), a
  provider refusal, cancellation by `AbortSignal`, a timeout, concurrent runs, and that the host's timers keep running
  while a call is in flight.
- `test/program_test.mjs`: the Program API against the same server. A Program is compiled and admitted (and a refused
  source says why), a run streams its events, pauses at the interrupt and is resumed, a fork answers the same pause another
  way without asking the first call again, the forks of one pause made in the same turn of the event loop take half of what the
  earlier ones left and the rest are refused for the budget, a fork made again keeps its share, a paused run is cancelled and read back from the stores, runs in flight
  together keep their own answers, an `AbortSignal` closes the connection of a model call, and an error thrown by
  `onEvent` cancels its run. A fork that is cancelled before it is sent is withdrawn, an aborted signal starts nothing, and a
  control source that never ends is stopped by its time limit, while one that recurses too deep (seventeen shapes in
  `define()` and in the source, and `main()`) fails with a diagnostic and leaves the module able to admit and run a
  Program. The failures that must stay failures are checked too: a model with no limits in the catalogue
  is not called, a budget that cannot cover a call exhausts the run before it is sent, a budget outside what the version
  admits is refused, and the key is in no result or event. The Node example is run as a child process.
- `test/program_stores_test.mjs`: the module's `stores` option. The reference store serves a whole lifecycle as it is and
  wrapped to answer in later tasks (the host's timers keep running between the calls); a rejected call, a thrown error and a
  value that is not one fail closed with the store's message and the name of the method, and the model is not called; an
  object without a required method is refused; a wrapper that writes what changes to a log lets a second object, over a
  store replayed from the log, read back a finished run, see that a paused one is still paused and start the first object's
  Program; a fork cancelled while the stores are still answering ends cancelled, and forks made together take their shares in turn; a misspelt member and a missing lineage
  reader are refused; releasing an object cancels a run that is executing and leaves one that waits as it is.
- `test/program_replacement_test.mjs`: checkpoint handoff (`handoff`, `releaseHandoff`), `replace` of a run held at a
  checkpoint with an admitted second version (remaining budget carried over, children kept), the five replacement refusals
  (`P_REPLACEMENT_AUTHORITY`, `P_REPLACEMENT_BUDGET`, `P_REPLACEMENT_TARGET`, `P_REPLACEMENT_INPUT`, `P_REPLACEMENT_HANDOFF`)
  with their `witness` and the source run and its hold left as they were, `migrateGraph` to the run's own admitted version
  and its guard refusals (same run id, an incompatible target), `synthesizeChild` with the host's grant and decision, its
  refusal without a grant, and `P_CHILD_SYNTHESIS_AUTHORITY`, a `root -> child -> grandchild` hierarchy whose child is
  replaced while it runs, and checkpoint handoff, `replace`, `synthesizeChild` and that hierarchy replacement over an
  asynchronous JavaScript `stores` object.
- `test/workers_test.mjs`: the Workers example's exported `fetch` handler is called with a `Request` against the same
  server, buffered and streamed. It shows the handler's logic and its use of the platform APIs; it is not a run on
  workerd.
- `checks.cpp` (below) runs a synchronous run against a host-pumped provider and checks that it is refused and that
  nothing was sent.
- `checks.cpp` also builds a `ProgramRuntime` on a `HostLoop`: a runtime without an executor and a synchronous `start` are
  refused, and a one-node Program with in-memory stores runs to its result in the host's turns.
  It then reads the run and the version back through the awaitable stores in a host turn, and shows that a blocking view
  refuses to wait on the loop.
  It also runs the Program over a Program store and a transition store that complete a call only in a later turn of the
  loop, and reads the catalog through such a store.
- `tests/test_program_runtime_executor.cpp` runs Programs natively on a single-thread `io_context` and on a `HostLoop` over a
  manual executor, so a native build covers the same paths.
- `tests/test_program_host_pumped_stores.cpp` runs a whole Program lifecycle natively on a `HostLoop` over stores that
  complete a call only in a later turn of the loop: a start, a model node, an interrupt and its resume, a fork, a child, a
  Core tool grant, a completion and a cancellation, with nothing blocked or refused. It also makes several forks of one pause in
  one turn, and shows that their budgets add up to at most what the paused run had.
- `tests/test_program_store_awaitable.cpp` runs the store contract natively: an inline store completes without suspending,
  an offloading one resumes the awaiting coroutine on its own executor, and a blocking view waits for a store that completes
  later or refuses.
- `tests/test_host_loop.cpp` runs the loop and the host-seam provider natively against a manual executor, so a native
  build covers the same paths.
- `test/store_conformance_test.mjs` runs the store conformance suites of `tests/` (`ProgramTransitionStoreConformance`,
  `ProgramStoreConformance` and the graph migration capsule row) against the JavaScript stores, under Node. The module is
  built with `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON` (it fetches googletest) from the target
  `neograph_wasm_store_conformance`: the suites with a JavaScript backend that hands every case stores through the adapters
  over the reference store, in both forms (`JavaScript` calls the operations that a synchronous caller uses, `JavaScriptAwaited`
  the awaitable ones). A case that races two writers on threads makes them take turns on the one thread. The script prints
  the counts per backend and the reason for every skipped case, and exits non-zero on a failure.
- `test/js_stores_test.mjs` drives the target `neograph_wasm_store_checks` with stores whose methods return Promises that
  settle in later tasks: a Promise resumes the awaiting coroutine in a turn of the loop and nothing waits while it is
  pending; a value that is not a Promise costs no turn; a rejection, a throw and a value that does not parse become C++
  exceptions that name the method; an object that mixes the two works; an outcome that arrives after the coroutine was
  destroyed or cancelled is dropped; a missing required method fails `adapt_js_stores`; a whole Program runs over Promise
  stores, and fails closed over stores that reject.

## Measured sizes

Release, `-O3 -flto`, Emscripten 6.0.13. The graph smoke contains the Core engine and the SDK runtime; the graphs-only
module adds the LLM component, the SDK's JavaScript host and the C API. The Program component is most of the rest: the
module with Programs is twice the size of the graphs-only one, and QuickJS is about 1 MB of that.

| Build | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| Graph smoke, single-thread | 5,615,386 B (5.4 MiB) | 1,642,238 B (1.6 MiB) | 83,223 B |
| Graph smoke, pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| Checks with Program, LLM and QuickJS, single-thread | 12,495,665 B (11.9 MiB) | 3,741,204 B (3.6 MiB) | 91,040 B |
| JavaScript module, graphs only (`NEOGRAPH_WASM_JS_PROGRAMS=OFF`), single-thread | 6,466,458 B (6.2 MiB) | 1,893,168 B (1.8 MiB) | 87,762 B |
| JavaScript module with Programs, without QuickJS, single-thread | 12,148,715 B (11.6 MiB) | 3,551,238 B (3.4 MiB) | 95,154 B |
| JavaScript module with Programs and QuickJS, single-thread | 13,209,571 B (12.6 MiB) | 3,931,479 B (3.7 MiB) | 96,889 B |

The module with Programs and QuickJS built with `-DCMAKE_CXX_FLAGS_RELEASE="-Oz -DNDEBUG"` (and the same for C) is
10,378,540 B and 3,122,630 B gzipped; its speed was not measured.

CI fails the single-thread smoke above 6,000,000 bytes of `.wasm` or 1,750,000 bytes gzipped (the measured size plus
about 9%), and the module, which it builds with Programs and QuickJS, above 14,400,000 bytes of `.wasm` or 4,300,000 bytes
gzipped, and reports the sizes in the job summary. A Worker's script has a size
limit that depends on its plan; compare the module's compressed size with the limit of yours. The 712 KB figure in
earlier versions of this page was measured before the typed SDK and no longer applies.

## What is unavailable on these hosts

Calls into these fail with a typed error where NeoGraph owns the call, and are not built otherwise.

- **Model calls other than through the host.** ASYNC, MCP client and A2A need sockets and TLS. Model calls go through
  the host's `fetch` with the module above; a build for another WebAssembly host supplies its own executor and
  transport to `SchemaProvider`.
- **Blocking entry points on a host-pumped provider.** `run()`, `Provider::dispatch` and the like refuse (above).
- **Program stores on a host database.** The store interfaces are awaitable. The SQLite and PostgreSQL stores need system
  libraries, so a host database needs a store of its own. A store whose operations complete at once (a host's synchronous
  storage) and a store that completes only through the host's event loop both serve the runtime on the host loop, through
  the `_async` forms ([#557](https://github.com/fox1245/NeoGraph/issues/557)). The synchronous Program calls refuse on a
  host loop; use the `_async` forms. The JavaScript module takes such stores as JavaScript objects (`stores`, above).
- **Native Program controls.** The C v1 plugin ABI is a 64-bit layout, so `NativeControlBinding::create` refuses on
  wasm32 with `NativeControlUnavailable`.
- **Synchronous checkpoint stores.** A store that is not asynchronous runs each operation on a pool of worker threads.
  A single-thread build has none, so the operation is refused with `SynchronousCheckpointStoreUnavailable` instead of
  waiting for a thread; implement `AsyncCheckpointStore`. The built-in in-memory store runs its operations, managed
  budgets included, on the calling thread, so a run with `model_token_budget` works on it.
- **A managed budget with a deadline.** The deadline is bound to a clock origin that a restart can recover, and a
  WebAssembly host has none. The run is refused with `ManagedBudgetDeadlineClockUnavailable`, before the store is
  asked.
- **The native archive.** There is no file custody backend; the SDK refuses with an `Unsupported` error.
- **Subprocess delegation** (Harness CLIs, stdio MCP servers), **listening sockets**, **gRPC**, **file-path
  uploads** and **Python**.

## Browser

`smoke.html` runs the generated `smoke.js` at `http://127.0.0.1:8765/smoke.html` when served by `serve_smoke.py`:

```bash
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
```

`serve_smoke.py` sends `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.
`smoke.html` checks for cross-origin isolation and `SharedArrayBuffer`, loads `/smoke.js`, and sets
`document.body.dataset.result = "pass"` and `document.body.dataset.exitCode = "0"` when the program prints
`doubled = 42` and `trace = d` and exits with `0` and no stderr output. Verified with the default single-thread build
in headless Chromium 140.0.7339.186 (`doubled = 42`, `trace = d`, exit code `0`); the pthread build runs in Node only
(its `main` blocks the main thread without `-sPROXY_TO_PTHREAD`), CI runs Node only, and the JavaScript module
(`neograph.mjs`) has not been run in a browser.

## Milestones

The plan after the toolchain review is in [#535](https://github.com/fox1245/NeoGraph/issues/535):

- **M0 and M1.** The wasm32 fixes, the single-thread build, and the Node smoke with a size gate in CI.
- **M2 (this page).** Model calls through the host's `fetch`: the host loop, the host-seam provider, the JavaScript
  module, and the Node and Workers examples.
- **M3.** Programs on a host store ([#557](https://github.com/fox1245/NeoGraph/issues/557),
  [#556](https://github.com/fox1245/NeoGraph/issues/556)). The runtime half is in: `ProgramRuntime` runs on
  `HostLoop::executor()` ("Programs on the host loop" above). So is the store interface: it is awaitable. So is the
  JavaScript surface ("Programs in the module" above).
- **M4.** Realtime sessions, WASI, a custody backend for native archives, and remote Harness.
