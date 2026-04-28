# Troubleshooting

Symptoms first, root causes and fixes after. If you hit something
that's not here, please open an issue with the symptom — it'll likely
go on this list afterwards.

> **The five-second sanity check.** Before anything else, confirm
> you're on the latest patch:
> ```bash
> pip install --upgrade neograph-engine
> python -c "import neograph_engine; print(neograph_engine.__version__)"
> ```
> Most issues below are fixed in a specific release. Upgrade first,
> debug second.

---

## Install / import

### `pip install neograph-engine` succeeds but `import` fails

Likely a Python-version / platform mismatch. We ship wheels for:

| Platform | Versions |
|---|---|
| Linux x86_64 (manylinux_2_34) | Python 3.9 – 3.13 |
| Linux aarch64 (manylinux_2_34) | Python 3.9 – 3.13 |
| macOS arm64 (14+) | Python 3.9 – 3.13 |
| Windows x64 (MSVC) | Python 3.9 – 3.13 |

Anything outside this matrix falls through to the sdist (source
build), which needs CMake 3.16+, OpenSSL, and a C++20 toolchain. If
your platform isn't listed and source build fails, please open an
issue.

### `ImportError: ... GLIBC_2.32 not found` on Linux

The Linux wheel is `manylinux_2_34` — needs glibc ≥ 2.34 (Ubuntu 22.04+,
Debian 12+, RHEL 9+). On older distros, build from source.

### `ImportError: DLL load failed` on Windows

The Windows wheel ships its own dependencies, but the Python install
must match the wheel architecture (x64). Confirm with:

```powershell
python -c "import platform; print(platform.architecture())"
```

If it prints `('32bit', ...)` you're on a 32-bit Python — install a
64-bit one.

---

## TLS / network

### Provider call hangs for 60 seconds and then errors with `ConnPool::async_post: timeout`

**Affected:** `neograph-engine` wheels v0.1.0 – v0.1.6.

**Root cause:** the bundled OpenSSL has compiled-in CA store paths
pointing at `/etc/pki/tls/...` (RHEL convention). On Ubuntu, Debian,
macOS, the CA store lives elsewhere (`/etc/ssl/certs/...`), so the
wheel's libssl can't verify any peer certificate and the TLS handshake
silently waits for the full request timeout before erroring.

**Fix (≥ v0.1.7):** the wheel's `__init__.py` now auto-points
`SSL_CERT_FILE` at `certifi.where()` on import. Upgrade:

```bash
pip install --upgrade neograph-engine
```

**Workaround on older wheels:**

```bash
# Debian / Ubuntu
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Cross-distro
export SSL_CERT_FILE=$(python -c "import certifi; print(certifi.where())")
```

**To opt out of the auto-fix on v0.1.7+** (e.g. you have a custom CA
bundle): set `NEOGRAPH_SKIP_CERT_AUTOFIX=1` before importing.

### `urllib` works, NeoGraph doesn't

Same root cause as above — `urllib` uses the system OpenSSL, while the
wheel uses its bundled OpenSSL with the wrong CA paths. Same fix:
upgrade to ≥ v0.1.7 or set `SSL_CERT_FILE`.

### WebSocket Responses (`use_websocket=True`) closes immediately with `close=1000`

Three common causes, in order of frequency:

1. **WebSocket access not enabled on your API key / org.** Some OpenAI
   tier 1 accounts don't have WebSocket-mode access yet. Fall back to
   HTTP/SSE by setting `use_websocket=False`.
2. **Missing `User-Agent` header on certain proxy paths.** Fixed in
   commit `d7c61d0`. Upgrade to ≥ v0.1.4.
3. **`temperature` field rejected by some Responses-API models.** Same
   commit removes it from the WS handshake on supported models.

### CORS errors when running from a browser via WASM

