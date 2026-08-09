<!-- neograph-i18n: source=CHANGELOG.md locale=zh-CN source_sha256=fe44de5a1daaa7973fe032ec711d496702f2576bed6ce0431e64250b63eb676e -->
# 更新日志

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

NeoGraph 的所有显著变更均记录于本文件。

格式遵循 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)。
版本号遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

---

## [未发布]


### 新增
- **OpenRouter 提供商路由。** `OpenAIProvider` 现在会将
  `CompletionParams::extra_fields.provider` 中传入的对象作为 `provider`
  转发到 Chat Completions 请求体；非对象值会在发出 HTTP 请求前失败。
  这公开了 OpenRouter 已文档化的逐调用路由偏好，同时仍会忽略其他原生
  `extra_fields` 键。实时 Beast cookbook 会固定提供商，并为 4,000-token
  生成预算使用显式的 180 秒超时。

- **Copy Ninja 本地图节点桥接。** 新增无 transport 的 `a2a::CopyNinjaNode`：
  它包装独立 materialize 的 Copy Ninja harness，读取 `prompt` 并 overwrite
  `response`。同时新增 live cookbook `cookbook_the_beast_copy_ninja`：其 LLM
  只能编写该固定 local node，须在常规 Core gate 之后通过第四个 local-binding
  gate；若合成 source agent 观察到 RPC，运行即失败。Card text、endpoint、
  credential 和 source 仍被排除在 unadmitted candidate 之外，caller prompt
  不会进入 authoring LLM request。


- **可选 Program 组件边界。** 新增了选择启用的
  `NEOGRAPH_BUILD_PROGRAM` 开关、导出的 `neograph::program` 目标以及
  `<neograph/program/program.h>` 入口。安装包仅在构建 Program 时报告该
  组件；仅 Core 安装会保持现有的 `neograph::core` 链接接口。

- **不可变 Program 值模型。** 新增稳定的类型化诊断、深度拥有的 canonical
  JSON/C++ 构建器 `ProgramSource` 输入、不可变的内容寻址
  `ProgramBundle`/`ProgramVersion` 值、规范序列化、带 SHA-256 算法标签的
  标识、源映射、import 以及严格的版本化存储值模式。`neograph::program`
  现在是仅依赖 Core 的已编译导出库。
  Bundle/version v1 投影现要求封存的 Core 定义与计划标识、带语义版本的可执行项
  摘要、契约、闭包、边界以及类型化的准入/物化回执。标识会绑定格式和存储版本，
  语义集合按稳定顺序规范化；诊断会拒绝无效指针、反向 span 和未知 enum，并在
  无精确解析器偏移量时保持 span 为空。

- **封存的 Program 准入闭包。** 新增不可变的 `RegistrySnapshot`、
  `AdmissionProfile` 和 `PolicySnapshot` 值，支持在构建器阶段捕获可调用对象、
  严格的规范清单及域分离指纹；`ProgramVersion` 会以故障关闭方式校验跨对象
  指纹一致性。Core 新增了用于 Program 物化的显式仅本地
  parse/link/validate 入口，现有本地优先/全局回退重载保持不变。
  注册表条目现以规范形式记录精确的可执行对象依赖边，用于传递式准入闭包；
  仅本地条件检查也会覆盖旧版键值 edge 文档，且不会查询进程全局注册表。

- **单根 `call_core` Program 编译器。** 新增 `ProgramCompiler`，仅接受封闭的
  Program-v1 信封，在封存前纯执行仅本地 Core parse/round-trip/validation，
  并输出带 RFC 6901 指针和源映射归属的聚合类型化诊断。编译过程不会调用
  factory 或 callable，而是确定性派生 canonical Program、注册表、传递式
  可执行项闭包、capability/effect、import Merkle、封存定义及 Core 计划标识。
  同时提供创作文档模式、完整有限预算契约、zero-dispatch 拒绝测试，以及静态和
  共享安装使用者验证。Core 新增 total parse/round-trip 和仅本地 validation
  报告，同时保持现有抛异常 API 的行为不变。

- **固定 Program 运行时垂直切片。** 新增 `ProgramCatalog`、
  `EngineGenerationCache`、`ProgramRuntime`、共享 `ProgramHandle`、不可变
  `ProgramResult`、类型化 Program 事件信封、内存 `ProgramStore` 以及仅追加
  CAS `ProgramJournal`。准入会在物化前重新计算不可信 bundle 的语义；每次
  attempt 固定一个不可变 Core generation，并且只调用现有 `GraphEngine`
  异步路径。完成、中断、精确 checkpoint 恢复、取消、超时、Core 步数耗尽、
  checkpoint 不兼容和失败均映射为类型化终态，同时保留不可补充预算与
  checkpoint 谱系。Journal 提交先于 checkpoint/terminal 事件交付；并发恢复
  仅允许一个 CAS 胜者；在 Core broker 就绪前拒绝 effectful 或非空 schema
  Program。

- **SQLite Harness 记录存储（issue #147 后续）。** 新增了可选的
  `neograph::mcp_sqlite` 目标和 `SqliteHarnessRecordStore`，用于 WAL 支持、
  带模式版本的工件/运行持久化，具有不可变工件和运行到工件绑定。
  Harness MCP 二进制文件现在将记录存储在 `runs.db` 中，而检查点仍存储在
  `checkpoints.db` 中。
- **AMD OpenMP GPU 卸载概念验证。** 新增可选的
  `bench_openmp_offload` 基准，在相同数值扇出工作负载上比较串行 CPU、
  OpenMP 自动线程、逐次映射 GPU 和 GPU 常驻数据四种模式。它分别报告真实
  设备执行与主机回退、计算正确性、包含传输的延迟、仅内核延迟以及相对串行
  CPU 的加速比。Radeon AI PRO R9700 可通过
  `NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201` 启用 ROCm/Clang 设备镜像。


### 变更

- **C++ ABI 与 SOVERSION 策略（issue #194）。** 所有公开
  `neograph_*` 二进制库现在都带有项目 `VERSION` 和主版本
  `SOVERSION`，安装后的共享库会从自身目录解析同级依赖。v1 之前使用 ABI
  代次 0，但可以公布强制重新构建边界。包含 bounded `NodeCache` 的版本改变了
  `NodeCache` 和 `EngineConfig` 的公开对象布局，因此所有基于 `0.11.1` 或
  更早版本构建的 C++ 使用者都必须重新构建。1.0 将 ABI 代次改为 1，并冻结
  v1 布局。CI 现在构建并运行隔离的静态和共享安装使用者，并检查 ELF/Mach-O
  加载器元数据。详见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)。
- **`GraphNode::run(input)` 迁移指南完成。** Python `GraphNode` 基类不再
  引用已删除的 `execute*` 方法；当缺少 `run(input)` 时，会引发包含迁移
  文档路径的 `NotImplementedError`。C++/Python 参考文档、异步/流式指南
  和示例 README 均已与实际 v0.9.0 单一入口点对齐。迁移步骤在
  [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md) 中
  以 C++ 和 Python 示例进行了文档化。
- **Provider API 永久兼容性策略（issue #5）。** 原先计划移除
  `Provider::complete()`、`complete_async()`、`complete_stream()`、
  `complete_stream_async()` 和基于回调的 `invoke()` 的决定已撤销，且
  `[[deprecated]]` 警告已被移除。现有 API 继续获得兼容性和安全性修复。
  新的 Provider 实现和直接调用者推荐分别使用
  `CompletionProvider::do_invoke()` 和 `invoke_request(CompletionRequest)`；
  不保证将所有新功能回移植到现有 API。公开签名、虚函数顺序、对象大小
  和虚函数表保持不变。

### 移除

- **已弃用的 TransformerCPP 集成示例。** 移除了 `example_inproc_gemma`、
  `NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE` 和 `TRANSFORMERCPP_DIR`，这些
  依赖一个不再可用的外部托管仓库。保留了使用标准 OpenAI 兼容本地服务器
  的 `example_local_transformer`。

### 修复

- **Harness 聚合发现来源（issue #174）。** 详情现在包括与现有扁平
  `findings` 数组对齐的 `finding_sources` 数组。每个条目记录其聚合索引、
  来源工作器 ID 和工作器本地索引，而不更改经过模式验证的工作器输出或
  已建立的 `findings` 形态。
- **Harness 导出结果检查（issue #173）。** 节点效果约定现在可在可选的
  `exports` 数组中声明写入的通道，当调用者在图执行后消费它们时使用。
  因此 Harness 编译和 `GraphEngine` 运行时验证对 `final_result` 上真正
  只写通道保留 E6 检查，而不会发出误报警告。
- **MCP 2025-11-25 工具-客户端约定现代化（issue #147 M0）。** 初始化现在
  是幂等的，并保留协商的服务器元数据；HTTP 工具复用发现会话；`/mcp`
  端点构造由请求和通知共享；工具发现遵循不透明游标；且 JSON-RPC
  code/data、完整工具元数据、非文本内容、`structuredContent`、`isError`
  和 `_meta` 在 C++ 和 Python 路径中均能保留。添加了可配置的 HTTP
  超时/静态/动态头、输出模式验证、严格响应 ID 检查和类型化的
  `InitializeResult`、`ToolDefinition`、`ListToolsPage` 和
  `CallToolResult` API。SSE 检测现在使用 `Content-Type`，而非将包含
  `data:` URL 的 JSON 错误分类。
- **每任务取消状态和已发布发送生命周期安全。** `GraphEngine::run`、
  `run_async`、`run_stream`、`run_stream_async` 各自从调用者提供的父令牌
  为每次运行创建一个执行子令牌，仅将该子令牌绑定到内部 `co_spawn`/
  同步桥接，并将相同的子令牌作为 `RunContext` 传递。在单个父令牌下取消
  所有并发运行因此不能相互覆盖取消槽。分叉的执行子令牌通过已发布的发送
  保留现有 `shared_ptr` 所有权，防止引擎工作完成和发送执行之间的释放后
  使用。由取消引起的 asio `operation_aborted` 被传播为
  `CancelledException` 而非可重试的节点错误。`CancelToken` 0.11.x 对象
  布局和内联/仅头文件行为不变。已编译的 C++ 消费者需要重新编译以接收
  更新的 `fork()` 生命周期行为。仅替换共享库保持了对象布局兼容性，但
  嵌入消费者二进制文件中的现有内联函数体不会改变。但是，当外部代码在
  自行创建的令牌上调用 `bind_executor()` 时，调用者仍负责保持令牌存活
  直到执行器的已投递工作完成。
- **PostgreSQL 异步连接全局超时策略已文档化。** 异步初始连接和替换使用
  跨所有主机/IP 地址的单个超时。直接在正向连接字符串中写入的显式
  `connect_timeout` 强制执行至少 2 秒；未指定、零、负数或仅环境变量/
  服务文件的值使用操作安全默认值 30 秒。这与 libpq 的每主机同步超时有意
  不同；同步创建/替换行为不变。
- **JARVIS mock 构建修复（issue #130）。** 修复了当音频依赖缺失时，
  `MicCapture` 作为不完整类型导致的 `cookbook_jarvis` 编译失败。添加了
  `NEOGRAPH_JARVIS_FORCE_MOCK`，使 ASan CI 无论运行器安装了什么包都始终
  构建 mock 配置。会话运行器现在使用实际的 CMake 输出路径和 specialist
  目标名称，并正确启动现有的 `demo_mcp_server.py`。
