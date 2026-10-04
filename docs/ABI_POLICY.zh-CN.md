<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=zh-CN source_sha256=e17b6ed782cd2ef13aa7091cb02ea0f5fa2459df3e1f670ef014fd3c9bc9b07d -->
# 二进制兼容政策

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

## version 与 loader 契约

CMake 从 `pyproject.toml` 读取 NeoGraph version，将公开 compiled library 的 `VERSION` 设为完整 version，`SOVERSION` 设为 major 部分。

| 发布系列 | loader 代际 | 契约 |
|---|---:|---|
| `0.x` | `0` | pre-v1；公告的 rebuild 边界可能破坏二进制兼容。 |
| `1.x` | `1` | 计划中的 stable v1 政策，不表示 v1 已发布。 |
| `N.x`, `N >= 2` | `N` | major ABI 边界，须 rebuild consumer。 |

`SOVERSION 0` 给 pre-v1 package 一个 loader 名，不表示 layout 可互换。loader 无法拒绝所有不兼容的 `0.x` 替换。阅读目标 release note，一并替换 header/library，在公告的边界 rebuild。不要在运行的 process 中 hot-swap 单个 pre-v1 library。

## 必须 rebuild 的边界

- pre-`0.9.0` 到 `0.9.0+`：GraphNode 删除了八个旧执行 virtual。custom node 实现 `run(NodeInput)`；SyncGraphNode 是新增 adapter。
- 后续 pre-v1 bounded-resource、UsageAccumulator 预留、issue #216 变更：公开 layout 增加 accounting、admission、cache、runtime-interposition 状态。使用匹配 header rebuild。
- typed-provider 切换：用匹配的 NeoGraph/SchemaProvider SDK header/library rebuild 所有 C++ consumer 和 custom provider。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、旧 descriptor interpreter、Responses WebSocket 已删除，没有 alias。
- event-driven provider dispatch：CancelToken 使用 `std::stop_source` 并公开 `stop_token()`。`cancel`、`fork`、Asio-slot signature 不变不使旧 inline cancel code 兼容。
- 将来 `1.0.0` 边界把 loader 代际变为 `1`，须 rebuild。generation-1 freeze 是该 release 的政策，不是当前验证结果。

## 公开接口

Provider 的 subclass hook 为 `get_name`、`family`、`prepare(ProviderRequest)`。通用 `invoke(_async)`、`dispatch(_async)` 消费 owned typed request，返回不可变 `sp::runtime::Result`；不是 virtual completion override 对。

CheckpointStore 为显式 adapter 迁移保留 legacy layout。sync 默认实现失败，不转入 async override；async 默认实现 offload sync override。新 async-only backend 实现 AsyncCheckpointStore 并用 `adapt_async_checkpoint_store`；sync capability backend 实现 CheckpointStoreCore 并用 `adapt_checkpoint_store`。在公告的 pre-v1 边界 rebuild；adapter 设计不授权只替换 library。

## SDK 与 Python 边界

即使禁用 LLM node，Core 也要求外部 `SchemaProvider::runtime`。选定的 SDK release 是 `0.1.0` alpha，interface revision `4`、shared-library ABI revision `4`，有 out-of-line capability check；不表示 stable interface。一并安装匹配 component。`libsp_*.so.4` 代际与 NeoGraph loader 代际、Python `abi3` wheel tag 都不同。

Interface 4 添加各 family 的 request control，并改变公开 request layout。须一起 rebuild SDK consumer、NeoGraph 和 Python extension；interface-3 header/library 不能与 interface 4 混用。Native archive v3 / `spna3` 和 portable JSON v2 是独立格式，保持不变。历史 interface-3 测量不验证 interface 4。

Python 公开含 prepared handle 与不可变 outcome 的 typed provider 契约，没有 legacy completion shim。extension 与匹配 library 作为一个 wheel 安装。不要单独替换 bundled NeoGraph/SDK library。graph ChatMessage 便利值不替代 native ProviderMessage custody。参见 [Python binding](python-binding.md)。

wheel 包含六个匹配的 SDK runtime shared library，不包含 SDK C++ header 或 CMake package。C++ consumer 须单独安装 SDK。source resolution 依次使用显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、installed package、公开 revision-pinned archive fallback。用 installed SDK 离线构建时设 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`，通过 `CMAKE_PREFIX_PATH` 提供 prefix。

## 安装名称与平台限制

Linux shared library 有 versioned file、major-generation SONAME link 和 unversioned linker 名。NeoGraph shared library 对 sibling 依赖使用 `$ORIGIN`。macOS `.dylib`/`@loader_path` 和 Windows 无后缀 `.dll` 是 packaging 规则，不验证新 SDK runtime。static archive 没有 SONAME，也不移除 transitive link 要求。

已记录的 interface-3 SDK runtime/archive 验证覆盖 Linux/POSIX，不验证 interface 4。已有 macOS/Windows metadata 与依赖验证不同，WASM runtime 验证尚未确立。wheel tag 描述 Python/ABI/platform 兼容，不证明所有 runtime 路径已执行。参见 [PyPA tag 规范](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/)。

## 验证证据

`scripts/test_find_package.sh` 定义 installed-consumer 检查，不是通过结果。有日期的切换前测量保留为[历史证据](VALGRIND.md)。当前 NeoGraph/SDK/Python 结果须在集成 release report 中说明 build、platform、执行路径；本政策不产生新的通过声明。
