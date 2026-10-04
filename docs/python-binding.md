# Python Binding

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` is the pybind11 surface of the same C++ runtime. The wheel enables Core, LLM, Program/QuickJS, MCP and SQLite runtime durability; optional source builds expose only the components they compile.

This guide describes the current typed binding API. Older wheels that expose `complete` use a different provider interface; they cannot run the typed examples below.

```bash
pip install neograph-engine
```

## Typed provider requests and outcomes

`Provider` separates request preparation from dispatch. `SchemaProvider` validates and encodes a family-specific request through the SchemaProvider SDK. A `PreparedProviderRequest` holds that native preparation until one dispatch consumes it.

| Operation | Python signature | Result |
|---|---|---|
| Admit a descriptor | `load_provider_descriptor(source: str, policy=None)` | `ValidatedDescriptor`; invalid closed JSON raises `ValueError` |
| Construct a provider | `SchemaProvider(descriptor, options, defaults)` | Provider with admitted endpoint/family and runtime policy |
| Build a request | `make_provider_request(provider, model, messages, tools=[], controls=ProviderControls(), mode=ProviderMode.Collect)` | Typed `ProviderRequest` |
| Prepare | `provider.prepare(request)` | `PreparedProviderRequest` |
| Dispatch once | `provider.dispatch(prepared)` | Owned `ProviderOutcome` |
| Prepare and dispatch | `provider.invoke(request)` | Owned `ProviderOutcome` |

`model` is explicit. `ProviderControls` supplies typed limits and generation controls, including `max_output_tokens`, `temperature` and `top_p`. The factory constructs the SDK's family-specific payload; a raw dictionary cannot replace that payload. Set `request.mode` to `ProviderMode.Stream` to request streaming; setting `on_event` alone does not select it. `request` also carries `cancel_token`, `on_event`, `observer_limits` and optional `timeout_ms`.

`ProviderControls.provider` and `response_format`, `SchemaProviderDefaults.provider`, and `ProviderToolResult.host` return detached optional records. If present, read the record, edit it, then assign it back to the property; changing `controls.provider.order` alone does not update the controls. Assign `None` to clear a record.

Assigning a representable nonnegative `request.timeout_ms` starts an absolute deadline immediately. Set it just before preparation; later reads report milliseconds remaining, clamped to zero once expired. Leaving it `None` uses the provider's default timeout at preparation. Waiting with a prepared handle does not renew its deadline.

### Descriptor and runtime policy

The descriptor is closed, versioned JSON accepted by `load_provider_descriptor`. It admits the family, `base_url`, routes, field bindings and authentication rules. The request supplies `model`; runtime options supply credentials. `ProviderRuntimeOptions` supplies `api_key`, `default_timeout_ms`, `ca_file`, `workers` and the SDK's transport/resource limits. `SchemaProviderDefaults` supplies typed provider defaults. Keep credentials in runtime options, outside descriptor files and logs.

`ProviderDescriptorPolicy.identity` returns Python `bytes` containing the SDK policy's raw SHA-256 digest. Use `policy.identity.hex()` for display; the identity itself is neither UTF-8 text nor a hex-string alias.

The current transport uses libcurl. There is no `prefer_libcurl` switch, runtime endpoint override or WebSocket transport. The [C++ provider reference](reference-en.md) explains descriptor admission and supported request families.

Start the one-request loopback peer from the [SDK usage guide](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#run-the-first-request-without-a-hosted-api), then run this text-only example in another terminal. The endpoint, model and output cap match that peer. It sends no credentials. For an HTTPS peer, set its trusted CA through `NG_EXAMPLE_CA_FILE`. For a hosted endpoint, explicitly change the descriptor origin and request model and supply credentials for that origin in runtime options; hosted calls may incur charges.

```python
import json
import os
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider

descriptor = ng.load_provider_descriptor(json.dumps({
    "descriptor_version": 1,
    "revision": 1,
    "id": "python-guide-chat",
    "family": "openai.chat",
    "connection": {
        "base_url": "http://127.0.0.1:8765",
        "paths": {
            "buffered": "/v1/chat/completions",
            "streaming": "/v1/chat/completions",
        },
    },
    "bindings": {
        "model": "model", "messages": "messages", "stream": "stream",
        "max_output_tokens": "max_tokens", "usage": ["usage"],
    },
    "stop_reasons": {"stop": "EndTurn", "length": "MaxTokens",
                     "tool_calls": "ToolUse", "content_filter": "ContentFilter"},
}))
options = ng.ProviderRuntimeOptions(
    api_key="",
    ca_file=os.getenv("NG_EXAMPLE_CA_FILE", ""),
    default_timeout_ms=30_000,
)
provider = SchemaProvider(descriptor, options=options)
controls = ng.ProviderControls()
controls.max_output_tokens = 64
request = ng.make_provider_request(
    provider, "example-model",
    [ng.ProviderMessage(role=ng.ProviderRole.User, parts=[ng.Text("Say hello.")])],
    controls=controls,
)
prepared = provider.prepare(request)
outcome = provider.dispatch(prepared)
print("consumed:", prepared.consumed)
if outcome.failure is not None:
    print("failed:", outcome.failure.error.kind,
          outcome.failure.error.safe_message)