- **节点失败上下文保留（issue #123）。** C++ 执行错误被传播为
  `NodeExecutionError`，包含原始 `exception_ptr`、失败节点名称和尝试计数；
  终端 `ERROR` 事件也记录相同的上下文。在 Python 中，原始异常对象、类型、
  参数、用户属性和回溯按原样保留，仅添加 `.node_name` 和 `.attempts`
  属性。`NodeInterrupt`、取消和内存不足异常遵循现有控制流而不被包装。

### 文档修复

- **从 Provider Cookbook 中移除被忽略的每节点提示（issue #116）。** 修复了
  三个使用 `config.system` 描述多角色行为但内置 `llm_call` 不读取该字段的
  Python 示例。每个示例被重写为使用 `NodeContext.instructions` 的严格单次
  调用图，相关 README 与真实行为对齐。
- **保留的 `RunContext::deadline` 文档修正（issue #115）。** 修正了将
  `deadline` 和 `trace_id` 呈现为可用每运行元数据的文档和 Doxygen 注释，
  而实际上它们无法通过 `RunConfig` 设置，也不在 Python 中暴露。
- **`GraphNode::run` 示例签名修复（issue #129）。** 修复了公共头文件示例
  中接受 `const NodeInput&`（按引用）但无法重写实际按值虚函数的问题，
  并通过编译时测试锁定了协程参数生命周期所需的按值约定。

### 新增

- **向后兼容的 Provider 迁移路径。** 新增的 `CompletionRequest` 将流式模式
  与回调存在性分离，`CompletionProvider` 要求新实现仅编写 `do_invoke()`。
  现有 `Provider` 虚函数表、四个旧虚函数、基于回调的 `invoke()` 和
  Python `complete()` 子类约定均被保留。

- **Python 持久化后端**（#117）— `Store` 和 `CheckpointStore` 现在是可
  构造的子类基类，支持从 Python 到 C++ 的虚函数分发。`StoreItem`、
  `CheckpointPhase`、`Checkpoint` 和 `PendingWrite` 以 JSON 形态字段暴露；
  检查点 pending-write 方法保持可选。
- **Python 同步取消**（#119）— Python 调用者可以构造 `CancelToken`，将其
  赋值给 `RunConfig.cancel_token`，并从另一个线程协作式地停止
  `engine.run()`。

- **Python 检查点历史**（#118）— `GraphEngine.get_state_history()` 暴露
  最新优先的检查点记录，使调用者能够从历史状态分叉之前检查父链接、元数据、
  步骤和 ID。

- **DSL 表面（展开层）+ 模式演化门禁**（#75 M4）。
  - **展开器**：`vars`（`{"$var":...}` / `${...}` 插值，无环强制）/
    `templates`+`use`（精确参数匹配强制，节点前缀重命名——包括局部引用、
    屏障和路由；通道是共享状态所以全局合并）/ `when` 条件包含。
    **非图灵完备且完全的**：每个 DSL 文档在有限时间内归一化为唯一核心且
    对该核心是幂等的。所有错误以 DSL 源码坐标（`use[2].args`、
    `vars.model`）和源映射（输出位置 → 生成语法）报告。锁文件工作流：
    `./example_elaborate harness.dsl.json > harness.json`（示例 53）。
  - **`GraphCompiler::upgrade_to_latest()`**：无损 v0→v1 机械转换——
    严格模式拒绝的键被隔离到 `x-upgraded-<key>` 注释命名空间（零数据
    删除），空屏障被显式移除。整个语料库经过测试以保证"旧宽松编译 IR ==
    升级后严格编译 IR"（规范等价关系，版本戳除外）。
  - **模式演化门禁**：对 `tests/fixtures/schema_snapshot.json` 基线的仅
    新增子集判断（JSON 子模式家族的可判定子集）——节点类型/属性/reducer/
    条件的移除、必需集增加、封闭条件标签变更和效果约定变更均导致测试失败 =
    CI 合并阻止。不兼容变更强制在同一审查提交中进行版本升级 + 升级器 +
    快照重新生成。

- **PBT / delta 验证工具**（#75 M3）。300 种子的确定性拓扑生成器（来自
  模式封套的有效严格文档，自检测特征覆盖——当 conditional_edges/barrier/
  interrupt 出现率低于 30% 时测试失败：未测试特征变成失败，而非静默空洞）。
  - **变异检测**：在 300 种子语料库上确认，翻译验证捕获所有 5 种丢弃类型
    （conditional_edges/edge/barrier/interrupt/channel）+ 3 种错接类型
    （路由折叠 / 边重定向 / 节点重命名 = 丢弃+伪造反平衡）的每次应用。
    应用率下限（10% 的种子）也被断言。
  - **引用解释器 delta**：一个独立模型，从代码分离的实现中重新实现文档化
    的超级步骤语义（goto 抢占、屏障累积、字典序回退、隐式 __end__），
    在 12 步 × 300 图上与 Scheduler 进行比较（DESIL 教训：单独的验证器
    无法捕获错误执行）。
  - **引擎 ↔ Studio 共享语料库**：`tests/fixtures/topology_corpus/` 15 个
    变体（3 个有效 + 12 个违反 E3–E11）与 NeoGraph-Studio
    `tests/corpus/` 逐字节相同，两者断言相同判定（code:severity 多重集）—
    两个实现无法静默发散。

- **GraphValidator — 拓扑静态语义检查（E3–E11 + 效果）**（#75 M2）。
  解析（M1）和执行之间的通过层。在严格文档（schema_version>=1）中错误
  是编译失败，警告是 stderr lint；在宽松文档中只有错误级诊断作为 stderr
  警告出现（对现有图为零噪音）。判断哲学 = 检查器健全性优先：只有在引擎
  语义下永远不可能正确的事物才是错误（悬垂引用 E3、无信号路径的屏障 E8 —
  goto 绕过屏障记账因此不可恢复、空路由 E10 — 分发将解引用 rend() UB、
  未声明通道写入 E4 — 确认在运行时抛出）；Command.goto/Send 可以证明的
  事物是警告（可达性 E7、无逃逸循环 E11、无屏障的普通扇入 E9、覆写竞态
  E5、死亡通道 E6）。每个诊断都附带机器可读的见证（反例）JSON — 用于
  Studio 画布高亮（M3）。
  - **路由完整性（E10）**：`ConditionSpec` 标签约定引入。通过
    `register_condition` 3 参数重载声明条件的输出标签集，要求封闭条件路由
    与标签精确匹配——未覆盖的标签落入调度器的"字典序最后路由"回退（顺序
    依赖的任意目标），这是错误。内置 `has_tool_calls` = 封闭 {false,true}、
    `route_channel` = 开放 + 已知 {default}。
  - **通道效果约定**：`register_type` 4 参数重载声明每节点类型的读/写
    通道。E4/E5/E6 分析仅当图中**每个**节点类型都已声明时才激活（单个
    未知类型跳过整个分析——健全性优先于覆盖）。内置 3 种类型（llm_call/
    tool_dispatch/intent_classifier）完全声明。
  - `node_effects` · `condition_specs` 添加到 `export_schema()`（现有
    `conditions` 数组为向后兼容保留）。22 个新测试。

- **拓扑编译时一致性门禁 — 消费键记账 + 翻译验证**（#75 M1）。双重机制，
  结构性地阻止"静默语义丢失"类别（与 v0.1.0–v0.1.7 的 `conditional_edges`
  静默丢弃同种）：
  - **消费键记账**：声明 `"schema_version": 1` 的文档切换到严格编译——
    未消费的键（拼写错误 `conditionnal_edges`、不支持的字段、因空
    `wait_for` 被静默丢弃的屏障、内联条件中被忽略的 `to`）全部被收集并
    报告为编译错误。标记在解析块**内部**发生，因此擦除解析阶段也擦除标记，
    导致使用这些特征的严格文档立即失败——一种丢弃回归不能静默的结构。
    `_`/`x-` 前缀的键（`_comment`、`x-studio-*`）始终被允许作为注释
    命名空间。没有 `schema_version` 的现有文档保持宽松行为（字节保留）。
  - **翻译验证**：`CompiledGraph::to_json()` 重新发射 + `GraphCompiler::canon()`
    正规形式检查 `canon(input) == canon(re-emit)` 在每次编译时进行。
    不匹配（= 编译器丢弃了某些东西或错接了线路）在严格文档上抛出，在
    宽松文档上 stderr-warn。等价是结构比较——交换路由键等错接也被捕获
    （存在比较遗漏的类别）。
  - `NodeFactory::config_schema(type)` 查询添加，`schema_version` 字段在
    `export_schema()` 中文档化。27 个新测试
    （`tests/test_compiler_strict.cpp`）——包含 v0.1.x 丢弃变异模拟
    （conditional_edges/barrier/interrupt 丢弃 + 路由错接）。

## [0.11.1] - 2026-06-25

### 变更

- **stdio MCP 并发调用 — 用于 I/O 重叠的相关性 ID 解复用器。** `0.11.0`
  并发工具分发实际上只对 HTTP MCP 实现了重叠。stdio MCP 在
  `StdioSession::rpc_call_async` 中为**整个请求→响应往返**持有一个容量为 1
  的通道锁，在一轮中通过单个会话管道串行化多个调用（挂钟时间 ≈ 延迟之和）。
  单个管道不是根本原因——JSON-RPC `id` 正是为了在一连接上流水线化而存在。
  将锁替换为相关性 ID 解复用器：
  - 将容量为 1 的通道重新用作**仅写锁**——仅在帧写入的瞬间持有，因此两个
    调用的字节绝不交错，而读取不再被串行化。
  - 单个读取协程（`run_reader`）独占拥有读取侧，并通过 JSON-RPC `id` 将
    每个响应行交付给正确的调用者的接收器。N 个并发调用重叠读取，因此挂钟
    时间 ≈ max(latency) — 但**仅当对端 MCP 服务器并发处理时**（单线程
    串行服务器触及阿姆达尔地板）。
  - 读取器仅在存在进行中调用时惰性运行，并在等待者为空时退出，因此私有
    `run_sync` io_context 正常返回。等待者仅在其调用者正在等待时存在并
    通过 `MCPTool` 的 `shared_ptr` 保持会话存活，因此读取器从不触及已销毁
    的会话（不需要析构函数 join）。在管道 EOF/错误时，读取器关闭所有
    接收器，因此等待的调用者接收异常而非无限挂起。
  - **无 API/语法变更** — 公共头文件不变，现有代码无需重新编译。引擎开销
    回归 0（`bench_neograph` 交叉 A/B，seq/par Δ 0%）。
  - 测试：基于线程的延迟夹具 `tests/fixtures/mcp_stdio_slow.py` +
    `ConcurrentStdioCallsOverlapIO`（5 个 × 100 ms 调用在 ~130 ms 内完成
    对比 500 ms 串行地板；验证每个响应通过 `id` 路由到其调用者）。
    ASan+UBSan ×3 干净。

## [0.11.0] - 2026-06-25

### 新增

