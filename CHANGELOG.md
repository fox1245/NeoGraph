# Changelog

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

Dated entries below preserve historical APIs and measurements. Their old Provider compatibility guarantees are superseded by the breaking C++ typed contract in [0.14.0]; they are not current API guidance.

All notable changes to NeoGraph are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added
- **Deterministic Program gates with hidden test slots and content-addressed verdict tokens.** `GateTestSlot`, `ProgramGateVersion`, `GateVerdict`, `GateVerdictToken`, `GateSlotQualificationReceipt`, and `GateEvaluator` evaluate submitted JavaScript candidate modules (`export function evaluate(input)`) against host-owned public examples, hidden test cases, property predicates, and seeded deterministic case generators (`generate(seedHex, caseIndex)`, refusing an empty `generator_seed_hex` with `P_GATE_SEED_REQUIRED` when the slot has a `case_generator` and binding `generator_seed_hex` in the verdict token's canonical body and `id`, or the empty string when the slot has no `case_generator`). Every case runs in its own isolated QuickJS sandbox scope with strict intrinsics, explicit per-case resource limits (`GateSandboxLimits`), deep-frozen inputs, and module imports denied (`P_GATE_SANDBOX_IMPORT_DENIED`); a generated case whose generator throws on one `caseIndex` counts as one `runtime_exception` failed case with the exception text discarded while the rest of the slot still runs, and `GateTestSlot::parse` refuses `null` members (including `input` and `expected`) and requires `qualification_mutants` and `golden_candidates` so `parse(x).serialize_canonical()` equals the canonical form of `x`. Public feedback (`VerdictOnly`, `FailedCount`, `PublicExample`, `SanitizedSymptoms`) is built strictly from closed typed fields so hidden inputs, hidden expected values, hidden outputs, and candidate or generator exception text never leak; `GateEvaluator::qualify` refuses a slot with no qualification mutants (`P_GATE_SLOT_NO_MUTANTS`), validates slot modules and checks that the generator produces every case before it runs any candidate (`P_GATE_SLOT_GENERATOR_FAILED`), refuses a slot with no golden candidate (`P_GATE_SLOT_NO_GOLDEN`), verifies that every golden candidate passes every case (`P_GATE_SLOT_GOLDEN_FAILED`) and every qualification mutant fails at least one case (`P_GATE_SLOT_MUTANT_SURVIVED`), and binds the `generator_seed_hex` derived from `slot.serialize_canonical()` in the qualification receipt's canonical body and `id`.
- **Cookbook `examples/cookbook/program-coding-swarm/`: experimental proof of concept for a hierarchical multi-module coding workflow on the Program API.** A root Program uses an `orchestrator` model role to plan a toy module split and review the joined result, while its JavaScript `main()` spawns one child Harness per module in parallel (`ng.all`, or `--sequential` / `--concurrency <n>`) with non-renewable budgets carved by criticality, distinct model roles (`implementer-cheap`, `implementer-strong`, `reviewer`, `second-opinion`) resolved from `demo-models.json` or `live-models.json`, a host-owned verify and review-verdict gate, a grandchild second-opinion Harness for critical modules, mid-run SQLite generation replacement and rollback on `SqliteProgramStore`, and a comparison of `--synthesis=central` vs `--synthesis=delegated` through `ProgramSynthesisGateway` (`--report <file.json>`, `--runs <k>`, `--max-cost-usd`, `--max-cost-usd-other`).
- **WebAssembly JavaScript Program checkpoint handoff, replacement, live root Core graph migration and child synthesis (`handoff`, `releaseHandoff`, `replace`, `migrateGraph`, `synthesizeChild` on `ProgramRun` and `Programs`) (#640).** A `Programs` instance holds runs that reach an `ng.checkpoint(value)` safe point when `handoff()` waits for one, and exposes `ProgramRuntime::replace_async`, `ProgramRuntime::migrate_graph_async` and `ProgramRuntime::prepare_child_synthesis_async` (through `ProgramSynthesisGateway`) on the host-pumped executor.
  - **Checkpoint handoff and replacement (`handoff`, `releaseHandoff`, `replace`).** `handoff()` resolves with `{ runId, journalHead, coordinateId, entryId, value }` when a run reaches a checkpoint safe point (`programs.handoff()` with no run argument waits for the next checkpoint handoff of any run on the object), and `releaseHandoff()` releases the hold so the run continues without replacing it, resolving with a boolean. `replace(version, { input, budget, runId, journalHead, coordinateId, entryId, traceId, onEvent, signal })` replaces a held run (root or child) with an admitted `version` (starting from the source lineage's remaining budget, with `options.budget` overriding specified fields and `journalHead`, `coordinateId` and `entryId` overriding the handoff reference fields; when `input` is an object or omitted, starting from `{}` without copying the source run's initial `input`, `input.handoff` defaults to the checkpoint `value` and `input.previous_run_id` defaults to the source `runId`), keeps the source run's children so the replacement generation can attach to them, and returns the replacement `ProgramRun`. `replace` refuses with the C++ codes (`P_REPLACEMENT_AUTHORITY`, `P_REPLACEMENT_BUDGET`, `P_REPLACEMENT_TARGET`, `P_REPLACEMENT_INPUT`, `P_REPLACEMENT_HANDOFF`), rejecting with a `NeoGraphError` carrying `code` and `witness`, and leaves the source run and its hold as they were.
  - **Live root Core graph migration (`migrateGraph`).** `migrateGraph(version, { runId, semanticAdapter?, onEvent, signal })` calls `ProgramRuntime::migrate_graph_async` on the in-memory transition store and returns the migrated `ProgramRun`. It attaches a semantic migration adapter only when `semanticAdapter: true`; without one, the target must be the run's own admitted version or a compatible one (`P_GRAPH_MIGRATION_*` on refusal).
  - **Child synthesis (`synthesizeChild`).** `synthesizeChild(binding, source, { budget, capabilities?, effects?, grant, semanticDecision })` (or a single options object with `binding` and `source`) runs `ProgramRuntime::prepare_child_synthesis_async` through `ProgramSynthesisGateway` while the parent run is held at a checkpoint handoff and resolves with `{ id, binding, state, versionId, bundleId, proposalId, grantId }`. It needs the child's `budget` and the host's `grant` (`{ templateIdentity, validatorIdentity, contractIdentity, allowedCapabilities?, allowedEffects?, childBudget }`) and `semanticDecision` (`{ validatorIdentity, contractIdentity, accepted, evidence? }`); without them it is refused; the module makes neither from the child's request; the gateway accepts the decision only when it names the grant's validator and contract (`P_CHILD_SYNTHESIS_AUTHORITY` when the requested authority exceeds the parent run's or grant's envelope). While `synthesizeChild` uses a run's held handoff, `releaseHandoff`, `replace` and `cancel` of that run are refused until it returns, and a second `synthesizeChild` on the same hold is refused.
  - **Admission budgets and host ceilings (`programs.admit`, `programs({ limits })`).** In `programs.admit(source)`, `source` may be `{ javascript, name, budget }` or `{ javascript, name, budgetBounds: { minimum, maximum } }` (`budgetBounds` takes precedence if both are present). `budget` is the exact budget runs of this version are granted; `budgetBounds` is the range a grant may take, which a version that will be replaced needs, because a replacement carries the source's remaining budget, below the first grant. Members left out keep the host's default budget (`budget` and `budgetBounds.maximum`) or nothing (`0`, in `budgetBounds.minimum`, so `minimum` must set the structural floor members). The minimum must meet the Program's structural floor (at least one concurrent attempt, Program operation and Core step; at least one level and one child when the Program dispatches children), and the compiler refuses one that does not. Without either, the source's own declaration applies; a different `budget` or `budgetBounds` compiles a distinct bundle and version. `programs({ limits })` is the host's ceiling; its defaults allow one dynamic compile, a child depth of four, and four children per run (`maxDynamicCompiles: 1`, `maxChildDepth: 4`, `maxTotalChildren: 4`), and a host that synthesizes children or builds deeper hierarchies raises `maxDynamicCompiles`, `maxChildDepth` and `maxTotalChildren` there.
  - **Tests.** `wasm/test/program_replacement_test.mjs` checks checkpoint handoff and release, replacement of a held run with budget carry-over and retained children, the five `P_REPLACEMENT_*` refusals with `witness` and the source run and its hold left as they were, `migrateGraph` to the run's own admitted version and its guard refusals (same run id, an incompatible target), `synthesizeChild` with the host's grant and decision, its refusal without a grant, and `P_CHILD_SYNTHESIS_AUTHORITY`, replacement of a running child in a `root -> child -> grandchild` hierarchy, and checkpoint handoff, `replace`, `synthesizeChild` and that hierarchy replacement over an asynchronous JavaScript `stores` object.
- **Python Program replacement, live graph migration, and child synthesis gateway (`LocalProgramHost.replace`, `replace_async`, `migrate_graph`, `migrate_graph_async`, `register_synthesis_validator`, `prepare_child_synthesis`, `prepare_child_synthesis_async`) (#640).** `LocalProgramHost` now exposes checkpoint handoff leases (`ProgramHandle.next_handoff`, `next_handoff_async`, `latest_handoff`, `latest_handoff_async`), generation replacement at a held `ng.checkpoint`, live root-Core graph migration (`MigrationPlan`, `GraphSemanticMigrationAdapter`, `ProgramGraphMigrationTarget`), static and synthesized child bindings (`bind_child`, `start_child`, `recover_children`, `ProgramSynthesisProposal`, `ProgramChildSynthesisGrant`, `ProgramSynthesisSemanticDecision`, `register_synthesis_validator`, `prepare_child_synthesis`, `recover_child_synthesis`, `reserve_synthesis`, `synthesize`), and lineage/generation inspection (`run_record`, `run_lineage`, `run_generation`, `child_syntheses`). `ProgramChildSynthesisGrant(owner_scope, parent_run_id, parent_version, expected_lineage_head_id, reviewed_source, child_budget_ceiling, template_identity, semantic_validator_identity, semantic_contract_identity, ...)` requires `template_identity`, `semantic_validator_identity`, and `semantic_contract_identity` as `sha256` identities; `ProgramSynthesisSemanticDecision(validator_identity, contract_identity, accepted, evidence=None)` has no defaults for the first three arguments; and `LocalProgramHost.register_synthesis_validator(validate)` registers `validate(proposal, bundle, reservation)` returning a `ProgramSynthesisSemanticDecision`. Without a registered validator every synthesis is refused; the gateway accepts a decision only when it names the grant's validator and contract, and a durable child synthesis (`prepare_child_synthesis`) whose decision names another validator or contract ends `Failed` with `P_CHILD_SYNTHESIS_SEMANTICS`. Refused replacements leave the source run on its current generation with its held `ProgramHandoff` lease unconsumed, and accepted replacements carry existing children over to the new generation to rejoin through matching `ng.spawn` bindings. `migrate_graph` moves a live root Core graph run to a new generation of an admitted compatible version at a super-step boundary. The binding forwards to the C++ runtime and adds no policy of its own (`P_REPLACEMENT_AUTHORITY`, `P_REPLACEMENT_BUDGET`, `P_REPLACEMENT_TARGET`, `P_REPLACEMENT_INPUT`, `P_REPLACEMENT_HANDOFF`, `P_GRAPH_MIGRATION_INCOMPATIBLE`, `P_CHILD_SYNTHESIS_AUTHORITY`, `P_CHILD_SYNTHESIS_SEMANTICS`).
- **The developer chooses the stack and the `define()` time of a control source in the WebAssembly module: `neograph.programs({ javascript: { stackBytes, defineTimeMs } })`.** The module applied at most 48 KiB of its own stack to a control source whatever `JavaScriptCompileLimits::max_stack_bytes` asked for, gave `define()` a fixed 5 seconds, and the JavaScript API had no way to change either. 48 KiB and 5 seconds are now the defaults, and the developer chooses.
  - **`stackBytes`.** A whole number from 16384 up to what the module's own stack holds (983,040 in the module that `wasm/` builds), for `define()` and `main()` alike. A larger value, a value that is not a whole number and a member that does not exist are refused when `programs()` is called, with an error that states the bounds. The module enforces nothing else: the engine stays inside its own stack in linear memory, and what the host's call stack carries is the developer's to judge. The `max_stack_bytes` in the witness of a `P_JS_RESOURCE_LIMIT` diagnostic reports the size in force.
  - **`defineTimeMs`.** How long `define()` may run when a source is admitted, from 1 to 3,600,000 milliseconds; the default stays 5,000.
  - **What the host's stack carries, measured on Node 22 and 24** (host stack about 984 KiB, optimizing and baseline compiler): the 17 shapes of recursion and nesting that `wasm/test/program_test.mjs` refuses all end with `P_JS_RESOURCE_LIMIT` at 48, 64, 96, 128 and 144 KiB. `JSON.stringify` of data nested 100,000 deep overflows the host's stack at 160 KiB, seven of the shapes do at 256 KiB, and fourteen at 512 KiB; with the host's stack halved the largest size caught everywhere is 64 KiB. Keep `stackBytes` within an eighth of the host's call stack. A size that the stack cannot carry ends in a trap that the module cannot recover from, so a host that admits sources it does not trust still does it in a Worker that it can discard. `wasm/README.md` has how deep sources can go at 48, 96 and 128 KiB.
  - **Sources that run both natively and in the module.** The native limit is 512 KiB of the C stack and the module's is counted in its own stack, so the same source can be refused in one and run in the other. A source meant for both should stay within the module's limit (about 190 nested calls and 131 nested literals in a source at the default).
  - **C++ API.** `JavaScriptCompileLimits::max_stack_bytes` defaults to `JavaScriptCompileLimits::kDefaultStackBytes`, 48 KiB in a WebAssembly build and 512 KiB elsewhere, as before. A WebAssembly build honours a larger value as far as its stack holds instead of cutting it to 48 KiB (`javascript_stack_ceiling()` is its stack less 64 KiB). `RuntimeConfig::javascript_stack_bytes` (0 is the default) is the stack of `main()`, which was the default of `JavaScriptCompileLimits` whatever the compiler was configured with. A version has the same id whatever the limit, and `quickjs.c` compiled for a native build with `-O2 -DNDEBUG` gives the same object file.
  - **A stack below 4,096 bytes is refused, and a stack too small to start the engine says so.** `JavaScriptCompileLimits::max_stack_bytes` and `RuntimeConfig::javascript_stack_bytes` below `JavaScriptCompileLimits::kMinStackBytes` (4,096) make `ProgramCompiler` or `ProgramRuntime` throw `std::invalid_argument` when it is made: such a value is nearly always a count of KiB given as bytes, and every JavaScript run under it failed at once. The floor does not promise that a source runs in so little (a sanitizer build needs about 10 KiB to start one). When the engine cannot start, the diagnostic `P_JS_RUNTIME` now carries the stack and memory limits in its message and witness (`max_stack_bytes`, `memory_limit_bytes`) instead of naming no cause.
  - **Rebuild C++ consumers: `RuntimeConfig` gained a member.** `RuntimeConfig` gained `javascript_stack_bytes`. Rebuild NeoGraph and C++ consumers together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
  - **Tests.** `wasm/test/program_test.mjs` raises the stack to 96 KiB and checks that a source nested deeper than the default now runs in `define()` and in `main()` with the same version id, that every shape of recursion still fails cleanly with the raised size in the witness, that bad values and the bounds are refused, and that a module at the smallest size admits and runs a Program. `tests/test_program_runtime.cpp` checks that the runtime applies `javascript_stack_bytes` to `main()` and refuses one beyond the memory limit.
- **Experimental ACP remote transports: Streamable HTTP and WebSocket on one endpoint, off by default.** `neograph::acp_http_server` (`NEOGRAPH_BUILD_ACP_HTTP_SERVER`, default off) serves an `ACPServer` agent over the remote transport that the ACP project has proposed as a draft. stdio stays the standard transport and is unchanged. The wire details follow the draft where it is explicit and may change when it does. [`docs/acp.md`](docs/acp.md#remote-transports-experimental) has the details.
  - **One connection, one `ACPServer`.** `ACPHttpServer(factory, config)` builds an agent for every connection and drives it as stdio does: messages are handled in the order they arrive, and a connection that ends cancels its prompts. Connections are independent. A session id belongs to one live connection, and `session/resume` of one that another connection holds is refused with `session_busy`. `ACPHttpServer::client()` and `attach_client()` give graph nodes an `ACPClient` (new: a constructor and `bind` that take a resolver) whose `fs/*` and permission requests reach the connection that owns the session.
  - **Streamable HTTP.** A `POST` of `initialize` creates the connection (`200`, with `Acp-Connection-Id`). Every other `POST` is answered `202`, and the answer travels on a server-sent-event stream: a connection stream, and a session stream for each session (`Acp-Session-Id`). `DELETE` ends the connection, and a connection with no stream and no request for `connection_idle_timeout` (60 s) is closed, cancelling its prompts.
  - **WebSocket.** A `GET` with `Upgrade: websocket` on the same endpoint, one JSON-RPC message per text frame, with RFC 6455 framing: masked client frames, ping and pong, fragmentation, and a message over `max_payload_bytes` closes the connection with code 1009.
  - **Security.** The server binds loopback by default and a remote bind needs `authenticator`. Host and Origin (`allowed_hosts` and `allowed_origins`, with the rules of the MCP and A2A servers), the authenticator and the size limit (`max_payload_bytes`, 4 MiB) are checked on every route, stream and upgrade, before the body is read. A connection answers only to the principal that created it. A client cannot make the server start processes: `mcpServers` in `session/new` and `session/resume` is refused with `-32602` unless `allow_client_mcp_servers` is set. The messages that wait for the agent are bounded, and a `POST` beyond the bound is answered `503`.
  - **Where it differs from the draft.** The server speaks HTTP/1.1, while the draft requires HTTP/2, so each stream and `POST` is its own TCP connection. The 101 response does not carry `Acp-Connection-Id`, no cookies are set and `Acp-Protocol-Version` is not checked.
  - **Rebuild C++ consumers: `ACPClient` gained a member.** `ACPClient` gained a resolver, a constructor and a `bind` overload that take it. Rebuild NeoGraph and C++ consumers together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
  - **Tests.** `tests/test_acp_http_server.cpp` runs a full session, a permission round trip, an `fs/read_text_file` round trip, cancellation, two connections at once, a connection dropped in the middle of a prompt, the Host, Origin, size and authentication checks, and raw WebSocket frames (unmasked, fragmented, ping, binary). `bindings/python/tests/interop/acp_suite/test_acp_sdk_remote_interop.py` drives the probe agent (`neograph_acp_probe_agent --http`) with the client that the official Python SDK ships for the draft transport (`agent-client-protocol` 0.12), over both transports.
- **Python: every run, stream and resume entry point takes the same per-run values, and a resumed turn can stream.** `run_stream`, `run_async`, `run_stream_async`, `resume` and `resume_async` dropped the `RunMetadata` that `run` and `resume_from` took, and no resume entry point took a stream callback.
  - **`metadata` everywhere.** The entry points above take a `RunMetadata` (deadline, trace and run identity, owner scope). Without it the asyncio and streaming paths could not carry a deadline or an identity, and an engine with a lifecycle Hook runtime failed closed on them, because a Hook refuses an event that has no owner scope or no run id. A resume takes its own `RunMetadata`: the deadline of the interrupted run is not carried over. The argument is copied when the call is made, so changing the object afterwards does not reach a run in flight and one object can serve several calls. **Behavior change:** a value that is not a `RunMetadata` raises `TypeError` on every entry point; `run` and `resume_from` raised `RuntimeError` for it.
  - **`callback=` on the resume entry points.** `resume`, `resume_from` (which passed an empty callback) and `resume_async` (which built an empty one) take a stream callback, so a turn that continues after a human approval streams its tokens and events. It follows `run_stream`: it runs on the thread that drives a synchronous call and on the loop thread for an asyncio one.
  - **`resume_from_async`**, the awaitable peer of `resume_from`: exact checkpoint, `metadata=`, `interrupt_id=` and `callback=`.
  - **Compatibility.** The new arguments follow the existing ones. `callback` of the resume entry points and `metadata` of `resume` and `resume_async` are keyword-only, so a call that worked keeps its meaning.
  - **Not bound.** `run_until_safe_point_async` has no Python peer. A safe-point request is bound to an engine linked to one host generation; the Program catalog links such engines and `GraphEngine.compile` does not, so the entry point would reject every request.
  - `bindings/python/tests/test_run_entrypoints.py` drives each of the eight entry points and looks at what a node, a Hook and a stream callback observe, including concurrent calls that share one `RunMetadata`.
- **Python: a graph interrupt reaches A2A and ACP clients through `ProtocolHostAdapter`.** The adapter read the output of a run that had paused for a human as if it were the agent's answer, so an `interrupt_before`, a `NodeInterrupt` or a tool gate behind the official A2A or ACP SDKs completed the task or the turn with the last message of the state, which is the user's own text with the default reader.
  - **Reporting.** `stream(..., interrupts=True)` ends with an `interrupt` event instead of the final one. `ProtocolStreamEvent.interrupt` is a `ProtocolInterrupt` (`reason`, `payload`, `node`, `checkpoint_id`, `interrupt_id`), read from `interrupt_value` the way the native A2A and ACP servers read it. The adapter keeps the pause per thread: `pending_interrupt()` returns it and `discard_interrupt()` forgets it.
  - **Answering.** `resume_stream()` and `resume_payload()` resume the checkpoint the run paused at (`resume_from_async` with the interrupt id), so the resumed turn streams its tokens, and a turn that pauses again yields another `interrupt`. The paused node reads `resume_value(answer, interrupt)` from `input.ctx.resume_value`. The default is the answer itself, which is what the native servers pass: the text of an A2A follow-up message, the id of the ACP permission option. `ProtocolHostAdapter(resume_value=...)` replaces the mapping.
  - **The examples.** `27_a2a_server.py` puts the task in `input-required` (the reason as text, the payload as a data part, `metadata["neograph/interrupt"]` with the node, checkpoint and interrupt id) and resumes it on the next message to the task. `28_acp_agent.py` sends `session/request_permission` with the options `approve` and `reject` in the same turn and resumes with the option the client selects; a `cancelled` outcome, or `session/cancel` while it waits, ends the turn `cancelled`. A prompt that starts with `approve:` pauses the demo graph.
  - **Behavior change.** `run()`, `run_payload()` and `stream()` without `interrupts=True` raise `ProtocolInterrupted`, which carries the `ProtocolInterrupt`, when the graph pauses; they returned the last message of the state before, or what a custom `output_reader` made of the paused result, which it no longer receives. A graph that never pauses behaves as before, and every new argument is optional.
  - **Limits.** The pause is remembered in the process, and the engine needs a checkpoint store to resume it. After a restart, pass a `ProtocolInterrupt` built from stored ids as `interrupt=`; the A2A example always answers a task from the pause its own metadata names. With no recorded pause and no `interrupt=`, `resume_stream()` and `resume_payload()` raise `LookupError`: the engine answers a resume of a thread that is not paused with the text of its last turn. A pause that nobody answered (a `cancelled` permission request, a canceled task) stays open in the checkpoint store; the pause that follows it resumes normally. An ACP question lives in its turn and does not survive a restart of the agent, which the native server restores with `session/resume`. The collaboration envelope, Program-backed A2A tasks and tenant isolation of the native servers are not part of the adapter path.
  - `bindings/python/tests/test_protocol_interrupts.py` drives the adapter with a fake engine and with a real `NodeInterrupt`, and `test_protocol_sdk_e2e.py` runs both examples against the official SDK clients (`a2a-sdk` 1.2.2, `agent-client-protocol` 0.12.1): the pause, the answer that resumes it with streamed tokens, a rejection, a cancelled permission request, and a cancel while the question waits.
- **The stable-first assembly layout, opt-in per epoch (#329).** A provider's prompt cache reuses only the start of a request that is identical from its first byte, so an artifact that changes in front of the history costs every call a cache miss. An epoch that sets `ContextEpochData::assembly_layout` to `StableFirst` (it needs `ordered_artifact_ids`) is assembled as host instructions, `Stable` artifacts, `Slow` artifacts, RAW messages, `Volatile` artifacts, then the trusted supplemental input. `Legacy`, the default, is the assembly that 0.16.0 has: an epoch that does not ask for the new layout, and every epoch stored before, is encoded byte for byte as before, and a test compares the two layouts' bodies call by call where they mean the same prompt.
  - **The order is the epoch's.** The recorded list is the order, and no hash of the content takes part, so a sibling whose text changes keeps its place. `order_context_artifacts` builds the list: stability class, then higher `priority`, then `producer_id`, then a key the host gives; two artifacts that agree on all four are refused as ambiguous.
  - **A placement the layout cannot honour is refused, not moved.** `Stable` and `Slow` artifacts must be placed at `BeforeHistory` and `Volatile` ones at `AfterHistory`, and the list must name the `Stable`, then the `Slow`, then the `Volatile` ones. `ContextStabilityError` carries the refusal (`StableOutsideHistoryHead`, `VolatileOutsideTail`, `TierOrderViolated`). An artifact created with the default `BeforeLatestUser` placement therefore needs a placement of its own before it can join such an epoch. `validate_context_layout` runs the check without assembling.
  - **System text stays in front.** A System message may be a host instruction or a `Stable` or `Slow` artifact and nothing else (`VolatileSystemText`): `anthropic.messages`, `google.generate` and `google.interactions` lift every System message into the system field, where a RAW or supplemental one would move the start of the request. The lifted text is ordered `Stable`, then `Slow`.
  - **A layout change is a break.** Two consecutive epochs may differ in layout only if the later one declares `Breaks` with the reason `LayoutChange`, and `verify_context_epoch_claim` refuses it otherwise (`LayoutChangeUndeclared`), an `Extends` claim across the change included. `RuntimeInterpositionController::activate`, `StrictRuntimeProfile::activate` and the Program transition stores run it. `activate` now looks for the predecessor of every epoch, not only of one that claims to extend, since a layout change has to be declared whether or not it is claimed.
  - **Two boundaries and a digest in the receipt, read from the encoded body.** `stable_boundary` is where the `Stable` part (host instructions and `Stable` artifacts) ends, `prefix_boundary` where the append-only part (everything in front of the `Volatile` artifacts) ends, and `prefix_digest` covers the request up to the second. A boundary is the first message and part of the encoded body that is not in the prefix, counted after System text is lifted and tool results are regrouped; `{0, 0}` on a family that lifts System text is a prefix of the tools and the system field alone. The assembler finds them by encoding the request again with only the messages of the prefix and a marker message after them, and refuses the epoch with `PrefixBoundaryUnavailable` if the bodies do not agree. Preparing a request sends and claims nothing, but these are two more preparations per call, and none for a legacy epoch. A `Slow` System text that shares the system field with the `Stable` text leaves no `Stable` boundary, since the field is one block. The digest covers every member of the body except the stream flag and the output cap, so a streamed call and a buffered one have different digests. Those two are the members that the family's descriptor names at the top of the body; `google.generate` keeps its output cap inside `generationConfig`, which the digest covers. The number of boundaries is two, below any limit a family sets on cache breakpoints; marking them on the wire needs the SchemaProvider support that follows.
  - **Python.** `AssemblyLayout`, the `assembly_layout` field of `ContextEpochData` and `ContextEpoch`, `order_context_artifacts`, `validate_context_layout` and the new refusal codes.
- **The data model for a cache-stable context layout: a stability class and an anchor mode on artifacts, an order and a prefix claim on epochs, and the prefix fields of the assembly receipt (#329).** Nothing the assembler writes changes: the encoded request bodies and the order of the messages are byte for byte what they were. These are the fields the layout work builds on. Only the stable-first layout (the entry above) reads the order and fills the boundaries; no code acts on the anchor mode yet.
  - **Artifacts.** `ContextArtifactData::stability_class` (`StabilityClass`: `Stable`, `Slow`, `Volatile`) and `anchor_mode` (`AnchorMode`: `Replace`, `KeepInPlace`) are optional. Each is part of an artifact's identity only when it is set (storage schema version 2), so an artifact that sets neither keeps its bytes and its id. An artifact that declares no class derives one when it is read and nothing is rewritten: `RequiredSkill` and `HardConstraint` are Stable, a `DerivedContext` whose producer is `COMPACTION_PRODUCER_ID` (the summary a compaction transform writes) is Slow, and everything else is Volatile. `ContextArtifact::stability_class()` returns the declared or derived class and `declared_stability_class()` only the declared one. `ContextArtifact::create` refuses a Volatile artifact of a kind that renders as a System message with `ContextStabilityError` (code `VolatileSystemArtifact`), which is also a `std::invalid_argument` and, in Python, a `ValueError`.
  - **Epochs.** `ContextEpochData::ordered_artifact_ids` records the artifacts in the order the epoch fixes (`artifact_ids` stays the set, sorted by id). `prefix_claim` says whether the epoch `Extends` its predecessor's request prefix or `Breaks` it, and a break names its `break_reason` (`Compaction`, `StableChange`, `LayoutChange`, `AnchorDrop`, `OrderChange`, `Explicit`). An epoch that sets none of them is a version 1 epoch with the bytes and the id it always had. The legacy layout does not read the order: an epoch without one keeps the order the assembler derives from the artifacts.
  - **Claim verification.** `verify_context_epoch_claim` checks an `Extends` claim structurally. The predecessor is the epoch's predecessor and records an order, which an epoch of version 1 does not, so nothing extends one. `raw_from_sequence` is the same and `raw_through_sequence` is larger. The Stable and Slow artifacts are the same, in the same order. Every `KeepInPlace` artifact of the predecessor is still selected, and each one the epoch selects covers records inside its RAW window. Under the stable-first layout the artifacts must also form a layout the assembler accepts. What the claim promises depends on the layout: under `Legacy` it says that the `Stable` and `Slow` artifacts and their recorded order are unchanged and the window only grew, and it does not promise the start of the request, since the legacy assembler orders by placement, priority and producer; under `StableFirst` the recorded order is the order of the request. `RuntimeInterpositionController::activate` and `StrictRuntimeProfile::activate` run the check before they switch epochs, against the epoch that is active or against a predecessor the host passes to the new overload. A claim that does not hold, or that has no loadable predecessor, throws `ContextStabilityError` and leaves the active epoch as it was. The Program transition stores run it against the previous context publication, so a publication whose claim does not hold is a conflict. A break is not verified.
  - **Receipt.** `ContextAssemblyReceipt` version 2 adds `assembled_artifact_ids` (the artifacts in the order of the prompt), `artifact_classes` (with `class_sources`, which says for each artifact whether it declared its class or the receipt derived it, and `class_rule_version`, the `STABILITY_DERIVATION_RULE_VERSION` that the derivation followed), `encoder_family`, `tools_digest` (the digest of the `tools` member of the encoded body, unset when there is none; a body that is not a JSON object is an error and not a request without tools) and `break_reason` (the epoch's). It also has `stable_boundary`, `prefix_boundary` and `prefix_digest`, which the stable-first layout fills and the legacy layout leaves unset. The assembler writes a version 2 receipt for every turn, so the receipt of a turn has another id than before, and 0.16.0, which reads only version 1 receipts, cannot load a Program run whose context this version stored. A receipt of version 1 still parses and knows none of these fields. A stored receipt is checked against its artifacts strictly where a class was declared, and by deriving the class again only when its `class_rule_version` is the one in force, so a later change of the derivation rule does not make stored receipts unreadable. A Program run that is forked or replaced copies the order and these fields into the first epoch of the target.
  - **Python.** `StabilityClass`, `AnchorMode`, `ContextBreakReason`, `ContextPrefixClaim`, `ContextPrefixBoundary`, `ContextStabilityError` (with `code`, a `ContextStabilityRefusal`), `derive_stability_class`, `verify_context_epoch_claim` and `COMPACTION_PRODUCER_ID`, the new fields on the data and value classes, and an optional `predecessor` on both `activate` methods. `schemas/` gains `context-artifact-v2`, `context-epoch-v2` and `context-assembly-receipt-v2`.
- **A single-thread WebAssembly build of the graph engine, built and run in CI (#535).**
  - **`NEOGRAPH_THREADS`** is `OFF` on Emscripten and WASI and `ON` elsewhere. Off, NeoGraph builds without OS threads: no `-pthread`, no `Threads::Threads`, `ASIO_DISABLE_THREADS` exported with the `asio` target, and SchemaProvider built with `SP_THREADS=OFF` and no libcurl transport. `NEOGRAPH_THREADS=ON` keeps the pthread build on WebAssembly. Native builds are unchanged.
  - **One setting per program.** The generated `neograph/build_config.h` records the setting and is installed with the headers. A translation unit that disagrees with it stops compiling, as with the SDK's `sp/build_config.h`, and configuration fails when NeoGraph and the SDK differ. On Emscripten every target gets `-fexceptions`, and configuration fails when exceptions are disabled. CMake refuses the components that need sockets, TLS, system libraries or threads there (ASYNC, the MCP client, A2A, gRPC, SQLite, PostgreSQL, Python).
  - **CI** (`.github/workflows/wasm.yml`) builds the graph smoke and the wasm32 checks with Emscripten 6.0.13 in single-thread mode and runs them under Node 22 and 24. It fails when the smoke's `.wasm` or its gzip is over a stated budget, and it builds and runs the pthread smoke under Node 24. [`wasm/README.md`](wasm/README.md) has the commands, the measured sizes and what is unavailable on these hosts. The graph engine runs there; model calls follow in the next entry, Programs and stores do not yet.
- **Model calls through a WebAssembly host's `fetch`: a host event loop, a provider over the SDK's host seam, and a JavaScript module (#535).**
  - **`async::HostLoop`** (`neograph/async/host_loop.h`) runs coroutines on an Asio `io_context` that a host's event loop pumps, with no thread and no blocking call. A turn is a task on a SchemaProvider host executor that runs the ready handlers and returns. One is scheduled when work is executed on `HostLoop::executor()`: a run starts, a run is cancelled, or an SDK completion posts the continuation of a coroutine that awaits a provider call. An Asio timer is invisible to the host, so the engine's waits (a retry backoff, a tool timeout, the host-admission and resource polls) announce their deadline with `async::note_deadline`, and the loop sets one host timer for the earliest. An idle loop schedules nothing.
  - **`SchemaProvider(descriptor, options, executor, transport)`** builds a provider over the SDK's public host seam, with no libcurl and no OpenSSL; `create` takes the same arguments. `Provider::host_pumped()` says whether the SDK client's executor is run by the host's loop. `neograph::llm` is host-only where the SDK has no libcurl transport (WebAssembly, `NEOGRAPH_THREADS=OFF`): CMake does not look for OpenSSL, the component links neither `neograph::async` nor `SchemaProvider::transport`, and the libcurl constructor throws the SDK's `ConfigError` at `/transport`. TLS belongs to the host. `NEOGRAPH_BUILD_ASYNC`, the MCP client and A2A stay refused on WebAssembly. Native builds are unchanged.
  - **Behavior change on a host-pumped provider: a synchronous entry point refuses.** `GraphEngine::run` and `resume`, `Provider::dispatch` and `invoke`, and the synchronous endpoint calls drive a private `io_context` until the call is done, which would wait for a host loop that only the waiting thread can run. When such a call reaches a host-pumped provider it fails with the SDK's `runtime.blocking.host_pumped` reason (`ErrorKind::Misuse`, retry safety `NotSent`) before anything is sent, and the message names the `_async` form. A run that never reaches such a provider is unchanged.
  - **`complete_endpoint_async` takes the outcome the SDK delivered on a host-pumped provider.** It took it from `EndpointCall::join()`, which a host-pumped executor refuses, so a call that the provider accepted would have reported a refusal.
  - **The JavaScript module** (`NEOGRAPH_BUILD_WASM_JS=ON`: `neograph_module.wasm`, its loader and `wasm/neograph.mjs`) compiles a graph from JSON and runs it with a provider whose base URL, route, key and model are given at run time, returns the final state, and streams the graph events when asked. The API is five C functions and a small ES module (`load`, `compile`, `run`, `Graph.run` with `onEvent`, `signal` and `timeoutMs`), and the key is not in the binary. `wasm/examples/` has a Node program and a Cloudflare Workers-style handler with a `wrangler.toml` template that holds no secret.
  - **CI** runs the module under Node 22 and 24 against a local OpenAI-compatible server, buffered and as server-sent events, with no network and no key: the final state, the order of the streamed events, a character cut across two chunks, a provider refusal, cancellation by `AbortSignal`, a timeout, concurrent runs, and the host's timers running while a call is in flight. The Workers example's `fetch` handler is called in Node with a `Request` against the same server; it has not run on workerd. The wasm32 checks gain the refusal of a synchronous run. A size gate on the module (6.27 MB of `.wasm`, 1.84 MB gzipped; the graph smoke grows from 5,502,805 to 5,509,570 bytes). `tests/test_host_loop.cpp` covers the loop and the host-seam provider natively against a manual executor. [`wasm/README.md`](wasm/README.md) describes how the engine and the host's loop are joined.
- **A priced call that is charged more than its reservation is reported as a notice (#428).** The hold of a priced call prices the request's output cap at the dearest output rate, and `provider.max_price` bounds the endpoint's prices, not the tokens it returns. An upstream that ignores the cap, such as an OpenRouter upstream that ignores `max_tokens`, can return more output than the hold paid for, and the call is charged what its response reports.
  - **The notice.** `ProgramHandle::notices()` (Python `handle.notices`, read after `wait()`) holds a `P_MONEY_OVER_HOLD` notice for each call charged above its reservation, whether the evidence is `reported` or `estimated`. The witness is `{call_id, operation_id, node, model, evidence, reserved_microunits, microunits, over_by}`, plus `served_by` (the upstream that served the call) when the call's record has one, and holds nothing else of the request or the reply. Each attempt of a node retry is a call of its own. A `held` call is charged its hold and gives none, and neither does a call that a durable journal replays after a resume, a recovery or a fork.
  - **Nothing else changes.** The call is charged as reported, and the run's status, its budgets and `P_MONEY_BUDGET` are what they were. The notice is not stored with the run, so `result.monetary_records` (`microunits` above `reserved_microunits`) is the stored view.
  - **No route refusal.** NeoGraph has no setting that refuses a route after an overshoot; which routes to avoid is host policy. The notice and the record name the upstream that served the call (`served_by`, below), so the host can see which one overshot.
  - **Documentation.** The reference ([Pricing evidence and the model catalogue](docs/reference-en.md#pricing-evidence-and-the-model-catalogue)), the Program guide, the Python guide and the troubleshooting table say that the hold is an upper bound only for an upstream that stops at the cap, and list what a host can do: restrict the route with `only`, `order` and `ignore`, act on the notices and the records, and grant less than it can afford to lose.
- **A Program's monetary record names the upstream that served the call (#428).** `ProgramMonetaryRecord::served_by` (Python `record.served_by`) is the upstream that OpenRouter names for a Chat or Messages response, buffered or streamed, as SchemaProvider reports it in `ServedModel::served_by`. A host that is charged more than it held for a call can read from the record which upstream served it, to leave that upstream out of its routing.
  - **Where it comes from.** Every record that settles a response takes the name: `reported`, `estimated`, and a `held` record whose response, or whose failure's partial response, named one. It is unset when the response names none, when the origin is not a routing gateway, for a Responses reply, when the outcome is unknown and when the name is not one the record can hold (empty, over the SDK's 256 bytes, not UTF-8 or with a control character). A name never changes what the call is charged, and nothing else of the response is stored.
  - **Record metadata only.** The name is outside the call's identity, the journal's digests and the replay identity, and the journaled outcome, which is restored from its archive form, does not carry it. The record stored with the call does, so a record restored after a restart or a crash, and one that a fork adopts from its source, have the value they were stored with.
  - **The notice.** The `P_MONEY_OVER_HOLD` witness of the previous entry carries `served_by` when the call's record has one, and leaves it out otherwise. Only a `reported` or `estimated` call gives a notice, so the notice has the name that the record has. It is content of the notice and of nothing else: no identity or digest includes it, and the notice is not stored with the run, where the record is.
  - **Encoding.** A record with a name has the member `served_by`, and a record without one has the bytes it always had. A stored record whose `served_by` is not a string of that bound is refused. A build from before this change rejects a stored result or journal row that holds a `served_by`, so a downgrade of a store that has one finds a corrupt record.
  - **Documentation.** The reference ([Pricing evidence and the model catalogue](docs/reference-en.md#pricing-evidence-and-the-model-catalogue)), the Program guide, the Python guide and the troubleshooting table list the field and say that the notice carries it.
- **ProgramRuntime runs on an executor the host supplies, with no thread of its own (#557).**
  - **`RuntimeConfig::executor`** (an `asio::any_io_executor`) and **`executor_identity`**. Attempts, deadline timers and waiters run on that executor, and nothing runs inline. Unset, a native build keeps its pools (`scheduler_threads` threads for attempts, one for deadlines), which are now one implementation of an executor. A build without threads (`NEOGRAPH_THREADS=OFF`) requires it, and the constructor throws `ProgramExecutorRequired` (a `ConfigurationFailure`). Destroying the runtime cancels its runs; on an injected executor it does not wait for them, and the attempts keep the runtime's state alive until they finish, so keep running the executor until the handles are terminal.
  - **`ProgramExecutorIdentity`** tells the runtime what it cannot see in the executor: `host_pumped` (a host's event loop runs it) and `running_in_this_thread`. Both are detected for an executor over an `io_context` or a `thread_pool`, a strand of either, and the executor of an `async::HostLoop` (the new `async::is_host_pumped`).
  - **Timers.** Every timer the runtime waits on is an `async::HostTimer` (#567), so a host loop schedules a turn when one is due, and `scripts/host_timer_allowlist.txt` no longer excuses `src/program/`. `HostTimer` now takes its deadline back from the loop when it is cancelled, re-armed or destroyed (`async::forget_deadline`), so a run that has finished does not keep a host timer armed until its wall-time deadline. `HostTimer::arm_never()` is for a timer that only signals: a waiter that `cancel()` wakes.
  - **Node attempts.** A node attempt that needs its own worker (every attempt of a Program run, and any attempt with a timeout) runs on a strand of the injected executor instead of the 32-thread process-wide attempt pool. `RunMetadata::dispatch_executor` and `RunContext::dispatch_executor` carry it, and subgraphs inherit it. A node body then must not block its thread: it holds every run that shares the executor.
  - **Awaitable forms of every operation.** `start_async`, `start_active_async`, `replay_recorded_async`, `start_child_async`, `reserve_synthesis_async`, `prepare_child_synthesis_async`, `recover_child_synthesis_async`, `reconnect_async`, `recover_children_async`, `resume_handle_async`, `reconcile_async`, `fork_async`, `replace_async` (four forms) and `migrate_graph_async`. Each runs the body of its synchronous namesake on the runtime's executor and completes on the executor of the awaiting coroutine. `resume_async` keeps meaning "resume and wait for the result"; `resume_handle_async` returns the handle. `run_async` and `resume_async` are built on them. `migrate_graph` and `replace` wait for the source's safe point or handoff without blocking a thread (`RunControl::wait_graph_migration_async`, `ProgramHandoff::wait_async`).
  - **Behavior change on a host-pumped executor, or in a build without threads: the synchronous calls refuse.** `start`, `start_active`, `start_child`, `replay_recorded`, `fork`, `replace`, `migrate_graph`, `reconnect`, `recover_children`, `reserve_synthesis`, `prepare_child_synthesis`, `recover_child_synthesis`, `resume`, `reconcile` and `run` throw `ProgramBlockingCallRefused` (a `ConfigurationFailure`) before they do anything. `operation()` names the call and `alternative()` its awaitable form, and the message says both. `ProgramHandle::wait`, `ProgramHandoff::reference` and the wait inside `migrate_graph` refuse the same way when the result is not ready, and also when they are called from a thread that runs the injected executor, where they would wait for work only that thread can do. A `hook_runtime` blocks its caller, so the constructor refuses it on a host-pumped executor with the same error. Native runtimes without an executor are unchanged.
  - **Locks.** A critical section that reaches a store, or that waits for a Program, no longer holds a `std::mutex`. It holds an `OperationLock` (`src/program/operation_lock.h`): not recursive, granted in order, taken by `co_await lock.lock_async()` without blocking a thread or by `lock_blocking()` for the synchronous API. These are the lock of `fork`, `replace` and `migrate_graph`, the stripes that serialize a parent's child relation, and the process-wide transition lock (`ReentrantOperationLock`, which the same thread can take again, as the `std::recursive_mutex` it replaces could). A run attempt takes three more across store calls: the update of a task graph fragment's record, the recording of the start of an await in the command journal, and the debit of the dynamic-compile budget. The `std::mutex` that remain guard memory only. A Debug build, or `-DNEOGRAPH_PROGRAM_LOCK_AUDIT=ON`, counts them and aborts with the call's name if a call into the transition store is made while one is held.
  - **Tests.** `tests/test_program_runtime_executor.cpp` runs a Program on a single-thread `io_context` (the node runs on that thread; a wall-time deadline fires on it; a wait made on that thread is refused) and on a `HostLoop` over a manual executor (the synchronous calls are refused and name the awaitable form; a Program completes in host turns on the test's thread and the process gains no thread; a blocking wait is refused until the result is in; a deadline fires through the host's timers), and tests the lock. The wasm32 checks (`wasm/checks.cpp`) refuse a runtime without an executor, refuse a synchronous start on a host loop, and run a one-node Program with in-memory stores to its result in the host's turns, under Node 22 and 24. [`wasm/README.md`](wasm/README.md) says what runs where.
- **The Program store interfaces are awaitable, and a run attempt awaits them (#556).**
  - **Five interfaces return `asio::awaitable`.** `ProgramTransitionStore`, `ProgramStore`, `ProgramCoreToolGrantStore`, `TaskGraphFragmentStore` and `ModuleStore` keep their operation names, and each operation now returns `asio::awaitable<T>`. The operations are `[[nodiscard]]`: a call that drops its awaitable does nothing. The arguments of an awaited call, string views included, must stay alive until it completes.
  - **`Sync...Store` bases for synchronous backends.** A store whose backend is synchronous derives from `SyncProgramTransitionStore`, `SyncProgramStore`, `SyncProgramCoreToolGrantStore`, `SyncTaskGraphFragmentStore` or `SyncModuleStore` and implements the `..._sync` operations (`load_sync`, `compare_publish_sync`, ...). The base completes each awaitable operation from them: at once on the calling thread, or, when the store's `offloads()` returns true, on a thread of the bounded pool that the checkpoint stores use, and the awaiting coroutine resumes on its own executor. The in-memory and SQLite stores complete at once; PostgreSQL offloads. The Harness stores and the test doubles moved with them.
  - **`Blocking...Store` views for callers that cannot suspend.** `BlockingProgramTransitionStore` and its four siblings have the signatures the interfaces had. A view over a synchronous store calls its `..._sync` operation directly. Over any other store it drives the awaitable on a private `io_context` of the calling thread and waits, unless `BlockingStorePolicy::refuse` says that waiting would hang the thread that has to complete the call: then the call throws `ProgramBlockingCallRefused` before the store is touched. The runtime's policy refuses on a host-pumped executor and on a thread that runs its executor. Inside a turn of an `async::HostLoop` a view refuses whatever its policy says (`async::in_host_turn()`), so the views that have no policy (`ProgramCatalog`'s, `ModuleResolver`'s and the Core tool grant resolver's) cannot block the loop. `BlockingStorePolicy::await_synchronous_stores` makes a view await even a synchronous store, which the conformance suites use. `inspect_program_replacement_chain` takes a `BlockingProgramTransitionStore`.
  - **What the runtime awaits.** A run attempt awaits the transition store (the reads at the start of an attempt, the publication of a JavaScript command, the dynamic-compile debit, the command reservation and the graph-migration hold) and every call to the task graph fragment store, so an attempt that waits for an offloading store frees its executor thread for another run. The entry points, the terminal publication, the child-launch callback, the Core tool grant resolver and `ProgramCatalog` await their stores too (see the entry below).
  - **Tests.** The three conformance suites (`ProgramTransitionStoreConformance`, `ProgramStoreConformance` and the graph-migration capsule row) run every backend in two forms: `Direct` calls a synchronous store's own operations and `Awaited` calls the awaitable ones. One case in each of the first two awaits the operations in a coroutine. `tests/test_program_store_awaitable.cpp` covers the store contract: an inline store completes without suspending, an offloading one resumes the awaiting coroutine on its own executor, a blocking view waits for a store that completes on a timer or on another thread, and a refusing policy never reaches the store. `tests/test_program_runtime_executor.cpp` runs a Program over an offloading transition store, and shows that a run waiting for a store call leaves its executor free for another run, that a store which completes later serves a Program run by a host loop through the awaitable forms, that a synchronous call is refused there, and that the same store serves a runtime with its own pools. The wasm32 checks read a run back through the awaitable stores in a host turn. The PostgreSQL cases are built everywhere and run where `NEOGRAPH_TEST_POSTGRES_URL` is set.
- **Every Program operation awaits its stores, so a store that completes only in a later turn of a host's event loop works on a host-pumped executor (#556, #557).**
  - **One coroutine body per operation.** `start`, `start_active`, `start_child`, `replay_recorded`, `fork`, `replace`, `migrate_graph`, `reconnect`, `recover_children`, `reserve_synthesis`, `prepare_child_synthesis`, `recover_child_synthesis`, `resume` and `reconcile` are coroutines that await the transition, Program, module and task graph fragment stores. The `_async` form runs that body on the runtime's executor and never blocks a thread. The synchronous form, for a threaded host, runs the same body to completion on the calling thread, where a `Sync...Store` completes at once even if it offloads when it is awaited (`detail::InlineStoreCalls`, `detail::run_to_completion`), so the two forms cannot drift apart. The private `io_context` of such a wait is kept per thread and reused (`detail::BlockingIoContext`). A synchronous call is refused with `ProgramBlockingCallRefused`, before it starts, on a host-pumped executor, and, when a store is not a `Sync...Store` or completes on a pool, on a thread that runs the executor and inside a turn of a host's event loop. `operation()` now names the runtime call (`ProgramRuntime::start`), not a store operation. A refused `replace` leaves the caller's `ProgramHandoff` as it was.
  - **The terminal publication, the child launch and the recovery callbacks await.** `RunControl::complete_async` publishes a run's terminal state, and the completion callback that records a child's result in its parent is awaited. The child-launch, child-recovery, child-binding-validation and existing-child-reconnect callbacks, the checkpoint handler, the release of a graph-migration hold and the lineage reads of a reconnect are awaitable.
  - **The process-wide transition lock.** A coroutine can hold the lock that serializes a reconnect, a replacement, a migration, a handoff request and the cancellation of an interrupted run across a store call. It is no longer reentrant by thread, as it was for the blocking code, so a cancellation, a child's attachment or a retirement made while a replacement, a migration or a reconnect holds it is handed the lock (`held`) and does not take it again. Cancelling a run takes the lock whether the run is interrupted or live, so a cancellation that arrives while a replacement or a migration commits waits for it and then sees its outcome. **Behavior change:** a hook of the cancellation of an interrupted run (`hook_runtime`) and the `runtime_recovery_handler` that a reconnect calls run while that lock is held, so they must not call back into the runtime. Such a call re-entered the lock on its own thread before; it now waits for it for ever.
  - **`ProgramCatalog` and `ModuleResolver` await their stores.** `admit_async`, `admit_composed_async`, `find_version_async`, `resolve_version_async`, `resolve_version_with_binding_async`, `plan_migration_async`, `activate_async`, `rollback_async`, `activation_async`, `active_binding_async`, `collect_retention_async` and `ModuleResolver::resolve_async` are the bodies of the synchronous calls. The catalog's locks that were held across a store call are `OperationLock`s. `ProgramCatalog::completes_inline()` says whether every store of the catalog completes at once, and a synchronous catalog call is refused inside a turn of a host's event loop when one does not, and when an operation of the catalog that is suspended holds one of its locks. `ProgramSynthesisGateway::continue_child_async` awaits the publication of each stage and the catalog's admission.
  - **`RuntimeConfig::core_tool_grant_awaitable_resolver`** (a `ProgramCoreToolGrantAwaitableResolver`) and `make_durable_core_tool_grant_awaitable_resolver` resolve a Core tool grant by awaiting the grant store. When both resolvers are set, the awaitable one is used. The durable resolver of a store that offloads no longer shares the caller's string views with the pool thread.
  - **Handles.** `ProgramHandle::cancel_async`, `snapshot_async`, `latest_handoff_async` and `next_handoff_async` are awaitable. Each takes its own reference to the run, so the awaitable may outlive the handle. Cancelling an interrupted run publishes its Cancelled state through the store. The synchronous `cancel` does that where waiting is allowed. Where it is not (a host-pumped executor, or a thread that runs the runtime's executor) it does not wait: it stops an executing run in memory, leaves the release of a held graph migration and the durable cancellation of the run's children to a task of the executor, and schedules the Cancelled state of an interrupted run as such a task. `cancel_async` is the call that waits until that state is published.
  - **Behavior change: `cancel()` returns whether it accepted the cancellation.** It is true when the call stopped an executing run, or made (where it may wait) or scheduled (where it may not) the Cancelled state of an interrupted one. It is false when the run is over or cancelled already, when the cancellation of an interrupted run is pending already, and when the executor cannot take the task. An interrupted run on a host-pumped executor, or on a thread of the runtime's executor, used to give false and stay `Interrupted` and resumable, which a caller could not tell from a run that was over. Calling `cancel()` twice schedules one cancellation. While a scheduled cancellation is pending, `resume` and `reconcile` of the run are refused with `P_RESUME_CONFLICT` and `P_RECONCILE_CONFLICT` (a reconcile with an unknown outcome would move the record to AmbiguousEffect, which a cancellation leaves alone), so a resume or reconcile that starts after the call was accepted cannot continue the run; one that reached the store first wins, and the cancellation then finds the run no longer interrupted. A task that records nothing (the store failed, or the run was not interrupted any more) adds a `P_CANCELLATION_NOT_RECORDED` notice to `ProgramHandle::notices()` and leaves the run as it was, and a later `cancel()` tries again. The handle of an attempt that paused keeps its Interrupted result after the run is resumed through another handle. While a later attempt executes, `cancel()` on it returns false and schedules nothing. Once that attempt has paused too, the handle cannot tell: `cancel()` is accepted, and the Cancelled state is written in the attempt of the record it cancels.
  - **What still waits.** `ProgramHandoff::reference()` waits for its checkpoint, `RuntimeInstructionController` and `inspect_program_replacement_chain` read through blocking views, a `hook_runtime` blocks its caller, and the runtime reads and writes its `graph::CheckpointStore` with the store's synchronous calls.
  - **Tests.** `tests/test_program_host_pumped_stores.cpp` runs a Program lifecycle through the awaitable API on a host loop that the test pumps, over wrappers of the five stores that complete a call only in a later turn of the loop (`tests/fixtures/host_pumped_stores.h`): a start, a model node with a fake provider, an interrupt and its resume, a fork, a child spawn, a Core tool grant, a completion, a cancellation and reads of the catalog and the module store. Each path is also run with only its own store behind a wrapper. They show that nothing blocks, that no thread starts, that every call completes in a turn of the loop on the test's thread, and that the synchronous forms are refused inside a turn before they touch a store. `tests/test_program_runtime_executor.cpp` shows that a store which completes later serves a Program run by a host loop, and `tests/test_program_store_awaitable.cpp` covers the reuse and the nesting of the private `io_context`.

- **The JavaScript module runs Programs: compile and admit a source, start a run, pause it at an interrupt, resume and fork it, cancel it (#556, #557).** `neograph.programs({ provider, stores, owner, limits, budget })` in `wasm/neograph.mjs` returns a `Programs` object over the module's `ProgramRuntime`, which runs on the host's event loop and awaits its stores and model calls there. No store or model call blocks the loop and no thread starts; a control source runs in QuickJS on that thread, limited to 5 seconds for `define()` and to the run's wall-time budget for `main()`, and a source that recurses without end, or nests deeper than the parser can follow, fails with `P_JS_RESOURCE_LIMIT` (QuickJS's stack limit applies to the module's own stack, 48 KiB of it unless the developer chooses another size). `neograph.d.ts` has the types.
  - **The surface.** `programs.admit(source)` compiles and admits a JavaScript control source (or a Program document) and resolves with the version. `programs.start(version, { input, budget, runId, onEvent, signal })` returns a `ProgramRun` at once; its `result` resolves when the run ends or waits for an answer, and `resume(value)`, `fork(value)` and `cancel()` continue, branch or stop it. `programs.result(runId)` reads what the stores hold of a run and `programs.release()` cancels the runs that are executing. A source the compiler or admission policy refuses rejects with a `NeoGraphError` of type `compile` or `admission` and the list of diagnostics; a refusal of the runtime is a `NeoGraphError` of type `program` with its `code` and `witness`. A run that fails or is cancelled resolves with that status; a resume or a fork that is cancelled before it is sent rejects with a `cancelled` error.
  - **Events.** `onEvent` receives the run's journal entries in order, the model's reply as `llm_token` events. An error it throws cancels that run and rejects its `result`, and a run cancelled while it is still starting is cancelled once its handle exists.
  - **A fork takes half of what the paused run has left, read from the run's lineage, so a second fork of the same pause takes half of the rest.** The paused run stays resumable and the node that finished is not run again. Forks of one pause may be made in the same turn of the event loop. Each `Programs` object holds a lock per paused run from the read of its lineage to the publication of the fork, which debits it, so each fork reads what the forks before it left and the amounts they take add up to at most what the paused run had. A fork that would take all the wall time or Core steps that are left is refused with `P_FORK_SOURCE_BUDGET`, and one that asks for more than is left with `P_FORK_BUDGET`; neither is a conflict. A fork made again under the same `runId` is that fork: its record holds the amounts it was given, and the share is not read again from what the forks since then left.
  - **Budgets and the model catalogue.** The defaults are the largest budget a JavaScript source can declare (60 s, 10,000 model tokens, 32 Program operations, 100 Core steps), and `budget` and `limits` change them (a name that is not a member, and an amount that is not a whole number, are refused). A model call is sent only when its worst case fits the tokens left: the model's input limit plus its output limit, from the model catalogue. A model the embedded catalogue does not know needs its limits stated, and a new `provider.catalogue` member (the SDK's catalogue format, laid over the embedded one) does that for `compile` and `programs` alike. A model with no limits is not called, and the run ends `failed` with `NG.ConfigError`.
  - **A control source that recurses too deep fails with a diagnostic, and the module goes on.** QuickJS leaves its stack check out of an Emscripten build, so `JS_SetMaxStackSize` did nothing there: a source that recursed without end exhausted the host's own call stack, which raises a `RangeError` that no code in the module can catch, and the module instance was left unusable. The vendored `quickjs.c` now keeps the check on Emscripten (recorded in `deps/quickjs/NEOGRAPH_PROVENANCE`), reads the pointer of the stack in linear memory, and the Program runtime asks for 48 KiB of it by default, because the host's stack is used up between 1.5 and 7 times as fast as that one depending on what recurses (the default is calibrated against the host stack of Node 22 and 24; a developer can choose another size, see the entry on `javascript.stackBytes` below). The `max_stack_bytes` in the witness of the diagnostic reports the stack that is in force. Direct and mutual recursion, getters, Proxy traps, the callbacks of `sort`, `map` and `JSON.parse`, constructors, generators, `JSON.stringify` and `Array.prototype.flat` on data nested thousands deep, and a source nested 2,000 deep (parentheses, arrays, objects, calls) end with `P_JS_RESOURCE_LIMIT`: `admit` rejects with a `compile` error and a `main()` fails its run. The preprocessed source of a native build is unchanged. **Behavior change in the module:** a control source can now use, by default, about 200 nested calls (about 30 through a `sort` comparator) and about 130 nested parentheses or literals in a source, and no more. A source that went deeper and still finished before the host's stack ran out used to run, and now fails. The limit does not see a chain of about 5,000 Proxy objects without traps that `Object.getPrototypeOf` or `Object.isExtensible` follows, and a trap in WebAssembly cannot be recovered in the same instance, so a host that admits sources it does not trust should do it in a Worker that it can discard (`wasm/README.md`).
  - **Build.** The module links `neograph_program` when `NEOGRAPH_WASM_JS_PROGRAMS` (default `ON`) and `NEOGRAPH_BUILD_PROGRAM` are on; a JavaScript source also needs `NEOGRAPH_BUILD_QUICKJS_CONTROL`, which the CI configuration now sets. `NEOGRAPH_WASM_JS_PROGRAMS=OFF` builds the module that runs graphs only. **Size:** the module with Programs and QuickJS is 13,209,571 B (3,931,479 B gzipped), twice the 6,466,458 B (1,893,168 B) of the graphs-only module; the Program component is most of the difference and QuickJS about 1 MB of it (`wasm/README.md` has the other builds).
  - **The host's own stores.** `programs({ stores })` takes `{ transitions, programs, fragments, modules }` (`grants` is for C++ code that links the adapters; the module's Programs grant no tools), each an object whose methods are named after the C++ operations and return a value or a Promise. `adapt_js_stores` (`wasm/js_stores.h`) adapts them to the five awaitable store interfaces: a value completes the call at once, a Promise suspends the coroutine until a later turn of the loop, and the arguments are copied into the call. A thrown error or a rejected Promise is a C++ exception with the JavaScript message (an error with a `kind` names the class, such as `invalid_argument`), a value that does not parse is the same failure and names the method, and the adapter never returns a default for a failed call. A required method that is missing fails when the object is made, and so does a misspelt member. The stores keep their rules in C++: the adapters move values and decisions, and a publication that fails the checks that need no stored state is a conflict before it crosses. `neograph.memoryStores()` is the reference store, the in-memory C++ stores behind the same contract, which a host can wrap to persist what changes and replay it on start without writing the rules again (the contract, its value table and an example are in `wasm/README.md`). A JavaScript store does not take part in live graph migration. The registration of the object lasts until the last store made from it is gone, so a run cancelled by `programs.release()` can still publish its end.
  - **The store suites under Node.** The three conformance suites (`ProgramTransitionStoreConformance`, `ProgramStoreConformance` and the graph-migration capsule row) run against the adapters over the reference store, in both forms, in a single-thread Emscripten build of googletest (`NEOGRAPH_BUILD_WASM_STORE_TESTS`): 371 tests, 315 pass and 56 are skipped with their reasons (a durable backend, a thread, file custody), none fail; the `JavaScript` and `JavaScriptAwaited` backends run 64 and 62 of the same cases as the in-memory ones. Cases that raced two writers on threads make them take turns when the build has none, with the same assertions. `wasm/store_checks.cpp` and `wasm/test/js_stores_test.mjs` add what the suites cannot reach: a Promise resumes the awaiting coroutine in a loop turn and nothing waits meanwhile, a rejection and a malformed value fail the call, an outcome that arrives after the coroutine is gone is dropped, a missing method fails, and whole Programs run over Promise stores and fail closed over stores that reject.
  - **Tests and example.** `wasm/test/program_test.mjs` runs the surface against a local OpenAI-compatible server under Node 22 and 24: admission and its refusals, a document source, the event stream, a pause and its resume, forks, cancellation and `AbortSignal`, reading a run back from the stores, runs in flight together, a host event loop that keeps ticking, a provider refusal, a model with no limits, budgets out of bounds, a released object, and seventeen shapes of recursion and nesting (in `define()`, in the source and in `main()`) that must fail with a diagnostic, leave nothing uncaught and leave the module able to admit and run a Program; `wasm/test/program_stores_test.mjs` runs the host's stores through the module (values and Promises, rejections and malformed values, a missing method, a durable wrapper whose log a second object replays, and release). `wasm/examples/node/program.mjs` is a two-call Program with a pause that the test also runs as a child process.
- **`async::RequestOptions::connect_timeout` bounds the connection and nothing after it.** `RequestOptions::timeout` is one deadline for a whole exchange, so a caller that wanted a bound on a stalled connect had to bound the reply and the event stream too. `connect_timeout` (default zero, none) bounds name resolution, the TCP connect and the TLS handshake of `async_post`, `async_get`, `async_post_stream` and `async_request_stream`, and expires with `asio::error::timed_out`; the response is bounded only by `timeout`, when it is set. A name lookup that is already running cannot be interrupted, so the deadline is then noticed when the lookup returns. `ConnPool` and `CurlH2Pool` do not read it. The default changes nothing for a caller that does not set it.
- **The built-in gRPC servers take the caller's server credentials and message size limits.** `ServeOptions`, which `run_server`, `run_tool_server` and `run_checkpoint_server` accept, gains three fields. With none of them set a server behaves as before.
  - **`credentials`** is a `std::shared_ptr<grpc::ServerCredentials>`, for example `grpc::SslServerCredentials(...)` for TLS, with client certificate verification for mutual TLS. Left null, the server is insecure, as it always was; `grpc::InsecureServerCredentials()` says so explicitly. Credentials that cannot be used make the start throw `std::runtime_error`. NeoGraph's own gRPC clients, `GrpcRemoteTool` and `GrpcCheckpointStore`, still open channels without TLS, so a server that serves TLS is reached by another gRPC client.
  - **`max_receive_message_bytes` and `max_send_message_bytes`** set the largest message the server receives and sends. A larger one fails the call with `RESOURCE_EXHAUSTED`. Zero leaves gRPC's own settings, which are 4 MiB for a message received and no limit for one sent; a server that takes larger payloads, such as a checkpoint with a long history, raises the first. A value above `INT_MAX` throws `std::invalid_argument`.
  - `ServeOptions` grew, so code that builds one is rebuilt with the library. Authentication interceptors and several services on one port still go through your own `grpc::ServerBuilder` with `make_graph_service()`, `make_tool_service()` or `make_checkpoint_service()`.

### Changed
- **A loopback A2A server answers only loopback `Host` names and its own origin, and reads no body over 4 MiB.** `A2AServer` checked neither `Host` nor `Origin` and kept the HTTP library's body limit of 100 MiB. A page on another origin reached a server bound to a loopback address once its host name was re-pointed at the machine (DNS rebinding), and any client could send a body of 100 MiB. `A2AServer::set_http_config(A2AHttpConfig)`, called before `start()`, takes the fields of the MCP HTTP server with the same meaning: `allowed_hosts`, `allowed_origins` and `max_payload_bytes`.
  - **Host.** On a loopback bind (`127.0.0.1`, `::1`, `localhost`) the server answers `localhost`, `127.0.0.1` and `[::1]` on any port and the names in `allowed_hosts`, and refuses any other `Host` with `403`, with or without an `Origin`. A missing `Host`, or one that is not `host[:port]`, gets `400`. Entries are host names without a port, bracketed IPv6 addresses or `*` for any, compared ignoring case. On a non-loopback bind an empty list checks nothing, as before, and a non-empty one admits only the listed names.
  - **Origin.** A request that carries an `Origin` gets `403` unless `allowed_origins` lists it or, on a loopback bind, it is the server's own: `http://localhost:<port>`, `http://127.0.0.1:<port>` or `http://[::1]:<port>` at the bound port, each reported once on stderr. A request without an `Origin` is not checked. A non-loopback bind admits only listed origins.
  - **Every route, before the body.** The checks run before routing, so they cover the Agent Card, the JSON-RPC endpoint with its event streams and any other path, and the body of a refused request is never read. The JSON-RPC endpoint answers with a JSON-RPC `-32600` error (ErrorInfo reason `PERMISSION_DENIED`); other paths answer `{"error": "..."}`.
  - **Request size.** A body over `max_payload_bytes` gets `413`. The default is 4 MiB: a long message, or one inline file of a little under 3 MiB (base64 makes it a third larger). A larger file travels as a `uri` part.
  - An `allowed_hosts` entry with a port, an IPv6 address without brackets or an empty entry, and a zero `max_payload_bytes`, throw `std::invalid_argument`; the call throws `std::logic_error` once the server listens.
  - **Behavior change:** a loopback A2A server behind a local reverse proxy or a tunnel that forwards the public `Host` refuses those requests until that name, or `*`, is listed. A request with an `Origin` that is not listed (on a loopback bind: not listed and not the server's own) gets `403` on any bind; a client that sends no `Origin`, such as a server-side A2A client, is unaffected. A body over 4 MiB, which was read before, gets `413` unless `max_payload_bytes` is raised.
- **A built-in gRPC server that starts without credentials on an address other than loopback writes one warning to stderr.** `run_server`, `run_tool_server` and `run_checkpoint_server` have always served without TLS or authentication; started on `0.0.0.0:50051`, that is a plaintext service for every peer that can reach the port, and nothing said so. A start without `ServeOptions::credentials` on such an address now writes one `[neograph] warning: the gRPC server for neograph.v1.<Service> listens on <address> with insecure credentials ...` line. Loopback means `localhost`, `127.0.0.0/8`, `::1` and `unix:` sockets; `0.0.0.0` and `[::]` are not. The server starts and serves as before. Passing `grpc::InsecureServerCredentials()` as `credentials` says that plaintext is intended, for example behind a proxy that terminates TLS, and the line is not written.
- **The Program chatbot example answers only loopback `Host` names and its own origin.** The example's page and API (`cookbook_program_chatbot`) are bound to loopback, but a page on another origin reached them once its host name was re-pointed at the machine (DNS rebinding). For every route, before the route runs, `Host` must now be `localhost`, `127.0.0.1` or `[::1]` (`403` for another name, `400` when it is missing or malformed), and an `Origin`, when the request sends one, must be this server's own (`403`). The 12 KiB body limit and the demonstration tokens are unchanged.
- **The cross-framework benchmark tables are one cohort that a script renders from a checked-in record.** `benchmarks/README.md` no longer carries hand-typed tables or a hard-coded chart.
  - **One session, one machine, interleaved rounds.** NeoGraph (the `origin/master` commit, in the default hardened Release build and in an unhardened build as the engine ceiling) and five Python frameworks (the tip of each default branch from git at an exact commit, plus the latest release for the matched cells) ran in shuffled rounds, one fresh process per cell, on one quiet Arm machine. `benchmarks/records/` holds the record with the raw samples and the provenance: commits, compiler and flags, CPU, kernel, package versions, load average and the sha256 of the scripts. `scripts/render_bench_report.py` generates the tables and the chart in four languages from it, and `bench_report_current` fails when a document differs from its record.
  - **Ratios only between matched regimes.** NeoGraph with `worker_count=1` is compared with each framework's coroutine path with `async def` nodes, and `set_worker_count_auto()` with each thread-pool path (LangGraph `invoke`; Haystack `run_async` with sync components). pydantic-graph and AutoGen have no thread-pool path, and LlamaIndex's (plain `def` steps on the event loop's default executor) was not timed; the three are marked `n/a`. The API each framework's documentation shows first is reported separately, with absolute times and no ratio. NeoGraph's two worker modes are shown side by side, so no single "N times faster" figure is published.
  - **New tables.** Whole-process memory with the empty-runtime baselines, the increase over idle and the marginal memory per in-flight run; cold start; the sizes of the NeoGraph binary, library and WebAssembly modules and the installed footprint of each framework, labelled as different units of deployment; and probes (LlamaIndex instance reuse, the threads of LangGraph's fan-out, whether pydantic-graph's fan-out is real, the cost of hardening, and why two older pydantic-graph figures disagreed).
  - **The 2026-04 tables are history.** They moved to `benchmarks/history/` with a note on what is known to be invalid: mixed sessions, a thread pool compared with inline dispatch, the legacy pydantic-graph API, a LlamaIndex instance rebuilt in every iteration, whole-process memory of unlike processes.
- **Withdrawn: the burst guide's P50 and P99 columns and latency chart, its LlamaIndex multiprocessing rows and its multiprocessing memory figures.** The percentiles mixed service time with time in the system, the LlamaIndex harness created its handler outside an event loop so every call failed, and the memory counted the parent process only. The wall-time column stays, labelled as a closed single burst; a re-measurement with the corrected harnesses is pending. The `dr_compare` FANOUT=50 comparison is withdrawn as an engine comparison (the pools were not matched) and re-run with matched widths.
- **`dr_compare` matches the pool widths of its two runners.** `LG_MAX_CONCURRENCY` (default: the NeoGraph worker count) sets LangGraph's pool width, the runner refuses unmatched widths, and `session.py` runs the mock-model matrix and writes a record. The LangGraph runner is the synchronous `invoke`; it was called asyncio before.
- **A provider call's identity names what the request asks, not the SchemaProvider policy that admitted it (#543).** The Program provider-call journal named a call by `Provider::request_digest`, which includes the identity of SchemaProvider's descriptor policy. A request of the same content under another policy identity was therefore another call: a resume or a crash recovery found a logical call binding conflict, and a fork did not adopt the source's call and sent it again, so a call that was already paid for was paid for twice. Interface 7 changes every policy identity, which is how 0.16.0 met it. The journal now keeps the request content and the policy identity apart.
  - **Request content.** `Provider::request_content_digest(prepared)` covers the family, model, mode and origin; for an endpoint call its endpoint, method and URL; for an upload its metadata and the digest of the bytes it sends (#541); and the encoded body, which holds the messages (any prompt text NeoGraph supplies included), the tools and the options. The body is canonicalized, and the members the descriptor names for the model, messages, stream flag and output cap are set apart by role, so a policy that renames one of them does not change the content. The policy's identity, the descriptor's routes and headers, the retry policy and the observer limits are not part of it. `Provider::request_policy_identity(prepared)` returns the policy identity, and `Provider::request_digest` is unchanged. SchemaProvider has no canonical form of a typed request, and one written in NeoGraph would have to follow every field of every request type, where a missed field would let two requests share a call. So the encoded body is the content, and an SDK that encodes the same request differently counts as different content, which fails closed.
  - **Journal rows.** A row records the policy identity beside the content. It has the new columns `identity_version`, `content_digest`, `policy_identity` and `producer` (the NeoGraph build that encoded the request), and its `request_digest` names the content and the policy together. `SQLiteProgramProviderCallJournal::Record` gains the same four members, and `policy_changes()` lists the changes noted for a call (table `ng_program_provider_call_policy_changes`). Rows written by this version are not matched by 0.16.0, so a downgrade finds a binding conflict.
  - **A policy change is not a conflict.** A resume, a crash recovery or a fork that finds the same content under another policy identity replays or adopts the journaled outcome, notes the change without rewriting the row, and reports the notice `P_PROVIDER_CALL_POLICY_CHANGED` on the run's handle (`ProgramHandle::notices()`, Python `handle.notices`). The notice can be added while the run executes, so read it after `wait()`. Different content is still a logical call binding conflict: a resume that would send other prompt text, the host's or NeoGraph's built-in, sends nothing.
  - **Journals from 0.16.0 keep resuming.** Their rows hold only the dispatch digest, which includes the policy, and are read as before: a row whose digest equals the call's matches, so a 0.16.0 journal resumes, and a fork adopts its calls, under the policy it was written under. If the policy has changed since, the digest no longer matches and a resume finds a binding conflict.
  - **Journals from 0.15 still do not resume.** The digest of 0.15 includes the policy identity and the observer limits of SchemaProvider 0.3, which 0.4 cannot recompute, so no 0.16.x can tell whether the request of such a row is the call's. The 0.16.0 statement that a run journaled by 0.15 cannot be resumed by 0.16 stands. What changed is the fork of such a run, which no longer sends its calls again without saying so (next entry).
- **Behavior change: a fork refuses a provider call it cannot match to its source's (#543).** A fork looks up the source's record at the same position. It adopts a record of the same content. It sends the call when the source has no record there, when the source's record is a failure that proves the provider accepted nothing, or when the content differs and the same NeoGraph build encoded both. When it can neither adopt the record nor show that the call differs, sending would pay for the call again, so it refuses before anything is sent or journaled, with `ProgramProviderForkResendRefusedError`. Its `reason()` is `Unverifiable` for a row from before the content identity whose digest differs, `EncoderDiffers` when the content differs and another NeoGraph build encoded the row, and `DeploymentDiffers` when the content is the same and the source journaled it under another provider deployment. In a Core node it is `NG.ConfigError`, never retried and never routed, and the fork fails. `RuntimeConfig::fork_may_resend_provider_calls` (default `false`; Python `LocalProgramHost(fork_may_resend_provider_calls=True)`) allows the resend: the fork sends the call, which the provider may charge again, and its handle carries `P_FORK_PROVIDER_CALL_RESENT`. A source call with an unknown outcome stays uncertain whatever the flag says. A fork of a journal from 0.15 therefore fails by default, where it used to pay for its calls again.
- **Rebuild C++ consumers: `ContextEpochData` gained a member (#329).** `ContextEpochData` gained `assembly_layout`, `ContextEpoch` gained `assembly_layout()`, and `ContextPrefixBoundary` gained an ordering. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). An epoch of the stable-first layout is not read by 0.16.0.
- **Rebuild C++ consumers: members were added (#329).** `ContextArtifactData`, `ContextEpochData` and `ContextAssemblyReceiptData` gained members, `ContextArtifact`, `ContextEpoch` and `ContextAssemblyReceipt` gained accessors, and `RuntimeInterpositionController` and `StrictRuntimeProfile` gained an `activate` overload. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). A value that sets the new fields, and every receipt the assembler writes, is of a version that 0.16.0 does not read, so 0.16.0 cannot load a Program run whose context this version stored.
- **Rebuild C++ consumers: members were added (#543).** `RuntimeConfig` gained `fork_may_resend_provider_calls`, `ProgramCoreProviderCallReplaySource` gained `may_resend`, `ProgramCoreProviderCallContext` gained `diagnostic`, `SQLiteProgramProviderCallJournal::Record` gained four members, and `Provider` gained two static functions. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: `Provider` gained a virtual function (#535).** `Provider::host_pumped()` is a new virtual function, `OpenInferenceProvider` overrides it, and the protected `Provider::prepare_runtime` gained a defaulted argument. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: `ToolExecutionPolicy` gained a member (#545).** `ToolExecutionPolicy` gained `error_limit_bytes`. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: a virtual function was added (#541).** `detail::EndpointPayload` gained `opens_upload_session()`, and `PreparedProviderRequest` gained a member function of the same name. Opening the provider-call journal adds two columns, `withheld_secret` and `opens_upload`, to its table and creates the table `ng_program_provider_call_restarts`; an earlier build reads such a journal as before. `SQLiteProgramProviderCallJournal::Record` gained three members (`opens_upload_session`, `restarts`, `superseded`). Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Adopt SchemaProvider `34fd00f7e0eb3e43878cdc11b4e84d5b44d21d46`, the 0.4.x fixes on its `main` line (#535).**
  - **Compatibility.** The package stays 0.4.0, and the interface revision and shared-library generation stay 7, so `find_package(SchemaProvider 0.4.0 EXACT)` and the interface contract that NeoGraph checks are unchanged. The new capability bit `capability::HostSeam` is not part of `RequiredProvider`, which is what NeoGraph requires. The policy's replay identity is unchanged: native history and Program journals written by 0.16.0 still replay and resume. The embedded policy's content digest changes, because the policy gained facts, so a pin on the old digest (`expected_sha256_hex`) needs the new one. Some SDK structs grew (`EndpointFact`, `ModelPrice`, `CostEstimate`, the realtime `Options`). Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
  - **Pricing ([SchemaProvider #149](https://github.com/fox1245/SchemaProvider/pull/149)).** `sp::pricing::estimate` no longer gives a complete-looking total for usage it cannot price. Audio input and output tokens bill at their own rates (`input_audio`, `output_audio`); a missing audio or image-output rate, or Gemini's `toolUsePromptTokenCount`, leaves no total and is named in `unpriced`; a unit priced at zero is applied and recorded as an assumption; an entry priced only outside the asked service tier gives the reason `tier`. A Program call whose usage has no total is held, with the unpriced counters in its reason.
  - **Chat requests and replies ([SchemaProvider #151](https://github.com/fox1245/SchemaProvider/pull/151)).** A request of text alone over the size limit is `InvalidRequest` in every family (Responses and Interactions answered `ResourceLimit`); a request that carries media stays `ResourceLimit`. Generated audio keeps its `expires_at` on `Media`. A Messages request with `skills` and no code-execution tool is refused before it is sent.
  - **Realtime sessions ([SchemaProvider #152](https://github.com/fox1245/SchemaProvider/pull/152)).** A turn's usage keeps its modality detail, and a tool-call turn keeps its usage. The opening handshake leaves out the reserved headers a caller names. Every queued event is bounded, not only audio, and `Options::deadline` counts from `open`. Audio formats are policy facts, and a provider's close code for a wrong key ends the session with an `Authentication` error instead of a clean close.
  - **Endpoint calls ([SchemaProvider #153](https://github.com/fox1245/SchemaProvider/pull/153)).** A charged endpoint call is attempted again only when it never reached the provider or the provider refused it with a complete 429 or 503; that refusal is retried without the duplicate-billing opt-in, and the opt-in lets no other failure through. Images and speech accept responses up to 48 MiB, with `runtime.response.limit` as the reason beyond it. Transcription asks for `verbose_json` when word timestamps are requested and checks the audio type and size (25 MiB at OpenAI); chunked-upload parts have a size cap; image results, duplicate decision options, calendar dates and poll answer ids are checked.
  - **Model catalogues ([SchemaProvider #155](https://github.com/fox1245/SchemaProvider/pull/155), [#156](https://github.com/fox1245/SchemaProvider/pull/156), [#157](https://github.com/fox1245/SchemaProvider/pull/157)).** A catalogue's canonical text and digest no longer depend on the C locale or on the order of its lists, which the loader now sorts. A layer without origins applies at every origin when merged, so the embedded entries layered over a refresh keep applying at a gateway origin. A refresh fails a source that lists no models, far fewer than before, a malformed Gemini page token or too many entries, and keeps that source's entries; `sp_catalogue_refresh` exits 3 and writes nothing after a failure.
  - **WebAssembly ([SchemaProvider #159](https://github.com/fox1245/SchemaProvider/pull/159)).** The SDK builds for Emscripten and WASI without libcurl or threads (`SP_BUILD_TRANSPORT`, `SP_THREADS`), `sp::crypto::random_bytes` works on those hosts, and an admission ceiling above the target's `std::size_t` is clamped. NeoGraph's own WebAssembly build follows in #535.
  - **Host seam ([SchemaProvider #161](https://github.com/fox1245/SchemaProvider/pull/161)).** `runtime/host.h` is public: an `Executor` (with `host_pumped()`), a `HostTransport` and `Client(descriptor, options, executor, transport)`, defined by `sp_runtime` without libcurl or threads. A blocking call that cannot work, `Client::complete`, `Operation::join` or a realtime session's `next_event`, now returns at once with a `Misuse` failure (`runtime.blocking.host_pumped` or `runtime.blocking.in_thread`) instead of waiting; the in-thread refusal had no reason before. `SP_BUILD_JS_HOST` builds a reference host over `fetch` for Emscripten. NeoGraph uses the seam for its WebAssembly module (see Added).
- **Adopt SchemaProvider `76afa3cf69b9ac81180217dfcc489572b9a59f4b`: typed queries for hosts, cache marks on request parts and the serving upstream.**
  - **Compatibility.** The package stays 0.4.0, and the interface revision and shared-library generation stay 7, so `find_package(SchemaProvider 0.4.0 EXACT)` is unchanged. The policy's replay identity is unchanged: native history and Program journals written under the previous pin still replay and resume. The embedded policy's content digest changes, because the policy gained facts, so a pin on the old digest (`expected_sha256_hex`) needs the new one. The capability bits `PartCacheMarks` (14) and `ServedByUpstream` (15) join `RequiredProvider`, so a NeoGraph built on this pin refuses an SDK library built before them when it creates a `SchemaProvider`. The interface revision stays 7, so the reverse is not detected: rebuild every consumer that was built with the older SDK headers.
  - **Typed queries ([SchemaProvider #163](https://github.com/fox1245/SchemaProvider/pull/163)).** `core/enum_names.h` gives the public enums a host stores a stable name, `DescriptorPolicy` answers which controls, service tiers, routing and tool facts a family has, `PreparedRequest::facts()` returns the service tier, fallback models, plugins, routing and server tools of a prepared request, `request_declined` tells a failure that the provider declined, `sp::pricing::worst_case` and `sp::runtime::worst_case_cost` bound the cost of a prepared request (invocations times attempts) and name the reason when there is no bound, and `messages::decode_usage` reads a Messages `usage` object. NeoGraph's own pricing and settlement do not use them, so the holds, charges and records of a Program are what they were.
  - **Cache marks on request parts ([SchemaProvider #164](https://github.com/fox1245/SchemaProvider/pull/164)).** `Text`, `Media`, `ToolResult` and Messages tool definitions carry a typed `cache` (`CacheControl`, five minutes or one hour), and a Messages request carries `system_blocks`. The policy says, per origin and route, whether marks are taken, how many (a request above the limit is refused, never trimmed) and which lifetimes, and a mark leaves out the request-level `cache_control`. Marks are outside the SDK's configuration and content digests and its replay identity. NeoGraph's portable JSON of a message (`provider-message-v3`) does not carry a mark. NeoGraph's content digest of a Program provider call (#543) is taken over the encoded body, so it covers a mark that is written. The cache routes are facts of a family's declared OpenRouter origins: a policy that changes those origins has to change the `origins` of the family's `cache` entries with them, or leave the list out where it declares none, or the loader rejects it (`/cache/routes`).
  - **Serving upstream ([SchemaProvider #165](https://github.com/fox1245/SchemaProvider/pull/165)).** At a declared routing gateway, `ServedModel::served_by` (`Completion::model` and `PartialCompletion::model`) holds the upstream that OpenRouter names for a Chat or Messages response, buffered or streamed. It is response metadata outside the replay identity, so the portable JSON of an outcome (`provider-outcome-v3`) and the outcome that the Program provider-call journal stores do not carry it, and an outcome that is replayed names none.
  - **Behavior change: a response whose chunks name different serving upstreams fails ([SchemaProvider #165](https://github.com/fox1245/SchemaProvider/pull/165)).** At a declared routing gateway, SchemaProvider did not read the `provider` member of a Chat or Messages response before; a later chunk or object that names another upstream than the first is now `ProtocolCorrupt`, with the reason of a changed id or model (`chat.identity.changed`, `messages.identity.changed`), and the failure keeps the first name on its partial result. A chunk that leaves the member out is not a contradiction. Other origins are unchanged: the member is still not read.
  - **Documentation.** [SchemaProvider #162](https://github.com/fox1245/SchemaProvider/pull/162) changes documents only.
- **Rebuild C++ consumers: SDK layouts grew.** SchemaProvider's `Text`, `Media`, `ToolResult`, `ServedModel`, `messages::ToolDefinition` and `messages::Request` gained members. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: `ProgramMonetaryRecord` gained a member (#428).** `ProgramMonetaryRecord` gained `served_by`. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **The public-text guard checks documents.** `scripts/check_public_text.py` and its ctest entry `public_text_guard` scan Markdown, `docs/`, README and CHANGELOG files (the `NEOGRAPH_PUBLIC_TEXT_GUARD_INCLUDE_DOCS` option is removed), and two new rules flag wording that narrates internal decisions and the process behind them.
- **The live tests, the Python examples and the documentation no longer name OpenAI models that OpenAI lists as retiring.** The shutdown dates are OpenAI's, on its [deprecations page](https://developers.openai.com/api/docs/deprecations); this project promises none. Each replacement was called once with a real key before it was used. No library code named one of these models.
  - **Chat.** The default of `OPENAI_MODEL` in the Python examples and in the live fan-out test, and the `controls.chat.models` example, move from `gpt-4.1-mini`, `gpt-4o-mini` and `gpt-4.1` to `gpt-6-luna` and `gpt-5.6-luna`. The live tests that used `gpt-5-nano` use `gpt-6-luna`, and the live catalogue test uses `gpt-5.6-luna`, a model that the built-in catalogue does not list. The OpenAI Chat tool-call case sets `reasoning_effort` to `none`, because OpenAI refuses function tools on Chat Completions while these models reason. OpenAI accepts only the default `temperature` from these models, so the Python examples that set one no longer do.
  - **Transcription and images.** The live speech round trip transcribes with `gpt-transcribe`, not `gpt-4o-mini-transcribe`, and the live image test uses `gpt-image-2.5-sunburst`, not `gpt-image-1-mini`.
  - **Spoken input of the realtime tests.** The live realtime transcription sessions get their speech from a `gpt-realtime-2.1-mini` Realtime session that reads a text aloud, not from the speech endpoint.
  - **The speech endpoint.** One live test still calls `/v1/audio/speech` with `tts-1`. OpenAI's page lists `tts-1`, `tts-1-hd` and both `gpt-4o-mini-tts` snapshots as removed on January 6, 2027, with `gpt-realtime-2.1-mini` through the Realtime API as the replacement; it names no model for the endpoint. The test names `tts-1`, so it has to change or go by then.
- **Rebuild C++ consumers: members and functions were added (#557).** `RuntimeConfig` gained `executor` and `executor_identity`, `RunMetadata` and `RunContext` gained `dispatch_executor`, and `ProgramRuntime` gained its awaitable functions and keeps its state in a shared object. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: members and functions were added, and some changed (#556, #557).** `RuntimeConfig` gained `core_tool_grant_awaitable_resolver`. `ProgramHandle` gained four awaitable functions, `ProgramCatalog` its `_async` functions and `completes_inline()`, `ModuleResolver` `resolve_async` and `ProgramSynthesisGateway` `continue_child_async`. The five `Sync...Store` bases gained `completes_at_once()`. `ProgramRuntime` gained private coroutine bodies, and the callbacks that `RunControl` keeps return awaitables. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: the Program store interfaces changed (#556).** The operations of `ProgramTransitionStore`, `ProgramStore`, `ProgramCoreToolGrantStore`, `TaskGraphFragmentStore` and `ModuleStore` return `asio::awaitable<T>`. A class that implemented one of them derives from the matching `Sync...Store` class instead and renames its operations `..._sync` (or returns awaitables itself), and code that called a store through the interface reads it through a `Blocking...Store` view or awaits it. `inspect_program_replacement_chain` takes a `BlockingProgramTransitionStore`. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: `RequestOptions` gained a member.** `async::RequestOptions` gained `connect_timeout`. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Behavior change: the A2A client no longer ends an exchange after 30 s.** The 30 s default of `A2AClient::set_timeout` was a deadline for the whole HTTP exchange, so a blocking `SendMessage` whose task ran longer than 30 s, and any event stream that stayed open that long, failed with `A2A request failed` on the defaults, although the agent was still working. The default is now a 30 s bound on connecting (name resolution, the TCP connect and the TLS handshake, through `RequestOptions::connect_timeout`) and no bound on the exchange. `set_timeout(seconds)` still bounds the whole exchange, as before; the new `set_connect_timeout(seconds)` sets the bound on connecting; 0 removes either, so `set_timeout(0)`, which used to remove every bound, now leaves the one on connecting. A caller that relied on the 30 s limit to end a stalled agent, or to stop a blocking call that nothing else cancels, calls `set_timeout`.
  - **A stream ends when its consumer stops it.** `send_message_stream` closes the connection when `on_event` returns `false` or an event carries an error, instead of reading the stream to its end. With no deadline on the exchange, a stream that never ends would otherwise hold the call.
  - **A Harness job's downstream A2A call ends when the job is cancelled and after `call_timeout`.** The capability executor of `a2a::make_harness_capability_executor` read the job's cancel token only before and after its call, and the Program-hosted Harness hands each Tool call a token that is never cancelled, so the 30 s default was all that ended a call to an agent that did not answer. `send_message_sync` now takes a trailing `graph::CancelToken*`, and the executor runs the call under a child of the job's token: cancelling the job closes the connection and throws `graph::CancelledException`, and the new `call_timeout` argument of `make_harness_capability_executor` (30 s by default, the bound these calls had; 0 for none) does the same with a `std::runtime_error` that the Harness reports as a Tool error. An `A2ACallerNode` call already ended with the run's cancel token or the node's `attempt_timeout_ms`.
  - The Python `A2AClient.set_timeout(seconds)` is unchanged: it truncates to whole seconds, so a value below 1 now means no bound on the exchange where it meant no bound at all. Python has no `set_connect_timeout`.
- **Rebuild C++ consumers: `A2AClient` gained members and parameters.** `A2AClient` gained `set_connect_timeout`, a data member, and a trailing `graph::CancelToken*` parameter on both `send_message_sync` overloads; `a2a::make_harness_capability_executor` gained a `call_timeout` parameter. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: `StdioClientConfig` gained members.** `mcp::StdioClientConfig` gained `max_stdout_text_lines` and `max_stdout_text_bytes`. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).

### Fixed
- **A JavaScript source that runs out of memory while QuickJS records its error's stack fails with the memory limit instead of crashing the process.** QuickJS 2026-06-04 recorded the stack on the pending error without holding it, and a second out of memory during that step released the error first, so the step then used freed memory (upstream issue bellard/quickjs#533). The vendored `quickjs.c` now holds the error for that step, in the shape of the open upstream fix bellard/quickjs#565; `deps/quickjs/NEOGRAPH_PROVENANCE` records the patch and the digests of the archive's and the vendored `quickjs.c`. The JavaScript runtime identity (`ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS`) is unchanged, so stored sources and bundles still load. Seen first as an access violation on Windows in the gate's memory-limit test.
- **Program: `migrate_graph()` retires a live source run as `Cancelled` without a transition conflict, and a migrated root Core graph continues from its migrated channel state without turning the run input into a user message (#634, #635).** `ProgramRuntime::migrate_graph()` cancelled a live source run with `cancel_process_controls()` after the successor run had already moved the active lineage, so the source's own terminal publication lost its compare-and-swap and its handle ended `Failed` with `P_TRANSITION_CONFLICT`. `migrate_graph()` now retires the live source control to the successor control with `retire_to()` as `replace()` does, so the superseded source handle ends `Cancelled` without publishing a conflicting terminal record. In addition, `migrate_graph()` and `reconnect()` of an unanswered checkpoint passed the run input as the attempt input to `execute_run_attempt`, which stored it in `resume_answer` and passed it to `GraphEngine::resume_from_async`, serializing the run input as a user message in `messages`. Those checkpoint continuations now pass `json()`, and `execute_run_attempt` leaves `resume_answer` empty and passes `json()` to `GraphEngine::resume_from_async` when no resume answer is given, so the migrated run continues from the source checkpoint's channel state.
- **Program: a run granted no `model_tokens` sends no model call.** Like every `RunBudget` amount, `model_tokens = 0` grants nothing, but a Core run held a model call against its token budget only when that budget was above 0. A call under a grant of 0 was therefore sent, on the first attempt, on resume and on reconcile alike, and the run was reported `BudgetExhausted` only afterwards, with the tokens already spent. The call is now refused before it is sent: the run ends `BudgetExhausted` with `failure.code` `NG.BudgetExhausted` and `failure.witness["model_tokens"]` of `granted` 0, `used` 0 and `over_by` 0, and nothing is journaled or charged. A run that sends no model call still completes under a grant of 0, and a Harness worker node does not start under it. A SchemaProvider endpoint call (an embedding, an image) has no token bound to hold, so a grant of 0 does not refuse it (a ceiling above 0 does), and the tokens it reports still end the run `BudgetExhausted` after the call. `monetary_microunits` and `max_core_steps` already treated 0 the same way at the hold and at the end of the run. **Behavior change:** a run that relied on a grant of 0 to send model calls without a token budget now ends `BudgetExhausted`; grant it the tokens it may spend. `graph::RunConfig` gained `model_token_budget_bounded` (default false, so a Core run outside a Program still reads `model_token_budget = 0` as no ceiling), and `graph::RunContext`, `graph::ProviderCallIdentity` and `ProviderDispatchBudget` carry it: rebuild C++ consumers.
- **Opening multiple `PostgresCheckpointStore` instances against one database no longer hangs on schema migration locks.** Constructing a `PostgresCheckpointStore` or rebuilding a connection slot ran `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and `ALTER TABLE ... ALTER COLUMN save_seq SET DEFAULT` unconditionally outside the migration advisory lock. In PostgreSQL those `ALTER TABLE` statements acquire `AccessExclusiveLock` even when the columns and default already exist, and they locked `neograph_checkpoints` and `neograph_checkpoint_blobs` in the opposite order from `save()` and `delete_thread()`. A store constructed or reconnected while another connection held a lock on the checkpoint tables waited for `AccessExclusiveLock` and queued later reads and writes behind it. Schema setup now runs under the migration advisory lock, creates tables in the same order as runtime writes, and checks `pg_attribute`, `pg_attrdef`, and `to_regclass` before issuing migration DDL so an already-current schema takes no exclusive table lock.
- **A JavaScript `main()` that waits on several calls at once no longer fails with `P_JS_COMMAND_JOURNAL` while the children it started publish (#614).** An `ng.await` with a timeout records its start in the command journal, and `ng.spawn` records each child in the run's record. Each is a compare against the head of the run, and every child that is attached, dispatched or joined moves that head. The command publication read the head again and tried again, but three times only, and the child record five times. On a Release build with a threaded executor, a `main()` that awaited four spawned children ended `Failed` about one run in ten with `P_JS_COMMAND_JOURNAL` ("Could not durably record the start of a JavaScript await"), and a few runs in a thousand with `P_CHILD_CONFLICT` ("Parent-child dispatch lost its transition CAS"), although nothing the Program or its children did was wrong.
  - **A lost compare is tried again up to 64 times.** A compare is lost only when another publication of the run was written between the read of the head and the write, so each retry reads a newer head. The JavaScript command publication (the pending entry, the start of an await, the settled members of a join, the result of a command) and the child record publications (attachment, dispatch and completion) share the bound, and a head read that disagrees with itself is re-read and counts against the same bound. When the bound is exhausted, the run fails as before (`P_JS_COMMAND_JOURNAL`, or `P_CHILD_CONFLICT` for a child record), now after at most 64 attempts instead of 3 or 5.
  - `tests/test_program_runtime.cpp` refuses the compare of one member's await-start record ten times in a row, and for ever, with a store wrapper (that member's start is then recorded and the run completes with all four starts recorded; the run fails with `P_JS_COMMAND_JOURNAL` after all 64 compares of that record are refused), and runs the four-child case 40 times on a four-thread executor.
- **A Program wall-time budget or await timeout that the clock cannot count is no limit, not a deadline in the past (#617).** `RunControl` computed a run's deadline by adding `std::chrono::milliseconds(wall_time_ms)` to the steady clock in the constructor for a new attempt and the constructor that continues a stored record, and `run_attempt` did the same for `await` timeouts. Because `std::chrono::steady_clock` counts signed 64-bit nanoseconds, a budget or remaining budget above about 9.2e12 milliseconds (about 292 years), including `UINT64_MAX`, overflowed the time-point addition into the past and ended the run at once with `TimedOut`. Run deadlines, `await` deadlines, lifecycle hook parent deadlines, and JavaScript control remaining wall-time calculations now saturate through the shared `deadline_after()` helper.
  - When the system clock is before the 1970 epoch, a lifecycle hook's parent deadline is `system_now + remaining` rather than saturating to the latest time point.
- **A durable child synthesis whose semantic decision names another validator or contract than its grant ends `Failed` with `P_CHILD_SYNTHESIS_SEMANTICS` (#640).** `prepare_child_synthesis` records a `Failed` `ProgramChildSynthesisRecord` with `error_code` `P_CHILD_SYNTHESIS_SEMANTICS` when the `ProgramSynthesisSemanticDecision` returned by the registered validator names a `validator_identity` or `contract_identity` other than the grant's `semantic_validator_identity` and `semantic_contract_identity`, instead of failing with a generic invalid-record error.
- **A JavaScript wall-time limit that the clock cannot count is no limit, not a deadline in the past.** `JavaScriptCompileLimits::max_wall_time_ms` became a deadline by adding `std::chrono::milliseconds(limit)` to the steady clock. The clock's nanoseconds overflow a signed 64-bit count at about 292 years (9.2 trillion milliseconds), and `2^63` milliseconds do not fit the millisecond count at all, so a limit above about 9.2e12, `UINT64_MAX` among them, which a host can use for "unlimited", ended `define()` and `main()` at once with `P_JS_TIMEOUT` (the addition overflowed). The deadline now saturates at the latest time point. The JavaScript module was not affected: `defineTimeMs` is bounded to 3,600,000 milliseconds.

- **`reconcile()` with `Completed` on an interrupted JavaScript `ng.callCore` continues `main()` instead of failing with `P_JS_COMMAND_JOURNAL_MISMATCH` (#613).** A run with a JavaScript `main()` that is interrupted at a Core node's effect holds no reservation of the call's wall time, model tokens, money and Core steps, because the interruption releases it. A resume reserves them again when it continues the call. A reconciliation that completed the call did not: its attempt found no reservation in the journal, and the run ended `failed` with `P_JS_COMMAND_JOURNAL_MISMATCH` ("Exact Core resume has no durable command resource reservation"), so the result the host supplied was never used.
  - **Now** the reconciliation reserves the call's resources as a resume does. The attempt that continues the call holds the reservation and is granted what the journal holds, the call settles against it, `main()` continues with the reconciled result, and the run ends with the tokens and money it did not spend. An attempt that a restarted process recovers after a stop inside the reconciled call finds the reservation in the journal.
  - **Behavior change.** A reconciled call that ends before it has started (it is cancelled, its deadline passes, the host refuses its capacity, or a handler throws at its first event, `Started`) ends with the remaining budget the reconciliation left it, as a resumed one does (#557): the wall time, model tokens, money and Core steps reserved for the call are not given back, because nothing has measured their use. Before, such a run gave back its whole budget.
  - **Unchanged.** A reconciliation that ends as `Unknown` or `Failed`, one that completes a JavaScript command that is not a Core call, and one of a run without a JavaScript `main()` take the paths they took.
  - **Tests.** `ProgramRuntimeTest` (in memory and on SQLite) and `ProgramHostPumpedStores` complete a reconciled `ng.callCore`, compare the budget its attempt is granted with the journal, reconcile a second call after the first, and recover a reconciled attempt after a stop inside the call. On the previous engine none of them got past the reconciliation: the run ended `failed` with the code above.
- **A run resumed with `resume()` after stopping inside a JavaScript command whose effect is still pending is granted what its journal holds.** This applies when a run stopped inside a JavaScript command before its effect was settled (`reconnect()` returns `Interrupted` with `result.interrupt.pending_effect` naming the command, not a normal `callCore` pause on `pending_input`), and the host continues the run with `resume()` (the only way in Python) rather than `ProgramRuntime::reconcile()`. `reconcile()` of the same state already grants what the journal holds: the unreserved remainder plus the wall time and Core steps the command had reserved. `resume()` instead granted the command's whole reservation, tokens and money included, which the journal had already consumed, so the attempt's limits were wider than the journal's: a later priced command could be sent and then rejected by the journal with `P_JS_COMMAND_JOURNAL` when its result was published.
  - **`resume()` and `reconcile()` now agree.** The attempt gets back only the wall time and Core steps reserved for the stopped command. The tokens and money reserved for that stopped command stay consumed, because what it used is unknown, so after such a `resume()` the run can spend only what was not reserved when the command started (`reconcile()` of the same state already behaves this way). A later priced command that no longer fits is refused before it is sent, with `BudgetExhausted` and `P_MONEY_BUDGET`. A later command that spends nothing continues `main()` and the run completes. A cancel before the resumed attempt runs on the scheduler ends the run `Cancelled`. Python's `LocalProgramHost.resume()` has the same behavior.
  - `tests/test_program_command_recovery.cpp` stops a run inside a JavaScript command and answers it with `resume()` and with `reconcile()`. It compares the grant at `Started` with the journal's remainder, and checks a later priced command, a join of priced calls, the provider calls attempted and sent across both processes against the grant, a cancel before the attempt starts, and that the resumed command is not sent again.
- **A pause that follows a pause nobody answered can be resumed (#600).** A host that did not resume a `NodeInterrupt` and ran again on the same thread with `resume_if_exists` (a cancelled task, a closed session, the next message) left the first pause as the parent of the new run's checkpoints, and when the new run's first super-step paused too, resuming that second pause failed with `NodeInterrupt ancestry crosses super-step boundaries` before any node ran. So did `fork` of it and a persistent subgraph's child in the same state. A step of the new run that completed before the second pause saved a Completed checkpoint for it to hang from, and was not affected. The pending writes a resume replays are those of the pause's own super-step: the walk over its ancestors reads the pauses of that step and the checkpoint the step started from, and it ends at a pause of an earlier step after reading the writes recorded under it for this step, which are the finished siblings of the new run's step.
  - **The abandoned pause's writes are not replayed.** They carry the earlier step, so a sibling that finished in the first turn does not stand in for the second turn's, and a sibling that finished in the second turn is replayed instead of running again. A run that continues a thread starts at the step after the checkpoint it continues from, so its first pause never has the step of the pause it follows. The unanswered pause stays in the store and still resumes by its checkpoint ID.
  - **The other checks stay.** A cycle, a parent that is missing, of another thread or of another schema, and a pause whose parent is a pause of a later step are refused as before, in the blocking walk (`fork`, `resume_pending_writes`) and in the awaitable one (`resume`), which now share their checks. **Behavior change:** the refusal of a pause whose parent is a pause of a later step now reads `NodeInterrupt ancestry runs backwards across super-steps`, and `NodeInterrupt ancestry crosses super-step boundaries` is no longer thrown.
  - `tests/test_abandoned_pause.cpp` covers a second, a third and a fan-out pause after an unanswered one, pauses of one step on both sides of it, a state update between them, a run that restarts at the same step number, `fork`, resume by checkpoint ID, a persistent subgraph and the refusals above, on the in-memory and SQLite stores. 33 of its 39 store-parameterized cases failed on the previous engine; the other six pass on both and pin what the fix must leave alone (a step that completes between the pauses, a run that starts over without `resume_if_exists`).
- **Python: cancelling the Future of `resume_async` stops the resumed run.** The call made no cancel token, so cancelling its Future ended the wait and nothing else: the run went on, and `input.ctx.cancel_token` was `None` in its nodes. It ties the Future to the run's cancel token as `run_async` does. **Behavior change:** a cancelled `resume_async` now stops its run.
- **A resume replays the newest pending write of a task id, so a retried step no longer replays the attempt that failed (#604).** A step that ran more than once from the same checkpoint, because an earlier attempt failed or was cancelled before it saved its pause and the host ran the turn again, leaves the pending writes of every attempt under that checkpoint, and every attempt gives its tasks the same ids (`s3:calm`). A resume replayed the oldest row of a task id, so the pause of the retry was resumed with the result that a sibling had produced in the attempt that failed, and the result of the attempt being resumed was dropped. A resume now reads the rows of each checkpoint newest first and keeps the first one seen for each task id. `resume`, `resume_async`, `resume_from` and `resume_from_async` read them through the awaitable walk, `fork`, `resume_pending_writes` and `CheckpointCoordinator::load_for_resume` through the blocking one, and both use one selection.
  - **What stays.** The walk still goes from the resumed checkpoint to its ancestors of the same step, nearest first, so a row under a nearer checkpoint decides before a newer row under an ancestor. Rows of different task ids are all kept. The stores are unchanged: `get_writes` returns the rows in insertion order, and rows that are already stored are read the new way. The empty rows that mark the start of a retried invocation (`s3:act#a1`) are read by presence, so they are not affected.
  - **Behavior change.** A task id with a row from each of two attempts is replayed from the attempt that recorded last, also when the checkpoint both attempts started from is itself resumed by its ID.
  - **What is still open.** A store keeps one channel value per `(thread, channel, version)`, so a checkpoint that reuses a version of another branch of the thread reads that branch's value; that is the other half of #604.
  - `tests/test_linear_history.cpp` runs a step whose first attempt cannot save its pause and is run again, answered with `resume`, `resume_from` and a `fork`, with a task id that only the retry recorded, and the rows of an ancestor, a pause and a Completed checkpoint through every read of the resume, on the in-memory and SQLite stores. Its cases for the channel values stay skipped unless `NEOGRAPH_TEST_LINEAR_HISTORY=1`.
- **Python: an exception that a stream callback raises is reported.** `run_stream` documented that such an exception is printed to stderr and does not abort the run. The run went on, but nothing was printed: pybind11 had already cleared the error, so `PyErr_Print` had none to print. The exception goes to `sys.unraisablehook` now, which prints it to stderr by default; `PyErr_Print` is not used because it exits the process for `SystemExit`. **Behavior change:** a callback that raises is no longer silent. The callbacks of `resume` and `resume_from` take the same path.
- **The Python binding exposes `ContextArtifactKind.UntrustedSupplemental`, and the artifact schema lists every kind and placement (#329).** The kind existed in C++ and was not bound, so a Python host could not create or compare that kind. `schemas/context-artifact-v1.schema.json` listed two placements and five kinds, which rejected stored artifacts that the code writes.
- **The Python binding exposes `ContextPlacement.AfterHistory` and `ContextPlacement.BeforeHistory` (#329).** The enum bound only `BeforeLatestUser` and `AfterLatestUser`, so a Python host could not create a context artifact that sits before the history or after it, although the C++ assembler orders all four placements. `test_every_context_placement_is_bound_and_assembles_in_its_documented_place` assembles one artifact per placement through a controller and checks the order in which the provider receives them.
- **A completion that arrives from another thread no longer races with the host-pumped loop (#602).** A blocking tool runs on the process's blocking pool and posts its continuation back to the run's executor from the pool's thread. On a `HostLoop` that post reached `HostLoopCore::wake()`, which read and wrote the plain flags `closed_`, `scheduled_` and `running_` that the host's turn writes, while the header said that `HostLoop` is not thread-safe. ThreadSanitizer reported 25 data races in three `ProgramHostPumpedStores` tests, and the test executor `tests/fixtures/manual_executor.h` raced the same way (a queue without a lock, and an `in_thread()` that was true on every thread while the host thread ran a task). `wake()` also skipped scheduling whenever a turn was running, so a post from another thread that landed as the turn ended could be left without a turn.
  - **The contract.** Handing work to the loop is safe from any thread, as it is for an `io_context`: `execute`, `post` and `dispatch` on a copy of `HostLoop::executor()`, and `HostLoop::wake()`. Turns, and every handler in them, still run one at a time on the host's thread, and `spawn`, `wake_at`, `forget_at`, `idle`, `note_deadline` and the destructor stay the host thread's. The host executor's `post` is now called from the posting thread, with no lock of the loop's held, so an executor that takes work from threads other than its own must accept concurrent `post` calls; its `schedule`, `cancel` and `now` are still called on the host's thread only. The loop must outlive such posts: its destructor drops the handlers that did not run and cannot wait for a thread that is still posting. The completions that the library's own pool threads post are the exception (#605). `include/neograph/async/host_loop.h` states this.
  - **How.** `scheduled_` and `closed_` are atomics. A wake claims the turn with one atomic exchange, so at most one turn of the loop is queued on the host, and a turn clears the flag before it polls, so a post that lands after that schedules the next turn. A post from inside the loop's own turn is still run by that turn (a thread-local marks the thread that runs it). The wake path takes no lock, and the single-thread WebAssembly build runs the same code (the atomics compile to plain accesses there).
  - **A host that refuses a post.** An executor that is shutting down may throw from `post`. A thread that had found the turn scheduled while the loop was asking for it returned counting on it, so its work was left without a turn when the host then refused. The wake whose post is refused now hands the flag back and looks for such a thread, and the thread looks at the flag again once it has said so, so one of them sees the other and the loop asks the host again, three times at most. Work posted while the host keeps refusing, and a refusal that no other wake counted on, still wait for the next wake (`HostLoop::wake()` or any post once the host takes posts again); `host_loop.h` says so.
  - **Tests.** `ManualExecutor` takes `post` from any thread, runs a task with no lock held, and reports `in_thread()` for the thread that runs the task only; `tests/test_host_loop.cpp` uses it instead of a copy of its own. New tests post from several threads while the host pumps (also with one handler a turn), wake from several threads (one turn is queued at a time), post while a turn runs, finish a blocking tool on the pool and resume its coroutine on the host thread, destroy the loop with work queued by other threads and with posts racing the host's turns, and post from a frame that the loop's close destroys. Under ThreadSanitizer the three tests above, the rest of `ProgramHostPumpedStores.*` and `ProgramRuntimeExecutorTest.*`, and `HostLoop.*` report nothing now.
- **A pool tool that completes after the host loop is destroyed no longer touches the loop (#605).** A tool that blocks a thread, a process tool and a synchronous checkpoint store run on a pool of the process and hand their result back by posting to the executor of the run that awaits them. The host cannot join those pools, so it could destroy a `HostLoop` while such work was still running. The pool thread then posted to the destroyed `io_context` and released a work guard on it, and AddressSanitizer reported a heap-use-after-free in `io_context::execute` and in the guard's destructor. The coroutine that awaited the work was also owned by the pool thread, so it would have been destroyed there, with its timers and strands bound to a context that was gone.
  - **How.** `detail::LoopHandoff` (`src/core/loop_handoff.h`) is used only when the awaiting executor belongs to a `HostLoop` (`async::is_host_pumped`); executors that no `HostLoop` runs keep their direct pool post without a `LoopHandoff` or an extra work guard. For a `HostLoop`, `LoopHandoff` owns the continuation of the awaiting coroutine, and the executor it is posted to, for as long as the pool thread works. Storage for abandoned continuations is reserved when a handoff is tracked so taking or setting aside a continuation does not allocate. The pool thread posts through it with a lock of the loop's held, and the loop's destructor takes, with the same lock, the continuations that were not posted yet (or whose post could not be queued) and destroys them on the host's thread before it destroys the `io_context`, as it destroys every other coroutine that has not finished. A pool thread either posted before that, and its handler is destroyed with the loop's other queued handlers, or finds the continuation gone and touches nothing of the loop. Once a handoff is queued or set aside (when `asio::post` fails or the handoff is destroyed without `finish()`), a wake is requested unconditionally (so a completion posted to a strand that already had a handler queued after an earlier refused wake still wakes the loop, and an abandoned continuation on an idle loop still gets a turn to destroy its frame on the host thread) and runs after the lock is released, whole (the claim of the turn, the host executor's `post` and the re-asks after a refusal), so no host code runs under the lock. The destructor waits for any such pool wake that is already in progress before returning, so the host executor's `post` is never called after `~HostLoop()` has returned. The destructor then releases the loop's reference to the host executor on the host's thread, so a pool thread that finishes later never destroys the host executor. The single-thread WebAssembly build compiles the same code, which no pool thread reaches there.
  - **What the caller of an abandoned run sees.** Nothing: the `done` handler of `spawn` or `co_spawn` is destroyed without being called, as for any run that is in flight when the loop is destroyed, and the result that arrives later is dropped. Nothing is published for the run after the loop is gone. A `ResourceLease` or `HostResourceLease` acquired for a pool tool call is moved into the pool task and held until the pool work itself finishes, so another run cannot enter the same non-reentrant tool concurrently while the abandoned call still runs on the pool; a `SingleFlight` leader whose coroutine frame is destroyed publishes an ordinary failure to its waiting followers instead of leaving them waiting until timeout. The tool is not stopped and what it did is not undone, and the `Tool` object, or the store, and whatever else the call uses must stay alive until it returns: the library keeps no reference to a `Tool` that it was given by reference.
  - **Contract.** `include/neograph/async/host_loop.h` keeps the rule that the loop must outlive the posts of threads the host owns, and states that it does not apply to these pool completions, that only `HostLoop` executors use the handoff, that `host->post` is never called after `~HostLoop()` returns, and how leases and `SingleFlight` followers behave when a call is abandoned. It also states that the host executor's `post` must queue the task and return without running it and without waiting for the host's thread.
  - **Tests.** `HostLoopPoolWork.*` in `tests/test_host_loop.cpp` destroy the loop while a blocking tool, a process tool and a synchronous checkpoint store run (also on a strand of the loop, and next to a second loop that stays), check that the awaiting coroutine is destroyed on the host's thread and that the late result schedules no turn, deterministically exercise both delivered and taken outcomes as well as 150 racing rounds when pool tools finish as the loop is destroyed, destroy the loop while the pool thread is inside the host executor's `post` (verifying that no lock of the loop is held during `post` and that `~HostLoop()` waits for the in-progress wake so no `post` happens after `~HostLoop()` returns while not waiting for a running tool), refuse the pool thread's posts (and wake a strand whose earlier wake was refused), verify that an abandoned exclusive tool holds its lease until the pool work finishes and an abandoned `SingleFlight` leader fails its followers, verify that a handoff destroyed without `finish()` or whose `asio::post` fails on an idle loop schedules a turn that destroys its frame on the host thread (releasing any held lease and failing any `SingleFlight` followers) and that `~HostLoop()` waits for its wake, and verify that native executors do not open a `LoopHandoff`.
- **A `HostLoop` no longer loses a timer that the host reaches before Asio dispatches it (#535).** A turn that polled after a noted deadline and found no handler ready dropped the deadline, on the assumption that Asio had dispatched every timer due by then. Asio decides that, and the host's timer can run first: Asio's Windows reactor learns from a thread of its own that a timer is due, so a poll made at the deadline can find the timer not yet dispatched, and a host's clock can run ahead of Asio's. The turn dropped the deadline and left the loop idle with the wait pending, so a run that waited on a retry backoff or any other `HostTimer` never ended. `HostLoop.ARetryBackoffWaitsOnTheHostsTimer` and `HostLoop.AHostTimerArmedAfterADelayFiresOnTheHostsTimer` failed intermittently in the Windows CI job.
  - **A deadline stays noted until Asio has dispatched its timer.** The loop waits on a timer of its own at the earliest deadline and drops the deadlines up to it when Asio dispatches that one. A turn that ran before then looks again after 1 ms. A deadline that is already due on the host's clock when it is noted is kept the same way; it used to be dropped.
  - Nothing changes in the API. On a host whose timers fire at their time, the loop does what it did. The regression tests give the manual executor a clock that runs ahead of Asio's; they failed on the previous engine on Linux and pass now.
- **A Program resumed during a chunked Google upload starts the upload again or is refused with a typed error, and no record holds the upload's session URL (#541).** A Google resumable upload names its session by a URL that lets anyone write the file without the API key. SchemaProvider hands it to the live call only (`Completion::secret`) and keeps it out of the model and the raw events. A journal therefore gave a resumed Program the call that opened the session without its URL, and the next part was refused when it was prepared (`InvalidRequest`, nothing sent), with nothing to say that the upload had to be sent again.
  - **No record holds the URL.** A test runs journaled uploads from a file, from memory and in parts through a loopback Google peer, also when a part fails, and scans every byte of the Program, checkpoint and provider-call databases (with their write-ahead logs), the results and the failure text for the URLs, the API key and the file's path. It found none: `provider_codec::encode_outcome` does not encode `secret`. The test guards that.
  - **The journal decides at the call that opens the session.** When the answer to a `files::Start` carries a secret, the row now records its identity (the new column `withheld_secret`: a domain-separated SHA-256, never the secret). A resume that finds such a row, or an opening whose answer is not known, looks at the calls of the same node task after it, in any state:
    - **None.** Nothing was written into the session, and opening a session charges nothing, so the call is sent again and opens a new session. The row is not rewritten, and the handle carries the notice `P_PROVIDER_UPLOAD_RESTARTED` with the witness `{call_id, node, reason}` (`session_not_stored`, or `outcome_unknown` when the process died with the request on the wire; the latter was `NG.EffectUncertain`). The restart is appended to a table of its own, `ng_program_provider_call_restarts`, in the transaction before the call is sent (`SQLiteProgramProviderCallJournal::restarts()`: the attempt, the reason, and the state and outcome digest of the row it replaced), and `inspect()` reports it as `Record::restarts` and `Record::superseded`, so a `Dispatched` row that was restarted no longer reads as uncertain.
    - **One or more** (a part that was sent, failed or finished the file). The resume fails before anything is sent or journaled with the new `ProgramProviderUploadResendRequiredError` (`call_id()`, `later_call_id()`). The parts' call positions name the first session by the digest of its URL, so no other session can take them. In a Core node it is `NG.ConfigError`, never retried and never routed. The upload is sent again from a new run, or by a node that catches the error and uploads at call ordinals the journal has not used. A finished upload is refused too: its file exists at the provider, but a resumed node cannot replay a part without the session's URL.
  - **`reconcile_success` and the opening.** The row of a call that opens a session carries a marker (the column `opens_upload`, `Record::opens_upload_session`). `reconcile_success()` refuses a restarted opening, since a resume restarts or refuses an upload by itself, and an opening whose completion names neither an upload id nor a URL: either would give the row an answer without the URL, the next resume would replay it as a success, and the next part would fail with `InvalidRequest`. A completion that carries a URL is accepted without storing it, and the row says that its answer is short of it. A row written before the marker has none of this: an opening journaled by 0.16.0 replays without its URL, as it did.
  - **Unchanged.** OpenAI's upload names its session by an id that the journaled answer holds, so a resumed Program replays its calls and goes on. A fork opens a session of its own and sends its parts.
- **The Program stores agree where they differed, and each difference now fails closed (#556).** The shared transition validator kept four differences between the in-memory, SQLite and PostgreSQL stores. Three now follow the contract; the fourth is a stated limit of the in-memory store.
  - **A malformed expected head is refused before stored state is read, by every store.** A head that is not a SHA-256 identity cannot match a stored head, so `compare_publish` answers `Conflict` without reading. The in-memory store did this; the SQL stores reached the same `Conflict` at the head comparison, after reading the run, so a stored record that could not be read made them throw instead.
  - **A stored command row whose coordinate column disagrees with its bytes is corruption, in every store.** The PostgreSQL store threw `std::invalid_argument` for it. The SQLite store answered `Conflict` to a publication, as if a caller had been stale, while a read of the same row threw. Both stores now throw `std::invalid_argument` from the publication check as well.
  - **The SQLite store checks the identity columns that the PostgreSQL store checks.** A stored generation, a lineage head and its history, a graph migration capsule and an execution lease are returned only when the owner, the key and the identity column agree with the record in their bytes; otherwise the read throws `std::invalid_argument`. The generation id was the case the conformance suite skipped for SQLite. Databases written by 0.16.0 or earlier have agreeing columns and read as before.
  - **Stored context history is checked by the SQL stores only.** This stays: the in-memory store keeps typed values that it validated as it stored them and has no stored bytes to check.
- **A failed Python tool no longer sends its traceback to the provider (#545).** When a Python tool raised, the tool result the model read held the exception's whole traceback, with the absolute paths of the host's files and the frames of its code, and that text went to the provider. The result now holds the exception's type and message, such as `ValueError: bad input`. The traceback is written to the `neograph_engine.tools` logger at WARNING, with the exception as `exc_info`, so the host keeps it in its logs; where logging is not configured, Python's last-resort handler prints it to stderr.
  - **Behavior change: the error text of a failed call is bounded for every tool.** `ToolExecutionPolicy::error_limit_bytes` (`ToolExecutionPolicy.error_limit_bytes` in Python) is the most bytes the `error` of a failed call carries: 2048 by default, 1 byte to 1 MiB. Longer text is cut on a code-point boundary and ends with `... [truncated]`.
  - The C++ tool path was checked the same way. A C++ tool's error is its exception's `what()` and nothing else, so it had no traceback to leak, but it had no bound either: an exception that embedded a response body went to the provider whole. It has the bound now, like an MCP or gRPC tool's error.
- **wasm32 fixes (#535).**
  - The managed-budget journal draws its nonces from the SDK's entropy source (`sp::crypto::random_bytes`) instead of per-platform calls. A WebAssembly build had none, so a managed model-token budget could not start there.
  - **Behavior change: the deadline-clock refusal has a type.** A managed budget with a deadline on a host without a recoverable clock origin (every host but Linux, WebAssembly among them) throws `ManagedBudgetDeadlineClockUnavailable`, which is a `std::runtime_error` and a `ConfigurationFailure`, instead of a plain `std::runtime_error`. The message is unchanged. A managed budget without a deadline runs there.
  - **Native Program controls on wasm32.** The C v1 plugin ABI keeps its 64-bit layout. wasm32 pads `neograph_program_native_invoke_request_v1` to 32 bytes where the layout ends at 28, which stopped `src/program/native.cpp` from building. `NativeControlBinding::create` now throws `NativeControlUnavailable` (a `ConfigurationFailure`) there, and `program::NATIVE_CONTROLS_AVAILABLE` says so. Nothing changes on other targets.
  - **A synchronous checkpoint store in a single-thread build no longer hangs.** The adapter that runs a synchronous store's operations on a pool of worker threads waited for a thread that a single-thread build never starts; the run never returned. The operation now throws `SynchronousCheckpointStoreUnavailable` (a `ConfigurationFailure`). Threaded builds are unchanged.
  - **The in-memory checkpoint store runs its managed-budget operations on the calling thread.** `InMemoryCheckpointStore` ran a lease, a provider call's claim and settlement, a checkpoint publication, a release and a fork on that pool, while its other operations already ran in place. They run in place too, so a run with `model_token_budget` works in a single-thread build, and in a threaded one it no longer waits for a worker thread. A subclass that overrides the synchronous forms still goes through the pool.
- **A resumed Program run with a JavaScript `main()` that ends before its Core call has started ends in the state it was given, not `failed` with `P_TRANSITION_CONFLICT` (#557).** A resume of an interrupted `ng.callCore` withholds the wall time, model tokens, money and Core steps that the call may use, as a reservation in the journal, and the call settles the reservation with the usage it measured. A run that was cancelled before the call had started (a `cancel()` right after the resume, or an `onEvent` of the JavaScript module that throws, which cancels the run) had settled nothing, and the terminal state it published took the whole grant of the attempt as its remaining budget. That gives the reservation back without measured usage, which the transition validator refuses: the publication failed, the run ended `failed` with `P_TRANSITION_CONFLICT`, its record stayed `running`, and `reconnect` recovered it as a run that was still running.
  - **Now** such a run publishes its terminal state with the remaining budget the resume left it, and the reservation is not given back. Nothing is released without usage that the journal has verified, and the validator is unchanged. The run ends `cancelled`, or `failed` with the code of its own error, the stores keep it terminal, and `reconnect` returns that result. **Behavior change:** the `remaining_budget` of such a run no longer includes the wall time, model tokens, money and Core steps that the resume reserved for the call.
  - **A handler that throws at the first event of a resume** (`Started`, delivered before the attempt exists) ended through a branch that returned `AmbiguousEffect` without marking the pending effect ambiguous. The result was refused (`P_RUNTIME_INTERNAL`, "Ambiguous Program result requires one ambiguous pending effect") and the record stayed `running`. It now ends `failed` with `P_EVENT_SINK`, as the resume of a Program without a JavaScript `main()` does, and with the remaining budget the resume left.
  - **Unchanged.** A run that ends once the call has started, at its first event or later, settles the reservation with the measured usage as before and gets back what the call did not use.
  - **Tests.** A resumed run is ended before its Core call has started, and at the first event of the call: with the attempt held at its first load of the checkpoint, in memory and on SQLite (`ProgramRuntimeTest`), on a host loop that the test pumps (`ProgramHostPumpedStores`), and in the JavaScript module (`wasm/test/program_test.mjs`). The runs that end before the call has started failed on the previous engine with `P_TRANSITION_CONFLICT` and a record that stayed `running`; the run whose handler throws at `Started` failed with `P_RUNTIME_INTERNAL`.
- **Program: a run that holds the reservation of a JavaScript command ends terminal when it ends before the command has run (#557).** A run with a JavaScript `main()` holds the wall time, model tokens, money and Core steps of a command as a reservation in its journal while the command runs: after a resume of an interrupted `ng.callCore`, and after a process stopped inside a command, which `reconnect()` recovers. The grant of an attempt that starts that way includes the reservation. The entry above covers the runs that end in the attempt of a resume. Four other ends came before the command had run and either published the grant as the remaining budget, which returns the reservation without measured usage and which the transition validator refuses, or built a result that no store accepts. The publication failed, the run ended `failed` with a code that named the failure itself, and its record stayed `running`, so a later `reconnect()` ran the run again.
  - **A resume of an interrupted `ng.callCore` whose Core checkpoint is gone, or is no longer the one the journal published, when its attempt reads it** (the stores of a restarted process that do not agree) ended `failed` with `P_TRANSITION_CONFLICT`. It now ends `checkpoint_incompatible` with `P_CHECKPOINT_INCOMPATIBLE`.
  - **A run that resumed an interrupted `ng.callCore` and ended while its attempt waited for host capacity** (`RuntimeConfig::host_admission`) ended `failed` with `P_TRANSITION_CONFLICT` when it was cancelled, when the host refused it, and when the host's `host_admission_resolver` threw. It now ends `cancelled`, `failed` with `P_HOST_ADMISSION`, and `failed` with `P_RUNTIME_CORE_FAILURE`.
  - **`reconcile()` with `Completed` on a paused `ng.callCore`, whose handler throws at `Started`,** ended through a branch that returned `AmbiguousEffect` without marking its pending effect ambiguous, as the resume did before the entry above. The result was refused (`P_RUNTIME_INTERNAL`) and the record stayed `running`. It now ends `failed` with `P_EVENT_SINK`.
  - **A run that `reconnect()` recovered after a process stopped inside a command, and that ended before the recovered attempt reached the command** (cancelled, or ended by an error of its own), ended `failed` with `P_TRANSITION_CONFLICT`. It now ends `cancelled`, or `failed` with its own code. The command may have been sent, and nothing has measured what it used: the run is not asked to reconcile the command, and the reservation is not returned.
  - **Now** these runs publish their terminal state with the remaining budget the journal holds beneath the reservation, as the entry above does, and the reservation is not given back. Nothing is released without usage that the journal has verified, and the validator is unchanged. The stores keep the run terminal and `reconnect()` returns that result. **Behavior change:** the `remaining_budget` of such a run no longer includes the wall time, model tokens, money and Core steps that the reservation held.
  - **Unchanged.** A run that ends once its command has started, at its first event or later, settles the reservation with the measured usage and gets back what the command did not use, including a resumed run that the host admitted and that was then cancelled.
  - **Tests.** Each end, with the attempt held before it began, in memory and on SQLite (`ProgramRuntimeTest`); all but the recovered run also on a host loop that the test pumps (`ProgramHostPumpedStores`). On the previous engine each failed, with `P_TRANSITION_CONFLICT` and a record that stayed `running`, and the reconciliation with `P_RUNTIME_INTERNAL`.
- **The A2A client reads a reply as an event stream only when its `Content-Type` says so.** It took any reply whose body contained `data: ` for a server-sent event and parsed the text after it, so a JSON-RPC reply whose task or `history` held that string, the message of the request echoed back included, failed with `A2A response not valid JSON`. A reply is now an event stream when its media type is `text/event-stream`, and its message is the first `data:` event; every other reply is JSON. **Behavior change:** a server that sends events without that content type is no longer read as a stream.
- **The MCP stdio client skips text on the server's stdout that is not JSON.** One line there that was not a JSON-RPC message, such as a startup banner or a log line, ended the session and failed every pending call, although the specification puts the duty on the server. The session now skips a line that is not JSON, up to `StdioClientConfig::max_stdout_text_lines` (1000) lines and `max_stdout_text_bytes` (256 KiB) per session, and writes one warning to the host's stderr, without quoting the text. The line past a limit ends the session with `MCPFailure::protocol`, and 0 for a limit skips nothing. A line that could be a damaged message is never skipped, since the call it answers would wait for its timeout: one that starts with `{`, or with `[` and then `{`, or mentions `"jsonrpc"`, and JSON that is not an object or a batch, end the session as before.

### Security

- **The Python package requires certifi 2026.10.19 or later ([GHSA-6q48-6c7w-9gw2](https://github.com/advisories/GHSA-6q48-6c7w-9gw2)).** On import, `neograph_engine` points `SSL_CERT_FILE` at certifi's CA bundle unless `SSL_CERT_FILE` or `NEOGRAPH_SKIP_CERT_AUTOFIX` is already set, so the native transport trusts that bundle by default. The advisory reports that eight eMudhra intermediate CA certificates chaining to the `emSign ECC Root CA - G3` root, which certifi's bundle carried, have compromised private keys. It lists certifi `>= 2019.03.09, < 2026.10.19` as affected and 2026.10.19 as the first patched release; 2026.10.18 removed every emSign root, and 2026.10.19 restored the ones that are not affected. `pyproject.toml` now declares `certifi>=2026.10.19`, so installing `neograph-engine` upgrades an older certifi, and the MCP interop test requirements pin 2026.10.19. A host that sets `SSL_CERT_FILE` itself keeps its own bundle and should check it.

## [0.16.0] - 2026-10-20

### Added
- **SchemaProvider's endpoints on `Provider` (#531).**
  - `prepare_endpoint`, `complete_endpoint` and `complete_endpoint_async` run SchemaProvider's non-chat endpoints: images, decisions, embeddings, rerank, moderation, speech, transcription and translation, video jobs, files, batches, token counts and OpenRouter generation statistics.
  - The calls run on the provider's own SchemaProvider client, which its chat calls share, and the descriptor policy admits each endpoint per origin.
  - They replace the typed clients that 0.16.0 removes.
  - **Python.** `Provider.prepare_endpoint`, `complete_endpoint` and `complete_endpoint_async` take the SDK's request classes (`EmbeddingsRequest`, `ImagesRequest`, `FileUpload`, `BatchSubmit`, `TokenCount`, ...) and return their result classes or a `ProviderFailure`. Each request class has a strict `from_dict`. `complete_endpoint_async` returns an `asyncio.Future` on the binding's one asio runtime, and cancelling it cancels the call. `batch_jsonl`, `batch_decode_line`, `files_media` and `verify_webhook` are module functions.
  - `Provider` gained a virtual member, `endpoint_runtime()`, so rebuild C++ subclasses of `Provider` against the new header (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Endpoint nodes in graphs (#531).**
  - The built-in node types `image_generate`, `image_edit`, `embed`, `rerank`, `moderate`, `decide`, `speak`, `transcribe` and `translate` make one endpoint call each. They read their inputs from channels the node's config names, and write the result to one channel: provider media JSON, vectors, rankings, verdicts, answers or text.
  - `job_submit` and `job_collect` run a video job. The collect node polls once each time it runs and interrupts the run until the job has finished.
  - The token usage a call reports is recorded on the run.
- **Endpoint calls in Programs (#531).**
  - `make_endpoint_request` puts an endpoint request in a `ProviderRequest`, and the endpoint nodes call through it. So an endpoint call takes the path a chat call takes: the provider call broker, the durable dispatched marker, the SQLite journal, and replay on resume and fork. A Program registers the nodes with `RegistrySnapshotBuilder::add_core_endpoint_node`.
  - A model-token budget refuses an endpoint call before it is sent, since it has no output cap. A priced binding refuses it with `P_MONEY_UNMODELED`, because the model catalogue cannot price an endpoint request yet (fox1245/SchemaProvider#124).
  - An endpoint call's dispatch identity also names its endpoint, method and URL. A chat call's identity does not gain these members.
  - An upload's dispatch identity also names its file name, MIME type, purpose and expiry, and the SHA-256, offset and size of the bytes it sends, but not the file's path (#541). Before, an upload read from a file had the same identity whatever its file, name or purpose, and an upload from memory carried the SDK's random multipart boundary in its body, so its identity differed on every preparation. A file that is missing or shorter than its slice is refused when the call is prepared. The journal also no longer rejects the row of a call that names no model, such as an upload, as corrupt when a resume loads it.
  - An endpoint read (a GET: a job poll or download, a file or batch lookup) is never charged and is safe to send again, so the journal sends it each time (`PreparedProviderRequest::is_endpoint_read()`). A Program paused on a running video job therefore sees the job finish when it is resumed, instead of replaying the first poll.
  - The Program provider wrapper and `OpenInferenceProvider` forward their inner provider's endpoints (`Provider::endpoint_runtime_of`).

### Changed
- **Every SchemaProvider interface 7 request control is reachable from C++ and Python, in a nest per family (#512, #513).** `ProviderControls` gains `chat`, `responses`, `messages`, `generate` and `interactions` (`ChatControls` ... `InteractionsControls`), which hold the family's own controls in the SDK's types; the flat members stay portable.
  - **New controls.** Chat modalities, audio output, OpenRouter image output and plugins; Responses hosted tools (web search, image generation, file search, tool search, shell, code interpreter, MCP, computer use, OpenRouter server tools and policy-admitted tools), output modalities, image output, plugins, include names and MCP approvals; Messages server and typed client tools, container, skills, betas and MCP servers; Generate response modalities, speech, image output, audio transcription, audio format, built-in tools, MCP servers, server tool invocations and retrieval; Interactions response formats, speech, transcription, built-in tools, `previous_interaction_id`, `video_task`, `agent` and `background` (the portable `store` now applies to Interactions too). Each nest has `extensions`, top-level members the SDK policy admits for the origin.
  - **Behavior change: a family takes only its own nest.** A graph whose nodes call different families can share one `ProviderControls`; the other families' controls are ignored. A portable control the family cannot express still throws.
  - **Deprecated: the flat family members** (`chat_reasoning`, `include_reasoning`, `usage_include`, `models`, `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity`, `truncation`, `responses_include`, `thinking_mode`, `output_effort`, `cache_control`, `messages_tool_choice`, `gemini_history_mode`, `gemini_thinking_level`, `safety_settings`, `gemini_tool_choice`). They move into their nests in 0.16.0, another family refuses them as in 0.15, setting one and its nest member throws, and 0.17.0 removes them.
  - **Python.** The nests and every SDK type they hold are bound, `MessagesOutputEffort` gains `XHigh`, and each controls class has a strict `from_dict`: unknown keys are refused, enums take their member names and tool or format variants take a `"type"` (`neograph_engine.provider_controls`).
- **Program prices come only from the SchemaProvider model catalogue of the provider's policy (#520).** Route ceilings, tier rates and tool fees are catalogue units and layers, not a NeoGraph price file.
  - **Behavior change: the host supplies prices.** The embedded SchemaProvider catalogue lists none, so a budgeted run refuses every priced call until the provider's policy carries a catalogue layer that prices the model: `sp::descriptor::load_policy(..., catalogue)` or `with_catalogue`, and in Python `load_provider_policy(..., model_catalogue_json)`. An entry's `pricing` gives decimal USD per unit (`input`, `output`, cache, reasoning and image units), OpenRouter's route ceiling (`ceiling_input`, `ceiling_output`), service tiers (`input/flex`), long-context bands (`input+200000`) and server-tool fees (`tool:<type>`).
  - **The catalogue decides what is refused.** A service tier needs its own units. OpenRouter fallbacks (`models`) need every candidate priced, and the hold is the dearest candidate's. A server or hosted tool needs a fee (`"0"` declares a free one) and a use cap (Messages `max_uses`, Responses `max_tool_calls`). A long-context band is reserved when the model's input limit reaches it. OpenRouter plugins and Gemini's built-in tools, which take no use cap, are refused on a budgeted run.
  - **Client tools bill only tokens (#540).** A Messages tool the policy marks as a client tool (`memory_*`, `bash_*`, `text_editor_*`, the computer and browser toolsets) is run by the caller, so a priced request that carries one needs neither a use cap nor a fee. A server tool whose type admits no `max_uses` (`mcp_toolset`, `tool_search_tool_*`, `code_execution_*`) is still refused, and the refusal says so. The wire types of Responses hosted tools and Gemini built-in tools are matched exhaustively, so a tool that SchemaProvider adds later fails the build instead of being priced as `file_search`.
  - **Settlement.** Without a reported cost, `sp::pricing::estimate` prices the call from the catalogue snapshot its hold was priced with, and reserved server-tool fees are charged at their caps.
  - **Behavior change: records.** `ProgramMonetaryRecord::origin` is the provider's base URL (`https://openrouter.ai`, was `openrouter`), `catalog_version` is `sp-model-catalogue/1` with the catalogue's digest, and the new `pricing` (Python `record.pricing`) holds the entry, its price rows, the service tier, the reserved tool uses and the estimate's assumptions. Records journaled before 0.16.0 keep their bytes.
  - **Behavior change: `P_MONEY_UNKNOWN_ORIGIN`** now means only a provider without an admitted descriptor. A model the catalogue does not price at the base URL is `P_MONEY_UNKNOWN_MODEL`.
- **Adopt SchemaProvider interface 7 (#512).** NeoGraph builds against interface 7 headers and the generation 7 libraries (`libsp_*.so.7`). The build pins SchemaProvider 0.4.0, the release commit `f47548fed63038d926aad7dc840bfaf701a8f1fc`, and an installed SDK must be exactly 0.4.0 (`find_package(SchemaProvider 0.4.0 EXACT)`).
  - **Behavior change: typed media.** `sp::Image` is `sp::Media`: an image, audio, video or document, inline or at a URL or provider file. `ChatMessage::image_urls` still projects inline images as data URLs, and now also projects URL images; `portable_message` sends an https entry as URL media for the provider to fetch. Generated images (Chat `message.images`, Responses `image_generation_call`, Gemini `inlineData`) are `Media` parts instead of `Opaque` ones; their wire items stay in the native output.
  - **Provider outcome format v3.** `neograph.provider-outcome/v3` and `neograph.provider-message/v3` (`schemas/*-v3.schema.json`) add media parts, text citations, typed tool result content, the error `reason` and the served `model`. Records in the v2 formats (Program journals, checkpoints, runtime history) are still read: an image part becomes inline image media and the new members are empty. New records are written in v3, which NeoGraph 0.15 cannot read.
  - **Runtime history records** are storage version 3. A version 2 record is read with its stored bytes and id, so a feed's predecessor chain is unchanged. Program provider journal rows keep the digest domain they were written under.
  - **Behavior change: Program journals from 0.15 do not resume (#543).** A provider call's dispatch identity includes SchemaProvider's policy identity, which interface 7 changes. A run journaled by 0.15 therefore cannot be resumed by 0.16: the journal reports a logical call binding conflict. A fork of such a run does not adopt its calls and sends them again, so calls already paid for are paid again. Finish or abandon such runs before upgrading.
  - **Behavior change: native history from 0.15 does not replay.** Interface 7 changes every policy identity, so continuing native history sealed by 0.15 is `ReplayIneligible`. Portable history is not affected.
  - **Model facts are catalogue data.** The descriptor policy has no `models` list; model limits and per-model defaults are entries of the SDK model catalogue (`sp::descriptor::load_catalogue`, `merge_catalogues`, `DescriptorPolicy::with_catalogue`). The cookbook chatbot takes `--model-catalogue PATH` and ships `demo-models.json` instead of `demo-policy.json`, and a session binds the policy's content digest, catalogue included.
  - **Python.** `ProviderMedia` replaces `ProviderImage`: `ProviderMedia.image(...)`, `.audio`, `.video`, `.document`, `.url` and `.file`, with `kind`, `source`, `mime`, `data`, `reference`, `name`, `detail`, `id` and `transcript`, and the enums `ProviderMediaKind` and `ProviderMediaSource`. New: `Text.citations` (`ProviderCitation`), `ProviderToolResult.content_parts`, `ProviderError.reason`, `ProviderCompletion.model` and `ProviderPartialCompletion.model` (`ProviderServedModel`, `ProviderServedRelation`), `ProviderPartHeader.media`, `ProviderPartSeal.citations`, `ProviderStopKind.Pending` and `Compaction`, `ProviderPartKind.Media` and `ProviderCostRounding.CeilingCatalogueProduct`. `load_provider_policy` takes an optional `model_catalogue_json` layered over the embedded catalogue, and `provider_model_catalogue_json()` returns the embedded one.
  - Rebuild NeoGraph, C++ consumers and Python extensions together against matching interface 7 headers and libraries.
- **Built-in graphs and nodes send the caller's model, sampling and token settings, not one vendor's (#521).** A model that refuses `temperature` (`gpt-5*`, `o3`, Claude 4.7 and later) failed at prepare in every step of the deep research graph, in the intent classifier, in the history summarizer and in the Harness provider worker.
  - **Behavior change: `DeepResearchConfig::model` is required.** It defaulted to `claude-sonnet-4-5`; `create_deep_research_graph` now throws `std::invalid_argument` when it is empty.
  - **Deep research settings are configuration.** `DeepResearchConfig::controls` holds what every step sends (only the members that are set). Each step's output cap is a field (`brief_max_output_tokens`, `clarify_…`, `supervisor_…`, `researcher_…`, `compress_…`, `report_…`, with the old values as defaults), and so are the largest cap the empty-answer retry asks for (`max_output_budget`, 16384) and the node retry policy of the model steps (`retry`, as before: twice, after 15 s and 30 s; the brief and clarify steps now take it too). No step sends a temperature of its own.
  - **Behavior change: the brief and clarify steps report a provider failure.** They caught it and went on with the raw query, so a wrong model id or a refused parameter degraded the run without a trace.
  - **Behavior change: no fixed temperature or classifier cap.** The intent classifier sends the node context's `provider_controls` instead of `temperature = 0` and `max_output_tokens = 20`, which a reasoning model spends before it answers. `default_summary_controls()` keeps its 500-token cap and no longer sends `temperature = 0.2`. The Harness provider worker no longer sends `temperature = 0`.
  - **Harness translation ceilings are the host's.** `HarnessTranslationDefaults` refused a `provider_timeout_seconds` over 600 and a `max_output_tokens` over 128,000. The host's values are now the ceilings, up to 86,400 seconds and 2^32 − 1 tokens.

### Removed
- **The compiled price catalog (#520).** `data/pricing/catalog.json`, the internal `PriceCatalog` (`src/program/pricing.h`), `scripts/check_price_drift.py` and the weekly `Price drift` workflow are gone; prices are catalogue data the host supplies. `tests/fixtures/pricing/model_prices.json` keeps the 13 entries as a catalogue layer for the tests.
- **The typed `ImagesClient`, `VeoClient` and `DecisionsClient` (#510).** They were deprecated in 0.15.1. Images, video and decisions are SchemaProvider endpoints: call them with `Provider::complete_endpoint` (#531). Their headers (`neograph/llm/images_client.h`, `veo_client.h`, `decisions_client.h`, `endpoint_types.h`), the shipped configurations (`schemas/typed-*-client.json`), `scripts/sync_model_catalogues.py` and their tests are gone. The live suite drops its four typed-client cases.

### Fixed
- **Small fixes from the 0.16.0 review (#542).**
  - **Native history records validate against their schema.** `schemas/runtime-history-record-v3.schema.json` required an `spna2:` archive reference, but the SDK writes `spna3:`, so every native record failed the schema. The v2 schema had the same pattern. A test now validates a record written with a real archive.
  - **Portable JSON is v3.** The README, the ABI policy, the reference and the example READMEs said portable JSON "remains v2". NeoGraph writes `provider-message-v3`, `provider-outcome-v3` and `runtime-history-record-v3` and still reads their v2 records; the native archive stays `spna3`.
  - **An endpoint node registered in a Program has no effect contract (#531).** `add_core_endpoint_node` declared an empty effects object, which Program compilation counted as a node that reads and writes nothing, and it sealed warnings about the node's channels (E6) into the bundle. It now declares none, as the same node does in a Core graph, so the validator skips the analysis.
  - **gRPC `Health` reports the library version.** It read a macro the build never defines, so it always answered `dev`.
  - **Python exports `provider_model_catalogue_json`.** The native module had it, `neograph_engine` did not.
  - **The cookbook chatbot does not branch on a model name.** The low reasoning effort of its default model is now a model-catalogue entry, `live-models.json`, which is passed with `--model-catalogue`. Without that entry a call sends no reasoning effort.
  - **Reference.** It describes the call path of the endpoint nodes as it is (`make_endpoint_request` through `invoke_provider`), and the `P_MONEY_UNMODELED` table lists endpoint calls.
- **A resume with a value no longer breaks the next model call after a gated tool call (#544).** A graph with a `messages` channel receives the value of `resume()` there as a user message (#94). When the run had paused on a tool gate, or on an `interrupt_before` pause of the tools node, the history ended in an assistant turn whose tool calls had no results yet, and the message landed between the calls and the results the tools step then wrote. Every provider family requires a result to follow its call, so the next model call was refused locally with `InvalidRequest` and nothing was sent. The engine now leaves the value out of `messages` while the history holds client tool calls without results, the test that the `has_tool_calls` route and the tools node use. The gate still reads its answer from `ToolGateContext::resume_value` and a node from `ctx.resume_value`, and a graph with a `messages` channel still receives the value as a user message at any other time.
- **A failed or refused tool call no longer makes the next Responses call fail locally (#544).** NeoGraph marks the result of a failed call as an error, and the Responses wire has no field for that: SchemaProvider refuses a request that carries one, with `Unsupported`. A gate's denial, a tool that threw, an unknown tool and an expired call each ended the run at its next model call. The request now sends the failure as the result's text, which already holds the error, the status and whether the call can be retried. Other families are unchanged.

## [0.15.1] - 2026-10-19

### Added
- **Releases pass a live end-to-end suite with real provider keys (#522).** Local tests replay recorded traffic and cannot show that a provider still accepts what NeoGraph sends; 0.15.0 shipped two streaming defects that one real call shows. `bindings/python/tests/live/` and `tests/live_e2e_test.cpp` cover the eight family presets buffered and streamed, vision, tool calls, generated media, the Anthropic Messages controls, typed failures, the C++ adapter and the typed endpoint clients, with the known gaps of the release line as strict xfail. `scripts/live_e2e.py run` writes `tests/live/report.json` for a clean commit. A release pull request commits it, and the `Live e2e report` workflow and the Wheels release tag check it before anything is published (see [`tests/live/README.md`](tests/live/README.md)).

### Changed
- **Adopt SchemaProvider `060f52c9ab0fbbc6b785b097785a17bdf16d7773`, SDK 0.3.5 on `maint/interface6` (#511).**
  - **Compatibility.** The interface revision and shared-library generation stay 6, no public struct or enum changes layout, and the embedded descriptor policy and its identity are unchanged. NeoGraph now requires `SchemaProvider 0.3.5 EXACT`, in its build and in the `find_dependency` of the installed `NeoGraphConfig`.
  - **Behavior change: generated images on Chat Completions are kept.** Image models (OpenRouter `google/gemini-3.1-flash-lite-image` and the like) return their images in `message.images`. The SDK ignored the member, so the outcome had no image although its tokens were billed. Each image is now an `Opaque` part of wire type `images`, and its `wire_metadata` holds the wire entry (the data URL is at `image.image_url.url`). A history may keep the part on its assistant message; it is not sent back to the provider.
  - **Behavior change: a Messages response with a code-execution `container` or the cache `diagnostics` object completes.** It used to fail as `Unsupported`, which threw away the billed answer. Claude 5.5 with `web_fetch_20260318` or `web_search_20260318` returns both. They stay in `raw_events`.
  - **Fix: a streamed Interactions call completes.** The API now opens an Interactions stream with an `interaction.created` resource without `status`, and the SDK refused it, so every streamed Interactions call failed, plain text included (SchemaProvider #65).
  - **Fix: a streamed generated image over 1 MiB completes.** The SSE line and event defaults are now 16 MiB instead of 1 MiB. A streamed `gemini-3.1-flash-lite-image` answer with 1.5 MB of base64 used to fail with `ResourceLimit` (SchemaProvider #66). The runtime defaults are not part of the policy identity.
  - **Python.** `Opaque.wire_metadata` is now readable, so a kept image can be read from Python.
  - **Not fixed on this line.** On models without programmatic tool calling, such as `claude-haiku-4-5`, the `_20260318` web tools need `allowed_callers`, which interface 6 cannot send; use `web_search_20250305` or `web_fetch_20250910` there. Lyria and other Gemini models without thinking still answer 400 to the family's `includeThoughts` default. Both are fixed in SchemaProvider 0.4.0, which NeoGraph 0.16.0 adopts (#512).
  - Rebuild NeoGraph, C++ consumers and Python extensions together against the 0.3.5 headers and libraries (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **The typed `VeoClient`, `ImagesClient` and `DecisionsClient` admit the models their configuration lists (#518).** Each client also refused every model that was not on a list compiled into it, so a model the provider already served needed a NeoGraph release. `DecisionsClient` admitted only `typesafe/jev-1.13` of OpenRouter's 27 decision models, `ImagesClient` refused `gpt-image-2` and `gemini-3.1-flash-image`, and `VeoClient` admitted only the three `veo-3.1-*-preview` ids. The configuration is now the only model list.
  - **Model limits are configuration data.** `ImagesClient` takes an optional `capabilities` object (`sizes`, `qualities`, `aspect_ratios`, `image_sizes`, `max_n`, `max_prompt_characters`, `min_output_tokens`, `max_output_tokens`). A `VeoClient` model entry may be an object with `id`, `resolutions` and `max_reference_images`. A limit the configuration does not declare is not checked locally; the provider judges it. The shipped `schemas/typed-images-client.json` and `schemas/typed-veo-client.json` declare the limits that used to be compiled in. **Behavior change:** without `capabilities`, `ImagesClient` no longer refuses an unknown size, quality, aspect ratio or image size before dispatch, and `VeoClient` refuses 4K or reference images only for a model whose entry says so.
  - **`DecisionsClient`.** It forwards `provider.options` (objects keyed by provider slug) instead of refusing them, and admits a model with `implicit_caching: true`. A refused configuration now names the failed check in `safe_message`. A request for a model the configuration does not list is still refused before dispatch.
  - **Catalogue refresh.** The shipped `schemas/typed-decisions-client.json` lists all 27 OpenRouter decision models. `scripts/sync_model_catalogues.py` regenerates that list from OpenRouter's public models listing, and the Veo list from the Gemini models listing when `GEMINI_API_KEY` is set. With `--check` it writes nothing and exits 1 on a difference. It never chooses a new default model.
  - No public header changes layout.

### Deprecated
- **The typed `VeoClient`, `ImagesClient` and `DecisionsClient` will not be extended, and 0.16.0 removes them (#510).**
  - **`VeoClient`.** The Gemini API offers three Veo models: `veo-3.1-generate-preview`, `veo-3.1-fast-generate-preview` and `veo-3.1-lite-generate-preview`. Google shuts all three down on 2026-10-22, so from that date the client has no model to call. Google names Gemini Omni (`gemini-omni-1.1-flash`, through the Interactions API) as the replacement.
  - **`ImagesClient`.** OpenAI removed `dall-e-2` and `dall-e-3` on 2026-05-12. It shuts down `gpt-image-1` on 2026-10-23, and `gpt-image-1-mini` and `gpt-image-1.5` on 2026-12-01. Configure `gpt-image-2` instead (#518). The Gemini profile (`gemini-3.1-flash-lite-image`) is not affected.
  - **`DecisionsClient`.** Its shipped configuration lists all 27 OpenRouter decision models (#518).
  - **What replaces them.** SchemaProvider takes over every provider endpoint: images, video, audio, embeddings, rerank, decisions and the realtime APIs. It admits models by capability profile instead of closed lists. NeoGraph 0.16.0 adopts it, deletes these three clients and serves Omni video through it.

## [0.15.0] - 2026-10-18

### Changed

- **A loopback MCP HTTP server answers only loopback and listed `Host` names (#499).** The server checked `Origin` but not `Host`, so a request without `Origin` was served whatever name it was addressed to. A DNS-rebinding page sends its own name in `Host`; its `initialize` was refused only because it also sends an `Origin`. `MCPHttpServerConfig::allowed_hosts` now lists the names the server answers: host names, IPv4 addresses and bracketed IPv6 addresses, compared without the port and ignoring case, or `*` for any. On a loopback bind (`127.0.0.1`, `::1`, `localhost`) the server always answers `localhost`, `127.0.0.1` and `[::1]`, and refuses any other `Host` with `403` (`-32003`, "Host is not allowed"), with or without an `Origin`. A request without `Host`, or with one that is not `host[:port]`, gets `400` (`-32600`). As in the official TypeScript SDK's `createMcpExpressApp()`, the port is not compared. On a non-loopback bind an empty list checks nothing, as before, and a non-empty one admits only the listed names. The check covers `POST`, `GET` and `DELETE` on the endpoint, not the public RFC 9728 metadata. An entry with a port, an IPv6 address without brackets or an empty entry is refused with `std::invalid_argument` when the server is constructed. `neograph-harness-mcp` reads the list from `NEOGRAPH_HARNESS_ALLOWED_HOSTS`, comma-separated. **Behavior change:** a loopback server behind a local reverse proxy or a tunnel that forwards the public `Host` refuses those requests until that name, or `*`, is listed. `MCPHttpServerConfig` gained a member, so rebuild NeoGraph and C++ consumers together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **A failure whose attempts are used up is no longer reported retryable, and a client's refusal of an ACP request is not retried (#500).** Every protocol host sent `FailureVerdict::retryable`, which was the node's retry verdict whether or not attempts were left. So a node that failed on each of its attempts, or a node without a retry policy that failed once, told the peer `retryable: true`, and an editor that read it as "trying again can succeed" could retry what the engine had already retried. `retryable` is now `false` once the node's retry loop has used up the node's attempts (`kind` `exhausted`). A failure the loop stopped retrying because of the run's deadline or retry ceiling keeps `true`, and so does `NG.ProviderFailed`. The new `FailureVerdict::retryable_by_class` keeps the old reading, for callers that retry outside the engine; ACP (`data`), A2A (`metadata["neograph/failure"]`), gRPC (ErrorInfo metadata) and the MCP server (`_meta["io.neograph/failure"]`) send it next to `retryable`. A JSON-RPC error the ACP client answers to a request of the agent is now an `acp::ACPClientError` with the method, the code, the client's message and `data`; its `what()` is unchanged. When the code says the request cannot succeed (`-32700`, `-32600`, `-32601`, `-32602`, `-32002` resource not found, `-32000` authentication required), it is an `acp::ACPClientRefusal`, a `ToolRequestRefusal` that is not retryable: a node whose `fs/read_text_file` gets `-32002` runs once, and its turn reports `retryable: false`, `retryable_by_class: false`. `-32603`, other codes and timeouts are retried as before. `NodeFailure::retryable`, routed failure records and Program task retries keep the class reading. **Behavior change:** a peer that read `retryable: true` for a failure whose attempts were used up now reads `false`, and a node whose ACP client refuses its request is no longer retried. `FailureVerdict` gained a member, so rebuild NeoGraph and C++ consumers together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).

### Fixed

- **A write to a child process that stopped reading no longer ends the host with SIGPIPE (#495).** NeoGraph writes to the stdin of stdio MCP servers and of process tools (`ProcessTool`). Once the child has closed its stdin or exited, such a write raises SIGPIPE, and its default action ends the whole process. The stdio MCP client blocked SIGPIPE only on its own I/O thread. That is enough on Linux, where the signal goes to the writing thread, but not on macOS, which raises it for the process: an MCP server that exited before it read `initialize` could end a C++ host, as a macOS CI run showed. The process tool bridge did not block it at all. On macOS the write end of each such pipe is now marked `F_SETNOSIGPIPE`; on Linux the process tool bridge blocks SIGPIPE on its thread around each write and takes back the one the write raised. The write then fails with `EPIPE`, and the call fails or ends as it would otherwise. The bridge also handled a full stdin pipe whose reader had gone (`POLLERR` only) by polling it again at once until the child exited, which kept a CPU core busy; it now ends the input there. Python hosts ignore SIGPIPE and were not affected, nor was a process that runs NeoGraph's A2A or MCP HTTP server, since cpp-httplib's server sets SIGPIPE to `SIG_IGN` for the process.
- **Stopping the A2A server no longer cuts a stream it has not started sending (#493).** cpp-httplib (0.41 and 0.61 alike) does not call a chunked content provider once its listener has stopped. `A2AServer::stop()` stopped the listener first, so a `SendStreamingMessage` or `SubscribeToTask` whose handler had accepted the request, but whose stream had not sent its opening Task yet, was closed without a byte of the stream: the client read `End of file` instead of the run's events, while the run went on. `GraphHostA2AContract.SharedBoundary` failed now and then this way. `stop()` now waits until every request it is handling has answered or, for a stream, has started sending it, and only then stops the listener; the stream ends with the run's final state, as streams already open did. A request that arrives during that wait gets JSON-RPC error `-32603` (reason `UNAVAILABLE`, "The agent is stopping; retry later") instead of starting work.
- **A request a tool's service refused is no longer retried when a node called the tool through the throwing facade (#502).** `ToolExecutionController::execute_async` turns a tool's `ToolRequestRefusal` (an MCP `-32602`, a gRPC `INVALID_ARGUMENT`, an `acp::ACPClientRefusal`) into a failed result and throws it as a `ToolExecutionError`, which did not say that the call was refused. The node failure classifier read it as an ordinary failure, so the node's retry policy sent the refused request again and every protocol host reported it as `retryable_by_class: true`. `ToolExecutionResult` gains `refused`, set for such a call, and the classifier treats a refused call that is not retryable as it treats the refusal itself: the node runs once. `ToolExecutionResult` gained a member, so rebuild NeoGraph and C++ consumers together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).

## [0.14.3] - 2026-10-18

### Changed

- **Adopt SchemaProvider `0437853529b5932e7097a38629a95779d08f03aa`, SDK 0.3.3 on `maint/interface6` (#496).** The interface revision and shared-library generation stay 6 and no public struct or enum changes layout. Two SDK fixes change what NeoGraph users see. **Behavior change:** a streamed text part no longer delivers a delta with empty text (SchemaProvider #37). OpenAI Chat, Anthropic Messages, Gemini and Responses streams each delivered one, which NeoGraph passed on as an `LLM_TOKEN` event, and as an `llm.token` span event, with empty text, so a consumer that counted tokens or took the first event for the first token was off by one. The text, the parts and the final result are unchanged. **Behavior change:** a `google.generate` descriptor whose buffered and streaming paths do not name the same literal model (`/v1beta/models/<model>:generateContent` and `/v1beta/models/<model>:streamGenerateContent?alt=sse`) is refused when it is loaded, with a `ConfigError` naming the path; it used to load and then fail every request with `InvalidConfig` (SchemaProvider #40). The SDK documentation now says how a deployment behind a proxy or gateway that answers with another model name declares it as a routing gateway (SchemaProvider #38). NeoGraph now requires `SchemaProvider 0.3.3 EXACT`, in its build and in the `find_dependency` of the installed `NeoGraphConfig`. Rebuild NeoGraph, C++ consumers and Python extensions together against the 0.3.3 headers and libraries (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).

### Fixed

- **The MCP HTTP server serves a request that names another supported `MCP-Protocol-Version` (#490).** After `initialize` the server answered `400` to any request whose `MCP-Protocol-Version` differed from the session's negotiated revision, including the revisions it supports (`2025-11-25`, `2025-06-18`, `2025-03-26`, `2024-11-05`). MCP 2025-11-25 asks for `400` only for an invalid or unsupported revision, and the official TypeScript server accepts any supported one; the conformance suite's `server-sse-multiple-streams` scenario sends `2025-03-26` on a `2025-11-25` session and got three `400` answers. Such a request is now served under the revision the session negotiated: batches, tool fields and the rest follow the session, not the header. The first such request of a session writes one warning to stderr. An unsupported or malformed value is still `400` (`-32600`, now with the message "MCP-Protocol-Version is not a supported revision"), and a request without the header still uses the negotiated revision. **Behavior change:** a client that sends another supported revision is served instead of refused.
- **On a loopback bind, the MCP HTTP server admits its own loopback origin by default (#491).** A request that carried an `Origin` was refused with `403` unless `allowed_origins` listed it, and the list is empty by default, so a loopback server refused even its own origin. The conformance suite's `dns-rebinding-protection` scenario sends `initialize` with the server's own `Host` and `Origin` and expects success; it failed with the defaults. On a loopback bind (`127.0.0.1`, `::1`, `localhost`) the server now also admits `http://localhost:<port>`, `http://127.0.0.1:<port>` and `http://[::1]:<port>` at the port it is bound to, compared exactly as browsers send them, and writes a line to stderr the first time it admits each one. A DNS-rebinding page sends its own origin and is still refused, as are loopback origins on other ports, `https` ones and `null`. A page on another localhost port, such as a browser inspector, still has to be listed. Non-loopback binds admit only listed origins, as before. **Behavior change:** on a loopback bind, a request whose `Origin` is the server's own loopback origin is served instead of refused with `403`.

## [0.14.2] - 2026-10-18

### Changed

- **Rebuild C++ consumers: `WsClientOptions` gained members, `A2AServer` and `WsClient` gained functions (#456, #458).** `neograph::async::WsClientOptions` gained `handshake_timeout` and `read_timeout`, so its layout changed (it is passed by value to `ws_connect`); `A2AServer` gained `set_trust_forwarded_headers`, and `WsClient` gained private member functions. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Adopt SchemaProvider `91fb993e50e49e3b03d290b170c83cf57afbc518`, SDK 0.3.2 on `maint/interface6`, which fixes SchemaProvider #33: streaming from Google's OpenAI-compatible endpoint works again (#459).** Google's `https://generativelanguage.googleapis.com/v1beta/openai/chat/completions` leaves `finish_reason` out of every streamed chunk before the last, and the SDK's Chat codec refused such a chunk, so every `openai.chat` stream from that endpoint longer than one chunk ended with `ProtocolCorrupt` ("invalid response protocol") since 0.13.0, when NeoGraph moved its provider layer to SchemaProvider. The SDK now reads the missing member as `null` while the stream is open; a stream still needs a string `finish_reason` before `[DONE]`, and a buffered choice without it is still refused. OpenRouter, the native `google.generate` family and buffered calls were not affected. The interface revision and shared-library generation stay 6 and no public struct or enum changes layout. NeoGraph now requires `SchemaProvider 0.3.2 EXACT`, in its build and in the `find_dependency` of the installed `NeoGraphConfig`, so an installed 0.3.1 package no longer matches. Rebuild NeoGraph, C++ consumers and Python extensions together against the 0.3.2 headers and libraries (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **gRPC contract notes for implementations in other languages (#453).** The proto and the reference now state what 0.14 already required: a `CheckpointService` must serve every RPC, `RequiresManagedBudget` included. The engine calls it for every new or resumed run and fails the run on `UNIMPLEMENTED` (a managed budget is never assumed absent), so a server written against the 0.13 RPC set needs a `RequiresManagedBudget` that answers `required = false` when it manages no budget. `RunGraphRequest.thread_id` is documented as required (an empty one has been refused with `INVALID_ARGUMENT` since 0.14). Since 0.14 the gRPC headers include gRPC's own, so code that has `using namespace neograph;` must name NeoGraph's types `neograph::grpc::...`: a bare `grpc::` is ambiguous there. Documentation only, no behavior change.
- **The vendored cpp-httplib is updated from 0.41.0 to 0.61.0 (#460).** It serves only the A2A server, the MCP Streamable HTTP server and the test peers; NeoGraph's HTTP clients use libcurl and Asio. `deps/httplib.h` is the unmodified upstream release again: the local MSVC fix it carried (`X509_get_subject_name` returns `const X509_NAME *`) has been upstream since 0.44.0, and the two #450 backports are part of 0.61.0. The servers gain upstream's request-parsing hardening since 0.41.0: a request with an invalid `Content-Length`, a control byte in its request-target or chunk-size line, or more than one `Host` field is answered with 400 and the connection is closed; pipelined requests are served without waiting for the keep-alive timeout; an exception thrown by a callback drops only its connection instead of terminating the process; and the listen backlog is 128 instead of 5. **Behavior change:** cpp-httplib 0.44.0 stopped percent-decoding request header values, so they reach the servers as sent: `Authorization`, `Origin`, `Mcp-Session-Id` and the other headers the servers check are compared byte for byte, and a client that percent-encodes such a value is no longer matched.
- **The QuickJS performance gate tolerates runner variation up to 10% above a ceiling (#463).** `javascript_wrapped_core_ratio` ranged from 0.79 to 1.05 of its ceilings on hosted runners, so CI extended failed without a regression: byte-identical benchmark binaries measured a median of 547 in one run and 617 in another. The preregistration now declares `threshold_tolerance` 0.1, and `scripts/run_quickjs_performance.py` pins the same value. A statistic above its ceiling but no higher than 1.1 times it passes and is reported as a warning in the JSON report and the job log; a higher one fails as before. The ceilings are unchanged. Each statistic above its ceiling is now named in the job log, not only in the uploaded report.
- **The QuickJS performance gate times the JavaScript-wrapped Core run itself; its ratio to a direct Core run is still reported but no longer blocks (#465).** `javascript_wrapped_core_ratio` divides by a direct Core run, so a faster Core alone raised it: after #442 the direct run on the slower runner class went from 8.94 to 7.97 µs, the JavaScript-wrapped run stayed within 2% (4,311 and 4,366 µs), and the ratio rose from 489 to 549. A slower Core, in turn, could hide a slower JavaScript path. The new blocking case `javascript_wrapped_core` measures the same run in µs per invocation, with a median ceiling of 5,500 µs and a p95 ceiling of 6,000 µs. In 39 CI extended measurements from 2026-10-09 to 2026-10-16 its run medians ranged from 2,436 to 4,515 µs and its run p95s reached 4,720 µs; the preregistration records this basis. The ratio stays in the report with its ceilings and is logged as a warning when above them. Its ceilings are calibrated on the GitHub-hosted x86-64 runners of CI extended, which the preregistration and the README now state: a local run on slower hardware can fail this case without a regression (a quiet Arm Neoverse-N1 machine measured a median of 6,234 µs). The report now records the CPU model and the runner environment.
- **Program invocations are about 30% faster: canonical JSON is written from the underlying yyjson values (#469).** Every Program record (run records, journal entries, events, lineage) gets its identity from canonical JSON, which took a third of a Program invocation. `canonical_json_bytes`, its size and depth check and the strict duplicate-member check walked `neograph::json` handles. Each child access copied a handle, which costs an atomic reference count; each iteration built key and value lists; each key or string was copied. The three walks now read the yyjson values under the handle. The bytes, the member order, the limits and which error is reported first are unchanged: a randomized test compares the new writer with the old one, and every pinned identity is unchanged. On a quiet Arm machine the JavaScript-wrapped Core run of the `quickjs-performance` gate went from 6,126 to 4,350 µs, `structured_join_sequence` from 10,060 to 6,967 µs and `structured_join_await` from 7,505 to 5,143 µs; canonical JSON fell from 34% to 12% of the wrapped run.
- **Program invocations are about 23% faster: json documents take their memory from a per-thread block cache (#471).** Every `neograph::json` value owns a yyjson document, which made up to four separate allocations: the `shared_ptr` control block, the document, and a value and a string pool chunk. A Program record body creates and frees hundreds of them per transition. That is more blocks of one size than malloc's per-thread cache holds, so most went through its slower paths. These blocks now come from a per-thread cache by size class: 32 bytes to 2 KiB, at most 64 blocks of each, under 260 KiB per thread. Larger blocks go to malloc as before. A block freed on another thread joins that thread's cache, and a thread's cache is released when the thread ends. Builds with AddressSanitizer, ThreadSanitizer or MemorySanitizer allocate every block directly. On a quiet Arm machine the JavaScript-wrapped Core run of the `quickjs-performance` gate went from 6,196 to 4,745 µs, and `structured_join_sequence` and `structured_join_await` became 24% faster. Together with #469 the three cases are 44-47% faster.
- **The QuickJS performance gate also compares each case with a base build on the same runner (#476).** Hosted runner types differ by up to 1.7 times, so absolute ceilings could not see the 9-23% slowdown of #467. `scripts/verify_ci.py quickjs-performance --base <ref>` now exports that commit with `git archive` and builds its QuickJS benchmarks. `scripts/run_quickjs_performance.py` then runs every primitive and control case except `replay_growth` on both builds, alternating 20 samples of each. A head median more than 1.1 times the base median is logged as a warning, and a blocking case above 1.25 times is compared again with fresh samples and fails only if the rerun is above 1.25 too. A case the base build does not have is reported as skipped. The ratios are in the preregistration (`ab_comparison`), and the report records the comparison. CI extended compares a branch with its merge base on `master`, and `master` with the latest release. The absolute ceilings still apply.
- **Program invocations are about 20% faster again: record bodies are built in one JSON document (#478).** A Program invocation built the canonical bytes of its records from `json` initializer lists. An initializer list makes a yyjson document for every key, every value and every pair, then copies each value again into the result. That came to about 5,400 documents per invocation of the JavaScript-wrapped Core run in the `quickjs-performance` gate. An internal builder (`neograph::detail::JsonBuilder`) now builds the bodies of events, run records, lineage and generation records, journal records, invocations and command journal entries in a single document. Its value overloads mirror `json`'s converting constructors, so every member keeps its yyjson type, and the bytes, ids and limit checks are unchanged. A new test pins 31 identities of representative records, produced before the change. An invocation now makes about 1,000 documents. On an x86 desktop the JavaScript-wrapped Core run, `structured_join_sequence` and `structured_join_await` became 21%, 18% and 19% faster.

### Added

- **The built-in gRPC servers answer the standard health service and can be stopped (#454).** `run_server`, `run_tool_server` and `run_checkpoint_server` now serve `grpc.health.v1.Health`: `SERVING` for the server and for its service (`neograph.v1.GraphService`, `neograph.v1.ToolService`, `neograph.v1.CheckpointService`) while it takes calls, `NOT_SERVING` once it stops, so Kubernetes gRPC probes and `grpc-health-probe` work against them. The process-wide gRPC default for the health service is turned on only while a built-in server starts and then set back, so the host's own servers are unaffected. New overloads take `ServeOptions` (`<neograph/grpc/serve_options.h>`): a `std::stop_token` that stops the server after `shutdown_grace` (5 s by default) for the calls in flight, and an `on_serving` callback with the bound port, so the address may name port 0. Server reflection stays the host's choice: link `gRPC::grpc++_reflection` and call `grpc::reflection::InitProtoReflectionServerBuilderPlugin()` before the servers start, and gRPC adds it to every server, the built-in ones included. The existing signatures are unchanged.
- **`json::parse` takes a nesting limit from the caller (#481).** New overloads `json::parse(std::string_view, std::size_t max_depth)` and `json::parse(std::istream&, std::size_t max_depth)` refuse text that nests arrays and objects deeper than `max_depth` levels; a limit of 0 admits only scalars. The overloads without a limit keep `json::max_parse_depth` (256, since #480), and NeoGraph's own callers, its servers and clients among them, still use them. Copying and comparing a value recurse once per level, so a limit above 256 is for text the caller trusts, read on a thread whose stack can copy and compare a value that deep. The existing signatures are unchanged: this adds symbols and changes no layout.

### Fixed

- **`MCPClient::shutdown()` returns while the server keeps the GET event stream open (#448).** Over Streamable HTTP the client opens the optional GET event stream after `initialize`. When a server kept that stream open after the session's `DELETE`, `shutdown()` and the destructor waited until the server closed it, possibly forever. The official Python and TypeScript servers close it on `DELETE`, which hid the problem; the conformance suite's `sse-retry` scenario does not, and timed out on exit. Shutdown also waited for the pause before a reconnect when the stream had just ended, up to the `retry` the server named. The listener's stop signal was a cancelled timer wait, which `awaitable_operators::operator||` counts as a failure, so it kept waiting for the stream read or the pause. The stop wait now completes normally when the timer is cancelled or expires, and shutdown expires the timer, so it also stops a listener that has not begun to wait. No API change.
- **The MCP HTTP client reads an SSE event sent as one HTTP chunk over 1 MiB, and follows 307 and 308 redirects on the endpoint's origin (#449).** A response stream with an event larger than 1 MiB in one HTTP chunk, as Node and uvicorn servers write each event, failed with `async_post_stream: response chunk limit`, although the client accepts events up to 16 MiB; a chunk may now be as large as an event. A `307` or `308` answer, such as the one FastMCP servers on `mcp` 1.x send for `POST /mcp` to move it to `/mcp/`, was an error; the client now repeats the request, with its method, body and headers, at the `Location`, up to five times and only when the `Location` is on the endpoint URL's origin (same scheme, host and port; a relative `Location` is). A redirect to another origin, including from https to http, is not followed and nothing is sent there: the call fails with the `307` or `308` as before, and the error message says that the redirect was not followed. `301`, `302` and `303` are not followed. **Behavior change:** a server that redirects on its own origin is now reached instead of failing with HTTP 307 or 308.
- **The MCP HTTP server closes the connection after answering a `DELETE` without a body (#450).** The official clients send their session `DELETE` without `Content-Length`. The vendored cpp-httplib treated that request as having a body after the handler ran, so it kept the connection open reading for one until the client closed it or the read timed out, and would have read a next request on that connection as that body; it also kept connections open after a response that said `Connection: close`. The vendored cpp-httplib, now 0.61.0 (#460), fixes both: a request without `Content-Length` or `Transfer-Encoding` has no body to drain (RFC 9112 §6.3), and a response that carries `Connection: close` closes the connection (RFC 9112 §9.6). **Behavior change:** the two NeoGraph servers still built on the vendored cpp-httplib, the A2A server and the MCP Streamable HTTP server (NeoGraph's HTTP clients use libcurl and Asio and are not affected), now close the connection after a response that carries `Connection: close`, which cpp-httplib adds to every `4xx` and `5xx` answer; clients that honour the header, NeoGraph's own included, already opened a new connection.
- **gRPC: `RunGraphStream` never sends the thrown text (#451).** When a node threw, the stream forwarded the engine's `ERROR` event unchanged as a `DEBUG` event, so the exception's `what()` reached the client ahead of the sanitized failure, while unary `RunGraph` sent the public message only. A node error, its retries included, is now a `DEBUG` event whose `error` is the fixed text `node failed` (`attempts`, `handled`, `routed_to`, `retry`, `max_retries` and `delay_ms` are kept); the run's failure event (`{error, code}` with the public message) and the final status are unchanged. **Behavior change:** a client that read the exception text from a stream event gets `node failed` there.
- **gRPC: `RunGraphStream` follows `stream_mode`, and a state snapshot is `VALUES` (#452).** `RunGraphRequest.stream_mode` was ignored, so every stream carried every event, and the full-state snapshot (node `__state__`) was sent as `UPDATES`, so `VALUES` was never sent. The mode now applies with the bits of `StreamMode` (`EVENTS=1`, `TOKENS=2`, `VALUES=4`, `UPDATES=8`, `DEBUG=16`; 0 streams every event as before, unknown bits are ignored), `TOKEN` events are sent only with `TOKENS`, and a state snapshot is a `VALUES` event; an interrupt and a node error stay `DEBUG` events, as the proto now documents. `FINAL` is sent whatever the mode. **Behavior change:** a request that set `stream_mode` receives only the events it asked for, and a client that read the state snapshot from `UPDATES` events finds it in `VALUES` events.
- **gRPC `CheckpointService`: a record the server cannot store whole is refused, not cut (#453).** The server parses `checkpoint_json` into a `Checkpoint` before it saves it, so a top-level member it does not know (written by a newer engine or by another implementation) was silently dropped. `SaveCheckpoint` and `PublishManagedBudgetCheckpoint` now refuse such a record, and one whose `schema_version` is newer than the server's, with `INVALID_ARGUMENT` (`NG.InvalidRequest`; `FAILED_PRECONDITION` for the managed-budget publication, as for its other refusals), and store nothing. Free JSON inside `channel_values` and `metadata` is kept as before. **Behavior change:** a client that sent extra top-level members, which were dropped, now gets an error.
- **A2A: an optional member sent as JSON `null` is absent (#455).** Message parsing read a present-but-null `taskId` or `contextId` as an empty string, so `"taskId": null` answered `-32001 Task not found` and `"contextId": null` stored the task with an empty context id, in both the 0.3 and the 1.0 dialect. Configuration members had the same defect: `"blocking": null` was read as `false`, so the send returned the SUBMITTED task at once, and `"historyLength": null` as 0. A null `taskId`, `contextId`, `name`, `description`, `blocking`, `returnImmediately`, `historyLength` or `configuration` is now the same as an absent one, for the server and the client. This was the one 0.3 conformance-kit quality test (`test_null_optional_fields`) that NeoGraph failed and the reference server passed. **Behavior change:** a request with these members set to `null` now succeeds as if they were absent.
- **A2A: the card URL behind a TLS-terminating proxy, and no more empty `url` (#456).** When `AgentCard::url` is empty, `A2AServer` advertises the address the client used, always with the `http` scheme, so behind a proxy that terminates TLS the card (and its `supportedInterfaces`) sent SDK clients to plain HTTP. `A2AServer::set_trust_forwarded_headers(true)` now takes the scheme and host from RFC 7239 `Forwarded` (its first element), else from `X-Forwarded-Proto` / `X-Forwarded-Host`; only `http` / `https` and plausible hosts are taken. It is off by default, since a client can send those headers itself; a configured `AgentCard::url` is still preferred. The 0.3 JSON of an `AgentCard` without a `url` (written outside `A2AServer`, for a registry for example) no longer has `"url": ""`: it takes the first interface of the preferred transport, else leaves the member out. **Behavior change:** such a card has no `url` member instead of an empty one.
- **ACP: `$/cancel_request` cancels an in-flight `session/prompt` (#457).** The server sent `$/cancel_request` for its own pending client requests but ignored the notification from a client, so the prompt it named ran to completion. A `$/cancel_request` whose `requestId` is that of a running `session/prompt` now cancels that turn as `session/cancel` does, and the prompt still gets its one response with `stopReason: "cancelled"`; a `requestId` of an unknown or finished request, or of another method's request, is ignored. **Behavior change:** a client that sends `$/cancel_request` for a prompt now gets `cancelled` instead of the completed turn.
- **`WsClient` has a handshake deadline and an optional read deadline (#458).** `ws_connect` waited forever on a server that accepted the TCP connection and never answered the Upgrade request. `WsClientOptions::handshake_timeout` (30 s by default) bounds name resolution, connect, the TLS handshake and the Upgrade exchange; `WsClientOptions::read_timeout` (0 by default, no deadline, since an idle connection that waits for server pushes is normal) bounds one `recv()`. Both expire with `asio::system_error(asio::error::timed_out)`; a read timeout closes the connection, since a frame may be half read. Negative values throw `std::invalid_argument`. **Behavior change:** `ws_connect` with default options fails after 30 s instead of waiting forever.
- **A Core node attempt without a timeout inside a Program waits for its body with less overhead (#467).** Since #367 and #368, every node body of a Core run inside a Program runs on the node-attempt worker pool instead of the shared Program scheduler. The orchestrator waited for each such attempt through a strand, a coroutine and a 24-hour timer of its own. An attempt without a timeout can neither time out nor be detached, so its body now resumes the orchestrator with one post when it finishes. That saves one thread wake-up per node, about 8 µs per node on a quiet Arm machine. Most of the per-node cost of #367 and #368 remains: the body runs on another thread, often on another core, which made the JavaScript-wrapped Core run of the `quickjs-performance` gate 9% slower and `structured_join_sequence` 12% slower. A timed `ng.await` is a further 11% slower because it records its start in the command journal before dispatching (#391), which crash recovery needs.
- **`json::parse` refuses text that nests arrays and objects deeper than 256 levels (#480, #483).** Copying a `neograph::json` value and comparing two values recurse once per level of nesting, as do some of NeoGraph's own walks over values, and `json::parse` accepted any depth, so text nested deeply enough could exhaust a thread's stack. `json::parse` now throws `json::parse_error` for text nested deeper than the new constant `json::max_parse_depth` (256, the limit Program JSON already had), before it copies the value. At that depth the walk with the largest frames needs about 384 KB of stack, less than the 512 KiB of a macOS secondary thread. **Behavior change:** text from peers, files and callers that nests deeper than 256 levels no longer parses; a caller that trusts such text can pass a higher limit (#481). `SqliteCheckpointStore`, `PostgresCheckpointStore`, `SqliteHarnessRecordStore`, `FileHarnessRecordStore`, `SqliteEvidenceLedger` and NeoGraph's internal copies read back what NeoGraph wrote at any depth, so a checkpoint that holds a deeper value still loads. A document of at most 256 values skips the check; a larger one takes one more pass over its values.
- **Python: converting a value that contains itself, or one nested deeper than 256 levels, raises instead of crashing (#484).** The binding converted Python values to `neograph::json` recursively, without a depth limit or a cycle check, so a dict or list that contained itself recursed until the process ran out of stack. Dicts, lists and tuples nested deeper than `json::max_parse_depth` (256) now raise `ValueError`, and a value that contains itself reaches that depth. Each container also enters Python's recursion check, so a conversion that starts deep in a Python call stack raises `RecursionError` instead.
- **NeoGraph's own recursive walks over `json` values stop at `json::max_parse_depth` (#485).** Several walks recursed once per level with no limit of their own: the graph round-trip check, the Program compiler's condition, operation and config-string checks, the Program metadata diff, harness journal redaction, the harness host configuration filter, MCP adoption's scan, JSON Schema `const`, `enum` and `uniqueItems` equality, the Decisions client's value check and the harness Program translator's operation check. A value that bypassed the parse limit could therefore exhaust a thread's stack: a value built in code, one parsed with a higher limit, or one read back from a store. Each walk now throws `std::invalid_argument` for arrays and objects nested deeper than 256 levels instead of recursing further. It fails rather than returning early, so neither redaction nor a check is skipped. Program JSON Pointer assignment past 256 tokens fails as an invalid pointer does. Values within the limit behave as before.

## [0.14.1] - 2026-10-14

### Changed

- **Linux wheels are built from pinned images (#333).** The `manylinux_2_34` images are pinned by the digests that built 0.14.0, and the CI runners no longer use `-latest` labels. The bundled libraries of the Linux x86_64 wheel are byte-identical to 0.14.0's, including the LGPL `keyutils-libs` and `libxcrypt`, so the published library source still applies.
- **Rebuild C++ consumers: `RuntimeConfig` gained a member and `ProgramHandle` a function (#431).** `RuntimeConfig` gained `core_provider_calls_journaled`, `ProgramHandle` gained `notices()`, and `ProgramNotice` is new. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **A fork on a host without a durable provider-call journal says so (#431).** A fork replays the provider calls of the paused invocation only from a durable provider-call journal (`SQLiteProgramProviderCallJournal`, which `LocalProgramHost` has when it has a `sqlite_directory`). The in-process broker keeps none, so a node that runs again in the fork sends and charges its earlier calls again; that is unchanged. **Behavior change:** when `RuntimeConfig::core_provider_calls_journaled` is `false` (the default) and the paused attempt recorded priced provider calls in the paused operation, the handle `ProgramRuntime::fork` returns now carries a `ProgramNotice` with the code `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` (`ProgramHandle::notices()`, Python `handle.notices`). Its `witness` lists the calls (`call_id`, `node`, `model`, `microunits`) and their total. The notice is not an error and changes neither what the fork sends nor what it charges. It cannot tell which listed calls the fork runs again, since a call of a node that finished in the paused step is replayed from the step's saved writes. It is not stored with the run, so the handles of `reconnect()` and `resume()` have none. A host whose resolver returns `SQLiteProgramProviderCallJournal` brokers should set `core_provider_calls_journaled = true` (`LocalProgramHost` does when it has a `sqlite_directory`). Documented in the Program guide, the Python binding guide and the reference.

### Fixed

- **The Core run path recovers most of the speed it lost in 0.14.0 (#442).** The 0.14.0 work on barrier-stall judging, failure routing, retry attempts and `any_of` slots made every run of a plain graph 14-19% slower than right after #343, so the 0.14.0 line "Core engine speed is back at v0.11.1 level" did not hold for the released tag. A run now allocates its bookkeeping once, a graph without failure routing never reads its routing tables, a fan-out keeps its branch state in one vector, and a node invocation runs in one coroutine frame that Asio recycles. Measured on idle machines with `bench_neograph 10000 5000 1` (24 interleaved rounds, medians): on our own ARM server a sequential run went from 11.56 to 10.34 µs and a parallel run from 24.90 to 23.04 µs (right after #343: 10.16 and 21.91); on an x86_64 machine (Core i7-8750H) from 7.68 to 6.78 µs and from 18.64 to 16.87 µs (right after #343: 6.45 and 15.91). A gap of 2-7% to the state right after #343 remains. No API or behavior change. (The figures were corrected after the release, which quoted a measurement that did not reproduce on an idle machine.)
- **Program: a run that finishes over its money, Core-step or wall-time grant ends with the record of that budget (#443).** Usage is settled once a plan has completed, and a run found over its Core-step, money or wall-time grant ended `BudgetExhausted` with no `failure`, so a caller could not tell which budget it was or by how much; a run over its wall time ended `BudgetExhausted` where one stopped by its deadline ends `TimedOut`. Every over-budget ending now has a failure record whose witness holds `granted`, `used` and `over_by` under the budget's key, as the model-token record does (#420): `P_MONEY_BUDGET` with `failure.witness["monetary"]`, `P_CORE_STEP_BUDGET` with `failure.witness["core_steps"]`, and `P_RUNTIME_TIMEOUT` with `failure.witness["wall_time_ms"]`. Wall time ends `TimedOut` on both paths. A run over several budgets lists each in the witness, and its status and code are those of the first of wall time, model tokens, money and Core steps. The records of a run that the Core engine stopped at its Core-step grant and of one that its deadline stopped gain the witness too (`over_by` is 0 at the grant). **Behavior change:** a run that finishes over its money or Core-step grant has a `failure`, one that finishes over its wall-time grant is `TimedOut` instead of `BudgetExhausted`, and the failure of a timeout has `witness["wall_time_ms"]`.
- **A strict profile on a plain `GraphEngine` refuses the second LLM call of a tool loop instead of resending the first turn's context (#444).** With `StrictRuntimeProfile` attached, a tool loop inside one `engine.run()` (LLM call, tool dispatch, LLM call, as `create_react_graph` builds it) built every prompt from the epoch's fixed RAW range, because nothing in the run appends the assistant and tool records to the context feed or activates a new epoch. The tool results stayed in graph state and never reached the provider, so the model could ask for the same tool again until the step limit or a budget stopped the run. `llm::Agent` with a controller had the same defect. The strict path is host-stepped: the host appends RAW records and activates a new epoch for every provider call. A provider call that arrives for an epoch whose turn the graph has already advanced, that is, whose presented messages hold assistant or tool messages added since the epoch's first dispatch and not also carried as host instructions or trusted supplemental input, now throws `std::logic_error` with a message that names this contract (the node is not retried; Python raises `RuntimeError`). **Behavior change:** a graph or agent that looped inside one run under the strict profile, and used to send stale context, now fails on its second provider call. A retry of the same turn, a conversation that already held those messages at the epoch's first dispatch, and the built-in nodes that carry their conversation as trusted supplemental input (such as the plan executor) are unchanged. Activating an epoch starts the next turn. Stepping to a later epoch of the same run also no longer fails with a receipt conflict: the controller restarted its dispatch counter with every activation, so the first dispatch under an epoch with sequence above 1 reused the id of the run's first dispatch. Such a dispatch is now identified `<run id>-epoch-<sequence>-dispatch-<n>`; the ids under a run's first epoch are unchanged. The strict guide documents the contract, and a helper that advances the epoch inside a run is not part of NeoGraph yet.
- **Documentation: context management, budget, placement and concurrency (#328, #326, #329, #330).** Documentation and comments only, no behavior change. The strict guide compares the `messages` channel, `history::compact_history`, the recorded profile and the strict profile (#328). It says that the context estimate (encoded request bytes divided by three) is a heuristic over the whole encoded request, not a proven upper bound (#326), and it gives placement guidance: which `ContextPlacement` values move between turns, that stable artifacts belong at `BeforeHistory`, and that on `anthropic.messages`, `google.generate` and `google.interactions` System-role artifacts (`RequiredSkill`, `HardConstraint`) go to the top-level system field whatever their placement (#329). The concurrency guide states that context feeds fail closed on append conflicts, that checkpoint stores are last-writer-wins per `thread_id`, that serializing the runs of one `thread_id` is the host's job, and that an in-process guard is planned (#330).
- **A Send execution that returns Sends fails the run instead of losing them (#432).** The Sends returned by a node that was itself started by a Send were dropped, so their targets never ran and nothing said so. The run now fails with an error that names the node and the targets, before any result of the step is applied. **Behavior change:** a graph whose Send targets return Sends, which used to finish without running those targets, now fails; write the items to a channel and let an ordinary node send them.
- **A failed Program attempt names its Send executions as the engine's trace does (#433).** A failing Core rethrows without its engine trace, so a failed attempt builds its trace from `NODE_END` events, and it listed a Send execution by the bare node name. The `NODE_END` event of a Send execution now carries `"send": true`, and the attempt's trace lists it as `<target>[send]`, in the order the executions completed. **Behavior change:** the trace of a failed Program attempt names Send executions `<target>[send]`, and the `NODE_END` event of a Send execution has the `send` field.
- **Python can silence the build-time lint (#415).** `GraphEngine.compile` gained the keyword `stderr_notices` (default `True`, as `EngineConfig::stderr_notices` in C++). With `stderr_notices=False` it prints neither the lint of a strict graph, such as E7 for a node that only `Send` or `Command.goto` starts, nor the one-time notice that a fan-out ran serially; validation errors still raise. E7 itself is unchanged: a graph definition declares no dynamic targets, so the validator cannot tell a node that Send starts from one nothing reaches, and the docs now say so. They also say that a static edge to a Send target is not a way to quiet the lint: the target runs once more, after the Sends, on the shared state. Code that does not pass the keyword behaves as before, and graphs run as before.
- **`RunResult::execution_trace` has a stated contract (#424).** Documentation and tests only, no behavior change. The report that nodes started by `Send` are missing from the trace could not be reproduced: `run`, `run_stream`, `run_async`, `run_stream_async` and `resume`, with and without a worker pool, list each Send execution as `<target>[send]`. `types.h`, `engine.h` and the docs now state the order: super-steps in order, and within a step the ready nodes, then the Sends in the order they were emitted, whatever order they finished in. A step that paused lists nothing, a resumed run lists the whole re-entered step, and a subgraph is one entry.
- **Documentation: a channel's `version` is the value of one write counter shared by all channels (#416).** The `RunResult` documentation, the headers and the guides called `output["channels"][name]["version"]` the number of times the channel was written and `output["global_version"]` a super-step counter, and the docstring of the Python `global_version()` called it the sum of the channel versions. They are values of one counter that every write to any channel advances, the writes of the run input included: a channel's version is the counter at its last write and `global_version` is the counter after the run's last write. Versions rise with every write and no two channels share one, but neither number is a write count or a super-step number. A channel never written has version 0, also with an `initial` value. Checkpoints store both numbers (the engine does not set `Checkpoint::channel_versions`), and a resumed or forked run goes on counting from them. The numbers did not change; tests pin them (`ChannelVersions.*`, `test_channel_versions.py`).

## [0.14.0] - 2026-10-13

### Changed

- **Rebuild C++ consumers: task graph types gained members (#412).** `TaskGraphProposalLimits` gained `min_schema_version`, `max_attempts`, `max_literal_bytes` and `max_total_literal_bytes`, so `TaskGraphProposalOptions`, `TaskGraphFragmentCompileOptions` and `TaskGraphExpansionPolicy`, which embed it, changed layout. `TaskGraphTask` gained `literals` and `max_attempts`, `CompiledTaskGraphTask` gained `input_literals` and `max_attempts`, and `TaskGraphInputLiteral` is new. `TaskGraphProposal::SCHEMA_VERSION_2` and `CompiledTaskGraphFragment::SCHEMA_VERSION_2` are new, and `TaskGraphProposal::schema_version()` returns the version of the document. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Rebuild C++ consumers: tool, Hook, MCP, gRPC and graph types gained members.** `RunResources` gained `hook_runtime`, `InvocationResult` gained `failure`, `MCPTool` gained the base `ContextualAsyncTool` and `execute_result_async(arguments, context)`, `MCPClient` gained a bounded `call_tool_result_async`, `MandatoryHookRunner` gained `blocks` and a private member function, `GrpcRemoteTool` gained the base `ContextualAsyncTool` and `set_call_timeout`, and `GrpcCheckpointStore` gained `set_rpc_timeout`; `graph::FailureVerdict`, `graph::classify_failure`, `ToolExecutionError`, `grpc::make_tool_service` and `grpc::make_checkpoint_service` are new. `proto/neograph.proto` gained `ToolCallResponse.error_code` at a new field number, so the wire stays compatible. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Tool Hooks run on every tool path.** An engine's `HookRuntime` (`EngineConfig::hook_runtime`, `GraphEngine::set_hook_runtime`) used to publish checkpoint Hooks only; it now also runs the `BeforeToolExecution` and `AfterToolExecution` Hooks of every tool call its nodes make (`tool_dispatch`, the plan-and-execute executor, the deep-research researcher, host nodes using `make_tool_execution_context`), exactly as `llm::Agent::set_hook_runtime` does for the Agent. `RunResources::hook_runtime` replaces it for one run, a subgraph's tool calls use their parent run's, and a Program Core's tool calls use the Program host's `RuntimeConfig::hook_runtime`. **Behavior change:** a host whose Hook registry admits tool-phase Hooks now sees graph and Program tool calls, and a mandatory one can block them; a Hook boundary that cannot run (a run without an owner scope) fails closed with `HookBoundaryBlocked` state `failed`, for checkpoint, provider and Program events too, instead of an `std::invalid_argument`.
- **A Hook block leaves a graph run as `HookBoundaryBlocked`.** A graph run used to throw a `NodeExecutionError` whose cause was the `HookBoundaryBlocked`, while `llm::Agent` threw it directly; now both throw the `HookBoundaryBlocked` itself, with its invocation id and state, wherever it sat in the cause chain, and the node emits no `ERROR` event for it. A cancellation found inside a failure's cause chain leaves as `CancelledException` the same way. Python already saw `ng.HookBoundaryBlocked`; it no longer carries the `node_name` and `attempts` attributes.
- **A Hook reports `timed_out` only when it timed out.** A mandatory Hook whose last attempt failed used to be recorded and reported as `timed_out`; it is now `failed`, in `InMemoryHookJournal`, `SQLiteHookJournal` and `HookBoundaryBlocked::state()`, and an exhausted failure is no longer listed by `pending()`. A Hook whose deadline elapsed, or whose native target timed out (as an RPC Hook's already did), reports `timed_out`, including when its lease ran past the deadline.
- **No tool call starts after the run's deadline.** On every dispatch path a call that would start at or after `RunContext::deadline` (`ToolExecutionContext::deadline`) is a tool result with status `expired` and error `run deadline elapsed before the tool started`, without its Hooks. Before, it ran when no Hook was configured, and stopped the run with a `timed_out` `HookBoundaryBlocked` when one was. The deadline also bounds the admission queue wait.
- **MCP and gRPC servers keep failure classes apart, and send no exception text.** `MCPServer` answers a Hook block with JSON-RPC error `-32010` and an interrupt with `-32011`, and puts the stable code of every other failure in the `isError` result's `_meta["io.neograph/failure"]`. A call the client cancelled gets no response on any transport, as MCP asks (a response sink's `NoResponseSink` receives its id); a call `stop()` cancelled gets `-32000`. gRPC uses canonical status codes: a failed `RunGraph` or `RunGraphStream` ends with the class's status (`UNKNOWN`, `DEADLINE_EXCEEDED`, `UNAVAILABLE`, `RESOURCE_EXHAUSTED`, `FAILED_PRECONDITION`, `INTERNAL`, `ABORTED`, `CANCELLED`) and a `google.rpc.Status` whose `ErrorInfo.reason` is the stable code; an invalid request is `INVALID_ARGUMENT` (or `FAILED_PRECONDITION` without a graph) before anything runs; `ToolService` keeps a tool's own failure in the payload with `error_code`, and answers a run control signal with a status, an unknown tool with `NOT_FOUND` and malformed arguments with `INVALID_ARGUMENT`. Every message is the public message (`NodeFailureError::public_message` or a description of the class), never `what()`. **Behavior change:** a failed graph run over gRPC used to be status `OK` with `error` set, and a failed stream ended `OK`; `RunGraphResponse.error` is deprecated and kept for one release. An `MCPServer` tool error used to carry the exception text, and a cancelled or stopped call an `isError` result. See [Protocol failure mapping](docs/reference-en.md#protocol-failure-mapping).
- **`ToolExecutionController::execute_async` throws the typed result.** The throwing facade threw `asio::system_error` for `timed_out`, `expired` and `rejected` and `std::runtime_error` for every other failure; it now throws `ToolExecutionError`, which carries the whole `ToolExecutionResult`, and still `CancelledException` for the cancellation statuses. The node failure classifier treats an uncertain effect in it as `NG.EffectUncertain`, so a node retry policy no longer runs such a call again, and a timed-out or expired call as `NG.Timeout`. An asio or `std::system_error` with `timed_out` is now `NG.Timeout` too instead of `NG.TaskFailed`; both stay retried and routable.
- **Rebuild C++ consumers: A2A and ACP layouts gained members.** `a2a::AgentCard` gained `skills`, `security_schemes`, `security_requirements` and `extensions`, `a2a::GraphAgentAdapter` gained the virtual `build_resume_value`, and `a2a::A2AClient` gained endpoint-policy state. `acp::ClientCapabilities` gained `elicitation`, `acp::ACPClient` gained a member, and `acp::ACPGraphAdapter` gained virtual functions. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **A2A: 1.0 (v1.0.1) is the primary version and the native server follows its task rules.** 0.3 stays as a compatibility dialect. Behavior changes: task ids are generated by the server, so a message naming an unknown `taskId` fails with TaskNotFoundError (`-32001`) instead of creating that task (NeoGraph collaboration envelopes keep their sender-named ids); a message to a terminal task fails with `-32004`, one to a running task with `-32603` (ErrorInfo `TASK_BUSY`), and one to a Program task waiting for input with `-32004`, since answering a Program's input request over A2A is not supported yet; `CancelTask` of a terminal task fails with TaskNotCancelableError (`-32002`) and leaves the task unchanged; the in-flight limit answers `-32603` (ErrorInfo `RESOURCE_EXHAUSTED`) instead of a `REJECTED` task; only `A2A-Version` 1.0 and 0.3 are accepted; `AgentCard::streaming` now defaults to `true`, and a card with `streaming = false` refuses the streaming methods. The card is served per version: `A2A-Version: 1.0` receives a pure 1.0 card, a request without a version the 0.3 card (which still lists the 1.0 interface first). A task's history starts with the user's message. On the Program path, missing credentials get HTTP 401 with `WWW-Authenticate` and an unpermitted collaboration peer HTTP 403, instead of `AUTH_REQUIRED` tasks; the card declares a Bearer security scheme.
- **A2A client: the first call fetches the AgentCard and follows the selected interface.** Behavior change: without a fetched card the client used to probe 0.3 first. It now fetches the card before its first call and, only when none can be fetched, probes 1.0 first, falling back to 0.3 on `-32601`, `-32009` or HTTP 400. RPCs go to the selected interface URL when it has `base_url`'s origin; `set_allow_cross_origin_interfaces(true)` lets a card name another origin (without it the client keeps `base_url` and writes a warning to stderr), and `endpoint_url()` reports the endpoint in use. Message ids are random UUIDs. `A2ACallerNode` keeps `<output_key>_task_id` only while the task waits for input and always sends the context id, so a terminal turn is followed by a new task in the same conversation.
- **ACP: a failed prompt turn is a JSON-RPC error, and exception text never reaches the client.** A failed turn is `-32603` carrying the public message and `data: {code, retryable}` (it used to be a chunk with the exception text followed by `stopReason: end_turn`); a provider refusal ends with `refusal`, a crossed model-token budget with `max_tokens`, a step limit with `max_turn_requests` (no empty chunk), and a cancellation the client did not ask for is `-32800`.
- **ACP: a NodeInterrupt asks `session/request_permission` in the same turn** and resumes from its checkpoint with the selected option (it used to be a chunk with `_meta` and `end_turn`, answered by the next prompt); `ACPGraphAdapter::interrupt_permission` and `interrupt_resume_value` customize it.
- **ACP error codes follow the v1 schema:** busy and overloaded turns are `-32603` with `data.reason` (no longer `-32000`, which ACP reserves for "authentication required"), calls before `initialize` are `-32600`, and an unknown `session/resume` is `-32002`.
- **ACP cancellation:** a stray cancel is ignored (`set_pre_armed_cancel` restores the old pre-cancel); a pending agent→client request is abandoned with `$/cancel_request`; permission requests have no hard timeout (`set_permission_timeout`); stdin EOF cancels in-flight turns (`set_eof_grace`).
- **ACP prompt content and session context:** `resource_link` and advertised embedded resources reach the graph; malformed or unadvertised content blocks are `-32602`; `_acp_cwd` and `_acp_additional_directories` come from `build_session_state`; `terminal/*` and `elicitation/*` need the client capability; `agentInfo` is always a valid Implementation.
- **Rebuild C++ consumers: `HookRuntime` gained members.** `HookRuntime` gained `compose`, `emit_once_async` and two private members, and `MandatoryHookRunner` gained a private member function; `graph::RemoteControlFailure` and `tool_call_not_started` are new. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **A subgraph's tool calls run both Hook runtimes.** When a subgraph's engine and its parent run both have a `HookRuntime`, its tool calls go through both, the parent's Hooks first and then the child's (`HookRuntime::compose`), and a block from either blocks the call. **Behavior change:** the parent run's runtime used to replace the subgraph engine's, whose tool Hooks then never ran.
- **Tool Hook events have a durable identity and are delivered once.** In a graph run, and wherever a call has a task slot (`ToolExecutionContext::effect_task_id`), the `BeforeToolExecution` and `AfterToolExecution` events carry `task_id` and `call_ordinal`, the call's effect identity (the run, the thread, the task with its node attempt and the call ordinal), and `request_id` is derived from it instead of a per-process counter. A resume or replay that runs a batch again does not deliver an event already delivered for that identity; an emission that a Hook blocked was no delivery and runs again. `llm::Agent` gives each turn's batch its own task slot. **Behavior change:** a resumed batch used to run every tool Hook of its calls again.
- **gRPC `ErrorInfo.reason` follows AIP-193.** `reason` is now an UPPER_SNAKE constant derived from the stable code (`HOOK_BLOCKED`, `TIMEOUT`, `TOOL_NOT_FOUND`, `INVALID_REQUEST`, `NO_GRAPH`, `CAPACITY`, `BACKEND_UNAVAILABLE`, `BARRIER_STALLED`, ...; an author code gives its class's reason), and the `NG.X` code itself moved to `ErrorInfo.metadata["code"]`; the domain stays `neograph`. **Behavior change:** `reason` used to be the `NG.X` code; a client that read it should read `metadata["code"]`. NeoGraph's own clients accept both.
- **A remote Hook block or interrupt is a typed tool failure.** When `MCPTool` receives JSON-RPC error `-32010` (the server's Hook blocked the call) or `-32011` (the server's run needs input), or `GrpcRemoteTool` a status whose `ErrorInfo` code is `NG.HookBlocked` or `NG.Interrupted`, it raises `graph::RemoteControlFailure`: an ordinary failure, not retried, with the code `NG.HookBlocked` or `NG.Interrupted` and the remote's public message. It neither stops nor pauses the local run, which goes on with a failed tool result; native propagation of a remote interrupt is a later phase. **Behavior change:** these were untyped tool failures (`NG.TaskFailed`), and a remote Hook block that timed out (`DEADLINE_EXCEEDED`) looked like the call's own deadline.
- **Rebuild C++ consumers: MCP and async types gained members.** `MCPError` gained `http_status()`, `StdioClientConfig` gained `cancel_grace`, `MCPHttpServerConfig` gained `max_http_threads`, `session_idle_timeout` and `protected_resource_metadata`, `MCPServer` gained `protocol_version()`, the `NoResponseSink` type and a `set_response_sink` overload that takes one, `MCPClient` gained `initialize` and `initialize_async` overloads that take a deadline and a cancel token and `set_tool_list_changed_handler`, `MCPTool` gained a private member function, and `SseEventParser` gained `last_event_id()`, `retry_ms()` and private members; `async::async_request_stream`, `async::HttpResponseHead`, `mcp::MCP_SUPPORTED_PROTOCOL_VERSIONS`, `mcp::is_supported_protocol_version`, `MCPServer::Reply` and a `MCPServer::handle_message` overload that takes one are new. `ToolExecutionController` gained private member functions. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **The MCP client uses its URL as given.** `MCPClient(url)`, `MCPTool(url, ...)` and Python's `mcp.MCPClient(url)` send every request to the configured URL, path and query included, as MCP's Streamable HTTP transport requires. **Behavior change:** they appended `/mcp` unless the path already ended in it (`http://host:8000` became `http://host:8000/mcp`, `/v1/rpc` became `/v1/rpc/mcp`) and refused a query string; pass the server's full MCP endpoint URL, such as `http://host:8000/mcp`. The examples now default to that form.
- **The MCP server also answers 2025-06-18, 2025-03-26 and 2024-11-05; the client accepts only known revisions.** `MCPServer` negotiates the revision a client asks for when it is `2025-11-25`, `2025-06-18`, `2025-03-26` or `2024-11-05`, as the official SDK servers do, and `2025-11-25` otherwise, and sends an older client only the fields its revision defines (no `icons` or `execution` for `2025-06-18`; also no `title`, `outputSchema`, `_meta`, `structuredContent` or `resource_link` content for `2025-03-26`, where a result with only structured data carries it as text; also no `annotations` or audio content for `2024-11-05`; a content block the revision lacks is sent as a text block holding its JSON). The HTTP transport checks `MCP-Protocol-Version` against the session's revision and uses it when the header is missing. NeoGraph's MCP client accepts the same four answers (`MCP_SUPPORTED_PROTOCOL_VERSIONS`), over stdio and Streamable HTTP; the older HTTP+SSE transport is not spoken. **Behavior change:** the server answered `2025-11-25` to every client, and a missing header on a session was 400; the client accepted any `protocolVersion`, such as `1999-01-01`, and now fails `initialize` with a protocol error and disconnects for one it does not list. MCP `2026-07-28` is not served or spoken yet; it is planned as a second mode.
- **MCP tool registration refuses what the official clients reject.** `MCPServer::register_tool` and `register_raw_tool` require an `inputSchema`, and an `outputSchema` when there is one, that says `"type": "object"` and is JSON Schema 2020-12 this validator supports, and a name of 1 to 128 characters of `A-Za-z0-9_.-` (SEP-986). `ToolDefinition::input_schema` now defaults to `{"type": "object"}`, and an empty object is read as that default. **Behavior change:** such registrations used to succeed; one tool registered without a schema made the official clients reject the whole `tools/list`.
- **An unknown MCP tool is a protocol error.** `tools/call` for a tool the server does not have is JSON-RPC error `-32602` `Unknown tool: <name>` with `data.code` `NG.ToolNotFound`, as MCP's tools "Error Handling" asks. **Behavior change:** it was an `isError` result.
- **MCP HTTP sessions expire and make room, and a cancelled call gets no answer.** `MCPHttpServer` closes a session after `session_idle_timeout` (30 minutes) without a request and, when a new client initializes at `max_sessions`, the least recently used session with no request in flight; a closed session's id answers 404, which tells the client to initialize again. Only when every session is busy is initialize refused, with 503 and `Retry-After`. **Behavior change:** sessions lived until `DELETE`, so after 64 official TypeScript clients, which do not send `DELETE` on `close()`, every new client got 503 for good. A call the client cancelled now ends its POST with an empty `text/event-stream` instead of JSON-RPC error `-32800`, which every official TypeScript client reported as a response to an unknown request; `MCPServer`'s response sink no longer gets `-32800` for it either, and a sink set with a `NoResponseSink` receives the call's id instead.
- **Hardened MCP stdio sessions keep running on stderr output, stop in order, and tell the server before they terminate.** A hardened session (`StdioClientConfig`) drains the server's stderr and keeps its last `max_stderr_bytes`; a cancelled or timed-out request sends `notifications/cancelled`, waits `cancel_grace` (500 ms) and then terminates the server; shutting down any stdio server closes its stdin and waits up to a second before SIGTERM, and another second before SIGKILL. **Behavior change:** a hardened session ended once 64 KiB of stderr had been written, and a cancelled or timed-out one killed the server at once without telling it; every stdio shutdown sent SIGTERM and SIGKILL 20 ms apart.
- **The MCP client shakes hands before any request and listens for its HTTP session.** `MCPClient::rpc_call_async` on a client that has not initialized performs the handshake first, so a hook RPC transport over MCP no longer sends its request before `initialize`. After `initialize` an HTTP client opens the optional `GET` event stream of its session, as the official clients do, so it receives the server's requests and notifications that belong to no call (FastMCP v1 sends its pings there). **Behavior change:** each HTTP session keeps one more connection open; a server without the stream answers 405 and the client stops asking.
- **Rebuild C++ consumers: Program provider-call structs gained members, and `monetary_microunits` is enforced for priced calls (#372).** `ProgramResultData` gained `monetary_records`, `ProgramCoreProviderCallContext` gained `monetary_account`, `SQLiteProgramProviderCallJournal::Record` gained `monetary_record`, and `Provider` gained the static `invocation_upper_bound`. Adopting SchemaProvider `f7ed816` grew `sp::OpenRouterRouting` by `max_price`, and with it `ProviderControls`, `SchemaProvider::Defaults` and the SDK's Chat, Messages and Responses requests, within interface revision 6. Rebuild NeoGraph, C++ consumers and Python extensions together against the matching SDK (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). The provider journal adds a `monetary_record` column to existing databases. A stored `ProgramResult` carries `monetary_records` only when a call was priced, so other results keep their bytes and ids. **Behavior change:** like every `RunBudget` amount, `monetary_microunits = 0` grants nothing, so a run with a zero monetary budget refuses every call a pricing broker prices, with `P_MONEY_BUDGET`; before, nothing charged money.
- **Rebuild C++ consumers: interrupt identity added members (#388).** `RunResult` gained `interrupt_id` and `interrupts`, `RunMetadata` gained `resume_interrupt_id`, and `ProgramPendingInputData` and `ProgramPendingEffectData` gained `core_interrupt_id`; `PendingInterrupt` is new in `graph/types.h`. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Core: a single `Send` keeps its payload to itself, as several do (#389).** Behaviour change: the payload of the only `Send` of a step no longer reaches the shared run state, the nodes of the next step or the run's final state; it is visible only to that Send's own execution, as every payload of a step with several Sends already was. A graph that read a single Send's payload after the Send step must return the value as a write instead. No example or test in this repository relied on the old behaviour.
- **Core: a resume value no longer reaches later steps (#393).** Behaviour change: a node that read `ctx.resume_value` (Python `input.ctx.resume_value`, and `ToolGateContext.resume_value` in a tool gate) in a later super-step of a resumed run now sees none, and so does every node of a resume after an `interrupt_after` pause, from a step's checkpoint or from a safe point. The value goes only to the invocation the resume answers: the node or `Send` whose `NodeInterrupt` it answers, or the node an `interrupt_before` gate held. A graph with a `messages` channel still receives it there as a user message. No example or test in this repository relied on the old behaviour.
- **Core: a failure ends its step whatever the index of a sibling that asks (#392).** Behaviour change: a fan-out step, or the Sends of a step, in which one sibling raises `NodeInterrupt` and another fails now fails the run in either order; before, an interrupt with the lower index paused the run and dropped the failure. Cancellation now stops such a step in either order. A reconciliation interrupt still pauses beside a failure, and then its checkpoint lists it under `_neograph.interrupts` even when it is the only one; every other checkpoint keeps its bytes.
- **Core: each `interrupt_before` gate of a step is asked on its own (#396).** Behaviour change: when several gated nodes are ready in one super-step, a resume answers the gate the run paused at, and the run pauses again before each other gated node of the step until every gate is answered; before, one resume ran them all. The Before checkpoint of a later gate of the step lists the gates already answered under `_neograph.answered_gates`; a step with one gate keeps its checkpoint bytes.
- **Program: joins pause for reconciliation beside a failed member (#397).** Behaviour change: in JavaScript `ng.all` (fail-fast), `ng.race` and `ng.quorum`, and in a plan-level `parallel` and `quorum`, a member paused on an uncertain effect (the Core's `tool_effect_reconciliation` interrupt) now pauses the join when a member's failure would end it, and the failure ends the join once it is resumed; before, the failure ended it at once. A join that a success decides is unchanged.
- **Program: a fork of a JavaScript `main()` continues its paused command (#395).** Behaviour change: the fork runs `main()` with the source run's input and its run record keeps that input, not the fork's; a fork whose source is not paused in a `callCore` is rejected with `P_FORK_STATE` instead of starting `main()` over.
- **Program: a fork or graph migration re-binds the provider evidence it carries (#398).** Behaviour change: a recorded failure with provider evidence that a fork or a graph migration carries into the new run is restored under the source run's custody and stored again under the new run's; a record that does not match the source run's custody rejects the fork with `P_FORK_CUSTODY` before anything is published.
- **Program: a plan quorum counts a branch whose Core node fails (#399).** Behaviour change: such a branch is a failed branch of the quorum, which goes on and ends by its own rule (met, `P_QUORUM_UNREACHABLE`, or paused by a later branch); before, the node failure failed the run at once.
- **Program: a resumed join waits for the member it answers (#400).** Behaviour change: while the member the resume answers runs, a decision that a replayed member's completion would take (a fail-fast failure, a race's first finisher, a quorum met or out of reach) is held and no further member starts; it is taken when the answered member finishes.
- **Program: a paused join asks for reconciliation first (#401).** Behaviour change: a JavaScript join or a plan-level `parallel` that pauses with no failure now pauses on its first member paused on an uncertain effect, before a member paused on an ordinary question, whatever the declaration order. A collecting `ng.all` keeps pausing on a paused member beside a failure, now documented as the deliberate exception to the #392/#397 precedence.
- **Rebuild C++ consumers: graph and Program structs gained members (#357, #358, #361, #365, #367).** `EngineConfig` gained `validation` and `stderr_notices`, `RunMetadata` and `RunContext` gained `shared_scheduler`, `ProgramCompilerConfig` gained `require_acyclic`, `ProgramCompilerConfig` and `ValidationOptions` gained `soundness_check` and `soundness_state_cap`, `GraphEngine` gained a private member and `RunStatus` gained `BarrierStalled`. For #364, `EngineConfig` gained `failure_text_redactor`, `RunMetadata` gained `node_retry_ceiling`, `RunBudget` gained `max_node_retries`, `ProgramUsage` gained `node_retries`, `CatalogConfig` gained `failure_text_redactor`, `Scheduler`, `NodeExecutor` and `GraphEngine` gained members, `RunStatus` gained `FailureExit`, and `ProgramDiagnosticError`, `ProgramCompileError` and `ProgramAdmissionError` gained the base `graph::ConfigurationFailure`. These change layouts: rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). A `switch` over `RunStatus` that lists every value needs `BarrierStalled` and `FailureExit` cases.
- **Rebuild C++ consumers: attempt timeouts changed graph layouts, and node bodies get an attempt token (#364, #367, #368).** `NodeRetryPolicySpec` gained `attempt_timeout_ms`, `NodeExecutor` gained members, and `GraphEngine` gained members and a declared destructor; rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). **Behavior change:** in a Program run, and for every attempt that has a timeout, a node body runs on a worker pool instead of the coroutine that drives the run, and its `ctx.cancel_token` is a child of the run's token that belongs to that attempt: cancelling the run still cancels it, but cancelling it cancels only the attempt. In a Program run a parallel step's node bodies therefore overlap, so its `NODE_END` events arrive in completion order. Definitions that do not use `attempt_timeout_ms` keep their bytes and ids.
- **Program: a failed node reports its classified code (#364).** A node failure without a failure edge ends a Program run `Failed` with the node failure classifier's code (`NG.TaskFailed`, an author code such as `app.compile_error`, `NG.ProviderFailed`, ...) instead of `P_RUNTIME_CORE_FAILURE`, and `failure.witness.node_failure` holds `{node, code, kind, attempts, retryable, routable}`. `P_RUNTIME_CORE_FAILURE` remains for Core failures without a node failure record. The `retry` operation of an orchestration plan asks the same classifier: it no longer runs its body again after a logic, configuration, budget or provider failure, a committed effect, or a reached retry ceiling.
- **Node retries and tool failures follow one failure classifier (#364).** The node retry loop and the tool failure contract ask one classifier, which walks the whole cause chain (`NodeExecutionError` causes, `std::nested_exception`, subgraph failures) and takes the most severe class it finds. Still retried: `std::runtime_error` and its subclasses, asio errors other than cancellation, and subgraph failures whose root is one of those. No longer retried: a Program diagnostic raised inside a node (`ProgramDiagnosticError`, `ProgramCompileError`, `ProgramAdmissionError`, in C++ or Python), which is now `NG.ConfigError` through the `graph::ConfigurationFailure` marker; running the node again reproduces it. No longer retried: a subgraph failure whose root is a `std::logic_error` or an unknown exception, any exception outside the `std::runtime_error` and `std::logic_error` families (`throw 42` included), and a quota failure behind `std::nested_exception`. A run control signal anywhere in a tool's cause chain, such as a Hook block inside the failure of a graph the tool ran, now stops the run instead of becoming a tool message. See the [node failure contract](docs/reference-en.md#node-failure-contract).
- **Every node retry attempt has its own effect namespace (#364).** Attempt 0 keeps the task id (`s3:act`), so existing journals keep their slots; attempt k uses `s3:act#a<k>`. A retry therefore asks a `ProviderCallBroker` for a fresh call slot instead of the first attempt's, and a subgraph that a retried node runs starts in a fresh child thread. Before attempt k starts, an empty pending write keyed by its namespace records the start, so a resume continues that attempt and keeps the remaining retry budget instead of starting again at attempt 0. Under a `ToolEffectBroker`, an attempt that committed a non-idempotent effect is never retried and fails as `NG.EffectCommitted`, and an effect whose outcome is unknown pauses the run for reconciliation even when the node turned that interrupt into an error.
- **Example 11 (Clay chatbot) is redesigned.** Text uses a system TrueType/OpenType font with UTF-8-aware measurement, so Hangul and other scripts render (`NEOGRAPH_CHAT_FONT`, `NEOGRAPH_CHAT_BOLD_FONT`, `NEOGRAPH_CHAT_MONO_FONT` override the choice). The window has a header with model and status, avatars, light Markdown with code blocks, a welcome card with suggestions, a scrollbar that follows new output, and a composer whose Send button becomes Stop while a reply streams; Stop and quitting cancel the reply.
- **Adopt SchemaProvider `69f8aba5c75decd21d786853de134ac713634966`, SDK 0.3.1 on `maint/interface6`, which fixes SchemaProvider #26 to #29.** The interface revision and shared-library generation stay 6 and no public struct or enum changes layout. NeoGraph now requires `SchemaProvider 0.3.1 EXACT`, in its build and in the `find_dependency` of the installed `NeoGraphConfig`, so an installed 0.3.0 package no longer matches. Rebuild NeoGraph, C++ consumers and Python extensions together against the 0.3.1 headers and libraries. What changes: direct OpenAI Chat (`https://api.openai.com`) writes the output cap as `max_completion_tokens`, which current OpenAI models require (they answered `max_tokens` with HTTP 400), while OpenRouter and compatible endpoints keep `max_tokens` and an explicit `bindings.max_output_tokens` wins on any origin; a vendor 400 that names the parameter it refused says so in `Error::safe_message` (`invalid request: unsupported parameter 'max_tokens'`); a Chat `Tool` message that holds several results is written as one wire `tool` message for each; every Claude model from generation 4.7 on refuses `temperature` locally, so `claude-haiku-5-5` no longer reaches the API to be answered with HTTP 400; a cut Anthropic stream no longer reports the placeholder output count of `message_start`; Gemini derives `output_total` and `total` when `candidatesTokenCount` is omitted; a completed HTTP 4xx answer is `RejectedBeforeOutput` (see the next entry). **Behavior change:** the output-cap member is part of the origin identity of native history, so history sealed against direct OpenAI Chat before this adoption is `ReplayIneligible` when it is continued, unless `bindings.max_output_tokens` is set to `max_tokens` for that conversation. Conversations on other origins and families, and new conversations, are not affected.
- **A provider's completed HTTP 4xx answer releases its hold and charges nothing (SchemaProvider #29).** `provider_failure_proves_not_sent` released a token reservation, a Program's monetary hold and a dispatch receipt only for `NotSent`, so a 401, a 400 or a 404 kept the whole reservation held. With SDK 0.3.1 such an answer is `RejectedBeforeOutput`: the server declined the request, so nothing ran and nothing is billed. The new `provider_failure_proves_not_accepted` is the one rule every settlement path uses (`ProviderBudgetClaim`, `ControlledProvider`, the managed budget journal, the Program's in-process and SQLite provider brokers, the MCP harness provider). It holds for `NotSent` evidence, and for a completed HTTP 4xx answer other than 408, 425 and 429 whose failure carries no output, stop reason or usage and has no earlier attempt that could have been billed. **Behavior change:** such a call used to be charged its whole reservation, was `held` in `ProgramResult::monetary_records()` and left its dispatch receipt in `ReconciliationRequired`; it now leaves no monetary record, charges no model tokens and records `Failed`. An HTTP 408, 425, 429 or 5xx answer, a cut call and a transport failure after the request left keep the hold, and so does an outcome stored by an earlier version. Usage with stage `Missing` is no evidence and `Partial` is what was seen, so neither settles a call; a derived count settles like a reported one, so a Gemini call whose output budget was spent while thinking is charged what it used, not its whole reservation.
- **Adopt SchemaProvider `f7ed8161fbcf39a6bf9358f78cfdcfda133537a2`, the then-current `maint/interface6` tip: OpenRouter `provider.max_price` (#372).** `sp::OpenRouterRouting` gained `max_price` (`sp::OpenRouterMaxPrice`), OpenRouter's cap on the endpoint price it may route to: optional integer nanoUSD amounts, `prompt` and `completion` per million tokens, `image`, `audio` and `request`, sent as exact decimal USD (`75'000'000` is `0.075`). OpenRouter then routes only to endpoints listed at or below the cap and fails the request when none is. A cap without amounts or with an amount above 2^53 is rejected before I/O. `ProviderControls::provider` carries it on Chat Completions, Responses and Messages requests, and `SchemaProvider::Defaults::provider` merges it like its other routing fields: a request's own `max_price` replaces the default one. Python exposes `ng.OpenRouterMaxPrice` (`prompt_nano_usd`, `completion_nano_usd`, `image_nano_usd`, `audio_nano_usd`, `request_nano_usd`) and `OpenRouterRouting.max_price`, which returns a detached copy: edit it, then assign the whole object back. The cap joins the native configuration binding, so changing it between turns makes captured native history `ReplayIneligible`; routing without one keeps its earlier bytes and digest. The library advertises the field with the capability bit `sp::capability::OpenRouterRoutingMaxPrice`, part of `RequiredProvider`, so NeoGraph's handshakes refuse an earlier interface-6 library that would drop the cap. SDK 0.3.0 and interface/shared ABI 6 are unchanged, but the request layouts grew: see the #372 rebuild entry. The same SDK revision also makes its HTTP transport refuse TLS versions older than 1.2 on every platform, whatever the libcurl build or TLS backend would accept by default.
- **Security hardening from code scanning.** The CI workflows run with read-only repository permissions. NeoGraph's own HTTP/2 pool now requires TLS 1.2 or newer and only speaks HTTP and HTTPS, and the loopback TLS servers in the test suites refuse older protocols too. The vendored QuickJS `js_realloc_array` computes its growth and byte size in 64 bits and fails with OutOfMemory instead of letting an `int` multiplication wrap (recorded in `deps/quickjs/NEOGRAPH_PROVENANCE`); the vendored Clay example header computes its arena size in `size_t`. The promotional video tooling under `docs/promo` takes the patched `source-map-js`, `postcss-selector-parser` and `fast-uri`.
- **Adopt SchemaProvider `0d536be66017dfb8c4c91d21f4f62bd8ce698d55`, the then-current `maint/interface6` tip.** Graph and agent tool loops on OpenAI Responses no longer fail with HTTP 400 (`InvalidRequest`) when a tool's parameter schema is not strict-compatible, for example an MCP tool schema that leaves `additionalProperties` open: the policy's strict default now applies only to schemas in which every object sets `"additionalProperties": false` and lists each property in `required`, and other tools are sent with `"strict": false`. An explicit `strict = true` on an incompatible schema fails before dispatch with `InvalidRequest` naming the tool and the first failing JSON pointer. Chat streams from OpenAI-compatible local servers whose `created` timestamp advances between chunks no longer fail as `ProtocolCorrupt`; a decrease, or a changed `id` or `model`, still does. A vendor failure the codec has already admitted in-band, such as a Responses `response.failed` event or a buffered failed body, is now classified from its own error code whatever its size. Before, one over the runtime's `max_error_bytes` (64 KiB by default), for example a 3 MB `response.failed` repeating a hosted `image_generation` result, was reported as `ResourceLimit` with `RetryClass::Never` instead of the vendor's failure. Errors the runtime buffers and parses itself remain bounded by `max_error_bytes`. SDK 0.3.0 and interface/shared ABI 6 are unchanged.
- **Adopt merged SchemaProvider PR #25 at `f7cb27e68ee02f3a27d01c31af85bcac96f00963` on `maint/interface6`.** `Usage::input_uncached` now follows one rule in every family (input − cache read − cache write, an absent write counter counting as zero only on wires that never send it): direct OpenAI Chat now reports it and Responses subtracts a reported cache write. Gemini stop kinds are pinned by tests. SDK 0.3.0 and interface/shared ABI 6 are unchanged.
- **Adopt merged SchemaProvider PR #23 at `45dd2ad36e18717adae3e2d10b2b9c8bb0e85471` on `maint/interface6`: usage that matches the vendors exactly.** Verified with real API calls: OpenRouter Messages streams no longer fail on the trailing `[DONE]` and carry their reported cost; Chat keeps vendor detail counters such as `video_tokens`; reported USD amounts equal the vendor decimals (no +1 nanoUSD), so input + output == total; Gemini reports zero thoughts when its total proves it; Interactions keys modality counts by name; Anthropic aliases such as `claude-haiku-4-5` are accepted when answered with a dated model. Stored outcomes may now carry `CostRounding::CeilingShortestDecimal`; earlier records still decode, and Python exposes `ProviderCostRounding.CeilingShortestDecimal`. SDK 0.3.0 and interface/shared ABI 6 are unchanged.
- **`llm::Agent` and the deep-research and plan-execute tool loops continue a paused turn (#311).** On `sp::StopKind::PauseTurn` (Anthropic/Chat `pause_turn`) they resend the history, paused assistant turn included, within their existing iteration budget instead of returning the paused turn as final. `paused_turn()` exposes the same check. The reference now documents the Responses `response.done` alias rule and the declared-gateway served-model rule.
- **Adopt merged SchemaProvider PR #21 at `83112573ba59e3b561fc33c22394638be7aa5294` on its `maint/interface6` branch: the same SDK 0.3.0, interface/shared ABI 6 plus the routed-gateway fix.** Responses and Messages now accept the concrete model a declared OpenRouter origin returns for a routing alias such as `~deepseek/deepseek-v4-flash-latest`; before, every such Responses call failed with `invalid response protocol (HTTP 200)`. The served model must stay stable within a response and replay stays bound to the requested model. No NeoGraph API or ABI change.
- **Adopt merged SchemaProvider PR #16 at `3b88e4ba020c3a4d39ff0660014e7292b516b7cb`: rebuild with SDK 0.3.0 EXACT, interface/shared ABI 6.** This supersedes the earlier interface-5 SDK steps recorded below. Blocking calls use the single async SDK pipeline and retain event-loop/callback-worker re-entry rejection. Full outcomes, usage observations, journals, native archive reload and Python typed values retain provider-reported monetary fields, statuses, source, evidence, rounding and conflicts. Canonical v2 JSON omits `provider_cost` only when entirely absent, preserving old absence-only bytes. Monetary evidence, including zero and BYOK false, prevents an unknown token hold from being refunded as proven-not-sent; money is never converted to tokens or spending authority. Native archive v3 and portable JSON v2 stay unchanged.
- **Core engine speed is back at v0.11.1 level.** In the historical measurements, the 3-node sequential benchmark went from 10.19 to 6.29 µs per run (v0.11.1: 6.10) and the parallel-3 one from 30.03 to 15.66 µs (15.80); per-thread time with one engine per thread matches v0.11.1 at 2, 4 and 8 threads. The loss came from a strand per fan-out branch, per-node `RunContext` copies, one process-wide mutex around the context binding registry (now 64 shards), state snapshots built from many JSON documents, coroutine calls that returned nothing (`run_sends_async` without Sends, `record_pending_write_async` without checkpointing) and ordered sets in the scheduler. Per-commit measurements are in #343.
- **A program that links only `neograph::core` no longer loads libcurl.** `neograph::core` linked SchemaProvider's runtime, which linked its libcurl transport, so a graph-only process loaded libcurl and the roughly 25 libraries it needs: 12.7 MB resident at start for the benchmark, against 5.0 MB for v0.11.1. Only `neograph::llm` links the transport now; the same benchmark starts at 7.7 MB with 7 shared libraries instead of 36. Code that constructs `sp::runtime::Client` itself without `neograph::llm` must link `SchemaProvider::transport`. SchemaProvider is pinned to the revision that makes the split. See #347.
- **Rebuild with SchemaProvider 0.2.0 alpha, interface/shared-library generation 5.** The SDK adds optional connect, first-byte and idle bounds: off by default, never extending the total deadline, and settable per call through `ProviderRequest::options` (an `sp::runtime::RunOptions`); graph LLM nodes do not expose them yet. Five-family real-transport lifecycle coverage and self-contained, private test fixtures close SDK issues #2, #3 and #4. Rebuild C++ consumers and Python wheels with matching SDK headers and libraries; native archives remain v3 and portable JSON remains v2.
- **A program that links only `neograph::core` no longer loads libcrypto.** The SDK implements SHA-256, HMAC-SHA256 and operating-system entropy itself, so its runtime no longer links OpenSSL; archive and policy bytes are unchanged. The graph-only benchmark (`bench_neograph 1 1 1`) starts at 6.3 MB resident instead of 7.7 MB (median of 31 runs, Linux x86_64) with 6 shared libraries instead of 7, the same as v0.11.1, which started at 5.0 MB. Code that calls OpenSSL directly must link `OpenSSL::Crypto` itself.

### Added

- **Program: a flat step plan (`step-plan/v1`) is checked and lowered into a task graph proposal.** `intake_step_plan` (`neograph/program/step_plan.h`) takes the text of a planner's answer, or a parsed document, and either returns every problem as a sorted list of violations `{path, code, message, hint, detail}` with stable `STEP_*` codes, or lowers the plan, byte for byte deterministically, into a schema-2 `TaskGraphProposal` with a source map back to the steps. A plan is an ordered list of steps that name a template of the host's allow-list, give each input a literal or a reference to an earlier step, and may add ordering edges and a retry hint. The format is closed, has no provider, credential, budget or executable text, and cannot hold a cycle. The lowered proposal takes the unchanged trusted path (`TaskGraphProposal::parse`, `TaskGraphFragmentCompiler::compile`, `expand_task_graph`); passing intake means the plan is well formed and inside policy, not that it solves the request. In feedback, strings taken from the plan are cut to 64 printable bytes and literal values are never echoed. `format_violations_for_model` renders the violations as bounded prompt text, `step_plan_json_schema` generates a JSON Schema for a catalog, and `schemas/step-plan-v1.schema.json` is the catalog-independent shape. See [`docs/plan-intake.md`](docs/plan-intake.md).
- **Program: a bounded plan repair loop.** `add_step_plan_repair` registers `neograph.step_plan_intake/v1`, `neograph.step_plan_route/v1` and `neograph.step_plan_report/v1`, which make `plan -> intake -> (plan again)` an ordinary Core graph. The intake node checks the planner's last answer, writes the lowered proposal and a report (`status`, `attempts`, `last_violations`, `violation_code_histogram`, `plan_digests`, `envelope`) to channels, and rebuilds `messages` with the violations: `feedback` `replace` (the default) keeps the context from growing, `append` keeps the conversation. The attempt count, the run's budgets, the provider call journal and the Core checkpoints bound and record the loop, so a resumed run does not ask the model again. A loop that gives up ends in a fallback branch or in a failure exit (`plan.repair_exhausted`). The host's allow-list and limits are options of the registered executables, so the sealed graph cannot change them. `repair_step_plan` runs the same loop around a callback for hosts that plan before they start a run.
- **Program: task graph tasks take literal inputs and attempts, and the scheduler retries (proposal and fragment schema 2).** A schema-2 task may carry `literals` (JSON values assigned to declared input fields after the bindings) and `max_attempts`. The new codes are `P_PROPOSAL_LITERAL_TARGET`, `P_PROPOSAL_LITERAL_DUPLICATE_TARGET`, `P_PROPOSAL_LITERAL_SIZE` and `P_PROPOSAL_ATTEMPTS`, and a schema-1 document that uses either member is `P_PROPOSAL_SCHEMA_VERSION`. A host opts in with `TaskGraphProposalLimits::max_literal_bytes`, `max_total_literal_bytes` and `max_attempts`; the defaults (0, 0 and 1) refuse literals and retries. The aggregate reserves a task's budget once per attempt, and each attempt's child starts with the whole budget. When a task's child failed with a retryable Core node failure (the rule of the plan-level `retry` operation) and attempts remain, the scheduler dispatches the task again with the next attempt number, a new attempt identity and a new child. Nothing is written as `Failed` for an attempt that will be retried, a dependent waits for it, and `fail_fast` is not triggered by it. Documents and fragments that use neither member keep their bytes and identities.
- **Core: a node can tell which interrupt a resume answers (#406).** `RunContext::resume_interrupt_id` (Python `input.ctx.resume_interrupt_id`) is the id of the interrupt that `resume_value` answers when it answers the node's own `NodeInterrupt`, the id `RunResult::interrupt_id` and `RunResult::interrupts` report, whether the resume named it or answered the first one. It is empty (Python `None`) when the value is the approval of the `interrupt_before` gate that held the node. A node behind a gate that also asks its own question no longer has to take the approval to run as its answer: it asks while the id is empty. Which invocation receives a resume value is unchanged (#393). `RunContext` gains a member: rebuild native consumers against matching headers.
- **A2A: `ListTasks`, `SubscribeToTask` and non-blocking sends on the native server.** `ListTasks` filters by `contextId`, `status` and `statusTimestampAfter` and pages newest first; `SubscribeToTask` (0.3 `tasks/resubscribe`) streams the current task and its live events; `returnImmediately` (0.3 `blocking: false`) returns the submitted task at once. Every run executes on its own worker, so a task outlives a dropped connection, and every status carries a timestamp. Push notifications, the extended card and the HTTP+JSON/gRPC bindings stay unclaimed: their methods answer `-32003` / `-32004`.
- **ACP: client-supplied stdio MCP servers** start through NeoGraph's MCP client, and nodes reach their tools with `ACPClient::session_tools()`. A server gets a minimal environment, the agent's `PATH` (on Windows also `SystemRoot`) plus the client's `env`, never the rest of the agent's environment, which may hold provider API keys.
- **A2A: the card declares the collaboration envelope as an extension.** A Program agent with a `CollaborationMailbox` lists `a2a::COLLABORATION_EXTENSION_URI` in `capabilities.extensions` (`AgentCard::extensions`), so the sender-named, idempotent task ids of collaboration messages are a declared extension (A2A spec 4.6).
- **ACP: incremental updates.** Model tokens stream as `agent_message_chunk`, and tool calls are reported as `tool_call`/`tool_call_update`.
- **Official-SDK interop suites and a protocol-interop CI job.** `bindings/python/tests/interop/a2a_suite` drives the native A2A server and client with a2a-sdk 1.2.2 and @a2a-js/sdk 1.3.0 and 0.3.14, validating every message against the A2A v1.0.1 proto and JSON Schema and the v0.3.0 JSON Schema; `acp_suite` drives the native ACP agent (`neograph_acp_probe_agent`) with agent-client-protocol 0.12.1 and 1.0.0rc2 and @agentclientprotocol/sdk 1.8.0, validating every message against the ACP schema v1.25.0. The suites pin their SDKs, skip cleanly when the pinned SDK is not installed, and run in `.github/workflows/protocol-interop.yml`, which installs the pins and Node.js 22.
- **MCP: JSON-RPC batches for 2025-03-26 and 2024-11-05 peers.** MCP 2025-03-26 requires a receiver to accept JSON-RPC batches. `MCPServer` and `MCPHttpServer` now answer a batch from a client that negotiated `2025-03-26`, or `2024-11-05`, which defined none but whose peers are treated alike, with one array, on stdio and over HTTP (HTTP 202 for a batch of notifications only); a cancelled call is left out, and a batch with nothing left to answer gets nothing. Later revisions removed batches, and initialize is never part of one, so such a batch is one `-32600` error (HTTP 400), as before. `MCPServer::handle_message(message, reply)` hands each message's outcome, a response, a batch's array or none, to its caller; `MCPHttpServer` and the stdio loop use it. NeoGraph's MCP client handles a batch a server sends message by message and answers the requests in it with one array. The MCP interop suites validate each session's messages against the schema of the revision it negotiated.
- **MCP: protected resource metadata, bounded initialize, tool list changes, inherited environments.** `MCPHttpServerConfig::protected_resource_metadata` publishes RFC 9728 metadata for OAuth deployments and names it in every 401 challenge with `scope`. `MCPClient::initialize(name, deadline, cancel_token)` bounds the handshake. `MCPClient::set_tool_list_changed_handler` reports `notifications/tools/list_changed`. A `StdioClientConfig` without `replace_environment` now takes `environment` entries that add to or override the inherited environment; they were refused.
- **Python MCP client: transport errors, hardened stdio, shutdown and bounded calls.** `mcp.MCPTransportError` carries `failure` and `http_status`, `mcp.MCPError` also carries `http_status`, `mcp.StdioClientConfig` and `MCPClient(config)` launch a hardened stdio server, `MCPClient.shutdown()` ends a client for good, `initialize()` and `call_tool_result()` take `timeout` and `cancel_token`, and `set_tool_list_changed_handler()` takes a callable.
- **Official MCP SDK interop suites and their CI job.** `bindings/python/tests/interop/mcp` runs the official Python SDK (`mcp` 2.3.0 and 1.30.0) and TypeScript SDKs (`@modelcontextprotocol/sdk` 1.32.1, `@modelcontextprotocol/client` and `@modelcontextprotocol/server` 2.3.1) against NeoGraph's MCP server and client, and validates every captured message against the MCP 2025-11-25 `schema.json`; the new `MCP interop` workflow installs the pinned SDKs and Node and runs them. Without the SDKs the tests skip.
- **Program: an opt-in soundness check (#363, #364, #365).** `ProgramCompilerConfig::soundness_check` (Python: `soundness_check=True` on `ProgramCompiler` and `LocalProgramHost`; `GraphValidator`: `ValidationOptions::soundness_check`) explores every run of the root graph that its routes allow. A state is the ready set, the signals each join holds (`any_of` slots included) and the visit counts of nodes with `max_visits`; conditional routes and failure edges are nondeterministic choices, and a failure exit ends the run. The check returns warnings whose witness is the shortest route sequence that shows them: `P_CORE_S1` (from some point no run can complete), `P_CORE_S2` (a run ends while a join holds signals, barrier-stalled or through an explicit end), `P_CORE_S3` (a join that never fires), `P_CORE_S4` (a loop with no exit) and `P_CORE_S5` (two writers of one overwrite channel ready in the same super-step, beyond the fan-out siblings of `P_CORE_E5`). Their help points at the route choices of the witness and, for a stall that one node outside the slots could fill, suggests the `any_of` slot. A `P_CORE_SOUNDNESS` summary is a note when every run was explored and a warning when not: runs after a node whose type declares `Command.goto` or `Send` (effect contract `"control": ["goto"]` or `["send"]`) are not analyzable, and `soundness_state_cap` (default 100 000 states) stops the check as not checked. Neither case claims what was not explored. Nothing is sealed, so bundle ids do not change. On every graph the bounded-exhaustive join harness enumerates, the verdicts agree with its reference interpreter and each witness replays step by step. The capability manifest adds the flag `diagnostics.soundness_check`.
- **Program: failure routing keys compile (#364).** `graph.retryPolicy(node, {max_retries, non_retryable, max_visits, error_text, on_failure, error_channel, ...})` lowers to the strict Core keys `node_retry_policies` (only the fields written, with bounds such as `max_retries` 0 to 10 and `max_visits` 1 to 100) and `failure_edges`, and `graph.failureExit(node, {code})` writes `failure_exits`. The compiler checks them (`P_CORE_GC_FAILURE_EDGE` for a sentinel, self, join or implicit-end target, a second edge from one node or a non-append error channel; `P_CORE_GC_FIELD_VALUE` for bounds and codes; `P_CORE_E3` for unknown names), adds failure edges to the routed successor map (E7, E11 and E15 see them), counts an `error_channel` as written, and warns with `P_CORE_E18` about a `retry_policy` key in node config that nothing reads. `on_failure` in the graph-level policy is an unknown field with a hint. The keys are sealed and part of the migration shape, except `node_retry_policies`; definitions without them keep their ids. The runtime that acts on them is the next entry. `TopologySpec` and `CompiledGraph` gained members. See [Program graph features](docs/program-graphs.md).
- **Failure routing runtime (#364).** A node whose final failure is routable and that has a failure edge no longer fails the run: its failed attempts write nothing, one structured record (`node`, `kind`, `code`, `attempts`, `retryable`, `step`, `routed_to`, `effects`) is appended to the edge's `error_channel`, and the node routes to the edge's target only. Static nodes, fan-out branches, one or many `Send` targets and subgraph nodes take the same path, a resume after a crash replays the route from the pending write without running the node again, an `interruptBefore` target pauses with `{"reason": "failure_route", "failure": record}`, and the `ERROR` event gains `handled` and `routed_to`. Free text is opt-in per node (`error_text`), comes only from a typed `public_message`, needs the host's `EngineConfig::failure_text_redactor` (`CatalogConfig::failure_text_redactor`; Python `failure_text_redactor=` on `GraphEngine.compile` and `LocalProgramHost`), and is stripped of invisible characters and cut to 512 bytes. Retry timing is resolved per field, and a host per-node policy may only tighten a node whose definition declares one (`GraphEngine::resolved_retry_policy`); `non_retryable` codes are not retried. `max_visits` routes the activation past the limit as `NG.VisitLimit`, with its counter in checkpoint metadata. A failure-exit node ends the run with `RunStatus::FailureExit` and `output["_neograph"]["failure_exit"]`, and a Program run `Failed` with the declared code. `RunBudget::max_node_retries` (Python `ProgramRunBudget.max_node_retries`) caps node retries across a run, reported as `ProgramUsage::node_retries`; both are serialized only when set, so existing identities do not change. The manifest flags `retry_policy.per_node`, `failure_edges` and `failure_exits` are on. See [Program graph features](docs/program-graphs.md#failure-routing).
- **Attempt timeouts and deadline-aware backoff (#364).** `graph.retryPolicy(node, {attempt_timeout_ms})` (Core `node_retry_policies[node].attempt_timeout_ms`, 100 to 3 600 000) limits each attempt of a node. A timed-out attempt is cancelled through its own child `CancelToken` and fails with `NG.Timeout`, which is retried and routable; the first terminal event wins, so a completion at or after the timeout is the timeout. The attempt's late completion is discarded: its writes, result and events never reach the run, a mediated Tool call it starts is refused, and the next attempt starts only after it has stopped. One that does not stop by its stop deadline (the timeout again, at least one second) fails the run with `NG.EffectUncertain`; it runs on in the background and keeps its node alive, and `wait_for_node_attempts()` waits for it (the Python binding does at interpreter exit). When the backoff before the next attempt would cross the run's deadline (a Program's `wall_time_ms`, `RunMetadata::deadline`), the node stops retrying at once with kind `deadline` instead of sleeping until the budget ends the run, and the `retry` operation of an orchestration plan does not run its body again after such a stop. Static nodes, fan-out branches, `Send` targets, subgraph nodes and the sync and async entry points behave the same, and a crash-resume continues the attempt that was running. `GraphEngine::set_node_attempt_timeout_ms` and `resolved_attempt_timeout_ms` (Python too) let a host set a timeout, and only shorten a definition's. `NodeAttemptTimeout` is the cause of a timed-out attempt's failure. The manifest flag `retry.attempt_timeout` is on. See [Program graph features](docs/program-graphs.md#attempt-timeouts-and-the-deadline).
- **Program: bound `llm_call` providers under monetary budgets (#372).** Python hosts bind a provider to Program `llm_call` nodes: `ProgramRegistryBuilder.add_provider(name, version, digest)` declares it, `add_registered_node("llm_call", ..., provider=name)` requires it, and `LocalProgramHost(..., node_context=ng.NodeContext(provider=..., model=..., provider_controls=...), binding_label=...)` binds it through a `CatalogCapabilityBinder`, the pattern of the MCP harness. Only the validated, non-secret label enters identity, in the provider's capability receipt; keys and models never do, and a Program that requires no provider keeps its bundle and version ids. Every bound call is priced against the run's `monetary_microunits` from the price catalog, now compiled into `neograph::program`. Before dispatch the broker reserves the catalog upper bound for the call's input bound and output cap, the route ceiling on OpenRouter, and refuses a call it cannot bound, or whose reservation does not fit, with a `P_MONEY_*` failure before any request is sent. After the outcome it settles the reported cost (OpenRouter `usage.cost`, evidence `reported`) or else the catalog estimate (`estimated`, naming the catalog version and digest), and keeps the reservation when the outcome or usage is unknown (`held`). `ProgramResult::monetary_records()` (Python `result.monetary_records`, `ng.ProgramMonetaryRecord`) lists the calls and `ProgramUsage::monetary_microunits` is their sum. C++ hosts opt in with `bind_priced_provider_call(context)` or `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)`, which keeps each call's record in its journal row: a call journaled before a crash is charged once after recovery, also when an expired execution lease ends the run, and a call an earlier attempt journaled is not charged again. `LocalProgramHost(sqlite_directory=..., native_history_archive=...)` keeps runs, checkpoints and that journal in SQLite files, and `reconnect(run_id)` continues a run a stopped process left running. On OpenRouter the broker pins the route ceiling on Chat Completions, Responses and Messages requests as `provider.max_price`, keeping a lower cap the host already sets field by field, so OpenRouter routes only to endpoints listed at or below the price the call was reserved at. See [pricing evidence](docs/reference-en.md#pricing-evidence-and-the-price-catalog) and the [Python guide](docs/python-binding.md#model-calling-nodes).
- **Typed node failures (#364).** `NodeFailureError(code, public_message, retryable)` fails a node with an author code such as `app.compile_error`, and `retryable = false` stops its retry loop. `node_failure_of()` returns the `NodeFailure` record on the `NodeExecutionError` the executor throws: code, class, why retrying stopped, attempts, the retryable and routable flags, `public_message` and the final attempt's Tool effects. A Python node's exception is read the same way through its `code`, `retryable` and `public_message` attributes, and the caller still receives the original exception.
- **Program: witness paths, the `any_of` suggestion and the route table (#363, #365).** A `P_CORE_E15` witness now has `paths`, the route sequence from the barrier member through each bypass route, and when that sequence comes back to the join, `substitute` and `suggested_wait_for`: the returned diagnostic suggests the `any_of` slot that lets the node before the join stand in for the member (for #357, `[{"any_of": ["B", "FIX"]}, "C"]`). A stalled run's witness adds the same `paths` and `suggested_wait_for`, derived from the trace and the routed successor map. `ProgramCompilerConfig::echo_routes` (Python: `ProgramCompiler(..., echo_routes=True)`) returns the routed successor table as a `P_CORE_ROUTES` note, and `GraphValidator::routed_successors()` exposes it. None of this is sealed.
- **Program: `any_of` slots in barrier joins (#363).** `barrier("D", [{any_of: ["B", "FIX"]}, "C"])` waits for one alternative of each slot: a slot is filled when any alternative signals the join, the join runs when every slot is filled, and then all signals clear. The #357 shape now completes on both routes. An alternative that arrives after the join ran opens a round that never completes, so the run ends `P_CORE_BARRIER_STALLED` (fail-closed); the new warning `P_CORE_E17` reports alternatives that can both run. A slot with one name is that name, so legacy-equivalent specs keep their bytes and ids; other specs print sorted by UTF-8 bytes into a new `TopologySpec::join_specs`, which `CompiledGraph` and `Scheduler` carry too. The validator checks E3, E8 and E15 per slot, stall witnesses print slots, and migration adapters list slot joins as barriers. Modes such as any or quorum joins remain deferred. `Scheduler` gained a member and a constructor, a layout change. See [Program graph features](docs/program-graphs.md).
- **Program: graph metadata (#362).** `ng.graph("plan").metadata({...})` stores host-readable author claims in the Core definition as a top-level `"metadata"` object, readable from `bundle.sealed_core_definitions()` (Python: `bundle.sealed_core_definitions[0]["definition"]["metadata"]`). It is hashed with the definition and never reaches node factories, state, checkpoints, outputs or events. Limits: 8 KiB of canonical JSON, nesting depth 4, 64 members, keys of 1 to 128 bytes; invisible characters are errors (`P_CORE_GC_METADATA_LIMIT`, `P_CORE_GC_METADATA_TEXT`). `GraphSemanticMigrationAdapter::metadata_changes()` lists metadata differences, which do not change `topology_shape_id()`. A Core definition that uses a Program API v2 key, metadata included, seals no validator warnings; programs without one keep their bundle ids. `TopologySpec` and `CompiledGraph` gained a member, a layout change. See [Program graph features](docs/program-graphs.md).
- **Program: the JavaScript capability manifest publishes feature flags, signatures and argument schemas (#365).** `javascript_authoring_capability_manifest()` adds `features` (one boolean per authoring feature, such as `graph.barrier`, `graph.metadata` or `failure_edges`, so a host or a model can see what this build accepts), a TypeScript `dts` signature for every method with shared declarations in `dts_prelude`, and a JSON Schema for every graph-builder argument. New schema files: `schemas/javascript-capability-manifest-v1.schema.json` describes the manifest, and `schemas/core-definition-v1.schema.json` describes a sealed Core definition for constrained decoding.
- **Program: diagnostics carry self-repair help (#365).** Every diagnostic a compilation returns (`ProgramCompileError::diagnostics()`, `ProgramBundle::compile_diagnostics()`, the `.diagnostics` of the Python exceptions) has `help`: a link to its entry in the new [diagnostics guide](docs/diagnostics.md), labelled related locations, suggestions as RFC 6902 JSON Patch edits with an applicability (`machine`, `maybe` or `placeholder`), and `candidates`, the declared names within two edits of an unknown one. Diagnostics about one unknown name collapse into one, with the other sites as related locations, and at most 20 are returned, errors first, followed by a `P_DIAGNOSTICS_TRUNCATED` note. JavaScript messages name the builder call, for example `graph.retryPolicy({...}) has an unknown field 'on_exhausted'`. Sealed diagnostics never carry `help`, so bundle ids do not change. `program::Diagnostic` gained a member, which changes its layout: rebuild C++ consumers (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Program: `P_JS_CONTROL_CHARACTER` warning (#361).** A JavaScript config string containing C0 control characters other than tab, newline and carriage return (for example `"\b"` meant as a regex word boundary) warns at the string's call site. It is reported in the diagnostics returned from compilation only.
- **Topology validation follows the routes the scheduler takes and reports four more problems (#357, #358).** A node with a conditional edge routes only through its first conditional edge, so E7 (unreachable node), E8 (barrier liveness) and E11 (no path to `__end__`) no longer count that node's static edges or later conditional edges. E13 reports a static edge out of a node that also has a conditional edge, which never fires. E14 reports a second conditional edge from the same node, which never routes. E15 (warning) reports a barrier member with a route that leads somewhere other than the join or `__end__`; when that route is taken the run ends barrier-stalled. E16 (error, opt-in) reports any cycle, for hosts that need a DAG; enable it with `ValidationOptions::require_acyclic`, `ProgramCompilerConfig::require_acyclic` or `require_acyclic=True` on Python `ProgramCompiler` and `LocalProgramHost`. An effects contract that lists neither reads nor writes now counts as undeclared, so E6 no longer reports the channels a Python node writes as dead (#361). **Behavior change:** E13 and E14 are errors in strict documents (`schema_version` 1 or later, which includes every JavaScript Program), so such graphs no longer compile; legacy documents get warnings. Bundle ids do not change: Program compilation fails on the current rules and returns their warnings through `compile_diagnostics`, but seals the warnings of the earlier rules (`ValidationOptions::original_rules`), and admission links with those rules, so bundles sealed earlier keep materializing. `EngineConfig::validation` selects the rules for `GraphEngine` build and link.
- **`EngineConfig::stderr_notices` (#361).** It defaults to `true`. When `false`, `build()` and `link()` do not print validation lint to stderr (errors still throw), and the one-time notice that fan-out runs serially is not printed. The Program Catalog links its engines with it off, so `compile_admit` no longer prints `[neograph] lint [E6] ...` and Program runs no longer print a notice that names `EngineConfig::worker_count`, a setting `LocalProgramHost` users could not reach. Program lint reaches the caller only through diagnostics.
- **Python: `LocalProgramHost(..., worker_count=1)` (#370).** It sets the worker pool of each Core engine, through `CatalogConfig::worker_count`, for parallel fan-out inside a graph, and must not exceed an admitted Program's `max_concurrency`. The class docstring describes `scheduler_threads` and `worker_count`.
- **`ChatTool::strict` sets OpenAI Responses strict mode per tool; rebuild C++ consumers.** The flag is a `std::optional<bool>`, unset by default, and `make_provider_request` carries it into the Responses function tool. Unset keeps the SDK default described above, `false` is sent as given, and `true` on a schema that is not strict-compatible fails before dispatch with `InvalidRequest`. The other families cannot send the flag, so they reject `true` with `std::invalid_argument` (`ValueError` in Python) and accept `false`. Python: `ChatTool(..., strict=None)` and the `strict` attribute. `ChatTool` gained a member, which changes its layout: rebuild NeoGraph, C++ consumers and custom `Tool` implementations, and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- `CheckpointCoordinator::records_pending_writes(parent_cp_id)`: true when `record_pending_write[_async]` would store something, so callers can skip the call.

### Fixed

- **Program: `reconnect()` of a run that holds an execution lease says why (#421).** A `define()`-only Program run without child budgets holds an execution lease for its wall time, so `reconnect()` raised `P_RUN_LEASE_HELD` with the text "owned by another live runtime" until the lease expired, which looked like a requirement of child budgets. The error now names the lease, says that `reconnect()` ends the run `TimedOut` once the lease has expired, and names `main()` and child budgets (`max_child_depth` and `max_total_children`) as what holds no lease and is continued from its last checkpoint; its witness carries `expires_at_ms` and `retry_after_ms`. Which runs hold a lease is unchanged. The Program guide lists the rule under restarting a run.
- **Program: a fork takes a share of the source's amounts, none of its concurrency, and never leaves it unable to resume (#414).** The whole of a fork's budget was taken out of the paused run's remaining budget, `max_concurrency` included, so a fork given the run's whole `remaining_budget` left the source with no concurrency (`P_CONCURRENCY_BUDGET`, `P_TRANSITION_CONFLICT`) or no wall time (`P_RUN_LEASE`, or `TimedOut` at 0 ms with a `main()`). Wall time, model tokens, money, Program operations, Core steps, dynamic compiles and child budgets are amounts: the fork takes its share and the source keeps the rest. `max_concurrency` is a limit: the fork carries its own, at most the source's, and the source keeps its own; the transition stores check that a fork leaves it unchanged. **Behavior change:** `fork()` raises `P_FORK_SOURCE_BUDGET`, and changes nothing, when the fork would take all the wall time or all the Core steps the source has left. A fork given the whole `remaining_budget` of a paused run was accepted before; give it a share. The Program guide and the Python binding guide state the rule.
- **Program: the model-token budget ends a run in one shape (#420).** A reply that took the run over `model_tokens` ended it `BudgetExhausted` with no `failure`, while a call whose reservation did not fit and was refused before it was sent ended it `Failed` with `NG.BudgetExhausted`, so a caller had to read `status` in one case and `failure.code` in the other. Both now end `BudgetExhausted` with `failure.code` `NG.BudgetExhausted`, the code `graph::classify_failure` gives a budget exhaustion and the MCP, gRPC, A2A and ACP hosts report for it. `failure.witness["model_tokens"]` holds `granted`, `used` and `over_by`, how far the last reply went over (0 for a refused call, whose failure keeps its provider evidence). A run that is granted no `model_tokens` and calls a model gets the record too. **Behavior change:** the run that a reply took over the budget now has a `failure`, and the run whose next call was refused before sending ends `BudgetExhausted` instead of `Failed`.
- **Program: with `main()`, `remaining_budget.max_node_retries` is the ceiling less the retries taken (#419).** A Program with a JavaScript `main()` reported the ceiling it started with, as if no retry had been taken (5 where `usage.node_retries` was 1), because the remainder of a command was settled without the attempt's retries; a `define()`-only Program already reported 4. Both now report the ceiling less the retries of the attempt, and a resumed attempt starts from that remainder.
- **Program: a retried failure routed to an `interruptBefore` handler pauses under `main()` and a retry ceiling (#418).** A run with a JavaScript `main()` and `max_node_retries`, whose node used its retries and was routed to an `interruptBefore` handler, ended `Failed` with `P_TRANSITION_CONFLICT` instead of pausing `Interrupted`. The `callCore` holds a reservation until it pauses, and the check on the transition that releases it required the remaining retry ceiling to be the one the call started with, although the retries the call took had lowered it. The check now accepts the ceiling reduced by the retries the attempt used, no higher and no lower, and a run without a ceiling still has none. **Behavior change:** such a run now ends `Interrupted`, with the remaining ceiling reduced by the retries used, and can be resumed. A run without `main()`, without the ceiling or without retries was not affected. The in-memory, SQLite and PostgreSQL stores share the check, and stored data is unchanged.
- **Program: a provider failure that carries native state publishes on a host without a native archive (#426).** A stream cut in the middle, or any provider failure that returned part of a reply, left the run's terminal state unpublished on a host with no `native_history_archive`, which includes the in-memory `LocalProgramHost`: stderr showed `terminal publication exception ... Program provider evidence lacks durable native custody`, `run()` returned the failed result, and `reconnect()` reported `P_RUN_LEASE_HELD`. A provider's native replay state may be stored only under an archive's authenticated custody, so the whole failure was refused. Such a host now publishes the failure with the portable part of its evidence: the same messages, usage, stop reason, error and raw events, each message without its native seal, and the witness entry `provider_outcome_native_retained: false` says so. Refusing the configuration at the start would reject every in-memory host that binds a provider, and dropping the evidence on every host would lose what a host with an archive can keep, so the fallback applies only where there is no archive. **Behavior change:** on a host without an archive, `failure.provider_outcome` of the result holds the messages without their native state (the original outcome stays on `provider_cause`), and the run can be reconnected and read. A host with an archive stores the native evidence as before, and a failed archive write still leaves the failure unpublished.
- **A2A: `SubscribeToTask` on a Program task recovered after a restart follows its run to the final status (#413).** A server that recovers its Program tasks from the mailbox holds no channel for a run it did not start in this process, so the stream sent the Task and closed at once, without a final status. The stream now follows the durable run until the run has a result or a cancel commits, then sends the result artifact and the final status as for a run the server started (a run that paused ends on `TASK_STATE_INPUT_REQUIRED`). A recovered run that settled before the call is read first and answered as terminal with `-32004`. When the run cannot be followed, the stream ends with a `-32603` error event (ErrorInfo reason `RUN_UNAVAILABLE`), or the call is answered with `-32603` when the task has no run here, instead of closing empty. **Behavior change:** a subscriber to a recovered running task now holds its connection open until the run settles, and `A2AServer::stop()` waits for it, as it waits for the streams of runs the server started.
- **A Python process exits cleanly while an engine with Python nodes is still alive (#429).** A script that ended with an engine still referenced (a module global, say) after `run_async` could crash at interpreter exit, about 1 run in 10 under load, with SIGSEGV or SIGABRT: the thread behind `run_async`, still releasing the Python objects of the last run, took the GIL while the main thread was finalizing the interpreter, `PyThread_exit_thread` made it exit, and the forced unwind ended in `std::terminate` or in destructors that touched thread state already freed. A run that was still executing Python code when the script ended crashed every time. The module now registers an `atexit` hook when it is imported. Before finalization starts it waits for node attempts that outlived their run, stops accepting `run_async` calls, lets the callbacks that are running finish (with the GIL released) and joins the `run_async` thread and the pools that run Python node and tool bodies. What a thread without the GIL releases after that, such as the handlers of an abandoned run destroyed when the process exits, is dropped without calling into the interpreter. **Behavior change:** `run_async`, `run_stream_async` and `resume_async` raise `RuntimeError` once the hook has run (from a later `atexit` handler or a finalizer), and a run that is still waiting at exit is abandoned instead of being left to crash the process. A test starts the reproduction as eight child processes at a time and requires every exit status to be 0.
- **A request an MCP or gRPC server refused before running it no longer reaches the model as `reconciliation_required` (#423).** Any failed MCP call, an `isError` result or a JSON-RPC error, became `{"status":"reconciliation_required","retryable":false,"effect_uncertain":true}` unless the tool had an execution policy that rules out an effect, even when the server had refused the request before it ran anything and the model only had to correct its arguments. A refused request is now a failed tool result with the server's message, `retryable` false and `effect_uncertain` false, whatever the policy says: a JSON-RPC error `-32700`, `-32600`, `-32601` or `-32602`; an `isError` result marked with `_meta["io.neograph/failure"].code` `NG.InvalidRequest`; and, for `GrpcRemoteTool`, the statuses `NOT_FOUND` and `INVALID_ARGUMENT`. `MCPServer` now sends that marker with the result for arguments that fail a tool's input schema (it sent a bare `isError` result before). A failure that may have come after the tool started, such as any other `isError` result, `-32603` or a server error, is `reconciliation_required` as before unless the policy rules out an effect. **Behavior change:** the tool message of a refused request, and the `_meta` of `MCPServer`'s schema-validation result. New: `ToolRequestRejected` and the `ToolRequestRefusal` interface (`<neograph/tool_execution.h>`): a tool throws the first, or an exception type with a base of its own implements the second, when it knows its call did not run, and the node failure classifier reads a refusal as an ordinary failure that is retried only when its `retryable()` is true. `MCPTool` still throws `MCPError` for a refused JSON-RPC error, so callers that catch it are unaffected. See the [tool failure contract](docs/reference-en.md#tool-failure-contract).
- **`make_tool_service()` and `make_checkpoint_service()` can be used with their own header (#425).** Both return a `std::unique_ptr<::grpc::Service>`, and `<neograph/grpc/tool_service.h>` and `<neograph/grpc/grpc_checkpoint.h>` only forward-declared `grpc::Service`, so destroying the result did not compile unless the caller also included the generated `neograph.grpc.pb.h`. Both headers now include the gRPC service base (`<grpcpp/impl/service_type.h>`). A test builds each header in a translation unit that includes nothing else of gRPC.
- **The serial fan-out notice is printed only for branches that ran one after another (#417).** The `[neograph] warning: fan-out ... executing serially` notice appeared for every fan-out of width 2 or more on an engine without a worker pool, including coroutine nodes that wait on I/O: those interleave on the one thread, and three of them with `worker_count` 1 were in flight at the same time. The engine now records when the node body of each branch ran, and prints the notice after the fan-out only if no two branches overlapped, for a static fan-out and for `Send`. **Behavior change:** the notice comes after the fan-out instead of before it, and not at all for branches that overlap. Synchronous branches (`SyncGraphNode`, Python nodes) still print it once per engine, and `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` still turns it off.
- **Program: a task graph task that leaves a budget dimension unset is counted by what it receives (#412).** A `TaskGraphProposal` task whose `budget` left a dimension at 0 was counted as 0 by the proposal and fragment ceilings, but its child was launched with the link's whole ceiling for that dimension, and an unset `output_bytes` allowed any output, so a planner could make an expansion spend more than the aggregate ceiling it had been checked against. Proposal schema 2 closes the gap: a task must state a non-zero amount for every dimension in which its template has a non-zero ceiling (`P_PROPOSAL_BUDGET_ZERO`), and a schema-2 fragment launches each task with exactly its reservation. A dimension of 0 grants nothing, as in `RunBudget`, and the output is held to the stated bytes. A schema-1 proposal and fragment keep their canonical bytes, identities and launch behaviour, so stored data and existing hosts are unchanged; a host that takes proposals from a planner sets `TaskGraphProposalLimits::min_schema_version` to 2 to refuse schema 1. A rejected expansion proposal now reports the code and pointer of its first diagnostics in `failure.witness.diagnostics`.
- **Program: a fork replays the provider calls of the invocation it continues (#405).** A fork of a paused run ran the paused invocation again under the fork's run, and the SQLite provider journal, which keys a call by its run, sent and charged again the provider call the invocation had made before it asked, where a resume of the source replays it. Every attempt of a fork now gives the broker its source run (`ProgramCoreProviderCallContext::replay_source`), and the journal adopts the source's record of the same call: the same request at the same slot on the source's Core thread, journaled before the fork. The outcome is re-encoded under the fork's dispatch id and the row keeps the source's attempt, so the call is neither sent nor charged again; a source call whose outcome is unknown stays uncertain in the fork. The in-process broker keeps no journal and still sends again, as for a node retry.
- **Program: a typed plan that stopped before it published a checkpoint is recovered, not blocked (#404).** `reconnect()` raised `P_RUN_RECOVERY_BLOCKED` for a plan that is not replay-safe and had published no checkpoint. It now follows the rule of a JavaScript `callCore` (#402): each plan call continues the Core run it started, in plan order, from that run's latest checkpoint, and the first call that saved none returns the run `Interrupted` with a `pending_effect` (`core_node` `"plan"`) that names its operation. `reconcile()` with `Completed` records the result as the operation's outcome and the plan runs on from it; `Unknown` leaves the run `AmbiguousEffect`.
- **Program: a stopped JavaScript `callCore` continues from its own Core checkpoint (#402).** `reconnect()` returned a run whose process stopped inside a `callCore` for reconciliation even when the call's Core had saved a checkpoint, so a call that had finished, or had paused on a question the run had not published yet, had to be reconciled by hand. Recovery now continues each call of the pending command from the latest checkpoint of the call's own Core thread, answering nothing: a finished call replays its result, a paused one asks again, and the members of a join continue the same way. A call that saved no checkpoint may have started without a trace and is still returned for reconciliation, unless it provably never started (an `ng.await` with a timeout whose start the command journal does not record). So is a call whose latest checkpoint is an `interrupt_before` or `interrupt_after` pause: recovery never passes a gate unanswered. A restarted attempt is now charged, when it binds an operation, the tokens of the calls it journaled before the restart, as it is charged their money, so a call that replays from its checkpoint is charged once.
- **Program: a Completed reconcile of a JavaScript command continues `main()` (#403).** A command reserves the run's whole remaining wall time and Core steps, and a reconciled command kept them, so `main()` ended `TimedOut` ("Program wall-time budget (0 ms) expired") right after the reconcile; when the run carried an earlier command's checkpoint, the pause for reconciliation had also released the reservation the reconcile settles. Reconciling a command now returns its wall time and Core steps to the run, and the pause keeps the reservation until the reconcile. The tokens and money the command reserved stay spent, because what it used is unknown.
- **A2A task streams send an SSE comment while the run is quiet.** The `SendStreamingMessage`, `message/stream`, `SubscribeToTask` and `tasks/resubscribe` streams said nothing between the Task and the next status, so a run that stayed in one node longer than the client's read timeout (httplib and httpx wait 5 s by default) was cut off while it was still working, and a subscriber saw "no response" instead of the final status. A stream that has been silent for 5 seconds now writes the comment `: keep-alive`, which SSE parsers ignore. **Behavior change:** comment lines can appear between a stream's `data:` events; a client that reads raw lines instead of using an SSE parser must skip them. A subscriber joining a running task was not at fault: the Task, the cursor and the channel are taken under one lock, and so are the run's final status, the closing of the channel and the removal of the run.
- **ACP accepts Windows workspace paths.** `session/new` and `session/resume` refused every `cwd` and `additionalDirectories` entry that did not start with `/`, so an ACP client on Windows could not open a session. A drive path (`C:\work`, `C:/work`) and a UNC share (`\\host\share`) are absolute now, like a POSIX path; a relative or drive-relative path (`C:work`) is still `-32602`.
- **gRPC checkpoint failures are no longer "no checkpoint".** The `CheckpointService` answered a backend outage on a load with "not found" and on a list with an empty list, so a resumed run silently started over, and `GrpcCheckpointStore::delete_thread` reported a failed delete as success. A backend failure is now `UNAVAILABLE`, which the client throws, a failed delete throws, and a malformed `SaveCheckpoint` is `INVALID_ARGUMENT`. `GrpcCheckpointStore` sets a deadline on every RPC (`set_rpc_timeout`, 30 s by default), and `GrpcRemoteTool` on every call (`set_call_timeout`, 60 s by default).
- **NeoGraph's MCP client tells the server when it stops waiting.** A request the client abandons, cancelled or timed out, is followed by `notifications/cancelled` (never for `initialize`), so the server can stop the work; before, the server ran the abandoned tool to completion.
- **An MCP call in flight stops when its run is cancelled.** `MCPTool` ignored the dispatcher's cancel token and deadline, so cancelling a run, or an attempt timeout, waited for the server's reply (an attempt then outlived its stop deadline and failed as `NG.EffectUncertain`). It now honours both on stdio and HTTP: cancellation is `CancelledException` (`cancellation_requested`), the deadline or request timeout a `timed_out` call, and the late response is dropped. `grpc::GrpcRemoteTool` follows the same rule: the run's cancel token cancels its RPC and the run's deadline is the RPC deadline.
- **Ill-formed UTF-8 no longer empties text.** `json::dump` returned an empty string for a document holding an ill-formed string, so a failed tool message, an MCP response line or a gRPC payload arrived empty, and Python raised `UnicodeDecodeError`. Each maximal ill-formed subsequence now becomes U+FFFD at every boundary, and well-formed documents keep their bytes.
- **A system error from tool code is a tool failure.** A `std::system_error` raised by a running tool (a missing file, a refused connection) was reported as `rejected`, the admission status; it is now `failed` or `reconciliation_required`. A host admission refusal is now `rejected` or `expired` like the resource arbiter's, instead of `failed`.
- **A Python node that raises `HookBoundaryBlocked` is not retried.** A Python node's Hook block was an ordinary, retried and routable failure, while a C++ node's and a Python tool's stopped the run; it now stops the run unretried.
- **A long or non-UTF-8 Hook failure is recorded.** A Hook target failure message over 4096 bytes, or one cut inside a UTF-8 sequence, made its receipt invalid and failed the Hook boundary with an unrelated error; receipt text is now truncated on a code point boundary and repaired.
- **Python `mcp.MCPError` keeps `code` and `data`.** Only the message reached Python; the JSON-RPC error's `code` and `data` are now attributes.
- **A2A: exception text no longer reaches peers.** A failed run's status message carried `what()` (for example a path or a credential in an exception message). Peers now receive only the failure's public message and `metadata["neograph/failure"] = {code, retryable}`; this also covers failed Program runs. A hook block before any node finished is `REJECTED`, after it `FAILED`. `retryable` comes from `graph::classify_failure`, as on MCP, gRPC and ACP, and is now also `true` for a provider failure (`NG.ProviderFailed`), which a new request may get past although the node retry loop leaves it to the provider runtime.
- **A2A: an interrupt says what it needs and a follow-up resumes it.** `INPUT_REQUIRED` carries the interrupt reason as text and its payload as a data part, and a message to the task resumes the run from its checkpoint instead of starting the graph over.
- **A2A: requests and cards follow the specification.** Invalid params get `-32602` with field violations; malformed envelopes and batches `-32600`; every error carries a `google.rpc.ErrorInfo` detail. Skills always have tags, the 1.0 card has no 0.3 fields, and the card answers with `ETag`, `Cache-Control` and `If-None-Match`. The 0.3 stream opens with the task and ends on `final: true`.
- **Python example 28 and `test_protocol_sdk_e2e.py` work on agent-client-protocol 0.12 and 1.0.** The 1.0 line removed the `text_block`/`image_block`/`update_*` helpers they imported.
- **A mediated tool call that never started frees its slot.** `SQLiteToolEffectBroker` recorded a call that ended without starting (`expired`, `rejected`, `cancelled_before_start`) as `reconciliation_required` and left its slot pending, so the run paused for reconciliation and every later call of the slot did too. It now returns the call with its own status, terminal and not effect-uncertain, and releases the slot, as the node attempt's in-memory record already treated it.
- **A tool output over `output_limit_bytes` is effect-uncertain when the call may have had an effect.** It was always `failed` with `effect_uncertain` false; it now follows the rule for every failure after a call started: `reconciliation_required` and effect-uncertain unless the policy is `ReadOnly`, `ExternalRead` or `Idempotent`.
- **A Hook target that timed out is not started again by the tool execution controller.** The Hook runner already settled a timed-out native target as final, but the controller that runs the target retried it within that one attempt when the target's policy allowed retries (`retry_max_attempts` above 1, for an idempotent or read-only target), so a target that may still have been running was started beside itself. A Hook target attempt that timed out is now final in the controller too; a tool call keeps its policy's retries.
- **MCP tool arguments and results are validated as JSON Schema 2020-12.** The server and the client each checked a keyword subset: the server ran tools whose arguments broke `minimum`, `anyOf` or a `$ref`, and both rejected `1.0` as an integer. One validator now covers the core, applicator, unevaluated and validation vocabularies and local references for the server, the client and the Harness; the client skips a schema in a dialect it cannot check instead of failing the call.
- **MCP error responses never carry `"id": null`.** An invalid envelope whose id could be read was answered with a null id, and so were parse errors, batches and every HTTP error body, which the 2025-11-25 schema does not allow; the readable id is echoed, and an error with none carries no id.
- **NeoGraph's MCP client reads the official servers' event streams.** It split Server-Sent Events only on `"\n\n"`, so the official Python servers' CRLF streams lost every response that followed a notification (a tool that logs or reports progress); it now follows the WHATWG rules and stops reading once the response arrived. A stream the server closed before the response is resumed with `GET` and `Last-Event-ID` after its `retry` interval.
- **NeoGraph's MCP client answers server requests.** A server's `ping` gets an empty result and any other request `-32601`, on stdio and on an HTTP call's event stream; they went unanswered, so servers that ping their clients failed. A server request that reused the id of a pending call no longer completes that call.
- **NeoGraph's MCP client recovers an expired session and ends its own.** HTTP 404 for a request that carried `Mcp-Session-Id` starts a new session and sends the request again once; the client kept sending the dead session id. `shutdown()` and the release of the last reference send `DELETE`, and the URL constructor `MCPTool(url, ...)` keeps one session for all its calls instead of opening one per call that was never ended, which exhausted a server's session limit.
- **A JSON-RPC error in an MCP HTTP error body keeps its code.** A server that refused a request with an HTTP error status and a JSON-RPC error body (such as `-32022` from a server that speaks only MCP `2026-07-28`) raised a transport error with the body as text; it is now `MCPError` with the code, data and `http_status()`.
- **A cancelled graph run tells the MCP server.** When a run's cancel token cancelled the coroutine awaiting an MCP call, the transports reported the aborted wait as a shutdown or connection failure and sent no `notifications/cancelled`; it is now a cancellation, and the server is told.
- **MCP HTTP: cancellations are not starved, a response timeout cancels the tool, `DELETE` closes its connection.** cpp-httplib's pool could leave a `notifications/cancelled` waiting behind the calls it should stop; HTTP threads now start on demand up to `max_http_threads`. A request that outlives `response_timeout` cancels its tool call as well as answering 504. A `DELETE` with `Content-Type` and no body, as the official Python client sends, dropped the next request on its connection.
- **Structured MCP tool results also carry their JSON as text.** The Harness tools and example 59 returned `structuredContent` without the serialized JSON in a text block, which MCP asks for clients that do not read structured content.
- **Program: crash recovery of a resumed attempt continues with its resume value (#377).** Recovery re-ran an attempt that `resume()`, `reconcile()` or `fork()` had started with the run's input instead of the value the attempt resumed its checkpoint with. A `define()` Program's node saw the run input as its resume value, the interrupted `callCore` of a `main()` Program resumed with the command's input, and with a bound provider the SQLite journal failed closed with "logical call binding conflict". The run record now carries that value, `ProgramRunRecord::attempt_resume()`, durable before the attempt starts, and recovery resumes with it. A record that carries it uses run record storage schema 5; every other record keeps its schema, bytes and id, and stored records load unchanged. `ProgramRunRecordData` gained a member: rebuild C++ consumers (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). The first attempt of a forked `main()` Program is not covered yet.
- **Program: a replayed provider call's tokens are charged once per run (#378).** On interrupt and resume with the SQLite provider journal, the resumed attempt charged again the model tokens of calls an earlier attempt had journaled and settled: remaining `model_tokens` went from 999,976 to 999,952 with no new request. The journal still recognises those calls, so they are neither sent nor counted again, but charges an attempt only for the calls it journaled itself, as the monetary accounting of #372 already did. A call journaled before a crash is still charged once after recovery, and the new call of a node retry is charged.
- **Program: a join reports the failure that stopped it (#379).** A fail-fast `ng.all` reported its first failed member in declaration order, so a sibling declared earlier and cancelled because another member failed replaced that failure: a member refused with `P_MONEY_BUDGET` was reported as an operation cancelled. The join now reports the member whose failure stopped it; with `collect` it still reports the first failure in declaration order. A plan-level `parallel` operation had the same flaw, where a cancelled branch replaced the failure that cancelled it. `ng.race` and `ng.quorum` were already correct; tests now pin both.
- **Program: a join member's interrupt pauses the join, and resume replays the members that finished (#380).** An `ng.all` whose member's Core interrupted did not resume correctly. The run kept the checkpoint of whichever member wrote one last, so a SQLite host resumed a member that had completed and the join was `Interrupted` again; an in-memory host ran the completed member again, sending and charging its model call twice; and a member still running when its sibling interrupted was cancelled, keeping the reservation of the call it abandoned. Now a member's interrupt pauses its join: the join launches no further member, lets the members already running finish and keeps every result they reach. If those results end the join by its own rule (a fail-fast failure, a race winner, a quorum met or out of reach), it ends that way, so `ng.race` and `ng.quorum` finish without the paused member when they can. Otherwise the join is `Interrupted` on its first paused member in declaration order, its output lists the results so far, and the command journal records the members that finished. `resume()` continues the paused member with the resume value and replays the recorded members without running or charging them again; every other member runs again. In-memory and SQLite hosts behave the same, and so do nested joins. A command journal entry that records finished members uses entry storage schema 2; every other entry keeps its schema, bytes and id. `ProgramJavaScriptCommandJournalEntryData` gained a member: rebuild C++ consumers (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Program: a run that an expired execution lease ends is charged its journaled calls' tokens (#381).** When `reconnect()` ended a run `TimedOut` because the root execution lease of the stopped process had expired, the provider calls that attempt had journaled were charged their money (#372) but not their model tokens: `usage.model_tokens` was 0 and `remaining_budget.model_tokens` still held them. Both are now charged, once. `ProgramCoreProviderCallContext` gained `journaled_usage`, the token account that a journaling broker charges when it is bound for such a run: rebuild C++ consumers.
- **Core: a resume skips only the gate of the interrupt it answers (#382).** The first super-step after any resume skipped its `interrupt_before` check. Recovery after a process stopped between a step's commit and the next step's interrupt save resumed the step's checkpoint, and the gated node ran without approval; a resume after `interrupt_after`, or after `update_state(..., as_node)`, ran the next gated node without asking as well, which a Program with `interruptAfter` followed by `interruptBefore` reached. Now only resuming an `interrupt_before` pause runs the node it held, and resuming a `NodeInterrupt` continues its step; every other resume still pauses before an `interrupt_before` node it is about to run. `ResumeContext` gained `current_node`: rebuild C++ consumers.
- **Core: every checkpoint save path writes the same run metadata (#383).** The checkpoints the node executor saves for a `NodeInterrupt` built their metadata without the `barrier_stalled` and `failure_exit` that the engine's own save paths add. Every path now builds it with one helper, which takes whether the checkpoint is saved during a step or between steps for the `max_visits` counters. With the current scheduler a `NodeInterrupt` cannot occur once a run has stalled or reached a failure exit, so no checkpoint differed yet; tests compare the checkpoints of every path for the same run state.
- **Program: an interrupted branch pauses a plan-level `parallel` (#384).** A plan-level `parallel` (the typed operation plan, not `ng.all`) whose branches both interrupted resumed only the first: the second re-ran with the resume value as its input, or asked again on every resume, and a branch that had finished ran and was charged again. The `parallel` now follows the join/interrupt contract of #380: a failing branch still ends it; otherwise it is `Interrupted` on the first paused branch in declaration order, and the run resumes that branch's checkpoint. Resume continues the Core call the pending question names, replays the branches that finished without running or charging them, and replays every other paused branch as paused, so it asks its own question again. The records are kept in the run record, `ProgramRunRecord::settled_branches()`, on the `Interrupted` record and on the `Running` record of the attempt that resumes it; a record that carries them uses run record storage schema 6, and every other record keeps its schema and id. `ProgramRunRecordData` gained `settled_branches`: rebuild C++ consumers.
- **Soundness check: a visit count resets only when the failure target completes (#364).** The opt-in soundness check reset a node's `max_visits` count when its failure-edge target ran, including when the target itself failed and took its own failure edge; the runtime resets it only when the target completes. The check now does the same and explores the states in which the node stays at its limit.
- **Core: a `Send` target that interrupts pauses the run (#385).** A `NodeInterrupt` raised by the target of a `Send` escaped the run as an exception and left no checkpoint, so there was nothing to resume; a Program whose map step dispatched a Send to a node that asked failed with `P_RUNTIME_CORE_FAILURE`. The run now pauses as it does for a node of the ready batch, on every sync, async and streaming entry point and in Python. The `NodeInterrupt` checkpoint re-enters the whole super-step from the state it started with, and the paused result reports that state. Resume replays the node that emitted the Sends and every Send that finished, so they run and are charged once, and runs the Send that asked with the answer under the same task id, so a provider call it journaled replays. In a step that no checkpoint precedes, such as a map node at the entry, the finished work is recorded under the interrupt checkpoint, and a node of the ready batch that finished in such a step is now replayed too instead of running again.
- **Program: a plan resume continues where the plan stopped (#386).** Resuming a paused plan-level Program (the typed operation plan, not a `main()`) ran the plan again from its root with the resume value as its state: the operations before the paused call ran and were charged again (tokens, money, Core steps and Program operations), and a `branch` evaluated its condition on the resume value. The paused run record now keeps, by operation id, the position of every operation the run was executing (the next child of a `sequence`, the iteration of a `loop`, the attempt of a `retry`, the item of a `map` or the winner of a `race`, with that position's state, and the plan's input on the root), next to the result of each `parallel` branch that finished and the question of each other paused call. Resume, reconcile, fork and graph migration carry these records, and so does the recovery of each: what finished is neither run nor charged again, a `branch` sees the state it saw the first time, and only the answered call receives the resume value. A fork gives each recorded paused call its own copy of its checkpoint, and an effect that `reconcile` leaves ambiguous keeps the records. Branch 0 of a `parallel` or `race` now continues on that operation's own Core thread, so the first branch of a nested `parallel` no longer runs on the root's thread next to the root's first branch; programs without nested parallels keep their thread ids, and a call that a resume answers continues on the thread of its checkpoint, so runs paused before this change still resume. The records extend `ProgramRunRecord::settled_branches()` (#384), whose paused-call records are now keyed by the call's operation id; a plan that pauses inside a `sequence`, `loop`, `retry`, `map`, `race` or `branch` now stores run record schema 6, and `P_PLAN_PARALLEL_RECORD` is now `P_PLAN_REPLAY_RECORD`.
- **Core: a run paused by a `NodeInterrupt` reports the checkpoint it saved (#386).** It reported the thread's latest checkpoint in the store instead. SQLite and PostgreSQL order a thread's checkpoints by millisecond timestamp, then step, and every run on a thread starts again at step 0, so when an earlier run on the same thread had saved its last checkpoint in the same millisecond, that checkpoint was the latest. Resuming the reported checkpoint returned the earlier run's final state without running the paused node: on macOS, where SQLite writes return at once, a resumed Program plan call ran no Core step and returned the previous call's output. `RunResult::checkpoint_id` of a run that a node, a parallel step or a `Send` pauses with a `NodeInterrupt` is now the checkpoint the run saved for that pause.
- **Core: a thread's latest checkpoint is the one saved last, on SQLite and PostgreSQL (#386, #396).** `SqliteCheckpointStore` and `PostgresCheckpointStore` picked a thread's latest checkpoint, and ordered `list()` and `get_state_history()`, by millisecond timestamp, then step. Saves that tie on both, or whose step goes back, came out in the wrong order: a later run on a thread starts again at step 0 (a Program plan's next Core call), two `interrupt_before` gates of one step each save a checkpoint of that step (#396), and on macOS, where SQLite writes return at once, saves share the millisecond. `resume()` then continued from an earlier checkpoint than the one saved last: it answered a gate or an interrupt that was already answered, so the run paused again, or a node received the answer to another question. Both stores now order a thread's checkpoints by save order, as `InMemoryCheckpointStore` does, and `load_latest()` is the first entry of `list()`. Saving a checkpoint ID that the thread already holds replaces that checkpoint and makes it the latest in all three stores; the in-memory store no longer lists such an ID twice. Each store adds a nullable `save_seq` column to `neograph_checkpoints` when it opens the database. PostgreSQL numbers a save from the sequence `neograph_checkpoints_save_seq`, which is also the column default, so a writer that does not set the column is numbered too; SQLite numbers it one above the thread's highest number, and a trigger numbers a row that a writer leaves unnumbered. The migration rewrites no row and repeats without effect: rows saved before it keep `save_seq` NULL and their (timestamp, step) order, behind every row saved since. The index `neograph_checkpoints_save_order` replaces `neograph_checkpoints_recent`. The previous release can still open a migrated database, and the rows it saves are numbered.
- **SQLite: stores that open one new database at the same moment all open.** A store could fail at once with `database is locked`, whatever its busy timeout, when another store or process opened the same new database file at the same moment: when two connections switch one file to WAL at once, both hold its read lock, and SQLite returns `SQLITE_BUSY` at once, without calling the busy handler, to the one that asks second for the write lock, since waiting could deadlock. Two test processes on macOS hit it. `SqliteCheckpointStore`, `SQLiteContextStore`, `SQLiteHookJournal`, `SQLiteProviderDispatchReceiptStore`, `SQLiteToolEffectBroker`, `SQLiteProgramProviderCallJournal` and `SqliteHarnessRecordStore` now retry the switch until their busy timeout runs out, and the stores that set the timeout after the switch now set it first. `SqliteCheckpointStore` runs its schema and migrations again, in one `BEGIN IMMEDIATE` transaction, when that transaction ends in `SQLITE_BUSY` before the timeout. The `GateIdentity` and `ResumeValueScope` tests give each case a randomly named directory that they create exclusively, so two cases that start together no longer share, and delete, one database.
- **Build: MSVC compiles every target with `/bigobj`.** `src/program/runtime.cpp` exceeded the COFF object section limit in the Windows AddressSanitizer build (`C1128`). The top-level `CMakeLists.txt` now passes `/bigobj` with `/utf-8` to every MSVC target, so the library, the tests, the Python wheels and installed-package builds all take it; the flag that only `neograph_program_tests` had is removed.
- **Core and Program: a fork of a paused run replays what had finished (#390).** `GraphEngine::fork` of a `NodeInterrupt` checkpoint, the Program fork and its copies of paused-call checkpoints dropped the checkpoint's pending writes, so the nodes and `Send` targets that had finished in the interrupted super-step ran again in the forked run, with their effects and provider calls. A fork now copies the writes a resume of the source would replay, so the forked run replays them exactly as a resume does: nothing that finished is run, sent or charged again. Python hosts can fork a Program run with `LocalProgramHost.fork(run_id, value, version, budget, ...)`.
- **Program: `ng.await` keeps its deadline across resume and recovery (#391).** An inline `ng.await(..., timeout_ms)` in `main()` started its timeout again from zero after every resume, so each interrupt and resume extended the deadline the author wrote. The await's start is now recorded in the command journal when the await begins (a pending entry with `await_started_at`, command journal entry schema 3), and a resumed or recovered attempt waits only for the time that remains; a plan-level `await` records its start in its replay record (#386). Journal entries without the field keep their schema and bytes.
- **Program: an interrupted branch pauses a plan-level `quorum` (#387).** A plan-level `quorum` (the typed operation plan, not `ng.quorum`) counted a branch whose Core call interrupted as one that did not succeed and went on to the next branch: the question was dropped, and the quorum completed on later branches or failed. It now follows the join/interrupt contract of #380 and #384: a paused branch counts as a success it may still reach, so it pauses the quorum unless the branches that finished already decide it, and no further branch starts. The run record's `settled_branches` keep the result of each branch that finished by its own end; resume replays them without running or charging them again, continues the paused branch with the resume value and goes on from there, and a later branch that asks pauses the quorum again. A quorum whose remaining branches can no longer reach `min_success` now fails with `P_QUORUM_UNREACHABLE` instead of completing with fewer successes, and the quorum reports the execution traces of its branches.
- **Core: each interrupt of a super-step is answered on its own (#388).** When several ready nodes or `Send` targets of one super-step raised `NodeInterrupt`, one `resume(value)` handed the same value to all of them, so the second question was answered with the first one's answer and never asked. Now the paused `RunResult` lists every interrupt of the step in `interrupts` (`{id, node, value}`, the id being the paused invocation's task id) and reports the first through `interrupt_node`, `interrupt_value` and `interrupt_id`. A resume answers the interrupt that `RunMetadata::resume_interrupt_id` (Python `interrupt_id=` on `resume`, `resume_async` and `resume_from`) names, or the first one; every other invocation of the step runs without the value and asks its own question again. An id that is not pending is rejected with `std::invalid_argument` (`ValueError`) before anything runs. A checkpoint that paused on several interrupts lists them in `_neograph.interrupts`; one that paused on one keeps its bytes. A Program pending input or effect for such a step names its Core interrupt in `core_interrupt_id` (pending value storage schema 2; other values keep schema 1, their bytes and ids), and resuming the run answers exactly that interrupt; JavaScript joins and plan-level `parallel` and `quorum` already answered one paused member per resume.
- **Core: one `Send` and many `Send`s follow one contract (#389).** A step with a single `Send` applied the payload to the shared run state and ran the target on it, while a step with several ran each target on an isolated copy; a replayed single Send did not apply it at all. Every Send now runs on its own copy of the state with its payload applied, with retry, live and replayed, and only the writes it returns reach the shared state. The `executor.h` comment that said the multi-Send path runs without retry is corrected: every Send attempt retries in its own effect namespace (#364).
- **Tests: the plan pause tests build without QuickJS.** `JoinTestStores` sat inside the QuickJS-only block of `tests/test_program_runtime.cpp` while the plan-level `parallel` and resume tests (#384, #386) outside it use it, so the file did not compile with `NEOGRAPH_BUILD_QUICKJS_CONTROL=OFF`. The shared stores now live outside that block, and three tests that admit JavaScript sources are guarded.
- **Core: a resume value reaches only the invocation it answers (#393).** After a resume, `ctx.resume_value` stayed set for every node of every later super-step of the resumed run, so a later node that asks for approval, or a `ToolGate` that asks before a tool runs, took the earlier answer as its own and did not ask: one approval answered a different question. The value now reaches exactly one invocation, in the step the resume re-enters, each of its attempts included: the node or `Send` whose `NodeInterrupt` the resume answers (#388), or the node an `interrupt_before` gate held. Every other invocation, and every node of a later step, sees none, so a node or gate that asks pauses the run again. A subgraph resumed through its parent receives its answer through the parent's answered node, and Program `call_core` and JavaScript `callCore` resumes follow the same rule.
- **Core: a fan-out step and the Sends of a step end by one rule (#392).** Whether a step whose siblings both failed and interrupted paused or failed depended on which had the lower index. One rule now holds for fan-out branches and Sends, with one worker or many, whatever the index or completion order: cancellation stops the run; otherwise an uncertain effect (a `tool_effect_reconciliation` interrupt) pauses the step for reconciliation even beside a failure, reporting only those interrupts, and the failed sibling runs again on resume; otherwise the first failure in dispatch order fails the run and the step's ordinary interrupts are not reported; otherwise the step pauses on every interrupt (#388). A failure routed along a failure edge (#364) is the node's result and does not end the step. JavaScript joins and plan `parallel` and `quorum` already ended on a member failure (#380, #384, #387).
- **Tests: a fork answers the Core interrupt its pending call names (#394).** A fork of a Program run paused on a Core step with several interrupts was reported not to carry `core_interrupt_id` to its first attempt. It does: the fork publishes the source's pending call, which keeps the id, on its first run record, and the attempt reads it from there as a resume or reconcile does. Tests now pin the record and the answer, in memory and on SQLite, and in Python with a bound `llm_call` that is neither resent nor recharged.
- **Core: two `interrupt_before` gates in one step are no longer passed by one resume (#396).** When two gated nodes were ready in the same super-step, one resume ran both, so the second human approval was never asked. A resume now answers the gate the run paused at, identified by the gated node's task id (#388), and every other gated node of the step pauses the run again with its own Before interrupt before anything runs. Once every gate is answered the step runs, and each gated node receives the answer to its own gate (#393).
- **Program: joins rank a reconciliation pause above a member's failure, as the Core does (#397).** JavaScript `ng.all`, `ng.race` and `ng.quorum` and plan-level `parallel` and `quorum` ended with a member's failure even when another member had paused to reconcile an effect whose outcome is unknown, abandoning that effect. They now follow the #392 precedence: cancellation, then the reconciliation pause, then the failure, then ordinary interrupts. The failed member is recorded and replays on resume without running again; a plan-level `parallel` records a branch whose Core node failed and runs again a branch that failure cancelled.
- **Program: a fork of a JavaScript `main()` resumes its paused `callCore` (#395).** The fork's first attempt got neither the command journal nor the Core resume value, so `main()` started over: finished commands ran again and their provider calls and effects were sent and charged again, every asker asked again, and the answer was lost. The fork now carries the source's command journal rebound to the target bundle, saves its copy of the paused call's checkpoint on that call's Core thread, runs `main()` on the source's input and resumes the paused call with the fork's value, as a resume does; its first attempt reserves the resumed command's resources, and recovery continues it the same way.
- **Program: a fork replays a recorded provider failure under the right custody (#398).** A fork of a run paused beside a failure that carries provider evidence, in a collecting `ng.all` recorded in the command journal (#395) or in a plan's `settled_branches` (#386), restored that evidence under the fork's custody, which the record does not match, so the fork failed with a custody mismatch. The fork, and a graph migration, now re-bind each such record from the source run's custody to the new run's when they carry it, so the replay restores the evidence (`result.failure.provider_outcome`) and sends nothing again. A plan `parallel` stores a settled branch's provider evidence as a quorum does.
- **Program: a plan quorum no longer fails the run when a branch's Core node fails (#399).** A branch whose node raised was thrown out of the quorum as the run's failure. It is now a failed branch, with the code, node, attempts and provider evidence the run would have reported, recorded when a later branch pauses the quorum and replayed on resume; cancellation and a lost checkpoint still end the run, and the #392/#397 precedence is kept.
- **Program: a resumed join no longer cancels the member it answers (#400).** A fail-fast `ng.all`, an `ng.race` or an `ng.quorum` that paused for reconciliation beside a failed member (#397) replays that member at once on resume; its completion ended the join and cancelled the reconciled member while its node was still consuming the answer, so the effect never settled. The answered member now runs to its end first, and the replayed failure then ends the join.
- **Program: a resumed join settles the members it replays first (#400).** On resume a JavaScript join launched its members in declaration order, so a member replaying a result recorded before the pause could be registered after the member the resume answers had already finished. The outcome then depended on timing: an `ng.race` paused for reconciliation beside a failed member could complete with the answered member's value instead of ending with the replayed failure. A resumed join now launches the members it replays first, then the member the resume answers, then the others. A replayed member settles before any member that runs, so a race's winner and a fail-fast join's failure are the same in every run.
- **Program: an ordinary question no longer outranks an uncertain effect in a paused join (#401).** A join (collecting or not) or a plan-level `parallel` with no failure paused on its first paused member in declaration order, so an ordinary question was asked before a member paused on an uncertain effect. It now asks for reconciliation first, as a Core step does (#392). The collecting `ng.all` exception, which waits for every member and so pauses on a paused member beside a failure, is kept, documented and pinned by tests.
- **Program: a plan `parallel` or `race` whose branches finish at once no longer hangs.** A plan-level `parallel` or `race` waited for its branches on the scheduler thread that ran it, while the branches signalled their completion on a strand through an asio channel, which is not thread-safe. When the last branch signalled while the wait was starting on another scheduler thread, the signal could be lost and the run never finished; it hung on Windows with branches that return at once. The wait now runs on the strand the branches signal on, as the other joins already did, and a JavaScript join that settles while it registers its members sends its signal from that strand too.
- **Core: a resume that the process stopped during can be repeated.** A host that recovers repeats the resume it was executing, with the same value and interrupt id. When the step had paused on one interrupt and the answered node or `Send` finished before the process stopped, the repeat failed: naming the interrupt raised `std::invalid_argument` ("is not pending"), and answering by default gave the value to the step's first `Send` that had not run, which never asked for it. The interrupt of such a checkpoint is now the node it paused on, finished or not, and a repeat that names a `Send` that finished is accepted; what finished replays. Found by the resume model harness.
- **Program: recovery of a resumed join whose answered member had finished.** In a JavaScript join that runs its members one at a time, the process could stop after the answered member finished and the join recorded it, before the pause on the next member was published. `reconnect()` then failed the run with `P_TRANSITION_CONFLICT` ("Recorded join members do not match the paused join"). Recovery now replays the recorded member and continues the join, so the next member asks its question. Found by the resume model harness.
- **Program: recovery of a JavaScript `main()` that stopped inside a later Core call.** The run keeps the checkpoint of the last Core call it published. When the process stopped inside a later `callCore`, after it was recorded as pending, `reconnect()` took the earlier call's checkpoint for the pending call's and ended the run `CheckpointIncompatible` ("Exact Core resume checkpoint does not match a pending JavaScript callCore coordinate"). It now returns the run for reconciliation of the pending call, as it does when the process stops inside the first call, and does not run the call again. Found by the resume model harness.
- **A node retry no longer collides with an earlier attempt's effects (#364).** One invocation scope covered the whole retry loop, so every attempt reused the first attempt's effect task id and call ordinals: under a `ToolEffectBroker` a retry replayed the first attempt's receipt, or was rejected because its arguments had changed, and a crash during a retry resumed in the first attempt's slots.
- **An unconfirmed Program provider dispatch is an uncertain outcome (#364).** When the transport raised after the SQLite provider journal had committed its dispatch marker, the raw exception was rethrown, and a node retry could send the request again. The journal now throws `ProgramProviderDispatchUncertainError`, a `ProviderOutcomeError` without an outcome, for that case and for a later call to the unresolved slot; it is never retried or routed.
- **Program: `LocalProgramHost.resume(run_id, value)` works without a pending id (#359).** It resumes the single awaiting call; with none or several awaiting it raises `ValueError` naming `result.interrupt.pending_input.call_id`. An explicit id keeps the exact-match contract.
- **Program: failed runs keep the trace of the nodes that completed (#359).** `execution_trace` lists the nodes that finished before the failure, and `failure.core_node` names the node that threw. Identities of failed results change accordingly.
- **Program: compile-time JavaScript honours the run budget's wall time (#360).** A non-terminating `define()` stops at the smaller of the compiler limit and the budget's `wall_time_ms` with `P_JS_TIMEOUT`, instead of after 60 s. Python sets the compile limits with `ng.ProgramJavaScriptCompileLimits` through `javascript_limits=` on `ProgramCompiler` and `LocalProgramHost`.
- **Program: authoring diagnostics point at their source (#361).** JavaScript builder diagnostics carry their call site (pointer, line and column) in the diagnostics returned from compilation: `ProgramBundle::compile_diagnostics()`, Python `bundle.compile_diagnostics`. The diagnostics sealed into a bundle are unchanged, so bundle ids stay stable. Compile failures raise `ng.ProgramCompileError` (a `RuntimeError`) whose message ends with `source:line:col` and whose `.diagnostics` carries the details. Registry-miss hints name the Python registration calls.
- **Python: `ProgramRegistryBuilder` rejects unregistered executables with one error type (#370).** `add_registered_node` now checks that the node type is registered, as `add_registered_reducer` and `add_registered_condition` already did, instead of leaving the miss to admission. All three raise `ng.ProgramRegistryError`, whose `kind` is `"node"`, `"reducer"` or `"condition"`, whose `name` is the missing name and whose message names the Python registration call. It subclasses both `ValueError` and `RuntimeError`; the reducer and condition calls raised `RuntimeError` before. A Program that uses a name missing from the registry snapshot still fails compilation with `ng.ProgramCompileError` and `P_CORE_GC_REGISTRY_MISS`.
- **A run that stops at a barrier that can no longer fire is no longer `Completed` (#357).** When nothing is ready, the final super-step did not reach `__end__` explicitly and a join still holds signals, for example because a barrier member took a conditional route around the join, `RunResult::status()` returns the new `RunStatus::BarrierStalled` (Python `ng.RunStatus.BarrierStalled` and `RunResult.barrier_stalled`), and `output["_neograph"]["barrier_stalled"]` lists each stalled join as `{node, wait_for, received, missing}`. A Program run ends `Failed` with `P_CORE_BARRIER_STALLED`, `failure.core_node` set to the join and a witness that lists the stalled joins. Only an explicit route to `__end__` in the final super-step suppresses the stall: a static edge to `__end__` (which `.exit()` writes), a conditional route to `__end__` or `Command.goto("__end__")`. An implicit end, such as a node without successors or a Send worker, never does, and neither does an explicit end in an earlier super-step. Every checkpoint the run saves records the stall, so resuming one reports it again. An `interruptAfter` checkpoint stores the barrier-gated next nodes, so resuming it no longer runs the join before its last member arrives. A JavaScript `callCore` whose Core exhausts its step budget now ends the run `BudgetExhausted` with `P_CORE_STEP_BUDGET`; it used to return to `main()` as completed. **Behavior change:** runs that ended `Completed` in these situations now end `BarrierStalled`, or `Failed` in a Program.
- **Reserved node names and graphs without an entry no longer compile (#358).** A node named `__start__` or `__end__` was shadowed by the routing sentinel of the same name: an edge to `__end__` ended the run, and the JavaScript builder sealed an `__end__` → `__end__` self-loop. A graph with nodes but no edge from `__start__` ran nothing and reported `Completed`. The graph parser now reports `GC_RESERVED_NODE` and `GC_NO_ENTRY` (`P_CORE_GC_RESERVED_NODE` and `P_CORE_GC_NO_ENTRY` in a Program) in every schema version; a graph with no nodes is still accepted. The JavaScript builder rejects a reserved name at the `.node()` call with `P_JS_GRAPH_ARGUMENT`, and `define()` rejects a graph without `.entry()` with `P_JS_GRAPH_ENTRY`. The evolution `remove_edge` mutation keeps a graph's only entry edge. **Behavior change:** these graphs compiled before; `GraphEngine::compile` and Program compilation now reject them.
- **Program: C++ node bodies and Core subgraphs no longer hold the Program scheduler (#367, #368).** The fix for Python node bodies left every other node body on the Program scheduler thread: with the default `scheduler_threads=1`, a C++ node that blocked its thread for 800 ms made `ng.all` of two such members take 1.6 s and made `ng.race` return it over a faster member, and a Core subgraph whose node blocked did the same one level down. Every node body of a Program run now runs on a process-wide worker pool (`NEOGRAPH_NODE_ATTEMPT_THREADS` sets 1 to 1024, default 32), each attempt as one dispatched unit with its own cancel token, so the scheduler only orchestrates. Cancellation stays cooperative: a cancelled run still waits for a body to stop. Plain `GraphEngine` runs keep running untimed bodies on the calling thread.
- **Program: Python node bodies no longer hold the Program scheduler (#367, #368).** A Python node body ran on the Program scheduler thread for its whole duration, so join members ran one after another: `ng.all` of two 800 ms members took 1.6 s, `ng.race` returned the slow member declared first, `ng.quorum` waited for a slow member and included it, and with the default `scheduler_threads=1` an `ng.await` deadline could not fire. In a Program run, Python node bodies now run on a worker pool owned by the binding (32 threads; `NEOGRAPH_PYTHON_NODE_THREADS` sets 1 to 1024), so members progress concurrently and timers fire; the GIL still serializes Python code. Plain `GraphEngine` runs keep running Python bodies on the calling thread. `ng.race` and fail-fast joins settle on the first member to finish rather than the first declared, and `ng.quorum` keeps exactly the successes that met it. An `ng.await` records its deadline: a completion that arrives after it is a timeout even if the timer has not fired yet, and the timeout wins over an error raised by the cancelled child. Cancellation stays cooperative, so the await returns once the child stops. **Behavior change:** a late completion now ends the run `TimedOut` with `P_AWAIT_TIMEOUT` instead of `Completed`, and `ng.race` can return a different member.
- **Program: a model-calling node without a provider no longer aborts the process (#366).** An `llm_call` or `intent_classifier` node built without a provider dereferenced it on its first run and aborted the process (SIGABRT), in a plain `GraphEngine` and in a Program whose node was registered through `ProgramRegistryBuilder.add_registered_node` alike. Both nodes now throw `std::invalid_argument` naming the node, which the executor does not retry, so the run fails with `NodeExecutionError`; `GraphEngine` still compiles such graphs for structure-only tooling. Program admission rejects them with `P_ADMIT_PROVIDER`, which Python raises as `ng.ProgramAdmissionError`. Python binds a provider with `LocalProgramHost(node_context=...)` since #372.
- **Python: Program admission and runtime diagnostic errors are typed (#369).** An admission failure raised a bare `RuntimeError("Program admission failed with 1 diagnostic(s)")`, and runtime diagnostic errors such as `P_START_BUDGET` lost their code and witness. They now raise `ng.ProgramAdmissionError` and `ng.ProgramDiagnosticError` from every binding entry point; both subclass `RuntimeError`, as `ng.ProgramCompileError` does. The message has one line per diagnostic with its code, JSON pointer and message, plus `source:line:col` when it has a source site, and `.diagnostics` keeps the code, severity, phase, message, source site and witness. Budget errors name the resource and its limits, for example `Program invocation budget wall_time_ms=9000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`; so do each structural start-budget rule, admission's policy-ceiling and structural-floor checks and the Catalog `worker_count` check.
- **Program: messages and traces for Python hosts (#370).** Resuming a run that the `LocalProgramHost` does not hold raises `ng.ProgramDiagnosticError` with `P_RUN_NOT_FOUND`, and the message says that runs are kept in memory by the host that started them. A wall-time timeout fails with `Program wall-time budget (N ms) expired`; the text of the operation it interrupted moves to `failure.witness["cause_message"]`. `TimedOut`, `Cancelled` and diagnostic failures keep the `execution_trace` of the nodes that completed, as node failures already did; identities of those results change accordingly. For JavaScript sources, the E9 fan-in hint that compilation returns names `graph.barrier("<node>", [...])`; the sealed copy keeps the Core wording, so bundle ids do not change. **Behavior change:** the timeout's `failure.message` is new text; check `status` or `failure.code` instead of the message.
- **Program: a second interrupted `callCore` no longer terminates the process (#371).** A JavaScript `main()` whose second `callCore` was interrupted, after a resume or during the first `run()`, ended in `std::terminate`. The release of the interrupted reservation was checked against the attempt's cumulative usage, which also counts work an earlier command had settled, so the store rejected the publication and the fallback built an invalid result. The in-memory, SQLite and PostgreSQL transition stores now check that release with `is_valid_program_journal_reservation_release`: each usage-bearing resource may come back anywhere from the reservation minus the attempt's usage up to the whole reservation. Each interrupted `callCore` now returns an `Interrupted` result that `resume()` continues. A result that cannot be resumed no longer carries an interrupt, and an exception that escapes terminalization ends the run `Failed` with `P_RUNTIME_INTERNAL` instead of terminating the host.
- **Strict context assembly admits short histories with several required artifacts (#373).** The mandatory token count summed the message projection JSON of each required artifact, while the input count measured the prepared wire body, which is smaller. With a short history and two or more required artifacts the mandatory count exceeded the input, and `StrictRuntimeProfile.invoke` (and `RuntimeTurnAssembler::assemble`) failed with `Mandatory token count exceeds estimated input tokens`. Both counts now measure bytes the prepared request carries, three per token: a required artifact counts its text, and its role and JSON framing count toward the input only. Required content above `max_input_tokens` still raises `ContextBudgetBlocked`, and a receipt whose mandatory count exceeds its input count is rejected with both numbers, for example `Mandatory input tokens (41) exceed the estimated input tokens (40) they are part of`. New assembly receipts record a lower `mandatory_input_tokens`, so their identities differ from receipts built before; stored receipts still parse.
- **Program: each result of a `main()` lists the Core nodes it ran (#374).** With two `callCore` commands on a Core that interrupts, each result's `execution_trace` came one attempt late: `["draft"]`, `["draft"]`, `["deploy", "deploy"]`. An interrupted command ended the attempt with only its own trace, which dropped a command that had completed earlier in the same attempt, and a resume added again the recorded trace of every command an earlier attempt had completed. Each `Interrupted` or `Completed` result now lists exactly the Core nodes that ran since the previous result, in order: `["draft"]`, `["deploy", "draft"]`, `["deploy"]`. A result that a later command ends with another status also keeps the nodes of the earlier commands of its attempt. A failed result also lists the nodes that completed before the Core failed, as a `define()`-only run does; before, a `callCore` whose Core failed left `execution_trace` empty. Identities of the affected results change accordingly.
- **Topology errors that apply at every schema version no longer suggest removing `schema_version` (#375).** `GraphEngine::compile` ended every error list with `Remove 'schema_version' to fall back to lenient parsing`, also for documents without `schema_version`. For `GC_NO_ENTRY` and `GC_RESERVED_NODE`, which lenient parsing does not relax, the summary now says what to change instead: add an edge from `"__start__"`, or rename the node. The lenient hint and the `strict` prefix appear only when an unknown key (`GC_UNKNOWN_FIELD`) or a node config schema error (`GC_NODE_CONFIG`) is listed. Diagnostic codes, pointers and messages are unchanged.
- **Per-node engine settings reject unknown node names (#376).** `set_node_retry_policy`, both `set_node_cache_enabled` overloads, and the `node_retry_policies`, `cached_nodes` and `node_cache_policies` maps of `EngineConfig` accepted a name the graph has no node for, and the setting never applied. They now throw `std::invalid_argument` (`ValueError` in Python) that names the setting, the graph, the unknown node and the graph's nodes, for example `set_node_retry_policy: graph 'demo' has no node 'fecth' (nodes: 'fetch', 'parse')`; `build()` and `link()` check the `EngineConfig` maps before constructing the engine. **Behavior change:** code that set a policy for a missing or misspelled node now fails where it used to be silently ignored.
- **Retry backoff no longer overflows (#364).** The next retry delay was `delay_ms * backoff_multiplier` converted to `int` before it was clamped to `max_delay_ms`. A product outside the `int` range was undefined behaviour and became `INT_MIN` on x86, so every later retry ran without waiting; the jittered delay had the same conversion. Both are now computed in `double`: NaN and values above `max_delay_ms` clamp to the maximum, negative values clamp to 0, and the conversion comes last.
- **Program: a zero budget ceiling names its fields (#370).** `PolicySnapshot` and Python `LocalProgramHost` reject a ceiling with a zero field as `Every policy budget ceiling except child budgets must be positive; zero: model_tokens, max_dynamic_compiles`, listing each zero field. `max_child_depth` and `max_total_children` may still be zero together.
- **Program: the Python and QuickJS docs match the runtime (#370).** The Python Program quickstart runs again: it called `GraphState.get` with a default argument, which the binding does not accept. The QuickJS `main()` examples export `define()` and call only that Core, and say that a failed `callCore` ends the run rather than reaching a `try`/`catch`. The Python binding guide now covers the `callCore` result shape, sorted barrier `wait_for`, `ng.spawn` failing with `P_CHILD_BINDING` under `LocalProgramHost`, the budget that `compile_admit` pins, the all-positive ceiling, graph checks and barrier stalls, the typed Program errors, `compile_diagnostics` next to the sealed `diagnostics`, `llm_call` nodes failing admission with `P_ADMIT_PROVIDER`, `scheduler_threads`, `worker_count` and `NEOGRAPH_PYTHON_NODE_THREADS`, timeouts, in-memory-only runs, the `resume()` default and the post-resume `execution_trace`; the troubleshooting guide lists the matching errors, and the reference and concepts guides list `RunStatus::BarrierStalled`. The README points to that Program example instead of the Python examples, which do not use the Program API.
- **Examples run end to end.** Every example was run against real providers or in its offline mode, and the failures were fixed: the multi-agent debate and the self-evolving chatbot pass turns on as portable text instead of replaying native state into differently configured calls; the-beast variants give model-authored `llm_call` nodes a model, use the shared zero-data-retention provider factory and report run failures; the A2A client defaults to example 38's port; the Harness MCP server works over loopback HTTP without a token; example 34 raises its stream limits for generated images and uses an image-capable model for that section; the program chatbot demo policy matches descriptor policy revision 4; the multi-tenant live server takes `NG_MULTI_TENANT_REQUESTS` and `NG_MULTI_TENANT_WORKERS`; the Crawl4AI clients accept base URLs with a path and send the bearer token; the RE agent checks its GhidraMCP backend before it needs a key.
- **A tool failure caught by the parallel tool-dispatch fallback is valid JSON.** When a call in a multi-call batch failed outside the per-call error handling, its tool message was built as `{"error": "..."}` by string concatenation, so quotes, backslashes or newlines in the exception text produced invalid JSON. It is now serialized with the JSON library.
- **Tool failures follow one contract on every tool path.** In a multi-call batch, a `HookBoundaryBlocked` became an `{"error": ...}` tool message the model could read and route around, while a single call stopped the run; both now stop the run. A `HookBoundaryBlocked` or `NodeInterrupt` raised by tool code, C++ or Python, was turned into a failed tool result on every path by `ToolExecutionController`; it now stops or pauses the run, so a tool can ask for a human. A mandatory tool Hook that cannot run, for example because an `llm::Agent` run has no owner scope, now blocks the run instead of failing every call with a message to the model. A throwing `get_name()` is now that call's `failed` result in one call as in a batch, where a single call used to fail the node; batch fallback messages, which carried only `error` and reached the model as non-error results, now have the same `tool_status` and `{"error", "status", "retryable", "effect_uncertain"}` shape as any failed call. A node retry policy no longer retries a `HookBoundaryBlocked`, and when several calls of a batch stop the run, cancellation wins, then a Hook block, then an interrupt. Python's `HookBoundaryBlocked` now has `invocation_id` and `state`. See the [tool failure contract](docs/reference-en.md#tool-failure-contract).
- **MCP tool errors keep their message when the tool declares `outputSchema`.** The client validated the output schema before checking `isError`, so a legal error result without `structuredContent` failed with "MCP tool advertised outputSchema but returned no structuredContent", and the tool's message, such as `[FUNCTION_NOT_FOUND] No function found named 'x'`, never reached the caller or the model. Error results now skip output-schema validation, as the MCP specification allows and the official Python SDK does; successful results are still validated.
- **Program publication builds with libc++ without relying on node-handle move assignment.** Lease retirement directly initializes the extracted handle after all index publications, preserving commit atomicity without a library-specific `noexcept` trait.
- **Windows interface-6 archive fixtures create private custody directories atomically.** A protected, current-user-only DACL satisfies native archive admission; all outcome, monetary evidence, replay and wrong-binding assertions remain enabled.
- **Parallel client-tool results retain one turn for Anthropic Messages and Gemini.** Adjacent portable result-only messages are grouped before family encoding; mixed, native and turn boundaries remain intact. Other API families and stored history are unchanged (#311).
- **History compaction preserves authority and incomplete evidence.** It forwards supplied typed controls, accepts only clean nonempty summaries, records rejected outcomes without truncating history, and marks summaries as user-derived rather than system instructions. Authentic native replay prefixes remain protected (#314, #325).
- **Program fan-out joins no longer lose an early completion.** Persistent join state handles signal-before-wait, scoped cancellation stops dispatch and cancels current or late-attached children, and partial-launch failures drain every borrowing producer before propagation. Await registers its deadline before potentially synchronous child publication (#341).
- **Child generations and held migration use coherent durable heads.** Generation checks follow immutable admitted lineage; record/journal snapshots cannot mix publications, and releasing a held source cannot renew elapsed wall-time budget (#324). Native PostgreSQL heads use one MVCC statement; admission and recovery move owned head members instead of copying them.
- **Child reservation recovery keeps already-admitted wall-time grants.** Cold reconnect binds retained reservations to the immutable original generation when replacement has less wall time remaining. The current spendable budget, token and monetary limits, and original child deadlines are not renewed.
- **Synthesis persistence fixtures use the parent run budget instead of an unrelated local await deadline.** Dedicated timeout and cancellation tests continue to enforce operation deadlines.
- **Blocking Tool exception handoff has one terminal owner.** Typed exceptions and system error codes survive the async-to-blocking bridge without worker-side exception access after publication (#339).
- **Examples and benchmark drivers fail on unusable output.** Partial/refused provider output cannot become a successful routing fallback; UTF-8 previews retain complete code points, and failed benchmark attempts are excluded from successful timing samples (#317).
- **Durable Program integration coverage checks isolation and recovery.** Cold rematerialization retains per-node attenuation; provider journals and Tool effects distinguish runs, attempts and slots, reject changed authority, and preserve unresolved dispatch evidence across reconnect (#293, #295, #297).
- **Multi-Send fan-out on an engine worker pool could run a branch's cancellation handler while that branch's own operation was completing.** The branches of a multi-Send step ran on the bare pool executor, while static fan-out branches already had a strand each, so a cancellation emitted from another pool thread raced the branch's coroutine (data races in `cancellation_signal::emit`, reproduced under ThreadSanitizer). Each pooled Send branch now runs on its own strand; runs without a pool are unchanged. See #344 and #345.
- **The HTTP-enabled `neograph-harness-mcp` and `example_harness_mcp_server` now link `OpenSSL::Crypto`.** Both call OpenSSL directly (`SHA256`, `CRYPTO_memcmp`) but never linked it themselves; with SchemaProvider 0.2.0 no longer linking libcrypto, a shared-library build with `NEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` failed at link time with an undefined reference to `SHA256`.
- **Synchronous graph runs now wait for the scheduler's posting tails before destroying their private `io_context`.** A pool thread could still be inside the wake-up after its completion handler had already run, racing `pthread_cond_signal` against `pthread_cond_destroy` (#346). The blocking bridges fence foreign posts and tracked-work releases, close cancellation admission, drain late queued notifications, and only then destroy the context. Caller-thread execution, nested runs and parent cancellation authority are retained; a throwing posted handler cannot abandon its still-owned coroutine frame.
- **The SDK pin advances to SchemaProvider `8f32af7`, still SDK 0.2.0 and interface/shared-library generation 5.** The SDK now zeroes its archive activation key on every failed provisioning path (SDK #10), no longer reads libcurl 8.21's synthesized completion timing as proof that a request was sent (refused connects and untrusted TLS stay `NotSent`, so their safe retries work), seeds literal addresses into libcurl's DNS cache so libcurl builds without c-ares (Homebrew on macOS) stop spawning resolver threads beyond the configured worker budget, and keeps the qualification meter's fixed table off thread stacks. The SDK's own suite now runs on native Windows and on a real macOS runner. This is a source pin, not a new SDK release or version.
- **An engine released by one of its own fan-out workers no longer crashes the process, and releasing one while holding the Python GIL no longer deadlocks.** A failed fan-out branch keeps its exception until its coroutine frame unwinds on a worker, after the run has returned. When that exception owned the host object that owned the engine (a Python exception whose traceback holds the `GraphEngine`), the worker dropped the last reference and `~GraphEngine` destroyed the engine's thread pool on a thread of that pool. Joining a pool from its own thread skips that thread, so the pool was freed under the worker, which then used it as it left its handler: an intermittent segmentation fault, seen in the Python test suite under load. Such a pool is now stopped at once and destroyed by a thread outside it after the worker has left. In the Python binding, the thread that drops the last reference to a `GraphEngine` now releases the GIL while the destructor joins the workers; a worker releasing a failed branch's Python exception needs the GIL, so the join could wait for it forever.

## [0.13.1] - 2026-10-07
NeoGraph `0.13.1` targets alpha SDK `0.1.1`. Interface revision and shared-library generation stay 4, so consumers only select SDK `0.1.1` (its installed package config requires the exact version) and rebuild. Native archives remain v3 / `spna3`; portable JSON remains v2. The functional change is the Windows transport fix below: the Windows wheels published as 0.13.0 contain the defect and should be replaced by 0.13.1. Hosted CI and the Windows wheel gate passed for the fix commit (GitHub Actions runs 37576205702 and 37576205395, CPython 3.12 only); the full CPython 3.9–3.13 matrix runs on the release tag.
### Fixed
- **Windows: a large request body could stall until the operation deadline.** The SDK transport waited for `FD_WRITE`, which Winsock records only after a `send()` has failed with `WSAEWOULDBLOCK`. A send that succeeded and left the socket full produced no event, so about 4-6% of 4 MB uploads (measured on loopback) stopped at roughly 131 KB or 197 KB and ended as `DeadlineExceeded` with retry safety `PossiblyAccepted`. The pinned SDK now also probes `select()` on a short timer while a write wait is pending. The published 0.13.0 Windows wheels contain the defect. Regression test: `test_large_post_does_not_stall_when_the_send_buffer_fills` (150 fresh-connection rounds on Windows; it failed in 8 of 8 runs on the 0.13.0 wheel). Issue #340.

### Changed
- **CI is tiered.** Every push and pull request now runs only `ci.yml`: the full native suite on Linux, macOS and Windows and two installed-consumer rows, skipped for documentation-only changes. Sanitizers, the fuzz canary, performance gates, native ARM64, gRPC, the Visual Studio generator and the other eight installed-consumer rows moved unchanged to `ci-extended.yml`, which runs nightly and on demand. Wheels build CPython 3.12 per platform for packaging changes and CPython 3.9–3.13 on release tags, a weekly canary and manual runs, one job per interpreter. Windows dependencies reuse vcpkg binary archives. `verify_ci.py` accepts `--jobs auto`, `wheel --python`, and `native-windows --generator`; `native-windows` now builds with Ninja by default (run it from an x64 MSVC environment, or pass `--generator "Visual Studio 17 2022"`). All runners remain standard GitHub-hosted runners.
- **ctest runs with a per-test timeout.** `verify_ci.py` passes `--timeout 600` (1200 under ASan and MSVC ASan), so a hung test fails by name instead of consuming the job limit (issue #341).

## [0.13.0] - 2026-10-07

NeoGraph `0.13.0` targets alpha SDK `0.1.0`, loaded interface revision/shared
generation 4. Rebuild all native consumers and Python wheels with matching
headers/libraries. Native archives remain v3 / `spna3`; portable JSON remains v2.
The retained-feature integration passed local Linux x86_64 native and installed
wheel checks. Hosted Wheels and CI for commit `79c21e1` (GitHub Actions runs 37459723655 and 37459723545) built the Linux x86_64, Linux aarch64, macOS arm64 and Windows AMD64 wheels and passed every CI job except the hosted native Windows job (`build-windows`), which exceeds its 30-minute limit before ctest; publication follows the protected release approval.
Earlier paid observations and benchmark cohorts retain their SDK3-era scope.

SDK4 Linux x86_64 validation exercised all 27 registered cases: 25 passed in
the initial full run, then the two corrected cases passed a focused rerun.
This is not a second full-suite 27/27 run. The unchanged Stop CLI passed its
four buffered/SSE valid/invalid client-call cases, and three exact installed
README consumer variants passed (reported zero, unknown count, HTTP400 failure).
These local proofs add no vendor or Windows/macOS/ARM64 qualification.

NeoGraph SDK4 coverage: 1261 Core cases across the initial and corrected focused
runs; 691 Program passes with 14 designed Memory process-loss skips; 504 installed
Python cases across 499 initial passes and five corrected focused passes. These
counts do not claim another full-suite run. Local A2A 0.x/1.0 peers, evolution file
mode, plan replay, changed C++ helpers and 11 Python applications were exercised.

### Changed
- **Windows provider construction and byte-budget bridge.** The SDK gate `NativeSocketRuntime` (formerly `PosixRuntime`) is now granted on Windows, whose transport already has a Winsock event path. The shipped `max_host_connections` admission ceiling was `INT64_MAX`, larger than a Windows 32-bit `long`, and Windows runs rejected it; it is now 2147483647 and a regression test pins it. NeoGraph's SDK Bridge now receives its byte budget before its pmr containers are built: MSVC's `unordered_map` allocates in its constructor, so the previous zero budget raised `bad_alloc` there. Native MSVC Release: 1915 of 1915 registered cases passed (two designed skips), from 209 failures of 1916 before these changes. Test-only changes: three `exception_ptr` identity assertions use a platform-aware helper because MSVC's `current_exception()` returns a copy; one trusted-archive fixture test is skipped on Windows like its siblings because it only sets POSIX permission bits; the Images/Veo HTTPS peers also listen on `::1` because `localhost` resolves there first and Windows needs about two seconds to refuse a loopback connection. The 1915-case run was done on a developer machine because the hosted native Windows CI job times out before ctest; the hosted Windows wheel job built, repaired and ran the installed-wheel suite for CPython 3.9-3.13. The SDK's own POSIX-only suite does not run on Windows.
- **Windows wheels build and test on hosted CI.** The first hosted Windows wheel run needed four fixes: the wheel job now provisions its own vcpkg checkout at the manifest's `builtin-baseline` (the runner image's checkout predates it); its CMake defines go through `SKBUILD_CMAKE_DEFINE` because the quoted `CMAKE_ARGS` value split into junk variables under cibuildwheel and CMake kept looking in `<vcpkg root>/installed` (the toolchain honours `VCPKG_INSTALLED_DIR` only in manifest mode); `verify_ci.py` names vcpkg's `openssl.cnf` for the TLS test fixtures; and one archive-custody Python test is skipped on Windows (`mkdir(mode=0o700)` creates no owner-only DACL). The wheel vendors libcurl 8.22.0, OpenSSL 3.6.5, libpq 18.4, nghttp2 1.70.0, SQLite 3.53.4, zlib 1.3.2 and the Microsoft Visual C++ runtime DLLs `msvcp140`, `msvcp140_1` and `msvcp140_atomic_wait`; those three are excluded from delvewheel's name mangling so they stay unmodified with valid Microsoft signatures, as Microsoft's Distributable Code terms require. `THIRD_PARTY_LICENSES.md` records the artifact-observed macOS, Windows and Linux closures and what is still missing (exact-version notice texts for the added components). Hosted run 37459723655 produced CPython 3.9-3.13 wheels for all four platforms; checking the signatures of the published Windows artifacts is part of the release check.
- **Shared local/hosted verification entrypoint.** `python scripts/verify_ci.py <profile> --work-dir <fresh-output> --jobs N` centralizes fourteen existing CI gate profiles without removing native platform, sanitizer, installed-consumer, wheel, archive, or release gates. Installed consumers no longer require Git/Bash; the separate immutable QuickJS performance gate still requires Linux/Git provenance. Native Linux runs the full Python/protocol suite serially after native PostgreSQL tests. The benchmark checker preserves the four original workloads and numeric thresholds, rejecting ambiguous or invalid metrics. Source review and CLI help do not qualify runtime/platform/performance behavior; integrated execution remains required.
- **One QuickJS matrix command.** Replace the duplicate shell build driver with `python scripts/verify_ci.py quickjs-performance --work-dir <fresh-build-root> --jobs 4`; retain `<fresh-build-root>/quickjs-performance.json`. Fresh owned outputs are refused if already present. The immutable performance runner, preregistration and source-root Git provenance requirement remain unchanged. Native Windows toolchain forwarding and work-dir-specific wheel vcpkg roots preserve the declared bootstrap/repair gates; PostgreSQL prerequisites use an explicit database URL for `psql`. These are source changes, not claims that the new profiles passed.
- **Drained cancellation-context lifetime.** Detach releases completed Asio slot handlers before their context is destroyed; generation fencing discards stale queued emissions while callbacks may rebind without token-lock recursion. Cancellation state is not reset. Rebuild NeoGraph and all native consumers together. The 23 local cancellation/fork cases and a real plain/ASan retained-token consumer passed. [Commit `24d3166` CI](https://github.com/fox1245/NeoGraph/actions/runs/37275542831/job/111651716661) passed its 1,333-case TSan run with three skips, including the retained-parent-token teardown regressions. Existing dependency suppressions and disabled ASLR bound this proof; it is not universal race-freedom. Detach still requires drained operations and must not run from its active signal callback.
- **Platform wheel prerequisites and cold loading.** Pin pybind11 to `2.13.6`, resolve the SDK's direct OpenSSL Crypto dependency, and use matching LLVM 20 headers/runtime for macOS 14. Preserve every CPython 3.9–3.13/platform row and the full installed-wheel suite; Linux replacement and macOS Homebrew-denied cold-loader checks run afterward. The exact final artifacts remain a release gate, not a source-only portability claim.
- **Matching Linux LGPL library source.** The sdist includes the library-only keyutils/libxcrypt source, downstream patch, build and replacement material in `deps/redistribution/`. Preserve file-scoped grants and recipient modification rights; excluded unknown-grant upstream tests are not library inputs. Source/notice delivery alone does not prove the final wheels' replacement arrangement or legal clearance.
- **Python cookbook import.** The native OpenRouter recipe imports `SchemaProvider` from `neograph_engine.llm`, matching the installed package rather than a removed top-level export.
- **Explicit source distribution allowlist.** Require scikit-build-core 1.0 or newer and use its `explicit` inclusion mode with root-anchored files. The actual 1131-file sdist passed allowlist, private/cache exclusion, SDK4 pin and required-content checks; `manual` mode had included unrelated files. Wheel commands use the extracted package directory and pass test paths as arguments, including paths with spaces or Windows backslashes.
- **Retained typed controls and model rules (#309).** Reconciled newer-master `c708e51a`, `165dbbbd` and `5553d9ea` through family-specific controls: Chat reasoning/include/usage/alternative models, Responses provider-held cursor/parallel tools/verbosity/truncation/include, Messages manual/adaptive/disabled thinking/effort/cache/tool choice/routing, and Gemini thinking level/safety/tool choice/temperature. Wrong-family/origin/value combinations reject before I/O. Admitted model-prefix rules match case-insensitively, including gateway suffixes, and reject forbidden explicit temperature; Messages thinking omits temperature. No JSON knob interpreter is restored.
- **Explicit history and deployment headers (#305/#306, #310).** Reconciled `9e161fc8`, `c5035e00` and `02b4c275`. Authentic same-route native reasoning remains owned. Gemini `PortableForeign` admits unsigned assistant text/client-tool history; only its first foreign function call gets the documented bypass, and failed native seals are never demoted. Responses `previous_response_id` uses new input plus non-wire `previous_response_history` when genuine tool ownership requires it; provider-held state is not full native/archive replay authority. Deployment environment headers are explicit host preprocessing before admission, with optional omission and case-insensitive override precedence.
- **Retained outcome, usage and retry semantics (#307/#308/#312).** Newer failure/terminal fixes (`782e23ea`, `0eb271de`, `f89afe08`, `99fa81a2`) remain typed owned failures/partial evidence, including HTTP-200 errors. `ec69b845`'s cache/reasoning intent uses nullable full reports, not missing-as-zero counters. `054a2c1c`/`b67ccf4c` are represented by SDK no-output-replay safety and bounded exponential full-jitter policy, without a legacy retry wrapper. `51374ce8`'s client-tool intent now agrees in Stop events and final outcomes for valid and invalid calls; specific stop reasons and server-executed tools remain distinct.
- **Retained A2A wire and HTTP streaming fixes.** Integrated `08c4df70` and `cd21e3e3`: card-selected 0.x/1.x dialects change method, body and version header together; SSE task/status/artifact assembly preserves output and never dialect-retries after observed events. Server encoding follows the version header. Answer precedence is terminal/interrupted agent status, artifact, then agent history. Fixed-length stream replies preserve bounded body/status and reject early EOF or buffered surplus. Bound discovery/restart behavior remains. Local native/Python peers passed; macOS qualification remains pending.
- **Retained research, fork and example fixes.** Integrated `861619a4`, `03e78f6d`, `5553d9ea` and `9b4b2776` through typed paths. Research adds at most two calls for completed empty `MaxTokens` without tool calls, doubling the cap to 16384 under the original bank/deadline while retaining distinct call identities and every outcome/usage report. Visible partial reports are `Incomplete` without retry; empty final reports fail. Exceptions and delivered stream parts cannot authorize redispatch. Fork retains pending continuation; example 08 keeps its new-turn flow. Evolution registers `pnoop` in both modes; plan replay saves four calls. Scoped research, file-mode and changed-helper checks passed locally.
- **Breaking typed Python provider API.** Python uses `make_provider_request`, `Provider.prepare`, single-use `Provider.dispatch`, and owned `ProviderOutcome` completion/failure views. `SchemaProvider` accepts `ValidatedDescriptor`, `ProviderRuntimeOptions`, and `SchemaProviderDefaults`; typed `ProviderMessage` is separate from graph convenience `ChatMessage`. Missing usage stays `None`. Legacy completion APIs and `OpenAIProvider`/`RateLimitedProvider` are removed without shims. Installed Linux wheel and actual example execution passed in the scoped cohorts above, not a new qualification of every platform or hosted vendor.
- **Event-driven provider handoff; mandatory C++ rebuild.** Replaced the SDK Bridge's 1ms completion/cancellation polling with capacity-one coalesced Asio notification. Active drains resnapshot publication hints without extra wake submissions; context shutdown detaches non-owning publisher endpoints before channel teardown. `CancelToken::stop_token()` uses the same `std::stop_source` state as `is_cancelled()`, preserving fork/Asio-slot cancellation while forwarding unbound/shared tokens without stealing a slot. Rebuild all C++ consumers with matching headers/libraries; replacing only a pre-v1 shared library is insufficient. Local TLS GraphEngine A/B:48 process records,4800 measured graph runs,zero failures;256B padding p50 1.302474→0.952992ms and throughput738.90→1022.96 graph runs/s. Tool/SSE throughput fell2.54%/6.69%, so this is not a universal speedup.43 targeted regressions,200 ASan/UBSan publisher-teardown rounds and a300-run installed-SDK consumer passed; TSan could not execute on this host. Parser/ISA flags and paid/Python qualification are unchanged. See [measured results](benchmarks/provider-notification-summary.json).
- **E2E-exposed protocol/demo corrections.** `neograph_get` now selects the requested retained URI and its cursor page instead of ignoring it; stdio and HTTP lifecycle/URI checks and retained SQLite restart passed. Empty A2A discovery URLs follow the real bound endpoint across restarts while configured public URLs remain unchanged; the uppercase example advertises its exercised SSE capability. MCP time demos honor the requested IANA timezone. ProgramChat mock commands select an explicit shipped model-fact policy instead of bypassing bounded admission. Fresh verification covers 39 numbered behavioral targets, nine offline Beast modes, Assembly A2A, Jarvis persistence, dedicated mock topology load, retrieval admission and ProgramChat SQLite/PostgreSQL/browser paths; the separate measurement-only, live, voice, GUI and deferred Python boundaries remain explicit.
- **Documentation/comment consistency.** Archived Stage 3 proposals explicitly mark removed Provider completion APIs as superseded and link to current typed prepare/dispatch guidance. Cookbook status now distinguishes the exercised offline Assembly/Jarvis/Beast and ProgramChat PostgreSQL/browser scopes from historical timings, unqualified live/voice/Python variants and the separate multitenant load recipe. Current cancellation/HTTP/isolation comments no longer describe removed provider APIs. No runtime behavior or qualification boundary changed.
- **Breaking C++ typed provider/custody contract.** Rebuild every consumer and custom provider with matching headers/libraries. Core now requires external `SchemaProvider::runtime` even with `NEOGRAPH_BUILD_LLM=OFF`; providers use owned `ProviderRequest` → move-only `PreparedProviderRequest` → immutable `sp::runtime::Result`, explicit Collect/Stream mode and declared family controls. Completion compatibility types, the JSON descriptor interpreter, primitive registry and Responses WebSocket path are removed without aliases or shims. SDK `0.1.0` is alpha (interface revision/shared generation 4), not a stable interface. The earlier C++ runtime/archive qualification recorded below used SDK3 on Linux/POSIX; it did not exercise Python wrappers or the retained SDK4 changes.

  Fresh installed find_package Program C++/C ABI/dualQuickJS consumers and the NeoGraph/SchemaProvider typed two-request lifetime/native/raw/mismatch consumer passed. Interface/ABI declarations alone remain distinct from this exercised package result; broader platforms and stable release are not claimed.
  Native custody is closed v3 / `spna3` (v2 rejected), binding every semantic descriptor field and exact owner/custody. Diagnostic duplicate keys retain original raw evidence but cannot become executable configuration; non-2xx JSON and named SSE errors remain factual SDK evidence. Host-positive `observer_limits` tighten only, and digest v3 binds their effective ceilings. PMR ownership includes queued and draining containers, with cancellation outside the queue mutex. Generic messages stay generic; mapped native history retains its C++ sidecar.
  Common `ProviderOutcomeError`, `ProviderFailure` and `ProgramFailure` retain full SDK outcomes and original/secondary live causes; no outer Node/Program redispatch occurs. Native failure witnesses require genuine archive custody and Runtime hydration; data-only prefilled witnesses cannot bypass admission, nonpersistable failures cannot publish/replay, and restart leaves the original exception pointer null.
  `RecordedBindingSet` is source-bound data authenticated by trusted Catalog capture from source events. `replay_recorded` transfers actual remaining credit by durable CAS after checking original selected-source permissions; inherited spend is not a new grant. The renewable `start_recorded` API is removed; exact owned Program leases never renew on expiry, and captured JavaScript commands cannot repeat external effects. Archive possession never grants spending authority. Three separately authorized minimal Images/Veo/Decisions calls succeeded, not broad qualification or invoice proof. Actual integrated verification and remaining limits are reported below; older M5 measurements are historical.

  **Recorded-control causal fix exercised in the full suite.** Captured command replay durably reserves only new CPU wall-time/Core work before execution, then publishes measured work and any newly produced Core checkpoint through the result CAS. It consumes no new model, money or Program-operation allowance and does not redispatch captured external effects. An unreconciled reservation remains debited. The reservation selects the authenticated settlement transition rather than an ordinary Running→Running transition that rejected the first new Core checkpoint. Await channel receive, timer wait/cancel and handoff wait initiation/release are serialized on their owning executors/strands; the existing Recorded CPU/Memory await/handoff scenarios passed in the full suite; remote TSan coverage limits remain explicit below.

  **Completed paid observations; not universal qualification.** Original `SPQUAL1` base630/1000000 microUSD is unchanged; ONE hash-chained `A` admits approved extension480/3000000 in the same original ledger, aggregate1110/4000000, with cumulative calls/spent/holds/settlements and no new grant ID/header/reset. Exact declaration bytes/file identity and original authorization/baseline/catalog/activation/ledger-prefix hashes/totals remain pinned; removal/replacement/change fails closed. The final canonical ledger is calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000; spent+held is US$1.725786 LOCAL catalogue meter, not an invoice. The documented five-family60-pair baseline completed600 requests: Chat60/60, Responses60/60, Messages60/60, Generate56/60 (four incorrect-vision SSE), Interactions57/60 (one buffered and two SSE incorrect-vision); aggregate293/300 pairs, not300/300. Other old600 financial records remain preserved, not full behavioral proof. Earlier M5/media one-shot cohorts are unchanged. The earlier three-round Google prerequisites retain two invalid-tool and one unreadable-positive failures. Final SDK evidence and native-axis limits are separate from baseline success. Earlier activation/reopen smoke remains recorded at calls610/spent219159/held751233 after two reopens, with SDK meter/canary/vision four tests passed19.38seconds; these are scoped prior checkpoints, not final ledger totals. The earlier verified Chat60-pair cohort retains120 actual attempts,120 UpperBound charges and no UnknownHold.

  **Native-axis observations, not cryptographic verification or native consumption/equivalence.** Generate accepted mutation, omission and duplication. Interactions accepted the isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission and duplication. Removing all thoughts/signatures returned generic400; removing all signature fields while keeping THOUGHT items also returned generic400. The last capture had a local encoded-original retention control, not a same-capture server positive; the earlier positive cohort remains genuine. These observations establish an aggregate-carrier-absence boundary only, not issuer/signature validation or vendor consumption. Actual reports: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`; prerequisite-failed/not-run/negative-inconclusive states remain factual. Thought-only/carrier-only omissions were accepted while another carrier remained; this does not strengthen issuer-validation or native-consumption claims.

  **Actual integrated proof and remaining limits.** Latest Core full run:2242 tests, zero failures,16 skips (14 RAM process-loss cases not applicable; two live-credential gates),130.17seconds. `PgNestedJsonRoundTrips` preserved exact duplicate keys/order/null metadata, blob and residual in0.18seconds. The unchanged original shared-bank fork and existing Recorded CPU/Memory await/handoff scenarios passed. Real wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probes passed plain and ASan+UBSan. LOCAL Memory/SQLite/PostgreSQL TSan scopes:seven passed,zero warnings. Full mixed gRPC plus system Abseil/Protobuf TSan exited66 with402 race warnings in dependency/generated-RPC stacks: an instrumentation/coverage limit, not a proven false positive; remote TSan/race-freedom is NOT claimed and no warning is suppressed. Installed find_package Program C++/C ABI/dualQuickJS three consumers passed. Fresh installed NeoGraph/SchemaProvider typed consumer passed two real HTTP requests, provider destruction before coroutine start, native/tool replay, refusal,known-zero/raw retention and actual LinkedMismatch rejection. Browser Alice/Bob isolation and generation2 replacement were visually verified; PostgreSQL Program Chat six black-box tests passed18.989seconds. Latest SDK26/26 passed,zero failures,74.07seconds. Final ReleaseGraph16 configurations ×3 fresh process repetitions/48 records completed38.29seconds,zero failures,all actual protocol/owned-outcome checks passed. NeoGraph `benchmarks/provider-cutover-final-results.json` and `benchmarks/provider-cutover-final-summary.json` retain this separate final cohort. No compiler or paid model ran during measurement; historical cohorts stay unchanged and semantic/resource equivalence is not claimed. Unstable SDK/ABI3 is not a stable release or broader-platform qualification.

  The completed chat pairs do not establish native-continuation consumption by a downstream vendor.
- **Standalone bank journal correction — current contract revised; exercised runtime evidence below.** The protocol requires a monotonic trusted-store namespace obligation and a real immutable original owner/thread/graph scope, ceiling, deadline/clock identity and generation. Only exact durable head CAS over the full checkpoint commitment and revision may issue a host-owned opaque lease. Exact pending effect windows must persist before provider I/O; settlement must use genuine SDK outcomes and actual charges, nullable reports, holds and dedup identities. Checkpoint and next head must publish atomically under the same owned actor/revision. Removing bank metadata, pruning a checkpoint, replaying an old authenticated snapshot, overwriting the same ID or losing the actor must not grant credit. Tightening a 130 ceiling to 129 with an existing 65 hold cannot admit another 65; a proven no-effect failure may release the unchanged head so authentic 130 recovery can still proceed. Crash/unknown/lost-lease windows remain held without refund, retry or fallback. Plain/pristine archive configuration grants no money or native spending lease, and current `config.usage` cannot replace an existing standalone obligation; Program/external-bank journal ownership is unchanged. This is the required contract; actual currency/custody evidence and instrumentation limits are reported below, not a stable released API guarantee.
  Actual declarations now include `ManagedBudgetLeaseScope`, `OwnedManagedBudgetLease`, `ManagedBudgetEffectReceipt` and the checkpoint-store acquire/begin/settle/publish/release operations. `begin_managed_budget_effect` binds the exact claim and prepared request digest. Actual backend/runtime currency probes and installed-consumer results are reported below; declarations alone are not runtime evidence.

  **Genuine InMemory shared-bank fork retained and exercised.** The original genuine C++ fork uses ONE original financial journal and trusted current branch heads, not cloned grants. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` (and `_async`) requires the authentic current source/full commitment and actual same-bank native C++ pointer; durable standalone forks remain explicitly unsupported. `OwnedManagedBudgetLease::scope()` and original owner/thread/graph, ceiling, deadline/clock and generation remain immutable. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` select the execution branch separately; `GraphState::budget_original_thread_id()` identifies the original financial bank. Exact selected-branch head CAS and global actor/revision serialize all branches against canonical current counters, pending effects and burned identities. Original and fork branches remain usable without replenishment; stale snapshots, copied checkpoints and imported JSON cannot mint aliases or rewind heads. The original root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank proof PASSED in the unchanged test_graph_engine.cpp:810–913; saved original ceiling30 is separate from effective fork ceiling20; widening31 and JSON-only restore must reject. Unbounded reported observations are factual data, not finite grants. Only a proven zero-effect lease can release an unchanged head; unknown/pending effects keep their obligations.

  **Current release-error contract; exercised suite/probes below.** `graph::ManagedBudgetLeaseReleaseError` in `<neograph/graph/engine.h>` derives from `ProviderOutcomeError`. `cause()` preserves the original execution exception and `release_error()` exposes the secondary durable lease-disposition failure. `outcome()` retains genuine SDK evidence when available and is null when no SDK outcome exists; release failure cannot invent an outcome or permit redispatch. Closed `_neograph_managed_budget_scope` metadata describes original logical scope/cap/deadline clock/generation, but is data rather than backend CAS authority.

  **Archive-owner/retention contract; exercised suite/probes below.** Only finite standalone roots or authenticated finite sources inherit an omitted original owner from the genuinely configured `sp::NativeArchive::owner_scope()`; unbounded/plain owner metadata semantics are unchanged. An explicitly conflicting archive owner is rejected before lease acquisition. `CheckpointStore::retains_native_checkpoint() const noexcept` and the corresponding Core/Async storage capability default to false; the real InMemory backend overrides true, and wrappers must delegate actual retention. This read-only description permits legitimate unleased/plain/unbounded C++ native checkpoint custody; it grants neither spending credit nor native replay authority. Leased custody uses the actual store-issued receipt rather than a JSON flag or guessed store type.

  **Native-custody pre-I/O gate; exercised suite/probes below.** Beginning a managed effect requires a genuinely bound NativeArchive or the actual local store-issued private C++ retention capability before any pending-effect, slot or held-window mutation. The private capability is never imported from JSON or transferred over the wire. gRPC requires real client and server archives even when the remote backend is InMemory, because a C++ sidecar cannot cross that boundary. Original anonymous owner scope remains empty when no archive supplies a finite source owner; a real archive binding must match the original scope. Financial head/lease evidence alone does not prove native-custody readiness.
- **Portable WASM smoke builds.** Distro Emscripten 3.1.x now enables
  Asio's available coroutine header explicitly and avoids unsupported
  stack-protector symbols only for WASM targets; native hardening is unchanged.
- **Bounded async transport failures.** Stdio MCP exchanges preserve reader
  protocol/connection errors instead of masking them behind the RPC deadline,
  and a timed-out request no longer kills the shared subprocess session.
  SchemaProvider cancellation normalizes concurrent socket-reset races to
  `asio::error::operation_aborted`.
- **Owned graph tool bindings.** `NodeContext::tools` is now an owned,
  copyable `ToolSet` retained across compilation and engine linkage. Context
  reassignment does not invalidate earlier engines; the unsafe post-compile
  `GraphEngine::own_tools()` transfer has been removed. Python-defined,
  MCP-discovered and native C++ tools use the same compile-time lifetime rule.

### Added
- **Non-recursive async-primary checkpoint adapters.** Native coroutine
  stores use `AsyncCheckpointStore` plus `adapt_async_checkpoint_store()`;
  sync-only stores use `CheckpointStoreCore` plus the bounded-worker adapter.
  Legacy missing sync operations now fail explicitly without changing the
  `CheckpointStore` vtable or checkpoint wire schema.
- **Engine-scoped graph registries (#218).** `GraphRegistry` defaults to
  built-ins without process-global custom entries; `GlobalFallback` opts into
  legacy registrations. Engines snapshot registrations at construction, so
  later mutations cannot alter running graphs. Python exposes scoped node,
  reducer, and condition registration, `GraphEngine.compile(..., registry=...)`,
  and registry-aware topology schema export. Legacy global APIs remain available.
- **Execution-only protocol hosting and guarded state administration.**
  A2A, ACP, and gRPC retain an owning `GraphExecution` capability instead of
  administrative/configuration access. `GraphAdmin` and legacy direct state
  calls now reject while an engine executes, and execution rejects during
  administration. Legacy engine constructors and methods remain compatible
  through the documented pre-v1 migration window.

- **Credentialless OpenCode global MCP adoption.** Added inspection-only,
  redacted discovery of user-global local stdio definitions plus explicit
  pinned/trusted-mutable launch and tool-manifest approvals. Adopted clients
  use canonical identity checks, replacement environments, bounded local
  subprocesses, and fail-closed manifest intersection; project/HTTP entries,
  imported secrets, recursion, and unknown or unselected tools remain denied.
- **Local authenticated Harness workers.** The opt-in installable stdio MCP host
  can preflight and delegate read-only worker calls to installed OpenCode,
  Claude Code, or Codex CLIs using their own saved login. The Provider path
  remains explicit; no host credential files or OAuth tokens enter NeoGraph.
  Subprocess trees, output/events, time and usage are bounded and host errors
  remain distinct from schema failures.
- **Transport-independent MCP protocol sessions.** HTTP and subprocess stdio
  now share JSON-RPC correlation, initialization, tool discovery, and tool
  adaptation while retaining separate transport ownership. Added typed
  `MCPTransportError` categories for connection, deadline, cancellation,
  shutdown, HTTP status, and invalid wire responses; JSON-RPC server errors
  continue to expose `MCPError` codes. stdio sync and async calls now share
  its session-owned I/O path and support concurrent calls after cancellation.
- **Experimental owner-scoped active Program starts.** C++ `ProgramRuntime`
  and Python `LocalProgramHost` can select an admitted immutable activation
  once for a new run, returning the observed activation alongside a handle
  pinned to its version; rollback changes future starts only. Graph migration
  now rejects opaque checkpoint metadata instead of treating unknown
  node-local state as transferable. Durable cross-store GC and production
  performance qualification remain separate gates.
- **Durable Program Core Tool grant identities.** Hosts may admit an exact,
  owner-scoped Tool grant in SQLite and rebind its gate/controller on each
  Program operation and reconnect. The runtime also checks the admitted
  executable-binding fingerprint; revocation survives restart and blocks all
  attempts using that grant ID. Effect results still require a separate
  per-call durable Tool broker.
- **Durable Program Core provider calls.** Hosts can bind an invocation-scoped
  `SQLiteProgramProviderCallJournal` through the existing Program resolver.
  It writes a durable marker before built-in Core provider transport, binds
  exact call slot/request/deployment independently of attempt provenance,
  stores completions for restart replay, and requires external evidence to
  reconcile uncertain outcomes without silent re-dispatch.
- **Durable mediated Tool effect journal.** An invocation-scoped SQLite host
  broker records the exact Program grant, executable binding, Core task/call
  slot and rewritten arguments before dispatch, replays verified receipts
  across restart, and blocks unresolved external outcomes instead of retrying.
  Program Core grants now require a Tool effect broker; standalone Core and
  Agent callers can supply an explicit host-owned broker identity.
- **Shared Graph host conformance.** A2A, ACP, and opt-in gRPC now exercise
  the same real-engine invocation, cancellation, admission, terminal-result,
  and nested Store/ToolGate contract. The gRPC suite has a dedicated CI gate.
- **Performance build configuration.** Added an opt-in
  `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` switch for local GCC/Clang builds,
  plus a warning when a single-config generator omits `CMAKE_BUILD_TYPE` and
  would silently miss Release optimization flags. Release hardening remains
  enabled by default.
- **Strict authentication for Program-backed A2A services.** Program A2A
  constructors now accept an opt-in `require_authenticated_requests` flag that
  applies the configured authenticator to ordinary message, stream, task-get,
  and task-cancel RPCs as well as collaboration envelopes. The default remains
  false for legacy compatibility.
- **Trusted-ingress tenant isolation (#244).** Added immutable `TenantScope`,
  fail-closed scoped Store/CheckpointStore and Harness namespaces, RAII-safe
  per-tenant concurrency/queue/token/cost/artifact quotas, and a
  non-secret `CatalogConfig::materialization_context_identity` cache
  partition. The existing multi-tenant cookbook now labels topology-sharing
  measurements separately from production isolation and extrapolation.
- **Host-owned A2A control-message interception.** `ProgramAgentAdapter` can
  now route an authenticated typed message to a host callback before starting
  `ProgramRuntime`. The callback must preserve task/context identity and its
  returned Task is retained for ordinary task observation.
- **Untrusted supplemental runtime context.** Added
  `ContextArtifactKind::UntrustedSupplemental` and
  `ContextPlacement::AfterHistory`. Existing artifact kinds retain their
  system-message behavior, while explicitly untrusted RAW or derived context
  can now be receipt-bound and delivered as user data after the complete
  chronological Human/AI/Tool window.

### Fixed
- **Subgraph recovery boundaries.** Administrative updates preserve interrupted
  continuation identity; retained children distinguish a new parent call from
  a same-call resume. Stateless static interrupts reject before effects, and
  interrupt recovery uses asynchronous checkpoint loading.
- **MCP and host-process isolation.** Adopted tools require independent pinned
  approvals and exact argument values; revocation/schema drift drain retained
  clients. Hardened subprocesses enforce startup/stderr bounds, handle/FD
  isolation and descendant cleanup; host output draining is bounded.
  macOS startup supports Darwin signal macros and reads the inherited
  environment through its platform accessor before fork.
  The POSIX close fallback uses the descriptor hard limit rather than a 65536
  cap, including descriptors retained above a lowered soft limit. Windows
  inheritance tests observe the original event, not reusable handle numbers.
- **Clean-runner contract gates.** CI explicitly installs Protobuf
  development headers and libraries, not only its compiler/runtime packages.
  Isolated stdio fixtures explicitly approve their discovered Python search path.
- **Tenant-safe durable Harness records.** Scoped IDs and references round-trip
  through File/SQLite stores, long File keys use bounded hash filenames, and
  SQLite retention cannot delete another namespace's records or journals.
- **Runnable Harness CLI.** Startup policy uses the supported Program child-depth
  ceiling; a real stdio compile/start/get regression covers the shipped binary.
- **Cold active Program starts.** A newly created catalog now materializes
  the already selected persisted version before run pinning, without rereading
  the activation pointer.
- **Durable generated-artifact replay.** Provider receipts retain ordered
  artifact payloads and nested metadata across reopen and reconciliation.
  Volatile SQLite paths and legacy receipts missing artifact evidence reject
  explicitly instead of inventing empty output or redispatching.
- **Ephemeral checkpoint resume safety.** Checkpoints with ephemeral channels
  now record a lifecycle guard in metadata. Resuming a written ephemeral
  channel, changing its declaration, or loading an older checkpoint without
  its guard fails explicitly instead of silently restoring its initial value.
  Isolated multi-`Send` workers and cache keys now see live ephemeral values,
  while durable overwrite, append, and custom reducers retain their
  materialized checkpoint format.
- **Explicit subgraph persistence modes and nested inspection.** `SubgraphNode`
  now supports compatibility `Legacy`, fresh durable `PerInvocation`,
  retained `PerThread`, and fail-closed `Stateless` policies. Stable
  length-framed namespaces, parent checkpoint invocation identity, nested
  `GraphEngine::inspect_nested_checkpoint()` lookup, and same-node
  persistent namespace collision rejection preserve cancellation, Store,
  ToolGate, and ordered output-delta behavior. SQLite/PostgreSQL reopen
  coverage is included where the backend is enabled.
- **Promo dependency security.** Updated the transitive `js-yaml` dependency
  from 4.3.1 to 4.3.2 in `docs/promo/package-lock.json` to fix the empty-merge
  CPU denial of service vulnerability (CVE-2026-84375 / GHSA-2883-xcg3-v3hh).
- **TSan CI RSS gate.** Skip the native-only RSS bound under GCC and Clang
  ThreadSanitizer, whose shadow memory and runtime bookkeeping can exceed
  the native threshold. Keep the 200-run concurrent stress test enabled under
  sanitizers and the unchanged 10 MiB RSS bound in non-sanitizer builds.

## [0.12.1] - 2026-08-23

### Fixed
- **Python example runtime contracts.** Updated route-channel examples to use
  reserved `__route__`, declared dynamic `Send` payload channels, opted the
  pure cache demo into `CacheScope::Reusable`, preserved OpenRouter base URLs,
  made Windows console output UTF-8-safe, and rejected or retried successful
  but empty reasoning-model responses.
- **Responses transport demos.** Official OpenAI now uses the real WebSocket
  streaming entry point, while compatible gateways select compiled HTTP/2 or
  HTTP/1.1. The deep-research demos use bounded token/time budgets and a
  three-way, two-worker fan-out that was validated live through OpenRouter.
- **Actionable Windows network failures.** Python now exposes `_HAVE_LIBCURL`
  and translates locale-encoded `std::system_error` messages into safe ASCII
  diagnostics with category and code, preserving errors such as Winsock 10060
  instead of replacing them with `UnicodeDecodeError`.

## [0.12.0] - 2026-08-23

### Added
- **Strict runtime interposition completion.** Added `StrictRuntimeProfile`,
  durable provider terminal-outcome receipts and SQLite schema v3 migration,
  transport-neutral mandatory Hook backends with JSON-RPC stdio/HTTP artifact
  publication, general required context and `HardConstraint` artifacts,
  exact-preservation `ContextTransformReceipt`, durable runtime developer
  instruction decisions, and a reserve-before-compile `ProgramSynthesisGateway`.
  Generated source still cannot activate, bind, migrate, or spawn itself; those
  remain separate host-owned Program transitions. Custom subclasses of
  `DurableProviderDispatchReceiptStore` must now implement terminal
  `settle`/`outcome`; C++ consumers must rebuild against this ABI change.

- **Isolated PostgreSQL Program-store integration fixture.** Added
  digest-pinned, loopback-only `tests/fixtures/q7-postgres/compose.yaml` with
  tmpfs storage and a health gate. When `NEOGRAPH_TEST_POSTGRES_URL` names a
  disposable test database, `ProgramCatalogTest.PostgreSQLProgramStoreReopensActivationAndOwnerVisibility`
  covers publish, activation, reopen, and owner isolation. This is test
  infrastructure.

- **Fail-closed QuickJS legacy-drain audit.** Added
  `scripts/audit_legacy_drain.py` and its CTest contract. The tool produces a
  canonical, content-addressed proof from an explicitly enumerated frozen
  Program/Harness storage snapshot; it rejects unknown or mutable records,
  unclassified legacy sources, drain-only records, and active or recoverable
  legacy runs. SQLite inputs with live `-wal`, `-shm`, or `-journal` sidecars
  are rejected before opening, so raw copies of active WAL databases cannot
  serve as final proof. This establishes the evidence mechanism but does
  not claim the deployment-specific final drain or legacy-parser deletion is
  complete.

- **PostgreSQL final-drain archive scan.** The legacy-drain auditor now
  accepts a frozen `program_postgres_dump` custom archive and invokes
  `pg_restore` only in data-only, strict-table, script-output mode; it never
  restores into a database. It validates the Program bundle, version, and
  activation tables against their persisted identities, rejects an archive that
  omits or changes any required table, and treats an activation of a legacy
  Program version as a final-removal blocker.

- **No-deployment final-proof mode.** The legacy-drain auditor now accepts
  a named operator attestation only when no pre-release or production NeoGraph
  deployment ever existed. That mode accepts neither storage targets nor
  historical legacy artifacts, labels the emitted proof
  `evidence_mode: "no_deployment_attestation"`, and fails closed for any
  mixed or unattested empty inventory. It cannot cover drained, deleted, lost,
  or inaccessible historical state.

- **OpenRouter provider routing.** `OpenAIProvider` now forwards an object
  passed at `CompletionParams::extra_fields.provider` into the Chat Completions
  request body as `provider`; non-object values fail before an HTTP request.
  This exposes OpenRouter's documented per-call routing preferences while
  leaving other native `extra_fields` keys ignored. The live Beast cookbook
  pins its provider and uses an explicit 180-second timeout for its
  4,000-token generation budget.

- **Copy Ninja local graph-node bridge.** Added transport-free
  `a2a::CopyNinjaNode`, which wraps a separately materialized Copy Ninja
  harness, reads `prompt`, and overwrites `response`. Added the
  `cookbook_the_beast_copy_ninja` live cookbook: its LLM may author only this
  fixed local node, must pass a fourth local-binding gate after the normal
  Core gates, and fails if the synthetic source agent observes an RPC. Card
  text, endpoint, credentials, and source remain excluded from the unadmitted
  candidate and the caller prompt never enters the authoring LLM request.


- **Optional Program component boundary.** Added the opt-in
  `NEOGRAPH_BUILD_PROGRAM` switch, exported `neograph::program` target, and
  `<neograph/program/program.h>` entry point. Installed package component
  discovery now reports Program only when built; Core-only installs keep the
  existing `neograph::core` link interface.

- **Immutable Program value model.** Added stable typed diagnostics, deeply
  owned canonical-JSON and C++-builder `ProgramSource` inputs, immutable
  content-addressed `ProgramBundle`/`ProgramVersion` values, canonical
  serialization, SHA-256 algorithm-tagged identities, source maps, imports,
  and strict versioned stored-value schemas. `neograph::program` is now a
  compiled exported library while remaining dependent only on Core.
  Bundle/version v1 projections now require sealed Core definitions and plan
  identities, semantic-versioned executable digests, contracts, closures,
  bounds, and typed admission/materialization receipts. Their identities bind
  the format and storage version, semantic sets use stable ordering, and
  diagnostics reject invalid pointers, reversed spans, and unknown enums while
  leaving parser spans absent when no exact offset is available.

- **Sealed Program admission closure.** Added immutable `RegistrySnapshot`,
  `AdmissionProfile`, and `PolicySnapshot` values with builder-time callable
  capture, strict canonical manifests, domain-separated fingerprints, and
  fail-closed cross-fingerprint validation in `ProgramVersion`. Core now
  exposes explicitly named local-only parse/link/validate entry points for
  Program materialization; existing local-first/global-fallback overloads
  remain unchanged.
  Registry entries now record canonical exact executable dependency edges for
  transitive admission closure, and local-only condition checks cover legacy
  keyed-edge documents without consulting process-global registries.

- **Single-root `call_core` Program compiler.** Added `ProgramCompiler`, which
  accepts only the closed Program-v1 envelope, performs pure local Core
  parse/round-trip/validation before sealing, and emits aggregate typed
  diagnostics with RFC 6901 pointers and source-map attribution. Compilation
  derives the canonical Program, registry, transitive executable closure,
  capability/effect, import Merkle, sealed-definition, and Core plan identities
  without invoking factories or callables. The authored document schema,
  complete finite budget contract, zero-dispatch rejection tests, and static
  and shared installed-consumer coverage ship with the compiler. Core gained
  additive total parse/round-trip and local validation reports while legacy
  throwing APIs retain their existing behavior.

- **Pinned Program runtime vertical slice.** Added `ProgramCatalog`,
  `EngineGenerationCache`, `ProgramRuntime`, shared `ProgramHandle`,
  immutable `ProgramResult`, typed Program event envelopes, in-memory
  `ProgramStore`, and an append-only CAS `ProgramJournal`. Admission recomputes
  untrusted bundle semantics before materialization; each attempt pins one
  immutable Core generation and invokes the existing `GraphEngine` async path.
  Runtime execution now maps completion, interruption, exact-checkpoint resume,
  cancellation, timeout, Core step exhaustion, checkpoint incompatibility, and
  failures to typed terminal states while preserving nonrenewable budget and
  checkpoint lineage. Journal commits precede checkpoint/terminal event
  delivery, concurrent resume has one CAS winner, and the PR6 slice rejects
  effectful or nonempty-schema Programs until the Core broker exists.

- **QuickJS control-language frontend.** Added opt-in
  `NEOGRAPH_BUILD_QUICKJS_CONTROL`, sealed
  `ProgramSource::from_javascript(...)`, and a private compile-only QuickJS
  context. The source envelope pins engine/language/host-API versions; its only
  `ng` host surface is a versioned graph builder, and memory, stack, and
  interrupt-poll limits fail closed. JavaScript produces one immutable
  `call_core` Program plan and never becomes a runtime VM, bytecode artifact,
  or Core dependency.

- **A2A Agent Card compatibility candidates.** Added a one-request,
  unauthenticated, no-redirect well-known-card collector and a factory-only
  immutable candidate compiler. Candidates retain only digest-pinned
  provenance, bounded protocol facts, and safe skill IDs; free-form card text,
  advertised RPC endpoints, provider/security configuration, and credentials
  are excluded. The Copy Ninja PoC additionally requires independently observed
  behavior pinned to that digest and never dispatches the source agent.

- **SQLite Harness record store (issue #147 follow-up).** Added the optional
  `neograph::mcp_sqlite` target and `SqliteHarnessRecordStore` for WAL-backed,
  schema-versioned artifact/run persistence with immutable artifact and run-to-
  artifact bindings. The Harness MCP binary now stores records in `runs.db`,
  while checkpoints remain in `checkpoints.db`.
- **AMD OpenMP target-offload proof of concept.** Added the opt-in
  `bench_openmp_offload` benchmark, which compares serial CPU execution,
  OpenMP auto-threading, per-iteration GPU mapping, and persistent GPU data on
  the same numeric fan-out workload. It reports real-device versus host
  fallback execution, correctness, transfer-inclusive latency, kernel-only
  latency, and speedup. `NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201` enables the
  ROCm/Clang device image for Radeon AI PRO R9700.


### Changed

- **C++ ABI and SOVERSION policy (issue #194).** Every compiled public
  `neograph_*` library now carries the project `VERSION` and major
  `SOVERSION`; installed shared libraries resolve sibling dependencies from
  their own directory. Pre-v1 releases use ABI generation 0 but may declare
  mandatory rebuild boundaries. All C++ consumers built against `0.11.1` or
  earlier must rebuild for the next release, because `NodeCache`, `EngineConfig`,
  `CompletionParams`, `Agent`, `RequestOptions`, `SseEventParser`, and provider
  configuration public layouts changed. The release containing
  bounded `UsageAccumulator` reservations is another mandatory rebuild boundary:
  its public object layout now carries reservation accounting state. Version 1.0
  changes the ABI generation to 1 and freezes the supported v1 layouts. CI now
  builds and runs isolated static and shared installed consumers and checks
  ELF/Mach-O loader metadata. See [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md).
- **`GraphNode::run(input)` migration guide complete.** Python `GraphNode` base class
  no longer references deleted `execute*` methods; when `run(input)` is missing it
  raises a `NotImplementedError` containing the migration documentation path.
  C++/Python reference, async/streaming guides, and example READMEs have been
  aligned with the actual v0.9.0 single entry point. Migration procedures are
  documented with C++ and Python examples in
  [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md).
- **Provider API permanent compatibility policy (issue #5).** The planned removal of
  `Provider::complete()`, `complete_async()`, `complete_stream()`,
  `complete_stream_async()`, and callback-based `invoke()` has been rescinded and
  `[[deprecated]]` warnings have been removed. Existing APIs continue to receive
  compatibility and security fixes. New Provider implementations and direct callers
  are recommended to use `CompletionProvider::do_invoke()` and
  `invoke_request(CompletionRequest)` respectively; backporting all new features into
  existing APIs is not guaranteed. Public signatures, virtual ordering, object size,
  and vtable remain unchanged.

### Removed

- **Deprecated in-process local-inference examples.** Removed `example_inproc_gemma`,
  `NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE`, and the CMake variable that pointed to the
  external checkout, which depended on an externally hosted repository that is no longer available. `example_local_transformer`,
  which uses a standard OpenAI-compatible local server, is retained.

### Fixed

- **Python Program wheel closure.** PyPI builds now enable the QuickJS
  control runtime, bundle the `neograph_program` loader beside the extension,
  and export the private evaluator used across the Program/Harness DLL
  boundary on Windows. This prevents `P_JS_UNAVAILABLE`, missing-loader import
  failures, and the shared-build `LNK2019` failure. Command identity assembly
  also avoids nested initializer-list JSON copies across component dylibs,
  which could crash macOS arm64 during Program execution. Windows wheel repair
  hash-mangles external DLL names so a preloaded system `sqlite3.dll` cannot
  override the bundled build and crash `SQLiteContextStore` construction.
- **Bounded remote transports and credential origins.** HTTP/1.1, HTTP/2,
  SSE, and WebSocket receive paths now enforce conservative response, header,
  chunk, line, frame, handshake, and message limits before untrusted sizes are
  allocated. Redirected POST requests are followed only within the normalized
  same origin, provider credentials require TLS unless an explicit numeric
  loopback development exception is enabled, and WebSocket debug output no
  longer contains request headers or payloads.
- **QuickJS `all` join startup race.** Completion handlers now wait for initial
  member-launch registration before closing a JavaScript join. An immediately
  completing child can no longer resume the generator before its sibling initial
  or replacement commands dispatch; repeated runtime regressions cover both paths.
- **Harness aggregate finding provenance (issue #174).** Details now include a
  `finding_sources` array aligned with the existing flat `findings` array.
  Every entry records its aggregate index, source worker ID, and worker-local
  index without changing schema-validated worker output or the established
  `findings` shape.
- **Harness exported result lint (issue #173).** Node effect contracts may now
  declare written channels in an optional `exports` array when callers consume
  them after graph execution. Both Harness compilation and `GraphEngine`
  runtime validation therefore keep E6 for truly write-only channels without
  falsely warning on `final_result`.
- **MCP 2025-11-25 tool-client contract modernization (issue #147 M0).**
  Initialization is now idempotent and retains negotiated server metadata;
  HTTP tools reuse the discovery session; `/mcp` endpoint construction is
  shared by requests and notifications; tool discovery follows opaque cursors;
  and JSON-RPC code/data, full tool metadata, non-text content,
  `structuredContent`, `isError`, and `_meta` survive C++ and Python paths.
  Added configurable HTTP timeout/static/dynamic headers, output-schema
  validation, strict response-ID checking, and typed `InitializeResult`,
  `ToolDefinition`, `ListToolsPage`, and `CallToolResult` APIs. SSE detection now
  uses `Content-Type` rather than misclassifying JSON containing `data:` URLs.
- **Per-task cancel status and published-emit lifetime safety.** `GraphEngine::run`,
  `run_async`, `run_stream`, `run_stream_async` each create one execution child per
  run from the caller-provided parent, bind only that child to the internal
  `co_spawn`/sync bridge, and pass the same child as `RunContext`. Cancelling all
  concurrent runs under a single parent therefore cannot overwrite each other's
  cancellation slots. Forked execution children retain the existing `shared_ptr`
  ownership through the published emit, preventing use-after-free between engine
  work completion and emit execution. asio `operation_aborted` caused by
  cancellation is propagated as `CancelledException` rather than as a retryable
  node error. `CancelToken` 0.11.x object layout and inline/header-only behaviour
  are unchanged. A recompile is required for already-compiled C++ consumers to
  pick up the updated `fork()` lifetime behaviour. Replacing only the shared
  library preserves object layout compatibility, but existing inline function
  bodies embedded in consumer binaries do not change. However, when external
  code calls `bind_executor()` on a token it created directly, the caller still
  bears responsibility for keeping the token alive until the executor's posted
  work completes.
- **PostgreSQL async connection global timeout policy documented.** Async initial
  connection and replacement use a single timeout across all host/IP addresses.
  An explicit `connect_timeout` written directly in a positive connection string
  is enforced with a minimum of 2 seconds; unspecified, zero, negative, or
  environment-variable/service-file-only values use the operationally safe default
  of 30 seconds. This differs intentionally from libpq's per-host synchronous
  timeout; synchronous creation/replacement behaviour is unchanged.
- **JARVIS mock build fix (issue #130).** Fixed `cookbook_jarvis` compilation failure
  caused by `MicCapture` remaining an incomplete type when audio dependencies are
  absent. Added `NEOGRAPH_JARVIS_FORCE_MOCK` so ASan CI always builds the mock
  configuration regardless of the runner's installed packages. The session runner
  now uses the actual CMake output path and specialist target name, and launches
  the existing `demo_mcp_server.py` correctly.
- **Node failure context preservation (issue #123).** C++ execution errors are
  propagated as `NodeExecutionError` containing the original `exception_ptr`,
  failed node name, and attempt count; the terminal `ERROR` event also records
  the same context. In Python the original exception object, type, args, user
  attributes, and traceback are preserved as-is, with only `.node_name` and
  `.attempts` attributes added. `NodeInterrupt`, cancellation, and out-of-memory
  exceptions follow existing control flow without being wrapped.

### Fixed (docs)

- **Removed ignored per-node prompts from Provider cookbook (issue #116).** Fixed
  three Python examples that described multi-role behaviour using `config.system`
  which the built-in `llm_call` does not read. Each example has been rewritten
  as a strict single-call graph using `NodeContext.instructions`, with related
  READMEs aligned with actual behaviour.
- **Reserved `RunContext::deadline` documentation correction (issue #115).** Fixed
  documentation and Doxygen comments that presented `deadline` and `trace_id` as
  usable per-run metadata, when they cannot be set via `RunConfig` and are not
  exposed in Python.
- **`GraphNode::run` example signature fix (issue #129).** Fixed the public header
  example accepting `const NodeInput&` (by reference) which failed to override
  the actual by-value virtual, and locked down the by-value contract required
  for coroutine argument lifetime with a compile-time test.

### Added

- **Backward-compatible Provider migration path.** New `CompletionRequest` separates
  streaming mode from callback presence, and `CompletionProvider` requires new
  implementations to write only `do_invoke()`. The existing `Provider` vtable, four
  legacy virtuals, callback-based `invoke()`, and Python `complete()` subclass
  contract are retained.

- **Python persistence backends** (#117) — `Store` and `CheckpointStore` are
  now constructible subclass bases with C++ virtual dispatch into Python.
  `StoreItem`, `CheckpointPhase`, `Checkpoint`, and `PendingWrite` are exposed
  with JSON-shaped fields; checkpoint pending-write methods remain optional.
- **Python synchronous cancellation** (#119) — Python callers can construct a
  `CancelToken`, assign it to `RunConfig.cancel_token`, and cooperatively stop
  `engine.run()` from another thread.

- **Python checkpoint history** (#118) — `GraphEngine.get_state_history()`
  exposes newest-first checkpoint records so callers can inspect parent links,
  metadata, steps, and IDs before forking from a historical state.

- **DSL surface (elaboration layer) + schema evolution gate** (#75 M4).
  - **Elaborator**: `vars` (`{"$var":...}` / `${...}` interpolation, acyclic
    enforcement) / `templates`+`use` (exact parameter match enforcement, node
    prefix renaming — including local references, barriers, and routes; channels
    are shared state so they merge globally) / `when` conditional inclusion.
    **Non-Turing-complete and total**: every DSL document normalises to a unique
    core in finite time and is idempotent with respect to that core. All errors
    are reported with DSL source coordinates (`use[2].args`, `vars.model`) and a
    source map (output position → generating syntax) is included. Lock-file
    workflow: `./example_elaborate harness.dsl.json > harness.json` (example 53).
  - **`GraphCompiler::upgrade_to_latest()`**: lossless v0→v1 mechanical transform —
    keys that strict rejects are isolated into the `x-upgraded-<key>` comment
    namespace (zero data deletion), empty barriers are explicitly removed. The
    entire corpus is tested to guarantee "legacy permissive compilation IR ==
    post-upgrade strict compilation IR" (canon equivalence, version stamp
    excluded).
  - **Schema evolution gate**: add-only subset judgement against
    `tests/fixtures/schema_snapshot.json` baseline (decidable subset of the JSON
    Subschema family) — removal of node types/properties/reducers/conditions,
    required-set increases, closed-condition label changes, and effect contract
    changes all cause test failure = CI merge block. Incompatible changes force
    version bump + upgrader + snapshot regeneration in the same review commit.

- **PBT / delta verification harness** (#75 M3). 300-seed deterministic topology
  generator (valid strict documents from schema envelope, self-instrumented
  feature coverage — test fails when conditional_edges/barrier/interrupt
  occurrence drops below 30%: untested features become failures, not silent
  holes).
  - **Mutation detection**: confirmed on a 300-seed corpus that translation
    validation catches every application of all 5 drop types
    (conditional_edges/edge/barrier/interrupt/channel) + 3 miswire types
    (route collapse / edge retarget / node rename = drop+fabrication
    counterbalance). Application-rate floor (10% of seeds) also asserted.
  - **Reference interpreter delta**: an independent model re-implementing the
    documented super-step semantics (goto preemption, barrier accumulation,
    lexicographic fallback, implicit __end__) from a code-disjoint
    implementation, compared against the Scheduler on 12-step × 300-graph
    (DESIL lesson: a verifier alone cannot catch wrong-execution).
  - **Engine ↔ editor shared corpus**: `tests/fixtures/topology_corpus/` 15
    variants (3 valid + 12 violating E3–E11) are byte-identical with
    `tests/corpus/` of a companion editor repository, both asserting the same
    verdict (code:severity multiset) — the two implementations cannot silently
    diverge.

- **GraphValidator — topology static semantic checks (E3–E11 + effect)** (#75 M2).
  Pass layer between parsing (M1) and execution. In strict documents
  (schema_version>=1) errors are compilation failures, warnings are stderr
  lints; in permissive documents only error-level diagnostics surface as stderr
  warnings (zero noise on existing graphs). Judgement philosophy = checker
  soundness first: only things that can never be correct under engine semantics
  are errors (dangling reference E3, barrier without signal path E8 — goto
  bypasses barrier accounting so unrecoverable, empty routes E10 — dispatch
  would dereference rend() UB, undeclared channel write E4 — confirmed throw
  at runtime); things that Command.goto/Send can justify are warnings
  (reachability E7, escapeless cycle E11, plain fan-in without barrier E9,
  overwrite race E5, dead channel E6). Every diagnostic is accompanied by a
  machine-readable witness (counterexample) JSON — for editor canvas
  highlighting (M3).
  - **Route completeness (E10)**: `ConditionSpec` label contract introduced.
    Declaring the output label set of a condition via the `register_condition`
    3-argument overload requires closed-condition routes to match labels
    exactly — uncovered labels fall to the scheduler's "lexicographic last
    route" fallback (order-dependent arbitrary target), which is an error.
    Built-in `has_tool_calls` = closed {false,true}, `route_channel` = open +
    known {default}.
  - **Channel effect contracts**: `register_type` 4-argument overload declares
    per-node-type reads/writes channels. E4/E5/E6 analysis activates only when
    **every** node type in the graph is declared (a single unknown type skips
    the entire analysis — soundness over coverage). Built-in 3 types
    (llm_call/tool_dispatch/intent_classifier) fully declared.
  - `node_effects` · `condition_specs` added to `export_schema()` (existing
    `conditions` array retained for backward compatibility). 22 new tests.

- **Topology compile-time consistency gate — consumed-key accounting + translation
  validation** (#75 M1). Dual mechanism structurally blocking the "silent semantic
  loss" class (same species as v0.1.0–v0.1.7 `conditional_edges` silent drops):
  - **Consumed-key accounting**: documents declaring `"schema_version": 1` switch
    to strict compilation — unconsumed keys (typo `conditionnal_edges`,
    unsupported fields, barriers silently dropped by empty `wait_for`, ignored
    `to` on inline conditionals) are all collected and reported as compilation
    errors. Marking occurs **inside** the parse block, so erasing the parse stage
    also erases marks, causing strict documents that use those features to fail
    immediately — a structure where drop regressions cannot be silent. `_`/`x-`
    prefixed keys (`_comment`, `x-studio-*`) are always allowed as comment
    namespace. Existing documents without `schema_version` retain permissive
    behaviour (byte-preserved).
  - **Translation validation**: `CompiledGraph::to_json()` re-emission +
    `GraphCompiler::canon()` normal form check `canon(input) ==
    canon(re-emit)` on every compilation. Mismatches (= the compiler dropped
    something or miswired) throw on strict documents and stderr-warn on
    permissive documents. Equivalence is structural comparison — miswirings
    like swapped route keys are also caught (a class that existence-comparison
    misses).
  - `NodeFactory::config_schema(type)` query added, `schema_version` field
    documented in `export_schema()`. 27 new tests (`tests/test_compiler_strict.cpp`)
    — v0.1.x drop-mutant simulation (conditional_edges/barrier/interrupt drops
    + route miswirings) included.

## [0.11.1] - 2026-06-25

### Changed

- **stdio MCP concurrent calls — correlation-ID demultiplexer for I/O overlap.**
  `0.11.0` concurrent tool dispatch only actually overlapped HTTP MCP. stdio MCP
  held a capacity-1 channel lock for the **entire request→response round-trip**
  in `StdioSession::rpc_call_async`, serialising multiple calls in one turn
  through a single session pipe (wall time ≈ sum of latencies). The single pipe
  was not the root cause — JSON-RPC `id` exists precisely to pipeline over one
  connection. Replaced the lock with a correlation-ID demultiplexer:
  - Repurposed the capacity-1 channel as a **write-only lock** — held only for
    the instant of frame write, so two calls' bytes never interleave while reads
    are no longer serialised.
  - A single reader coroutine (`run_reader`) exclusively owns the read side and
    delivers each response line to the correct caller's sink via JSON-RPC `id`.
    N concurrent calls overlap reads so wall time ≈ max(latency) — but **only
    when the peer MCP server processes concurrently** (a single-threaded
    sequential server hits Amdahl's floor).
  - The reader runs lazily only while in-flight calls exist and exits when
    waiters are empty, so the private `run_sync` io_context returns normally.
    Waiters exist only while their caller is awaiting and keep the session alive
    via `MCPTool`'s `shared_ptr`, so the reader never touches a destroyed
    session (no destructor join needed). On pipe EOF/error the reader closes all
    sinks, so awaiting callers receive an exception instead of hanging
    indefinitely.
  - **No API/syntax change** — public headers unchanged, existing code needs no
    recompilation. Engine overhead regression 0 (`bench_neograph` interleaved
    A/B, seq/par Δ 0%).
  - Tests: thread-based delay fixture `tests/fixtures/mcp_stdio_slow.py` +
    `ConcurrentStdioCallsOverlapIO` (5×100 ms calls complete in ~130 ms vs.
    500 ms serial floor; verifies each response routes to its caller via `id`).
    ASan+UBSan ×3 clean.

## [0.11.0] - 2026-06-25

### Added

- **Concurrent tool dispatch — `Tool::execute_async` official async path.**
  `ToolDispatchNode` executes multiple `tool_call`s from a single assistant turn
  **concurrently** using the engine's `make_parallel_group`. Previously each call
  ran sequentially via synchronous `execute()`, and MCP tools especially blocked
  on spawning their own `io_context` via `run_sync` per call, preventing
  parallel MCP calls from overlapping (discovered in an external C++ fork with
  parallel MCP calls). Fix:
  - Virtual `execute_async()` added to `Tool` — default implementation bridges to
    synchronous `execute()`, so existing tools work unchanged.
  - `MCPTool` converted to `AsyncTool` with native `execute_async` (stdio uses
    `rpc_call_async`, HTTP uses new `MCPClient::initialize_async`/
    `call_tool_async` for async handshake — `run_sync` removed).
  - `ToolDispatchNode::run` dispatches calls concurrently via the same
    `make_parallel_group` idiom as node fan-out (single calls are inlined),
    results applied in call order. Backward-compatible via synchronous
    `execute()` facade.
  - Verification: 478/478 ctest, Valgrind 0 leaks, TSAN 0 races.

### Fixed

- **Python async execution exception preservation (issue #122).** Fixed
  `run_async`, `run_stream_async`, and `resume_async` overwriting the original
  Python node exception with a new `RuntimeError` wrapping it as a string. Now
  the original Python exception object, type, user attributes, and traceback are
  preserved through pybind11's standard exception conversion path, and C++
  `py::type_error` is delivered as Python `TypeError` matching synchronous
  execution. The empty callback in `resume_async` is now retained until the
  coroutine completes, also fixing the dangling-reference conflict exposed in
  pybind11 3.x.

### Fixed (docs)

- **README summary badges corrected for missing conditions and internal
  contradictions revealed by sandbox measurements.** The "The four axes"
  summary-table badges stripped measurement conditions from the body/deep-dive,
  reading as exaggerated. Corrected to align with body measurement figures and
  conditions (measurement data table itself unchanged):
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — the badge's
    17 µs contradicted the body (`At N=10,000 concurrent ... 7 µs p99`) and
    `flat` described GPU-bound load-test run-latency (648 ms), not a µs
    measurement. Badge aligned with body measurement figures and conditions.
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`** — `libc.so.6`-only
    and 1.2 MB only hold for MinSizeRel + static libstdc++ builds (default
    Release dynamically links libstdc++/libgcc_s/libm/libc). Condition already
    documented in deep-dive §size restored to badge.
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** —
    direct dependencies are indeed `certifi` + `pydantic` (two), but the actual
    installation tree is 7 packages including pydantic transitive deps
    (pydantic-core, typing-extensions, annotated-types, typing-inspection).
- **Added `-DNEOGRAPH_BUILD_POSTGRES=OFF` to deep-dive MinSizeRel reproduction
  command.** PostgreSQL defaults to ON, so configure fails on hosts without libpq
  when run as-is. Fixed.

## [0.10.0] — 2026-05-20

### Added

- **Serial fan-out one-shot stderr warning (issue #62, PR #63).** The default of
  `compile()` is `set_worker_count(1)` — fan-out branches execute serially on the
  caller's executor with no engine-owned thread pool. This intended behaviour
  looks like silent serial execution to users who built multi-Send graphs based
  on docs alone. Added a one-shot guidance message to stderr the first time
  `NodeExecutor` dispatches a multi-Send (or multi-outgoing-edge) fan-out
  without a pool. `std::atomic` + compare-exchange guarantees exactly one
  emission even under concurrent fan-out. Calling `set_worker_count(N>=2)`
  rebuilds `NodeExecutor`, naturally resetting the flag. Suppressible via
  environment variable `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` (or `true` / `yes`)
  — for intentional worker=1 serial execution, benchmarks, and CI stderr
  assertion cases. Covered by 5 Linux + macOS unit tests
  (`test_fanout_worker_warning.py`): fire / one-shot / pool opt-in silence /
  env-var silence / single-Send no-warning. Windows: pytest capfd is
  incompatible with MSVC CRT fd caching in wheel binaries so module-level
  skip — wheel binary stderr output itself is normal.

- **Topology JSON Schema export — `NodeFactory::export_schema()`** (issue #56,
  prerequisite for code-free visual block editors). Exports the topology JSON
  format the engine consumes as a machine-readable schema (JSON Schema Draft
  2020-12) in one piece: `{ neograph_version, $schema, topology (fixed
  envelope), node_types, reducers, conditions }`. A separate repo's block
  editor auto-generates its palette from this schema → editor and engine
  cannot drift across versions. Entirely additive:
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-argument
      variant added. Existing 2-argument delegates to permissive default
      schema — existing user nodes/calls unaffected.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` query accessors added.
    - Configuration schema declared for 4 built-in types (`llm_call`/
      `tool_dispatch`/`intent_classifier`/`subgraph`). `NEOGRAPH_VERSION`
      exposed as a compile definition (pyproject.toml single source of truth)
      → schema version stamp.
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — standard path for the editor
      repo CI to produce the artifact pinned to a NeoGraph version.
    - Python: `neograph_engine.export_schema()` → dict (editor repo CI
      dumps after `pip install neograph-engine`).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. Key:
      top-level `conditional_edges` surviving the loader→compile round-trip
      (regression guard against v0.1.0–v0.1.7 silent-drop recurrence).

### Fixed

- **Topology top-level container format validation (#126).** `channels`/`nodes`
  must be objects; rejected in all modes if not. `edges`/`conditional_edges`
  array validation enforced in strict mode, legacy keyed-edge-map compatibility
  retained. Errors record the path and JSON type, not the full input.
- **`max_steps` termination state exposed (#114).**
  `RunResult::max_steps_exhausted()` and the read-only Python property
  `RunResult.max_steps_exhausted` added. True only when `max_steps` is reached
  while nodes remain to execute; same state provided in gRPC single-response
  and streaming final JSON. C++ struct size unchanged.

- **`set_worker_count` / `set_worker_count_auto` docstring correction
  (issue #62, PR #63).** The v1.0 prep cycle intentionally reverted the
  `compile()` worker pool default from `set_worker_count(hardware_concurrency())`
  to `set_worker_count(1)` (see `src/core/graph_engine.cpp:69-93` comments for
  rationale), but four user-facing docstrings retained the old claim → users
  who built multi-Send fan-out graphs trusting docs got silent serial execution
  on a single thread. Not visible with unit tests (fake spawn, instant body);
  only exposed in real wall-time e2e.
  - Rewrote both `set_worker_count` / `set_worker_count_auto` Python docstrings
    in `bindings/python/src/bind_graph.cpp` to match actual behaviour:
    `compile()` default is 1, `set_worker_count_auto()` /
    `set_worker_count(N>=2)` is explicit opt-in.
  - Corrected both Doxygen comments in `include/neograph/graph/engine.h`
    accordingly. Doxygen Pages auto-rebuilds on master push.
  - Corrected the same stale claim (default = hardware_concurrency) in
    `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md`.

- **Three missing API migrations from v0.9.0 ship supplemented.** PR `9b`
  (`19819d8`) in the v1.0 prep cycle destructively removed the `GraphNode`
  legacy 8-virtual chain, but PR `#48` (`6e654ad`, "C++ examples migrate to
  `GraphNode::run()`") only migrated `examples/` — the following 3 files were
  missed, shipping v0.9.0 in a build-broken state:
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3
      sustained-burst verification key benchmark)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (memory/
      concurrency comparison matrix body against LangGraph and other engines)
    - `wasm/smoke.cpp` (Phase 1 WASM feasibility smoke)

  CI did not pick these targets up as add_executables or (Docker build
  dependency) isolated them in a separate environment, so the merge to master
  and tag passed.

  **Fix**: all three migrated from `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` pattern. Node logic unchanged.

  **v1.0 key selling-point native re-verification**
  (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - Concurrency 10K · wall 10–23 ms · p99 17–21 µs · peak RSS **5.6 MB**
      (matches v0.3.0 / v0.5.0 measurements — no memory selling-point
      regression after destructive 9b)
    - 0 errors at 10K
  **Docker matrix (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6-way comparison) also re-measured within the same session**
  (`results_v0.9.0_docker_recheck.jsonl`).

  During matrix re-run, one independent regression discovered alongside the
  missing API migration — `benchmarks/concurrent/Dockerfile.neograph` could
  not build at all because it failed to track CMake option default changes on
  master (same at v0.9.0 ship time). Over time the following option defaults
  flipped OFF → ON:
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (requiring `libpq-dev` / `libsqlite3-dev` respectively)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (one prior incident closed in
      public build guidance — only the option toggle was
      added while the default remained ON, breaking the empty-container build
      path again)
    - `find_package(OpenSSL REQUIRED)` is unconditional without an option
      toggle (CMakeLists.txt:256) — separate v1.0 cleanup candidate

  **Dockerfile fix**: `libssl-dev` apt addition + all non-core options pinned
  with explicit `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF`.
  Comment notes "explicit freeze due to two drift incidents".
  `find_package(OpenSSL REQUIRED)` conditionalisation in CMakeLists.txt left
  as a separate task — impact verification needed for other build paths (PyPI
  wheel, ARM64, etc.).

  **6-way matrix key results** (concurrency=10000, 2 cpus / 1 GiB):

  | engine          | mode          | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
  | **neograph**    | threadpool    | **16**  | **18**      | **5.1** | 10000/0 |
  | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
  | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
  | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
  | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
  | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

  NG vs LangGraph (marketing comparison axis): wall **237× faster**, p99
  **4134× faster**, peak RSS **12× lower**.
  Withdrawn: the p99 compares NeoGraph's service time inside a worker with the time in the system of the Python rows, and the LangGraph multiprocessing memory counts the parent process only; see [`benchmarks/concurrent/CONCURRENT.md`](benchmarks/concurrent/CONCURRENT.md).

  **Harsh scenario** (concurrency=10000, 1 cpu / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

  Same v0.3.0 / v0.5.0 measurements — **no regression of NeoGraph's "10K
  concurrent workers, peak RSS 5 MB, no OOM" selling point after destructive
  9b.**

## [0.9.0] — 2026-05-14 — v1.0 prep (Candidate 1 Phase B + Candidate 6)

Two v1.0 single-dispatch unifications converge in one cycle:

  - **Candidate 1 Phase B (`9b`–`9f`)** — all of `GraphNode`'s legacy 8
    virtuals (`execute` / `execute_async` / `execute_stream` /
    `execute_stream_async` / `execute_full` / `execute_full_async` /
    `execute_full_stream` / `execute_full_stream_async`) +
    `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + all 6 `PyGraphNodeOwner` legacy overrides removed.
    **Destructive** — deprecation window closed. User GraphNode subclasses /
    user Python nodes must migrate to the single method `run(NodeInput)` /
    `def run(self, input)`.
  - **Candidate 6** — `Provider` 4-virtual cross-product → 1-virtual
    `invoke()`. Still in the addition + deprecation phase — legacy 4 virtuals
    unchanged and functional, deprecation warnings only visible. That side's
    Phase B (`Provider` legacy removal) also closes just before v1.0.0 ship.

The same cycle also includes b59444f's potential parallel-regression revert
(`e5ecb08`) + explicit fan-out example calls + 3 CI environment fixes
(httplib macro guard / Windows MSVC unistd.h / pybind pytest migration),
all part of this [Unreleased].

### Added

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 standard single
  dispatch entry point. Handles both non-streaming (`on_chunk == nullptr`) and
  streaming (`on_chunk` provided) in one method. Consolidates the previous
  4-virtual cross-product (`complete` / `complete_async` / `complete_stream` /
  `complete_stream_async`) into one async-streaming superset. Default
  implementation forwards to the 4 legacy virtuals so existing Provider
  subclasses work unchanged. 6 new ctest (`ProviderInvokeDefault`).
  (PR #40)
- **`invoke()` cancel propagation parity** — when `params.cancel_token` is not
  set and an engine thread-local scope is active, `current_cancel_token()` is
  stamped automatically. Equivalent to legacy sync `complete()` behaviour (node
  body inside the engine calling `provider->invoke(params, ...)` automatically
  receives the running graph's cancel signal). 3 new ctest
  (`InvokeCancelPropagation`). (PR #43)
### Changed

- **All internal LLM calls in engine routed through `invoke()`** — `LLMCallNode`,
  `IntentClassifierNode` (PR #41/#42), `Agent::complete` /
  `Agent::run_stream` (PR #43), `SupervisorLLMNode` /
  `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode` (PR #43),
  `PlannerNode` / `ExecutorNode` (PR #44). LLM dispatch within NeoGraph
  unified to a single surface.
- **C++ examples migration (2 files)** — `31_local_transformer.cpp`,
  `cookbook/ai-assembly/member_server.cpp` now use the new `invoke()`. No
  deprecation warning in user builds. (PR #45)
- **`GraphEngine::compile()` default worker count reverted to 1** (`e5ecb08`).
  `b59444f` was the root cause of a latent 18-day (2026-04-26 → 2026-05-13)
  parallel micro-bench regression of 11.8 → 283 µs (24×) — commit pinpointed
  via bisection (11 worktrees in parallel). From v1.0 default=1 (optimal for
  CPU-tiny sequential/parallel dispatch); for intentional fan-out add one line
  `engine->set_worker_count_auto()` to open hardware_concurrency. Explicit
  calls added to 5 affected fan-out examples (10/14/21/36 +
  deep_research_graph builder).

### Deprecated

- **`Provider::complete` / `complete_async` / `complete_stream` /
  `complete_stream_async`** — all 4 legacy virtuals carry
  `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` markers. Legacy
  methods function as-is through the deprecation window. Removed in v1.0.0.
  Internal forwarders wrapped with `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` so
  warnings appear only at user-facing override / call sites. (PR #44)

### Removed (Candidate 1 Phase B — destructive)

- **`GraphNode` legacy 8 virtuals** — `execute(GraphState&)` /
  `execute_full(...)` / 6 variants + `ExecuteDefaultGuard` recursion guard
  + 300+ lines of default chain. All removed. `run(NodeInput)` is the only
  pure virtual. (commit `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` member + `cancel()` hook
  iteration** — `cancel.h` retains only `fork()` + `cancel()` +
  `is_cancelled()` + `slot()`. (commit `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local +
  `GraphState::run_cancel_token_` + 3 accessors** — `RunContext::cancel_token`
  is the sole cancel channel. `src/core/cancel.cpp` emptied down to a stub
  (file itself a future-delete candidate). (commit `9e8e956`)
- **6 `PyGraphNodeOwner` legacy overrides** — pybind trampoline calls only
  `run(self, input)`. Python user code also requires a single method from
  v0.9.0. (commit `9e8e956`)
- **2 obsolete pytest files** — `test_execute_stream_dispatch.py` (v0.3.2
  stream-only fallback dispatch verification) + `test_streaming_only_error_
  hint.py` (execute_full_stream takes priority — meaningless in v1.0).
  (commit `4392fbb`)

### Fixed

- **Explicit calls added to 5 fan-out examples** — restored the real parallel
  intention buried by `e5ecb08`'s default worker count revert:
  `examples/10_send_command.cpp`, `examples/14_plan_executor.cpp`,
  `examples/21_mcp_fanout.cpp`, `examples/36_classifier_fanout.cpp`,
  `src/core/deep_research_graph.cpp`'s `create_deep_research_graph()` builder
  now calls `set_worker_count_auto()`. Verification: `classifier_fanout`
  4.22× speedup (25.2 ms sequential → 6.0 ms parallel). (commit `99c470b`)
- **`bench_async_http` httplib macro guard** — `bench_async_http.cpp` includes
  `<httplib.h>` via `<neograph/async/conn_pool.h>` but `CPPHTTPLIB_OPENSSL_SUPPORT`
  was undefined, causing the ODR guard to reject. Added
  `target_compile_definitions(... PRIVATE ...)` to the CMake target.
  (commit `d4be42a`)
- **Windows MSVC `unistd.h` missing** — `test_schema_provider_extra_
  fields_temperature.cpp` used POSIX-only `mkstemps` + `close`, failing the
  Windows build entirely. Wrapped the entire file in `#ifndef _WIN32` guard
  (coverage guaranteed by Linux/macOS). (commit `3c49f12`)
- **16 Python tests migrated** — wheel CI pytest hit `AttributeError` on 28 node
  classes with legacy `def execute(self, state)` pattern. Batch-migrated to
  `def run(self, input)`; streaming nodes gained `input.stream_cb` None-guard.
  (commit `4392fbb`)

### Migration (user code)

**Provider calls (Candidate 6 — deprecation phase)**

New code:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

The 4 legacy virtual overrides continue to work through the deprecation
window, but `-Wdeprecated-declarations` warnings are visible at user override
sites. Removal occurs just before v1.0.0; migration within the deprecation
window is recommended.

**`GraphNode` subclass (Candidate 1 Phase B — destructive)**

C++ code:
```cpp
// old (up to v0.8.x)
class MyNode : public GraphNode {
    NodeResult execute_full(const GraphState& state) override {
        auto x = state.get("x");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        return out;
    }
};

// v0.9.0+ current code (single method, coroutine entry)
class MyNode : public GraphNode {
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto x = in.state.get("x");
        // in.ctx.cancel_token / in.ctx.step / in.stream_cb also accessible
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        co_return out;
    }
};
```

Python code:
```python
# old (up to v0.8.x)
class MyNode(neograph_engine.GraphNode):
    def execute(self, state):
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]

# v0.9.0+ current code
class MyNode(neograph_engine.GraphNode):
    def run(self, input):
        state = input.state  # input.ctx.cancel_token / input.stream_cb etc. also accessible
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]
```

**Fan-out intent (worker count default change)**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

Migration 1/2/3 sections in `docs/migration-v0.4-to-v1.0.md` (run() /
ctx.cancel_token / worker count default) + Provider section (to be added in
next docs sweep) provide case-by-case before/after guidance.

## [0.8.0] — 2026-05-13 — DX policy + downstream-driven API gaps addressed

Bundles 8 issues (#22, #25, #26, #27, #28, #34, #35 + #16 follow-up)
surfaced by real-world downstream feedback and internal
coverage diff into a single minor bump. Two new public helpers
(`RunResult::channel<T>`, `RunContext::store`), 11 new offline examples,
`docs/migration-v0.4-to-v1.0.md` migration guide, and a 5-item DX bundle
reducing friction newcomers hit in their first 30 minutes.

### Added

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** —
  one-liner helpers to extract channel values from the result. Both output
  shapes (nested `output["channels"][name]["value"]` standard + flat keys added
  by builders like `react_graph`) handled automatically. 9 new ctest. (Issue #25)
- **`RunContext::store`** — node body reaches the Store with one line
  `in.ctx.store->get(ns, key)`. The old pattern (capturing `shared_ptr<Store>`
  in a `NodeFactory` lambda) still works — new code only needs the new shape.
  3 new ctest. (Issue #27)
- **`Provider::complete_stream` non-pure default body** — minimal mock / test
  fixture only needs to override `complete()`. Existing streaming-native
  overrides remain unchanged. 2 new ctest. (Issue #22)
- **`neograph::json` arrays `.front()` / `.back()`** — nlohmann muscle-memory
  pattern (`msgs.back()["content"]`) now compiles. 4 new ctest. (Issue #26)
- **11 new offline examples (41-51)** — `resume_if_exists_chat`,
  `custom_reducer_condition`, `store_personalization`,
  `request_queue_backpressure`, `cancel_token`, `node_cache`,
  `sqlite_checkpoint`, `openinference`, `async_tool`, `minimal`. All rc=0,
  no API key / external service dependencies. Fills gaps among the 27/53
  `NEOGRAPH_API` classes that previously had zero references.
- **`examples/51_minimal.cpp`** — 30-line introductory example with one node,
  no LLM, no tool, no mock provider. Understand how NeoGraph runs in under
  5 minutes.
- **`docs/migration-v0.4-to-v1.0.md`** — case-by-case before/after 4 examples +
  common mistakes migrating from the `[[deprecated]]` old 8-virtual chain
  (`execute` / `execute_async` / etc.) → new `run(NodeInput) ->
  awaitable<NodeOutput>`. Also linked from the `NEOGRAPH_DEPRECATED_VIRTUAL`
  macro message.
- **README "Common pitfalls 5" section** — five things newcomers hit in their
  first 30 minutes (`channel<T>` usage, `in.ctx.store`, `neograph::graph::`
  sub-namespace, `<httplib.h>` macro, GCC 13 coroutine ICE) in one place.
  Each item has a fix + related example/issue link.
- **Compile-time `#error` guard (`include/neograph/api.h`)** — when a user TU
  includes `<httplib.h>` before NeoGraph headers without `CPPHTTPLIB_OPENSSL_SUPPORT`,
  compilation fails with a clear message + opt-out macro
  (`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`). Promotes the old #16 runtime SEGV to a
  compile-time failure.
- **`example_minimal` 5 new friendly error-message ctest** — contract lock on
  `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to
  unknown channel` messages embedding available names + registration method +
  troubleshooting link in the message body.
- **`docs/troubleshooting.md` 4 new entries** — Tracer adapter `close()`
  hang/crash (#24), GCC 13 coroutine ICE (#23), friendly error message
  guidance (#22), `RunResult::output` shape (#25).
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` block** —
  explicitly documents the raw-pointer pitfall for adapter authors. Points to
  the `RecordedSpan` + wrapper separation pattern as the correct approach.
  References existing `tests/test_openinference_cpp.cpp::InMemoryTracer` + new
  `examples/49_openinference.cpp::PrintTracer`. (Issue #24)

### Fixed

- **`SchemaProvider::build_body` silent drop of `extra_fields` when
  `params.tools` is empty.** Old code gated `extra_fields` application inside
  `if (!params.tools.empty())`, causing core schema fields like `reasoning` and
  `response_format` to disappear entirely from tool-less calls. Fix: moved
  outside the tools branch so always applied. 3 new ctest. (Issue #34)
- **`temperature_path` schema-side opt-out.** Reasoning models (gpt-5.x,
  o-series) have mutually exclusive `temperature` and `reasoning.effort`, but
  the schema had no way to declare "this provider does not accept temperature",
  forcing a `params.temperature = -1.0f` sentinel workaround on every call.
  Fix: specifying `"temperature_path": null` in the schema causes build_body
  to skip it entirely. 4 new ctest. (Issue #35)
- **Friendly RuntimeError messages** — `ReducerRegistry::get` /
  `ConditionRegistry::get` / `NodeFactory::create` "Unknown <thing>: foo"
  and `GraphState::write` / `apply_writes` `Write to unknown channel` now
  embed available names + registration method + troubleshooting link in the
  message body. Newcomers can determine next steps from the message alone.
- **`SchemaProvider::complete_stream_async` HTTP/SSE branch** now dispatches on
  a long-lived dedicated `bridge_thread_` (old: `Provider` base default spawned
  a fresh `std::thread` per call). Old behaviour triggered an SEGV in glibc
  `internal_strlen` with cold thread-local resolver / NSS state. WS branch is
  already native co_await so unaffected. Token dispatch on awaiter's executor
  preserved (PR #10 invariant). (Issue #16)
- **`example/09_all_features.cpp`** Store demo — added docstring pointer directing
  to `examples/43_store_personalization.cpp` for node-body read pattern.
  Option 2 — option 3 (in-line live node) to be cleaned up together once
  #27's `RunContext::store` lands. (Issue #28)

### Docs

- Canonical shape of `RunResult::output` (channels-wrapped) and its relationship
  to flat-key projections added by builders like `react_graph` documented in
  header docstring. Use of new helpers (`channel<T>` / `channel_raw` /
  `has_channel`) recommended. (Issue #25)
- `RunContext::store` field `@brief` block — two plumbing patterns
  (`in.ctx.store` recommended / old factory-closure capture compatible) with
  code examples side by side. (Issue #27)
- Both paths documented in `examples/43_store_personalization.cpp` file header
  comment.

## [0.7.0] — 2026-05-11 — C++ openinference + async streaming bridge

Closes the four issues filed against v0.6.0 in one minor bump.
Headline: the `Provider::complete_stream_async` default no longer
segfaults when awaited from inside an outer engine coroutine
(issue #4) — the most common shape for SSE / streaming HTTP backends
sitting in front of NeoGraph. Companion: a C++ peer of the v0.6.0
Python OpenInference layer so Phoenix / Arize / Langfuse render
C++-driven traces the same way they render Python ones (issue #9).
Plus: cosmetic Python OTel detach noise silenced (issue #2) and
the same-`thread_id` concurrent-run + `schema_mutex_` × on_chunk
locking invariants are now pinned in the docstrings (issue #6).

### Added

- C++ peer of `neograph_engine.openinference` (issue #9). New
  `neograph::observability` module covers two pieces:
  - `Tracer` / `Span` — small dep-free abstract interface so NeoGraph
    itself doesn't pull in opentelemetry-cpp. Downstream provides an
    adapter wrapping its own backend (OTel SDK, in-memory test fake,
    logging recorder, etc.). 4 attribute setters (string, int64,
    double, bool — bool deliberately renamed `set_attribute_bool`
    so a `const char*` literal can't accidentally resolve to it),
    plus `add_event` for streamed-token diagnostics, status, and
    `end()`.
  - `openinference_tracer(tracer)` — opens a CHAIN-kind root span,
    returns an `OpenInferenceTracerSession` whose `cb` field plugs
    into `engine.run_stream()` and opens a CHAIN-kind child span per
    node, with `NODE_START`/`END` payloads stuffed into
    `input.value` / `output.value` JSON blobs and `LLM_TOKEN` events
    recorded as discrete span events.
  - `OpenInferenceProvider(inner, tracer)` — wraps any `Provider`,
    attaches the OpenInference LLM-kind attribute set
    (`llm.model_name`, `llm.invocation_parameters`,
    `llm.input_messages.{i}.message.{role,content}`,
    `llm.output_messages.0.message.{role,content}`,
    `llm.token_count.{prompt,completion,total}`) on every
    `complete*` call. The streaming overloads also append
    `llm.token` events and a final assembled `output.value`.
  - 7 parity tests in `tests/test_openinference_cpp.cpp` driving an
    `InMemoryTracer` reference adapter — assert root + per-node CHAIN
    span hierarchy, ERROR / INTERRUPT status surfacing, LLM_TOKEN
    span-event recording, straggler-span cleanup on session close,
    LLM provider attribute set, streaming token events, and
    exception status propagation.

### Fixed

- `Provider::complete_stream_async` default bridge no longer blocks
  the awaiting coroutine's executor for the duration of the stream.
  Pre-fix the default was `co_return complete_stream(...)` inline,
  which (a) suspended the engine's `io_context` worker thread for
  the whole HTTP/SSE recv loop — so other node coroutines on the
  same executor stalled — and (b) for `SchemaProvider`'s WebSocket
  Responses branch, additionally nested a fresh `run_sync` io_context
  on top of the engine worker via `run_sync(complete_stream_ws_responses(...))`,
  racing on shared provider state and producing intermittent segfaults
  when called from inside an outer `GraphEngine::run_stream_async`.
  New default spawns a dedicated worker thread for the synchronous
  `complete_stream`, dispatches each token back onto the awaiter's
  executor (so the user's `on_chunk` runs single-threaded with the
  awaiting coroutine — no reentrancy), and resumes the coroutine via
  a one-shot `steady_timer.cancel()`. Worker-thread exceptions
  re-raise on the awaiter. `SchemaProvider` adds a native
  `complete_stream_async` override that skips even the worker thread
  for the WebSocket path by directly `co_await`ing
  `complete_stream_ws_responses`. `OpenAIProvider` benefits from the
  new base default transparently (no WS path, no special case).
  Two new tests in `tests/test_provider_async_default.cpp`:
  `StreamAsyncBridgeDoesNotBlockExecutor` (a concurrent ticker
  coroutine advances during the stream + chunks deliver on the
  awaiter's thread, not the worker's) and
  `StreamAsyncBridgeRethrowsWorkerException`. (Issue #4)

- `openinference_tracer`: silence the `Failed to detach context`
  stderr traceback that OTel's SDK emitted on every shutdown when
  the tracer was used with `engine.run_stream_async` +
  `StreamMode.ALL`. The OTel contextvars token created at NODE_START
  was being detached from a different `asyncio.Task` (NODE_END
  callback fires from the engine's continuation, not the caller's
  task), so `Context.reset(token)` raised `ValueError`; the SDK
  swallowed the raise but still routed the full traceback through
  `logger.exception`, polluting production logs without affecting
  semantics. Fix records the (thread, task) at attach and skips
  detach on mismatch, plus installs a narrow `logging.Filter` on
  `opentelemetry.context` that drops the message only while our
  `_safe_detach` is on the stack. Sync callers and same-task async
  callers still get proper LLM-span nesting under the node span.
  (Issue #2)

---

## [0.6.0] — 2026-05-07 — OpenInference observability layer

Closes the LangSmith UX gap. NeoGraph already emitted OTel-shape
spans (so traces flowed to any OTel backend); this release adds the
LLM-specific attribute layer that Phoenix / Arize / Langfuse use to
render the trace as a chat-bubble + token-counts UI instead of a
flat generic-application span list. Verified end-to-end against a
local Phoenix container — writer→critic graph produces a 6-span
hierarchy (CHAIN root → node spans → LLM spans) with model name,
prompt/response, and token counts visible in the Phoenix UI.

### Added

- `neograph_engine.openinference` module:
  - `openinference_tracer(tracer)` — context manager that mirrors
    `otel_tracer` but tags root + node spans with
    `openinference.span.kind = "CHAIN"` and stuffs node payload
    into `input.value` / `output.value` JSON blobs.
  - `OpenInferenceProvider(inner, tracer)` — wraps any `Provider`.
    On every `complete()` opens an `llm.complete` child span tagged
    `span.kind = "LLM"`, capturing `llm.model_name`,
    `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`,
    `llm.output_messages.0.message.{role,content}`,
    `llm.token_count.{prompt,completion,total}`, and the Langfuse-
    compatible `input.value` / `output.value` blobs.
- 4 tests in `bindings/python/tests/test_openinference.py` —
  InMemorySpanExporter assertions on attribute presence, span
  hierarchy, exception path, and node-input/output JSON blobs.

### Fixed

- `openinference_tracer` now attaches each node span as the OTel
  *current* context (via `otel_context.attach`) so child LLM spans
  opened inside the node body nest under their node span. Without
  this, contextvar propagation across the C++→Python pybind callback
  boundary produced 3+ unrelated trace_ids per run instead of the
  expected single trace tree. The token is detached on NODE_END /
  ERROR / INTERRUPT to restore the prior current span. Same pattern
  the existing `otel_tracer` documents — explicit attach/detach
  rather than `trace.use_span(...).__enter__()` which is unsafe to
  use without a matching `__exit__`.

### Notes

- OpenTelemetry remains an opt-in dependency. Importing
  `neograph_engine.openinference` raises a clear ImportError on
  first use only if `opentelemetry-api` isn't installed; not at
  import time.
- For a Phoenix end-to-end run::

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

  Configure the OTLP gRPC exporter to `http://localhost:4317` and
  open `http://localhost:6006` to view traces. The module docstring
  has the full snippet.

---

## [0.5.0] — 2026-05-07 — Binding ergonomics: live-mutation list properties

Closes a silent-no-op trap on the most-natural Python idiom for
mutating message / writes / sends lists exposed via the binding.
Previously `params.messages.append(msg)` mutated a copy and the
underlying C++ vector never saw the new item — graceful failure (no
crash, no warning) that produced degraded LLM replies. Now `.append()`
pushes through to the live std::vector.

### Added

- `bindings/python/src/opaque_types.h` — `PYBIND11_MAKE_OPAQUE` for
  five vector types: `std::vector<ChatMessage>`, `<ChatTool>`,
  `<ToolCall>`, `<graph::ChannelWrite>`, `<graph::Send>`.
- `module.cpp` `init_opaque_vectors` — `py::bind_vector` registers
  each as a Python class (`ChatMessageList`, `ChatToolList`,
  `ToolCallList`, `ChannelWriteList`, `SendList`) supporting the
  full mutable-sequence protocol against the live C++ vector.
- `py::implicitly_convertible<py::list, …>` for each — the legacy
  build-then-assign pattern (`params.messages = [m1, m2]`) keeps
  working unchanged; assignment auto-converts a Python list into
  the bound class.
- `bindings/python/examples/23_evolving_chat_agent.py` — per-thread
  evolving chat agent (live LLM): the agent's JSON definition is
  rewritten between turns based on accumulated conversation history.
  Demonstrates checkpoint-resume across evolution (prior messages
  survive), the `__graph_meta__` audit channel pattern, and a
  validator boundary (whitelist node types, required channels).

### Changed

- `params.messages` / `.tools` / `chat_message.tool_calls` /
  `node_result.writes` / `.sends` now return their bound class
  instead of a plain `list`. `len()`, iteration, `__getitem__`,
  `__setitem__`, `.append()`, `.extend()`, slicing — all behave
  like a Python list. Only `isinstance(x, list)` returns False.
  Repo + downstream grep confirms zero such isinstance call sites.
- `.github/workflows/nightly.yml` — drop the `ops/s ≥ 600K` gate.
  After 4 consecutive failures with `err=0` and `leak=false`, the
  threshold (calibrated against local hardware at 969K ops/s) was
  unreachable on shared GitHub-hosted runners (measured 233~273K
  ops/s, 3-4× below local). Throughput regression detection lives
  in the PR-time `bench-regression` job (stable hardware, single-
  shot dispatch in µs). The nightly soak's actual value is
  `err==0` + `leak_suspect==false` over 5 minutes — both kept as
  hard gates.

### Notes

- `ChatMessage.image_urls` (`std::vector<std::string>`) intentionally
  not migrated — `vector<string>` is used too widely across the
  binding for a global OPAQUE without sweeping every callsite.
  Documented as a remaining limitation; v0.6+ candidate.

---

## [0.4.0] — 2026-05-05 — v1.0 prep: unified `run(NodeInput)` dispatch

The opening release of the v1.0 sharpening track.
The 8-virtual `GraphNode` cross-product (`execute` / `execute_async` /
`execute_full` / … / `execute_full_stream_async`) collapses to a
single canonical method: `run(NodeInput) -> awaitable<NodeOutput>`.
Per-run cancellation metadata moves from a non-channel-set `GraphState`
member + a thread-local smuggling channel into an explicit `RunContext`
argument. `deadline` and `trace_id` were added only as reserved extension
slots and are not populated by `RunConfig`. `CancelToken` gains
hierarchical `fork()` so multi-Send fan-out workers each own a
private signal that the parent's `cancel()` cascades to.

### Added

- `RunContext` (`include/neograph/graph/engine.h`) — explicit per-run metadata:
  usable `cancel_token`, `thread_id`, `step`, `stream_mode`, plus reserved
  `deadline` and `trace_id` slots. Engine threads it through every
  `NodeExecutor::run` call. **PR 1, commit `a473f0e`.**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — single
  canonical dispatch entry point. `NodeInput { state, ctx,
  stream_cb }`; `NodeOutput { writes, command, sends }`. Default
  body forwards to the legacy 8 virtuals so existing subclasses
  keep compiling. **PR 2, commit `607ce66`.**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — child token
  with its own `cancellation_signal`. Parent `cancel()` cascades
  to all live children (and to grandchildren recursively).
  `run_sync(aw, parent_token)` switches to `parent_token->fork()`
  so each nested op binds its own slot — closes the v0.3.x emit-
  vs-bind race and the multi-Send single-handler overwrite. The
  v0.3.x `add_cancel_hook` list keeps working through deprecation.
  **PR 3, commit `897645c`.**
- `[[deprecated]]` on the 8 legacy `GraphNode` virtuals + `add_cancel_hook`.
  Internal call sites (graph_node.cpp default chain, default
  `run()` forwarder) bracketed by new
  `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` macros (`api.h` — GCC /
  clang / MSVC portable). User code overriding deprecated virtuals
  sees migration warnings; engine internals stay clean.
  **PR 4, commit `35a4517`.**
- `engine.get_state_view(thread_id) -> StateView` is now the
  canonical state read; raw-dict `engine.get_state(...)` soft-
  deprecated in the docstring (no warning emitted — raw dict
  remains a valid escape hatch). **PR 5, commit `f31aa53`.**
- 7 C++ + 19 Python examples migrated to `run(NodeInput)`. Smoke
  runs match v0.3.2 outputs bit-for-bit. **PR 6a/6b, commits
  `a2a24ef` / `0a76e3a`.**
- Pybind `PyGraphNodeOwner` overrides `run(NodeInput)` and
  dispatches to a Python user's `run` method (when defined),
  falling through to the legacy chain otherwise. `RunContext` /
  `NodeInput` / `CancelToken` exposed to Python; `cancel_token`
  reachable as `input.ctx.cancel_token` without the thread-local
  smuggle. **PR 7, commit `4e186a5`.**
- `docs/reference-en.md` §6 GraphNode collapsed to a single `run()`.
  RunContext + `fork()` example subsections added under §7.
  README "Differences from LangGraph" picked up a "One node method"
  entry. **PR 8, commit `519a00b`.**
- Built-in C++ nodes (`LLMCallNode`, `ToolDispatchNode`,
  `RouteToNode`) migrated to `run(NodeInput)` overrides.
  **PR 9a, commit `d1070dc`.**
- Newcomer-mode trap fixes: README CMake snippet documents
  `graph::` sub-namespace, cppdotenv path, `OpenAIProvider::create()`
  vs `create_shared()`, `neograph::json` as nlohmann subset,
  3-arg vs 2-arg `compile()`. Python `compile(def, ctx, store=None)`
  keyword-arg added (additive, non-breaking). **commit `ee11ed6`.**

### Changed

- README: "10K-worker measured stress test" section — RTX 4070 Ti +
  Gemma 4 E2B Q4 on neoclaw, N=10000 done @ 0 err / 424s / 2572 MB
  peak / ~1 KB marginal worker cost / p99 648 ms (`7840b81`).
- README: "Production economics" section — fleet safety + RAM delta
  framing (`b82b15a`).
- README: "No Docker required" + "Dependency-drift immunity"
  bullets in the LangGraph delta list (`333b482`, `a6061d7`).

### Deprecated

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — kept working
  with `[[deprecated]]` annotation through v0.5.x, removed in v1.0.
- `CancelToken::add_cancel_hook` — replaced by `fork()`. Same
  deprecation window.

### Notes

- Validation: 442 → 452 ctest (3 NodeRunDispatch + 7 CancelTokenFork
  added) + 96 pytest + 5 live LLM/WS green at the v0.4.0 tag.
- A sub-PR (`run(const NodeInput&)` reference param) tripped the
  v0.2.0 RunConfig coroutine-reference UAF crash shape under the
  pybind async path. Fix landed before merge: `NodeInput in` by
  value. Documented in `node.h`.

---

## [0.3.2] — 2026-05-05 — Cancel propagation hardening (5 rounds)

Five-round patch series closing the gaps the v0.3.0 single-shot
cancel uncovered: Send fan-out propagation, in-process polling,
hooks for Python, C++ scope, exception typing. Also lands the
feedback batch from the FastAPI SSE chat-demo
evaluation — `resume_if_exists`, dict-or-list `update_state`,
StateView for typed state reads.

### Added

- `RunConfig::resume_if_exists` — opt-in resume of a prior
  thread's checkpoint without explicit `resume()` call. Standard
  multi-turn chat semantics: `engine.run(cfg)` continues the
  conversation if `thread_id` exists.
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — accepts both shapes. Pre-fix only `dict` worked;
  passing a list silently no-op'd. List form is symmetric with
  every node body's emit shape.
- `StateView` (`bindings/python/neograph_engine/state_view.py`) —
  Pydantic-typed state read. `engine.get_state_view(thread_id) ->
  StateView` returns flat dot-access (`view.messages` /
  `view.foo`) plus `view.raw` for the dict escape hatch.
  Subclass for typed channel definitions:
  `class ChatState(ng.StateView): messages: list[dict] = []`.
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` —
  asserts mid-flight cancel really aborts every Send-spawned
  sibling at the socket layer (was the v0.3.1 root-cause patch).
- `examples/22_self_evolving_graph.py` — moved to v0.3.2 with the
  cookbook fold.
- Doxygen `/* */` wildcard fix — `acp/types.h` had `/**` blocks
  containing path wildcards (`fs/*`, `terminal/*`) that opened a
  nested comment + suppressed all subsequent diagnostics. Replaced
  with `&#42;` HTML entity.

### Fixed

- Cancel propagation, 5 cumulative rounds:
  1. v0.3.0 single-node — `cancel_token` reaches `Provider::complete`.
  2. v0.3.1 multi-Send pointer drop — fan-out workers now share
     `run_cancel_token_shared()` (was lost when `init_state +
     restore` rebuilt per-worker state outside the channel set).
  3. v0.3.1+ in-process polling — engine super-step loop polls
     between steps, not just at LLM I/O.
  4. v0.3.2 hooks for Python — `add_cancel_hook` registers a
     callback on the per-run token, fires on `cancel()`. Lets sync
     Python `execute()` install ad-hoc cancel handlers without
     the thread-local scope.
  5. v0.3.2 C++ scope + retry + exception typing — fresh-throw
     `NodeInterrupt` on the main thread (avoids libstdc++
     `__exception_ptr::_M_release` race), retry budget honours
     cancel, runtime-vs-logic exception split.
- `execute_stream`-only Python nodes silently fell through to the
  default `execute` path (NotImplementedError). Now `run_stream`
  wires `execute_stream` directly when the user only overrode the
  streaming variant.
- `update_state` accepting list[ChannelWrite] — closes the silent
  no-op.

### Notes

- 442 ctest + 96 pytest + 2 live LLM (single + fanout cancel)
  green at v0.3.2 tag (`915e90e`).
- 27/30 C++ examples + 20/22 Python examples pass under
  `examples/run_all.py`. Skipped tests need external services
  (Postgres / Crawl4AI / live OpenAI).
- Valgrind 6 examples 0 errors, 815 allocs / 815 frees clean.
- Bench median 5.185 µs/iter on the seq path (v0.3.0 baseline) —
  zero perf regression across the round.

---

## [0.3.0] — 2026-05-04 — Cooperative cancel propagation

Closes the production cost-leak gap reported during the FastAPI SSE
chat-demo evaluation: a frontend `AbortController` cancelling the
asyncio task no longer leaves the upstream OpenAI request running to
completion. Cancel propagates through every layer of the run.

### Added

- `neograph::graph::CancelToken` (atomic flag + asio
  `cancellation_signal`) and `CancelledException` —
  `include/neograph/graph/cancel.h`. Cooperative cancel primitive.
  Pass via `RunConfig::cancel_token` (optional `shared_ptr`); the
  engine super-step loop polls `is_cancelled()` between steps and
  bails with `CancelledException`. The token's `cancellation_slot()`
  binds to the run's `co_spawn` so an in-flight LLM HTTP socket op is
  aborted on the wire (asio `operation_aborted`).
- `CompletionParams::cancel_token` — explicit pin for users threading
  abort across multiple `provider.complete()` calls. `Provider::complete`
  reads it (or falls back to the thread-local
  `current_cancel_token()` set by `PyGraphNode::execute_full_async`)
  and binds the slot to its inner `run_sync` io_context, so even sync
  Python nodes hit by a cancel stop billing.
- `GraphState::run_cancel_token()` — per-run, non-serialized handle
  used by the pybind `PyGraphNode` to install a
  `CurrentCancelTokenScope` around the synchronous Python `execute()`
  call. This is what gives sync Python users transparent cancel
  propagation without changing their node code.
- pybind `engine.run_async` / `run_stream_async`: asyncio
  `Future.cancel()` now wires through `add_done_callback` to
  `CancelToken::cancel()`, and the `co_spawn` binds the token's
  cancel slot.
- pybind safe-resolve helpers `_safe_set_future_result` /
  `_safe_set_future_exception` — guard `future.set_result` /
  `set_exception` calls posted via `call_soon_threadsafe` against
  cancelled-future `InvalidStateError` storms.
- `bindings/python/tests/test_async_cancel_live_llm.py` — live
  OpenAI E2E asserting OpenAI HTTP completes within < 3 s of
  `Future.cancel()` (in practice immediate; pre-fix was ~7–8 s of
  uncancelled streaming). Skipped unless `NEOGRAPH_LIVE_LLM=1`.
- `examples/22_self_evolving_graph.py` — self-evolving graph PoC:
  `prompted_llm` node reads its own prompts from JSON config so an
  LLM rewriter can mutate the graph definition between runs and
  recompile. Demonstrates `0.0 → 0.4` score improvement; documents
  the channel-flow reasoning gap in the rewriter.

### Changed

- `Provider::complete(params)` now binds an inner cancellation slot
  to its `run_sync` when `params.cancel_token` is set OR when a
  thread-local `current_cancel_token()` is active. Previous default
  behaviour (no cancellation) is preserved for callers that don't
  opt in.
- `neograph::async::run_sync` gained an optional
  `graph::CancelToken*` parameter; when non-null the bound spawn
  binds the token's slot.
- pybind `resolve_future_async` routes through the safe-resolve
  helpers instead of calling `future.set_result` directly via
  `call_soon_threadsafe`.

### Roadmap (deferred to v0.3.x)

- LangGraph-style auto checkpoint resume on same `thread_id`.
- Streaming-only-node hint in `run_async` error message.
- `cb.emit_token(node, data)` ergonomic helper.
- README "Differences from LangGraph" section.
- `update_state` signature alignment with docs.
- `get_state` flat helper / Pydantic accessor.
- Live verification of cancel propagation in `run_parallel_async`
  and `run_sends_async` branch fan-outs.
- pgvector RAG example.

---

## [Unreleased] — Stage 4

Stage 4 closes the last `run_sync` hop on the async path. `run_async`
now stays on the caller's executor end-to-end: three 50 ms agents
on one `io_context` thread drop from ~150 ms (serial) to ~50 ms
(overlapping) in `examples/27_async_concurrent_runs`.

### Breaking

- **`GraphNode::execute_full_async` default flipped to async-first.**
  It now wraps `co_await execute_async(state)` into a `NodeResult`
  instead of calling sync `execute_full(state)`. Any subclass that
  emits `Command`/`Send` only from a sync `execute_full` override
  MUST add a one-line `execute_full_async` bridge:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
  Without the bridge, `Command`/`Send` are silently dropped on the
  async path — the 2.0 latent dispatch bug that 3.0 fixed by routing
  through sync at the cost of an `io_context` spawn per super-step.
  All in-tree subclasses (`deep_research_graph`, examples 10/14/21,
  tests 5 sites) now carry the bridge.

### Performance

- Example 27 wall time: **152 ms → 53 ms** (3 agents × 50 ms timer
  step on one `io_context` thread, full overlap).
- No measurable regression on single-run benchmarks; `run()` still
  drives the same coroutine through a fresh single-threaded
  `io_context` via `run_sync`.

### Tests

- 341/341 ctest green
- 295/295 ASan+UBSan green
- Valgrind clean on coroutine-heavy subset (20 tests, 2.4 s)

### Post-release validation (same day)

- **All 30 examples re-run:** 26/29 PASS, 0 FAIL, 3 environment-gated
  (clay_chatbot → raylib, postgres_react_hitl → docker compose,
  deep_research full loop → crawl4ai service). `21_mcp_fanout`
  measured at 3 MCP calls / 8 ms wall — Stage 4 overlap holds under
  real network I/O.

- **ARM64 compatibility (docker buildx --platform linux/arm64):**
  `Dockerfile.arm64-smoke` at repo root. ubuntu:24.04-arm64 +
  core+llm+async+sqlite+tests build under QEMU emulation completes
  in ~15 min; **306/306 ctest green** on ARM64. Stripped binary sizes
  0.81-0.88 MB (nearly identical to x86_64). example 27 runs in
  65 ms under emulation (native x86_64: 53 ms). Confirms Linux/ARM64
  as a supported target alongside macOS beta (Apple Silicon).

- **Cache locality (Ryzen 5800X / Zen 3, Valgrind cachegrind,
  32 KB L1i/d 8-way, 32 MB L3 16-way):**
  `bench_concurrent_neograph` sweep N=1 → 10,000.

  | N | I refs | LLi misses | LLi miss% | Native p50 |
  |---:|---:|---:|---:|---:|
  | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
  | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
  | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

  Last-level instruction misses stay flat at ~4,320 across 4 orders
  of magnitude of N. Unique hot code working set ≈ 277 KB (0.85% of
  L3). 648 M instructions at N=10,000 incur only 4,329 LL misses —
  roughly 1 miss per 150,000 instructions. Native p50 drops from
  17 µs to 5 µs purely from I-cache warming. First measured evidence
  for the "burst concurrency robustness" positioning.

---

## [3.0.0] — 2026-04-22

3.0 removes the Taskflow dependency and unifies sync and async
super-step execution on a single asio coroutine path. Graph-definition
JSON, node ABI, checkpoint schema, and public entry points (`run`,
`run_async`, `run_stream`, `resume`) are source-compatible with 2.0;
the break is confined to `GraphNode` subclasses that emit
`Command`/`Send` from the **sync** `execute_full` override only.

### Breaking

- **`deps/taskflow/` and the Taskflow INTERFACE target are gone.**
  The sync super-step loop, `run_one`, `run_parallel`, `run_sends`,
  and the process-wide `tf::Executor` static are deleted. Downstream
  consumers that `#include <taskflow/...>` via NeoGraph's include
  path must vendor Taskflow separately.
- **`GraphNode::execute_full_async` default now bridges to the sync
  `execute_full` via direct call (no `co_await execute_async`).**
  This preserves `Command`/`Send` emitted from a sync-only override
  — the common 2.0 pattern — through the async path that all entry
  points now share. Async-native nodes that need non-blocking I/O
  AND `Command`/`Send` must override `execute_full_async` directly;
  the docstring has said this since 2.0, but 2.0 never exercised it
  because sync `run()` bypassed the coroutine path entirely.
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` sync
  methods removed.** Use the `_async` peers.
- **CPU parallel fan-out is opt-in.** Previously Taskflow provided a
  process-wide thread pool by default. In 3.0 `run_parallel_async`
  and the multi-Send branch of `run_sends_async` dispatch branches
  on whichever executor drives the coroutine — the single-threaded
  io_context spun up by sync `run()`, or the caller's own executor
  for `run_async()`. I/O-bound fan-out still overlaps (co_await
  suspension on a single thread); CPU-bound fan-out serializes
  unless the caller uses a multi-threaded executor for `run_async()`
  or opts into an engine-owned pool via `engine->set_worker_count(N)`.

### Added

- `neograph::async::run_sync_pool(awaitable, n_threads)` — N-worker
  sync↔async bridge alongside the existing single-threaded
  `run_sync`. Spins a fresh `asio::thread_pool` for the call so
  inner `make_parallel_group` branches execute on separate workers.
- `GraphEngine::set_worker_count(n)` — opt-in engine-owned
  thread_pool used by `NodeExecutor` for parallel fan-out dispatch.
  Rebuilds the executor; must be called before any concurrent run.

### Changed

- `GraphEngine::execute_graph` (sync) is gone. All entry points
  (`run`, `run_stream`, `resume`) route through
  `execute_graph_async` via `neograph::async::run_sync`, so the
  super-step loop, retry backoff, checkpoint I/O, and parallel
  fan-out now live on one coroutine path end-to-end.
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` switched
  from `tf::Executor` / `tf::Taskflow` to `asio::thread_pool` +
  `asio::post` for the caller-side driver.

### Perf (bench_neograph Release -O3 -DNDEBUG on reference Linux, 10-run median)

- `seq` engine overhead (3-node chain, counter): **~5.0 µs** per call.
- `par` engine overhead (5-worker fan-out + summarizer): **~11.8 µs**
  per call.
- Peak RSS of the whole bench process (warm-up + seq + par iters):
  **4.8 MB**.
- vs LangGraph 1.1.9 on the same workload: **131× faster seq, 199×
  faster par** per iteration; RSS ~12× lighter.
- Correction: the `par` figure compares LangGraph's synchronous `invoke`, which runs a fan-out on a thread pool created for every call, with NeoGraph's inline dispatch (`worker_count=1`); the pool-to-pool ratio of the 2026-04-29 re-measurement (LangGraph 1.1.10 against `set_worker_count_auto()`) was 8.1×. The memory figure compares a native process with Python processes that include the interpreter. Neither number is a current result; the matched ratios and the memory baselines are in [`benchmarks/README.md`](benchmarks/README.md).

Prior drafts of this CHANGELOG listed "~46 µs seq / ~114 µs par"
as a 3.0 regression. Those numbers came from a build tree where
`CMAKE_BUILD_TYPE` was unset, so the bench binary was compiled
without `-O3 -DNDEBUG`. On a proper Release build the async-peer
collapse is a **win** vs 2.0's Taskflow sync path (which the 2.0
README advertised at 20.65 µs seq / 150.7 µs par on the same
host). The chart of this entry has been replaced by one that is generated from the current benchmark record; see
[`benchmarks/README.md`](benchmarks/README.md).

### Migration

- No action needed if your nodes override `execute()` / `execute_async()`
  and don't emit `Command` / `Send`.
- If you override sync `execute_full` to emit `Command` / `Send`:
  no change required — the 3.0 async-path default now calls your
  sync override directly. `Command.goto_node` routing works via
  sync and async entry points alike.
- If you override `execute_async` (async-native I/O) AND want
  `Command` / `Send`: override `execute_full_async` directly and
  assemble `NodeResult` there. Overriding only `execute_async`
  silently drops `Command` / `Send` because the default
  `execute_full_async` now routes through sync `execute_full`, not
  async `execute_async`.
- If you relied on Taskflow's process-wide pool for CPU parallel
  fan-out via `engine->run()`: call `engine->set_worker_count(N)`
  once after compile(), or drive the engine via `run_async()` on
  your own multi-threaded `asio::thread_pool` / io_context.

---

## [2.0.0] — 2026-04-22

First public release with the Stage 3 async API. This is a breaking
release; the changes below affect compilation (C++ standard) and
ABI (abstract base classes gained async peers). Sync call sites are
preserved bit-for-bit, so **application code that doesn't override
`Provider` / `CheckpointStore` / `GraphNode` / `Tool` continues to
work unchanged**.

### Breaking

- **C++20 required.** The public API exposes `asio::awaitable<T>`
  return types that need `std::coroutine` support. Consumers must
  compile with `-std=c++20` (or higher). GCC 13+, Clang 15+ tested;
  see `docs/ASYNC_GUIDE.md` §4.1 for GCC 13 coroutine workarounds.
- **libpqxx dependency dropped.** `neograph::postgres` now links
  libpq directly. Ubuntu 24.04 users no longer hit the
  `pqxx::argument_error::argument_error(..., std::source_location)`
  link error introduced by libpqxx-7.8t64's C++17/C++20 ABI split.
  CMake find now targets `PostgreSQL::PostgreSQL` (CMake-bundled
  FindPostgreSQL). Consumers who installed only `libpqxx-dev`
  must now also install / retain `libpq-dev`.
- **`Provider`, `CheckpointStore`, `GraphNode`, `MCPClient` ABIs
  extended.** Each grew async peer virtual functions
  (`complete_async`, `save_async`, `execute_async`, `rpc_call_async`
  and their variants). Downstream subclasses recompile against the
  2.0 headers; source is unchanged unless the subclass wants to
  provide a native async override (recommended for any implementor
  that does real I/O).
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list`
  / `delete_thread` are no longer pure virtual.** They now have
  default implementations that bridge to the matching `_async`
  peer via `neograph::async::run_sync`. Subclasses that override
  the sync side keep working; subclasses that didn't provide any
  override (which would have been a compile error before) now
  infinitely recurse — contract: override at least one of each
  sync/async pair.

### Added

- **Async API** across all I/O layers
  (`docs/ASYNC_GUIDE.md` for full reference):
  - `Provider::complete_async` on the base class and all built-in
    providers (OpenAI, Schema, RateLimited).
  - `MCPClient::rpc_call_async` for both HTTP and stdio
    transports. stdio uses `asio::posix::stream_descriptor`.
  - `CheckpointStore::*_async` for all eight sync methods.
  - `GraphNode::execute_async` + stream / full / full_stream
    variants, with async-native crossover defaults.
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async`
    driving `execute_graph_async` — an end-to-end coroutine super-
    step loop including parallel fan-out via
    `asio::experimental::make_parallel_group`.
  - `neograph::AsyncTool` adapter for user tools that want a
    coroutine body while preserving the sync `Tool` interface.
- **`neograph::async` namespace** — HTTP client, connection pool,
  SSE parser, run_sync bridge, URL endpoint splitter. See
  `include/neograph/async/*.h`.
- **New examples**:
  - `examples/27_async_concurrent_runs.cpp` — multiple agents on
    one `io_context`.
  - `examples/05_parallel_fanout.cpp` (rewritten) — async fan-out
    within a single graph run using `run_parallel_async`.
- **CI bench regression gate** (`.github/workflows/ci.yml`) —
  PR checks enforce floors on `bench_async_http` / `bench_async_fanout`
  / `bench_neograph`.

### Performance

Measured on the feat/async-api branch against Stage 2 sync baselines:

- `bench_async_http --mode async_pool --concur 1000`:
  6064 ops/s → **17834 ops/s** (2.9×).
- `bench_async_fanout --concur 50000`:
  thread-per-agent unachievable → **541K ops/s / 67 MB RSS**.
- `examples/27_async_concurrent_runs` (3 × 50ms async work):
  150ms (sync) → **50ms** (1 io_context thread).
- `examples/05_parallel_fanout` (3 × 100-150ms async work):
  370ms (sequential) → **150ms** (1 io_context thread).
- `bench_neograph` engine overhead: unchanged (~30 µs seq /
  ~205 µs par). Coroutine machinery does not regress the hot path.

### Not yet in 2.0.0

- **Taskflow dependency** remains. The sync `engine.run()` path
  still uses it for fan-out; Sem 4.5 revisits whether sync paths
  can be replaced by `run_sync(*_async)` so the dependency can
  drop entirely.

### Cross-platform

Three platforms are supported in 2.0.0 at different stability tiers.
The tier reflects how much real-world validation the platform has
seen before release — not feature coverage (the codebase is single-
sourced with `#ifdef _WIN32` splits; features are equivalent across
platforms once tests pass).

#### Linux — **GA** (production-ready)

* Ubuntu 24.04, GCC 13.
* Full 332/332 ctest green locally (Postgres via docker
  `postgres:16-alpine`) plus all benches inside committed CI floors.
* MCP stdio on fork/pipe/execvp + `asio::posix::stream_descriptor`.
* Postgres async peers on libpq nonblocking + `asio::posix::stream_
  descriptor` wrapping `PQsocket`.
* Reference platform for every performance number quoted above.

#### macOS — **beta**

* macos-latest (Apple Silicon), Clang via Xcode.
* CI builds + runs non-Postgres tests; Postgres integration cases
  self-skip without a service container. POSIX paths (same fork/
  pipe + asio::posix code) are exercised.
* `CoreFoundation` + `Security` frameworks linked through httplib
  for system cert loading on TLS.
* Treat as beta until 2-4 weeks of CI runs and user reports
  confirm no runtime-behaviour differences (coroutine scheduling,
  SIGPIPE / EPIPE shape, pipe buffer sizing). Targeted promotion
  to GA once those roll in without incident.

#### Windows — **alpha**

* windows-latest, MSVC 19.44 (VS 2022), x64.
* CI scope: **core + async + MCP + LLM only**. Postgres and
  SQLite backends are disabled on the Windows CI job because
  vcpkg would compile OpenSSL / libpq / zlib / lz4 from source
  on every run (~20 min, no working binary cache backend upstream
  since `x-gha` was removed). Windows users compile these
  locally via their own vcpkg / choco setup.
* OpenSSL via the runner's preinstalled choco package
  (`C:/Program Files/OpenSSL-Win64/`). TLS paths in httplib +
  asio::ssl compile and link.
* MCP stdio: `CreateProcess` + named-pipe (FILE_FLAG_OVERLAPPED) +
  `asio::windows::stream_handle`. The overlapped-pipe path was
  written against MSDN spec without local Windows validation;
  expect first-users to surface edge cases (ERROR_IO_PENDING
  handling, pipe buffer boundary on large JSON responses).
* Postgres async peers (when enabled locally): `asio::ip::tcp::
  socket::assign` wrapping the SOCKET returned by `PQsocket`
  (cast through `native_handle_type` to preserve 64-bit SOCKET
  values). Not exercised by Windows CI — local only.
* Coroutine machinery lives in MSVC's `<coroutine>`; behaviour
  expected to match GCC/Clang by spec but `examples/27` cross-run
  overlap measurements haven't been confirmed on Windows yet.
* Treat as **alpha** through 2.0.0. Promote to beta once one
  production user runs a multi-agent workload for a week without
  hitting stdio/pipe or coroutine-scheduler issues, AND Postgres
  async peers get locally validated by a user willing to run
  vcpkg's full libpq build.

> **Pattern**: CI green is a floor, not a ceiling. Layer 3 runtime
> behaviour differences (coroutine scheduling timing, pipe buffer
> boundaries, socket takeover semantics) only surface under real
> workloads. The tier language above gives users the right
> expectation for each platform rather than pretending all three
> are interchangeable on day one.

### Fixed post-bump

- **`async::HttpResponse` headers map** — the response surface now
  exposes a `headers` vector of `(name, value)` pairs preserving wire
  order and original casing, plus `get_header(name)` as a
  case-insensitive accessor. Retry-After and Location remain as
  dedicated fields for backward compatibility. Unblocks the MCP
  session tracking fix below.
- **MCP `Mcp-Session-Id` header tracking** — the Sem 2.6
  httplib→async_post migration silently dropped this. Every post-
  initialize RPC now echoes the server-assigned session id back
  via the new headers accessor, so the server's session state
  stays routable.
- **MCP stdio awaitable mutex** — `StdioSession::rpc_call_async`
  used `std::mutex`, which deadlocked when two coroutines on the
  same single-threaded io_context called the same session (the
  second's `lock_guard` blocked the worker the first needed).
  Replaced with an `asio::experimental::channel<void(error_code)>`
  capacity-1 semaphore so the second acquirer suspends
  cooperatively.
- **`PostgresCheckpointStore` async peers** — all eight
  CheckpointStore async methods (`save_async`, `load_latest_async`,
  `load_by_id_async`, `list_async`, `delete_thread_async`,
  `put_writes_async`, `get_writes_async`, `clear_writes_async`)
  are now true-async. Internals: `PQsetnonblocking(1)` +
  `PQsendQueryParams` + `asio::posix::stream_descriptor` on
  `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)`.
  Four concurrent `save_async` calls on a pool of 4 slots now
  commit-fsync in parallel at the wire level rather than
  serialising through `run_sync`.

---

## [0.1.0] — pre-2026-04

Pre-release development. No public API stability guarantees.