else:
    print(outcome.text)
count = outcome.usage.output_total
print("output tokens:", count.value if count is not None else "unknown")
```

`provider.invoke(request)` combines preparation and dispatch when you do not need the handle. `outcome.text` is a visible-text projection; `outcome.messages` retains the full typed parts. A missing usage counter prints `unknown`; a reported zero prints `0`.

Expected output with the linked synthetic peer:

```text
consumed: True
Hello.
output tokens: 0
```

Remove the peer's entire `usage` member and restart it to exercise the missing-counter path; the last line should become `output tokens: unknown`. This checks protocol mapping, not a real model's token counting.

### Interface 4 family controls

Use a wheel built against SDK interface/shared-library generation 4. Package versions, native archive v3 and portable JSON v2 are separate. These fragments construct controls without dispatch. Use each with an admitted provider of the corresponding family. Keep the first loopback request unchanged: OpenRouter-only controls reject that unadmitted origin before I/O.

```python
chat = ng.ProviderControls()
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
chat.chat_reasoning = reasoning
chat.include_reasoning = True
chat.usage_include = True
chat.models = ["openai/gpt-4.1", "openai/gpt-4.1-mini"]

responses = ng.ProviderControls()
responses.parallel_tool_calls = False
responses.verbosity = ng.ResponsesVerbosity.Low
responses.truncation = ng.ResponsesTruncation.Disabled
responses.responses_include = [ng.ResponsesInclude.ReasoningEncryptedContent]

messages = ng.ProviderControls()
messages.max_output_tokens = 4096
messages.thinking_mode = ng.MessagesThinkingMode.Adaptive
messages.output_effort = ng.MessagesOutputEffort.High
cache = ng.MessagesCacheControl()
cache.ttl = ng.MessagesCacheTtl.FiveMinutes
messages.cache_control = cache
choice = ng.MessagesToolChoice()
choice.mode = ng.MessagesToolChoiceMode.Auto
choice.disable_parallel_tool_use = True
messages.messages_tool_choice = choice