- **并发工具分发 — `Tool::execute_async` 正式异步路径。**
  `ToolDispatchNode` 使用引擎的 `make_parallel_group` **并发**执行来自单个
  assistant 轮的多个 `tool_call`。以前每次调用通过同步 `execute()` 串行
  运行，尤其是 MCP 工具每次调用通过 `run_sync` 生成自己的 `io_context`
  从而阻塞，防止并行 MCP 调用重叠（在一个外部 C++ 分支中发现此问题，该
  分支有并行 MCP 调用）。修复：
  - 虚拟 `execute_async()` 添加到 `Tool` — 默认实现桥接到同步 `execute()`，
    因此现有工具不变地工作。
  - `MCPTool` 转换为具有原生 `execute_async` 的 `AsyncTool`（stdio 使用
    `rpc_call_async`，HTTP 使用新的 `MCPClient::initialize_async`/
    `call_tool_async` 进行异步握手 — `run_sync` 已移除）。
  - `ToolDispatchNode::run` 通过相同的 `make_parallel_group` 惯用法并发
    分发调用，与节点扇出相同（单次调用内联），结果按调用顺序应用。通过
    同步 `execute()` 门面向后兼容。
  - 验证：478/478 ctest、Valgrind 0 泄漏、TSAN 0 竞态。

### 修复

- **Python 异步执行异常保留（issue #122）。** 修复了 `run_async`、
  `run_stream_async` 和 `resume_async` 用将其包装为字符串的新
  `RuntimeError` 覆盖原始 Python 节点异常的问题。现在原始 Python 异常对象、
  类型、用户属性和回溯通过 pybind11 的标准异常转换路径保留，且 C++
  `py::type_error` 作为 Python `TypeError` 传递，匹配同步执行。
  `resume_async` 中的空回调现在保留到协程完成，也修复了 pybind11 3.x 中
  暴露的悬垂引用冲突。

### 文档修复

- **README 摘要徽章根据缺失条件和沙箱测量揭示的内部矛盾进行了修正。**
  "四大维度"摘要表徽章从正文/深度解析中剥离了测量条件，读起来像夸大之词。
  修正为与正文测量数据和条件对齐（测量数据表本身不变）：
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — 徽章的
    17 µs 与正文矛盾（`At N=10,000 concurrent ... 7 µs p99`），且 `flat`
    描述的是 GPU 绑定的负载测试运行延迟（648 ms），而非 µs 测量。徽章与
    正文测量数据和条件对齐。
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`** — 仅
    `libc.so.6` 和 1.2 MB 仅在 MinSizeRel + 静态 libstdc++ 构建下成立
    （默认 Release 动态链接 libstdc++/libgcc_s/libm/libc）。条件已在深度
    解析 §size 中文档化，已恢复到徽章中。
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** —
    直接依赖确实仅 `certifi` + `pydantic`（两个），但实际安装树是 7 个
    包，包括 pydantic 传递依赖（pydantic-core, typing-extensions,
    annotated-types, typing-inspection）。
- **向深度解析 MinSizeRel 复现命令添加了 `-DNEOGRAPH_BUILD_POSTGRES=OFF`。**
  PostgreSQL 默认为 ON，因此在没有 libpq 的主机上按原样运行 configure
  会失败。已修复。

## [0.10.0] — 2026-05-20

### 新增

- **串行扇出一次性 stderr 警告（issue #62, PR #63）。** `compile()` 的
  默认值是 `set_worker_count(1)` — 扇出分支在调用者的执行器上串行执行，
  不持有引擎拥有的线程池。这种预期行为对仅依据文档构建了多 Send 图的用户
  看起来像静默串行执行。在 `NodeExecutor` 第一次调度多 Send（或多出边）
  扇出而没有池时，向 stderr 添加了一次性指导消息。`std::atomic` +
  compare-exchange 保证即使在并发扇出下也只发送一次。调用
  `set_worker_count(N>=2)` 会重建 `NodeExecutor`，自然地重置标志。可
  通过环境变量 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`（或 `true` / `yes`）
  抑制 — 用于有意的 worker=1 串行执行、基准测试和 CI stderr 断言情况。
  由 5 个 Linux + macOS 单元测试覆盖（`test_fanout_worker_warning.py`）：
  触发 / 一次性 / 池加入静默 / 环境变量静默 / 单 Send 无警告。Windows：
  pytest capfd 与 wheel 二进制中 MSVC CRT 文件描述符缓存不兼容，因此在
  模块级别跳过 — wheel 二进制 stderr 输出本身正常。

- **拓扑 JSON Schema 导出 — `NodeFactory::export_schema()`**（issue #56，
  无代码可视化块编辑器的前置条件）。将引擎消费的拓扑 JSON 格式作为机器
  可读的 schema（JSON Schema Draft 2020-12）整体导出：`{ neograph_version,
  $schema, topology（固定信封）, node_types, reducers, conditions }`。
  一个独立仓库的块编辑器从此 schema 自动生成其调色板 → 编辑器和引擎不能
  跨版本漂移。完全为纯新增：
    - `NodeFactory::register_type(type, fn, json config_schema)` 3 参数变体
      添加。现有 2 参数委托给宽松默认 schema — 现有用户节点/调用不受影响。
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` 查询访问器添加。
    - 为 4 种内置类型（`llm_call`/ `tool_dispatch`/`intent_classifier`/
      `subgraph`）声明了配置 schema。`NEOGRAPH_VERSION` 作为编译定义暴露
      （pyproject.toml 为单一真源）→ schema 版本戳。
    - `examples/52_export_schema.cpp`（`example_export_schema`）：
      `./example_export_schema > schema.json` — 编辑器仓库 CI 产生固定到
      NeoGraph 版本的工件的标准路径。
    - Python：`neograph_engine.export_schema()` → dict（编辑器仓库 CI 在
      `pip install neograph-engine` 后转储）。
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4。关键：
      顶级 `conditional_edges` 在 loader→compile 往返中存续（针对
      v0.1.0–v0.1.7 静默丢弃重现的回归守卫）。

### 修复

- **拓扑顶级容器格式验证（#126）。** `channels`/`nodes` 必须是对象；在所有
  模式下若不然则拒绝。`edges`/`conditional_edges` 数组验证在严格模式中
  强制，旧键控边映射兼容性保留。错误记录路径和 JSON 类型，而非完整输入。
- **`max_steps` 终止状态暴露（#114）。** `RunResult::max_steps_exhausted()`
  和只读 Python 属性 `RunResult.max_steps_exhausted` 添加。仅当达到
  `max_steps` 但仍有节点待执行时为 True；相同状态在 gRPC 单次响应和流式
  最终 JSON 中提供。C++ 结构体大小不变。

- **`set_worker_count` / `set_worker_count_auto` docstring 修正（issue #62,
  PR #63）。** v1.0 准备周期有意将 `compile()` 工作池默认值从
  `set_worker_count(hardware_concurrency())` 恢复为 `set_worker_count(1)`
  （参见 `src/core/graph_engine.cpp:69-93` 注释中的理由），但四个面向用户
  的 docstring 保留了旧的声明 → 依据文档构建了多 Send 扇出图的用户获得了
  单线程上的静默串行执行。单元测试不可见（假 spawn，即时体）；仅在真实
  挂钟端到端中暴露。
  - 重写了 `bindings/python/src/bind_graph.cpp` 中两个 `set_worker_count`
    / `set_worker_count_auto` Python docstring 以匹配真实行为：
    `compile()` 默认值为 1，`set_worker_count_auto()` /
    `set_worker_count(N>=2)` 是显式加入。
  - 相应地修正了 `include/neograph/graph/engine.h` 中的两个 Doxygen 注释。
    Doxygen Pages 在 master 推送时自动重新生成。
  - 修正了 `docs/concepts.md` / `docs/troubleshooting.md` /
    `docs/reference-en.md` 中相同的陈旧声明（default = hardware_concurrency）。

- **补充了 v0.9.0 发布中遗漏的三个 API 迁移。** v1.0 准备周期中的 PR `9b`
  （`19819d8`）破坏性地移除了 `GraphNode` 旧的 8 虚函数链，但 PR `#48`
  （`6e654ad`，"C++ 示例迁移到 `GraphNode::run()`"）仅迁移了 `examples/`
  — 以下 3 个文件被遗漏，使 v0.9.0 以构建损坏状态发布：
    - `benchmarks/stress/bench_sustained_concurrent.cpp`（第三阶段
      持续-爆发验证关键基准测试）
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp`（针对 LangGraph
      和其他引擎的内存/并发比较矩阵主体）
    - `wasm/smoke.cpp`（第一阶段 WASM 可行性冒烟）

  CI 未将这些目标作为 add_executables 拾取，或（Docker 构建依赖）将其
  隔离在独立环境中，因此合并到 master 和标签通过。

  **修复**：所有三个从 `std::vector<ChannelWrite> execute(const GraphState&)
  override` 迁移到 `asio::awaitable<NodeOutput> run(NodeInput in) override`
  + `co_return out` 模式。节点逻辑不变。

  **v1.0 关键卖点原生重新验证**
  （`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`）：
    - 并发 10K · 挂钟 10–23 ms · p99 17–21 µs · 峰值 RSS **5.6 MB**
      （匹配 v0.3.0 / v0.5.0 测量 — 破坏性 9b 后无内存卖点回归）
    - 10K 时 0 错误
  **Docker 矩阵（LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6 路比较）在同一会话中也重新测量**
  （`results_v0.9.0_docker_recheck.jsonl`）。

  在矩阵重新运行期间，在缺失的 API 迁移旁边发现了一个独立的回归 —
  `benchmarks/concurrent/Dockerfile.neograph` 根本不能构建，因为它在
  master 上未能跟踪 CMake 选项默认值变更（在 v0.9.0 发布时相同）。
  随时间推移，以下选项默认值从 OFF 翻转为 ON：
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      （分别需要 `libpq-dev` / `libsqlite3-dev`）
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL`（一个先前事件在 `feedback_libcurl_unconditional_dep.md`
      中关闭 — 仅添加了选项切换而默认值保持 ON，再次破坏了空容器构建路径）
    - `find_package(OpenSSL REQUIRED)` 是无条件的，没有选项切换
      （CMakeLists.txt:256）— 单独的 v1.0 清理候选

  **Dockerfile 修复**：`libssl-dev` apt 添加 + 所有非核心选项用显式
  `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF` 锁定。注释注明
  "因两次漂移事件而显式冻结"。`find_package(OpenSSL REQUIRED)` 在
  CMakeLists.txt 中的条件化留作单独任务 — 需要验证其他构建路径的影响
  （PyPI wheel、ARM64 等）。

  **6 路矩阵关键结果**（concurrency=10000，2 cpus / 1 GiB）：

  | engine          | mode          | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
  | **neograph**    | threadpool    | **16**  | **18**      | **5.1** | 10000/0 |
  | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
  | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
  | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
  | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
  | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

  NG 对 LangGraph（营销比较轴）：挂钟**快 237×**，p99 **快 4134×**，
  峰值 RSS **低 12×**。

  **严苛场景**（concurrency=10000，1 cpu / 512 MiB）：
    - NG：8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8：7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio：OOM killed**（超出 512 MB cap）
    - **AutoGen asyncio：OOM killed**

  与 v0.3.0 / v0.5.0 测量相同 — **破坏性 9b 后 NeoGraph 的"10K 并发
  工作器，峰值 RSS 5 MB，无 OOM"卖点无回归。**

## [0.9.0] — 2026-05-14 — v1.0 预备（候选 1 阶段 B + 候选 6）

