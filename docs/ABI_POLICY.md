# Binary Compatibility Policy

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

## Version and loader contract

CMake reads the NeoGraph version from `pyproject.toml` and sets compiled public libraries' `VERSION` to that version and `SOVERSION` to its major component.

| Release line | Loader generation | Contract |
|---|---:|---|
| `0.x` | `0` | Pre-v1; announced rebuild boundaries can break binary compatibility. |
| `1.x` | `1` | Planned stable v1 policy; not a claim that v1 has shipped. |
| `N.x`, `N >= 2` | `N` | Major ABI boundary; rebuild consumers. |

`SOVERSION 0` gives pre-v1 packages a loader name, not interchangeable layouts. The loader cannot reject every incompatible `0.x` replacement. Read the target release notes, replace headers/libraries atomically and rebuild at every announced boundary. Do not hot-swap individual pre-v1 libraries into an existing process.

## Mandatory rebuild boundaries

- Pre-`0.9.0` to `0.9.0+`: GraphNode removed eight legacy execution virtuals. Custom nodes implement `run(NodeInput)`; SyncGraphNode is an additive adapter.
- Later pre-v1 bounded-resource, UsageAccumulator-reservation and issue #216 changes: public layouts gained accounting, admission, cache and runtime-interposition state. Rebuild against matching headers.
- Typed-provider cutover: rebuild every C++ consumer and custom provider, with matching NeoGraph and SchemaProvider SDK headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the old descriptor interpreter and Responses WebSocket path were removed without aliases.
- Event-driven provider dispatch: CancelToken uses `std::stop_source` and exposes `stop_token()`. Old inline cancellation code is not compatible merely because `cancel`, `fork` and Asio-slot signatures remain.
- Drained cancellation-context detach: CancelToken's public layout now includes generation-fenced emission state. Rebuild NeoGraph and every native consumer together. Detach releases the completed slot handler before context destruction; it still requires drained operations and does not reset cancellation state.
- A future `1.0.0` boundary changes loader generation to `1` and requires rebuilding; the generation-1 freeze is a policy for that release, not a current qualification result.

## Exported interfaces

Provider has three subclass hooks: `get_name`, `family`, `prepare(ProviderRequest)`. Common `invoke(_async)` and `dispatch(_async)` consume owned typed requests and return immutable `sp::runtime::Result`; they are not paired virtual completion overrides.

CheckpointStore retains its legacy layout for the explicit adapter migration. Sync defaults fail instead of crossing into async overrides; async defaults offload sync overrides. New async-only backends implement AsyncCheckpointStore and use `adapt_async_checkpoint_store`; synchronous capability backends implement CheckpointStoreCore and use `adapt_checkpoint_store`. Rebuild at the announced pre-v1 boundaries; this adapter design does not authorize a shared-library-only replacement.

## SDK and Python boundaries

Core requires external `SchemaProvider::runtime`, even with LLM nodes disabled. The selected SDK release is `0.1.0` alpha, interface revision `4`, shared-library ABI revision `4`, with out-of-line capability checks; this is not a stable-interface claim. Install matching SDK components together. Its `libsp_*.so.4` generation is separate from NeoGraph's loader generation and from Python's `abi3` wheel tag.

Interface 4 adds family-specific request controls and changes public request layouts. Rebuild SDK consumers, NeoGraph and Python extensions together; interface-3 headers or libraries are not interchangeable with interface 4. Native archive v3 / `spna3` and portable JSON v2 remain independent, unchanged formats. Historical interface-3 measurements do not qualify interface 4.

Python exposes the typed provider contract, including prepared handles and immutable outcomes; there is no legacy completion shim. Install the extension and its matching libraries as one wheel. Do not replace a bundled NeoGraph or SDK library individually. Graph ChatMessage convenience values do not replace native ProviderMessage custody. See [Python binding](python-binding.md).

The wheel bundles the six matching SDK runtime shared libraries, not the SDK's C++ headers or CMake package. C++ consumers install the SDK separately. Source resolution prefers an explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, then an installed package, then the public revision-pinned archive fallback. Set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` for an offline build with an installed SDK; provide its prefix through `CMAKE_PREFIX_PATH`.

## Installed names and platform limits

On Linux, a shared library has a versioned file, a major-generation SONAME link and an unversioned linker name. NeoGraph shared libraries use `$ORIGIN` for sibling dependencies. macOS `.dylib`/`@loader_path` and Windows unsuffixed `.dll` naming remain packaging conventions; they do not qualify the new SDK runtime. Static archives have no SONAME and do not remove transitive link requirements.

Recorded interface-3 SDK runtime/archive qualification covers Linux/POSIX, not interface 4. Existing macOS/Windows package metadata remains distinct from dependency qualification; no WASM runtime qualification is established. A wheel tag describes Python/ABI/platform compatibility, not proof that every runtime path was exercised. See the [PyPA tag specification](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/).

## Verification evidence

`scripts/test_find_package.sh` defines installed-consumer checks, not a pass result. Dated pre-cutover measurements remain [historical evidence](VALGRIND.md). Current NeoGraph, SDK and Python results must name the build, platform and exercised path in the integrated release report; this policy does not create new pass claims.