The WASM build doesn't yet implement bypass headers for browser-CORS.
Track [Issue #wasm-cors](../../issues) for status.

---

## Graph compile / run

### `RuntimeError: Unknown reducer: <name>`

Two reducers ship with the binding: `"overwrite"` and `"append"`.
Anything else fails to compile. Custom reducers require building from
source and registering via `ReducerRegistry::register_reducer(name, fn)`
in C++. A Python registration hook is on the v0.2 roadmap.

If you typed `"last_value"` (a common LangGraph alias) — that's
`"overwrite"` here. Same semantics, different name.

### `RuntimeError: Unknown condition: <name>`

Built-in conditions: `has_tool_calls`, `route_channel`. Other names
are custom and must be registered in C++ via
`ConditionRegistry::register_condition(name, fn)`.

### `RuntimeError: Write to unknown channel: <name>`

The channel name in your `ChannelWrite` doesn't match anything in
`definition["channels"]`. Channel names are exact; `messages` and
`Messages` are different.

### `RuntimeError: Unknown node type: <name>`

The `type` field of one of your nodes references something not in the
factory registry. For built-ins (`llm_call`, `tool_dispatch`,
`intent_classifier`, `subgraph`) the type names are spelled out above.
For your own types, you must call
`ng.NodeFactory.register_type(type_name, factory)` BEFORE compile.

### My ReAct loop only runs once — `execution_trace == ['llm']`

**Affected:** `neograph-engine` wheels v0.1.0 – v0.1.7.

**Root cause:** the graph compiler dropped the top-level
`conditional_edges` block silently. Both the README quickstart and
every Python example use this form, so ReAct loops degenerated to a
single LLM call (no tool dispatch).

**Fix (≥ v0.1.8):** the compiler now accepts both forms — top-level
`conditional_edges` array OR inline-in-`edges` with a `condition`
field. Upgrade and verify with:

```python
result = engine.run(...)
print(result.execution_trace)
# Expected for ReAct: ['llm', 'dispatch', 'llm']
```

**Workaround on older wheels:** put the conditional inline:

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "llm",
     "condition": "has_tool_calls",
     "routes": {"true": "dispatch", "false": ng.END_NODE}},
]
# (no separate conditional_edges block)
```

### `result.execution_trace` is empty / shows only the start node

The graph routed to `__end__` immediately. Most common causes:

1. **Missing edge from `__start__`.** Every graph needs at least one
   `{"from": ng.START_NODE, "to": "..."}` edge.
2. **Conditional returned a value not in the `routes` map.** When the
   condition's return value doesn't match any key, the engine takes
   the lexicographically-last entry as a fallback. If that maps to
   `__end__`, you exit silently. Always include a default branch.
3. **`max_steps=0` or `max_steps=1`** — the run hit the ceiling
   immediately. Default is 25; ReAct loops typically need 10+.

### Compile error: `RuntimeError: Cycle detected: a -> b -> a`

NeoGraph allows cycles (ReAct loops are cycles), but the compiler
catches *unconditional* cycles — `a → b → a` with no conditional
escape. Add a conditional edge that can route to `__end__`.

---

## Performance

### Fan-out is slower than I expected

Two common causes:

1. **`worker_count` mismatch.** Default is `hardware_concurrency()`.
   For Send fan-out of width N, `engine.set_worker_count(N)` (or more)
   is required for true parallelism. Smaller worker counts serialize
   the fan-out branches.
2. **Python custom nodes hold the GIL** during their body. If your
   `@ng.node` function does CPU-bound Python work, fan-out won't speed
   up. ONNX / PyTorch / numpy / `requests.get` release the GIL during
   native calls, so they DO parallelize. For pure Python scoring loops,
   it doesn't matter how many workers you set.

### `set_worker_count(1)` made things slower than before

**Affected:** wheels v0.1.4 – fixed in `fd60aab` (post-v0.1.4 master).

**Root cause:** `set_worker_count(1)` allocated a 1-worker thread pool
that paid ~75 µs/iter for cross-thread submission. The pool was a
no-op at N=1 but not actually a no-op.

**Fix:** v0.1.5+ — `set_worker_count(1)` now uses a `nullptr` pool
(direct sync execution, no submission cost).

### `bench_neograph par` reports 200+ µs

**Default behaviour since v0.1.4.** The default `worker_count` flipped
to `hardware_concurrency()` to better match real LLM workloads (where
fan-out width is usually > 1). The pre-flip 11.8 µs `par` is reachable
in one line:

```python
engine.set_worker_count(1)
```

This isn't a regression — it's a tuning knob you can flip.

### My streaming callback fires twice per node

**Affected:** Python `@ng.node` write-only nodes. Fixed in
`re-agent` commits `2a5c5dc` / `5993935` and replicated in NeoGraph
master.

**Root cause:** pure-write `GraphNode` subclasses (no `Command`, no
`Send`) ran `execute()` once for the result and once for the stream
hook. Override `execute_full_stream` (or `execute_full_stream_async`)
to dedup.

If you're using the `@ng.node` decorator (not subclassing), this is
already handled.

---

## Checkpoints / Postgres

### `PostgresCheckpointStore` not found / import error

The PyPI wheels ship with `PostgresCheckpointStore` enabled (libpq is
bundled since v0.1.3). `import neograph_engine; neograph_engine.PostgresCheckpointStore`
should work directly.

If you built from source without `-DNEOGRAPH_BUILD_POSTGRES=ON`, the
class won't exist in the binding. Re-run CMake config with the flag
set, then rebuild.

### Postgres connection: `FATAL: password authentication failed`

The `PostgresCheckpointStore` connection string follows libpq:

```
postgresql://user:password@host:port/dbname
```

If your password contains URL-special chars (`@`, `:`, `/`, `%`), URL-encode
them — or use the `key=value` form:

```
host=localhost user=neo password=p@ss dbname=neograph
```

### Postgres `relation "neograph_checkpoints" does not exist`

The store creates its tables on first use (`CREATE TABLE IF NOT EXISTS`).
If your DB user doesn't have CREATE rights, run the schema by hand —
the SQL is in [`include/neograph/graph/postgres_checkpoint.h`](../include/neograph/graph/postgres_checkpoint.h)
under `kSchema`.

---

## Examples / docker

### `docker compose run agent` for example 26 fails to find PG

The compose file expects a `db` service to be reachable as
`postgres://neograph:neograph@db:5432/neograph`. If you're outside
docker-compose, set `PG_URL` to your reachable host instead. See
[`examples/26_postgres_react_hitl/README.md`](../examples/26_postgres_react_hitl/README.md)
for the full env table.