ROADMAP_v1.md 的两个 v1.0 单一分发统一在一个周期中汇合：

  - **候选 1 阶段 B（`9b`–`9f`）** — 所有 `GraphNode` 的旧 8 个虚函数
    （`execute` / `execute_async` / `execute_stream` /
    `execute_stream_async` / `execute_full` / `execute_full_async` /
    `execute_full_stream` / `execute_full_stream_async`）+
    `add_cancel_hook` + `CurrentCancelTokenScope` + `state.run_cancel_token_`
    + 所有 6 个 `PyGraphNodeOwner` 旧重写被移除。**破坏性** — 弃用窗口
    已关闭。用户 GraphNode 子类 / 用户 Python 节点必须迁移到单一方法
    `run(NodeInput)` / `def run(self, input)`。
  - **候选 6** — `Provider` 4 虚函数笛卡尔积 → 1 虚函数 `invoke()`。
    仍处于新增 + 弃用阶段 — 旧 4 虚函数不变且功能正常，弃用警告仅可见。
    那一侧的阶段 B（`Provider` 旧移除）也在 v1.0.0 发布前关闭。

同一周期还包括 b59444f 的潜在并行回归回退（`e5ecb08`）+ 显式扇出示例
调用 + 3 个 CI 环境修复（httplib 宏守卫 / Windows MSVC unistd.h /
pybind pytest 迁移），所有这些均属于此 [未发布]。

### 新增

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 标准单一分发
  入口点。在一个方法中处理非流式（`on_chunk == nullptr`）和流式（提供
  `on_chunk`）。将先前的 4 虚函数笛卡尔积（`complete` / `complete_async` /
  `complete_stream` / `complete_stream_async`）合并为一个异步流式超集。
  默认实现转发到 4 旧虚函数，因此现有 Provider 子类不变地工作。6 个新
  ctest（`ProviderInvokeDefault`）。（PR #40）
- **`invoke()` 取消传播等效性** — 当 `params.cancel_token` 未设置且引擎
  线程局部作用域活跃时，`current_cancel_token()` 自动打戳。等效于旧同步
  `complete()` 行为（引擎内的节点体调用 `provider->invoke(params, ...)`
  自动接收运行图的取消信号）。3 个新 ctest（`InvokeCancelPropagation`）。
  （PR #43）
### 变更

- **引擎中所有内部 LLM 调用通过 `invoke()` 路由** — `LLMCallNode`、
  `IntentClassifierNode`（PR #41/#42）、`Agent::complete` /
  `Agent::run_stream`（PR #43）、`SupervisorLLMNode` /
  `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode`（PR #43）、
  `PlannerNode` / `ExecutorNode`（PR #44）。NeoGraph 内的 LLM 分发统一
  到单一表面。
- **C++ 示例迁移（2 文件）** — `31_local_transformer.cpp`、
  `cookbook/ai-assembly/member_server.cpp` 现在使用新的 `invoke()`。
  用户构建中无弃用警告。（PR #45）
- **`GraphEngine::compile()` 默认工作器数恢复为 1**（`e5ecb08`）。
  `b59444f` 是潜在的 18 天（2026-04-26 → 2026-05-13）并行微基准回归
  11.8 → 283 µs（24×）的根本原因 — 通过二分（11 个工作树并行）精确定位
  到该提交。从 v1.0 默认值=1（对 CPU 微小顺序/并行分发最优）；对于有意识
  的扇出，添加一行 `engine->set_worker_count_auto()` 以打开
  hardware_concurrency。为 5 个受影响的扇出示例（10/14/21/36 +
  deep_research_graph 构建器）添加了显式调用。参见 ROADMAP_v1.md 中
  "性能回顾"部分了解详情。

### 弃用

- **`Provider::complete` / `complete_async` / `complete_stream` /
  `complete_stream_async`** — 全部 4 个旧虚函数带有
  `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` 标记。
  旧方法在弃用窗口期间按原样工作。在 v1.0.0 中移除。内部转发器用
  `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 包裹，因此警告仅出现在面向用户的
  重写/调用点。（PR #44）

### 移除（候选 1 阶段 B — 破坏性）

- **`GraphNode` 旧 8 虚函数** — `execute(GraphState&)` /
  `execute_full(...)` / 6 个变体 + `ExecuteDefaultGuard` 递归守卫 +
  300+ 行默认链。全部移除。`run(NodeInput)` 是唯一的纯虚函数。
  （提交 `19819d8`）
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` 成员 + `cancel()` 钩子
  迭代** — `cancel.h` 仅保留 `fork()` + `cancel()` + `is_cancelled()` +
  `slot()`。（提交 `1d786a5`）
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local +
  `GraphState::run_cancel_token_` + 3 个访问器** —
  `RunContext::cancel_token` 是唯一的取消通道。`src/core/cancel.cpp` 清空
  为存根（文件本身是未来删除候选）。（提交 `9e8e956`）
- **6 个 `PyGraphNodeOwner` 旧重写** — pybind 跳板仅调用 `run(self, input)`。
  Python 用户代码也从 v0.9.0 起需要单一方法。（提交 `9e8e956`）
- **2 个过时的 pytest 文件** — `test_execute_stream_dispatch.py`（v0.3.2
  仅流式回退分发验证）+ `test_streaming_only_error_hint.py`
  （execute_full_stream 优先 — 在 v1.0 中无意义）。（提交 `4392fbb`）

### 修复

- **为 5 个扇出示例添加了显式调用** — 恢复了被 `e5ecb08` 的默认工作器计数
  回退所掩盖的真正并行意图：`examples/10_send_command.cpp`、
  `examples/14_plan_executor.cpp`、`examples/21_mcp_fanout.cpp`、
  `examples/36_classifier_fanout.cpp`、`src/core/deep_research_graph.cpp` 的
  `create_deep_research_graph()` 构建器现在调用 `set_worker_count_auto()`。
  验证：`classifier_fanout` 4.22× 加速（25.2 ms 串行 → 6.0 ms 并行）。
  （提交 `99c470b`）
- **`bench_async_http` httplib 宏守卫** — `bench_async_http.cpp` 通过
  `<neograph/async/conn_pool.h>` 包含 `<httplib.h>` 但
  `CPPHTTPLIB_OPENSSL_SUPPORT` 未定义，导致 ODR 守卫拒绝。向 CMake 目标
  添加了 `target_compile_definitions(... PRIVATE ...)`。（提交 `d4be42a`）
- **Windows MSVC `unistd.h` 缺失** — `test_schema_provider_extra_fields_
  temperature.cpp` 使用仅 POSIX 的 `mkstemps` + `close`，完全无法进行
  Windows 构建。用 `#ifndef _WIN32` 守卫包裹整个文件（覆盖由 Linux/macOS
  保证）。（提交 `3c49f12`）
- **16 个 Python 测试已迁移** — wheel CI pytest 在 28 个使用旧 `def execute
  (self, state)` 模式的节点类上命中 `AttributeError`。批量迁移到 `def
  run(self, input)`；流式节点获得 `input.stream_cb` None-守卫。
  （提交 `4392fbb`）

### 迁移（用户代码）

**Provider 调用（候选 6 — 弃用阶段）**

新代码：
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

4 个旧虚函数重写在弃用窗口期间继续工作，但 `-Wdeprecated-declarations`
警告在用户重写点可见。移除发生在 v1.0.0 之前；建议在弃用窗口内迁移。

**`GraphNode` 子类（候选 1 阶段 B — 破坏性）**

C++ 代码：
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

**扇出意图（工作器计数默认值变更）**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

`docs/migration-v0.4-to-v1.0.md` 中的迁移 1/2/3 部分（run() /
ctx.cancel_token / 工作器计数默认值）+ Provider 部分（在下次文档清扫中
添加）提供逐例前后指导。

## [0.8.0] — 2026-05-13 — DX 策略 + 下游驱动的 API 缺口解决

将真实下游（ProjectDatePop）反馈和内部覆盖差异暴露的 8 个 issues
（#22、#25、#26、#27、#28、#34、#35 + #16 后续）捆绑为单次小版本
升级。两个新的公共辅助类（`RunResult::channel<T>`、
`RunContext::store`）、11 个新的离线示例、
`docs/migration-v0.4-to-v1.0.md` 迁移指南，以及一个减少新用户前 30 分钟
摩擦的 5 项 DX 捆绑。

### 新增

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`**
  — 从结果中提取通道值的一行辅助函数。两种输出形态（嵌套
  `output["channels"][name]["value"]` 标准 + 由 `react_graph` 等构建器
  添加的扁平键）自动处理。9 个新 ctest。（Issue #25）
- **`RunContext::store`** — 节点体用一行 `in.ctx.store->get(ns, key)` 访问
  Store。旧模式（在 `NodeFactory` lambda 中捕获 `shared_ptr<Store>`）
  仍然工作 — 新代码仅需新形态。3 个新 ctest。（Issue #27）
- **`Provider::complete_stream` 非纯虚默认体** — 最小 mock / 测试夹具仅需
  重写 `complete()`。现有流式原生重写保持不变。2 个新 ctest。
  （Issue #22）
- **`neograph::json` 数组 `.front()` / `.back()`** — nlohmann 肌肉记忆
  模式（`msgs.back()["content"]`）现在可以编译。4 个新 ctest。
  （Issue #26）
- **11 个新的离线示例（41–51）** — `resume_if_exists_chat`、
  `custom_reducer_condition`、`store_personalization`、
  `request_queue_backpressure`、`cancel_token`、`node_cache`、
  `sqlite_checkpoint`、`openinference`、`async_tool`、`minimal`。
  全部 rc=0，无 API 密钥 / 外部服务依赖。填补了 27/53 个 `NEOGRAPH_API`
  类别中此前零引用之间的缺口。
- **`examples/51_minimal.cpp`** — 30 行入门示例，一个节点，无 LLM、无工具、
  无 mock provider。5 分钟内理解 NeoGraph 如何运行。
- **`docs/migration-v0.4-to-v1.0.md`** — 逐例前后 4 个示例 + 从
  `[[deprecated]]` 旧 8 虚函数链（`execute` / `execute_async` 等）→ 新
  `run(NodeInput) -> awaitable<NodeOutput>` 迁移的常见错误。也从
  `NEOGRAPH_DEPRECATED_VIRTUAL` 宏消息链接。
- **README "常见陷阱 5"部分** — 新用户前 30 分钟遇到的五件事
  （`channel<T>` 用法、`in.ctx.store`、`neograph::graph::` 子命名空间、
  `<httplib.h>` 宏、GCC 13 协程 ICE）集中在一处。每项都有修复 + 相关
  示例/issue 链接。
- **编译时 `#error` 守卫（`include/neograph/api.h`）** — 当用户 TU 在
  NeoGraph 头文件之前包含 `<httplib.h>` 而没有 `CPPHTTPLIB_OPENSSL_SUPPORT`
  时，编译失败并带有清晰消息 + 退出宏（`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`）。
  将旧的 #16 运行时 SEGV 提升为编译时失败。
- **`example_minimal` 5 个新的友好错误消息 ctest** — 约定锁定了
  `Unknown reducer` / `Unknown condition` / `Unknown node type` /
  `Write to unknown channel` 消息，在消息体中嵌入可用名称 + 注册方法 +
  故障排除链接。
