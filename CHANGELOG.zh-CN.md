<!-- neograph-i18n: source=CHANGELOG.md locale=zh-CN source_sha256=bc98ab5033035a0bd02800d7fe9df6a25c7050a7c3f5a56effae5d40b5e80143 -->
# 变更日志

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

NeoGraph 的所有显著变更均记录在本文件中。

格式遵循 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)。版本控制遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

---

## [未发布]

## [0.12.0] - 2026-08-23

### 已添加
- **严格的运行时插桩完成。** 新增了 `StrictRuntimeProfile`、持久的提供方终端结果回执和 SQLite schema v3 迁移、带有 JSON-RPC stdio/HTTP 工件发布的传输无关强制 Hook 后端、通用必需上下文和 `HardConstraint` 工件、精确保留的 `ContextTransformReceipt`、持久的运行时开发者指令决策，以及编译前预留的 `ProgramSynthesisGateway`。生成的源码仍无法自行激活、绑定、迁移或生成；这些仍是由主机拥有的独立 Program 转换。`DurableProviderDispatchReceiptStore` 的自定义子类现在必须实现终端 `settle`/`outcome`；C++ 使用者必须针对此 ABI 变更重新构建。

- **隔离的 PostgreSQL Program 存储集成装置。** 新增摘要固定的、仅回环的 `tests/fixtures/q7-postgres/compose.yaml`，带有 tmpfs 存储和健康门控。当 `NEOGRAPH_TEST_POSTGRES_URL` 指定一个可抛弃的测试数据库时，`ProgramCatalogTest.PostgreSQLProgramStoreReopensActivationAndOwnerVisibility` 覆盖发布、激活、重新打开和所有者隔离。这是测试基础设施，不是 Q7 最终证明快照。

- **失败时关闭的 QuickJS 遗留数据排空审计。** 新增了 `scripts/audit_legacy_drain.py` 及其 CTest 契约。该工具从显式枚举的冻结 Program/测试装备存储快照生成规范化的、内容寻址的证明；它拒绝未知或可变记录、未分类的遗留来源、仅排空记录，以及活动或可恢复的遗留运行。带有活动 `-wal`、`-shm` 或 `-journal` sidecar 的 SQLite 输入在打开前即被拒绝，因此活动 WAL 数据库的原始副本不能作为最终证明。 这确立了 Q7 证据机制，但并不声称部署特定的最终排空或遗留解析器删除已完成。

- **PostgreSQL 终排空归档扫描。** 遗留排空审计器现在接受冻结的 `program_postgres_dump` 自定义归档，并且仅以仅数据、严格表、脚本输出模式调用 `pg_restore`；它从不恢复到位止。它根据持久化的身份验证 Program 包、版本和激活表，拒绝缺失或更改任何必需表的归档，并将旧版 Program 版本的激活视为最终移除的阻塞项。

- **无部署的 Q7 最终证明模式。** 遗留排空审计器仅在从未存在预发布或生产 NeoGraph 部署时才接受命名的操作员认证。该模式既不接受存储目标也不接受历史遗留工件，将生成的证明标记为 `evidence_mode: "no_deployment_attestation"`，并对任何混合或未认证的空库存采取失败时关闭。它无法覆盖已排空、删除、丢失或无法访问的历史状态。

- **OpenRouter 提供程序路由。** `OpenAIProvider` 现在将对象在 `CompletionParams::extra_fields.provider` 传递到 Chat Completions 请求体中作为 `provider`；非对象值在发出 HTTP 请求前失败。这公开了 OpenRouter 文档化的按调用路由首选项，同时保持其他原生 `extra_fields` 键被忽略。live Beast cookbook 固定其提供程序，并使用显式的 180 秒超时来实现其 4,000 token 的生成预算。

- **Copy Ninja 本地图节点桥接。** 新增了无传输的 `a2a::CopyNinjaNode`，它包装了一个单独物化的 Copy Ninja 工装，读取 `prompt`，并覆盖 `response`。 新增了 `cookbook_the_beast_copy_ninja` 实时食谱：其 LLM 只能编写此固定本地节点，必须通过正常 Core 门控之后的第四道本地绑定门控，并且如果合成源智能体观察到 RPC 则失败。 卡片文本、端点、凭据以及源仍然排除在未获准入(admission)的候选之外，并且调用方提示永远不会进入编写 LLM 请求。


- **可选的 Program 组件边界。** 新增了选择加入的 `NEOGRAPH_BUILD_PROGRAM` 开关、导出的 `neograph::program` 目标以及 `<neograph/program/program.h>` 入口点。 安装的软件包组件发现现在仅在构建时报告 Program；仅安装 Core 时保留现有的 `neograph::core` 链接接口。

- **不可变的Program值模型。** 新增了稳定的类型化诊断、深度拥有的规范JSON和C++构建器 `ProgramSource` 输入，以及不可变的内容寻址 `ProgramBundle`/`ProgramVersion` 值、规范序列化、带 SHA-256 算法标签的身份、source maps、导入，以及严格版本化的存储值模式。 `neograph::program` 现在是一个已编译的导出库，同时仅依赖 Core。Bundle/版本 v1 投影现在要求 sealed Core 定义和计划身份、语义化版本的可执行摘要、契约、闭包、边界以及类型化的准入(admission)/物化收据。它们的身份绑定格式和存储版本，语义集使用稳定顺序，diagnostics 拒绝无效指针、逆转的 spans 和未知枚举，而当没有可用的精确偏移时，解析器 spans 保持缺失。

- **密封 Program 准入(admission)闭包。** 添加了不可变的 `RegistrySnapshot`、`AdmissionProfile` 和 `PolicySnapshot` 值，具有构建时可调用捕获、严格规范清单、域分离指纹，以及在 `ProgramVersion` 中的失败时关闭跨指纹验证。Core 现在为 Program 物化公开了显式命名的仅本地解析/链接/验证入口点；现有的本地优先/全局回退重载保持不变。注册表现在记录规范精确的可执行依赖边，用于传递准入(admission)闭包，并且仅本地条件检查覆盖遗留键边文档，无需查询进程全局注册表。

- **单根 `call_core` Program 编译器。** 新增了`ProgramCompiler`，它仅接受封闭的 Program-v1 信封，在执行密封前执行纯局部 Core 解析/往返/语义验证，并发出带有 RFC 6901 指针和 source-map 归属的聚合类型化诊断。编译过程推导出规范 Program、注册表、传递可执行闭包、能力/效应、导入 Merkle、密封定义以及 Core plan 身份，而无需调用工厂或可调用对象。所编写的文档 schema、完整有限预算合同、零分派拒绝测试以及静态和共享已安装消费者覆盖范围随编译器一起提供。Core 获得了新增的全量解析/往返及局部验证报告，而遗留的 throwing API 保留其现有行为。

- **固定 Program 运行时垂直切片。** 添加了 `ProgramCatalog`、`EngineGenerationCache`、`ProgramRuntime`、共享的 `ProgramHandle`、不可变的 `ProgramResult`、类型化 Program 事件信封、内存中的 `ProgramStore`，以及仅追加的 CAS `ProgramJournal`。准入(admission)在物化之前重新计算不受信任的包语义；每次尝试固定一个不可变的 Core 代并调用现有的 `GraphEngine` 异步路径。运行时执行现在将完成、中断、精确检查点恢复、取消、超时、Core 步骤耗尽、检查点不兼容和失败映射为类型化终止状态，同时保留不可续用预算和检查点谱系。日志提交先于检查点/终止事件投递，并发恢复只有一个 CAS 胜出者，并且 PR6 切片在 Core 代理存在之前拒绝有副作用或非空模式的 Program。

- **QuickJS 控制语言前端。** 添加了可选加入的 `NEOGRAPH_BUILD_QUICKJS_CONTROL`、密封的 `ProgramSource::from_javascript(...)`，以及一个私有的仅编译 QuickJS 上下文。源信封固定引擎/语言/宿主 API 版本；其唯一的 `ng` 宿主表面是一个版本化图构建器，内存、栈和中断轮询限制失败时关闭。JavaScript 产生一个不可变的 `call_core` Program 计划，并且永远不会成为运行时 VM、字节码工件或 Core 依赖。

- **A2A Agent Card 兼容性候选。** 添加了一个单请求、未认证、无重定向的 well-known 卡片收集器，以及一个仅工厂的不可变候选编译器。候选仅保留摘要固定的来源、有界协议事实和安全技能 ID；自由格式卡片文本、广告的 RPC 端点、提供商/安全配置和凭据被排除。Copy Ninja PoC 额外要求独立观察到的行为固定到该摘要，并且绝不派发源智能体。

- **SQLite Harness 记录存储（issue #147 后续）。** 为 WAL 支持、模式版本化的工件/运行持久化添加了可选的 `neograph::mcp_sqlite` 目标和 `SqliteHarnessRecordStore`，具有不可变工件和运行到工件绑定。Harness MCP 二进制 现在将记录存储在 `runs.db` 中，而检查点仍保留在 `checkpoints.db` 中。
- **AMD OpenMP 目标卸载概念验证。** 添加了可选加入的 `bench_openmp_offload` 基准测试，它在相同的数值 fan-out 工作负载上比较串行 CPU 执行、OpenMP 自动线程化、每迭代 GPU 映射和持久 GPU 数据。它报告真实设备与主机回退执行、正确性、包含传输的延迟、仅内核延迟和加速比。`NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201` 为 Radeon AI PRO R9700 启用 ROCm/Clang 设备镜像。


### Revised translation: 变更

- **C++ ABI 和 SOVERSION 策略（issue #194）。** 每个编译的公共 `neograph_*` 库现在携带项目 `VERSION` 和主 `SOVERSION`；安装的共享库从其自身目录解析兄弟依赖。Pre-v1 版本使用 ABI 代 0，但可能声明强制重建边界。所有针对 `0.11.1` 或更早版本构建的 C++ 消费者必须为下一个版本重建，因为 `NodeCache`、`EngineConfig`、`CompletionParams`、`Agent`、`RequestOptions`、`SseEventParser` 和提供商配置公共布局已更改。包含有界 `UsageAccumulator` 预留的版本是另一个强制重建边界：其公共对象布局现在携带预留记账状态。版本 1.0 将 ABI 代更改为 1 并冻结受支持的 v1 布局。CI 现在构建并运行隔离的静态和共享已安装消费者，并检查 ELF/Mach-O 加载器元数据。参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)。
- **`GraphNode::run(input)` 迁移指南完成。** Python `GraphNode` 基类不再引用已删除的 `execute*` 方法；当 `run(input)` 缺失时，它引发包含迁移文档路径的 `NotImplementedError`。C++/Python 参考、异步/流式指南和示例 README 已与实际 v0.9.0 单一入口点对齐。迁移过程在 [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md) 中记录了 C++ 和 Python 示例。
- **Provider API 永久兼容性策略（issue #5）。** 计划移除的 `Provider::complete()`、`complete_async()`、`complete_stream()`、`complete_stream_async()` 和基于回调的 `invoke()` 已被撤销，`[[deprecated]]` 警告已被移除。现有 API 继续接收兼容性和安全修复。新的 Provider 实现和直接调用者分别建议使用 `CompletionProvider::do_invoke()` 和 `invoke_request(CompletionRequest)`；将所有新功能回移植到现有 API 不保证。公共签名、虚拟顺序、对象大小和 vtable 保持不变。

### 移除

- **弃用的 TransformerCPP 集成示例。** 移除了 `example_inproc_gemma`、`NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE` 和 `TRANSFORMERCPP_DIR`，它们依赖于一个不再可用的外部托管仓库。使用标准 OpenAI 兼容本地服务器的 `example_local_transformer` 被保留。

### 已修复

