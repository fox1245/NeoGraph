# Binary Compatibility Policy

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

This policy applies to C++ consumers of installed NeoGraph static and shared
libraries. Python wheel users receive the matching extension and libraries as
one package and must not replace individual bundled libraries.

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
| `0.11.1` or earlier to the next release containing bounded `NodeCache` | Rebuild all C++ consumers. | `NodeCache` and `EngineConfig` public object layouts changed. `SyncGraphNode` itself is additive and does not change the `GraphNode` vtable. |
| Any `0.x` build to `1.0.0` | Rebuild all C++ consumers. | The supported v1 layouts are frozen and the loader ABI generation changes from 0 to 1. |

Never copy a new shared library over an existing pre-v1 installation without
also reading the target release notes. Install headers and libraries from the
same release, and rebuild custom subclasses at every announced boundary.

## Exported Virtual Interfaces

- `GraphNode` has one canonical virtual execution entry,
  `run(NodeInput)`. `SyncGraphNode` is a separate additive adapter.
- `Provider` keeps its established vtable under the permanent compatibility
  decision. New implementations should derive from `CompletionProvider`; that
  migration does not alter the existing `Provider` layout.
- The planned `CheckpointStore` async migration must follow this policy.
  Before v1, any vtable break requires an announced rebuild boundary. After
  v1, new capability interfaces and adapters must be preferred over changing
  the stable `CheckpointStore` layout.

## Verification

`scripts/test_find_package.sh` builds and runs a consumer from an isolated
install prefix. Its `--shared` mode also checks every installed NeoGraph
library's version links and ELF SONAME or Mach-O install name. CI runs both
static and shared installed-consumer checks; platform shared jobs cover Linux
and macOS metadata.