gemini = ng.ProviderControls()
gemini.temperature = 0.7
gemini.gemini_thinking_level = ng.GeminiThinkingLevel.Low
safety = ng.GeminiSafetySetting()
safety.category = ng.GeminiSafetyCategory.Harassment
safety.threshold = ng.GeminiSafetyThreshold.BlockMediumAndAbove
gemini.safety_settings = [safety]
choice = ng.GeminiToolChoice()
choice.mode = ng.GeminiToolChoiceMode.Auto
gemini.gemini_tool_choice = choice
```

Default-construct records, then assign fields; these records have no keyword constructors. Optional records and vectors return detached copies: edit and assign back. `ChatReasoningOptions` also has `max_tokens` and `exclude`. Nested Chat reasoning, `include_reasoning`, `usage_include` and `models` require a declared OpenRouter origin; scalar `reasoning_effort`, `service_tier`, typed routing and response formats remain available.

Responses verbosity is `Low/Medium/High`, truncation `Disabled/Auto`; include also supports `WebSearchSources`, `FileSearchResults`, `MessageOutputTextLogprobs`, `ComputerCallOutputImageUrl`, `CodeInterpreterCallOutputs`. `responses_include=None` preserves the default encrypted-reasoning include; an explicit list, including `[]`, selects that list and may make later native replay ineligible. Existing store, reasoning, hosted tools and tool-call caps remain typed.

Messages thinking is `Manual/Adaptive/Disabled`. Manual requires `thinking_budget` at least the admitted minimum and below the output cap; an absent mode with a budget selects manual. Adaptive/disabled forbid a budget. Enabled thinking omits temperature under vendor semantics, but model prohibitions reject explicit temperature. Effort is `Low/Medium/High/Max`, cache TTL `FiveMinutes/OneHour`; tool choice `Auto/Any/None_/Tool`, with `Tool` requiring `name` of a declared client tool. Messages routing requires a declared OpenRouter origin.

Gemini thinking level is `Minimal/Low/Medium/High` and excludes `thinking_budget`. Tool choice is `Auto/Any/None_/Validated`, with list `allowed_function_names`, and excludes `required_tool`. Safety categories are `Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity`, thresholds `BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off`. Wrong families, ranges and incompatible thinking/cap/tool controls reject before I/O. Model-prefix temperature prohibitions compare ASCII case-insensitively, including the suffix after the final `/`: Chat/Responses use `gpt-5`, `gpt-6`, `o1`, `o3`, `o4`; Messages use `claude-opus-4-7`, `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-5`, `claude-fable-`. See [SDK control admission](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#reasoning-sampling-and-tool-controls).

### Deployment headers before admission

Plain `load_provider_descriptor(source, policy=None)` treats `${VAR}` literally. These helpers preprocess host values before real descriptor admission:

```python
environment = ng.ProviderDeploymentHeaderEnvironment()
environment.anthropic_workspace_id = "workspace-example"
environment.anthropic_beta = None
# source is Messages descriptor JSON text.
descriptor = ng.load_provider_descriptor_with_deployment_headers(
    source, [("anthropic-workspace-id", "workspace-override")], environment,
)
```

`load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)` reads optional `ANTHROPIC_WORKSPACE_ID`/`ANTHROPIC_BETA` for Messages. Unset/empty values are omitted. Literal descriptor headers override environment; explicit pairs override both case-insensitively. Duplicate overrides, invalid/reserved names and line breaks fail admission. Both helpers accept optional `policy`; neither mutates an admitted descriptor or evaluates encoder templates.

### Responses cursor and foreign Gemini history

`previous_response_id` selects provider-held Responses state. Send only new input, not full history again. Starting from a successful Responses call, keep its bound controls and genuine terminal ID:

```python
# first_request/first_outcome belong to responses_provider and response_model.
responses.previous_response_id = first_outcome.messages[-1].id
responses.previous_response_history = (
    first_request.messages + first_outcome.messages
)
new_input = [ng.ProviderMessage(
    role=ng.ProviderRole.User, parts=[ng.Text("Continue.")],
)]
next_request = ng.make_provider_request(
    responses_provider, response_model, new_input, controls=responses,
)
```

`previous_response_history` is a vector of `ProviderMessage`, never a single response or JSON object. It is not emitted. Authentic client-tool ownership can require the full original prefix plus terminal assistant with ID equal to the cursor; plain server-held text continuation may leave it empty. Later in-process cursor outputs retain private terminal ownership without becoming full-history native replay or archive authority. Failed, edited or mismatched origin/model/config/route state rejects. Full native replay and archives still require their original full prefix. See [SDK Responses continuation](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#responses-provider-held-continuation).

Set `gemini.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign` explicitly for caller-created foreign assistant `Text`/`ProviderToolCall` history without native state, wire output or signatures; the default is `NativeOnly`. Only the first foreign function call receives Google's signature-validator bypass; text-only turns receive no signature. Genuine native groups still undergo strict validation. This cannot repair, strip or demote a failed seal, import reasoning authority or grant native replay to portable data. See [SDK foreign Gemini history](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#explicit-portable-gemini-history).

PortableForeign rejects imported wire metadata as well as signatures. Cursor output has `NativeReplay.complete == False` and is archive-ineligible; its private owner does not authorize full-history replay.


### Prepared handles and Python providers

Dispatch consumes a prepared handle once, including a failed dispatch. Inspect `prepared.valid` and `prepared.error` before dispatch, and `prepared.consumed` afterward. Retaining the Python object does not make it reusable. Build and prepare a new request for another attempt.

Python subclasses call `Provider(family)` and implement `get_name()` and `prepare(request)`. A subclass can delegate preparation to a `SchemaProvider` and return its authentic prepared handle. It cannot create a successful SDK outcome or import dispatch authority from JSON. Common `invoke` and `dispatch` use the native lifecycle; overriding legacy `complete` methods does not implement it.

When native execution calls a Python `prepare` override, returning `None` raises `TypeError` before a handle can be consumed. Return an authentic `PreparedProviderRequest`.

`NodeContext(provider=provider)` and assignment to `ctx.provider` retain the original Python provider in a native shared owner lease. Native context, compiled-node and engine copies keep that lease and call the same object's overrides even after `ctx.provider` is reassigned or external Python references are collected. Reassignment releases only the mutable context's lease and affects future compilation, not an existing engine. This retains object identity and lifetime; it does not freeze the provider's mutable state. The last lease releases the Python owner under the GIL.

The removed `CompletionParams`, `ChatCompletion`, `OpenAIProvider` and `RateLimitedProvider` exports have no compatibility aliases. Construct a validated `SchemaProvider` and use the request factory. `ChatMessage` and `ToolCall` remain graph convenience values; they are distinct from full SDK `ProviderMessage` and `ProviderToolCall`.

### Results, failures and usage

`ProviderOutcome` retains an immutable owned SDK result. Inspect `outcome.completion` or `outcome.failure`; the absent branch is `None`. Completion and failure views preserve full ordered messages, usage, attempt evidence, stop/error information and retained wire evidence. A failure can retain partial output. Keep that evidence when reporting failure rather than returning partial text as success.

`ProviderCompletion.wire_envelope` and `ProviderPartialCompletion.wire_envelope` are nullable, family-specific evidence. Check `wire_envelope is not None` before reading it. The current buffered Chat decoder leaves it `None` and retains the full response JSON in `raw_events` as `ProviderRawWire` (SDK `RawWire`) with `type == "chat.completion"` and the document in `payload`. Inspect the actual typed raw events rather than assuming an envelope exists. A raw event does not turn a failure into success, and an absent envelope does not mean the retained response is missing. No SDK fallback synthesizes an envelope.

Keep the owned outcome or its retained typed views: their genuine messages, native owners and available wire documents remain valid after the call and provider collection. Raw payloads retain provider-private fields, while native tracing excludes raw envelopes/events and native replay/reasoning. Python JSON views are data copies, not native replay or financial authority.

Consume full typed messages/parts and compare logical roles and text where those are the required semantics. Checkpoint and Chat request text content can legally be a text string or a typed text-part array; callers must not require one incidental serialized shape. Use full parts and authentic native ownership for continuation, not a flattened text or JSON substitute.

`ProviderMessage.parts`, completion/partial `messages` and `raw_events`, and usage `extra`/`conflicts` return detached lists or maps, including their bound values. Retained parts remain safe to inspect after the original collection is replaced; changing a copy cannot rewrite an immutable outcome. To edit a message, use `parts = message.parts`, modify `parts`, then assign `message.parts = parts`. `message.parts.append(...)` only changes the temporary Python list.

An SDK failure is returned data. Host observer or budget-settlement failures raise `ProviderObserverError` or `ProviderBudgetSettlementError`, derived from `ProviderOutcomeError`; these retain the real outcome and cause. A callback failure is therefore not permission to redispatch.

Repeated inspection of a stored Python provider or graph exception cause preserves the original exception object and traceback, including causes reached through native nested exception translation.

Usage counters are `UsageCount` values with `value` and `evidence`, or `None` when unknown. A reported zero is known usage; it differs from an omitted counter. Check `count is not None` before reading `count.value`. Do not use `count or 0`, or replace unknown input/output/total counters with zero in accounting.

On resume or continuation, `RunResult.provider_outcomes` preserves the ordered original outcomes followed by newly produced outcomes. `RunResult.usage` reflects the active accounting bank, which can restore prior reports from a checkpoint; a resume with no new provider call does not guarantee `None` or zero usage. Restoring evidence must neither redispatch the original provider request nor charge it twice. Retained reports describe prior calls; they do not grant a new spending allowance.

### Native history and portable exports

Returned `ProviderMessage` values carry full typed parts and authentic native replay state. Their native replay and wire output are read-only. For a direct provider continuation, preserve the original request's entire `request.messages` prefix and append `outcome.messages`, which contains the returned output rather than the full input history. With `request` and `outcome` from the first-call example, construct `history = request.messages + outcome.messages`, then build the next request with `ng.make_provider_request(provider, "example-model", history, controls=controls)`. This construction sends nothing; any next turn must also include its new user message or required tool results.

Genuine `NativeContext` replay checks the original prefix as well as the returned assistant message. Passing only the assistant output fails with `ReplayIneligible` before wire dispatch. Loading that assistant from `NativeArchive` preserves its custody and prefix binding; it still requires the original full prefix.

For graphs, `RunConfig.provider_messages` accepts full history and `RunResult.native_messages` already contains the full captured history. Use that graph result as the history without prepending the original input again. `RunResult.provider_outcomes` exposes retained outcomes; inside nodes, `RunContext.provider_outcomes` and `provider_loop_history` retain provider evidence.

`RunConfig.provider_messages`, `RunResult.native_messages` and `ProviderLoopEntry.messages` also return detached history copies. To update mutable input history, edit the returned list and assign it back to `config.provider_messages`. Retained messages and parts remain valid after that input is replaced; result and loop-history properties remain read-only.

Use `provider_messages_write(messages_or_outcome)` in a custom node to keep native history in its channel write. `portable_message(ChatMessage)` imports portable content and rejects imported native reasoning; `project_message(ProviderMessage)` produces an observation-only graph projection.

For native persistence, `NativeArchive.provision/open(directory, independent_key_file, owner_scope, descriptor)` returns an archive or `ProviderError`. Its `save(messages, binding="")` returns a reference or error; `load(reference, binding="")` returns typed messages or error. This uses the SDK's authenticated local custody with a separate host key. It does not turn portable JSON into replay or managed-budget authority.

`RuntimeHistoryRecord.serialize_canonical()` remains valid for portable records. To persist a record containing genuine native messages, use `record.serialize_canonical(archive, owner_id)` with a real `NativeArchive` whose `owner_scope` matches `owner_id`. Restore it with `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`; the defaults suffice for portable records, but native archive references require that same owner-scoped archive. No-argument serialization or imported JSON cannot recreate native authority. The Python parse and archive-aware serialization calls release the GIL around their native work, including archive I/O.

`RuntimeHistoryRecord.message` returns a detached typed copy that retains authentic shared native owners; changing that copy does not rewrite the immutable RAW record's identity. `ContextStore.hydrate_records(range)` returns typed records, and `history_record_by_message_id(feed, message_id)` returns a record or `None`. `InMemoryContextStore` and `SQLiteContextStore` inherit these methods. `SQLiteContextStore(database_path, archive=None)` accepts a genuine archive for native custody; archive-free storage rejects native history when custody is required. Construction and typed retrieval release the GIL. `LocalProgramHost` accepts the final optional keyword `native_history_archive=None` and forwards it to the real Program runtime; this supplies custody, not extra grants or a durable Program-store backend.

Portable state dictionaries and JSON exports describe message data. They cannot recreate native replay state, provider origin or managed-budget authority. A `native` flag in exported JSON is descriptive, not an authorization token. Converting through `ChatMessage`, a flat text channel or JSON may discard provider-specific parts; preserve full typed history with the original prefix when those parts are needed.

## Core graph quickstart

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages", [
        {"role": "assistant", "content": f"Hello, {state.get('name')}!"}
    ])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

### Tool ownership at compile time

Pass Python `Tool` instances, native C++ tools, and `MCPClient.get_tools()`
results together as `ng.NodeContext(tools=[...])`. Compilation snapshots this
list into an owned native `ToolSet` **before** constructing graph nodes. The
compiled engine retains the exact tools across `run()` and `resume()` even
after you replace the context's `tools` list or drop external references.
Changing the list later only affects a subsequent compile. Native MCP tools
keep their asynchronous execution path; there is no post-compile transfer or
per-dispatch ownership allocation.

## Core API parity

Python exposes the C++ execution abilities rather than a separate Python scheduler:

- sync and asyncio run/stream/resume;
- exact-checkpoint `resume_from`, fork, state inspection and ordered state writes;
- static and dynamic HITL through graph interrupts and `NodeInterrupt`;
- `RunMetadata` deadlines, trace/run identity and model-token ceilings;
- graph-wide and per-node `RetryPolicy`, including jitter;
- execution-local or explicitly reusable `CacheScope`;
- checkpoint and long-term Store backends;
- custom nodes, reducers, conditions, providers and tools;
- tool gates, execution policy, mandatory lifecycle Hooks and strict runtime interposition.

### Parity contract

“Parity” means that Python reaches the same native execution path and safety contract; it does not mean that every internal C++ storage or authority type is copied into Python.

| Capability | Native C++ path | Python surface | Status |
|---|---|---|---|
| Compile and execute Core graphs | `GraphEngine` | `GraphEngine.compile`, run/stream/async methods | Same scheduler and runtime |
| Runtime identity, deadlines and budgets | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | Same per-run values |
| Retry and node-cache policy | `RetryPolicy`, `CacheScope` | graph/node setters and cache scopes | Same runtime policy |
| Checkpoint, HITL and time travel | checkpoint stores and resume APIs | resume, exact `resume_from`, fork, state history/update | Same checkpoint contract |
| Program authoring and local execution | compiler, Catalog and `ProgramRuntime` | `ProgramCompiler`, `LocalProgramHost`, handles/results | Native owner-scoped convenience host |
| Mandatory lifecycle Hooks | registry, runner and `HookRuntime` | definitions plus `create_hook_runtime` callbacks | Same fail-closed lifecycle boundary |
| Runtime context and strict dispatch | context stores, receipts and interposition | matching immutable values, stores and `StrictRuntimeProfile` | Same native controller |
| Durable wheel defaults | SQLite Core/context/dispatch stores | `_HAVE_SQLITE` exports | Enabled in the PyPI wheel |

Raw `ProgramCatalog`, transition stores, replacement/migration controllers, synthesis gateways, Hook journals and RPC executors remain host-composition APIs. Exposing only fragments of those authority-bearing paths would bypass the required `proposal -> compile -> admit -> publish -> migrate/spawn` protocol. A future Python host controller must bind that protocol and its nonrenewable lineage budget as one owner-scoped unit; `_HAVE_PROGRAM` does not claim raw control-plane administration parity.

### Runtime retry overrides

```python
policy = ng.RetryPolicy()
policy.max_retries = 3
policy.initial_delay_ms = 100
policy.backoff_multiplier = 2.0
policy.max_delay_ms = 2_000
policy.jitter_pct = 0.2

