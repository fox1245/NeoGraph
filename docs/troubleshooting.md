# Troubleshooting

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## Identify the installed artifact

Record `neograph_engine.__version__`, Python version, OS/architecture, installation method and the first error. Check available wheel files for that release rather than assuming a source checkout and an older wheel expose the same APIs. The typed cutover removes legacy provider names; an old-wheel import does not test the new binding.

NeoGraph's published platform metadata includes Linux, macOS and Windows. A compatible wheel tag or compiler alone does not prove runtime integration. WASM provider-runtime support is not established.

For a missing GLIBC symbol, compare the wheel's manylinux tag with the host glibc. A `manylinux_2_34` artifact requires glibc 2.34 or newer. For Windows DLL failures, check x64 Python and inspect missing dependency DLLs; architecture alone is not the only possible cause. Do not swap individual bundled libraries.

## Source configuration and missing dependencies

Core requires `SchemaProvider::runtime` even with `NEOGRAPH_BUILD_LLM=OFF`. NeoGraph configuration requires CMake 3.20+. Resolution prefers an explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, then an installed package, then the default revision-pinned public archive fallback. For offline configuration, install the matching SDK, set its prefix through `CMAKE_PREFIX_PATH` and set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`. SDK source builds require C++20, Python, standalone Asio, yyjson, libcurl 7.88+ and OpenSSL Crypto; NeoGraph supplies its checked-in Asio/yyjson to fetched/source integration.

The SDK alone declares no OpenSSL Crypto minimum version. NeoGraph's full HTTPS configuration and wheel/sdist path require OpenSSL 3 through the async/MCP HTTP dependencies; installing only Crypto headers is not enough for those builds.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

The build tree is not an installed package. Before `PYTHONPATH=build python -c "import neograph_engine"`, install the wheel's Python dependencies with `pip install 'pydantic>=2' certifi`: the import fails without pydantic, and without certifi TLS uses the host's trust store.

`Could NOT find CURL` is not fixed by disabling `NEOGRAPH_USE_LIBCURL`: that flag controls NeoGraph's optional CurlH2Pool, not the SDK's mandatory transport. Install libcurl development files (`libcurl4-openssl-dev` on Debian/Ubuntu, `libcurl-devel` on Fedora) and use a consistent toolchain/prefix.

SQLite and PostgreSQL are optional NeoGraph components. Supply their development packages or explicitly disable `NEOGRAPH_BUILD_SQLITE`/`NEOGRAPH_BUILD_POSTGRES`. This does not remove the SDK runtime. For unresolved binding symbols after a header/API change, reconfigure and rebuild all consumers against matching headers/libraries in a fresh build directory.

GCC 13 coroutine internal compiler errors have been reported. Upgrade to a suitable compiler or restructure the reported expression. Never `co_await` inside a catch handler: C++ itself prohibits await expressions there. Capture the error and await recovery outside the handler.

## Removed provider APIs and typed failures

`CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor-interpreter configuration, `prefer_libcurl` and Responses WebSocket selection are gone. Use `SchemaProvider(load_provider_descriptor(descriptor_json), ProviderRuntimeOptions(...), SchemaProviderDefaults(...))` and `make_provider_request`. See [Python binding](python-binding.md) for exact constructors and [migration](migration-v0.4-to-v1.0.md) for C++.

`prepare` returns a single-use `PreparedProviderRequest`; `dispatch` consumes it. `invoke` combines both. Never rebuild an admitted request after persisting its dispatch receipt or retry a consumed handle. A failure outcome is data, with genuine error/partial evidence; inspect `outcome.failure` instead of reading it as a success-shaped completion.

Observer exceptions retain the typed outcome and cause in `ProviderObserverError`. Budget settlement and terminal receipt persistence errors also retain genuine outcomes; that evidence does not authorize another send. Unknown delivery needs reconciliation, not a blind retry.

## Streaming and Python async boundaries

Set `ProviderMode.Stream` explicitly. An observer does not select streaming. Filter nonempty content-text deltas for a token display; usage, reasoning, tool, raw-wire and envelope events are not tokens. C++ event views are borrowed during callbacks; Python ProviderEvent owns copied borrowed data. Avoid slow observers, which consume the host delivery capacity.