- **`docs/troubleshooting.md` 4 个新条目** — Tracer 适配器 `close()` 挂起/
  崩溃（#24）、GCC 13 协程 ICE（#23）、友好错误消息指导（#22）、
  `RunResult::output` 形态（#25）。
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` 块** —
  显式文档化了适配器作者的原始指针陷阱。指向 `RecordedSpan` + 包装分离
  模式为正确方法。引用了现有 `tests/test_openinference_cpp.cpp::
  InMemoryTracer` + 新 `examples/49_openinference.cpp::PrintTracer`。
  （Issue #24）

### 修复

- **`SchemaProvider::build_body` 在 `params.tools` 为空时静默丢弃
  `extra_fields`。** 旧代码在 `if (!params.tools.empty())` 内把关
  `extra_fields` 应用，导致 `reasoning` 和 `response_format` 等核心模式
  字段在无工具调用时完全消失。修复：移到工具分支外，因此始终应用。3 个
  新 ctest。（Issue #34）
- **`temperature_path` 模式侧退出。** 推理模型（gpt-5.x、o-series）有互斥
  的 `temperature` 和 `reasoning.effort`，但模式无法声明"此 provider 不
  接受 temperature"，迫使每次调用采用 `params.temperature = -1.0f` 哨兵
  变通方法。修复：在模式中指定 `"temperature_path": null` 会使
  build_body 完全跳过它。4 个新 ctest。（Issue #35）
- **友好的 RuntimeError 消息** — `ReducerRegistry::get` /
  `ConditionRegistry::get` / `NodeFactory::create` 的 "Unknown <thing>:
  foo" 和 `GraphState::write` / `apply_writes` 的 `Write to unknown
  channel` 现在在消息体中嵌入可用名称 + 注册方法 + 故障排除链接。新手
  可以仅从消息确定下一步。
- **`SchemaProvider::complete_stream_async` HTTP/SSE 分支** 现在在长期专用
  的 `bridge_thread_` 上分发（旧：`Provider` 基类默认每次调用生成新的
  `std::thread`）。旧行为在冷线程局部解析器 / NSS 状态的 glibc
  `internal_strlen` 中触发 SEGV。WS 分支已是原生 co_await 所以不受影响。
  在等待者执行器上的 token 分发保留（PR #10 不变式）。（Issue #16）
- **`example/09_all_features.cpp`** Store 演示 — 添加了指向
  `examples/43_store_personalization.cpp` 的 docstring 指针，用于节点体
  读取模式。选项 2 — 选项 3（内联活动节点）将在 #27 的
  `RunContext::store` 落地后一起清理。（Issue #28）

### 文档

- `RunResult::output` 的标准形态（通道包装）及其与 `react_graph` 等构建器
  添加的扁平键投影的关系，在头文件 docstring 中文档化。推荐使用新辅助类
  （`channel<T>` / `channel_raw` / `has_channel`）。（Issue #25）
- `RunContext::store` 字段 `@brief` 块 — 两种接线模式（推荐 `in.ctx.store`
  / 旧工厂闭包捕获兼容）并排代码示例。（Issue #27）
- 两条路径在 `examples/43_store_personalization.cpp` 文件头注释中文档化。

## [0.7.0] — 2026-05-11 — C++ openinference + 异步流式桥接

在一次小版本升级中关闭了针对 v0.6.0 提起的四个 issues。头条：
`Provider::complete_stream_async` 默认实现从外部引擎协程内部 await 时不再
段错误（issue #4）— 位于 NeoGraph 前面的 SSE / 流式 HTTP 后端最常见的
形态。伴侣：v0.6.0 Python OpenInference 层的 C++ 等价实现，使 Phoenix /
Arize / Langfuse 以渲染 Python 追踪的相同方式渲染 C++ 驱动的追踪
（issue #9）。外加：Python OTel 分离噪音被静默（issue #2），且同
`thread_id` 并发运行 + `schema_mutex_` × on_chunk 锁定不变式现在已固定在
docstring 中（issue #6）。

### 新增

- v0.6.0 Python OpenInference 层的 C++ 等价实现（issue #9）。新的
  `neograph::observability` 模块涵盖两个部分：
  - `Tracer` / `Span` — 小型无依赖抽象接口，因此 NeoGraph 本身不引入
    opentelemetry-cpp。下游提供一个包装自己后端的适配器（OTel SDK、内存
    测试假货、日志记录器等）。4 个属性设置器（string、int64、double、
    bool — bool 有意重命名为 `set_attribute_bool`，因此 `const char*`
    字面量不会意外解析到它），加上用于流式 token 诊断的 `add_event`、
    status 和 `end()`。
  - `openinference_tracer(tracer)` — 打开一个 CHAIN 种类根 span，返回一个
    `OpenInferenceTracerSession`，其 `cb` 字段插入
    `engine.run_stream()`，并为每个节点打开一个 CHAIN 种类子 span，将
    `NODE_START`/`END` payload 填充到 `input.value` / `output.value`
    JSON blob 中，并将 `LLM_TOKEN` 事件记录为离散 span 事件。
  - `OpenInferenceProvider(inner, tracer)` — 包装任何 `Provider`，在每次
    `complete*` 调用上附加 OpenInference LLM 种类属性集
    （`llm.model_name`、`llm.invocation_parameters`、
    `llm.input_messages.{i}.message.{role,content}`、
    `llm.output_messages.0.message.{role,content}`、
    `llm.token_count.{prompt,completion,total}`）。流式重载还附加
    `llm.token` 事件和最终组装的 `output.value`。
  - 7 个等效性测试在 `tests/test_openinference_cpp.cpp` 中驱动
    `InMemoryTracer` 引用适配器 — 断言根 + 每节点 CHAIN span 层次结构、
    ERROR / INTERRUPT 状态显示、LLM_TOKEN span-event 记录、会话关闭时
    残余 span 清理、LLM provider 属性集、流式 token 事件和异常状态传播。

### 修复

- `Provider::complete_stream_async` 默认桥接不再在流持续期间阻塞等待协程的
  执行器。修复前默认为内联 `co_return complete_stream(...)`，这 (a) 为整个
  HTTP/SSE 接收循环挂起引擎的 `io_context` 工作线程 — 因此相同执行器上的
  其他节点协程停滞 — 且 (b) 对于 `SchemaProvider` 的 WebSocket Responses
  分支，另外在引擎工作器之上嵌套一个新的 `run_sync` io_context 通过
  `run_sync(complete_stream_ws_responses(...))`，当从外部
  `GraphEngine::run_stream_async` 内部调用时在共享 provider 状态上竞态并
  产生间歇性段错误。新默认实现为同步 `complete_stream` 生成一个专用工作
  线程，将每个 token 分发回等待者的执行器（因此用户的 `on_chunk` 与等待
  协程单线程运行 — 无重入），并通过一次性 `steady_timer.cancel()` 恢复
  协程。工作线程异常在等待者上重新抛出。`SchemaProvider` 添加了一个原生
  `complete_stream_async` 重写，对 WebSocket 路径通过直接 `co_await`
  `complete_stream_ws_responses` 甚至跳过了工作线程。`OpenAIProvider` 透明
  地受益于新的基类默认实现（无 WS 路径，无特殊情况）。
  `tests/test_provider_async_default.cpp` 中的两个新测试：
  `StreamAsyncBridgeDoesNotBlockExecutor`（一个并发 ticker 协程在流期间
  前进 + 块在等待者的线程上交付，而非工作器的线程）和
  `StreamAsyncBridgeRethrowsWorkerException`。（Issue #4）

- `openinference_tracer`：静默 OTel SDK 在每次关闭时发出的 `Failed to
  detach context` stderr 回溯，当追踪器用于 `engine.run_stream_async` +
  `StreamMode.ALL` 时发生。在 NODE_START 创建的 OTel contextvars token
  从一个不同的 `asyncio.Task`（NODE_END 回调从引擎的 continuation 触发，
  而非调用者的 task）被分离，因此 `Context.reset(token)` 引发
  `ValueError`；SDK 吞下了 raise 但仍然通过 `logger.exception` 路由完整
  回溯，污染生产日志而不影响语义。修复记录附加时的 (thread, task) 并在不
  匹配时跳过分离，外加在 `_safe_detach` 位于栈上时仅丢弃该消息的
  `opentelemetry.context` 上安装窄 `logging.Filter`。同步调用者和相同
  task 异步调用者仍然获得正确的 LLM-span 嵌套在节点 span 下。（Issue #2）

---

## [0.6.0] — 2026-05-07 — OpenInference 可观测层

关闭 LangSmith UX 差距。NeoGraph 已经发出 OTel 形态的 span（因此追踪流向
任何 OTel 后端）；此版本添加了 Phoenix / Arize / Langfuse 用于将追踪渲染
为聊天气泡 + token 计数 UI 而非扁平通用应用 span 列表的 LLM 特定属性层。
针对本地 Phoenix 容器端到端验证 — writer→critic 图产生具有模型名称、
提示/响应和 token 计数在 Phoenix UI 中可见的 6-span 层次结构（CHAIN 根
→ 节点 span → LLM span）。

### 新增

- `neograph_engine.openinference` 模块：
  - `openinference_tracer(tracer)` — 上下文管理器，镜像 `otel_tracer` 但
    用 `openinference.span.kind = "CHAIN"` 标记根 + 节点 span，并将节点
    payload 填充到 `input.value` / `output.value` JSON blob 中。
  - `OpenInferenceProvider(inner, tracer)` — 包装任何 `Provider`。在每次
    `complete()` 上打开一个标记为 `span.kind = "LLM"` 的 `llm.complete`
    子 span，捕获 `llm.model_name`、
    `llm.invocation_parameters`、
    `llm.input_messages.{i}.message.{role,content}`、
    `llm.output_messages.0.message.{role,content}`、
    `llm.token_count.{prompt,completion,total}` 和 Langfuse 兼容的
    `input.value` / `output.value` blob。
- 4 个测试在 `bindings/python/tests/test_openinference.py` 中 —
  InMemorySpanExporter 断言属性存在、span 层次结构、异常路径和节点输入/
  输出 JSON blob。

### 修复

- `openinference_tracer` 现在将每个节点 span 附加为 OTel *current* 上下文
  （通过 `otel_context.attach`），使节点体内打开的子 LLM span 嵌套在其
  节点 span 下。没有这一步，跨越 C++→Python pybind 回调边界的 contextvar
  传播产生每次运行 3+ 个无关 trace_id，而非预期的单一追踪树。token 在
  NODE_END / ERROR / INTERRUPT 时分离以恢复先前的当前 span。与现有
  `otel_tracer` 文档的模式相同 — 显式 attach/detach，而非
  `trace.use_span(...).__enter__()`（在没有匹配 `__exit__` 的情况下使用
  不安全）。

### 备注

- OpenTelemetry 仍然是可选依赖。仅当 `opentelemetry-api` 未安装时，
  首次使用导入 `neograph_engine.openinference` 才会引发清晰的
  ImportError；而非在导入时。
- Phoenix 端到端运行：:

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

  将 OTLP gRPC 导出器配置到 `http://localhost:4317` 并打开
  `http://localhost:6006` 查看追踪。模块 docstring 中有完整片段。

---

## [0.5.0] — 2026-05-07 — 绑定易用性：实时变异列表属性

关闭了通过绑定暴露的消息 / 写入 / sends 列表上最自然的 Python 惯用法的
静默无操作陷阱。此前 `params.messages.append(msg)` 变异一个副本，底层
C++ vector 从未看到新项 — 优雅失败（无崩溃、无警告），产生退化的 LLM
回复。现在 `.append()` 推送到实时 std::vector。

### 新增

