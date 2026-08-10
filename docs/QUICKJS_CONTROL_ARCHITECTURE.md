# NeoGraph QuickJS Control Architecture

Status: Base runtime implemented; final legacy drain/removal remains gated
Date: 2026-08-08
Source baseline: `61661e9ad1fc386b5142139c48c327ede7464633`
Supersedes: user authoring through the bounded Core DSL and Program JSON operation DSL
Runtime selection: QuickJS with JavaScript
Canonical migration plan: `QUICKJS_CONTROL_MIGRATION.md`
Executable plan: `../spec/quickjs-control-runtime.sdd.yaml`
Public authoring boundary: `QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md`
Controller extension: [`SELF_EVOLVING_AGENT_CONTROLLER.md`](SELF_EVOLVING_AGENT_CONTROLLER.md)
defines developer-authorized profiles, capability compilation, immutable
self-evolution, and the falsifiable general-agent-controller hypothesis.
Tracking epic: [#23](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/23)
Workstreams: [#24](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/24),
[#25](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/25),
[#26](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/26),
[#27](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/27), and
[#28](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/28)


## Decision

NeoGraph will have one user-authored language: standard JavaScript executed by
embedded QuickJS. The only public authoring frontends after cutover are that
sealed QuickJS surface and the direct NeoGraph C++ embedding API for trusted
applications. This replaces both the bounded Core DSL and the Program JSON
operation DSL. NeoGraph will own only the domain boundary that JavaScript cannot
provide: typed graph construction, canonical Core IR, capability admission,
typed host calls, budgets, cancellation, durability, replay, version identity,
and dispatch to pinned `GraphEngine` generations.

The decision follows three product requirements:

1. **Easy adoption.** Developers use a familiar language, editor, formatter,
   syntax highlighter, debugger vocabulary, and standard control flow. They
   learn the small `ng` domain API rather than two NeoGraph languages.
2. **Stable Program performance.** One fixed path—QuickJS computation, typed
   command validation, `ProgramRuntime`, then `GraphEngine` or a host
   binding—can be benchmarked and optimized without adding parser, plan, and
   dispatcher branches for every language feature. This is a measured stability
   requirement, not a claim that embedding QuickJS is automatically faster.
3. **Maintainability.** Core graph composition and Program control reuse
   ECMAScript functions, modules, expressions, conditions, and loops. NeoGraph
   deletes duplicate grammar, schema, lowering, dispatcher, migration,
   documentation, and test surfaces while retaining its validated IR and
   durable execution invariants.

QuickJS is the selected runtime, not one member of a permanent multi-language
surface. Lua and Janet are not parallel product modes. They may be reconsidered
only if QuickJS fails a blocking dependency, isolation, interruption, replay, or
platform gate.

## Why QuickJS

The current official QuickJS documentation describes a small embeddable
JavaScript engine with no external dependency, a C API, static or dynamic C
modules, `JS_NewCFunction()` for native bindings, `JS_SetMemoryLimit()` for a
runtime allocation ceiling, `JS_NewRuntime2()` for custom allocators, and
`JS_SetInterruptHandler()` for execution interruption. The selected language is
standard JavaScript rather than NeoGraph-specific syntax.

QuickJS is pinned at release `2026-06-04`; its audited source digest,
license, provenance, and isolated prefixed build sources are recorded under
[`../deps/quickjs/`](../deps/quickjs/). The default-off
`NEOGRAPH_BUILD_QUICKJS_CONTROL` target keeps Core-only consumers independent
of that dependency. Final cross-platform qualification and deletion of the
drain-only legacy implementation remain explicit cutover gates.

Authoritative upstream reference:
<https://bellard.org/quickjs/quickjs.html>

## Ownership boundary

### JavaScript owns

- lexical scope, functions, closures, and generators;
- expressions, conditions, loops, exceptions, and ordinary local state;
- arrays, objects, maps, sets, strings, numbers, and `BigInt`;
- pure graph-construction helpers and reusable control helpers; and
- source-level module syntax subject to NeoGraph's sealed module resolver.

NeoGraph will not add `ng.if`, `ng.loop`, `ng.map`, a second expression grammar,
or custom JavaScript syntax.

### NeoGraph owns

- typed graph-builder validation and canonical strict Core serialization;
- immutable source, bundle, runtime, Core, and dependency identities;
- the sealed `ng` module and versioned native binding ABI;
- input/output schemas and import-slot resolution;
- capability, effect, owner, tenant, and budget admission;
- command identity, journal publication, effect outbox, and replay;
- cancellation, child lineage, checkpoint, recovery, and terminal status; and
- invocation of pinned Core generations through `GraphEngine`.

`GraphEngine` remains the only executor of Core/application nodes. QuickJS has
two bounded contexts: definition evaluation constructs validated graph data
without dispatch, and Program evaluation yields durable control commands above
Core. Neither context is another node scheduler or a replacement for
`GraphEngine`.

## Public authoring boundary

JavaScript is the sole user-authored source language. Direct C++ construction
remains a trusted embedding API, not a persisted wire-level language. Strict
Core JSON, Program bundles, journals, and transport envelopes remain canonical
data artifacts, never fallback source languages. A C++ API may use `json`
in-process without creating a public JSON authoring protocol.

The full allowed-surface, canonical-artifact, removal, and cutover contract is
[QuickJS Public Authoring Boundary](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md).

## Authoring shape

### Graph definition

A module may export `define()` to construct Core topology through the sealed
`ng` graph-builder API. The host evaluates it in a compile context with
instruction, memory, and time limits and no effectful host calls. The returned
builder is validated and frozen as canonical strict Core IR before any node can
run.

```javascript
export function define() {
  const graph = ng.graph("review");
  graph.node("writer", {type: "agent", model: "writer-model"});
  graph.node("reviewer", {type: "agent", model: "reviewer-model"});
  graph.edge("writer", "reviewer");
  return graph;
}
```

JavaScript replaces Core DSL `vars`, interpolation, `templates`, `use`, and
`when` with ordinary constants, template literals, functions, imports, and
conditions. NeoGraph does not reproduce those features in the builder API.

### Program control

A control module exports a generator entry point. Generator yields are the only
way untrusted JavaScript requests durable NeoGraph work.

```javascript
export function* main(input) {
  let draft = input;

  for (let attempt = 0; attempt < 5; ++attempt) {
    const review = yield ng.callCore("reviewer", {draft, attempt});
    if (review.accepted) return draft;

    draft = yield ng.callCore("reviser", {
      draft,
      feedback: review.feedback,
    });

    yield ng.checkpoint({draft, attempt});
  }

  throw new Error("review attempts exhausted");
}
```

`ng.callCore()` does not execute Core directly. It constructs an immutable
command containing an admitted import slot and canonical arguments. Yielding
that command returns control to `ProgramRuntime`.

```text
JavaScript generator
  -> typed command yield
  -> command/schema/capability/budget validation
  -> durable transition publication
  -> pinned GraphEngine or registered host binding
  -> recorded result or failure
  -> generator.next(recorded outcome)
```

Unknown objects, forged slots, malformed arguments, unsupported commands, and
commands outside the admitted closure fail before dispatch.

## Native extension model

The language runtime must be extensible from C/C++. User programs receive no
arbitrary C FFI or ambient host authority by default. A developer may admit a
privileged profile with broader authority under the explicit guarantee and
identity rules below.

### Pure native intrinsics

A trusted intrinsic may run directly inside QuickJS only when it is:

- deterministic and safe to repeat during replay;
- nonblocking and free of external effects;
- bounded by declared allocation and instruction costs;
- unable to retain borrowed QuickJS or NeoGraph values after return; and
- unable to throw a C++ exception across the C ABI.

Examples include canonicalization, hashing, a reviewed parser, or a bounded
numeric primitive.

### Durable, recorded, and unmanaged host calls

Under the default `strict` profile, Core calls, child operations, providers,
tools, databases, files, networks, timers, and human input never block inside a
QuickJS C function. JavaScript yields a typed command; `ProgramRuntime` invokes
the registered C/C++ binding after the VM has returned to the host, journals the
outcome, and resumes the generator with a canonical value.

This keeps native callback lifetime, cancellation, asynchronous completion, and
replay outside the VM stack. A developer may explicitly admit `recorded` or
`unmanaged` effects. The effective Program guarantee becomes the weakest
reachable guarantee, and unmanaged effects receive no exact-replay,
duplicate-prevention, cancellation-completion, or crash-resume claim across
that boundary.

### ABI rule

External plugins bind through a versioned C ABI containing function pointers,
owned byte spans, an opaque `userdata` pointer, explicit cancellation, and an
explicit destroy hook. C++ users receive a convenience wrapper over that ABI.
`std::string`, `std::function`, C++ exceptions, project JSON object layouts, and
compiler-specific vtables do not cross the plugin boundary.

A raw function pointer is not an executable contract. Every binding also has:

- `ExecutableIdentity` name, semantic version, and implementation digest;
- input and output schemas;
- effect mode and idempotency classification;
- required capabilities and declared effects;
- replay and cancellation behavior;
- resource-cost declarations; and
- an exact `CapabilityBindingReceipt`.

The implementation must extend the existing `ExecutableManifest`,
`ExecutableKind::Imported`, `EffectMode`, admission profile, and catalog binding
surfaces instead of creating a second native registry.

## JavaScript safety profile

The default control context is constructed from an allowlist. It does not expose
the standalone QuickJS `std` or `os` modules.

The initial profile disables or excludes:

- dynamic native-module loading and arbitrary FFI;
- filesystem, network, process, signal, and environment access;
- `Worker`, atomics, shared memory, and host-created threads;
- wall clock, nondeterministic random values, and locale-dependent behavior;
- ambient module resolution;
- `eval` and `Function` construction where the selected QuickJS build permits
  compile-time removal;
- `WeakRef`, finalizer-driven control decisions, and host-observable GC order;
- direct provider, tool, credential, endpoint, or tenant selection; and
- host objects whose identity or lifetime cannot be serialized canonically.

NeoGraph supplies deterministic replacements only when a workload requires
them. Time, random values, external results, and human decisions are recorded
commands, not ambient JavaScript state.

### Developer-authorized profiles

NeoGraph is default-deny, not feature-deny. The default profile above remains
the safe product baseline, but a developer may explicitly grant scoped or broad
filesystem, network, process, environment, credential, provider/model,
dynamic-child, native-module, or unmanaged-effect authority. Requesting a
capability in source is not a grant; admission derives the effective
intersection with developer and tenant policy.

Every effective grant, native identity, capability scope, and execution
guarantee enters immutable Program identity and replay diagnostics. Credential
use and plaintext credential export are distinct grants. Dynamic source must
compile and admit before it can spawn. Budget may grow only through a separately
authorized journaled grant, never through retry, replay, resume, fork, child,
replacement, or mutation of the remaining balance.

The canonical authority model, `strict`/`recorded`/`unmanaged` guarantee
lattice, composition rules, capability-compiler boundary, and self-evolution
protocol are defined in
[`SELF_EVOLVING_AGENT_CONTROLLER.md`](SELF_EVOLVING_AGENT_CONTROLLER.md).

## Deterministic replay

QuickJS does not provide the architecture with a portable suspended-generator
snapshot contract. NeoGraph therefore recovers control execution by replaying
the pinned source from its entry point and consuming recorded command outcomes.

For each yielded command, the runtime compares at least:

- Program version and runtime build identity;
- logical command sequence;
- source location or stable command site identity;
- command kind and admitted import slot;
- canonical argument hash; and
- effect/idempotency coordinate when applicable.

An exact match returns the recorded result without external dispatch. A mismatch
is a deterministic replay failure and stops the run. The pending command at the
journal head becomes the live continuation point.

Replay does not replenish instruction, monetary, token, child, effect, or wall
budgets. Recorded historical work is not charged twice, while new computation
continues against the durable remainder under a separately reported replay-cost
counter.

Explicit `ng.checkpoint(serializableState)` may later reduce replay work, but it
must not introduce a second implicit continuation format. A checkpoint is
canonical application state plus an exact source/runtime coordinate, not a raw
QuickJS heap image.

## Source and bundle identity

The new Program source kind contains UTF-8 JavaScript source, a declared entry
point, sealed module imports, source maps, and the JavaScript profile version.
Admission compiles source with an exact QuickJS build and rejects source or
modules that cannot be resolved entirely from the sealed dependency closure.

A published bundle records at least:

- source hash and canonical source identity;
- QuickJS release, build options, and implementation digest;
- JavaScript profile and `ng` API versions;
- entry point and module dependency Merkle root;
- exact native/core executable receipts;
- requested and effective authority grants;
- capability/effect closure and effective execution-guarantee floor;
- input/output contracts and initial plus appended budget grants; and
- compiler diagnostics and source maps.

QuickJS bytecode is an internal cache only unless the dependency qualification
proves a stable, validated storage contract. Source plus exact compiler/runtime
identity remains the durable authority.

## Compatibility and cutover

Strict Core JSON remains the canonical serialization and low-level Core
interchange format; it is validated data, not a public programming language.
Direct C++ embedding may construct validated in-process `json` values, but
Harness and other public source transports do not accept standalone Core or
Program JSON authoring after cutover. The bounded Core topology elaborator,
Harness `mode: "dsl"`, Program JSON operation trees, Program-v2/v3/v4 authoring
schemas, and Harness `mode: "program"` are frozen legacy authoring surfaces.
They receive only correctness, security, and migration fixes while stored
definitions and in-flight runs drain. No new language feature is added to them.

The migration uses a clean cutover:

1. qualify and pin QuickJS without changing default runtime behavior;
2. add the sealed JavaScript `define()` graph-builder context and canonical Core
   lowering;
3. add JavaScript Program source, admission, and generator command execution;
4. prove deterministic recovery, stable performance, budgets, cancellation,
   and effect replay;
5. expose the versioned native C ABI and C++ wrapper;
6. switch all new user authoring to JavaScript;
7. classify and drain or explicitly migrate stored Core DSL and Program DSL
   versions; and
8. delete the Core elaborator and legacy Program parser/operation dispatcher
   after the announced compatibility boundary.

There is no permanent dual-language authoring product. Compatibility adapters
do not become alternate compilers or runtimes.

The self-evolving controller work tracked by
[#29](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/29) through
[#34](https://github.com/fox1245/NeoGraph-v1-redesign-backup/issues/34) is a
post-cutover extension. It reuses this one-language runtime and does not block
the base authoring cutover or introduce another compiler, scheduler, execution
engine, effect journal, capability registry, or activation model.

## Blocking evidence

The selected direction is accepted, but implementation cannot ship until the
following evidence exists:

1. QuickJS source, license, build, ABI, platform, sanitizer, and supply-chain
   qualification with an exact digest.
2. Memory ceilings and interruption terminate adversarial allocation, recursion,
   and infinite-loop fixtures without dispatch after failure.
3. The restricted context cannot access files, network, processes, environment,
   clocks, random values, dynamic modules, or unregistered native functions.
4. Generator replay reproduces nested loops, closures, exceptions, child calls,
   cancellation, checkpoints, and pending effects across process restart.
5. Replay of a completed non-idempotent call performs zero external dispatch.
6. Program source/runtime version mismatch and command mismatch fail closed.
7. Native ABI tests cover ownership, cancellation races, destruction, malformed
   results, exceptions, static/shared linking, and compiler boundaries.
8. Core-only installed consumers have no QuickJS dependency, allocation, branch,
   symbol, or binary-size cost when Program/QuickJS support is disabled.
9. Performance evidence separates compile-context evaluation, JavaScript
   computation, replay, Program scheduling, Core execution, provider/tool time,
   journal time, and startup. Repeated cold and warm benchmarks enforce explicit
   regression thresholds for stable Program performance.
10. The final source and issue migration leaves one authoritative user-authored
    language and no contradictory DSL roadmap.

## Non-goals

- Designing a NeoGraph expression, function, class, module, or macro language.
- Supporting JavaScript, Lua, Janet, the Core DSL, and the Program JSON DSL in
  parallel.
- Exposing Node.js, npm compatibility, browser APIs, or QuickJS `std`/`os` by
  implication or default.
- Treating planner-authored native libraries, shell commands, URLs,
  credentials, providers, endpoints, or descriptor claims as authority without
  an explicit developer grant and ordinary admission.
- Serializing raw QuickJS heaps, C pointers, callbacks, or live host objects.
- Moving Core node execution, persistence ownership, or effect commit into
  QuickJS.
- Claiming Turing completeness makes admitted runs unbounded; production runs
  remain finite under nonrenewable resource limits.

## Authority

This document owns the user-authored language and embedded-runtime direction.
`SELF_EVOLVING_AGENT_CONTROLLER.md` owns the developer-authority,
machine-readable capability, immutable self-evolution, and controller-evidence
extension. `V1_ARCHITECTURE.md` continues to own the retained Core IR, catalog,
activation, tenant, capability, durability, and `GraphEngine` boundaries.
Where the earlier architecture preserves the Core elaborator, requires a
bounded Program operation DSL, or rejects an embedded control interpreter, this
document supersedes it.