engine.set_retry_policy(policy)
engine.set_node_retry_policy("remote_call", policy)
```

The graph definition's `"retry_policy"` remains the declarative default. Runtime setters are a distinct C++/Python configuration surface.

### Metadata and exact resume

```python
config = ng.RunConfig(thread_id="job-42", input={"task": "..."})
config.model_token_budget = 20_000
metadata = ng.RunMetadata(
    timeout_ms=30_000,
    trace_id="trace-42",
    run_id="run-42",
    owner_scope="tenant-a",
)
result = engine.run(config, metadata)

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

Inside a Python node, the same values are available through `input.ctx.trace_id`, `run_id`, `has_deadline`, `deadline_remaining_ms`, and `model_token_budget`.

`RunMetadata(timeout_ms=None, trace_id="", run_id="", owner_scope="", budget_cancel_token=None)` starts without a deadline by default. A supplied timeout and `metadata.set_timeout_ms(timeout)` accept nonnegative integer milliseconds within the remaining steady-clock range, validated before signed duration conversion or deadline addition. Negative or oversized integers raise `OverflowError` or `ValueError`; a failed setter leaves the previous deadline intact. Zero sets an immediate deadline. Use `metadata.clear_deadline()` to remove it.

### Cache scope

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable` is an explicit assertion that the node is independent of tenant, provider, Store, tools, credentials, time and resume state.

## Program and QuickJS

Python wheels build `neograph::program` and the restricted QuickJS frontend. A Python-defined node can participate in an immutable Program registry and execute through the native `ProgramRuntime`.

```python
import neograph_engine as ng