### Crawl4AI example refuses to start

Crawl4AI is an optional Docker container:

```bash
docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
    unclecode/crawl4ai:latest
```

Examples 17, 25, 26 fall back gracefully when `CRAWL4AI_URL` (default
`http://localhost:11235`) isn't reachable.

### `example_clay_chatbot` build target not found

Example 11 requires `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` at CMake
configure time:

```bash
cmake -B build -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON ..
make example_clay_chatbot
```

It pulls Clay (UI layout) + Raylib (renderer) — that's why it's behind
a flag.

---

## Streaming events

### `event.node` raises `AttributeError`

The attribute is `event.node_name` (matches the C++ field name). Same
for `event.type` (the enum) and `event.data` (the JSON dict).

```python
def cb(event):
    print(f"{event.type.name} on {event.node_name}: {event.data}")
```

### My `StreamMode.TOKENS` callback never fires

The provider must support streaming. Currently:

| Provider | Streaming? |
|---|---|
| `OpenAIProvider` | ✓ HTTP/SSE |
| `SchemaProvider("openai_responses")` | ✓ SSE |
| `SchemaProvider("openai_responses", use_websocket=True)` | ✓ WS |
| `SchemaProvider("claude")` | ✓ SSE |
| Custom Python `Provider` subclass | depends on your `complete_stream` impl |

If you're using a custom provider, override `complete_stream_async` or
the engine falls back to non-streaming `complete_async` and your
TOKENS callback won't fire.

---

## OpenTelemetry

### My OTel spans appear with `parent_id=None` (4 separate traces instead of 1)

**Affected:** `neograph_engine.tracing` before commit `9073671`.

**Root cause:** `tracer.start_span` + `use_span(...).__enter__()`
relies on contextvars, which don't propagate across the
C++ → Python pybind callback boundary.

**Fix:** The `otel_tracer` helper now snapshots the parent context via
`set_span_in_context(root_span)` and passes it explicitly to each
child node's `start_span`. Upgrade past `9073671`.

If you're rolling your own OTel integration, do the same: don't rely
on contextvars across the binding boundary.

---

## Build from source

### CMake configure: `Could NOT find SQLite3` on Windows

The Windows wheel build sets `-DNEOGRAPH_BUILD_SQLITE=OFF` because
SQLite isn't ABI-compatible across MSVC runtimes. If you're building
from source on Windows for your own use, either install SQLite via
vcpkg or pass `-DNEOGRAPH_BUILD_SQLITE=OFF` explicitly.

### CMake configure: `Could NOT find CURL` on Linux

Optional dependency. Install via your package manager:

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev
# RHEL / Fedora
sudo dnf install libcurl-devel
# macOS
brew install curl
```

Or disable: `-DNEOGRAPH_USE_LIBCURL=OFF`. Without libcurl,
`SchemaProvider`'s `prefer_libcurl=True` mode (HTTP/2) is unavailable
— the default ConnPool (HTTP/1.1) still works.

### Pybind binding fails to link with undefined references

You're likely re-running `make` after pulling new code without re-running
CMake. The build dir's compiled object files reference symbols from
older headers. Either `make clean && make` or delete and reconfigure
the build directory.

---

## Reporting a bug

If your symptom isn't above:

1. Run `pip install --upgrade neograph-engine` first — many issues are
   patch-level fixes.
2. Capture the minimum reproducer:
   - graph definition
   - node types in use
   - the exact `engine.run(...)` call
   - the `result.execution_trace` and (if streaming) the events you saw
3. Note your platform, Python version, and `neograph_engine.__version__`.
4. Open an issue at <https://github.com/fox1245/NeoGraph/issues>.

If the bug shows up only against a specific LLM endpoint, please also
include the wire-level shape (`example_responses_envelope` for OpenAI
Responses; `tcpdump`/`wireshark` for raw HTTP traces if relevant).
