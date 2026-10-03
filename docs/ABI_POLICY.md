# Binary Compatibility Policy

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

This policy applies to C++ consumers of installed NeoGraph static and shared
libraries. Python wheel users receive the matching extension and libraries as
one package and must not replace individual bundled libraries.
The typed provider cutover is a mandatory recompile boundary, not the earlier permanent-compatibility plan. NeoGraph retains its pre-v1 loader naming; this does not make old provider objects compatible. Install matching NeoGraph headers/libraries and SchemaProvider SDK headers/libraries atomically. SDK interface revision 3 and `libsp_*.so.3` are a separate shared ABI with out-of-line capability gates; unstable package `0.0.0` is not a stable release. The SDK runtime/archive currently requires Linux/POSIX. Historical Windows/macOS naming examples below are packaging policy, not proof that the new dependency runs there. Python provider bindings/wrappers are deferred and not ported.

## Version Contract

NeoGraph reads its project version from `pyproject.toml`. CMake applies that
value as `VERSION` and its major component as `SOVERSION` to every compiled
public `neograph_*` library.

| Release line | Loader ABI generation | Contract |
|---|---:|---|
| `0.x` | `0` | Pre-v1. Binary compatibility is not guaranteed. A release may require every C++ consumer to rebuild, but the boundary must be announced in the changelog and migration guide. |
| `1.x` | `1` | Stable v1 ABI. Minor and patch releases preserve public virtual ordering and object layout unless an exceptional security fix is announced. |
| `N.x`, `N >= 2` | `N` | A major release may introduce a new ABI generation and requires rebuilding C++ consumers. |

`SOVERSION 0` does not claim that all `0.x` binaries are interchangeable. It
gives pre-v1 packages a deliberate loader name while the release notes remain
the authority for mandatory rebuild boundaries.

This is an explicit pre-v1 risk acceptance: the dynamic loader cannot reject an
incompatible `0.x` replacement because both files use ABI generation 0. Package
upgrades must replace NeoGraph headers and libraries atomically, and operators
must not hot-swap a pre-v1 shared library across an announced rebuild boundary.
Version 1.0 ends this exception by freezing the generation 1 layouts.

## Installed Names

- Linux installs a full file such as `libneograph_core.so.0.11.1`, a
  compatibility link `libneograph_core.so.0`, and an unversioned linker name.
  The ELF SONAME is `libneograph_core.so.0`.
- macOS installs the equivalent `.dylib` names and records the major-version
  install name.
- Windows keeps unsuffixed names such as `neograph_core.dll`; package version
  metadata records the release and ABI policy.
- Installed NeoGraph shared libraries find sibling `neograph_*` dependencies
  through `$ORIGIN` on Linux and `@loader_path` on macOS.
- Static archives have no runtime SONAME. Consumers must recompile whenever the
  headers or release notes declare a rebuild boundary.

## Mandatory Rebuild Boundaries

| Upgrade | Requirement | Reason |
|---|---|---|
| Any pre-`0.9.0` build to `0.9.0+` | Rebuild all C++ consumers and custom nodes. | `GraphNode` removed eight legacy virtual methods and changed its vtable. |
| `0.11.1` or earlier to the next release | Rebuild all C++ consumers. | Public layouts changed for bounded runtime/transport state, including `NodeCache`, `EngineConfig`, `CompletionParams`, `Agent`, `RequestOptions`, `SseEventParser`, and provider configuration. `SyncGraphNode` itself is additive and does not change the `GraphNode` vtable. |
| `0.11.1` or earlier to the release containing bounded `UsageAccumulator` reservations | Rebuild all C++ consumers. | `UsageAccumulator` gained public reservation accounting state and its object layout changed. |
| Any earlier pre-v1 build to the release containing issue #216 | Rebuild all C++ consumers. | `GraphEngine` gains atomic execution/administration admission state and `EngineConfig` gains per-node cache policies and runtime interposition; exported class/value layouts change, although legacy method signatures remain. |
| Any `0.x` build to `1.0.0` | Rebuild all C++ consumers. | The supported v1 layouts are frozen and the loader ABI generation changes from 0 to 1. |

Never copy a new shared library over an existing pre-v1 installation without
also reading the target release notes. Install headers and libraries from the
same release, and rebuild custom subclasses at every announced boundary.

## Exported Virtual Interfaces

- `GraphNode` has one canonical virtual execution entry,
  `run(NodeInput)`. `SyncGraphNode` is a separate additive adapter.
- `Provider`: `get_name()`, `family()`, `prepare(ProviderRequest)`;
  `invoke(_async)` / `dispatch(_async)` → `sp::runtime::Result`.
  This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is unstable `0.0.0`, interface revision 3 / shared ABI 3, with out-of-line capability checks; that is not a stable release claim. Current runtime/archive support is Linux/POSIX; no Windows, macOS or WASM runtime qualification is implied. Python provider bindings/wrappers are deferred and not ported by this C++ change.
- `CheckpointStore` retains its existing vtable and object layout for the
  pre-v1 migration. Sync defaults now fail explicitly instead of crossing to
  async overrides; async defaults offload synchronous overrides. Async-only
  subclasses must migrate to `AsyncCheckpointStore` and
  `adapt_async_checkpoint_store()` for a sync facade. New capability interfaces
  and adapters do not change the legacy vtable or checkpoint wire format.
  Rebuild custom backends with matching headers at the next announced boundary;
  do not hot-swap a pre-v1 shared library into an existing process.

## Verification

`scripts/test_find_package.sh` describes installed-consumer checks; its existence is not a current pass claim. The current SDK ABI3 full rebuild/CTest passed 26/26, and the shared installed consumer exercised real local HTTP two-turn typed requests, tool/native/refusal/known-zero outcomes and mismatch rejection. These results do not qualify NeoGraph, Python, Windows, macOS, WASM or paid live-provider compatibility. NeoGraph integrated verification is reported separately.