@ng.node("my_node")
def my_node(state):
    return [ng.ChannelWrite("value", state.get("value", 0) + 1)]

registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node(
        "my_node", "1.0.0", "sha256:" + "1" * 64
    )
    .add_registered_reducer(
        "overwrite", "1.0.0", "sha256:" + "2" * 64
    )
    .build()
)

source = ng.ProgramSource.from_javascript("agent.js", r'''
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "my_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}
export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
''')

ceiling = ng.ProgramRunBudget()
ceiling.wall_time_ms = 10_000
ceiling.model_tokens = 1_000
ceiling.monetary_microunits = 1_000
ceiling.max_concurrency = 2
ceiling.max_program_operations = 32
ceiling.max_core_steps = 20
ceiling.max_dynamic_compiles = 1

run_budget = ng.ProgramRunBudget()
run_budget.wall_time_ms = 10_000
run_budget.max_concurrency = 2
run_budget.max_program_operations = 32
run_budget.max_core_steps = 20

host = ng.LocalProgramHost(registry, "tenant-a", ceiling)
version = host.compile_admit(source, run_budget)
result = host.run(version, {}, run_budget)
print(result.status, result.output)
```

`LocalProgramHost` is an owner-scoped in-memory convenience host. It still uses the C++ compiler, Catalog, admission policy, transition store and ProgramRuntime. Generated proposals should additionally pass a host semantic validator before admission; see [DSL capability evaluation](DSL_CAPABILITY_EVAL.md).

Destroying `LocalProgramHost` releases the caller's GIL while its actual `ProgramRuntime` cancels, drains and joins scheduler work, so active Python nodes can finish. It reacquires the GIL before destroying the remaining host members; their Python callback/object owners keep GIL-safe destruction.

The native Program recorded-execution API is `ProgramRuntime::replay_recorded`, replacing `start_recorded`. `LocalProgramHost` exposes `run`, `start` and `resume`; it does not expose the raw recorded-binding control plane. Program results retain the typed provider outcome and failure evidence supplied by the native runtime, alongside the existing handle, budget and authority fields.

`ProgramResult.failure` is a read-only `ProgramFailure` value or `None`, not a dictionary. Its `provider_outcome` and `provider_cause` retain provider failure evidence; `code`, `message`, `operation_id`, `core_node`, `attempts` and `witness` describe the failed operation. Result fields also include `bundle_id`, `operation_id`, `attempt`, `checkpoint`, `interrupt` and `provider_budget_authority`.

The exact installed JavaScript vocabulary is available as a dict:

```python
manifest = ng.javascript_authoring_capability_manifest()
```

## Mandatory lifecycle Hooks

Hooks are triggered by host lifecycle events, not by a model deciding to call a tool.

```python
data = ng.HookDefinitionData()
data.phase = ng.HookPhase.CheckpointPublished
data.target_id = "audit"
data.delivery = ng.HookDelivery.BlockingMandatory
data.failure_mode = ng.HookFailureMode.FailClosed
data.effect = ng.ToolEffectClass.ReadOnly

