<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=zh-CN source_sha256=5ecfa5a3968c2ebfb4344b913e7b9be42f04485f90940d647b0d693b1f9d129a -->
# 二进制兼容性策略

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

## 版本与加载器契约

CMake 从 `pyproject.toml` 读取 NeoGraph 的版本，把已编译的公共库的 `VERSION` 设为该版本，`SOVERSION` 设为其主版本号。

| 发布线 | 加载器代数 | 契约 |
|---|---:|---|
| `0.x` | `0` | v1 之前；已公布的需要重新构建的边界，可能破坏二进制兼容性。 |
| `1.x` | `1` | 计划中的稳定 v1 策略；并不表示 v1 已经发布。 |
| `N.x`，`N >= 2` | `N` | 主版本 ABI 边界；使用方需要重新构建。 |

`SOVERSION 0` 只是给 v1 之前的包一个加载器名称，并不意味着各版本的内存布局可以互换。加载器无法拒绝所有不兼容的 `0.x` 替换。请阅读目标版本的发布说明，原子地替换头文件和库，并在每个已公布的边界处重新构建。不要把 v1 之前的某个库单独热替换到已有的进程里。

## 必须重新构建的边界

- 从 `0.9.0` 之前到 `0.9.0+`：GraphNode 移除了八个旧的执行虚函数。自定义节点要实现 `run(NodeInput)`；SyncGraphNode 是新增的适配器。
- 此后、v1 之前的有界资源、UsageAccumulator 预留和 issue #216 相关的变更：公共类型的内存布局增加了核算、准入(admission)、缓存和运行时拦截的状态。请针对匹配的头文件重新构建。
- 类型化 provider 切换：每个 C++ 使用方和自定义 provider 都要重新构建，并使用匹配的 NeoGraph 与 SchemaProvider SDK 头文件和库。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、旧的描述符解释器和 Responses WebSocket 路径都已移除，没有保留别名。
- 事件驱动的 provider 分发：CancelToken 使用 `std::stop_source`，并提供 `stop_token()`。不能因为 `cancel`、`fork` 和 Asio 槽的签名没变，就认为旧的内联取消代码仍然兼容。
- 排空后的取消上下文分离：CancelToken 的公共布局现在包含带代数隔离（generation-fenced）的发射状态。请把 NeoGraph 和每个原生使用方一起重新构建。分离操作会在销毁上下文之前，释放已完成的槽处理器；它仍然要求操作已排空，也不会重置取消状态。
- `ChatTool::strict`：可移植的工具定义增加了一个成员。`ChatTool` 在 `Tool::get_definition()` 和 `make_provider_request` 中是按值跨越库边界的，所以要把 NeoGraph、每个原生使用方、自定义 `Tool` 以及 Python 扩展一起重新构建。
- 将来的 `1.0.0` 边界会把加载器代数改为 `1`，并要求重新构建；第 1 代的冻结是针对该版本的策略，不是当前的资格认证结果。

## 导出的接口

Provider 有三个供子类重写的钩子：`get_name`、`family` 和 `prepare(ProviderRequest)`。通用的 `invoke(_async)` 和 `dispatch(_async)` 消耗持有所有权的带类型请求，并返回不可变的 `sp::runtime::Result`；它们不是成对的、可重写的 completion 虚函数。

CheckpointStore 为了显式的适配器迁移而保留旧的布局。同步的默认实现会直接失败，而不是转去调用异步的重写；异步的默认实现则会把同步的重写交给 worker 去做。新的、只支持异步的后端实现 AsyncCheckpointStore，并使用 `adapt_async_checkpoint_store`；同步的能力后端实现 CheckpointStoreCore，并使用 `adapt_checkpoint_store`。在已公布的 v1 之前的边界处要重新构建；这种适配器设计并不意味着可以只替换共享库。

## SDK 与 Python 的边界

