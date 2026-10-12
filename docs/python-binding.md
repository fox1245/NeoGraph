# Python Binding

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` is the pybind11 surface of the same C++ runtime. The wheel enables Core, LLM, Program/QuickJS, MCP and SQLite runtime durability; optional source builds expose only the components they compile.

This guide requires a wheel built against SchemaProvider SDK `0.3.0`, interface revision `6` and shared-library ABI revision `6`; install its bundled components together. Older wheels that expose `complete` use a different provider interface; they cannot run the typed examples below.

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

`ProviderControls.provider` and `response_format`, `SchemaProviderDefaults.provider`, `OpenRouterRouting.max_price`, and `ProviderToolResult.host` return detached optional records. If present, read the record, edit it, then assign it back to the property; changing `controls.provider.order` alone does not update the controls. Assign `None` to clear a record.

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

### Family controls

Package versions, native archive v3 and portable JSON v2 are separate. These fragments construct controls without dispatch. Use each with an admitted provider of the corresponding family. Keep the first loopback request unchanged: OpenRouter-only controls reject that unadmitted origin before I/O.

```python
chat = ng.ProviderControls()
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
chat.chat_reasoning = reasoning
chat.include_reasoning = True
chat.usage_include = True
chat.models = ["openai/gpt-4.1", "openai/gpt-4.1-mini"]
routing = ng.OpenRouterRouting()
price = ng.OpenRouterMaxPrice()
price.prompt_nano_usd = 75_000_000       # 0.075 USD per million prompt tokens
price.completion_nano_usd = 300_000_000  # 0.30 USD per million completion tokens
routing.max_price = price
chat.provider = routing

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

Default-construct records, then assign fields; these records have no keyword constructors. Optional records and vectors return detached copies: edit and assign back. `OpenRouterRouting.max_price` is one: `routing.max_price.prompt_nano_usd = ...` edits a copy, so build an `ng.OpenRouterMaxPrice` and assign the whole object. Its amounts are integer nanoUSD, `prompt_nano_usd` and `completion_nano_usd` per million tokens, `image_nano_usd`, `audio_nano_usd` and `request_nano_usd`, sent as exact decimal USD (`75_000_000` is `0.075`); at least one is required and none may exceed 2**53. `SchemaProviderDefaults.provider` merges `max_price` like the other routing fields: a request's own replaces the default. `ChatReasoningOptions` also has `max_tokens` and `exclude`. Nested Chat reasoning, `include_reasoning`, `usage_include` and `models` require a declared OpenRouter origin; scalar `reasoning_effort`, `service_tier`, typed routing and response formats remain available.

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

`outcome.usage.provider_cost` is the immutable `ProviderReportedCost` view. Its optional `ProviderUsdAmount` fields are `total`, `upstream_total`, `upstream_input` and `upstream_output`, with `nano_usd`, `evidence` and `rounding`. `status` is a four-entry array in that order; `byok_status` describes nullable `is_byok`. `ProviderCostStatus`, `ProviderCostSource` and `ProviderCostRounding` preserve missing, available, malformed, precision-exceeded, overflow, unknown-currency and conflict evidence. Amounts are rounded upward from parsed binary64, not from the original decimal lexeme. A reported zero or `is_byok=False` is not absence. These observations are neither invoices nor token-budget charges; inspect ordered `provider_outcomes` for per-call money evidence, not an aggregate token bank.

