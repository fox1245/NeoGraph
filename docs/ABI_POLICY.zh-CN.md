<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=zh-CN source_sha256=eadbd5019a617097b40042dc1f99f69e4ef46d890f562c028484e003fa10f5ce -->
# 二进制兼容性策略

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

本策略适用于使用已安装 NeoGraph 静态或共享库的 C++ 程序。Python wheel
会把匹配的扩展和库作为一个整体发布，不得单独替换 wheel 内的库。
typed provider 切换是强制重新编译边界，取代旧永久兼容计划。NeoGraph 保留 pre-v1 loader 命名，但不表示旧 provider 对象兼容。必须一并安装匹配的 NeoGraph 和 SchemaProvider SDK 头文件/库。SDK interface revision 3 与 `libsp_*.so.3` 是带 out-of-line capability gate 的独立 shared ABI；不稳定 package `0.0.0` 不是稳定发布。当前 SDK runtime/archive 要求 Linux/POSIX。下方 Windows/macOS 命名示例只是 packaging policy，不是新依赖 runtime 的验证证据。Python provider binding/wrapper 已延期、未移植。

## 版本约定

NeoGraph 从 `pyproject.toml` 读取项目版本。CMake 把该值设为所有公开
`neograph_*` 二进制库的 `VERSION`，并把主版本号设为 `SOVERSION`。

| 发布系列 | 加载器 ABI 代次 | 约定 |
|---|---:|---|
| `0.x` | `0` | v1 之前不保证二进制兼容。某次发布可以要求所有 C++ 使用者重新构建，但必须在 changelog 和迁移指南中明确说明边界。 |
| `1.x` | `1` | 稳定的 v1 ABI。除非另行公布特殊安全修复，minor 和 patch 发布会保持公开虚函数顺序和对象布局。 |
| `N.x`, `N >= 2` | `N` | 主版本可以引入新的 ABI 代次，并要求 C++ 使用者重新构建。 |

`SOVERSION 0` 不表示所有 `0.x` 二进制文件都可以互换。是否必须重新构建，
以目标版本的发布说明为准。

这是对 v1 之前风险的明确接受：不兼容的 `0.x` 替换仍使用 ABI 代次 0，
动态加载器无法拒绝它。升级时必须同时替换 NeoGraph 头文件和库，不得跨越
已公布的重新构建边界只热替换共享库。1.0 会冻结 ABI 代次 1 的对象布局。

## 安装名称

- Linux 安装完整版本文件、`.so.0` 兼容链接和无版本链接名，ELF SONAME
  为 `libneograph_core.so.0`。
- macOS 使用对应的 `.dylib` 名称和带主版本号的 install name。
- Windows 保持 `neograph_core.dll` 这样的无版本后缀名称。
- 共享库通过 Linux 的 `$ORIGIN` 和 macOS 的 `@loader_path` 查找同目录的
  `neograph_*` 依赖库。
- 静态库没有运行时 SONAME；遇到公布的边界时必须重新编译使用者。

## 必须重新构建的边界

| 升级 | 要求 | 原因 |
|---|---|---|
| `0.9.0` 之前版本到 `0.9.0+` | 重新构建所有 C++ 使用者和自定义节点 | `GraphNode` 删除了八个旧虚函数，vtable 已改变。 |
| `0.11.1` 或更早版本到下一版本 | 重新构建所有 C++ 使用者 | bounded runtime/transport 状态改变了 `NodeCache`、`EngineConfig`、`CompletionParams`、`Agent`、`RequestOptions`、`SseEventParser` 和 provider config 的公开对象布局；`SyncGraphNode` 本身只是新增 API。 |
| 任意 `0.x` 到 `1.0.0` | 重新构建所有 C++ 使用者 | v1 布局正式冻结，ABI 代次从 0 改为 1。 |

## 公开虚接口

- `GraphNode` 唯一正式执行虚函数是 `run(NodeInput)`。
- `Provider`: `get_name()`, `family()`, `prepare(ProviderRequest)`;
  `invoke(_async)` / `dispatch(_async)` → `sp::runtime::Result`.
  这是源码和二进制破坏性变更；所有 C++ 使用者与自定义提供方都必须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter 和 Responses WebSocket 已删除，没有 alias 或兼容 bridge。SDK 为不稳定 `0.0.0`、interface revision 3 / shared ABI 3，使用 out-of-line capability check，不表示稳定发布。当前 runtime/archive 为 Linux/POSIX，不代表 Windows、macOS、WASM runtime 已获验证。Python provider binding/wrapper 已延期，不由本 C++ 变更完成移植。
- 未来的 `CheckpointStore` 异步迁移也必须遵守本策略。v1 之后应优先新增
  独立能力接口和适配器，而不是修改稳定的对象布局。

## 验证

`scripts/test_find_package.sh` 描述 installed-consumer 检查，文件存在不代表当前已通过。当前 SDK ABI3 全量重建/CTest 已通过 26/26；shared 安装 consumer 实际执行 local HTTP 两 turn typed 请求、tool/native/refusal/known-zero 结果及 mismatch 拒绝。这不代表 NeoGraph、Python、Windows、macOS、WASM 或付费 live-provider 兼容已验证。NeoGraph 集成验证另行报告。
