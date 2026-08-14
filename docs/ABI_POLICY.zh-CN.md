<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=zh-CN source_sha256=d2a0d445bf112968279a8efd4c21953f01b3dae5bb9a5b026a821b04e12a99e9 -->
# 二进制兼容性策略

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

本策略适用于使用已安装 NeoGraph 静态或共享库的 C++ 程序。Python wheel
会把匹配的扩展和库作为一个整体发布，不得单独替换 wheel 内的库。

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
- `Provider` 根据永久兼容性决定保持现有 vtable。新实现推荐继承
  `CompletionProvider`。
- 未来的 `CheckpointStore` 异步迁移也必须遵守本策略。v1 之后应优先新增
  独立能力接口和适配器，而不是修改稳定的对象布局。

## 验证

`scripts/test_find_package.sh` 从隔离安装目录构建并运行使用者。`--shared`
还检查所有库的版本链接以及 ELF SONAME 或 Mach-O install name。CI 同时运行
静态和共享安装测试，并在 Linux、macOS 上验证共享库元数据。
