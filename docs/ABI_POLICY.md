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
- `ChatTool::strict`: the portable tool definition gained a member. `ChatTool` crosses the library boundary by value in `Tool::get_definition()` and `make_provider_request`, so rebuild NeoGraph, every native consumer and custom `Tool`, and Python extensions together.
- A future `1.0.0` boundary changes loader generation to `1` and requires rebuilding; the generation-1 freeze is a policy for that release.

## Exported interfaces

Provider has three subclass hooks: `get_name`, `family`, `prepare(ProviderRequest)`. Common `invoke(_async)` and `dispatch(_async)` consume owned typed requests and return immutable `sp::runtime::Result`; they are not paired virtual completion overrides.

CheckpointStore retains its legacy layout for the explicit adapter migration. Sync defaults fail instead of crossing into async overrides; async defaults offload sync overrides. New async-only backends implement AsyncCheckpointStore and use `adapt_async_checkpoint_store`; synchronous capability backends implement CheckpointStoreCore and use `adapt_checkpoint_store`. Rebuild at the announced pre-v1 boundaries; this adapter design does not authorize a shared-library-only replacement.

## SDK and Python boundaries

Core requires external `SchemaProvider::runtime`, even with LLM nodes disabled. The selected SDK release is `0.3.2` alpha, public `maint/interface6` commit `91fb993e50e49e3b03d290b170c83cf57afbc518`, interface revision `6`, shared-library ABI revision `6`, with out-of-line capability checks; this is not a stable-interface claim. Install matching SDK components together. Its `libsp_*.so.6` generation is separate from NeoGraph's loader generation and from Python's `abi3` wheel tag.

Interface 6 adds fixed-size provider-reported monetary metadata to public usage layouts and uses one async pipeline for blocking and async SDK calls. The selected commit also grows `sp::OpenRouterRouting`, which gained `max_price`, and the Chat, Messages and Responses request layouts within revision 6; its capability bit `OpenRouterRoutingMaxPrice`, part of `RequiredProvider` and required by NeoGraph's handshakes, refuses an earlier interface-6 library. Rebuild SDK consumers, NeoGraph and Python extensions together; earlier headers or libraries are not interchangeable with interface 6. Blocking wrappers continue rejecting SDK event-loop and callback-worker re-entry. Native archive v3 / `spna3` and portable JSON v2 remain independent, unchanged formats. Canonical outcome v2 usage adds optional `provider_cost` only when monetary metadata is present; absence-only historical records retain their original bytes and do not fabricate costs.

Python exposes the typed provider contract, including prepared handles and immutable outcomes; there is no legacy completion shim. Install the extension and its matching libraries as one wheel. Do not replace a bundled NeoGraph or SDK library individually. Graph ChatMessage convenience values do not replace native ProviderMessage custody. See [Python binding](python-binding.md).

The wheel bundles the seven matching SDK runtime shared libraries, including the curl-free wire layer, not the SDK's C++ headers or CMake package. C++ consumers install the SDK separately. Source resolution prefers an explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, then an installed package, then the public revision-pinned archive fallback. Set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` for an offline build with an installed SDK; provide its prefix through `CMAKE_PREFIX_PATH`.

## Installed names and platform limits

On Linux, a shared library has a versioned file, a major-generation SONAME link and an unversioned linker name. NeoGraph shared libraries use `$ORIGIN` for sibling dependencies. macOS `.dylib`/`@loader_path` and Windows unsuffixed `.dll` naming remain packaging conventions. Static archives have no SONAME and do not remove transitive link requirements.

WASM runtime integration is not established. A wheel tag describes Python, ABI and platform compatibility only. See the [PyPA tag specification](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/).

## Verifying an installation

`python scripts/verify_ci.py install --work-dir <fresh-output> --jobs 2` runs the installed-consumer checks. Use `--shared`, `--core-only`, or `--program --quickjs` for the corresponding platform-supported consumer row. The driver works from a checkout or extracted sdist without Git or Bash.