mapper = ng.HookInputMapper()
mapper.kind = ng.HookInputMapperKind.Template
mapper.value_template = {"kind": "checkpoint"}
data.input_mapper = mapper

definition = ng.HookDefinition.create(data)
runtime = ng.create_hook_runtime(
    [definition],
    {"audit": lambda arguments, event_type, event_data: persist(arguments)},
)
engine.set_hook_runtime(runtime)
```

A callback failure under `FailClosed` blocks the protected runtime boundary. `Continue` is available only when observational loss is acceptable.

## Runtime context, Skills and strict dispatch

The binding exposes immutable RAW history records, context artifacts, epochs, required Skills/constraints, transformation receipts and provider dispatch receipts.

When creating a RAW `RuntimeHistoryRecord`, set `RuntimeHistoryRecordData.trust` to `RuntimeTrustClass.ModelOutput` for an Assistant message. The default `UntrustedInput` accepts only User messages; using it for Assistant output is rejected. These labels describe message provenance. They grant no tool-execution, budget or code authority, and do not replace genuine native replay custody.

```python
requirements = ng.RuntimeContextRequirements()
requirements.required_artifact_ids = [skill.id, constraint.id]
requirements.required_skill_artifact_ids = [skill.id]

assembler = ng.RuntimeTurnAssembler(
    context_store,
    max_input_tokens=32_000,
    requirements=requirements,
)
```

`ContextTransformReceipt` permits arbitrary derived evidence but requires every required artifact to remain byte-identical.

For the full strict path use durable SQLite stores:

```python
contexts = ng.SQLiteContextStore("runtime.sqlite3")
receipts = ng.SQLiteProviderDispatchReceiptStore("runtime.sqlite3")
hooks = ng.create_hook_runtime(definitions, callbacks)

