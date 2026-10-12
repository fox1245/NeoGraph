# Changelog

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

Dated entries below preserve historical APIs and measurements. Their old Provider compatibility guarantees are superseded by the breaking C++ typed contract in [Unreleased]; they are not current API guidance.

All notable changes to NeoGraph are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Changed

- **Rebuild C++ consumers: tool, Hook, MCP, gRPC and graph types gained members.** `RunResources` gained `hook_runtime`, `InvocationResult` gained `failure`, `MCPTool` gained the base `ContextualAsyncTool` and `execute_result_async(arguments, context)`, `MCPClient` gained a bounded `call_tool_result_async`, `MandatoryHookRunner` gained `blocks` and a private member function, `GrpcRemoteTool` gained the base `ContextualAsyncTool` and `set_call_timeout`, and `GrpcCheckpointStore` gained `set_rpc_timeout`; `graph::FailureVerdict`, `graph::classify_failure`, `ToolExecutionError`, `grpc::make_tool_service` and `grpc::make_checkpoint_service` are new. `proto/neograph.proto` gained `ToolCallResponse.error_code` at a new field number, so the wire stays compatible. Rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)).
- **Tool Hooks run on every tool path.** An engine's `HookRuntime` (`EngineConfig::hook_runtime`, `GraphEngine::set_hook_runtime`) used to publish checkpoint Hooks only; it now also runs the `BeforeToolExecution` and `AfterToolExecution` Hooks of every tool call its nodes make (`tool_dispatch`, the plan-and-execute executor, the deep-research researcher, host nodes using `make_tool_execution_context`), exactly as `llm::Agent::set_hook_runtime` does for the Agent. `RunResources::hook_runtime` replaces it for one run, a subgraph's tool calls use their parent run's, and a Program Core's tool calls use the Program host's `RuntimeConfig::hook_runtime`. **Behavior change:** a host whose Hook registry admits tool-phase Hooks now sees graph and Program tool calls, and a mandatory one can block them; a Hook boundary that cannot run (a run without an owner scope) fails closed with `HookBoundaryBlocked` state `failed`, for checkpoint, provider and Program events too, instead of an `std::invalid_argument`.
- **A Hook block leaves a graph run as `HookBoundaryBlocked`.** A graph run used to throw a `NodeExecutionError` whose cause was the `HookBoundaryBlocked`, while `llm::Agent` threw it directly; now both throw the `HookBoundaryBlocked` itself, with its invocation id and state, wherever it sat in the cause chain, and the node emits no `ERROR` event for it. A cancellation found inside a failure's cause chain leaves as `CancelledException` the same way. Python already saw `ng.HookBoundaryBlocked`; it no longer carries the `node_name` and `attempts` attributes.
- **A Hook reports `timed_out` only when it timed out.** A mandatory Hook whose last attempt failed used to be recorded and reported as `timed_out`; it is now `failed`, in `InMemoryHookJournal`, `SQLiteHookJournal` and `HookBoundaryBlocked::state()`, and an exhausted failure is no longer listed by `pending()`. A Hook whose deadline elapsed, or whose native target timed out (as an RPC Hook's already did), reports `timed_out`, including when its lease ran past the deadline.
- **No tool call starts after the run's deadline.** On every dispatch path a call that would start at or after `RunContext::deadline` (`ToolExecutionContext::deadline`) is a tool result with status `expired` and error `run deadline elapsed before the tool started`, without its Hooks. Before, it ran when no Hook was configured, and stopped the run with a `timed_out` `HookBoundaryBlocked` when one was. The deadline also bounds the admission queue wait.
- **MCP and gRPC servers keep failure classes apart, and send no exception text.** `MCPServer` answers a Hook block with JSON-RPC error `-32010` and an interrupt with `-32011`, and puts the stable code of every other failure in the `isError` result's `_meta["io.neograph/failure"]`. A call the client cancelled gets no response on stdio (`run`), as MCP asks, and `-32800` through a response sink such as HTTP; a call `stop()` cancelled gets `-32000`. gRPC follows the owner's decision for canonical status codes: a failed `RunGraph` or `RunGraphStream` ends with the class's status (`UNKNOWN`, `DEADLINE_EXCEEDED`, `UNAVAILABLE`, `RESOURCE_EXHAUSTED`, `FAILED_PRECONDITION`, `INTERNAL`, `ABORTED`, `CANCELLED`) and a `google.rpc.Status` whose `ErrorInfo.reason` is the stable code; an invalid request is `INVALID_ARGUMENT` (or `FAILED_PRECONDITION` without a graph) before anything runs; `ToolService` keeps a tool's own failure in the payload with `error_code`, and answers a run control signal with a status, an unknown tool with `NOT_FOUND` and malformed arguments with `INVALID_ARGUMENT`. Every message is the public message (`NodeFailureError::public_message` or a description of the class), never `what()`. **Behavior change:** a failed graph run over gRPC used to be status `OK` with `error` set, and a failed stream ended `OK`; `RunGraphResponse.error` is deprecated and kept for one release. An `MCPServer` tool error used to carry the exception text, and a cancelled or stopped call an `isError` result. See [Protocol failure mapping](docs/reference-en.md#protocol-failure-mapping).
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
- **Rebuild C++ consumers: Program provider-call structs gained members, and `monetary_microunits` is enforced for priced calls (#372).** `ProgramResultData` gained `monetary_records`, `ProgramCoreProviderCallContext` gained `monetary_account`, `SQLiteProgramProviderCallJournal::Record` gained `monetary_record`, and `Provider` gained the static `invocation_upper_bound`; rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). The provider journal adds a `monetary_record` column to existing databases. A stored `ProgramResult` carries `monetary_records` only when a call was priced, so other results keep their bytes and ids. **Behavior change:** like every `RunBudget` amount, `monetary_microunits = 0` grants nothing, so a run with a zero monetary budget refuses every call a pricing broker prices, with `P_MONEY_BUDGET`; before, nothing charged money.
- **Rebuild C++ consumers: graph and Program structs gained members (#357, #358, #361, #365, #367).** `EngineConfig` gained `validation` and `stderr_notices`, `RunMetadata` and `RunContext` gained `shared_scheduler`, `ProgramCompilerConfig` gained `require_acyclic`, `ProgramCompilerConfig` and `ValidationOptions` gained `soundness_check` and `soundness_state_cap`, `GraphEngine` gained a private member and `RunStatus` gained `BarrierStalled`. For #364, `EngineConfig` gained `failure_text_redactor`, `RunMetadata` gained `node_retry_ceiling`, `RunBudget` gained `max_node_retries`, `ProgramUsage` gained `node_retries`, `CatalogConfig` gained `failure_text_redactor`, `Scheduler`, `NodeExecutor` and `GraphEngine` gained members, `RunStatus` gained `FailureExit`, and `ProgramDiagnosticError`, `ProgramCompileError` and `ProgramAdmissionError` gained the base `graph::ConfigurationFailure`. These change layouts: rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). A `switch` over `RunStatus` that lists every value needs `BarrierStalled` and `FailureExit` cases.
- **Rebuild C++ consumers: attempt timeouts changed graph layouts, and node bodies get an attempt token (#364, #367, #368).** `NodeRetryPolicySpec` gained `attempt_timeout_ms`, `NodeExecutor` gained members, and `GraphEngine` gained members and a declared destructor; rebuild NeoGraph, C++ consumers and Python extensions together (see [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)). **Behavior change:** in a Program run, and for every attempt that has a timeout, a node body runs on a worker pool instead of the coroutine that drives the run, and its `ctx.cancel_token` is a child of the run's token that belongs to that attempt: cancelling the run still cancels it, but cancelling it cancels only the attempt. In a Program run a parallel step's node bodies therefore overlap, so its `NODE_END` events arrive in completion order. Definitions that do not use `attempt_timeout_ms` keep their bytes and ids.
- **Program: a failed node reports its classified code (#364).** A node failure without a failure edge ends a Program run `Failed` with the node failure classifier's code (`NG.TaskFailed`, an author code such as `app.compile_error`, `NG.ProviderFailed`, ...) instead of `P_RUNTIME_CORE_FAILURE`, and `failure.witness.node_failure` holds `{node, code, kind, attempts, retryable, routable}`. `P_RUNTIME_CORE_FAILURE` remains for Core failures without a node failure record. The `retry` operation of an orchestration plan asks the same classifier: it no longer runs its body again after a logic, configuration, budget or provider failure, a committed effect, or a reached retry ceiling.
- **Node retries and tool failures follow one failure classifier (#364).** The node retry loop and the tool failure contract ask one classifier, which walks the whole cause chain (`NodeExecutionError` causes, `std::nested_exception`, subgraph failures) and takes the most severe class it finds. Still retried: `std::runtime_error` and its subclasses, asio errors other than cancellation, and subgraph failures whose root is one of those. No longer retried: a Program diagnostic raised inside a node (`ProgramDiagnosticError`, `ProgramCompileError`, `ProgramAdmissionError`, in C++ or Python), which is now `NG.ConfigError` through the `graph::ConfigurationFailure` marker; running the node again reproduces it. No longer retried: a subgraph failure whose root is a `std::logic_error` or an unknown exception, any exception outside the `std::runtime_error` and `std::logic_error` families (`throw 42` included), and a quota failure behind `std::nested_exception`. A run control signal anywhere in a tool's cause chain, such as a Hook block inside the failure of a graph the tool ran, now stops the run instead of becoming a tool message. See the [node failure contract](docs/reference-en.md#node-failure-contract).
- **Every node retry attempt has its own effect namespace (#364).** Attempt 0 keeps the task id (`s3:act`), so existing journals keep their slots; attempt k uses `s3:act#a<k>`. A retry therefore asks a `ProviderCallBroker` for a fresh call slot instead of the first attempt's, and a subgraph that a retried node runs starts in a fresh child thread. Before attempt k starts, an empty pending write keyed by its namespace records the start, so a resume continues that attempt and keeps the remaining retry budget instead of starting again at attempt 0. Under a `ToolEffectBroker`, an attempt that committed a non-idempotent effect is never retried and fails as `NG.EffectCommitted`, and an effect whose outcome is unknown pauses the run for reconciliation even when the node turned that interrupt into an error.
- **Example 11 (Clay chatbot) is redesigned.** Text uses a system TrueType/OpenType font with UTF-8-aware measurement, so Hangul and other scripts render (`NEOGRAPH_CHAT_FONT`, `NEOGRAPH_CHAT_BOLD_FONT`, `NEOGRAPH_CHAT_MONO_FONT` override the choice). The window has a header with model and status, avatars, light Markdown with code blocks, a welcome card with suggestions, a scrollbar that follows new output, and a composer whose Send button becomes Stop while a reply streams; Stop and quitting cancel the reply.
- **Adopt SchemaProvider `f7ed8161fbcf39a6bf9358f78cfdcfda133537a2`, the current `maint/interface6` tip: OpenRouter `provider.max_price` (#372).** `sp::OpenRouterRouting` gained `max_price` (`sp::OpenRouterMaxPrice`), OpenRouter's cap on the endpoint price it may route to: optional integer nanoUSD amounts, `prompt` and `completion` per million tokens, `image`, `audio` and `request`, sent as exact decimal USD (`75'000'000` is `0.075`). OpenRouter then routes only to endpoints listed at or below the cap and fails the request when none is. A cap without amounts or with an amount above 2^53 is rejected before I/O. `ProviderControls::provider` carries it on Chat Completions, Responses and Messages requests, and `SchemaProvider::Defaults::provider` merges it like its other routing fields: a request's own `max_price` replaces the default one. Python exposes `ng.OpenRouterMaxPrice` (`prompt_nano_usd`, `completion_nano_usd`, `image_nano_usd`, `audio_nano_usd`, `request_nano_usd`) and `OpenRouterRouting.max_price`, which returns a detached copy: edit it, then assign the whole object back. The cap joins the native configuration binding, so changing it between turns makes captured native history `ReplayIneligible`; routing without one keeps its earlier bytes and digest. The library advertises the field with the capability bit `sp::capability::OpenRouterRoutingMaxPrice`, part of `RequiredProvider`, so NeoGraph's handshakes refuse an earlier interface-6 library that would drop the cap. SDK 0.3.0 and interface/shared ABI 6 are unchanged, but the request layouts grew: see the #372 rebuild entry. The same SDK revision also makes its HTTP transport refuse TLS versions older than 1.2 on every platform, whatever the libcurl build or TLS backend would accept by default.
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

- **A2A: `ListTasks`, `SubscribeToTask` and non-blocking sends on the native server.** `ListTasks` filters by `contextId`, `status` and `statusTimestampAfter` and pages newest first; `SubscribeToTask` (0.3 `tasks/resubscribe`) streams the current task and its live events; `returnImmediately` (0.3 `blocking: false`) returns the submitted task at once. Every run executes on its own worker, so a task outlives a dropped connection, and every status carries a timestamp. Push notifications, the extended card and the HTTP+JSON/gRPC bindings stay unclaimed: their methods answer `-32003` / `-32004`.
- **ACP: client-supplied stdio MCP servers** start through NeoGraph's MCP client, and nodes reach their tools with `ACPClient::session_tools()`. A server gets a minimal environment, the agent's `PATH` (on Windows also `SystemRoot`) plus the client's `env`, never the rest of the agent's environment, which may hold provider API keys.
- **A2A: the card declares the collaboration envelope as an extension.** A Program agent with a `CollaborationMailbox` lists `a2a::COLLABORATION_EXTENSION_URI` in `capabilities.extensions` (`AgentCard::extensions`), so the sender-named, idempotent task ids of collaboration messages are a declared extension (A2A spec 4.6).
- **ACP: incremental updates.** Model tokens stream as `agent_message_chunk`, and tool calls are reported as `tool_call`/`tool_call_update`.
- **Official-SDK interop suites and a protocol-interop CI job.** `bindings/python/tests/interop/a2a_suite` drives the native A2A server and client with a2a-sdk 1.2.2 and @a2a-js/sdk 1.3.0 and 0.3.14, validating every message against the A2A v1.0.1 proto and JSON Schema and the v0.3.0 JSON Schema; `acp_suite` drives the native ACP agent (`neograph_acp_probe_agent`) with agent-client-protocol 0.12.1 and 1.0.0rc2 and @agentclientprotocol/sdk 1.8.0, validating every message against the ACP schema v1.25.0. The suites pin their SDKs, skip cleanly when the pinned SDK is not installed, and run in `.github/workflows/protocol-interop.yml`, which installs the pins and Node.js 22.
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
- **Program: an ordinary question no longer outranks an uncertain effect in a paused join (#401).** A join (collecting or not) or a plan-level `parallel` with no failure paused on its first paused member in declaration order, so an ordinary question was asked before a member paused on an uncertain effect. It now asks for reconciliation first, as a Core step does (#392). The collecting `ng.all` exception, which waits for every member and so pauses on a paused member beside a failure, is kept, documented and pinned by tests.
- **Program: a plan `parallel` or `race` whose branches finish at once no longer hangs.** A plan-level `parallel` or `race` waited for its branches on the scheduler thread that ran it, while the branches signalled their completion on a strand through an asio channel, which is not thread-safe. When the last branch signalled while the wait was starting on another scheduler thread, the signal could be lost and the run never finished; it hung on Windows with branches that return at once. The wait now runs on the strand the branches signal on, as the other joins already did, and a JavaScript join that settles while it registers its members sends its signal from that strand too.
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

  **Completed paid observations; not universal qualification.** Original `SPQUAL1` base630/1000000 microUSD is unchanged; ONE hash-chained `A` admits approved extension480/3000000 in the same original ledger, aggregate1110/4000000, with cumulative calls/spent/holds/settlements and no new grant ID/header/reset. Exact declaration bytes/file identity and original authorization/baseline/catalog/activation/ledger-prefix hashes/totals remain pinned; removal/replacement/change fails closed. The final canonical ledger is calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000; spent+held is US$1.725786 LOCAL catalogue meter, not an invoice. The documented five-family60-pair baseline completed600 requests: Chat60/60, Responses60/60, Messages60/60, Generate56/60 (four incorrect-vision SSE), Interactions57/60 (one buffered and two SSE incorrect-vision); aggregate293/300 pairs, not300/300. Other old600 financial records remain preserved, not full behavioral proof. Earlier M5/media one-shot cohorts are unchanged. The earlier three-round Google prerequisites retain two invalid-tool and one unreadable-positive failures. No further paid calls are authorized. Final SDK evidence and native-axis limits are separate from baseline success. Earlier activation/reopen smoke remains recorded at calls610/spent219159/held751233 after two reopens, with SDK meter/canary/vision four tests passed19.38seconds; these are scoped prior checkpoints, not final ledger totals. The earlier verified Chat60-pair cohort retains120 actual attempts,120 UpperBound charges and no UnknownHold.

  **Native-axis observations, not cryptographic verification or native consumption/equivalence.** Generate accepted mutation, omission and duplication. Interactions accepted the isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission and duplication. Removing all thoughts/signatures returned generic400; removing all signature fields while keeping THOUGHT items also returned generic400. The last capture had a local encoded-original retention control, not a same-capture server positive; the earlier positive cohort remains genuine. These observations establish an aggregate-carrier-absence boundary only, not issuer/signature validation or vendor consumption. Actual reports: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`; prerequisite-failed/not-run/negative-inconclusive states remain factual. Thought-only/carrier-only omissions were accepted while another carrier remained; this does not strengthen issuer-validation or native-consumption claims.

  **Actual integrated proof and remaining limits.** Latest Core full run:2242 tests, zero failures,16 skips (14 RAM process-loss cases not applicable; two live-credential gates),130.17seconds. `PgNestedJsonRoundTrips` preserved exact duplicate keys/order/null metadata, blob and residual in0.18seconds. The unchanged original shared-bank fork and existing Recorded CPU/Memory await/handoff scenarios passed. Real wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probes passed plain and ASan+UBSan. LOCAL Memory/SQLite/PostgreSQL TSan scopes:seven passed,zero warnings. Full mixed gRPC plus system Abseil/Protobuf TSan exited66 with402 race warnings in dependency/generated-RPC stacks: an instrumentation/coverage limit, not a proven false positive; remote TSan/race-freedom is NOT claimed and no warning is suppressed. Installed find_package Program C++/C ABI/dualQuickJS three consumers passed. Fresh installed NeoGraph/SchemaProvider typed consumer passed two real HTTP requests, provider destruction before coroutine start, native/tool replay, refusal,known-zero/raw retention and actual LinkedMismatch rejection. Browser Alice/Bob isolation and generation2 replacement were visually verified; PostgreSQL Program Chat six black-box tests passed18.989seconds. Latest SDK26/26 passed,zero failures,74.07seconds. Final ReleaseGraph16 configurations ×3 fresh process repetitions/48 records completed38.29seconds,zero failures,all actual protocol/owned-outcome checks passed. NeoGraph `benchmarks/provider-cutover-final-results.json` and `benchmarks/provider-cutover-final-summary.json` retain this separate final cohort. No compiler or paid model ran during measurement; historical cohorts stay unchanged and semantic/resource equivalence is not claimed. Unstable SDK/ABI3 is not a stable release or broader-platform qualification.

  The completed chat pairs do not establish native-continuation consumption by a downstream vendor.
- **Standalone bank journal correction — current contract revised; exercised runtime evidence below.** The owner-approved protocol requires a monotonic trusted-store namespace obligation and a real immutable original owner/thread/graph scope, ceiling, deadline/clock identity and generation. Only exact durable head CAS over the full checkpoint commitment and revision may issue a host-owned opaque lease. Exact pending effect windows must persist before provider I/O; settlement must use genuine SDK outcomes and actual charges, nullable reports, holds and dedup identities. Checkpoint and next head must publish atomically under the same owned actor/revision. Removing bank metadata, pruning a checkpoint, replaying an old authenticated snapshot, overwriting the same ID or losing the actor must not grant credit. Tightening a 130 ceiling to 129 with an existing 65 hold cannot admit another 65; a proven no-effect failure may release the unchanged head so authentic 130 recovery can still proceed. Crash/unknown/lost-lease windows remain held without refund, retry or fallback. Plain/pristine archive configuration grants no money or native spending lease, and current `config.usage` cannot replace an existing standalone obligation; Program/external-bank journal ownership is unchanged. This is the required contract; actual currency/custody evidence and instrumentation limits are reported below, not a stable released API guarantee.
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
  infrastructure, not a Q7 final-proof snapshot.

- **Fail-closed QuickJS legacy-drain audit.** Added
  `scripts/audit_legacy_drain.py` and its CTest contract. The tool produces a
  canonical, content-addressed proof from an explicitly enumerated frozen
  Program/Harness storage snapshot; it rejects unknown or mutable records,
  unclassified legacy sources, drain-only records, and active or recoverable
  legacy runs. SQLite inputs with live `-wal`, `-shm`, or `-journal` sidecars
  are rejected before opening, so raw copies of active WAL databases cannot
  serve as final proof. This establishes the Q7 evidence mechanism but does
  not claim the deployment-specific final drain or legacy-parser deletion is
  complete.

- **PostgreSQL final-drain archive scan.** The legacy-drain auditor now
  accepts a frozen `program_postgres_dump` custom archive and invokes
  `pg_restore` only in data-only, strict-table, script-output mode; it never
  restores into a database. It validates the Program bundle, version, and
  activation tables against their persisted identities, rejects an archive that
  omits or changes any required table, and treats an activation of a legacy
  Program version as a final-removal blocker.

- **No-deployment Q7 final-proof mode.** The legacy-drain auditor now accepts
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

- **Deprecated TransformerCPP integration examples.** Removed `example_inproc_gemma`,
  `NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE`, and `TRANSFORMERCPP_DIR`, which depended on
  an externally hosted repository that is no longer available. `example_local_transformer`,
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
  - **Engine ↔ Studio shared corpus**: `tests/fixtures/topology_corpus/` 15
    variants (3 valid + 12 violating E3–E11) are byte-identical with
    NeoGraph-Studio `tests/corpus/`, both asserting the same
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
  machine-readable witness (counterexample) JSON — for Studio canvas
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

  **Harsh scenario** (concurrency=10000, 1 cpu / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

  Same v0.3.0 / v0.5.0 measurements — **no regression of NeoGraph's "10K
  concurrent workers, peak RSS 5 MB, no OOM" selling point after destructive
  9b.**

## [0.9.0] — 2026-05-14 — v1.0 prep (Candidate 1 Phase B + Candidate 6)

Two v1.0 single-dispatch unifications from ROADMAP_v1.md converge in one cycle:

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
  deep_research_graph builder). See "Perf retrospective" section in
  ROADMAP_v1.md for details.

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
surfaced by real-world downstream (ProjectDatePop) feedback and internal
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

The opening release of the v1.0 sharpening track (ROADMAP_v1.md).
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
TODO_v0.3.md feedback batch from the FastAPI SSE chat-demo
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
  TODO_v0.3.md #9 cookbook fold.
- ROADMAP_v1.md — design-sharpening candidates derived from the
  cancel-rounds post-mortem (single dispatch, RunContext, hierarchical
  CancelToken — all delivered in v0.4.0).
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
  no-op (TODO_v0.3.md #5).

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

### Roadmap (deferred to v0.3.x — see `TODO_v0.3.md`)

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

Prior drafts of this CHANGELOG listed "~46 µs seq / ~114 µs par"
as a 3.0 regression. Those numbers came from a build tree where
`CMAKE_BUILD_TYPE` was unset, so the bench binary was compiled
without `-O3 -DNDEBUG`. On a proper Release build the async-peer
collapse is a **win** vs 2.0's Taskflow sync path (which the 2.0
README advertised at 20.65 µs seq / 150.7 µs par on the same
host). The corrected chart is at
[`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png).

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
