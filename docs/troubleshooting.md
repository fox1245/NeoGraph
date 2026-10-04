# Troubleshooting

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## Identify the installed artifact

Record `neograph_engine.__version__`, Python version, OS/architecture, installation method and the first error. Check available wheel files for that release rather than assuming a source checkout and an older wheel expose the same APIs. The typed cutover removes legacy provider names; an old-wheel import does not test the new binding.

NeoGraph's published platform metadata includes Linux, macOS and Windows. Current SchemaProvider runtime/archive qualification is Linux/POSIX; macOS/Windows ports need their own qualification before a new wheel is claimed usable there. A compatible wheel tag or compiler alone does not prove runtime integration. WASM provider-runtime support is not established.

For a missing GLIBC symbol, compare the wheel's manylinux tag with the host glibc. A `manylinux_2_34` artifact requires glibc 2.34 or newer. For Windows DLL failures, check x64 Python and inspect missing dependency DLLs; architecture alone is not the only possible cause. Do not swap individual bundled libraries.

## Source configuration and missing dependencies

Core requires `SchemaProvider::runtime` even with `NEOGRAPH_BUILD_LLM=OFF`. NeoGraph configuration requires CMake 3.20+. Resolution prefers an explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, then an installed package, then the default revision-pinned public archive fallback. For offline configuration, install the matching SDK, set its prefix through `CMAKE_PREFIX_PATH` and set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`. SDK source builds require C++20, Python, standalone Asio, yyjson, libcurl 7.88+ and OpenSSL Crypto; NeoGraph supplies its checked-in Asio/yyjson to fetched/source integration.

The SDK alone declares no OpenSSL Crypto minimum version. NeoGraph's full HTTPS configuration and wheel/sdist path require OpenSSL 3 through the async/MCP HTTP dependencies; installing only Crypto headers is not enough for those builds.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

`Could NOT find CURL` is not fixed by disabling `NEOGRAPH_USE_LIBCURL`: that flag controls NeoGraph's optional CurlH2Pool, not the SDK's mandatory transport. Install libcurl development files (`libcurl4-openssl-dev` on Debian/Ubuntu, `libcurl-devel` on Fedora) and use a consistent toolchain/prefix.

SQLite and PostgreSQL are optional NeoGraph components. Supply their development packages or explicitly disable `NEOGRAPH_BUILD_SQLITE`/`NEOGRAPH_BUILD_POSTGRES`. This does not remove the SDK runtime. For unresolved binding symbols after a header/API change, reconfigure and rebuild all consumers against matching headers/libraries in a fresh build directory.

GCC 13 coroutine internal compiler errors were reported during Stage 3. Upgrade to a suitable compiler or restructure the reported expression; the old workarounds are not evidence that a whole SDK/platform build is qualified. Never `co_await` inside a catch handler: C++ itself prohibits await expressions there. Capture the error and await recovery outside the handler.

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

The v0.1.0–v0.1.6 ConnPool timeout caused by wheel CA paths is historical; v0.1.7 added CA auto-selection. The current provider uses SDK libcurl rather than that old path. Local HTTP admission belongs in a validated descriptor, not a raw URL override; follow [example 31](../examples/31_local_transformer.cpp). Responses WebSocket close=1000 recipes apply only to removed historical transports.

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

## Checkpoint and PostgreSQL failures

A missing PostgresCheckpointStore export can mean the wheel/source build disabled the component; inspect that artifact's configuration, not an older wheel's feature list. Install matching libpq when building the optional target. In a connection URI, percent-encode special password characters or use libpq's key=value form; never publish actual credentials.

Async connection/reconnection uses one deadline across all hosts/IPs. A positive explicit `connect_timeout=N` gives seconds, with `1` rounded to two. Absent/zero/negative values and timeouts only supplied through PGCONNECT_TIMEOUT/service files use the 30-second async default. This differs from synchronous libpq's per-host timeout.

The store creates its tables when permitted; missing CREATE privileges require applying the schema in [the PostgreSQL header](../include/neograph/graph/postgres_checkpoint.h). No pending-write capability means full super-step replay, not exactly-once external effects. An async-only custom backend uses AsyncCheckpointStore plus `adapt_async_checkpoint_store`, not the old mutual sync/async default crossover.

## Tracing adapters and historical fixes

Tracer adapters must own recorded data, not raw pointers into Span wrappers destroyed by session close. See [the C++ tracing example](../examples/49_openinference.cpp). Python contextvars do not automatically cross C++ callback boundaries; pass/attach the parent context explicitly and install wrappers before engine compilation.

Historical fixes include v0.1.8 acceptance of top-level conditional_edges, removal of the double node-execution fallback, and the pre-cutover httplib macro-layout mismatch (issue #16). They do not restore removed provider signatures. Consumers that instantiate header-only httplib across translation units still need consistent CPPHTTPLIB_OPENSSL_SUPPORT definitions; current typed-provider HTTP dispatch is SDK libcurl.

Opaque convenience-vector properties may not be Python lists. Use iteration or `list(value)` for inspection, and build-then-assign for copied sequence fields such as ChatMessage.image_urls. Do not assume mutating a returned copy changes a C++ request; the new typed request/outcome contract is separate from old CompletionParams examples.

## Reporting a bug safely

Provide the version/platform, minimal topology and call, execution_trace/status, and redacted typed failure/stop/usage evidence. State whether the failure came from an installed wheel or a rebuilt checkout. Never include credentials, private prompts, encoded native request bodies or unsanitized packet captures. Report at <https://github.com/fox1245/NeoGraph/issues>.