- `bindings/python/src/opaque_types.h` — 为五种向量类型
  `PYBIND11_MAKE_OPAQUE`：`std::vector<ChatMessage>`、`<ChatTool>`、
  `<ToolCall>`、`<graph::ChannelWrite>`、`<graph::Send>`。
- `module.cpp` `init_opaque_vectors` — `py::bind_vector` 将每个注册为
  Python 类（`ChatMessageList`、`ChatToolList`、`ToolCallList`、
  `ChannelWriteList`、`SendList`），支持针对实时 C++ vector 的完整可变
  序列协议。
- `py::implicitly_convertible<py::list, …>` 为每个 — 旧构建-然后-赋值
  模式（`params.messages = [m1, m2]`）保持不变地工作；赋值自动将 Python
  列表转换为绑定类。
- `bindings/python/examples/23_evolving_chat_agent.py` — 每线程自演化聊天
  agent（实时 LLM）：agent 的 JSON 定义在每轮之间基于累积的对话历史被
  重写。演示跨演化的检查点恢复（先前消息存续）、`__graph_meta__` 审计
  通道模式，以及验证器边界（白名单节点类型、必需通道）。

### 变更

- `params.messages` / `.tools` / `chat_message.tool_calls` /
  `node_result.writes` / `.sends` 现在返回它们的绑定类而非普通 `list`。
  `len()`、迭代、`__getitem__`、`__setitem__`、`.append()`、`.extend()`、
  切片 — 全部表现得像 Python list。仅 `isinstance(x, list)` 返回 False。
  仓库 + 下游 grep 确认零此类 isinstance 调用点。
- `.github/workflows/nightly.yml` — 移除 `ops/s ≥ 600K` 门禁。在 4 次连续
  失败（`err=0` 且 `leak=false`）后，阈值（对本地硬件校准为 969K ops/s）
  在共享 GitHub 托管运行器上无法达到（测量值为 233~273K ops/s，3-4×
  低于本地）。吞吐量回归检测存活于 PR 时 `bench-regression` 作业
  （稳定硬件，以 µs 为单位的单次分发）。夜间浸泡的实际价值是 5 分钟内
  `err==0` + `leak_suspect==false` — 两者均作为硬门禁保留。

### 备注

- `ChatMessage.image_urls`（`std::vector<std::string>`）有意不迁移 —
  `vector<string>` 在绑定中使用太广泛，无法全局 OPAQUE 而不扫描每个调用点。
  已记录为剩余限制；v0.6+ 候选。

---

## [0.4.0] — 2026-05-05 — v1.0 预备：统一 `run(NodeInput)` 分发

v1.0 细化轨道的开幕发布（ROADMAP_v1.md）。8 虚函数 `GraphNode` 笛卡尔积
（`execute` / `execute_async` / `execute_full` / … /
`execute_full_stream_async`）折叠为单一标准方法：`run(NodeInput) ->
awaitable<NodeOutput>`。每次运行取消元数据从非通道集 `GraphState` 成员 +
线程局部暗通通道移动到一个显式 `RunContext` 参数。`deadline` 和
`trace_id` 仅作为保留扩展槽添加，不通过 `RunConfig` 填充。`CancelToken`
获得分层 `fork()`，因此多 Send 扇出工作器各自拥有一个私有信号，父节点的
`cancel()` 级联到该信号。

### 新增

- `RunContext`（`include/neograph/graph/engine.h`）— 显式每次运行元数据：
  可用 `cancel_token`、`thread_id`、`step`、`stream_mode`，加上保留的
  `deadline` 和 `trace_id` 槽。引擎将其传递至每次 `NodeExecutor::run` 调用。
  **PR 1，提交 `a473f0e`。**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 单一标准分发入口点。
  `NodeInput { state, ctx, stream_cb }`；`NodeOutput { writes, command,
  sends }`。默认体转发到旧 8 虚函数，使现有子类继续编译。
  **PR 2，提交 `607ce66`。**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 具有自己
  `cancellation_signal` 的子令牌。父 `cancel()` 级联到所有活动子令牌
  （并递归到孙子）。`run_sync(aw, parent_token)` 切换到
  `parent_token->fork()`，使每个嵌套操作绑定自己的槽 — 关闭 v0.3.x
  emit-vs-bind 竞态和多 Send 单处理器覆盖。
  v0.3.x `add_cancel_hook` 列表在弃用期间保持工作。
  **PR 3，提交 `897645c`。**
- `[[deprecated]]` 在旧 8 `GraphNode` 虚函数 + `add_cancel_hook` 上。
  内部调用点（graph_node.cpp 默认链、默认 `run()` 转发器）被新的
  `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 宏（`api.h` — GCC / clang /
  MSVC 可移植）包裹。重写已弃用虚函数的用户代码看到迁移警告；
  引擎内部保持干净。**PR 4，提交 `35a4517`。**
- `engine.get_state_view(thread_id) -> StateView` 现在是正式状态读取；
  原始字典 `engine.get_state(...)` 在 docstring 中软弃用（不发出警告 —
  原始字典保留为有效逃生口）。**PR 5，提交 `f31aa53`。**
- 7 个 C++ + 19 个 Python 示例迁移到 `run(NodeInput)`。冒烟运行与
  v0.3.2 输出逐位匹配。**PR 6a/6b，提交 `a2a24ef` / `0a76e3a`。**
- Pybind `PyGraphNodeOwner` 重写 `run(NodeInput)` 并分发到 Python 用户的
  `run` 方法（当定义时），否则回退到旧链。`RunContext` / `NodeInput` /
  `CancelToken` 暴露给 Python；`cancel_token` 可通过
  `input.ctx.cancel_token` 访问，无需线程局部暗通。
  **PR 7，提交 `4e186a5`。**
- `docs/reference-en.md` §6 GraphNode 折叠为单一 `run()`。§7 下新增
  RunContext + `fork()` 示例子节。README "与 LangGraph 的区别"增加
  "一个节点方法"条目。**PR 8，提交 `519a00b`。**
- 内置 C++ 节点（`LLMCallNode`、`ToolDispatchNode`、`RouteToNode`）迁移
  到 `run(NodeInput)` 重写。**PR 9a，提交 `d1070dc`。**
- 新手模式陷阱修复：README CMake 片段文档化了 `graph::` 子命名空间、
  cppdotenv 路径、`OpenAIProvider::create()` vs `create_shared()`、
  `neograph::json` 作为 nlohmann 子集、3 参数 vs 2 参数 `compile()`。
  Python `compile(def, ctx, store=None)` 关键字参数添加（纯新增，非破坏）。
  **提交 `ee11ed6`。**

### 变更

- README："10K-worker measured stress test"部分 — RTX 4070 Ti + Gemma 4
  E2B Q4 在 neoclaw 上，N=10000 完成 @ 0 err / 424s / 2572 MB 峰值 /
  ~1 KB 边际工作器成本 / p99 648 ms（`7840b81`）。
- README："Production economics"部分 — 队列安全性 + RAM delta 框架
  （`b82b15a`）。
- README："No Docker required" + "Dependency-drift immunity"要点在
  LangGraph delta 列表中（`333b482`、`a6061d7`）。

### 弃用

- `GraphNode::execute / execute_async / execute_full / execute_full_async /
  execute_stream / execute_stream_async / execute_full_stream /
  execute_full_stream_async` — 在 v0.5.x 期间通过 `[[deprecated]]` 注释
  保持工作，在 v1.0 中移除。
- `CancelToken::add_cancel_hook` — 由 `fork()` 替代。相同弃用窗口。

### 备注

- 验证：442 → 452 ctest（3 个 NodeRunDispatch + 7 个 CancelTokenFork
  新增）+ 96 pytest + 5 个实时 LLM/WS 在 v0.4.0 标签通过。
- 一个子 PR（`run(const NodeInput&)` 引用参数）在 pybind 异步路径下触发
  了 v0.2.0 RunConfig 协程引用 UAF 崩溃形态。修复在合并前落地：
  `NodeInput in` 按值传递。在 `node.h` 中文档化。

---

## [0.3.2] — 2026-05-05 — 取消传播加固（5 轮）

五轮补丁系列，关闭 v0.3.0 单次取消暴露的缺口：Send 扇出传播、进程内轮询、
Python 钩子、C++ 作用域、异常类型。还落地了来自 FastAPI SSE 聊天演示评估
的 TODO_v0.3.md 反馈批次 — `resume_if_exists`、dict 或 list 的
`update_state`、用于类型化状态读取的 StateView。

### 新增

- `RunConfig::resume_if_exists` — 无需显式 `resume()` 调用即可选择恢复
  先前线程的检查点。标准多轮聊天语义：如果 `thread_id` 存在，
  `engine.run(cfg)` 继续对话。
- `engine.update_state(thread_id, dict | list[ChannelWrite], as_node="")`
  — 接受两种形态。修复前仅 `dict` 可工作；传入 list 会静默无操作。
  List 形式与每个节点体的发送形态对称。
- `StateView`（`bindings/python/neograph_engine/state_view.py`）—
  Pydantic 类型化状态读取。`engine.get_state_view(thread_id) ->
  StateView` 返回扁平点访问（`view.messages` / `view.foo`）加上用于字典
  逃生口的 `view.raw`。针对类型化通道定义进行子类化：
  `class ChatState(ng.StateView): messages: list[dict] = []`。
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` — 断言飞行
  中的取消确实在套接字层中止了每个由 Send 生成的兄弟（是 v0.3.1 根本
  原因补丁）。
- `examples/22_self_evolving_graph.py` — 随 TODO_v0.3.md #9 cookbook 合并
  移至 v0.3.2。
- ROADMAP_v1.md — 源自取消轮次事后分析的细化候选（单一分发、RunContext、
  分层 CancelToken — 全部在 v0.4.0 中交付）。
- Doxygen `/* */` 通配符修复 — `acp/types.h` 有包含路径通配符
  （`fs/*`、`terminal/*`）的 `/**` 块，打开了嵌套注释 + 抑制了所有后续
  诊断。用 `&#42;` HTML 实体替换。

### 修复

- 取消传播，5 轮累积：
  1. v0.3.0 单节点 — `cancel_token` 到达 `Provider::complete`。
  2. v0.3.1 多 Send 指针丢失 — 扇出工作器现在共享
     `run_cancel_token_shared()`（此前当 `init_state + restore` 在通道集
     外重建每工作器状态时丢失）。
  3. v0.3.1+ 进程内轮询 — 引擎超级步骤循环在步骤间轮询，而不仅仅在
     LLM I/O 时。
  4. v0.3.2 Python 钩子 — `add_cancel_hook` 在每次运行令牌上注册回调，
     在 `cancel()` 时触发。允许同步 Python `execute()` 无需线程局部作用域
     即可安装临时取消处理器。
  5. v0.3.2 C++ 作用域 + 重试 + 异常类型 — 在主线程上重新抛出
     `NodeInterrupt`（避免 libstdc++ `__exception_ptr::_M_release` 竞态），
     重试预算尊重取消，运行时 vs 逻辑异常拆分。
- 仅有 `execute_stream` 的 Python 节点静默进入了默认 `execute` 路径
  （NotImplementedError）。现在 `run_stream` 在用户仅重写了流式变体时直接
  连接 `execute_stream`。
- `update_state` 接受 list[ChannelWrite] — 关闭静默无操作（TODO_v0.3.md
  #5）。

### 备注

- 442 ctest + 96 pytest + 2 个实时 LLM（单个 + 扇出取消）在 v0.3.2 标签
  （`915e90e`）通过。