`UsageAccumulator` is a token-only aggregate. A single report retains its monetary metadata; after a second report the snapshot's `provider_cost` is explicitly reset to canonical Missing/None, not the stale first-call cost and not a sum. Restoring that snapshot does not recreate costs or renew token authority. Read the exact monetary evidence from each original outcome or provider journal entry.

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
checkpoint_id = result.checkpoint_id

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
    # GraphState.get takes only the channel name; a missing channel is None.
    return [ng.ChannelWrite("value", (state.get("value") or 0) + 1)]

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
print(result.status, result.output["value"])  # ProgramTerminalStatus.Completed 1
```

`ProgramRegistryBuilder.add_registered_node`, `add_registered_reducer` and `add_registered_condition` copy an executable that is already registered in this process, through `@ng.node` or `ng.NodeFactory.register_type`, `ng.ReducerRegistry.register_reducer` or `ng.ConditionRegistry.register_condition`. Each raises `ng.ProgramRegistryError` for a name that is not registered, and the message names the registration call. The error's `kind` is `"node"`, `"reducer"` or `"condition"`, and its `name` is the missing name. It subclasses both `ValueError` and `RuntimeError`, so handlers written for either keep catching it. A Program that uses a node type, reducer or condition missing from the registry snapshot fails at compile time instead, in `ProgramCompiler.compile` and `LocalProgramHost.compile_admit` alike, with `ng.ProgramCompileError` and the diagnostic `P_CORE_GC_REGISTRY_MISS`.

Each diagnostic dict has a `help` entry: a dict with `doc` (its entry in the [diagnostics guide](diagnostics.md)), `related`, `suggestions` and `candidates` for the diagnostics a compilation returns, `None` for the sealed `bundle.diagnostics`. The exceptions always carry it. `ProgramCompiler(..., echo_routes=True)` also returns a `P_CORE_ROUTES` note with the routed successor table. `soundness_check=True` on `ProgramCompiler` or `LocalProgramHost` runs the [soundness check](program-graphs.md#soundness-check): `P_CORE_S1` to `P_CORE_S5` warnings with witness traces and a `P_CORE_SOUNDNESS` summary. None of these is sealed.

`LocalProgramHost` is an owner-scoped in-memory convenience host. It still uses the C++ compiler, Catalog, admission policy, transition store and ProgramRuntime. Generated proposals should additionally pass a host semantic validator before admission; see [DSL capability evaluation](DSL_CAPABILITY_EVAL.md).

Destroying `LocalProgramHost` releases the caller's GIL while its actual `ProgramRuntime` cancels, drains and joins scheduler work, so active Python nodes can finish. It reacquires the GIL before destroying the remaining host members; their Python callback/object owners keep GIL-safe destruction.

The native Program recorded-execution API is `ProgramRuntime::replay_recorded`, replacing `start_recorded`. `LocalProgramHost` exposes `run`, `start`, `resume` and `fork`; it does not expose the raw recorded-binding control plane. Program results retain the typed provider outcome and failure evidence supplied by the native runtime, alongside the existing handle, budget and authority fields.

`ProgramResult.failure` is a read-only `ProgramFailure` value or `None`, not a dictionary. Its `provider_outcome` and `provider_cause` retain provider failure evidence; `code`, `message`, `operation_id`, `core_node`, `attempts` and `witness` describe the failed operation. Result fields also include `bundle_id`, `operation_id`, `attempt`, `checkpoint`, `interrupt` and `provider_budget_authority`.

The exact installed JavaScript vocabulary is available as a dict:

```python
manifest = ng.javascript_authoring_capability_manifest()
```

### Module shape and `callCore`

A JavaScript Program must export `define()`, which returns one graph builder. That graph is the Program's only Core. Without `define()`, compilation raises `ProgramCompileError` with `P_JS_DEFINE_MISSING`, so a module that exports only `main()` is rejected. `main()` is optional; without it, a run executes the Core directly.

`ng.callCore(name, input, sourceSite)` must name that Core by the name given to `ng.graph(...)`, which is `"main"` in the example above. Any other name fails the run with `P_JS_CONTROL_COMMAND`. To run different steps, put them in that graph and call it again with different input. Register every node type with `@ng.node` or `ng.NodeFactory.register_type` before passing it to `add_registered_node`, which raises `ng.ProgramRegistryError` for a type that is not registered.

A completed `callCore` returns the Core's output object to `main()`. The quickstart above returns it unchanged as `result.output`:

```json
{"channels": {"value": {"value": 1, "version": 1}}, "global_version": 1, "value": 1}
```

`channels` holds each channel's `value` and `version`. Each channel's value is also copied to a top-level key with the channel's name unless that key already exists, so `main()` can read `result.value`; a channel named `channels` or `global_version` is reachable only under `channels`. Barrier lists are stored sorted: `graph.barrier("join", ["zeta", "alpha"])` appears as `{"wait_for": ["alpha", "zeta"]}` in `bundle.sealed_core_definitions`, and the order has no effect.

### Failed commands end the run

Only a completed command resumes `main()`. When a yielded command does not complete, for example a `callCore` whose node raises, which exhausts the Core step budget, which stops at a barrier that can no longer fire, or which runs past the wall-time limit, `ProgramRuntime` records that outcome and makes it the run's terminal result with the matching `status` and `failure`. The generator is not resumed: a `try`/`catch` around the `yield` never runs, and no later JavaScript executes. A join (`ng.all`, `ng.race`, `ng.quorum`) that fails under its failure policy ends the run the same way. Handle failures in Python through `result.status` and `result.failure`. An interrupted command is different: the run stops as `Interrupted`, and `resume()` continues that command. A join member that interrupts pauses its join: the join launches no further member and lets the members already running finish. If their results decide the join (a fail-fast failure, a race winner, a quorum met or out of reach), it ends that way. Otherwise the run stops as `Interrupted` on the first paused member, and `resume()` continues that member and replays the members that finished from the command journal, without running or charging them again. The deadline of `ng.await(command, timeout_ms)` is absolute: the command journal records when the await started, and a resume, or the recovery of a stopped process, has only the time that remains. Resumed after its deadline, the await ends the run `TimedOut` with `P_AWAIT_TIMEOUT` at once, without running the answered node (#391). A plan-level `await` operation keeps its deadline across a resume the same way. A member paused on an uncertain effect, a `NodeInterrupt` whose payload `kind` is `tool_effect_reconciliation`, outranks a member's failure, as in a Core step (#392): the join pauses on it instead of failing, records the failed member, and ends with that failure when it is resumed; a join that a success decides ends that way (#397). On resume, the member the answer continues runs to its end before the recorded failure ends the join, so the answer reaches it and its effect settles (#400). Whenever a join pauses, it pauses first on a member paused on an uncertain effect, whatever the declaration order (#401). A collecting `ng.all` (`collect: true`) is the deliberate exception to this precedence: it waits for every member, paused ones included, before it reports, so a member's failure does not end it while another member is paused on an ordinary question; the run stops as `Interrupted` on that member, and the failure is reported once every member has settled (#401).

### Graph checks and barrier stalls

Compilation checks the Core graph along the routes the scheduler actually takes: a node with a conditional edge routes only through its first conditional edge. A JavaScript Program is a strict document, so each of these raises `ng.ProgramCompileError`:

- `P_JS_GRAPH_ARGUMENT` at a `.node()` call that names a node `__start__` or `__end__`;
- `P_JS_GRAPH_ENTRY` when `define()` returns a graph with nodes but no `.entry()`;
- `P_CORE_E13` for a static edge out of a node that also has a conditional edge, because that edge never fires;
- `P_CORE_E14` for a second conditional edge from the same node, because it never routes.

`P_CORE_E15` is a warning in `bundle.compile_diagnostics`: a barrier member has a route that leads somewhere other than the join or `__end__`, and taking it stalls the join. Loops are valid; pass `require_acyclic=True` to `ProgramCompiler` or `LocalProgramHost` to reject every cycle with `P_CORE_E16`, for example for plans and checklists.

A barrier fires once every node in its `wait_for` list has routed to it. A node that reaches the join some other way, for example a fix-up node on a member's failure route, does not stand in for that member. When a run has nothing left to run and a join still holds signals, it does not complete: it ends `Failed` with `P_CORE_BARRIER_STALLED`, `failure.core_node` names the join, and `failure.witness["stalled_barriers"]` lists each stalled join with its `wait_for`, `received` and `missing` nodes. Only an explicit route to `__end__` in the final super-step ends such a run normally: a static edge to `__end__` (which `.exit()` writes), a conditional route to `__end__`, or `Command.goto("__end__")`. A node without successors does not, and neither does an explicit end in an earlier super-step. A `GraphEngine` run in the same state returns `ng.RunStatus.BarrierStalled`, and `result.output["_neograph"]["barrier_stalled"]` holds the same details.

### Errors and diagnostics

Program errors raised in Python are typed and keep their diagnostics:

| Exception | Raised by | Subclasses |
|---|---|---|
| `ng.ProgramCompileError` | `ProgramCompiler.compile`, `LocalProgramHost.compile` and `compile_admit` | `RuntimeError` |
| `ng.ProgramAdmissionError` | `LocalProgramHost.admit` and `compile_admit`, for example `P_ADMIT_POLICY` or `P_ADMIT_PROVIDER` | `RuntimeError` |
| `ng.ProgramDiagnosticError` | `run`, `start`, `start_active` and `resume`, for example `P_START_BUDGET` or `P_RUN_NOT_FOUND` | `RuntimeError` |
| `ng.ProgramRegistryError` | `ProgramRegistryBuilder.add_registered_node`, `add_registered_reducer` and `add_registered_condition` | `ValueError`, `RuntimeError` |

The message of the first three starts with `Program compilation failed`, `Program admission failed` or `Program operation failed`, followed by one line per diagnostic: its code, JSON pointer and message, and `(source:line:col)` when it has a source site. `.diagnostics` is a list of dicts with `code`, `severity`, `phase`, `message`, `source_id`, `json_pointer`, `span` and `witness`.

A bundle that compiles carries two lists. `bundle.compile_diagnostics` is what the compiler returned: warnings under the current rules, with JavaScript call sites in `span`, and authoring warnings such as `P_JS_CONTROL_CHARACTER`. Show this list to authors. `bundle.diagnostics` is the copy sealed into the bundle and covered by `bundle.id`. It keeps the rules and wording that bundles were first sealed under, so a newer compiler does not change a bundle id, and it can therefore differ: it has no spans and may still list warnings the current rules dropped, such as `P_CORE_E6` for a channel that only Python nodes write. Neither the compiler nor `LocalProgramHost` prints lint to stderr.

### Model-calling nodes

A `LocalProgramHost` binds one provider to the built-in `llm_call` nodes of the Programs it admits. The registry declares the provider by name and the nodes that require it; the host supplies the provider object, the model and the request controls, under a non-secret `binding_label`:

```python
from neograph_engine.llm import SchemaProvider