即使禁用了 LLM 节点，Core 仍然需要外部的 `SchemaProvider::runtime`。选定的 SDK 版本是 `0.3.0` alpha，公开的 `maint/interface6` 提交 `f7ed8161fbcf39a6bf9358f78cfdcfda133537a2`，接口修订版 `6`，共享库 ABI 修订版 `6`，并带有非内联的 capability 检查；这不表示接口已稳定。请把匹配的 SDK 组件一起安装。它的 `libsp_*.so.6` 代数，与 NeoGraph 的加载器代数、Python 的 `abi3` wheel 标签，都是各自独立的。

接口 6 在公共的 usage 布局里，增加了定长的、由 provider 上报的金额元数据，并让阻塞和异步的 SDK 调用共用同一条异步流水线。选定的提交还在修订版 6 之内，扩大了 `sp::OpenRouterRouting`（新增了 `max_price`）以及 Chat、Messages 和 Responses 的请求布局；它的 capability 位 `OpenRouterRoutingMaxPrice` 属于 `RequiredProvider`，NeoGraph 的握手要求具备它，因此更早的接口 6 库会被拒绝。SDK 的使用方、NeoGraph 和 Python 扩展要一起重新构建；更早的头文件或库与接口 6 不能互换。阻塞式包装器仍然拒绝从 SDK 的事件循环和回调 worker 中重入。native archive v3 / `spna3` 和可移植 JSON v2 是各自独立、保持不变的格式。规范的 outcome v2 usage，只有在存在金额元数据时才增加可选的 `provider_cost`；只是缺失该字段的历史记录保留原有字节，不会伪造成本。更早的接口 3/4/5 的测量值保持它们原来的适用范围，不能作为接口 6 的资格认证。

Python 暴露了带类型的 provider 契约，包括已准备的句柄和不可变的 outcome；没有保留旧的 completion 兼容层。扩展和与之匹配的库要作为同一个 wheel 安装。不要单独替换捆绑在其中的某个 NeoGraph 或 SDK 库。图使用的 ChatMessage 便捷值，不能替代 native ProviderMessage 的托管。参见 [Python 绑定](python-binding.md)。

wheel 捆绑了七个与之匹配的 SDK 运行时共享库（包括不依赖 curl 的 wire 层），但不含 SDK 的 C++ 头文件和 CMake 包。C++ 使用方需要另行安装 SDK。查找 SDK 源码的顺序是：优先使用显式指定的 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`，其次是已安装的包，最后是公开的、固定了修订版本的归档。使用已安装的 SDK 做离线构建时，设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`，并通过 `CMAKE_PREFIX_PATH` 给出它的前缀。

## 安装后的名称与平台限制

在 Linux 上，共享库有一个带版本号的文件、一个对应主版本代数的 SONAME 链接，以及一个不带版本号的链接器名称。NeoGraph 的共享库用 `$ORIGIN` 查找同目录的依赖。macOS 的 `.dylib`/`@loader_path` 和 Windows 不带后缀的 `.dll` 命名，仍然只是打包约定；它们不能证明新的 SDK 运行时合格。静态库没有 SONAME，也不会消除传递性的链接要求。

记录在案的接口 3 的 SDK 运行时和 archive 资格认证覆盖 Linux/POSIX，不覆盖接口 6。现有的 macOS/Windows 包元数据与依赖的资格认证是两回事；WASM 的运行时资格认证尚未确立。wheel 标签描述的是 Python/ABI/平台的兼容性，并不证明每条运行路径都被测试过。参见 [PyPA 的标签规范](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/)。

## 验证证据

`python scripts/verify_ci.py install --work-dir <fresh-output> --jobs 2` 定义的是对已安装使用方的检查，它本身不是通过结果。选用对应的、该平台支持的使用方配置时，加上 `--shared`、`--core-only` 或 `--program --quickjs`。这个驱动脚本可以在源码检出或解压后的 sdist 中运行，不需要 Git 或 Bash。切换之前带日期的测量值仍然是[历史证据](VALGRIND.md)。当前 NeoGraph、SDK 和 Python 的结果，必须在集成的发布报告里写明构建、平台和实际测试过的路径；本策略不产生新的通过声明。