- 27/30 C++ 示例 + 20/22 Python 示例在 `examples/run_all.py` 下通过。
  跳过的测试需要外部服务（Postgres / Crawl4AI / 实时 OpenAI）。
- Valgrind 6 示例 0 错误，815 allocs / 815 frees 干净。
- Bench 中位数 5.185 µs/iter 在 seq 路径上（v0.3.0 基线）— 跨轮次零
  性能回归。

---

## [0.3.0] — 2026-05-04 — 协作式取消传播

关闭了 FastAPI SSE 聊天演示评估期间报告的生产成本泄漏缺口：前端
`AbortController` 取消 asyncio 任务不再留下上游 OpenAI 请求运行至完成。
取消在运行的每一层中传播。

### 新增

- `neograph::graph::CancelToken`（原子标志 + asio `cancellation_signal`）
  和 `CancelledException` — `include/neograph/graph/cancel.h`。
  协作式取消原语。通过 `RunConfig::cancel_token` 传递（可选
  `shared_ptr`）；引擎超级步骤循环在步骤间轮询 `is_cancelled()` 并抛出
  `CancelledException`。token 的 `cancellation_slot()` 绑定到运行的
  `co_spawn`，因此飞行中的 LLM HTTP 套接字操作在线上被中止（asio
  `operation_aborted`）。
- `CompletionParams::cancel_token` — 用于跨多个 `provider.complete()` 调用
  传递中止的用户的显式 pin。`Provider::complete` 读取它（或回退到由
  `PyGraphNode::execute_full_async` 设置的线程局部 `current_cancel_token()`）
  并将槽绑定到其内部 `run_sync` io_context，因此即使同步 Python 节点被
  取消命中也会停止计费。
- `GraphState::run_cancel_token()` — 由 pybind `PyGraphNode` 使用的每次运行、
  非序列化句柄，在同步 Python `execute()` 调用周围安装
  `CurrentCancelTokenScope`。这给予了同步 Python 用户无需更改其节点代码的
  透明取消传播。
- pybind `engine.run_async` / `run_stream_async`：asyncio `Future.cancel()`
  现在通过 `add_done_callback` 连接到 `CancelToken::cancel()`，且
  `co_spawn` 绑定 token 的取消槽。
- pybind 安全解析辅助函数 `_safe_set_future_result` /
  `_safe_set_future_exception` — 守卫通过 `call_soon_threadsafe` 投递的
  `future.set_result` / `set_exception` 调用，防止已取消 future 的
  `InvalidStateError` 风暴。
- `bindings/python/tests/test_async_cancel_live_llm.py` — 实时 OpenAI 端到
  端，断言 OpenAI HTTP 在 `Future.cancel()` 的 < 3 秒内完成（在实践中即时；
  修复前是约 7–8 秒不可取消的流式）。除非 `NEOGRAPH_LIVE_LLM=1`，否则
  跳过。
- `examples/22_self_evolving_graph.py` — 自演化图 PoC：`prompted_llm` 节点
  从 JSON 配置读取自己的提示，因此 LLM 重写器可以在运行间变异图定义并重新
  编译。演示 `0.0 → 0.4` 分数提升；文档化了重写器中通道流推理的缺口。

### 变更

- `Provider::complete(params)` 现在当 `params.cancel_token` 被设置或线程
  局部 `current_cancel_token()` 活跃时，将其内部取消槽绑定到其 `run_sync`。
  先前默认行为（无取消）为不老实的调用者保留。
- `neograph::async::run_sync` 获得可选的 `graph::CancelToken*` 参数；当
  非空时，绑定的 spawn 绑定 token 的槽。
- pybind `resolve_future_async` 通过安全解析辅助函数路由，而非直接通过
  `call_soon_threadsafe` 调用 `future.set_result`。

### 路线图（推迟到 v0.3.x — 见 `TODO_v0.3.md`）

- 同一 `thread_id` 的 LangGraph 风格自动检查点恢复。
- `run_async` 错误消息中的仅流式节点提示。
- `cb.emit_token(node, data)` 易用性辅助函数。
- README "与 LangGraph 的区别"部分。
- `update_state` 签名与文档对齐。
- `get_state` 扁平辅助函数 / Pydantic 访问器。
- `run_parallel_async` 和 `run_sends_async` 分支扇出中取消传播的实时验证。
- pgvector RAG 示例。

---

## [未发布] — 第四阶段

第四阶段关闭了异步路径上的最后一个 `run_sync` 跳跃。`run_async` 现在从
头到尾保持在调用者的执行器上：三个 50 ms agent 在一个 `io_context` 线程
上从 ~150 ms（串行）下降到 ~50 ms（重叠）——在
`examples/27_async_concurrent_runs` 中。

### 破坏性

- **`GraphNode::execute_full_async` 默认实现翻转为异步优先。** 它现在将
  `co_await execute_async(state)` 包装到 `NodeResult` 中，而非调用同步
  `execute_full(state)`。任何仅从同步 `execute_full` 重写发出
  `Command`/`Send` 的子类必须添加一行 `execute_full_async` 桥接：
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
  没有桥接，`Command`/`Send` 在异步路径上静默丢弃 — 2.0 潜在分发错误，
  3.0 通过以每超级步骤一个 `io_context` spawn 为代价路由到同步来修复。
  所有树内子类（`deep_research_graph`、示例 10/14/21、测试 5 个点）现在
  都带有桥接。

### 性能

- 示例 27 挂钟时间：**152 ms → 53 ms**（3 个 agent × 50 ms 定时器步骤在
  一个 `io_context` 线程上，完全重叠）。
- 对单次运行基准无可测量的回归；`run()` 仍然通过 `run_sync` 在新的单线程
  `io_context` 上驱动相同的协程。

### 测试

- 341/341 ctest 通过
- 295/295 ASan+UBSan 通过
- Valgrind 在协程重子集上干净（20 测试，2.4 s）

### 发布后验证（同日）

- **所有 30 个示例重新运行：** 26/29 通过，0 失败，3 个环境门控
  （clay_chatbot → raylib、postgres_react_hitl → docker compose、
  deep_research 完整循环 → crawl4ai 服务）。`21_mcp_fanout` 在 3 个 MCP
  调用 / 8 ms 挂钟上测量 — 第四阶段重叠在真实网络 I/O 下保持。

- **ARM64 兼容性（docker buildx --platform linux/arm64）：** 仓库根目录的
  `Dockerfile.arm64-smoke`。ubuntu:24.04-arm64 + core+llm+async+sqlite+
  tests 构建在 QEMU 仿真下在约 15 分钟内完成；ARM64 上 **306/306 ctest
  通过**。精简二进制大小 0.81-0.88 MB（与 x86_64 几乎相同）。
  示例 27 在仿真下运行需要 65 ms（原生 x86_64：53 ms）。确认 Linux/ARM64
  作为与 macOS beta（Apple Silicon）并列的受支持目标。

- **缓存局部性（Ryzen 5800X / Zen 3，Valgrind cachegrind，32 KB L1i/d
  8-way，32 MB L3 16-way）：** `bench_concurrent_neograph` 扫描 N=1 →
  10,000。

  | N | I refs | LLi misses | LLi miss% | Native p50 |
  |---:|---:|---:|---:|---:|
  | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
  | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
  | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

  最后一级指令缺失在跨越 4 个数量级的 N 中保持平坦在 ~4,320。唯一热代码
  工作集 ≈ 277 KB（L3 的 0.85%）。在 N=10,000 时 648 M 指令仅产生 4,329
  次 LL 缺失 — 大约每 150,000 条指令 1 次缺失。原生 p50 从 17 µs 下降
  到 5 µs，纯粹来自 I 缓存预热。"突发并发鲁棒性"定位的首次实测证据。

---

## [3.0.0] — 2026-04-22

3.0 移除了 Taskflow 依赖，并将同步和异步超级步骤执行统一到单个 asio 协程
路径上。图定义 JSON、节点 ABI、检查点 schema 和公共入口点（`run`、
`run_async`、`run_stream`、`resume`）与 2.0 源码兼容；破坏仅限于从**仅同步**
`execute_full` 重写发出 `Command`/`Send` 的 `GraphNode` 子类。

### 破坏性

- **`deps/taskflow/` 和 Taskflow INTERFACE target 已消失。** 同步超级步骤
  循环、`run_one`、`run_parallel`、`run_sends` 和进程级 `tf::Executor`
  静态变量已删除。通过 NeoGraph 的包含路径 `#include <taskflow/...>` 的
  下游消费者必须单独集成 Taskflow。
- **`GraphNode::execute_full_async` 默认实现现在通过直接调用桥接到同步
  `execute_full`（无 `co_await execute_async`）。** 这保留了从仅同步重写
  发出的 `Command`/`Send` — 常见的 2.0 模式 — 通过所有入口点现在共享的
  异步路径。需要非阻塞 I/O 和 `Command`/`Send` 的异步原生节点必须直接重写
  `execute_full_async`；docstring 自 2.0 以来一直这样说，但 2.0 从未实际
  运行它，因为同步 `run()` 完全绕过了协程路径。
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` 同步方法已移除。**
  使用 `_async` 对应版本。
- **CPU 并行扇出是可选的。** 此前 Taskflow 默认提供了进程级线程池。在 3.0
  中，`run_parallel_async` 和 `run_sends_async` 的多 Send 分支在驱动协程
  的执行器上分发分支 — 由同步 `run()` 启动的单线程 io_context，或由
  `run_async()` 使用的调用者自己的执行器。I/O 绑定扇出仍然重叠（在单个
  线程上 co_await 挂起）；CPU 绑定扇出串行执行，除非调用者在 `run_async()`
  中使用多线程执行器或通过 `engine->set_worker_count(N)` 选择加入引擎拥有
  的池。

### 新增

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 在现有单线程
  `run_sync` 旁边的 N 工作器同步↔异步桥接。为调用启动一个新的
  `asio::thread_pool`，使内部 `make_parallel_group` 分支在不同工作器上执行。
- `GraphEngine::set_worker_count(n)` — 由 `NodeExecutor` 用于并行扇出分发
  的可选加入引擎拥有的 thread_pool。重建执行器；必须在任何并发运行之前
  调用。

### 变更

- `GraphEngine::execute_graph`（同步）已消失。所有入口点（`run`、
  `run_stream`、`resume`）通过 `neograph::async::run_sync` 路由到
  `execute_graph_async`，因此超级步骤循环、重试退避、检查点 I/O 和并行
  扇出现在端到端住在一个协程路径上。
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` 从 `tf::Executor`
  / `tf::Taskflow` 切换到用于调用侧驱动的 `asio::thread_pool` +
  `asio::post`。

### 性能（bench_neograph Release -O3 -DNDEBUG 在参考 Linux 上，10 次运行中位数）

- `seq` 引擎开销（3 节点链，计数器）：**~5.0 µs** 每次调用。
- `par` 引擎开销（5 工作器扇出 + summarizer）：**~11.8 µs** 每次调用。
- 整个 bench 进程峰值 RSS（预热 + seq + par 迭代）：**4.8 MB**。
- 对相同工作负载的 LangGraph 1.1.9：每次迭代 seq **快 131×**，par **快
  199×**；RSS **轻约 12×**。