- **补全 Python Program wheel 依赖。** PyPI 构建现会启用 QuickJS 控制运行时，在扩展模块旁捆绑 `neograph_program` 加载器，并导出 Windows 上跨 Program/Harness DLL 边界使用的私有求值器。这样可防止 `P_JS_UNAVAILABLE`、因缺少加载器导致的 import 失败，以及共享构建中的 `LNK2019` 失败。命令 identity 的构建也会避免跨组件 dylib 边界的嵌套 initializer-list JSON 复制，从而防止在 Program 执行期间可能发生的 macOS arm64 崩溃。
- **有界远程传输和凭据来源。** HTTP/1.1、HTTP/2、SSE 和 WebSocket 接收路径现在在分配不受信任大小之前强制执行保守的响应、头、块、行、帧、握手和消息限制。重定向的 POST 请求仅在规范化同源内被跟随，提供商凭据需要 TLS，除非启用了显式数字回环开发异常，并且 WebSocket 调试输出不再包含请求头或负载。
- **QuickJS `all` 加入启动竞争。** 完成处理器现在在关闭 JavaScript 加入之前等待初始成员启动注册。立即完成的子项不能再在其兄弟初始或替换命令派发之前恢复生成器；重复的运行时回归覆盖两条路径。
- **Harness 聚合发现的溯源（issue #174）。** 详情现在包含一个 `finding_sources` 数组，与现有的扁平 `findings` 数组对齐。每个条目记录其聚合索引、源 worker ID 和 worker 本地索引，同时不改变经 schema 验证的 worker 输出或既有的 `findings` 形状。
- **Harness 导出结果 lint（issue #173）。** 节点效果契约现在可以在可选的 `exports` 数组中声明写入通道，当调用者在图执行后消费它们时。因此，Harness 编译和 `GraphEngine` 运行时验证都为真正仅写入的通道保留 E6，而不会在 `final_result` 上产生误报。
- **MCP 2025-11-25 工具客户端契约现代化(issue #147 M0)。** 初始化现为幂等操作并保留协商后的服务器元数据;HTTP 工具复用发现会话;`/mcp` 端点构造由请求和通知共享;工具发现遵循不透明游标;JSON-RPC 代码/数据、完整工具元数据、非文本内容、`structuredContent`、`isError` 和 `_meta` 在 C++ 和 Python 路径中均得以保留。新增可配置的 HTTP 超时/静态/动态标头、输出模式验证、严格响应 ID 检查,以及类型化的 `InitializeResult`、`ToolDefinition`、`ListToolsPage` 和 `CallToolResult` API。SSE 检测现使用 `Content-Type`,而非将包含 `data:` URL 的 JSON 错误分类。
- **每任务取消状态和已发布发射生命周期安全。** `GraphEngine::run`、`run_async`、`run_stream`、`run_stream_async` 每次运行从调用方提供的父级创建一个执行子级，仅将该子级绑定到内部 `co_spawn`/同步桥，并将同一子级作为 `RunContext` 传递。因此，在单个父级下取消所有并发运行不会覆盖彼此的取消槽。分叉的执行子级通过已发布的发射保留现有的 `shared_ptr` 所有权，防止引擎工作完成与发射执行之间的释放后使用。由取消引起的 asio `operation_aborted` 被传播为 `CancelledException`，而不是可重试的节点错误。`CancelToken` 0.11.x 对象布局和内联/仅头文件行为不变。已编译的 C++ 使用者需要重新编译才能获取更新的 `fork()` 生命周期行为。仅替换共享库可保持对象布局兼容性，但嵌入在消费者二进制文件中的现有内联函数体不会改变。然而，当外部代码在它直接创建的令牌上调用 `bind_executor()` 时，调用方仍负责保持令牌存活，直到执行器的已发布工作完成。
- **PostgreSQL 异步连接全局超时策略已记录。** 异步初始连接和替换在所有主机/IP 地址上使用单一超时。直接写入正向连接字符串中的显式 `connect_timeout` 强制执行,最小值为 2 秒;未指定、为零、为负或仅由环境变量/服务文件提供的值使用操作安全默认值 30 秒。这与 libpq 的按主机同步超时有意不同;同步创建/替换行为保持不变。
- **JARVIS 模拟构建修复（问题 #130）。** 修复了 `cookbook_jarvis` 编译失败，原因是当音频依赖缺失时 `MicCapture` 仍为不完整类型。添加了 `NEOGRAPH_JARVIS_FORCE_MOCK`，使 ASan CI 始终构建模拟配置，无论运行器安装的包如何。会话运行器现在使用实际的 CMake 输出路径和专家目标名称，并正确启动现有的 `demo_mcp_server.py`。
- **节点失败上下文保留（问题 #123）。** C++ 执行错误作为`NodeExecutionError`传播，包含原始`exception_ptr`、失败节点名称和尝试次数；终端`ERROR`事件也记录相同上下文。在 Python 中，原始异常对象、类型、参数、用户属性和回溯按原样保留，仅添加`.node_name`和`.attempts`属性。`NodeInterrupt`、取消和内存不足异常遵循现有控制流，不被包装。

### 修复（文档）

- **从 Provider 手册中移除了被忽略的每节点提示(issue #116)。** 修复了三个 Python 示例,这些示例描述使用 `config.system` 的多角色行为,而内置 `llm_call` 不会读取该行为。每个示例已重写为使用 `NodeContext.instructions` 的严格单调用图,相关 README 已与实际行为对齐。
- **保留字`RunContext::deadline` 文档更正（问题 #115）。** 修复了文档和 Doxygen 注释，这些内容将`deadline` 和 `trace_id` 呈现为可使用的每次运行元数据，而它们无法通过`RunConfig` 设置，也未在 Python 中暴露。
- **`GraphNode::run` 示例签名修复（问题 #129）。**修复了公共头文件示例接受`const NodeInput&`（按引用传递）而未能覆盖实际按值虚拟函数的问题，并通过编译时测试锁定了按值约定，以满足协程参数生命周期要求。

### 已添加

- **向后兼容的 Provider 迁移路径。** 新的`CompletionRequest` 将流式模式与回调存在分离，`CompletionProvider` 要求新实现仅写入`do_invoke()`。现有的`Provider` vtable、四个旧虚拟函数、基于回调的`invoke()` 和 Python `complete()` 子类契约均保留。

- **Python 持久化后端** (#117) — `Store` 和 `CheckpointStore` 现在可作为可构造的子类基类，并具有 C++ 到 Python 的虚拟分发。`StoreItem`、`CheckpointPhase`、`Checkpoint` 和 `PendingWrite` 以 JSON 形状的字段暴露；检查点待写入方法保持可选。
- **Python 同步取消** (#119) — Python 调用方可以构造一个 `CancelToken`，将其赋值给 `RunConfig.cancel_token`，并从另一个线程协作地停止 `engine.run()`。

- **Python 检查点历史** (#118) — `GraphEngine.get_state_history()` 以最新优先的顺序暴露检查点记录，使调用者能够在从历史状态分叉之前检查父链接、元数据、步骤和 ID。

- **DSL 表面（细化层）+ 模式演化门** (#75 M4)。
  - **Elaborator**：`vars`（`{"$var":...}` / `${...}` 插值，无环强制）/ `templates`+`use`（精确参数匹配强制，节点前缀重命名——包括局部引用、屏障和路由；通道是共享状态，因此它们全局合并）/ `when` 条件包含。**非图灵完备且完全**：每个DSL文档在有限时间内规范化到唯一核心，并且相对于该核心是幂等的。所有错误均报告DSL源代码坐标（`use[2].args`，`vars.model`），并包含源映射（输出位置 → 生成语法）。锁文件工作流：`./example_elaborate harness.dsl.json > harness.json`（示例53）。
  - **`GraphCompiler::upgrade_to_latest()`**：无损 v0→v1 机械转换——严格拒绝的键被隔离到 `x-upgraded-<key>` 注释命名空间（零数据删除），空屏障被显式移除。整个语料库经过测试，保证“旧版宽松编译 IR == 升级后严格编译 IR”（规范等价，版本戳除外）。
  - **Schema 演进门**：针对 `tests/fixtures/schema_snapshot.json` 基线的仅添加子集判定（JSON Subschema 族的可判定子集）——节点类型/属性/reducers/条件的移除、必需集增加、封闭条件标签更改以及效果契约更改均导致测试失败 = CI 合并阻断。不兼容更改强制在同一审查提交中进行版本升级 + 升级器 + 快照重新生成。

- **PBT / 增量验证框架**（#75 M3）。300 种子确定性拓扑生成器（来自 schema 包络的有效严格文档，自检测功能覆盖率——当 conditional_edges/barrier/interrupt 出现率低于 30% 时测试失败：未测试的功能成为失败，而非静默空洞）。
  - **突变检测**：在 300 种子语料库上确认，翻译验证捕获了所有 5 种丢弃类型（conditional_edges/edge/barrier/interrupt/channel）的所有应用 + 3 种错误接线类型（路由折叠 / 边重定向 / 节点重命名 = 丢弃+伪造平衡）。应用率下限（10% 的种子）也已断言。
  - **参考解释器增量**：一个独立模型，从代码不相交的实现重新实现文档化的超步语义（goto 抢占、屏障累积、字典序回退、隐式 __end__），与 Scheduler 在 12 步 × 300 图上进行比较（DESIL 教训：仅靠验证器无法捕获错误执行）。
  - **Engine ↔ Studio 共享语料库**：`tests/fixtures/topology_corpus/` 15 个变体（3 个有效 + 12 个违反 E3–E11）与 NeoGraph-Studio `tests/corpus/` 字节相同，两者断言相同的判定（代码:严重性多重集）——两个实现不能静默分歧。

- **GraphValidator — 拓扑静态语义检查（E3–E11 + 效果）**（#75 M2）。解析（M1）与执行之间的传递层。在严格文档（schema_version>=1）中，错误是编译失败，警告是 stderr lint；在宽松文档中，仅错误级诊断作为 stderr 警告浮现（对现有图零噪音）。判定哲学 = 检查器健全性优先：只有那些在引擎语义下永远不可能正确的事物才是错误（悬空引用 E3、无信号路径的屏障 E8——goto 绕过屏障记账因此不可恢复、空路由 E10——分发将解引用 rend() UB、未声明通道写入 E4——运行时确认抛出）；Command.goto/Send 可以证明合理的事物是警告（可达性 E7、无逃逸循环 E11、无屏障的普通 fan-in E9、覆盖竞争 E5、死通道 E6）。每个诊断都附带机器可读的见证（反例）JSON——用于 Studio 画布高亮（M3）。
  - **路由完备性（E10）**：`ConditionSpec` 标签契约引入。通过 `register_condition` 3 参数重载声明条件的输出标签集要求封闭条件路由与标签精确匹配——未覆盖的标签落入调度器的“字典序最后路由”回退（顺序相关的任意目标），这是错误。内置 `has_tool_calls` = 封闭 {false,true}，`route_channel` = 开放 + 已知 {default}。
  - **通道效果契约**：`register_type` 4 参数重载声明每节点类型的读/写通道。E4/E5/E6 分析仅在图中**每个**节点类型都被声明时激活（单个未知类型跳过整个分析——健全性优先于覆盖）。内置 3 种类型（llm_call/tool_dispatch/intent_classifier）完全声明。
  - `node_effects` · `condition_specs` 添加到 `export_schema()`（现有 `conditions` 数组保留用于向后兼容）。22 个新测试。

- **拓扑编译时一致性门 — 消费键记账 + 翻译验证**（#75 M1）。双重机制结构性阻断“静默语义丢失”类别（与 v0.1.0–v0.1.7 `conditional_edges` 静默丢弃同种）：
  - **消费键记账**：声明 `"schema_version": 1` 的文档切换到严格编译——未消费的键（拼写错误 `conditionnal_edges`、不支持的字段、被空 `wait_for` 静默丢弃的屏障、内联条件上被忽略的 `to`）全部被收集并报告为编译错误。标记发生在**解析块内部**，因此擦除解析阶段也会擦除标记，导致使用这些功能的严格文档立即失败——一种丢弃回归不可能静默的结构。`_`/`x-` 前缀键（`_comment`、`x-studio-*`）始终允许作为注释命名空间。没有 `schema_version` 的现有文档保留宽松行为（字节保留）。
  - **翻译验证**：每次编译时执行 `CompiledGraph::to_json()` 重新发射 + `GraphCompiler::canon()` 规范形式检查 `canon(input) ==
    canon(re-emit)`。不匹配（= 编译器丢弃了某些内容或接线错误）在严格文档上抛出异常，在宽松文档上输出 stderr 警告。等价性是结构比较——像交换路由键这样的接线错误也会被捕获（存在性比较会遗漏的类别）。
  - `NodeFactory::config_schema(type)` 查询添加，`schema_version` 字段在 `export_schema()` 中记录。27 个新测试（`tests/test_compiler_strict.cpp`）——v0.1.x 丢弃突变模拟（conditional_edges/barrier/interrupt 丢弃
    + route miswirings) included.

## [0.11.1] - 2026-06-25

### Revised translation: 变更

- **stdio MCP 并发调用——用于 I/O 重叠的 correlation-ID 解复用器。** `0.11.0` 并发工具调度实际上仅对 HTTP MCP 实现了重叠。stdio MCP 在 `StdioSession::rpc_call_async` 中为**整个请求→响应往返**持有一个容量为 1 的通道锁，通过单个会话管道将一轮中的多个调用串行化（墙钟时间 ≈ 延迟之和）。单个管道并非根本原因——JSON-RPC `id` 的存在正是为了在单条连接上进行流水线处理。将锁替换为 correlation-ID 解复用器：
  - 将容量为 1 的通道重新用作**仅写锁** — 仅在帧写入的瞬间持有，因此两个调用的字节不会交错，而读取不再被串行化。
  - 单个读取协程（`run_reader`）独占读取侧，并通过 JSON-RPC `id` 将每个响应行投递到正确调用方的接收端。N 个并发调用重叠读取，因此墙钟时间 ≈ max(延迟)——**但仅当对端 MCP 服务器并发处理时**（单线程顺序服务器会触及 Amdahl 下限）。
  - 读取器仅在存在进行中的调用时惰性运行，并在等待者为空时退出，因此私有 `run_sync` io_context 正常返回。等待者仅在其调用方处于等待状态时存在，并通过 `MCPTool` 的 `shared_ptr` 保持会话存活，因此读取器永远不会触及已销毁的会话（无需析构函数 join）。在管道 EOF/错误时，读取器关闭所有接收器，因此等待中的调用方会收到异常而不是无限挂起。
  - **无 API/语法变更**——公共头文件未改动，现有代码无需重新编译。引擎开销回归为 0（`bench_neograph` 交错 A/B，seq/par Δ 0%）。
  - 测试：基于线程的延迟夹具 `tests/fixtures/mcp_stdio_slow.py` + `ConcurrentStdioCallsOverlapIO`（5×100 ms 调用在约 130 ms 内完成，而串行下限为 500 ms；通过 `id` 验证每个响应路由到其调用方）。ASan+UBSan ×3 通过。

## [0.11.0] - 2026-06-25

### 已添加

- **并发工具调度 — `Tool::execute_async` 官方异步路径。** `ToolDispatchNode` 从单个助手轮次中**并发**执行多个 `tool_call`，使用引擎的 `make_parallel_group`。此前每次调用通过同步方式顺序执行 `execute()`，且MCP工具尤其会阻塞于每次调用通过 `io_context` 的过程，导致并行的MCP调用无法重叠（在外部C++分支中通过并行MCP调用发现）。修复： `run_sync` 生成自身
  - Virtual `execute_async()` 已添加到 `Tool` — 默认实现桥接到同步 `execute()`，因此现有工具无需更改即可正常工作。
  - `MCPTool` 转换为 `AsyncTool`，使用原生 `execute_async`（stdio 使用 `rpc_call_async`，HTTP 使用新的 `MCPClient::initialize_async`/`call_tool_async` 进行异步握手——`run_sync` 已移除）。
  - `ToolDispatchNode::run` 通过与 node fan-out 相同的 `make_parallel_group` 惯用法并发调度调用（单次调用内联），结果按调用顺序应用。通过同步 `execute()` 门面保持向后兼容。
  - 验证：478/478 ctest，Valgrind 0 泄漏，TSAN 0 竞争。

### 已修复

- **Python 异步执行异常保留（issue #122）。** 修复了 `run_async`、`run_stream_async` 和 `resume_async` 用新的 `RuntimeError` 将原始 Python 节点异常包装为字符串从而覆盖它的问题。现在原始 Python 异常对象、类型、用户属性和回溯通过 pybind11 的标准异常转换路径保留，C++ `py::type_error` 作为 Python `TypeError` 交付，与同步执行一致。`resume_async` 中的空回调现在保留到协程完成，同时修复了 pybind11 3.x 中暴露的悬空引用冲突。

### 修复（文档）

- **README 摘要徽章针对缺失条件和沙箱测量揭示的内部矛盾进行了修正。** “四个轴”摘要表中的徽章从正文/深入分析中剥离了测量条件，读起来像夸大其词。已修正为与正文测量数值和条件一致（测量数据表本身未变）：
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`**——徽章上的 17 µs 与正文（`At N=10,000 concurrent ... 7 µs p99`）相矛盾，且 `flat` 描述的是 GPU 受限负载测试的运行延迟（648 ms），而非 µs 级测量。徽章已与正文的测量数据和条件对齐。
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`**——`libc.so.6` 专用和 1.2 MB 仅适用于 MinSizeRel + 静态 libstdc++ 构建（默认 Release 动态链接 libstdc++/libgcc_s/libm/libc）。该条件已在深度剖析 §size 中记录，已恢复至徽章。
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** — 直接依赖确实是 `certifi` + `pydantic`（两个），但实际安装树包含 7 个包，包括 pydantic 的传递依赖（pydantic-core、typing-extensions、annotated-types、typing-inspection）。
- **已将 `-DNEOGRAPH_BUILD_POSTGRES=OFF` 添加到深入 MinSizeRel 复现命令中。** PostgreSQL 默认为 ON，因此在没有 libpq 的主机上按原样运行配置会失败。已修复。

## [0.10.0] — 2026-05-20

### 已添加

- **串行 fan-out 一次性 stderr 警告（issue #62，PR #63）。** `compile()` 的默认值为 `set_worker_count(1)` — fan-out 分支在调用方的执行器上串行执行，没有引擎拥有的线程池。这种预期行为对于仅根据文档构建多 Send 图的用户来说，看起来像是静默串行执行。首次 `NodeExecutor` 在没有池的情况下调度多 Send（或多出边）fan-out 时，向 stderr 添加了一条一次性指导消息。`std::atomic` + 比较交换保证即使在并发 fan-out 下也恰好发出一次。调用 `set_worker_count(N>=2)` 会重建 `NodeExecutor`，自然重置该标志。可通过环境变量 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`（或 `true` / `yes`）抑制 — 用于有意的 worker=1 串行执行、基准测试和 CI stderr 断言场景。由 5 个 Linux + macOS 单元测试覆盖（`test_fanout_worker_warning.py`）：触发 / 一次性 / 池选择加入静默 / 环境变量静默 / 单 Send 无警告。Windows：pytest capfd 与 wheel 二进制文件中的 MSVC CRT fd 缓存不兼容，因此模块级跳过 — wheel 二进制 stderr 输出本身是正常的。

- **拓扑 JSON Schema 导出 — `NodeFactory::export_schema()`**（issue #56，无代码可视化块编辑器的先决条件）。将引擎消费的拓扑 JSON 格式作为机器可读的 schema（JSON Schema Draft 2020-12）一次性导出：`{ neograph_version, $schema, topology (fixed
  envelope), node_types, reducers, conditions }`。另一个仓库的块编辑器从该 schema 自动生成其调色板 → 编辑器和引擎不会跨版本漂移。完全增量式：
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-argument
      variant added. Existing 2-argument delegates to permissive default
      schema — existing user nodes/calls unaffected.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` query accessors added.
    - Configuration schema declared for 4 built-in types (`llm_call`/
      `tool_dispatch`/`intent_classifier`/`subgraph`). `NEOGRAPH_VERSION`
      exposed as a compile definition (pyproject.toml single source of truth)
      → schema version stamp.
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — standard path for the editor
      repo CI to produce the artifact pinned to a NeoGraph version.
    - Python: `neograph_engine.export_schema()` → dict (editor repo CI
      dumps after `pip install neograph-engine`).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. Key:
      top-level `conditional_edges` surviving the loader→compile round-trip
      (regression guard against v0.1.0–v0.1.7 silent-drop recurrence).

### 已修复

- **拓扑顶层容器格式验证（#126）。** `channels`/`nodes` 必须是对象；如果不是，则在所有模式下拒绝。`edges`/`conditional_edges` 数组验证在严格模式下强制执行，保留旧版键控边映射兼容性。错误记录路径和 JSON 类型，而非完整输入。
- **`max_steps` 终止状态已暴露（#114）。** 添加了 `RunResult::max_steps_exhausted()` 和只读 Python 属性 `RunResult.max_steps_exhausted`。仅当在仍有节点待执行时达到 `max_steps` 才为 True；在 gRPC 单响应和流式最终 JSON 中提供相同状态。C++ 结构体大小不变。

- **`set_worker_count` / `set_worker_count_auto` 文档字符串更正（issue #62，PR #63）。** v1.0 准备周期有意将 `compile()` worker 池默认值从 `set_worker_count(hardware_concurrency())` 恢复为 `set_worker_count(1)`（原因见 `src/core/graph_engine.cpp:69-93` 注释），但四个面向用户的文档字符串保留了旧声明 → 信任文档构建多 Send fan-out 图的用户得到了单线程上的静默串行执行。单元测试不可见（假 spawn、即时主体）；仅在真实墙钟 e2e 中暴露。
  - 重写了 `set_worker_count` / `set_worker_count_auto` 在 `bindings/python/src/bind_graph.cpp` 中的 Python 文档字符串以匹配实际行为：`compile()` 默认值为 1，`set_worker_count_auto()` / `set_worker_count(N>=2)` 是显式选择加入。
  - 相应更正了 `include/neograph/graph/engine.h` 中的两个 Doxygen 注释。Doxygen Pages 在 master 推送时自动重建。
  - 更正了 `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md` 中相同的过时声明（默认 = hardware_concurrency）。

- **补充了 v0.9.0 中缺失的三个 API 迁移。** PR `9b`（`19819d8`）在 v1.0 准备周期中破坏性地移除了 `GraphNode` 旧版 8 虚拟链，但 PR `#48`（`6e654ad`，“C++ 示例迁移到 `GraphNode::run()`”）仅迁移了 `examples/` — 以下 3 个文件被遗漏，导致 v0.9.0 以构建损坏状态发布：
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3
      sustained-burst verification key benchmark)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (memory/
      concurrency comparison matrix body against LangGraph and other engines)
    - `wasm/smoke.cpp` (Phase 1 WASM feasibility smoke)

CI 未将这些目标作为 add_executables 处理，或（Docker 构建依赖）将其隔离在单独环境中，因此合并至 master 和标签操作均通过。

**修复**：所有三个均从 `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` 模式迁移。节点逻辑不变。

**v1.0 关键卖点原生重新验证** (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - Concurrency 10K · wall 10–23 ms · p99 17–21 µs · peak RSS **5.6 MB**
      (matches v0.3.0 / v0.5.0 measurements — no memory selling-point
      regression after destructive 9b)
    - 0 errors at 10K
  **Docker matrix (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6-way comparison) also re-measured within the same session**
  (`results_v0.9.0_docker_recheck.jsonl`).

在矩阵重跑期间，除了缺失的 API 迁移之外，还发现了一个独立的回归问题——`benchmarks/concurrent/Dockerfile.neograph` 根本无法构建，因为它未能跟踪 master 上 CMake 选项默认值的变化（与 v0.9.0 发布时的情况相同）。随着时间的推移，以下选项默认值从 OFF 翻转为 ON：
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (requiring `libpq-dev` / `libsqlite3-dev` respectively)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (one prior incident closed in
      `feedback_libcurl_unconditional_dep.md` — only the option toggle was
      added while the default remained ON, breaking the empty-container build
      path again)
    - `find_package(OpenSSL REQUIRED)` is unconditional without an option
      toggle (CMakeLists.txt:256) — separate v1.0 cleanup candidate

**Dockerfile 修复**：`libssl-dev` apt 添加 + 所有非核心选项均使用显式 `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF` 固定。注释注明“因两次漂移事件而显式冻结”。`find_package(OpenSSL REQUIRED)` 在 CMakeLists.txt 中的条件化留作单独任务——需要验证对其他构建路径（PyPI wheel、ARM64 等）的影响。

**6 路矩阵关键结果**（并发=10000，2 个 CPU / 1 GiB）：

    | 引擎          | 模式          | wall_ms | p99_us      | 峰值_MB | 成功/错误 |
  |---|---|---|---|---|---|
    | **neograph**    | 线程池    | **16**  | **18**      | **5.1** | 10000/0 |
    | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
    | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
    | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
    | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
    | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

NG 与 LangGraph（营销对比轴）：wall **快 237 倍**，p99 **快 4134 倍**，峰值 RSS **仅为 1/12**。

**严苛场景**（并发数=10000，1 核 CPU / 512 MiB）：
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

相同的 v0.3.0 / v0.5.0 测量结果——**在破坏性 9b 之后，NeoGraph 的“10K 并发工作器、峰值 RSS 5 MB、无 OOM”卖点没有出现回归。**

## [0.9.0] — 2026-05-14 — v1.0 准备（候选 1 阶段 B + 候选 6）

来自 ROADMAP_v1.md 的两个 v1.0 单分派统一在一个周期内汇合：

  - **候选 1 阶段 B (`9b`–`9f`)** — `GraphNode` 的全部 8 个旧虚拟方法（`execute` / `execute_async` / `execute_stream` / `execute_stream_async` / `execute_full` / `execute_full_async` / `execute_full_stream` / `execute_full_stream_async`）+ `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + 全部 6 个 `PyGraphNodeOwner` 旧覆盖已移除。**破坏性变更**——弃用窗口已关闭。用户 GraphNode 子类 / 用户 Python 节点必须迁移到单一方法 `run(NodeInput)` / `def run(self, input)`。
  - **候选 6** — `Provider` 4 虚拟交叉积 → 1 虚拟 `invoke()`。仍处于添加 + 弃用阶段——旧 4 个虚拟方法保持不变且功能正常，仅可见弃用警告。该侧的阶段 B（`Provider` 旧方法移除）也在 v1.0.0 发布前关闭。

同一周期还包括 b59444f 的潜在并行回归回退（`e5ecb08`）+ 显式 fan-out 示例调用 + 3 个 CI 环境修复（httplib 宏保护 / Windows MSVC unistd.h / pybind pytest 迁移），全部属于此 [Unreleased]。

### 已添加

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 标准单分派入口点。在一个方法中同时处理非流式（`on_chunk == nullptr`）和流式（`on_chunk` 提供）场景。将之前的 4 虚拟交叉积（`complete` / `complete_async` / `complete_stream` / `complete_stream_async`）整合为一个异步流式超集。默认实现转发到 4 个旧虚拟方法，因此现有 Provider 子类无需更改即可工作。新增 6 个 ctest（`ProviderInvokeDefault`）。（PR #40）
- **`invoke()` 取消传播对等性** — 当未设置 `params.cancel_token` 且引擎线程本地作用域处于活动状态时，`current_cancel_token()` 会自动标记。等同于旧同步 `complete()` 行为（引擎内调用 `provider->invoke(params, ...)` 的节点主体自动接收运行中图的取消信号）。新增 3 个 ctest（`InvokeCancelPropagation`）。（PR #43）
### Revised translation: 变更

- **引擎中所有内部 LLM 调用均通过`invoke()`路由**——`LLMCallNode`、`IntentClassifierNode`（PR #41/#42）、`Agent::complete` / `Agent::run_stream`（PR #43）、`SupervisorLLMNode` / `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode`（PR #43）、`PlannerNode` / `ExecutorNode`（PR #44）。NeoGraph 内的 LLM 分发统一到单一接口。
- **C++ 示例迁移（2 个文件）**——`31_local_transformer.cpp`、`cookbook/ai-assembly/member_server.cpp` 现在使用新的`invoke()`。用户构建中无弃用警告。（PR #45）
- **`GraphEngine::compile()` 默认工作线程数恢复为 1**（`e5ecb08`）。`b59444f` 是潜在 18 天（2026-04-26 → 2026-05-13）并行微基准回归 11.8 → 283 µs（24×）的根因——通过二分法（并行 11 个工作树）精确定位提交。从 v1.0 默认=1（对 CPU 密集型顺序/并行分发最优）；对于有意的 fan-out，添加一行`engine->set_worker_count_auto()`以打开 hardware_concurrency。向 5 个受影响的 fan-out 示例（10/14/21/36 + deep_research_graph 构建器）添加了显式调用。详见 ROADMAP_v1.md 中的“Perf retrospective”部分。

### 已弃用

- **`Provider::complete` / `complete_async` / `complete_stream` / `complete_stream_async`** — 全部 4 个旧版虚拟方法均带有 `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` 标记。旧版方法在弃用窗口期内按原样运行。在 v1.0.0 中移除。内部转发器以 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 包裹，因此警告仅在面向用户的覆写/调用点出现。(PR #44)

### 已删除(候选 1 阶段 B — 破坏性)

- **`GraphNode` 旧版 8 个虚拟方法** — `execute(GraphState&)` / `execute_full(...)` / 6 个变体 + `ExecuteDefaultGuard` 递归保护
  + 300 多行默认链。全部移除。`run(NodeInput)` 是唯一的纯虚拟方法。(提交 `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` 成员 + `cancel()` 钩子迭代** — `cancel.h` 仅保留 `fork()` + `cancel()` + `is_cancelled()` + `slot()`。(提交 `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local + `GraphState::run_cancel_token_` + 3 个访问器** — `RunContext::cancel_token` 是唯一的取消通道。`src/core/cancel.cpp` 被清空为存根（该文件本身是未来删除的候选对象）。(提交 `9e8e956`)
- **6 个 `PyGraphNodeOwner` 旧版覆写** — pybind trampoline 仅调用 `run(self, input)`。Python 用户代码自 v0.9.0 起也要求单一方法。(提交 `9e8e956`)
- **2 个过时的 pytest 文件** — `test_execute_stream_dispatch.py`（v0.3.2 仅流式回退分发验证）+ `test_streaming_only_error_
  hint.py`（execute_full_stream 优先 — 在 v1.0 中无意义）。(提交 `4392fbb`)

### 已修复

- **向 5 个 fan-out 示例添加了显式调用** — 恢复了被 `e5ecb08` 的默认工作线程数回退所掩盖的真实并行意图：`examples/10_send_command.cpp`、`examples/14_plan_executor.cpp`、`examples/21_mcp_fanout.cpp`、`examples/36_classifier_fanout.cpp`、`src/core/deep_research_graph.cpp` 的 `create_deep_research_graph()` 构建器现在调用 `set_worker_count_auto()`。验证：`classifier_fanout` 4.22 倍加速（25.2 毫秒串行 → 6.0 毫秒并行）。(提交 `99c470b`)
- **`bench_async_http` httplib 宏保护** — `bench_async_http.cpp` 包含 `<httplib.h>` 通过 `<neograph/async/conn_pool.h>` 但 `CPPHTTPLIB_OPENSSL_SUPPORT` 未定义，导致 ODR 保护拒绝。已添加 `target_compile_definitions(... PRIVATE ...)` 到 CMake 目标。(提交 `d4be42a`)
- **Windows MSVC `unistd.h` 缺失** — `test_schema_provider_extra_
  fields_temperature.cpp` 使用了仅 POSIX 的 `mkstemps` + `close`，导致 Windows 构建完全失败。将整个文件包裹在 `#ifndef _WIN32` 保护中（覆盖率由 Linux/macOS 保证）。(提交 `3c49f12`)
- **16 个 Python 测试已迁移** — wheel CI pytest 在 28 个节点类上命中了 `AttributeError`，并带有遗留的 `def execute(self, state)` 模式。已批量迁移至 `def run(self, input)`；流式节点获得了 `input.stream_cb` None 守卫。（提交 `4392fbb`）

### 迁移(用户代码)

**Provider 调用(Candidate 6 — 弃用阶段)**

新增代码:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

4 个旧版虚拟覆写在废弃窗口期内继续工作，但 `-Wdeprecated-declarations` 警告在用户覆写点可见。移除发生在 v1.0.0 之前；建议在废弃窗口期内进行迁移。

**`GraphNode` 子类（候选 1 阶段 B — 破坏性）**

C++ code:
```cpp
// old (up to v0.8.x)
class MyNode : public GraphNode {
    NodeResult execute_full(const GraphState& state) override {
        auto x = state.get("x");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        return out;
    }
};

// v0.9.0+ current code (single method, coroutine entry)
class MyNode : public GraphNode {
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto x = in.state.get("x");
        // in.ctx.cancel_token / in.ctx.step / in.stream_cb also accessible
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        co_return out;
    }
};
```

Python 代码：
```python
# old (up to v0.8.x)
class MyNode(neograph_engine.GraphNode):
    def execute(self, state):
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]

# v0.9.0+ current code
class MyNode(neograph_engine.GraphNode):
    def run(self, input):
        state = input.state  # input.ctx.cancel_token / input.stream_cb etc. also accessible
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]
```

**fan-out 意图（worker 数量默认值变更）**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

`docs/migration-v0.4-to-v1.0.md` 中的迁移 1/2/3 章节（run() / ctx.cancel_token / worker 数量默认值）+ Provider 章节（将在下一次文档扫描中添加）提供了逐案例的前后对比指南。

## [0.8.0] — 2026年5月13日 — DX 策略 + 下游驱动的 API 缺口已解决

将真实下游（ProjectDatePop）反馈和内部覆盖率差异所暴露的 8 个问题（#22、#25、#26、#27、#28、#34、#35 + #16 后续）打包为一次次要版本升级。新增两个公共辅助函数（`RunResult::channel<T>`、`RunContext::store`）、11 个新的离线示例、`docs/migration-v0.4-to-v1.0.md` 迁移指南，以及一个包含 5 项的 DX 包，以减少新用户在最初 30 分钟内遇到的摩擦。

### 已添加

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** — 用于从结果中提取通道值的一行辅助函数。两种输出形状（嵌套的 `output["channels"][name]["value"]` 标准格式 + 由 `react_graph` 等构建器添加的扁平键）均自动处理。新增 9 个 ctest。（问题 #25）
- **`RunContext::store`** — 节点主体通过一行代码到达 Store `in.ctx.store->get(ns, key)`。旧模式（捕获 `shared_ptr<Store>` 于 `NodeFactory` lambda 中）仍然有效 — 新代码只需要新的形态。3 个新的 ctest。（Issue #27）
- **`Provider::complete_stream` 非纯默认主体** — 最小 mock / 测试夹具只需覆盖 `complete()`。现有的流式原生覆盖保持不变。新增 2 个 ctest。（问题 #22）
- **`neograph::json` 数组 `.front()` / `.back()`** — nlohmann 肌肉记忆模式（`msgs.back()["content"]`）现在可以编译。新增 4 个 ctest。（问题 #26）
- **11 个新的离线示例（41-51）** — `resume_if_exists_chat`、`custom_reducer_condition`、`store_personalization`、`request_queue_backpressure`、`cancel_token`、`node_cache`、`sqlite_checkpoint`、`openinference`、`async_tool`、`minimal`。全部 rc=0，无 API 密钥 / 外部服务依赖。填补了 27/53 个 `NEOGRAPH_API` 类中先前零引用的空白。
- **`examples/51_minimal.cpp`** — 30 行入门示例，包含一个节点，无 LLM、无工具、无 mock provider。在 5 分钟内理解 NeoGraph 的运行方式。
- **`docs/migration-v0.4-to-v1.0.md`** — 逐案例的前后对比 4 个示例 + 从 `[[deprecated]]` 旧 8 虚拟链（`execute` / `execute_async` / 等）迁移到新 `run(NodeInput) ->
  awaitable<NodeOutput>` 的常见错误。同时从 `NEOGRAPH_DEPRECATED_VIRTUAL` 宏消息中链接。
- **README “常见陷阱 5” 章节** — 新用户在最初 30 分钟内遇到的五件事（`channel<T>` 用法、`in.ctx.store`、`neograph::graph::` 子命名空间、`<httplib.h>` 宏、GCC 13 协程 ICE）集中在一处。每项都有修复方法 + 相关示例/问题链接。
- **编译时 `#error` 保护（`include/neograph/api.h`）** — 当用户 TU 在 NeoGraph 头文件之前包含 `<httplib.h>` 而未定义 `CPPHTTPLIB_OPENSSL_SUPPORT` 时，编译失败并显示清晰消息 + 退出宏（`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`）。将旧的 #16 运行时 SEGV 提升为编译时失败。
- **`example_minimal` 5 个新的友好错误消息 ctest** — 对 `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to
  unknown channel` 消息的契约锁定，在消息正文中嵌入可用名称 + 注册方法 + 故障排查链接。
- **`docs/troubleshooting.md` 4 个新条目** — Tracer 适配器 `close()` 挂起/崩溃 (#24)、GCC 13 协程 ICE (#23)、友好错误消息指南 (#22)、`RunResult::output` 形状 (#25)。
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` 块** — 明确记录了适配器作者的裸指针陷阱。指出 `RecordedSpan` + 包装器分离模式是正确的方法。引用现有的 `tests/test_openinference_cpp.cpp::InMemoryTracer` + 新的 `examples/49_openinference.cpp::PrintTracer`。(Issue #24)

### 已修复

- **`SchemaProvider::build_body` 静默丢弃 `extra_fields` 当 `params.tools` 为空时。**旧代码将 `extra_fields` 的应用限制在 `if (!params.tools.empty())`内部，导致核心模式字段如 `reasoning` 和 `response_format` 在无工具调用中完全消失。修复：将其移至工具分支之外，以便始终应用。新增3个ctest。(Issue #34)
- **`temperature_path` schema 端选择退出。** 推理模型（gpt-5.x、o 系列）具有互斥的 `temperature` 和 `reasoning.effort`，但 schema 无法声明“此提供者不接受 temperature”，迫使每次调用都使用 `params.temperature = -1.0f` 哨兵值变通方案。修复：在 schema 中指定 `"temperature_path": null` 会导致 build_body 完全跳过它。4 个新的 ctest。(Issue #35)
- **友好的 RuntimeError 消息** — `ReducerRegistry::get` / `ConditionRegistry::get` / `NodeFactory::create` “未知 <thing>: foo” 和 `GraphState::write` / `apply_writes` `Write to unknown channel` 现在在消息正文中嵌入可用名称 + 注册方法 + 故障排查链接。新手仅凭消息即可确定后续步骤。
- **`SchemaProvider::complete_stream_async` HTTP/SSE 分支** 现在在长期存在的专用 `bridge_thread_` 上分发（旧：`Provider` 基础默认值每次调用都会生成一个新的 `std::thread`）。旧行为在 glibc `internal_strlen` 中触发了 SEGV，并带有冷线程本地解析器 / NSS 状态。WS 分支已经是原生 co_await，因此不受影响。在 awaiter 的执行器上进行令牌分发得以保留（PR #10 不变量）。(Issue #16)
- **`example/09_all_features.cpp`** Store 演示 — 添加了 docstring 指针，指向 `examples/43_store_personalization.cpp` 以了解节点正文读取模式。选项 2 — 选项 3（内联实时节点）将在 #27 的 `RunContext::store` 落地后一起清理。(Issue #28)

### 文档

- `RunResult::output` 的规范形状（channels 包装）及其与构建器（如 `react_graph`）添加的扁平键投影的关系已在头文件 docstring 中记录。建议使用新的辅助函数（`channel<T>` / `channel_raw` / `has_channel`）。(Issue #25)
- `RunContext::store` 字段 `@brief` 块 — 两种管道模式（推荐 `in.ctx.store` / 兼容旧工厂闭包捕获）并排提供代码示例。(Issue #27)
- 两种路径都记录在 `examples/43_store_personalization.cpp` 文件头注释中。

## [0.7.0] — 2026-05-11 — C++ OpenInference + 异步流式桥接

在一个次要版本中关闭了针对 v0.6.0 提交的四个问题。头条：`Provider::complete_stream_async` 默认值在从外部引擎协程内部等待时不再段错误（issue #4）——这是位于 NeoGraph 前面的 SSE / 流式 HTTP 后端最常见的形式。配套：v0.6.0 Python OpenInference 层的 C++ 对等实现，以便 Phoenix / Arize / Langfuse 以与渲染 Python 跟踪相同的方式渲染 C++ 驱动的跟踪（issue #9）。另外：静音了装饰性的 Python OTel 分离噪声（issue #2），并且相同的 `thread_id` 并发运行 + `schema_mutex_` × on_chunk 锁定不变量现在固定在 docstring 中（issue #6）。

### 已添加

- C++ 对等实现 `neograph_engine.openinference`（问题 #9）。新的 `neograph::observability` 模块涵盖两个部分：
  - `Tracer` / `Span` — 小型无依赖抽象接口，使 NeoGraph 本身不引入 opentelemetry-cpp。下游提供适配器包装自己的后端（OTel SDK、内存测试假件、日志记录器等）。4 个属性设置器（string、int64、double、bool — bool 特意重命名为 `set_attribute_bool`，使 `const char*` 字面量不会意外解析到它），外加用于流式 token 诊断的 `add_event`、状态和 `end()`。
  - `openinference_tracer(tracer)` — 打开一个 CHAIN 类型的根 span，返回一个 `OpenInferenceTracerSession`，其 `cb` 字段接入 `engine.run_stream()`，并为每个节点打开一个 CHAIN 类型的子 span，将 `NODE_START`/`END` 负载塞入 `input.value` / `output.value` JSON 块中，并将 `LLM_TOKEN` 事件记录为离散的 span 事件。
  - `OpenInferenceProvider(inner, tracer)` — 包装任何 `Provider`，在每次`llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`）。流式重载还会追加 `complete*` 调用时附加 OpenInference LLM 类属性集（ `llm.token` 事件以及最终组装好的 `output.value`.
  - `tests/test_openinference_cpp.cpp` 中的 7 个对等测试，驱动 `InMemoryTracer` 参考适配器 — 断言根 + 每节点 CHAIN span 层级、ERROR / INTERRUPT 状态呈现、LLM_TOKEN span 事件记录、会话关闭时的掉队 span 清理、LLM 提供程序属性集、流式 token 事件以及异常状态传播。

### 已修复

- `Provider::complete_stream_async` 默认桥接不再在流的持续时间内阻塞等待协程的执行器。修复前，默认是 `co_return complete_stream(...)` 内联的，这（a）在完整的 HTTP/SSE 接收循环期间挂起了引擎的 `io_context` 工作线程——因此同一执行器上的其他节点协程停滞——并且（b）对于 `SchemaProvider`的 WebSocket Responses 分支，还通过 `run_sync` io_context，在共享提供者状态上竞争，并在从外部 `run_sync(complete_stream_ws_responses(...))`在引擎工作线程之上额外嵌套了一个新的 `GraphEngine::run_stream_async`内部调用时产生间歇性段错误。新的默认值为同步 `complete_stream`生成一个专用工作线程，将每个 token 分派回等待者的执行器（因此用户的 `on_chunk` 与等待协程单线程运行——无重入），并通过一次性 `steady_timer.cancel()`恢复协程。工作线程异常在等待者处重新抛出。 `SchemaProvider` 添加了一个原生 `complete_stream_async` 覆盖，通过直接 `co_await`处理 `complete_stream_ws_responses`. `OpenAIProvider` 来跳过 WebSocket 路径的工作线程，从而透明地受益于新的基础默认值（无 WS 路径，无特殊用例）。在 `tests/test_provider_async_default.cpp`: `StreamAsyncBridgeDoesNotBlockExecutor` 中有两个新测试： `StreamAsyncBridgeRethrowsWorkerException`（一个并发 ticker 协程在流期间推进 + 块在等待者线程上传递，而非工作线程）和

- `openinference_tracer`: 静默 `Failed to detach context` stderr 回溯，该回溯是 OTel 的 SDK 在每次关闭时发出的，当 tracer 与 `engine.run_stream_async` + `StreamMode.ALL`一起使用时。在 NODE_START 时创建的 OTel contextvars token 是从一个不同的 `asyncio.Task` 中分离的（NODE_END 回调从引擎的 continuation 触发，而非调用者的任务），因此 `Context.reset(token)` 引发了 `ValueError`；SDK 吞掉了该异常，但仍将完整的回溯信息路由到 `logger.exception`，污染了生产日志而不影响语义。修复方案在 attach 时记录（线程，任务），并在不匹配时跳过 detach，同时在 `logging.Filter` ，仅在我们的 `opentelemetry.context` 上安装一个窄范围的 `_safe_detach` 位于栈上时丢弃该消息。同步调用者和同任务异步调用者仍能在节点 span 下获得正确的 LLM-span 嵌套。（Issue #2）

---

## [0.6.0] — 2026-05-07 — OpenInference可观测性层

关闭LangSmith UX差距。NeoGraph已经发出OTel形状的跨度（因此跟踪流向任何OTel后端）；此版本添加了LLM特定的属性层，Phoenix / Arize / Langfuse使用该层将跟踪渲染为聊天气泡+令牌计数UI，而不是扁平的通用应用程序跨度列表。已针对本地Phoenix容器进行端到端验证——writer→critic图产生6跨度层次结构（CHAIN根→节点跨度→LLM跨度），模型名称、提示/响应和令牌计数在Phoenix UI中可见。

### 已添加

- `neograph_engine.openinference` 模块：
  - `openinference_tracer(tracer)` — 上下文管理器，镜像 `otel_tracer`，但用 `openinference.span.kind = "CHAIN"` 标记根和节点 span，并将节点负载放入 `input.value` / `output.value` JSON 块中。
  - `OpenInferenceProvider(inner, tracer)` — 包装任何 `Provider`。在每次 `complete()` 打开一个 `llm.complete` 子跨度，标记为 `span.kind = "LLM"`，捕获 `llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`，以及兼容 Langfuse 的 `input.value` / `output.value` 数据块。
- `bindings/python/tests/test_openinference.py` 中的 4 个测试 — InMemorySpanExporter 对属性存在性、span 层级、异常路径和节点输入/输出 JSON 块的断言。

### 已修复

- `openinference_tracer` 现在将每个节点 span 附加为 OTel *当前* 上下文（通过 `otel_context.attach`），以便节点主体内打开的子 LLM span 嵌套在其节点 span 下。没有这个，C++→Python pybind 回调边界上的 contextvar 传播会产生每次运行 3+ 个不相关的 trace_id，而不是预期的单一 trace 树。token 在 NODE_END / ERROR / INTERRUPT 时被分离，以恢复之前的当前 span。现有 `otel_tracer` 文档记录了相同的模式 — 显式附加/分离，而非 `trace.use_span(...).__enter__()`，后者在没有匹配的 `__exit__` 时不安全。

### 备注

- OpenTelemetry 仍然是一个可选依赖。导入 `neograph_engine.openinference` 仅在首次使用时，如果 `opentelemetry-api` 未安装，才会引发明确的 ImportError；不会在导入时引发。
- 对于Phoenix端到端运行：：

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

将 OTLP gRPC 导出器配置为 `http://localhost:4317` 并打开 `http://localhost:6006` 以查看追踪。模块文档字符串中包含完整代码片段。

---

## [0.5.0] — 2026-05-07 — 绑定人体工学改进：实时变更列表属性

修复了通过绑定暴露的 message / writes / sends 列表在 Python 最自然惯用变更方式下的静默无效陷阱。此前 `params.messages.append(msg)` 修改的是副本，底层 C++ 向量从未看到新条目——优雅失败（无崩溃、无警告）导致 LLM 回复质量下降。现在 `.append()` 会直接推送到实时 std::vector。

### 已添加

- `bindings/python/src/opaque_types.h` — 五种向量类型的 `PYBIND11_MAKE_OPAQUE`：`std::vector<ChatMessage>`、`<ChatTool>`、`<ToolCall>`、`<graph::ChannelWrite>`、`<graph::Send>`。
- `module.cpp` `init_opaque_vectors` — `py::bind_vector` 将每种类型注册为 Python 类（`ChatMessageList`、`ChatToolList`、`ToolCallList`、`ChannelWriteList`、`SendList`），支持针对实时 C++ 向量的完整可变序列协议。
- 每种类型的 `py::implicitly_convertible<py::list, …>` — 传统的构建后赋值模式（`params.messages = [m1, m2]`）保持原样工作；赋值会自动将 Python 列表转换为绑定类。
- `bindings/python/examples/23_evolving_chat_agent.py` — 每线程演化的聊天智能体（实时 LLM）：智能体的 JSON 定义在轮次之间根据累积的对话历史进行重写。演示了跨演化的检查点恢复（先前消息保留）、`__graph_meta__` 审计通道模式，以及验证器边界（白名单节点类型、必需通道）。

### Revised translation: 变更

- `params.messages` / `.tools` / `chat_message.tool_calls` / `node_result.writes` / `.sends` 现在返回其绑定类，而非普通 `list`。`len()`、迭代、`__getitem__`、`__setitem__`、`.append()`、`.extend()`、切片——所有行为都像 Python 列表。只有 `isinstance(x, list)` 返回 False。仓库及下游 grep 确认零个此类 isinstance 调用点。
- `.github/workflows/nightly.yml` — 移除 `ops/s ≥ 600K` 门控。在连续 4 次失败且使用 `err=0` 和 `leak=false` 后，阈值（根据本地硬件校准为 969K ops/s）在共享 GitHub 托管运行器上无法达到（测得 233~273K ops/s，比本地低 3-4 倍）。吞吐量回归检测位于 PR 时的 `bench-regression` 任务中（硬件稳定，单次调度在微秒级）。夜间浸泡测试的实际价值是 5 分钟内的 `err==0` + `leak_suspect==false` — 两者均保留为硬性门禁。

### 备注

- `ChatMessage.image_urls`（`std::vector<std::string>`）有意未迁移 — `vector<string>` 在绑定中使用过于广泛，无法在不扫描每个调用点的情况下进行全局 OPAQUE。已记录为剩余限制；v0.6+ 候选。

---

## [0.4.0] — 2026-05-05 — v1.0 准备：统一 `run(NodeInput)` 调度

v1.0 打磨轨道（ROADMAP_v1.md）的开篇版本。8 个虚拟 `GraphNode` 交叉乘积（`execute` / `execute_async` / `execute_full` / … / `execute_full_stream_async`）折叠为单一规范方法：`run(NodeInput) -> awaitable<NodeOutput>`。每次运行的取消元数据从非通道集 `GraphState` 成员 + 线程本地走私通道移入显式 `RunContext` 参数。`deadline` 和 `trace_id` 仅作为保留扩展槽添加，不由 `RunConfig` 填充。`CancelToken` 获得分层 `fork()`，因此多 Send fan-out 工作线程各自拥有私有信号，父级的 `cancel()` 会级联到该信号。

### 已添加

- `RunContext` (`include/neograph/graph/engine.h`) — 显式的每次运行元数据：可用的 `cancel_token`、`thread_id`、`step`、`stream_mode`，以及保留的 `deadline` 和 `trace_id` 槽位。引擎将其贯穿于每次 `NodeExecutor::run` 调用。**PR 1，提交 `a473f0e`。**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 单一规范分发入口点。`NodeInput { state, ctx,
  stream_cb }`；`NodeOutput { writes, command, sends }`。默认主体转发到旧版 8 个虚函数，以便现有子类保持可编译。**PR 2，提交 `607ce66`。**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 具有自身 `cancellation_signal` 的子令牌。父级 `cancel()` 级联到所有活动子级（并递归到孙级）。`run_sync(aw, parent_token)` 切换到 `parent_token->fork()`，使每个嵌套操作绑定自己的槽位 — 关闭了 v0.3.x 的 emit-vs-bind 竞争以及多 Send 单处理程序覆盖。v0.3.x 的 `add_cancel_hook` 列表通过弃用继续工作。**PR 3，提交 `897645c`。**
- `[[deprecated]]` 作用于 8 个旧版 `GraphNode` 虚拟方法 + `add_cancel_hook`。内部调用点（graph_node.cpp 默认链、默认 `run()` 转发器）由新的 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 宏（`api.h` — GCC / clang / MSVC 可移植）括起。覆盖已弃用虚拟方法的用户代码会看到迁移警告；引擎内部保持干净。**PR 4，提交 `35a4517`。**
- `engine.get_state_view(thread_id) -> StateView` 现在是规范状态读取；原始字典 `engine.get_state(...)` 在文档字符串中软弃用（不发出警告 — 原始字典仍然是有效的逃生舱）。**PR 5，提交 `f31aa53`。**
- 7 个 C++ + 19 个 Python 示例迁移到 `run(NodeInput)`。冒烟运行与 v0.3.2 输出逐位匹配。**PR 6a/6b，提交 `a2a24ef` / `0a76e3a`。**
- Pybind `PyGraphNodeOwner` 覆盖 `run(NodeInput)` 并分派到 Python 用户的 `run` 方法（如果定义），否则回退到旧版链。`RunContext` / `NodeInput` / `CancelToken` 暴露给 Python；`cancel_token` 可通过 `input.ctx.cancel_token` 访问，无需线程本地走私。**PR 7，提交 `4e186a5`。**
- `docs/reference-en.md` §6 GraphNode 折叠为单个 `run()`。在 §7 下添加了 RunContext + `fork()` 示例小节。README 的“与 LangGraph 的差异”新增了“一个节点方法”条目。**PR 8，提交 `519a00b`。**
- 内置 C++ 节点（`LLMCallNode`、`ToolDispatchNode`、`RouteToNode`）迁移到 `run(NodeInput)` 覆盖。**PR 9a，提交 `d1070dc`。**
- 新手模式陷阱修复：README CMake 片段记录了 `graph::` 子命名空间、cppdotenv 路径、`OpenAIProvider::create()` 与 `create_shared()`、`neograph::json` 作为 nlohmann 子集、3 参数与 2 参数 `compile()`。Python `compile(def, ctx, store=None)` 关键字参数已添加（增量式，非破坏性）。**提交 `ee11ed6`。**

### Revised translation: 变更

- README：“10K 工作进程实测压力测试”部分 — RTX 4070 Ti + Gemma 4 E2B Q4 在 neoclaw 上，N=10000 完成 @ 0 错误 / 424 秒 / 2572 MB 峰值 / 约 1 KB 边际工作进程成本 / p99 648 毫秒（`7840b81`）。
- README：“生产经济学”部分 — 舰队安全 + RAM 增量框架（`b82b15a`）。
- README：“无需 Docker”+“依赖漂移免疫”要点，位于 LangGraph 增量列表中（`333b482`、`a6061d7`）。

### 已弃用

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — 通过 v0.5.x 保持与 `[[deprecated]]` 注解兼容，在 v1.0 中移除。
- `CancelToken::add_cancel_hook` — 由 `fork()` 替代。相同的弃用窗口。

### 备注

- 验证：442 → 452 ctest（新增 3 个 NodeRunDispatch + 7 个 CancelTokenFork）+ 96 个 pytest + 5 个实时 LLM/WS 在 v0.4.0 标签下全部通过。
- 一个子 PR（`run(const NodeInput&)` 引用参数）在 pybind 异步路径下触发了 v0.2.0 RunConfig 协程引用 UAF 崩溃形态。修复在合并前落地：`NodeInput in` 按值传递。记录于 `node.h`。

---

## [0.3.2] — 2026-05-05 — 取消传播加固（5 轮）

五轮补丁系列，填补了 v0.3.0 单次取消所暴露的缺口：Send fan-out 传播、进程内轮询、Python 钩子、C++ 作用域、异常类型。同时落地了来自 FastAPI SSE 聊天演示评估的 TODO_v0.3.md 反馈批次 — `resume_if_exists`、字典或列表 `update_state`、用于类型化状态读取的 StateView。

### 已添加

- `RunConfig::resume_if_exists` — 选择性地恢复先前线程的检查点，无需显式调用 `resume()` 。标准多轮聊天语义： `engine.run(cfg)` 在存在 `thread_id` 时继续对话。
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — 接受两种形态。修复前仅 `dict` 有效；传递列表会静默无操作。列表形式与每个节点主体的 emit 形态对称。
- `StateView`（`bindings/python/neograph_engine/state_view.py`）— Pydantic 类型化状态读取。`engine.get_state_view(thread_id) ->
  StateView` 返回扁平点访问（`view.messages` / `view.foo`）以及用于字典逃生舱口的 `view.raw`。子类化以定义类型化通道：`class ChatState(ng.StateView): messages: list[dict] = []`。
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` — 断言飞行中取消确实在套接字层中止了每个 Send 派生的兄弟节点（这是 v0.3.1 根因补丁）。
- `examples/22_self_evolving_graph.py` — 移至 v0.3.2，并包含 TODO_v0.3.md #9 食谱折叠。
- ROADMAP_v1.md — 从取消轮次事后分析中衍生出的设计强化候选（单一调度、RunContext、分层 CancelToken — 全部已在 v0.4.0 中交付）。
- Doxygen `/* */` 通配符修复 — `acp/types.h` 包含带有路径通配符的 `/**` 块（`fs/*`、`terminal/*`），这些通配符打开了嵌套注释并抑制了所有后续诊断。已替换为 `&#42;` HTML 实体。

### 已修复

- 取消传播，5 个累计轮次：
  1. v0.3.0 单节点 — `cancel_token` 到达 `Provider::complete`。
  2. v0.3.1 多发送指针丢弃 — fan-out 工作线程现在共享 `run_cancel_token_shared()`（此前在 `init_state +
     restore` 于通道集之外重建每工作线程状态时丢失）。
  3. v0.3.1+ 进程内轮询 — 引擎超步循环在步骤之间轮询，而不只是在 LLM I/O 时。
  4. v0.3.2 Python 钩子 — `add_cancel_hook` 在每次运行的令牌上注册回调，在 `cancel()` 时触发。允许同步 Python `execute()` 安装临时取消处理器，而无需线程局部作用域。
  5. v0.3.2 C++ 作用域 + 重试 + 异常类型化 — 在主线程上重新抛出 `NodeInterrupt`（避免 libstdc++ `__exception_ptr::_M_release` 竞态），重试预算尊重取消，运行时与逻辑异常分离。
- 仅 `execute_stream` 的 Python 节点静默落入默认 `execute` 路径（NotImplementedError）。现在当用户仅覆盖流式变体时，`run_stream` 直接连接 `execute_stream`。
- `update_state` 接受 list[ChannelWrite] — 关闭静默无操作（TODO_v0.3.md #5）。

### 备注

- 442 个 ctest + 96 个 pytest + 2 个实时 LLM（单个 + fanout 取消）在 v0.3.2 标签处通过（`915e90e`）。
- 27/30 个 C++ 示例 + 20/22 个 Python 示例在 `examples/run_all.py` 下通过。跳过的测试需要外部服务（Postgres / Crawl4AI / 实时 OpenAI）。
- Valgrind 6 个示例 0 错误，815 次分配 / 815 次释放，干净。
- 基准中位数 5.185 µs/迭代（seq 路径，v0.3.0 基线）— 整轮中零性能回归。

---

## [0.3.0] — 2026-05-04 — 协作式取消传播

关闭 FastAPI SSE 聊天演示评估期间报告的生产成本泄漏缺口：前端 `AbortController` 取消 asyncio 任务不再让上游 OpenAI 请求运行至完成。取消传播贯穿运行的每一层。

### 已添加

- `neograph::graph::CancelToken`（原子标志 + asio `cancellation_signal`）和 `CancelledException` — `include/neograph/graph/cancel.h`。协作式取消原语。通过 `RunConfig::cancel_token` 传递（可选 `shared_ptr`）；引擎超级步骤循环在步骤之间轮询 `is_cancelled()`，并以 `CancelledException` 退出。令牌的 `cancellation_slot()` 绑定到运行的 `co_spawn`，因此进行中的 LLM HTTP 套接字操作在线上被中止（asio `operation_aborted`）。
- `CompletionParams::cancel_token` — 供用户在多个 `provider.complete()` 调用之间传递中止的显式固定项。 `Provider::complete` 读取它（或回退到由 `current_cancel_token()` ），并将该槽位绑定到其内部 `PyGraphNode::execute_full_async`设置的线程本地 `run_sync` io_context，因此即使被取消命中的同步 Python 节点也会停止计费。
- `GraphState::run_cancel_token()` — 每次运行时的非序列化句柄，由 pybind `PyGraphNode` 用于安装一个 `CurrentCancelTokenScope` 包裹同步 Python `execute()` 调用。这正是让同步 Python 用户无需修改节点代码即可获得透明取消传播的机制。
- pybind `engine.run_async` / `run_stream_async`：asyncio `Future.cancel()` 现在通过 `add_done_callback` 连接到 `CancelToken::cancel()`，并且 `co_spawn` 绑定令牌的取消槽。
- pybind 安全解析辅助函数 `_safe_set_future_result` / `_safe_set_future_exception` — 防护 `future.set_result` / `set_exception` 通过 `call_soon_threadsafe` 发布的调用，防止取消的未来 `InvalidStateError` 风暴。
- `bindings/python/tests/test_async_cancel_live_llm.py` — 实时 OpenAI E2E 测试，断言 OpenAI HTTP 在 `Future.cancel()` 后 3 秒内完成（实际为即时完成；修复前约为 7–8 秒的未取消流式传输）。除非设置 `NEOGRAPH_LIVE_LLM=1`，否则跳过。
- `examples/22_self_evolving_graph.py` — 自演化图 PoC：`prompted_llm` 节点从 JSON 配置读取自身提示词，使 LLM 重写器可在运行之间修改图定义并重新编译。演示 `0.0 → 0.4` 分数提升；记录了重写器中的通道流推理缺口。

### Revised translation: 变更

- `Provider::complete(params)` 现在将其内部取消槽绑定到其 `run_sync` 当 `params.cancel_token` 被设置，或当线程本地 `current_cancel_token()` 处于活动状态时。对于未选择加入的调用者，保留之前的默认行为（无取消）。
- `neograph::async::run_sync` 增加了可选的 `graph::CancelToken*` 参数；当非空时，绑定的 spawn 会绑定该令牌的槽。
- pybind `resolve_future_async` 通过安全解析辅助函数路由，而不是调用 `future.set_result` 直接通过 `call_soon_threadsafe`.

### 路线图（推迟至 v0.3.x — 参见 `TODO_v0.3.md`）

- 在同一 `thread_id` 上实现 LangGraph 风格的自动检查点恢复。
- `run_async` 错误消息中的仅流式节点提示。
- `cb.emit_token(node, data)` 易用辅助函数。
- README“与 LangGraph 的差异”部分。
- `update_state` 签名与文档对齐。
- `get_state` 扁平辅助函数 / Pydantic 访问器。
- 在`run_parallel_async`和`run_sends_async`分支fan-out中对取消传播进行实时验证。
- pgvector RAG 示例。

---

## [未发布] — 第 4 阶段

阶段4关闭了异步路径上的最后一个`run_sync`跳点。`run_async`现在端到端地停留在调用方的执行器上：在`io_context`线程上的三个50毫秒智能体在`examples/27_async_concurrent_runs`中从约150毫秒（串行）降至约50毫秒（重叠）。

### 破坏性

- **`GraphNode::execute_full_async` 默认已切换为异步优先。** 它现在将 `co_await execute_async(state)` 包装到 `NodeResult` 中，而不是调用同步的 `execute_full(state)`。任何仅从同步的 `Command`/`Send` 覆盖中发出 `execute_full` 的子类，都必须添加一行 `execute_full_async` 桥接：
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
没有桥接，`Command`/`Send`会在异步路径上被静默丢弃——这是2.0的潜在调度缺陷，3.0通过路由到同步路径来修复，代价是每个超级步骤产生一次`io_context`生成。所有树内子类（`deep_research_graph`、示例10/14/21、测试5处）现在都带有桥接。

### 性能

- 示例27的墙钟时间：**152毫秒 → 53毫秒**（在`io_context`线程上的3个智能体×50毫秒定时器步骤，完全重叠）。
- 在单次运行基准测试中没有可测量的性能回退； `run()` 仍然通过一个新的单线程 `io_context` 经由 `run_sync` 驱动同一个协程。

### 测试

- 115/115 tests pass
- 295/295 ASan+UBSan 通过
- Valgrind 在协程重度子集上干净（20 个测试，2.4 s）

### 发布后验证（当日）

- **所有30个示例已重新运行：** 26/29通过，0失败，3个受环境限制（clay_chatbot → raylib，postgres_react_hitl → docker compose，deep_research完整循环 → crawl4ai服务）。`21_mcp_fanout`测量为3次MCP调用/8毫秒墙钟——阶段4的重叠在真实网络I/O下保持。

- **ARM64兼容性（docker buildx --platform linux/arm64）：** 仓库根目录下的`Dockerfile.arm64-smoke`。ubuntu:24.04-arm64 + core+llm+async+sqlite+tests在QEMU仿真下构建完成约需15分钟；**306/306 ctest在ARM64上通过**。剥离后的二进制大小为0.81-0.88 MB（与x86_64几乎相同）。示例27在仿真下运行时间为65毫秒（原生x86_64：53毫秒）。确认Linux/ARM64与macOS beta（Apple Silicon）一起作为受支持的平台。

- **缓存局部性（Ryzen 5800X / Zen 3，Valgrind cachegrind，32 KB L1i/d 8路，32 MB L3 16路）：** `bench_concurrent_neograph`扫描N=1 → 10,000。

    | N | I refs | LLi 未命中 | LLi 未命中率 | 原生 p50 |
  |---:|---:|---:|---:|---:|
    | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
    | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
    | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

末级指令未命中在N的4个数量级范围内保持平稳，约4,320次。唯一热代码工作集≈277 KB（L3的0.85%）。N=10,000时648 M条指令仅产生4,329次LL未命中——大约每150,000条指令1次未命中。本地p50纯粹因I-cache预热从17 µs降至5 µs。"突发并发鲁棒性"定位的首个实测证据。

---

## [3.0.0] — 2026-04-22

3.0 移除了 Taskflow 依赖，并在单一的 asio 协程路径上统一了同步和异步超步执行。图定义 JSON、节点 ABI、检查点模式以及公共入口点（`run`, `run_async`, `run_stream`, `resume`）与 2.0 保持源代码兼容；破坏性变更仅限于 `GraphNode` 子类，这些子类仅从 **sync** `Command`/`Send` 。 `execute_full` 覆盖中发出

### 破坏性

- **`deps/taskflow/`和Taskflow INTERFACE目标已移除。** 同步超级步循环、`run_one`、`run_parallel`、`run_sends`以及进程范围的`tf::Executor`静态变量已被删除。通过NeoGraph的include路径`#include <taskflow/...>`的下游消费者必须单独引入Taskflow。
- **`GraphNode::execute_full_async`默认现在通过直接调用桥接到同步`execute_full`（无`co_await execute_async`）。** 这保留了从仅同步覆盖中发出的`Command`/`Send`——这是2.0的常见模式——通过所有入口点现在共享的异步路径。需要非阻塞I/O和`Command`/`Send`的异步原生节点必须直接覆盖`execute_full_async`；文档字符串自2.0以来一直如此说明，但2.0从未实际执行过，因为同步`run()`完全绕过了协程路径。
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` 同步方法已移除。** 请使用 `_async` 对等节点。
- **CPU 并行 fan-out 为可选启用。** 此前 Taskflow 默认提供进程级线程池。在 3.0 中，`run_parallel_async` 和 `run_sends_async` 的多 Send 分支会根据驱动协程的执行器进行分支调度——由同步 `run()` 启动的单线程 io_context，或调用方自己的执行器（用于 `run_async()`）。I/O 密集型 fan-out 仍可重叠（单线程上的 co_await 挂起）；CPU 密集型 fan-out 会串行化，除非调用方为 `run_async()` 使用多线程执行器，或通过 `engine->set_worker_count(N)` 选择启用引擎拥有的线程池。

### 已添加

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 与现有单线程 `run_sync` 并行的 N 工作线程同步↔异步桥接。为调用启动一个新的 `asio::thread_pool`，以便内部 `make_parallel_group` 分支在独立工作线程上执行。
- `GraphEngine::set_worker_count(n)` — 可选启用的引擎拥有线程池，由 `NodeExecutor` 用于并行 fan-out 调度。重建执行器；必须在任何并发运行之前调用。

### Revised translation: 变更

- `GraphEngine::execute_graph` (sync) 已移除。所有入口点（`run`, `run_stream`, `resume`）均通过 `execute_graph_async` 经由 `neograph::async::run_sync` 路由，因此超级步循环、重试退避、检查点 I/O 和并行 fan-out 现在端到端地存在于同一条协程路径上。
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` 从 `tf::Executor` / `tf::Taskflow` 切换为 `asio::thread_pool` + `asio::post` 用于调用方侧驱动。

### 性能（Release 模式基准测试 -O3 -DNDEBUG，Linux 参考环境，10 次运行中位数）

- `seq` 引擎开销（3 节点链，计数器）：**每次调用约 5.0 µs**。
- `par` 引擎开销（5 工作线程 fan-out + 汇总器）：**每次调用约 11.8 µs**。
- 整个基准测试过程（预热 + 顺序 + 并行迭代）的峰值 RSS：**4.8 MB**。
- 与 LangGraph 1.1.9 在相同工作负载下的对比：**顺序执行快 131 倍，并行执行快 199 倍**（每次迭代）；RSS 约轻 12 倍。

本 CHANGELOG 的先前草稿将“约 46 µs 顺序 / 约 114 µs 并行”列为 3.0 回归。这些数字来自 `CMAKE_BUILD_TYPE` 未设置的构建树，因此基准二进制文件在未启用 `-O3 -DNDEBUG` 的情况下编译。在正确的 Release 构建中，异步对等节点折叠相比 2.0 的 Taskflow 同步路径是**改进**（2.0 README 在同一主机上宣称 20.65 µs 顺序 / 150.7 µs 并行）。修正后的图表见 [`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png)。

### 迁移

- 如果您的节点覆盖 `execute()` / `execute_async()` 且不发出 `Command` / `Send`，则无需操作。
- 如果您覆盖同步 `execute_full` 以发出 `Command` / `Send`：无需更改——3.0 异步路径默认现在直接调用您的同步覆盖。`Command.goto_node` 路由通过同步和异步入口点均可工作。
- 如果您覆盖 `execute_async`（异步原生 I/O）且需要 `Command` / `Send`：请直接覆盖 `execute_full_async` 并在其中组装 `NodeResult`。仅覆盖 `execute_async` 会静默丢弃 `Command` / `Send`，因为默认的 `execute_full_async` 现在通过同步 `execute_full` 路由，而非异步 `execute_async`。
- 如果您依赖 Taskflow 的进程级池通过 `engine->run()`进行 CPU 并行 fan-out：请在 compile() 之后调用 `engine->set_worker_count(N)` 一次，或通过 `run_async()` 在您自己的多线程 `asio::thread_pool` / io_context 上驱动引擎。

---

## [2.0.0] — 2026-04-22

首个包含 Stage 3 异步 API 的公开版本。这是一个破坏性版本；以下变更影响编译（C++ 标准）和 ABI（抽象基类新增了异步对等接口）。同步调用点逐位保留，因此**未覆盖 `Provider` / `CheckpointStore` / `GraphNode` / `Tool` 的应用程序代码无需修改即可继续工作**。

### 破坏性

- **需要 C++20。** 公共 API 暴露了 `asio::awaitable<T>` 返回类型，这些类型需要 `std::coroutine` 支持。使用者必须使用 `-std=c++20` （或更高版本）进行编译。已测试 GCC 13+、Clang 15+；参见 `docs/ASYNC_GUIDE.md` §4.1 了解 GCC 13 协程的解决方法。
- **已移除 libpqxx 依赖。** `neograph::postgres` 现在直接链接 libpq。Ubuntu 24.04 用户不再遇到 libpqxx-7.8t64 的 C++17/C++20 ABI 分裂所引入的 `pqxx::argument_error::argument_error(..., std::source_location)` 链接错误。CMake 查找现在针对 `PostgreSQL::PostgreSQL`（CMake 自带的 FindPostgreSQL）。仅安装了 `libpqxx-dev` 的使用者现在还必须安装/保留 `libpq-dev`。
- **`Provider`、`CheckpointStore`、`GraphNode`、`MCPClient` 的 ABI 已扩展。** 每个都增加了异步对等虚函数（`complete_async`、`save_async`、`execute_async`、`rpc_call_async` 及其变体）。下游子类需针对 2.0 头文件重新编译；除非子类希望提供原生异步覆盖（任何执行真实 I/O 的实现者都建议这样做），否则源码无需更改。
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list` / `delete_thread` 不再是纯虚函数。** 它们现在有默认实现，通过 `_async` 桥接到匹配的 `neograph::async::run_sync`对端。覆盖同步侧的子类继续正常工作；未提供任何覆盖的子类（以前会导致编译错误）现在会无限递归——约定：每个同步/异步对至少覆盖一个。

### 已添加

- **异步 API** 覆盖所有 I/O 层（完整参考见 `docs/ASYNC_GUIDE.md`）：
  - 基类和所有内置提供者（OpenAI、Schema、RateLimited）上的 `Provider::complete_async`。
  - HTTP 和 stdio 传输的 `MCPClient::rpc_call_async`。stdio 使用 `asio::posix::stream_descriptor`。
  - 所有八个同步方法的 `CheckpointStore::*_async`。
  - `GraphNode::execute_async` + stream / full / full_stream 变体，带有异步原生交叉默认值。
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async` 驱动 `execute_graph_async`——一个端到端的协程超级步骤循环，包括通过 `asio::experimental::make_parallel_group` 的并行 fan-out。
  - `neograph::AsyncTool` 适配器，用于希望使用协程体同时保留同步 `Tool` 接口的用户工具。
- **`neograph::async` 命名空间** — HTTP 客户端、连接池、SSE 解析器、run_sync 桥、URL 端点拆分器。参见 `include/neograph/async/*.h`。
- **新示例**:
  - `examples/27_async_concurrent_runs.cpp` — 单个 `io_context` 上的多个智能体。
  - `examples/05_parallel_fanout.cpp`（重写）— 使用 `run_parallel_async` 在单次图运行内进行异步 fan-out。
- **CI 基准回归门禁**（`.github/workflows/ci.yml`）— PR 检查对 `bench_async_http` / `bench_async_fanout` / `bench_neograph` 强制设置下限。

### 性能

在 feat/async-api 分支上对照 Stage 2 同步基线测得:

- `bench_async_http --mode async_pool --concur 1000`：6064 ops/s → **17834 ops/s**（2.9×）。
- `bench_async_fanout --concur 50000`：线程每智能体不可实现 → **541K ops/s / 67 MB RSS**。
- `examples/27_async_concurrent_runs`（3 × 50ms 异步工作）：150ms（同步）→ **50ms**（1 个 io_context 线程）。
- `examples/05_parallel_fanout`（3 × 100-150ms 异步工作）：370ms（顺序）→ **150ms**（1 个 io_context 线程）。
- `bench_neograph` 引擎开销：不变（约 30 µs 顺序 / 约 205 µs 并行）。协程机制不会使热路径退化。

### 尚未包含在 2.0.0 中

- **Taskflow 依赖**保留。同步 `engine.run()` 路径仍使用它进行 fan-out；Sem 4.5 重新审视同步路径是否可以被 `run_sync(*_async)` 替换，以便完全移除该依赖。

### 跨平台

2.0.0 中支持三个平台，处于不同的稳定性层级。该层级反映的是平台在发布前经历的真实世界验证程度 — 而非功能覆盖范围（代码库通过 `#ifdef _WIN32` 拆分实现单一来源；一旦测试通过，各平台功能等同）。

#### Linux — **GA**（生产就绪）

* Ubuntu 24.04，GCC 13。
* 本地全部 332/332 ctest 通过（Postgres 通过 docker `postgres:16-alpine`），且所有基准测试均处于已提交的 CI 下限之内。
* MCP stdio 基于 fork/pipe/execvp + `asio::posix::stream_descriptor`。
* Postgres 异步对端基于 libpq 非阻塞模式 + `asio::posix::stream_
  descriptor` 包装 `PQsocket`。
* 上文引用的每个性能数字的参考平台。

#### macOS — **测试版**

* macos-latest（Apple Silicon），通过 Xcode 使用 Clang。
* CI 构建并运行非 Postgres 测试；Postgres 集成用例在无服务容器时自行跳过。POSIX 路径（同一 fork/ pipe + asio::posix 代码）会被执行。
* `CoreFoundation` + `Security` 框架通过 httplib 链接，用于 TLS 上的系统证书加载。
* 建议视为测试版，直至2-4周的持续集成运行及用户报告确认无运行时行为差异（协程调度、SIGPIPE/EPIPE 形态、管道缓冲区大小）。若这些运行无异常，则定向推广至正式版(GA)。

#### Windows — **内部版**

* windows-latest, MSVC 19.44 (VS 2022), x64。
* CI 范围：**仅 core + async + MCP + LLM**。Windows CI 任务中禁用 Postgres 和 SQLite 后端，因为 vcpkg 每次运行都会从源码编译 OpenSSL / libpq / zlib / lz4（约 20 分钟，自 `x-gha` 被移除后上游没有可用的二进制缓存后端）。Windows 用户通过自己的 vcpkg / choco 配置在本地编译这些组件。
* OpenSSL 通过运行器预装的 choco 包（`C:/Program Files/OpenSSL-Win64/`）。httplib + asio::ssl 中的 TLS 路径可编译并链接。
* MCP stdio：`CreateProcess` + 命名管道（FILE_FLAG_OVERLAPPED）+ `asio::windows::stream_handle`。重叠管道路径依据 MSDN 规范编写，未经本地 Windows 验证；预计首批用户会暴露边界情况（ERROR_IO_PENDING 处理、大型 JSON 响应上的管道缓冲区边界）。
* Postgres 异步对端（本地启用时）：`asio::ip::tcp::
  socket::assign` 包装 `PQsocket` 返回的 SOCKET（通过 `native_handle_type` 转换以保留 64 位 SOCKET 值）。Windows CI 不执行此路径——仅限本地。
* 协程机制位于 MSVC 的 `<coroutine>` 中；按规范行为应与 GCC/Clang 一致，但 `examples/27` 跨运行重叠测量尚未在 Windows 上确认。
* 在 2.0.0 之前视为 **alpha**。一旦一个生产用户运行多智能体工作负载一周而未遇到 stdio/管道或协程调度器问题，并且 Postgres 异步对等端由愿意运行 vcpkg 完整 libpq 构建的用户本地验证，则提升为 beta。

> **模式**：CI 绿色是底线，而非上限。第 3 层运行时
> 行为差异（协程调度时序、管道缓冲区
> 边界、套接字接管语义）仅在真实
> 工作负载下才会显现。上述层级语言让用户对每个平台
> 有正确的预期，而不是假装三者
> 在第一天就可互换。

### 提升后修复

- **`async::HttpResponse` 头映射** — 响应面现在暴露一个 `headers` 的 `(name, value)` 对向量，保留线上顺序和原始大小写，外加 `get_header(name)` 作为不区分大小写的访问器。Retry-After 和 Location 仍作为专用字段保留以保持向后兼容。解除了下面 MCP 会话跟踪修复的阻塞。
- **MCP `Mcp-Session-Id` 头跟踪** — Sem 2.6 httplib→async_post 迁移静默丢弃了此功能。现在每个 post-initialize RPC 都通过新的头访问器回显服务器分配的会话 ID，因此服务器的会话状态保持可路由。
- **MCP stdio 可等待互斥锁** — `StdioSession::rpc_call_async` 使用 `std::mutex`，当同一单线程 io_context 上的两个协程调用同一会话时会发生死锁（第二个的 `lock_guard` 阻塞了第一个所需的 worker）。已替换为 `asio::experimental::channel<void(error_code)>` 容量为 1 的信号量，使第二个获取者协作式挂起。
- **`PostgresCheckpointStore` 异步对等** — 全部八个 CheckpointStore 异步方法（`save_async`、`load_latest_async`、`load_by_id_async`、`list_async`、`delete_thread_async`、`put_writes_async`、`get_writes_async`、`clear_writes_async`）现在均为真正的异步。内部实现：`PQsetnonblocking(1)` + `PQsendQueryParams` + `asio::posix::stream_descriptor` 基于 `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)`。在 4 个槽位的池上并发调用四次 `save_async`，现在会在线缆层面并行执行提交 fsync，而不是通过 `run_sync` 串行化。

---

## [0.1.0] — 2026-04 之前

预发布开发版本。不提供公共 API 稳定性保证。