registry = (
    ng.ProgramRegistryBuilder()
    .add_provider("openrouter", "1.0.0", "sha256:" + "3" * 64)
    .add_registered_node("llm_call", "1.0.0", "sha256:" + "4" * 64, provider="openrouter")
    .add_registered_reducer("append", "1.0.0", "sha256:" + "5" * 64)
    .build()
)
controls = ng.ProviderControls()
controls.max_output_tokens = 256
ceiling.model_tokens = 100_000
ceiling.monetary_microunits = 500_000
# descriptor and options as in "Descriptor and runtime policy" above
host = ng.LocalProgramHost(
    registry, "tenant-a", ceiling,
    node_context=ng.NodeContext(provider=SchemaProvider(descriptor, options),
                                model="deepseek/deepseek-v4-flash-0731",
                                provider_controls=controls),
    binding_label="openrouter-prod")

source = ng.ProgramSource.from_javascript("chat.js", r'''
export function define() {
  const graph = ng.graph("chat");
  graph.channel("messages", {reducer: "append", initial: []});
  graph.node("llm", {type: "llm_call"});
  graph.entry("llm");
  graph.exit("llm");
  return graph;
}
''')
run_budget.model_tokens = 100_000
run_budget.monetary_microunits = 500_000  # 0.50 USD
version = host.compile_admit(source, run_budget)
result = host.run(version, {"messages": [{"role": "user", "content": "Hi"}]}, run_budget)
print(result.usage.monetary_microunits, [r.evidence for r in result.monetary_records])
```

`add_provider(name, semantic_version, implementation_digest)` registers the provider's identity, and `add_registered_node("llm_call", ..., provider=name)` registers the built-in node with that identity as a static requirement. `provider=` binds only `llm_call` and raises `ValueError` for any other type or an unknown provider. The host binds its `node_context` provider to that name whatever its descriptor id. `NodeContext(provider_controls=...)` takes the typed `ProviderControls` the node sends, such as `max_output_tokens`. A `node_context` needs a provider and `binding_label`, and no tools.

The label is the only part of the binding in any identity. A Program version that requires the provider carries a capability receipt whose identity is a digest of the label and the provider's registry identity; the provider's key, endpoint and model are not in it, so changing the model keeps the version id and changing the label changes it. A Program that requires no provider gets no receipt and keeps its bundle and version ids. The label must be 1 to 64 characters of `a-z`, `0-9`, `.`, `_`, `-` and `:`, starting with a letter or digit, and must not look like an API key. Without `node_context`, a Program that requires the provider is rejected at admission with `P_ADMIT_PROVIDER`, and so is a Program with an `llm_call` or `intent_classifier` node registered without `provider=`.

Each call is priced against the run's `monetary_microunits` from the price catalog compiled into NeoGraph. Before the request is sent, the call's upper bound is reserved. On OpenRouter the request also carries that bound as `provider.max_price`: the route ceiling's input and output rates, or the lower `max_price` your controls or `SchemaProviderDefaults` already set, so OpenRouter cannot route the call to a dearer endpoint. Pricing prepares, and never sends, a probe of the request whose conversation is one user message, so a Python provider's `prepare` sees it. A call that cannot be bounded (a model without a catalog entry, a router alias without a route ceiling, a request without an output cap) or whose reservation does not fit the remaining budget is refused before anything is sent, and the run fails with a `P_MONEY_*` code. Like every `ProgramRunBudget` field, `monetary_microunits = 0` grants nothing, so such a run refuses every call. After the reply, `result.monetary_records` holds one `ng.ProgramMonetaryRecord` per call with its `evidence`: `reported` (OpenRouter `usage.cost`), `estimated` (catalog rates; `catalog_version` and `catalog_digest` name the catalog) or `held` (the outcome or usage is unknown, so the reservation is charged). `result.usage.monetary_microunits` is their sum. The codes, the bounds and the settlement rules are in [Pricing evidence and the price catalog](reference-en.md#pricing-evidence-and-the-price-catalog).

A Python node that calls a model owns its provider itself; the Program budgets do not see such a call.

### Child Programs

`ng.spawn` needs a child-binding resolver, and `LocalProgramHost` configures none. Every `ng.spawn` from Python therefore fails the run with `P_CHILD_BINDING` ("Program spawn has no configured child binding resolver"). Child Programs need a C++ host that sets `RuntimeConfig::child_binding_resolver`, which maps each binding name to the `ModuleLinkReceipt` from `link_module_child` and the admitted child `ProgramVersion`. See [recursive agents](PROGRAM_RECURSIVE_HARNESSES.md) for the child lifecycle.

### Budgets

The `budget_ceiling` passed to `LocalProgramHost` becomes the admission policy's ceiling. Every field except `max_child_depth` and `max_total_children` must be positive, including `model_tokens`, `monetary_microunits` and `max_dynamic_compiles`, even for a Program that never calls a model. Otherwise the constructor raises `ValueError` naming the zero fields. The two child fields must be both zero or both positive.

`compile_admit(source, budget)` and `compile(source, budget)` seal `budget` into the bundle as both the minimum and the maximum of every field, and admission rejects a bundle whose sealed budget exceeds the ceiling. Each `run`, `start` and `start_active` call for that version must pass the same values, field for field. A larger or smaller budget raises `ng.ProgramDiagnosticError` with `P_START_BUDGET`, whose message names the first field that differs, for example `Program invocation budget wall_time_ms=5000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`. To use a different budget, compile and admit again; the result is a new `ProgramVersion`. A run budget needs positive `wall_time_ms`, `max_concurrency`, `max_program_operations` and `max_core_steps`; `model_tokens` and `monetary_microunits` may be zero. Zero grants nothing: a run that uses model tokens with `model_tokens = 0` ends `BudgetExhausted`, and with `monetary_microunits = 0` every priced provider call is refused before it is sent (see [Model-calling nodes](#model-calling-nodes)).

### Failure routing

A `retryPolicy(node, {...})` with `on_failure` routes a node's final failure to a handler instead of failing the run; [Program graph features](program-graphs.md#failure-routing) has the rules. A Python node names its failure with exception attributes: a string `code` (an author code such as `app.compile_error`), a boolean `retryable` and a string `public_message`. `str(exc)` never reaches a record. A node that raises `ng.ProgramDiagnosticError`, `ng.ProgramCompileError` or `ng.ProgramAdmissionError` reports a configuration verdict (`NG.ConfigError`), which is never retried or routed. When the handler has `interruptBefore`, the run stops `Interrupted` and `result.interrupt.pending_input.core_interrupt_value` holds `{"reason": "failure_route", "failure": record}`; `host.resume(run_id, value)` continues at the handler. A failure exit ends the run `Failed` with its declared code, and a node failure without a failure edge ends it `Failed` with the classified code (`NG.TaskFailed`, an author code, ...).

`ProgramRunBudget.max_node_retries` (default `None`, no ceiling) caps node retries across the whole run; `result.usage.node_retries` counts them and `result.remaining_budget.max_node_retries` is what is left. `compile_admit` does not seal it, so a run may set it without compiling again. `LocalProgramHost(..., failure_text_redactor=fn)` and `GraphEngine.compile(..., failure_text_redactor=fn)` take the redactor for nodes that set `error_text`: `fn` receives `{"node", "code", "kind", "attempts", "retryable", "public_message"}` and returns the text to keep, or `None`. Without it no text is written. On a `GraphEngine`, `result.status` is `ng.RunStatus.FailureExit` after a failure exit, and `engine.resolved_retry_policy(node)` returns the retry timing a node runs with.

`retryPolicy(node, {attempt_timeout_ms: ...})` limits each attempt of a node (100 to 3 600 000 ms). A timed-out attempt sees `input.ctx.cancel_token.is_cancelled()` become true and fails with `NG.Timeout`, retried and routable like any failure; whatever it returns afterwards is discarded. A Python body cannot be preempted, so a node that sleeps or blocks should poll the token: the next attempt starts only after the timed-out one returns, and one that has not returned within its stop deadline (the timeout again, at least one second) fails the run with `NG.EffectUncertain`. Such a body keeps running in the background, and the binding waits for it when the interpreter exits. A backoff that would cross the run's `wall_time_ms` stops retrying with kind `deadline`. On a `GraphEngine`, `engine.set_node_attempt_timeout_ms(node, ms)` sets a host timeout (`None` clears it; a definition's own timeout can only be shortened) and `engine.resolved_attempt_timeout_ms(node)` returns the one a node runs with. See [Program graph features](program-graphs.md#attempt-timeouts-and-the-deadline).

### Scheduler threads, workers and timeouts

`LocalProgramHost(..., scheduler_threads=1, worker_count=1)` takes two thread settings. `scheduler_threads` sets the number of threads in the `ProgramRuntime` scheduler pool that drives runs: JavaScript control, `callCore` dispatch, joins and deadlines. It must be at least 1. `worker_count` sets the worker pool of each Core engine for fan-out inside one graph, and must not exceed the admitted Program's `max_concurrency`; with the default 1, fan-out branches run on the scheduler thread.

A Python node body does not occupy a scheduler thread during a Program run. It runs on a worker pool that the binding owns, 32 threads by default; set `NEOGRAPH_PYTHON_NODE_THREADS` to a value from 1 to 1024 before the first Program run to change it. Even with the default single scheduler thread, several runs of the host, the members of `ng.all`, `ng.race` and `ng.quorum`, and the fan-out branches of one Core run make progress at the same time, and deadlines fire while a node runs. `ng.race` returns the first member to finish, and `ng.quorum` returns once the required successes are in. Python code still runs under the GIL, so CPU-bound nodes do not run in parallel; nodes overlap while they release the GIL, for example in `time.sleep`, I/O or a native call. A plain `GraphEngine` run keeps running Python node bodies on the calling thread.

Neither limit interrupts a running Python node; cancellation is cooperative. When the run's `wall_time_ms` runs out, the runtime cancels the run, and the result arrives once the running node returns, with `status` `TimedOut`, `failure.code` `P_RUNTIME_TIMEOUT` and the message `Program wall-time budget (N ms) expired`. The text of the operation that the timeout interrupted is in `failure.witness["cause_message"]`. When the deadline of `ng.await(command, timeoutMs, sourceSite)` passes, the run ends `TimedOut` with `P_AWAIT_TIMEOUT`, and a completion that arrives after the deadline is not accepted. In both cases the child Core run is cancelled; a node that checks `input.ctx.cancel_token.is_cancelled()` stops early, and one that ignores it delays the result until it returns. `TimedOut` and `Cancelled` results list the Core nodes that completed in `execution_trace`.

Program engines print no lint and no fan-out notice to stderr. The notice that recommends `EngineConfig::worker_count` comes only from a `GraphEngine` you build yourself.

### In-memory state and resume

By default `LocalProgramHost` keeps bundles, versions, activations, run records and checkpoints in memory inside the host object, and writes nothing to disk. Resume a run through the host instance that started it, while that object is alive. After a process restart, or from another `LocalProgramHost`, `resume()` raises `ng.ProgramDiagnosticError` with `P_RUN_NOT_FOUND`: `run '<id>' was not found in this LocalProgramHost; runs are kept in memory by the host that started them`. PostgreSQL Program stores are a C++ host API.

With `sqlite_directory=path` the host keeps bundles, versions, run records and checkpoints in SQLite files in that directory (`program.db`, `checkpoints.db`) and its provider calls in a provider-call journal (`provider_calls.db`), so a run survives the process: a host opened on the same directory with the same registry continues it. `resume()` continues an interrupted run, and `reconnect(run_id)` continues a run that a stopped process left running. Recovery never sends a journaled request again: the journal replays the reply, and the call is charged once. A host that binds a provider also needs `native_history_archive=` (`ng.NativeArchive.provision` the first time, `ng.NativeArchive.open` later), because the journal keeps every reply under the archive's custody. A `define()`-only Program run without child budgets holds an execution lease for its wall time: `reconnect()` raises `P_RUN_LEASE_HELD` until the lease expires, then ends the run `TimedOut` and charges the calls it had journaled. Crash recovery resumes from the run's last durable checkpoint, for example the one an interrupt left.

`resume(run_id, value)` returns a handle; call `.wait()` for the result. Without `pending_id` it resumes the run's single awaiting call. When none or several calls are awaiting, it raises `ValueError`; pass `result.interrupt.pending_input.call_id` as `pending_id` to name the call exactly. `fork(run_id, value, version, budget)` starts a new run of `version` from the run's exact checkpoint (or `checkpoint_id=`) and answers its pending call with `value`; `budget` must lie within the source run's `remaining_budget`, and `pending_id=` names the call as for `resume()`. The fork replays what the paused run finished, as `resume()` does: a node or Send that finished in the paused step is neither run nor charged again, and its provider call is not sent again (#390). When a Core step paused on several interrupts, `result.interrupt.pending_input.core_interrupt_id` (or `pending_effect.core_interrupt_id`) names the Core interrupt this pending call answers, and `resume()` answers exactly that one; the step's other interrupts ask again, one pending call at a time. With one Core interrupt it is `""` (#388). `fork()` answers exactly that interrupt as well, and the forked run asks the step's other questions again (#394). A fork of a JavaScript `main()` continues the command its source paused in, as `resume()` does: `main()` runs again with the source run's input (the fork's `input` is not used), the commands that finished replay from the source's command journal without running, sending or charging again, and the paused `callCore` resumes from its checkpoint with `value` (#395). A recorded failure the fork replays keeps its provider evidence: the fork re-binds that evidence from the source run's custody to its own, so `result.failure.provider_outcome` is restored in the fork as in the source (#398).

The `execution_trace` of a result returned by `resume()` lists only the Core nodes that ran after the resume; nodes that ran before the interrupt appear in the interrupted result. For a Core `a → b → c` that interrupts before `b`, the interrupted result has `["a"]` and the resumed result has `["b", "c"]`.

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

`HookBoundaryBlocked` carries the blocked Hook's `invocation_id` and its `state` (`"failed"`, `"timed_out"`, ...). Python tools follow the [tool failure contract](reference-en.md#tool-failure-contract): a `HookBoundaryBlocked` raised inside a tool (for example by a nested engine run) stops the run and a `NodeInterrupt` pauses it, while any other exception becomes a tool result the model reads.

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

A Python node dispatched by `ng.Send` that raises `ng.NodeInterrupt` pauses the run like any other node: `run`, `run_stream`, `run_async` and `run_stream_async` return an interrupted `RunResult` with a checkpoint, and `resume`, `resume_async` and `resume_from` run that Send with the answer while the node that emitted the Sends and the Sends that finished replay without running again. When several nodes or Sends of one step raise `ng.NodeInterrupt`, `result.interrupts` lists every one as `{"id", "node", "value"}` and `result.interrupt_id` names the first. `resume`, `resume_async` and `resume_from` take `interrupt_id=` and answer that interrupt, or the first one without it; each other paused node or Send asks its own question again on that resume, and the next resume answers it (#388). An `interrupt_id` that is not pending raises `ValueError`. `RunMetadata.resume_interrupt_id` carries the same id. The answer reaches only the node or Send it answers, in the step the resume re-enters: in every later step of the resumed run `input.ctx.resume_value`, and a tool gate's `ToolGateContext.resume_value`, are `None`, so a node or gate that asks there pauses the run again (#393). A resume after an `interrupt_before` pause gives the value to the node the gate held; one after an `interrupt_after` pause gives it to no node. When several gated nodes are ready in one step, each gate is answered on its own: the run pauses again before the next gated node until every gate of the step is answered, and each gated node then receives the answer to its own gate (#396). In one step, a node or Send that fails outranks one that asks, whatever their order: the run raises the failure and reports no interrupt; only an uncertain effect (a `NodeInterrupt` whose payload `kind` is `tool_effect_reconciliation`) still pauses beside a failure, so that it can be reconciled (#392).

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
params.message.role = a2a.Role.User
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