profile = ng.StrictRuntimeProfile(
    provider,
    contexts,
    receipts,
    hooks,
    provider_binding_identity,
    max_input_tokens=32_000,
    required_context_artifact_ids=[constraint.id],
    required_skill_artifact_ids=[skill.id],
)
profile.activate("tenant-a", strict_epoch)
outcome = profile.invoke(request)
profile.attach(engine)
```

## HITL and state

Static `interrupt_before`/`interrupt_after`, dynamic `NodeInterrupt`, synchronous `resume`, asyncio `resume_async`, and exact `resume_from` require a checkpoint store.

Python `CheckpointStore` subclasses can implement `requires_managed_budget(thread_id) -> bool`. This synchronous virtual method reads persisted managed-bank denial obligations; the engine's native async facade calls it, and the binding acquires the GIL for the Python override. Report the truthful persisted obligation even when checkpoint state has been stripped or deleted. Omitting the override preserves the native explicit unsupported-backend error rather than returning `False`.

This reader grants no spending, restoration or lease authority. Managed bounded execution still needs actual supported native managed-budget leases; implementing the reader alone cannot supply them.

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

Use `get_state_history`, `update_state`, and `fork` for inspection and time-travel. `get_state_view()` provides flat Pydantic-backed channel access while `get_state()` retains the canonical nested representation.

## Async and cancellation

`run_async`, `run_stream_async`, and `resume_async` return `asyncio.Future` objects. Cancelling these graph Futures requests cancellation through `CancelToken`; native I/O must reach its cancellation boundary before the operation finishes. Graph streaming callbacks return to the caller's asyncio loop thread.

Provider `invoke` and `dispatch` are synchronous Python methods. Their bindings release the GIL while the native call runs, then acquire it for Python provider overrides and event callbacks. Event callbacks receive owned `ProviderEvent` values and may run on a native worker thread. Use `loop.call_soon_threadsafe` when a callback must update asyncio-owned state.

Assign an observer with `request.on_event = callback`, or clear it with `None`. Reading `request.on_event` preserves the original Python callable's identity; `RunConfig.on_provider_event` follows the same rule. Each event exposes a string `kind` and a typed `value`; a `ProviderPartDelta` value owns its `bytes`. Retaining an event after the callback does not retain a borrowed SDK text view. Call `token.cancel()` on the token assigned to `request.cancel_token` to request cancellation.

Use `asyncio.to_thread(provider.invoke, request)` to keep a synchronous provider call off the event-loop thread. Cancelling that await alone does not cancel the provider call: assign a `CancelToken` to `request.cancel_token` and request cancellation explicitly.

For a valid prepared handle whose token is already cancelled, provider dispatch preflight returns an owned SDK `Cancelled` failure. Synchronous dispatch drains the same native async provider path. Cancellation at graph, host or coroutine entry is a separate boundary and can raise before a `ProviderOutcome` is returned; requesting cancellation does not guarantee typed `Cancelled` data at every boundary. A cancellation request also cannot prove that no request was sent, that remote work stopped or that no charge will be incurred.

The [pybind11 GIL documentation](https://pybind11.readthedocs.io/en/stable/advanced/misc.html#global-interpreter-lock-gil) explains why releasing the GIL and reacquiring it for Python callbacks are separate binding responsibilities.

## Protocols and observability

- MCP client tools are available through `neograph_engine.mcp` when built.
- A2A client types are available through `neograph_engine.a2a` when built.
- `ProtocolHostAdapter` integrates official Python A2A/ACP server SDKs with NeoGraph session semantics.
- `neograph_engine.tracing` and `neograph_engine.openinference` emit vendor-neutral OTel/OpenInference data for Phoenix, Langfuse, Arize and compatible backends.

### Native A2A discovery and streaming

`a2a.WireDialect` is `V0_3/V1_0`. `client.wire_dialect()` returns `None` after construction or forced card fetch until a card-selected RPC or successful probe. `AgentCard.supported_interfaces` contains detached `AgentInterface` records with `url`, `protocol_binding`, `protocol_version`, `tenant`; `card.raw` is observational JSON. The native client selects compatible JSONRPC interfaces, preferring its normalized base URL; card URLs never redirect the RPC endpoint. Without a card, only numeric RPC error `-32601` permits a dialect probe; card-selected calls do not fallback. `a2a.A2ARpcError` retains the actual integer `.code`.

```python
from neograph_engine import a2a
from uuid import uuid4

client = a2a.A2AClient("http://127.0.0.1:8080")
card = client.fetch_agent_card()
for interface in card.supported_interfaces:
    print(interface.protocol_version, interface.tenant)

params = a2a.MessageSendParams()
params.message.message_id = str(uuid4())
params.message.role = "user"
params.message.parts = [
    a2a.Part.text_part("Explain this item."),
    a2a.Part.text_part("Keep the answer short."),
]
configuration = a2a.MessageSendConfiguration()
configuration.blocking = True
configuration.accepted_output_modes = ["text/plain"]
params.configuration = configuration
events = []

def on_event(event):
    events.append(event)  # Owned snapshot remains valid after the callback.
    return True