```python
import asyncio
import neograph_engine as ng

text = ng.Text()
text.value = "Hello"
request = ng.make_provider_request(
    provider, model, [ng.ProviderMessage(ng.ProviderRole.User, [text])],
    mode=ng.ProviderMode.Stream,
)
request.on_event = observe
outcome = await asyncio.to_thread(provider.invoke, request)
if outcome.failure is not None:
    handle_failure(outcome.failure)
else:
    handle_completion(outcome.completion)
```

Provider invoke/dispatch are synchronous Python methods that release the GIL. Use `asyncio.to_thread`; they are not native asyncio awaitables. Callback invocation, copy and destruction acquire the GIL. Cancellation uses the graph CancelToken and forwards stop to the SDK; a timeout or cancelled waiter does not prove the remote effect never happened.

Keep full `ProviderMessage` values for continuation, including tool/refusal/reasoning parts. `ChatMessage` and `ToolCall` remain graph convenience values, not aliases for full provider history. Missing usage is `None`, not zero; known-zero counters remain evidence-bearing UsageCount values. A portable outcome projection cannot import native replay or financial authority. Persist native continuation only with admitted NativeArchive custody.

## TLS and local endpoints

Set `ProviderRuntimeOptions.ca_file` for an explicit trust bundle. Python import preserves an existing `SSL_CERT_FILE`, otherwise selects certifi when available; runtime options pass the selected CA file to SDK libcurl. `NEOGRAPH_SKIP_CERT_AUTOFIX=1` leaves host selection untouched. Do not disable certificate verification to hide an endpoint or trust-store error.

Local HTTP admission belongs in a validated descriptor, not a raw URL override; follow [example 31](../examples/31_local_transformer.cpp).

## Graph definitions and registration

Unknown reducer/condition/node-type diagnostics mean the name is absent from the registry used to compile. Built-in reducers are `overwrite` and `append`; register custom reducers, conditions and node factories before compile. `has_tool_calls` and `route_channel` are built-in conditions. Registered Python callbacks run under the GIL.

```python
import neograph_engine as ng

ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
ng.ConditionRegistry.register_condition("is_long",
    lambda state: "long" if len(state.get("messages") or []) > 10 else "short")
```

A write must name an existing channel exactly. Conditions return route labels: open conditions may use an explicit `default`; without it an unmatched label throws, and closed conditions reject labels outside their declared set. Ensure an edge from `__start__` and an escape route for loops. `RunConfig.max_steps` defaults to `50`; inspect step-limit status rather than treating truncation as ordinary completion.

With `schema_version: 1`, strict topology parsing rejects unknown/unconsumed keys and round-trip loss. Use `_`/`x-` annotations for metadata, nonempty `wait_for` on barriers, and `routes` on conditional edges. Export the live schema with `ng.export_schema()` after custom registration; do not maintain a separate editor palette. Legacy absent/zero versions remain lenient on the `0.x` path; `ng.upgrade_topology()` retains ignored data in collision-safe annotations.

## Fan-out and administration

The default worker_count is `1`, so no engine-owned pool is created. I/O branches still overlap when they suspend; CPU-bound bodies on a single caller thread serialize. Configure `set_worker_count(N)` or `set_worker_count_auto()` before concurrent execution if CPU work benefits from a pool. Python CPU-bound callbacks still serialize under the GIL unless a native operation releases it.

An admin state/history/update/fork call during any run/resume on the same engine fails with `std::logic_error`. Cancel/drain and await completion first; do not suppress the exception. Same-thread-id concurrent executions have unspecified checkpoint interleaving, and separate engines sharing a store need host coordination. See [concurrency](concurrency.md).

## Python Program errors