本 CHANGELOG 的早期草稿将"~46 µs seq / ~114 µs par"列为 3.0 回归。这些
数字来自 `CMAKE_BUILD_TYPE` 未设置的构建树，因此 bench 二进制文件在没有
`-O3 -DNDEBUG` 的情况下编译。在正确的 Release 构建上，异步对应版本折叠
相对于 2.0 的 Taskflow 同步路径是一个**胜利**（2.0 README 在同一主机上宣
称为 20.65 µs seq / 150.7 µs par）。修正后的图表位于
[`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png)。

### 迁移

- 如果您的节点重写 `execute()` / `execute_async()` 且不发出 `Command` /
  `Send`，无需操作。
- 如果您重写同步 `execute_full` 以发出 `Command` / `Send`：无需变更 —
  3.0 异步路径默认实现现在直接调用您的同步重写。`Command.goto_node`
  路由通过同步和异步入口点同样工作。
- 如果您重写 `execute_async`（异步原生 I/O）且想要 `Command` / `Send`：
  直接重写 `execute_full_async` 并在其中组装 `NodeResult`。仅重写
  `execute_async` 会静默丢弃 `Command` / `Send`，因为默认
  `execute_full_async` 现在通过同步 `execute_full` 路由，而非异步
  `execute_async`。
- 如果您依赖 Taskflow 的进程级池通过 `engine->run()` 进行 CPU 并行扇出：
  在 compile() 之后调用一次 `engine->set_worker_count(N)`，或在您自己的
  多线程 `asio::thread_pool` / io_context 上通过 `run_async()` 驱动引擎。

---

## [2.0.0] — 2026-04-22

首个包含第三阶段异步 API 的公开版本。这是一个破坏性发布；以下变更影响
编译（C++ 标准）和 ABI（抽象基类获得异步对应版本）。同步调用点逐位保留，
因此**不重写 `Provider` / `CheckpointStore` / `GraphNode` / `Tool` 的应用
代码保持不变地工作**。

### 破坏性

- **要求 C++20。** 公开 API 暴露需要 `std::coroutine` 支持的
  `asio::awaitable<T>` 返回类型。消费者必须用 `-std=c++20`（或更高）
  编译。GCC 13+、Clang 15+ 已测试；GCC 13 协程变通方法见
  `docs/ASYNC_GUIDE.md` §4.1。
- **libpqxx 依赖已移除。** `neograph::postgres` 现在直接链接 libpq。
  Ubuntu 24.04 用户不再遇到由 libpqxx-7.8t64 的 C++17/C++20 ABI 分裂
  引入的 `pqxx::argument_error::argument_error(...,
  std::source_location)` 链接错误。CMake find 现在针对
  `PostgreSQL::PostgreSQL`（CMake 内置 FindPostgreSQL）。仅安装了
  `libpqxx-dev` 的消费者现在也必须安装/保留 `libpq-dev`。
- **`Provider`、`CheckpointStore`、`GraphNode`、`MCPClient` ABI 已扩展。**
  每个获得了异步对应虚拟函数（`complete_async`、`save_async`、
  `execute_async`、`rpc_call_async` 及其变体）。下游子类应对 2.0 头文件
  重新编译；除非子类想提供原生异步重写（推荐任何执行真实 I/O 的实现者），
  否则源文件不变。
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list` /
  `delete_thread` 不再是纯虚函数。** 它们现在有通过
  `neograph::async::run_sync` 桥接到匹配的 `_async` 对应版本的默认实现。
  重写同步侧的子类保持工作；未提供任何重写（此前将是编译错误）的子类现在
  无限递归 — 约定：至少重写每个同步/异步对中的一个。

### 新增

- **异步 API** 跨所有 I/O 层（完整参考见 `docs/ASYNC_GUIDE.md`）：
  - `Provider::complete_async` 在基类和所有内置 provider（OpenAI、Schema、
    RateLimited）上。
  - 用于 HTTP 和 stdio 传输的 `MCPClient::rpc_call_async`。stdio 使用
    `asio::posix::stream_descriptor`。
  - 所有八个同步方法的 `CheckpointStore::*_async`。
  - `GraphNode::execute_async` + stream / full / full_stream 变体，具有
    异步原生交叉默认实现。
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async` 驱动
    `execute_graph_async` — 包含通过
    `asio::experimental::make_parallel_group` 并行扇出的端到端协程超级
    步骤循环。
  - 用于想要协程体同时保留同步 `Tool` 接口的用户工具的 `neograph::AsyncTool`
    适配器。
- **`neograph::async` 命名空间** — HTTP 客户端、连接池、SSE 解析器、
  run_sync 桥接、URL 端点拆分器。见 `include/neograph/async/*.h`。
- **新示例**：
  - `examples/27_async_concurrent_runs.cpp` — 单个 `io_context` 上的多个
    agent。
  - `examples/05_parallel_fanout.cpp`（重写）— 使用 `run_parallel_async`
    在单个图运行内的异步扇出。
- **CI bench 回归门禁**（`.github/workflows/ci.yml`）— PR 检查强制执行
  `bench_async_http` / `bench_async_fanout` / `bench_neograph` 的下限。

### 性能

在 feat/async-api 分支上对第二阶段同步基线的测量：

- `bench_async_http --mode async_pool --concur 1000`：6064 ops/s →
  **17834 ops/s**（2.9×）。
- `bench_async_fanout --concur 50000`：每个 agent 一个线程无法达到 →
  **541K ops/s / 67 MB RSS**。
- `examples/27_async_concurrent_runs`（3 × 50ms 异步工作）：150ms（同步）
  → **50ms**（1 io_context 线程）。
- `examples/05_parallel_fanout`（3 × 100-150ms 异步工作）：370ms（串行）
  → **150ms**（1 io_context 线程）。
- `bench_neograph` 引擎开销：不变（~30 µs seq / ~205 µs par）。协程机制
  不退化热路径。

### 尚未包含在 2.0.0 中

- **Taskflow 依赖** 保留。同步 `engine.run()` 路径仍使用它进行扇出；
  第四学期第 5 点重新审查同步路径是否可以被 `run_sync(*_async)` 替代，
  以便可以完全丢弃该依赖。

### 跨平台

2.0.0 中支持三个平台，处于不同的稳定性等级。等级反映了平台在发布前经历
了多少真实世界验证 — 而非功能覆盖（代码库是单一源码，带 `#ifdef _WIN32`
分割；一旦测试通过，各平台的功能是等价的）。

#### Linux — **GA**（生产就绪）

* Ubuntu 24.04，GCC 13。
* 本地 332/332 ctest 完全通过（Postgres via docker `postgres:16-alpine`）
  加上提交的 CI 下限内的所有基准测试。
* MCP stdio 在 fork/pipe/execvp + `asio::posix::stream_descriptor` 上。
* Postgres 异步对应版本在 libpq 非阻塞 + 包装 `PQsocket` 的
  `asio::posix::stream_descriptor` 上。
* 以上引用的每个性能数字的参考平台。

#### macOS — **beta**

* macos-latest（Apple Silicon），Cliang via Xcode。
* CI 构建 + 运行非 Postgres 测试；Postgres 集成用例在没有服务容器时自跳过。
  POSIX 路径（相同的 fork/pipe + asio::posix 代码）被实践。
* `CoreFoundation` + `Security` 框架通过 httplib 链接用于 TLS 上的系统
  证书加载。
* 视为 beta，直到 2–4 周的 CI 运行和用户报告确认没有运行时行为差异
  （协程调度、SIGPIPE / EPIPE 形态、管道缓冲大小）。一旦这些反馈无事故
  积累，目标推广为 GA。

#### Windows — **alpha**

* windows-latest，MSVC 19.44（VS 2022），x64。
* CI 范围：**仅 core + async + MCP + LLM**。Postgres 和 SQLite 后端在
  Windows CI 作业上被禁用，因为 vcpkg 将在每次运行中从源码编译 OpenSSL /
  libpq / zlib / lz4（约 20 分钟，自 `x-gha` 被移除后没有可用的二进制
  缓存后端上游）。Windows 用户通过自己的 vcpkg / choco 设置本地编译这些。
* OpenSSL 通过运行器预装的 choco 包（`C:/Program Files/OpenSSL-Win64/`）。
  httplib + asio::ssl 中的 TLS 路径编译和链接。
* MCP stdio：`CreateProcess` + 命名管道（FILE_FLAG_OVERLAPPED）+
  `asio::windows::stream_handle`。重叠管道路径是依据 MSDN 规范编写的，
  没有本地 Windows 验证；预计首批用户会发现边缘情况（ERROR_IO_PENDING
  处理、大型 JSON 响应的管道缓冲区边界）。
* Postgres 异步对应版本（在本地启用时）：`asio::ip::tcp::socket::assign`
  包装 `PQsocket` 返回的 SOCKET（通过 `native_handle_type` 转换以保留
  64 位 SOCKET 值）。不由 Windows CI 实践 — 仅本地。
* 协程机制运行在 MSVC 的 `<coroutine>` 中；行为预期按规范与 GCC/Clang
  匹配，但 `examples/27` 跨运行重叠测量尚未在 Windows 上确认。
* 通过 2.0.0 视为 **alpha**。一旦一个生产用户运行多 agent 工作负载一周
  而没有遇到 stdio/pipe 或协程调度器问题，且 Postgres 异步对应版本由愿意
  运行 vcpkg 完整 libpq 构建的用户本地验证，晋升为 beta。

> **模式**：CI 通过是下限，而非上限。第三层运行时行为差异（协程调度
> 时序、管道缓冲区边界、套接字接管语义）仅在真实工作负载下出现。以上
> 等级语言为每个平台设置了正确预期，而非假装三者从第一天起即可互换。

### 发布后修复

- **`async::HttpResponse` headers map** — 响应表面现在暴露一个按线路顺序
  和原始大小写保留的 `(name, value)` 对 `headers` 向量，加上作为不区分
  大小写访问器的 `get_header(name)`。Retry-After 和 Location 保留为向后
  兼容的专用字段。解除了以下 MCP 会话跟踪修复的障碍。
- **MCP `Mcp-Session-Id` header 跟踪** — 第二学期第 2.6 点 httplib→
  async_post 迁移静默丢弃了此项。现在每个 post-initialize RPC 通过新的
  headers 访问器将服务器分配的会话 ID 回显，使服务器的会话状态保持可路由。
- **MCP stdio 可等待互斥锁** — `StdioSession::rpc_call_async` 使用
  `std::mutex`，当同一单线程 io_context 上的两个协程调用同一个会话时
  死锁（第二个的 `lock_guard` 阻塞了第一个需要的工作线程）。替换为
  `asio::experimental::channel<void(error_code)>` 容量为 1 的信号量，因此
  第二个获取者协作式挂起。
- **`PostgresCheckpointStore` 异步对应版本** — 全部八个 CheckpointStore
  异步方法（`save_async`、`load_latest_async`、`load_by_id_async`、
  `list_async`、`delete_thread_async`、`put_writes_async`、
  `get_writes_async`、`clear_writes_async`）现在是真正的异步。内部：
  `PQsetnonblocking(1)` + `PQsendQueryParams` + 在 `PQsocket()` 上的
  `asio::posix::stream_descriptor` + `co_await sock.async_wait(wait_read/
  wait_write)`。在 4 槽池上的四个并发 `save_async` 调用现在在线路级别
  并行提交同步，而非通过 `run_sync` 串行化。

---

## [0.1.0] — 2026-04 之前

预发布开发。无公开 API 稳定性保证。