task = client.send_message_stream(params, on_event)
print(client.wire_dialect(), task.id, task.state)
```

Start an actual local A2A server on that endpoint before running this example; it is separate from the Chat loopback peer. `send_message(params)` is the nonstreaming multipart overload; text convenience overloads remain. Set credentials explicitly with `client.set_authorization_header(authorization_header)` and keep the value out of logs. `Part.media_type`, `file`, `data`, `metadata`, message extensions/reference IDs and task artifacts retain typed/data observations, never provider native authority.

`params.message` and `Task.status` are live inline records; optional configuration, vectors and event children are detached snapshots requiring reassignment to update mutable inputs. `StreamEvent.type` uses `StreamEvent.Type.StatusUpdate/ArtifactUpdate/Task`; inspect `status_update`, `artifact_update`, `task` and `is_final()`. V1 opening tasks are not final; native SSE assembles status and append/replace artifact updates. Blocking calls release the GIL; callbacks acquire it and retain GIL-safe owners. Callback delivery does not permit output-observed redispatch. Callback threads are not promised to be an asyncio loop; use `loop.call_soon_threadsafe` when needed.


Import `OpenInferenceProvider` from `neograph_engine.openinference`. Its constructor is `OpenInferenceProvider(inner: Provider, tracer, *, span_name="llm.complete")`; the facade delegates to the native C++ wrapper, which inherits typed `prepare(request)`, `dispatch(prepared)` and `invoke(request)`. The default span name is a label, not a restored `complete` method. Install `opentelemetry-api` and `opentelemetry-sdk`; construction requires the OTel API, and export requires a configured SDK span processor/exporter.

Preparation, failed admission and abandonment create no LLM span. An admitted dispatch starts one span and ends it on completion, SDK failure or a dispatch exception. The prepared operation retains the Python tracer adapter even after the wrapper and external tracer references are collected. Blocking `invoke`/`dispatch` release the GIL; OTel calls and Python-reference destruction acquire it. Tracer failures follow the native best-effort policy without replacing the owned outcome, original events or product exception.

The adapter calls the Python tracer's `start_span` at dispatch. Caller context is not automatically carried into native worker threads. Capture the intended OTel parent context before dispatch and pass it explicitly through a tracer adapter, as below; do the same for graph-node providers when they need a graph/node parent. OTel documents [explicit parent-context selection](https://opentelemetry-python.readthedocs.io/en/latest/api/trace.html#opentelemetry.trace.Tracer.start_span).

The example's `ParentContextTracer` keeps one captured parent context. For a different logical parent, capture fresh context and construct a new adapter/provider wrapper; reusing the old adapter keeps its earlier parent.

With `provider` and `controls` from the provider example above, restart the one-request peer before running this fragment:

```python
import neograph_engine as ng
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import ConsoleSpanExporter, SimpleSpanProcessor
from neograph_engine.openinference import OpenInferenceProvider

class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer = tracer
        self.parent_context = parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)

traces = TracerProvider()
traces.add_span_processor(SimpleSpanProcessor(ConsoleSpanExporter()))
tracer = traces.get_tracer("python-guide")
with tracer.start_as_current_span("request"):
    observed = OpenInferenceProvider(
        provider, ParentContextTracer(tracer, otel_context.get_current()),
        span_name="llm.request",
    )
    request = ng.make_provider_request(
        observed, "example-model",
        [ng.ProviderMessage(role=ng.ProviderRole.User,
                            parts=[ng.Text("Say hello.")])],
        controls=controls,
    )
    prepared = observed.prepare(request)
    outcome = observed.dispatch(prepared)
    print("consumed:", prepared.consumed)
    print("failure:", outcome.failure is not None)
    print(outcome.text)
traces.shutdown()
```

The console exporter should show `llm.request` with `openinference.span.kind="LLM"` and its `parent_id` matching the `request` span's `span_id`. With the linked peer, the printed result is consumed, has no failure, and contains `Hello.`. For a fresh request, `observed.invoke(request)` combines the same preparation and dispatch; never dispatch the consumed handle again.

LLM span fields contain the admitted model, declared temperature/output cap, public message roles and visible `Text` content, and known input/output/total usage counts. Missing counts omit the corresponding attributes; reported zero remains zero. A streaming request adds `llm.token` events only for visible text-content deltas, with text in `attributes["chunk"]`. SDK failures set ERROR status using the safe message and retain partial public output; successful completions set OK. `request.on_event` still receives the original typed events.

Native replay, reasoning parts, raw envelopes and raw events do not enter these LLM span fields. The outcome still retains its native messages and evidence; tracing exports grant neither replay custody nor accounting authority. Public text can contain sensitive application data, so choose prompts and an exporter accordingly. `openinference_tracer(tracer, *, root_name="graph.run", node_span_prefix="node.", on_event=None)` remains the separate graph-event context manager; it does not replace provider LLM spans.

## Optional components

The public package marks optional C++ components honestly:

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A`;
- missing components are absent rather than emulated in Python;
- the PyPI wheel enables Program/QuickJS, LLM, MCP and SQLite; source builds follow their CMake options.

## Tests and examples

The binding suite covers Core execution, custom callbacks, asyncio, cancellation, Program compilation/runtime, mandatory Hooks, strict context, SQLite persistence, protocols and README examples.

- [Python examples](../bindings/python/examples/README.md)
- [C++ examples](../examples/README.md)
- [QuickJS authoring boundary](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [Strict runtime interposition](STRICT_RUNTIME_INTERPOSITION.md)