These come from `LocalProgramHost` and the QuickJS Program compiler. The rules behind them are in [Program and QuickJS](python-binding.md#program-and-quickjs).

| Error | Cause | Fix |
|---|---|---|
| `TypeError: get(): incompatible function arguments` from a node | `GraphState.get` takes only the channel name | Use `state.get("value") or 0` |
| `ng.ProgramRegistryError` from `add_registered_node`, `add_registered_reducer` or `add_registered_condition` | The node type, reducer or condition is not registered in this process | Register it first with `@ng.node`, `ng.NodeFactory.register_type`, `ng.ReducerRegistry.register_reducer` or `ng.ConditionRegistry.register_condition` |
| `P_JS_DEFINE_MISSING` | The module does not export `define()` | Export `define()` and return the Program's one graph |
| `P_JS_GRAPH_ENTRY` or `P_JS_GRAPH_ARGUMENT` for a node named `__start__` or `__end__` | The graph has no `.entry()`, or a node uses a reserved name | Call `.entry()`, and rename the node; `.entry()` and `.exit()` connect the sentinels |
| `P_CORE_E13` or `P_CORE_E14` | A node has a static edge or a second conditional edge next to its first conditional edge; neither ever fires | Put every target in the first conditional edge's routes |
| `P_JS_CONTROL_COMMAND`: JavaScript command references a Core outside the admitted root | `ng.callCore` names a graph other than the one `define()` returns | Use the name passed to `ng.graph(...)` |
| `ValueError: Every policy budget ceiling except child budgets must be positive; zero: ...` | A listed ceiling field is zero | Make it positive, even if the Program never calls a model |
| `P_ADMIT_PROVIDER` in `ng.ProgramAdmissionError` | An `llm_call` node requires a provider the host does not bind, or an `llm_call` or `intent_classifier` node was registered without `provider=` | Register `add_provider(...)` and `add_registered_node("llm_call", ..., provider=...)`, and pass `node_context=` and `binding_label=` to `LocalProgramHost` |
| `P_MONEY_BUDGET` (`BudgetExhausted`), or another `P_MONEY_*` code (`Failed`), and the provider saw no request | The bound call's reservation does not fit `monetary_microunits` (0 grants nothing), or the call cannot be bounded: unknown model or origin, router alias without a route ceiling, no output cap, no known input limit, or a feature the price catalog does not model | Raise `monetary_microunits`, set `provider_controls.max_output_tokens`, or use a model with a catalog entry; see [Pricing evidence and the price catalog](reference-en.md#pricing-evidence-and-the-price-catalog) |
| `P_START_BUDGET`: `Program invocation budget <field>=<n> is outside its admitted bounds` | The run budget differs from the budget given to `compile_admit` | Pass the same budget, or compile and admit again |
| `P_CHILD_BINDING` from `ng.spawn` | `LocalProgramHost` has no child-binding resolver | Run child Programs in a C++ host |
| `P_RUN_NOT_FOUND`: `run '<id>' was not found in this LocalProgramHost` from `resume()` | The run belongs to another or a destroyed `LocalProgramHost` | Resume through the host that started the run |
| `P_RUN_LEASE_HELD` from `reconnect()` | A `define()`-only Program started without child budgets holds an execution lease for its `wall_time_ms`, so that no other runtime continues a run whose owner may still be alive | Retry after `retry_after_ms` in the diagnostic's witness; the run then ends `TimedOut`. To continue a crashed run from its last checkpoint, use `main()` or start the Program with child budgets (`max_child_depth` and `max_total_children` above zero) |
| `P_FORK_SOURCE_BUDGET` from `fork()` | The fork's budget takes all the `wall_time_ms` or all the `max_core_steps` the paused run has left, so the run could not resume | Give the fork a share of `paused.remaining_budget`, for example half; `max_concurrency` may stay whole |
| `P_CORE_BARRIER_STALLED` | A barrier member took a route that does not lead to the join, so the join can no longer fire | Route every member to the join or to `__end__`; `P_CORE_E15` warns about such routes at compile time |
| `P_RUNTIME_TIMEOUT`: `Program wall-time budget (N ms) expired` | The run used up `wall_time_ms`; the interrupted operation's text is in `failure.witness["cause_message"]` | Compile and admit again with a larger `wall_time_ms` |

A failed `callCore` ends the run with that failure; `main()` cannot catch it with `try`/`catch`. Check `result.status` and `result.failure` in Python.

## Checkpoint and PostgreSQL failures

A missing PostgresCheckpointStore export can mean the wheel/source build disabled the component; inspect that artifact's configuration, not an older wheel's feature list. Install matching libpq when building the optional target. In a connection URI, percent-encode special password characters or use libpq's key=value form; never publish actual credentials.

Async connection/reconnection uses one deadline across all hosts/IPs. A positive explicit `connect_timeout=N` gives seconds, with `1` rounded to two. Absent/zero/negative values and timeouts only supplied through PGCONNECT_TIMEOUT/service files use the 30-second async default. This differs from synchronous libpq's per-host timeout.

The store creates its tables when permitted; missing CREATE privileges require applying the schema in [the PostgreSQL header](../include/neograph/graph/postgres_checkpoint.h). No pending-write capability means full super-step replay, not exactly-once external effects. An async-only custom backend uses AsyncCheckpointStore plus `adapt_async_checkpoint_store`, not the old mutual sync/async default crossover.

## Tracing adapters and other pitfalls

Tracer adapters must own recorded data, not raw pointers into Span wrappers destroyed by session close. See [the C++ tracing example](../examples/49_openinference.cpp). Python contextvars do not automatically cross C++ callback boundaries; pass/attach the parent context explicitly and install wrappers before engine compilation.

Consumers that instantiate header-only httplib across translation units need consistent CPPHTTPLIB_OPENSSL_SUPPORT definitions; typed-provider HTTP dispatch uses SDK libcurl.

Opaque convenience-vector properties may not be Python lists. Use iteration or `list(value)` for inspection, and build-then-assign for copied sequence fields such as ChatMessage.image_urls. Do not assume mutating a returned copy changes a C++ request.

## Tool calls, Hooks and protocol errors

A graph run whose tool call, node or subgraph hits a mandatory Hook block throws `HookBoundaryBlocked` itself, as `llm::Agent` does, rather than a `NodeExecutionError` wrapping it, and emits no `ERROR` event for it. Catch `HookBoundaryBlocked` (Python: `ng.HookBoundaryBlocked`) and read `invocation_id` and `state`. A Python node that raises one, for example from a nested engine run, is not retried. See the [tool failure contract](reference-en.md#tool-failure-contract).

An engine's `HookRuntime` also runs the `BeforeToolExecution` and `AfterToolExecution` Hooks of its tool calls, and a Program host's Hook runtime those of its Cores' tool calls. If a graph run stops with `HookBoundaryBlocked` state `failed` and the message `lifecycle hook could not run`, give the run an owner scope (`RunMetadata::owner_scope`): a Hook boundary without one fails closed, for checkpoint Hooks as for tool Hooks.

`state == "timed_out"` means that a Hook's deadline elapsed or its target timed out. A Hook whose last attempt failed reports `failed`.

A tool result with status `expired` and error `run deadline elapsed before the tool started` means the run's deadline had passed before the call could start: the tool and its Hooks did not run. Raise `RunMetadata::deadline` (Python `timeout_ms`, Program `wall_time_ms`) rather than retrying the call. An MCP call that the run's deadline or the client's request timeout stops is `timed_out`, and cancelling the run stops an MCP call in flight at once.

`MCPServer` sends no response for a `tools/call` the client cancelled, on any transport, answers a Hook block with `-32010` and an interrupt with `-32011`; every other failure is an `isError` result whose `_meta["io.neograph/failure"].code` names it, and its text is a public message, never the exception text (raise `NodeFailureError` with a `public_message` to choose the text). A gRPC client must read the status, not `RunGraphResponse.error` of an `OK` response: a failed run is a non-OK status whose `google.rpc.ErrorInfo` carries the code in `metadata["code"]`. See [Protocol failure mapping](reference-en.md#protocol-failure-mapping).

Tool output or an exception message that is not valid UTF-8 arrives with U+FFFD in place of each ill-formed sequence, not as an empty tool message, an empty MCP or gRPC payload, or a `UnicodeDecodeError` in Python.

## Reporting a bug safely

Provide the version/platform, minimal topology and call, execution_trace/status, and redacted typed failure/stop/usage evidence. State whether the failure came from an installed wheel or a rebuilt checkout. Never include credentials, private prompts, encoded native request bodies or unsanitized packet captures. Report at <https://github.com/fox1245/NeoGraph/issues>.
