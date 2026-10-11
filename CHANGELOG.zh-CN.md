<!-- neograph-i18n: source=CHANGELOG.md locale=zh-CN source_sha256=e5b466acd86fb3de789e2d2dd34699f615b8366f014fec2e382ec522b1ded62d -->
# 变更日志

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

以下带日期的条目保留历史 API 与测量。旧 Provider 兼容保证已被 [未发布] 的 breaking C++ typed 契约取代，不是当前 API 指南。

NeoGraph 的所有显著变更均记录在本文件中。

格式遵循 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)。版本控制遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

---

## [未发布]

## [Unreleased]

### 变更

- **需要重新构建 C++ 使用方：Program provider 调用结构体新增了成员，被计价的调用会执行 `monetary_microunits` (#372)。** `ProgramResultData` 新增 `monetary_records`，`ProgramCoreProviderCallContext` 新增 `monetary_account`，`SQLiteProgramProviderCallJournal::Record` 新增 `monetary_record`，`Provider` 新增静态函数 `invocation_upper_bound`；请一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。provider journal 会给已有数据库添加 `monetary_record` 列。存储的 `ProgramResult` 只在有调用被计价时才带 `monetary_records`，因此其他结果的字节与 id 不变。**行为变更：** 与所有 `RunBudget` 数量一样，`monetary_microunits = 0` 不授予任何额度，因此金额预算为零的运行会以 `P_MONEY_BUDGET` 拒绝计价 broker 计价的每个调用；此前没有任何东西收取金额。
- **C++ 使用方须重新构建：图和 Program 结构体增加了 member (#357, #358, #361, #365, #367)。** `EngineConfig` 增加了 `validation` 和 `stderr_notices`，`RunMetadata` 和 `RunContext` 增加了 `shared_scheduler`，`ProgramCompilerConfig` 增加了 `require_acyclic`，`ProgramCompilerConfig` 和 `ValidationOptions` 增加了 `soundness_check` 和 `soundness_state_cap`，`GraphEngine` 增加了一个 private member，`RunStatus` 增加了 `BarrierStalled`。这些改变了 layout：NeoGraph、C++ 使用方和 Python 扩展须一起重新构建（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。列出所有值的 `RunStatus` `switch` 需要一个 `BarrierStalled` 分支。
- **节点重试与工具失败遵循同一个失败分类器 (#364)。** 节点重试循环和工具失败契约都询问同一个分类器。分类器会遍历整条原因链（`NodeExecutionError` 的原因、`std::nested_exception`、子图失败），并采用找到的最严重分类。仍会重试的：`std::runtime_error` 及其子类（包括 Program 诊断）、取消之外的 asio 错误，以及根因为上述之一的子图失败。不再重试的：根因为 `std::logic_error` 或未知异常的子图失败、`std::runtime_error` 与 `std::logic_error` 体系之外的任何异常（包括 `throw 42`），以及位于 `std::nested_exception` 之后的配额失败。工具原因链中任意位置的运行控制信号（例如工具所运行的图的失败中包含的 Hook 阻断）现在会停止运行，而不再变成工具消息。参见[节点失败契约](docs/reference-en.md#node-failure-contract)。
- **节点重试的每次尝试都有自己的效果命名空间 (#364)。** 尝试 0 沿用任务 id（`s3:act`），因此现有日志的槽位保持不变；尝试 k 使用 `s3:act#a<k>`。因此重试会向 `ProviderCallBroker` 请求新的调用槽位而不是第一次尝试的槽位，被重试节点所运行的子图也会在新的子线程中开始。尝试 k 开始之前，一条以其命名空间为键的空待写记录会记下这次开始，因此恢复会继续该尝试并保留剩余的重试次数，而不是从尝试 0 重新开始。在 `ToolEffectBroker` 下，提交了非幂等效果的尝试绝不会被重试，并以 `NG.EffectCommitted` 失败；结果未知的效果会暂停运行以便核对，即使节点把该中断变成了错误也是如此。
- **示例 11（Clay 聊天机器人）已重新设计。** 文本使用系统 TrueType/OpenType 字体并按 UTF-8 测量，因此韩文等文字都能显示（可用 `NEOGRAPH_CHAT_FONT`、`NEOGRAPH_CHAT_BOLD_FONT`、`NEOGRAPH_CHAT_MONO_FONT` 覆盖选择）。窗口包含显示模型与状态的标题栏、头像、带代码块的轻量 Markdown、带建议的欢迎卡片、跟随新输出的滚动条，以及在回复流式输出期间 Send 按钮变为 Stop 的输入框；Stop 和退出都会取消回复。
- **采用 SchemaProvider `maint/interface6` 当前最新提交 `0d536be66017dfb8c4c91d21f4f62bd8ce698d55`。** 使用 OpenAI Responses 的图和 agent 工具循环，在工具的 parameter schema 不兼容 strict 时（例如未关闭 `additionalProperties` 的 MCP 工具 schema）不再以 HTTP 400（`InvalidRequest`）失败：policy 的 strict 默认值只应用于每个 object 都设置 `"additionalProperties": false` 并在 `required` 中列出每个 property 的 schema，其他工具以 `"strict": false` 发送。对不兼容的 schema 显式设置 `strict = true` 会在 dispatch 前以 `InvalidRequest` 失败，并指出工具名和第一个失败的 JSON pointer。`created` 时间戳在 chunk 之间递增的 OpenAI 兼容本地服务器的 Chat 流不再以 `ProtocolCorrupt` 失败；该值减小，或 `id`、`model` 改变时仍会失败。codec 已接受的 in-band 供应商失败（例如 Responses 的 `response.failed` 事件或缓冲的失败响应体）现在无论大小都按其自身错误码分类。此前，超过 runtime `max_error_bytes`（默认 64 KiB）的失败，例如重复 hosted `image_generation` 结果的 3 MB `response.failed`，会被报告为 `RetryClass::Never` 的 `ResourceLimit`，而不是供应商的失败。runtime 自行缓冲并解析的错误仍受 `max_error_bytes` 限制。SDK 0.3.0 与 interface/shared ABI 6 不变。
- **采用 SchemaProvider `maint/interface6` 上已合并的 PR #25 提交 `f7cb27e68ee02f3a27d01c31af85bcac96f00963`。** `Usage::input_uncached` 在所有 family 中遵循同一规则（输入 − 缓存读取 − 缓存写入；仅在从不发送写入计数的 wire 上将缺失值视为 0）：直连 OpenAI Chat 现在也会报告该值，Responses 会减去上报的缓存写入。Gemini 的 stop 类型已由测试固定。SDK 0.3.0 与 interface/shared ABI 6 不变。
- **采用 SchemaProvider `maint/interface6` 上已合并的 PR #23 提交 `45dd2ad36e18717adae3e2d10b2b9c8bb0e85471`：用量与供应商数值完全一致。** 已通过真实 API 调用验证：OpenRouter Messages 流式响应不再因末尾的 `[DONE]` 失败，并带有上报的费用；Chat 保留 `video_tokens` 等供应商明细计数；上报的 USD 金额与供应商十进制值一致（不再多 1 nanoUSD），输入 + 输出 = 合计；Gemini 在合计可证明时报告推理为 0；Interactions 按名称记录 modality；`claude-haiku-4-5` 等 Anthropic 别名在以带日期模型应答时也被接受。保存的结果可能带有 `CostRounding::CeilingShortestDecimal`，旧记录仍可读取，Python 暴露 `ProviderCostRounding.CeilingShortestDecimal`。SDK 0.3.0 与 interface/shared ABI 6 不变。
- **`llm::Agent` 以及 deep-research、plan-execute 工具循环会继续被暂停的轮次 (#311)。** 收到 `sp::StopKind::PauseTurn`（Anthropic/Chat 的 `pause_turn`）时，它们会在现有迭代预算内重新发送包含该暂停 assistant 轮次的历史，而不再把暂停的轮次当作最终结果返回。同样的判断可通过 `paused_turn()` 使用。参考文档现已说明 Responses 的 `response.done` 别名规则以及已声明网关的服务模型规则。
- **采用 SchemaProvider `maint/interface6` 分支上已合并的 PR #21 提交 `83112573ba59e3b561fc33c22394638be7aa5294`：SDK 0.3.0、interface/shared ABI 6 不变，并加入路由网关修复。** Responses 与 Messages 现在接受已声明的 OpenRouter origin 针对 `~deepseek/deepseek-v4-flash-latest` 等路由别名返回的实际服务模型名；此前此类 Responses 调用都会以 `invalid response protocol (HTTP 200)` 失败。服务模型在同一响应内不得变化，replay 仍绑定到请求的模型。NeoGraph 的 API 与 ABI 均无变化。
- **采用合并的 SchemaProvider PR #16 提交 `3b88e4ba020c3a4d39ff0660014e7292b516b7cb`，须以 SDK 0.3.0 EXACT、interface/shared ABI 6 重建。** 这取代下方记录的先前 interface-5 SDK 步骤的当前要求。blocking 调用使用单一 async SDK pipeline，继续拒绝 event-loop/callback worker 重入。outcome、usage 观测、journal、native archive 重载与 Python 类型化值保留服务商报告金额、status、source、evidence、rounding 和冲突。canonical v2 JSON 仅在金额元数据完全缺失时省略 `provider_cost`，保持旧缺失记录 bytes 不变。零和 BYOK false 等金额证据阻止将未知 token hold 当作 proven-not-sent 退款；金额不转换为 token 或支出权限。Native archive v3 和 portable JSON v2 不变。
- **Core 引擎速度恢复到 v0.11.1 的水平。** 在历史测量中,3 节点顺序基准从每次运行 10.19 µs 降到 6.29 µs(v0.11.1:6.10),parallel-3 基准从 30.03 µs 降到 15.66 µs(15.80);每个线程各用一个引擎时,2、4、8 线程下的单线程耗时与 v0.11.1 相同。损失来自每个 fan-out 分支一个 strand、每个节点一次 `RunContext` 拷贝、包住上下文绑定注册表的进程级 mutex(现为 64 个分片)、由多个 JSON 文档构建的状态快照、什么都不返回的协程调用(没有 Send 时的 `run_sends_async`、没有检查点时的 `record_pending_write_async`)以及调度器中的有序集合。各提交的测量数据见 #343。
- **只链接 `neograph::core` 的程序不再加载 libcurl。** `neograph::core` 链接了 SchemaProvider 的运行时,而该运行时链接了 libcurl 传输层,因此仅运行图的进程也会加载 libcurl 及其所需的约 25 个库(基准测试启动时常驻内存 12.7 MB,v0.11.1 为 5.0 MB)。现在只有 `neograph::llm` 链接传输层;同一基准测试以 7.7 MB 启动,共享库从 36 个减少到 7 个。不使用 `neograph::llm` 而自行构造 `sp::runtime::Client` 的代码必须链接 `SchemaProvider::transport`。SchemaProvider 已固定到包含此拆分的修订版本。参见 #347。
- **须使用 SchemaProvider 0.2.0 alpha、interface/shared-library 代际 5 重新构建。** SDK 新增可选的连接、首个响应和空闲 timeout：默认关闭，不延长总 deadline，可通过 `ProviderRequest::options`（`sp::runtime::RunOptions`）按调用设置；图的 LLM 节点暂未暴露它们。五个 API family 的真实 transport lifecycle 检查与独立 private test fixture 解决 SDK #2、#3、#4。C++ 使用方与 Python wheel 必须使用匹配的 SDK header/library 一起重新构建；native archive 仍为 v3，portable JSON 仍为 v2。
- **只链接 `neograph::core` 的程序不再加载 libcrypto。** SDK 自行实现了 SHA-256、HMAC-SHA256 和 OS 随机源的使用，其运行时不再链接 OpenSSL，archive 与 policy 字节不变。仅运行图的基准测试（`bench_neograph 1 1 1`）以 6.3 MB 常驻内存启动，而不是 7.7 MB（Linux x86_64，31 次中位数），共享库从 7 个减少到 6 个，与启动内存为 5.0 MB 的 v0.11.1 相同。直接调用 OpenSSL 的代码必须自行链接 `OpenSSL::Crypto`。

### 新增

- **Program：可选启用的健全性检查 (#363, #364, #365)。** `ProgramCompilerConfig::soundness_check`（Python：`ProgramCompiler` 和 `LocalProgramHost` 上的 `soundness_check=True`；`GraphValidator`：`ValidationOptions::soundness_check`）会探索根图的路由所允许的每一个运行。状态由就绪集合、每个 join 持有的信号（含 `any_of` 槽位）以及带 `max_visits` 的节点的访问次数组成；conditional 路由和失败 edge 是不确定的选择，failure exit 会结束运行。检查返回以展示问题的最短路由序列为 witness 的警告：`P_CORE_S1`（从某一时刻起没有运行能够完成）、`P_CORE_S2`（运行结束时 join 仍持有信号，以 barrier-stalled 结束或经由显式结束）、`P_CORE_S3`（永不触发的 join）、`P_CORE_S4`（没有出口的循环）和 `P_CORE_S5`（写同一个 overwrite 通道的两个节点在同一个 super-step 中就绪，`P_CORE_E5` 的 fan-out 兄弟之外）。它们的 help 指向 witness 中的路由选择；对于可由一个槽位之外的节点填补的停滞，会建议 `any_of` 槽位。`P_CORE_SOUNDNESS` 摘要在探索了每个运行时为注记，否则为警告：类型声明了 `Command.goto` 或 `Send`（effect 契约 `"control": ["goto"]` 或 `["send"]`）的节点之后的运行无法分析，达到 `soundness_state_cap`（默认 100 000 个状态）时检查以 not checked 停止。两种情况都不声称未探索的内容。没有任何内容被封存，因此包 id 不变。在有界穷举 join 测试框架枚举的每个图上，判定都与其参考解释器一致，每个 witness 都能逐步重放。capability manifest 增加了标志 `diagnostics.soundness_check`。
- **Program：可以编译失败路由键 (#364)。** `graph.retryPolicy(node, {max_retries, non_retryable, max_visits, error_text, on_failure, error_channel, ...})` 降级为严格 Core 键 `node_retry_policies`（只保存写出的字段，带有 `max_retries` 0 到 10、`max_visits` 1 到 100 等范围）和 `failure_edges`，`graph.failureExit(node, {code})` 写入 `failure_exits`。编译器会检查它们（哨兵、自身、join 或隐式结束目标、同一节点的第二条 edge、非 append 的 error channel 报告 `P_CORE_GC_FAILURE_EDGE`；范围和代码报告 `P_CORE_GC_FIELD_VALUE`；未知名称报告 `P_CORE_E3`），把 failure edge 加入路由后继映射使 E7、E11 和 E15 能看到它们，把 `error_channel` 计为已写入，并对没有任何东西读取的节点配置 `retry_policy` 键发出 `P_CORE_E18` 警告。图级策略中的 `on_failure` 是带提示的未知字段。这些键会被封存，除 `node_retry_policies` 外都属于迁移形状；不使用它们的定义保持原有 id。执行它们的运行时尚未启用：清单标志 `retry_policy.per_node`、`failure_edges` 和 `failure_exits` 仍为关闭，节点失败仍会使运行失败。`TopologySpec` 和 `CompiledGraph` 新增了成员。参见 [Program 图功能](docs/program-graphs.md)。
- **Program：在金额预算下绑定的 `llm_call` provider (#372)。** Python 宿主为 Program 的 `llm_call` 节点绑定 provider：`ProgramRegistryBuilder.add_provider(name, version, digest)` 声明它，`add_registered_node("llm_call", ..., provider=name)` 要求它，`LocalProgramHost(..., node_context=ng.NodeContext(provider=..., model=..., provider_controls=...), binding_label=...)` 通过与 MCP harness 相同模式的 `CatalogCapabilityBinder` 绑定它。只有经过校验的非机密标签进入 identity，位于 provider 的 capability receipt 中；密钥和模型从不进入，不需要 provider 的 Program 保持其 bundle id 与版本 id。每个绑定的调用都依据现在编译进 `neograph::program` 的价格目录，针对运行的 `monetary_microunits` 计价。分发之前，broker 为调用的输入上限与输出上限预留目录上限（OpenRouter 上为 route ceiling），对无法确定上限或预留放不下的调用，在发送任何请求之前以 `P_MONEY_*` 失败拒绝。得到结果后，按报告费用（OpenRouter `usage.cost`，证据 `reported`）或目录估算（`estimated`，写明目录 version 与 digest）结算；结果或 usage 未知时保留预留额（`held`）。`ProgramResult::monetary_records()`（Python 为 `result.monetary_records`、`ng.ProgramMonetaryRecord`）列出各次调用，`ProgramUsage::monetary_microunits` 是其总和。C++ 宿主通过 `bind_priced_provider_call(context)` 或 `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)` 启用；后者把每次调用的记录写入其 journal 行：崩溃前记录的调用在恢复后只收费一次（execution lease 过期结束运行时也一样），较早 attempt 记录的调用不会再次收费。`LocalProgramHost(sqlite_directory=..., native_history_archive=...)` 把运行、检查点和该 journal 保存在 SQLite 文件中，`reconnect(run_id)` 继续停止的进程留下的仍在运行中的运行。OpenRouter 请求尚未通过 `provider.max_price` 固定到 route ceiling，这需要 SchemaProvider 的一个字段。见[计价证据](docs/reference-en.md#pricing-evidence-and-the-price-catalog)与 [Python 指南](docs/python-binding.md#model-calling-nodes)。
- **带类型的节点失败 (#364)。** `NodeFailureError(code, public_message, retryable)` 以 `app.compile_error` 这样的作者代码使节点失败，`retryable = false` 会停止其重试循环。`node_failure_of()` 返回执行器所抛出的 `NodeExecutionError` 上附带的 `NodeFailure` 记录：代码、分类、停止重试的原因、尝试次数、可重试与可路由标志、`public_message` 以及最后一次尝试的 Tool 效果。Python 节点的异常也通过其 `code`、`retryable` 与 `public_message` 属性以相同方式读取，调用方收到的仍是原始异常。
- **Program：witness 路径、`any_of` 建议和路由表 (#363, #365)。** `P_CORE_E15` 的 witness 现在带有 `paths`，即从 barrier 成员经过每条绕过路由的路由序列；当该序列回到 join 时还带有 `substitute` 和 `suggested_wait_for`：返回的诊断会建议一个 `any_of` 槽位，让 join 之前的节点替代该成员（对 #357 为 `[{"any_of": ["B", "FIX"]}, "C"]`）。停滞运行的 witness 也会增加由执行轨迹和路由后继映射得出的同样的 `paths` 和 `suggested_wait_for`。`ProgramCompilerConfig::echo_routes`（Python：`ProgramCompiler(..., echo_routes=True)`）以 `P_CORE_ROUTES` 注记返回路由后继表，`GraphValidator::routed_successors()` 公开该表。这些都不会被封存。
- **Program：barrier join 中的 `any_of` 槽位 (#363)。** `barrier("D", [{any_of: ["B", "FIX"]}, "C"])` 为每个槽位等待一个备选项：任一备选项向 join 发出信号即填满槽位，所有槽位填满后 join 运行，随后清空全部信号。#357 的结构现在在两条路由上都能完成。在 join 运行之后才到达的备选项会开启一个永远不会完成的轮次，因此运行以 `P_CORE_BARRIER_STALLED` 结束（失败即关闭）；新警告 `P_CORE_E17` 会报告可能同时运行的备选项。只有一个名称的槽位等同于该名称，因此与旧形式等价的定义保持其字节和 id；其他定义按 UTF-8 字节排序输出到新的 `TopologySpec::join_specs` 中，`CompiledGraph` 和 `Scheduler` 也携带它。校验器逐槽位检查 E3、E8 和 E15，停滞 witness 输出槽位，迁移适配器把槽位 join 列为 barrier。any、quorum 等 join 模式仍然推迟。`Scheduler` 新增了成员和构造函数，布局随之改变。参见 [Program 图功能](docs/program-graphs.md)。
- **Program：图元数据 (#362)。** `ng.graph("plan").metadata({...})` 将宿主可读的作者声明存为 Core 定义的顶层 `"metadata"` 对象，可从 `bundle.sealed_core_definitions()` 读取（Python：`bundle.sealed_core_definitions[0]["definition"]["metadata"]`）。它随定义一起参与哈希，从不传给节点工厂、状态、检查点、输出或事件。限制为规范 JSON 8 KiB、嵌套深度 4、成员 64 个、键长 1 到 128 字节；不可见字符会报错（`P_CORE_GC_METADATA_LIMIT`、`P_CORE_GC_METADATA_TEXT`）。`GraphSemanticMigrationAdapter::metadata_changes()` 列出元数据差异，这些差异不改变 `topology_shape_id()`。使用 Program API v2 键（包括元数据）的 Core 定义不封存校验器警告；不使用的程序保持原有包 id。`TopologySpec` 和 `CompiledGraph` 新增了成员，布局随之改变。参见 [Program 图功能](docs/program-graphs.md)。
- **Program：JavaScript 能力清单公开功能标志、签名和参数 schema (#365)。** `javascript_authoring_capability_manifest()` 新增 `features`（每个编写功能一个布尔值，例如 `graph.barrier`、`graph.metadata` 或 `failure_edges`，宿主或模型可据此了解本构建接受哪些功能）、每个方法的 TypeScript `dts` 签名及共享声明 `dts_prelude`，以及每个图构建器参数的 JSON Schema。新增 schema 文件：`schemas/javascript-capability-manifest-v1.schema.json` 描述清单，`schemas/core-definition-v1.schema.json` 描述用于约束解码的封存 Core 定义。
- **Program：诊断附带自我修复用的 help (#365)。** 编译返回的每个诊断（`ProgramCompileError::diagnostics()`、`ProgramBundle::compile_diagnostics()`、Python 异常的 `.diagnostics`）都带有 `help`：指向新[诊断指南](docs/diagnostics.md)中对应条目的链接、带标签的相关位置、以 RFC 6902 JSON Patch 编辑表示并标明适用性（`machine`、`maybe` 或 `placeholder`）的修复建议，以及 `candidates`，即与未知名称相差不超过两次编辑的已声明名称。关于同一未知名称的诊断会合并为一个，其余位置成为相关位置；最多返回 20 个诊断，错误在前，随后附一个 `P_DIAGNOSTICS_TRUNCATED` 注记。JavaScript 消息使用构建器调用的名称，例如 `graph.retryPolicy({...}) has an unknown field 'on_exhausted'`。封存的诊断从不携带 `help`，因此包 id 不变。`program::Diagnostic` 新增了成员，布局随之改变：请重新构建 C++ 使用方（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **Program：`P_JS_CONTROL_CHARACTER` 警告 (#361)。** JavaScript 配置字符串中含有制表符、换行和回车以外的 C0 控制字符时（例如本想写正则单词边界的 `"\b"`），会在该字符串的调用位置发出警告。该警告只出现在编译返回的诊断中。
- **拓扑验证遵循 scheduler 实际采用的路由，并新增报告四类问题 (#357, #358)。** 带有 conditional edge 的节点只通过其第一条 conditional edge 路由，因此 E7（不可达节点）、E8（barrier liveness）和 E11（没有通往 `__end__` 的路径）不再计入该节点的 static edge 或之后的 conditional edge。E13 报告从同时带有 conditional edge 的节点发出的 static edge，这种 edge 永远不会触发。E14 报告同一节点的第二条 conditional edge，这种 edge 永远不会路由。E15（警告）报告其 route 通向 join 或 `__end__` 以外位置的 barrier 成员；走该 route 时，运行以 barrier-stalled 结束。E16（错误，需显式启用）为需要 DAG 的宿主报告任何环；可通过 `ValidationOptions::require_acyclic`、`ProgramCompilerConfig::require_acyclic`，或 Python `ProgramCompiler` 与 `LocalProgramHost` 上的 `require_acyclic=True` 启用。既不列出 reads 也不列出 writes 的 effects 契约现在视为未声明，因此 E6 不再把 Python 节点写入的通道报告为 dead(#361)。**行为变更：** E13 和 E14 在 strict 文档（`schema_version` 1 及以上，包括所有 JavaScript Program）中是错误，因此此类图不再能编译；legacy 文档会收到警告。bundle id 不变：Program 编译按当前规则失败并通过 `compile_diagnostics` 返回其警告，但封存的是先前规则的警告（`ValidationOptions::original_rules`），准入(admission)也按这些规则 link，因此先前封存的 bundle 仍可继续 materialize。`EngineConfig::validation` 选择 `GraphEngine` build 和 link 时使用的规则。
- **`EngineConfig::stderr_notices` (#361)。** 默认值为 `true`。为 `false` 时，`build()` 和 `link()` 不会把验证 lint 打印到 stderr（错误仍会抛出），也不会打印 fan-out 串行运行的一次性提示。Program Catalog 在关闭该选项的情况下 link 其引擎，因此 `compile_admit` 不再打印 `[neograph] lint [E6] ...`，Program 运行也不再打印提及 `EngineConfig::worker_count`（`LocalProgramHost` 用户无法设置的选项）的提示。Program lint 只通过诊断到达调用方。
- **Python：`LocalProgramHost(..., worker_count=1)` (#370)。** 它通过 `CatalogConfig::worker_count` 为图内的并行 fan-out 设置每个 Core 引擎的工作线程池，且不得超过已准入 Program 的 `max_concurrency`。类的 docstring 说明了 `scheduler_threads` 和 `worker_count`。
- **`ChatTool::strict` 可按工具设置 OpenAI Responses strict 模式；C++ 使用方须重新构建。** 该标志是默认未设置的 `std::optional<bool>`，由 `make_provider_request` 传入 Responses function 工具。未设置时沿用上文所述的 SDK 默认值，`false` 按原样发送，对不兼容 strict 的 schema 设置 `true` 会在 dispatch 前以 `InvalidRequest` 失败。其他 family 无法发送该标志，因此以 `std::invalid_argument`（Python 中为 `ValueError`）拒绝 `true`，接受 `false`。Python：`ChatTool(..., strict=None)` 及 `strict` 属性。`ChatTool` 增加了 member，layout 随之改变：NeoGraph、C++ 使用方和自定义 `Tool` 实现以及 Python 扩展须一起重新构建（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- `CheckpointCoordinator::records_pending_writes(parent_cp_id)`:当 `record_pending_write[_async]` 确实会存储内容时返回 true,调用方可据此跳过调用。

### 修复

- **Program：恢复的尝试在崩溃恢复后以其恢复值继续 (#377)。** 崩溃恢复会以运行的输入重新执行由 `resume()`、`reconcile()` 或 `fork()` 启动的尝试，而不是该尝试恢复其检查点时使用的值。`define()` Program 的节点把运行的输入当作恢复值，`main()` Program 中被中断的 `callCore` 以命令的输入恢复，绑定了提供者时 SQLite 日志会以 "logical call binding conflict" 失败（fail closed）。运行记录现在在尝试开始前持久地保存该值 `ProgramRunRecord::attempt_resume()`，恢复时以它继续。带有该值的记录使用运行记录存储模式 5；其他记录保持各自的模式、字节和 id，已存储的记录照常加载。`ProgramRunRecordData` 新增了成员：请重新构建 C++ 使用方（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。派生（fork）的 `main()` Program 的第一次尝试尚未覆盖。
- **Program：重放的提供者调用的令牌每次运行只计费一次 (#378)。** 在使用 SQLite 提供者日志的中断与恢复中，恢复的尝试会再次计入先前尝试已记录并结算的调用的模型令牌：没有新请求，剩余的 `model_tokens` 却从 999,976 降到 999,952。日志仍然识别这些调用，因此既不重发也不重复计数，但只为尝试计入它自己记录的调用，#372 的金额计费已经这样做。崩溃前记录的调用在恢复后仍计费一次，节点重试的新调用照常计费。
- **Program：join 报告使其停止的失败 (#379)。** fail-fast 的 `ng.all` 按声明顺序报告第一个失败的成员，因此一个先声明、因另一成员失败而被取消的兄弟会取代那个失败：以 `P_MONEY_BUDGET` 被拒的成员被报告为操作已取消。join 现在报告使其停止的那个成员的失败；使用 `collect` 时仍按声明顺序报告第一个失败。计划级的 `parallel` 操作也有同样的缺陷，被取消的分支会取代使其取消的失败。`ng.race` 与 `ng.quorum` 原本就是正确的，现在由测试固定。
- **节点重试不再与先前尝试的效果冲突 (#364)。** 过去一个调用作用域覆盖了整个重试循环，所以每次尝试都复用第一次尝试的效果任务 id 与调用序号：在 `ToolEffectBroker` 下，重试会重放第一次尝试的回执，或因参数改变而被拒绝；重试期间发生崩溃后，恢复也会落在第一次尝试的槽位上。
- **未确认的 Program Provider 分发是结果不确定的效果 (#364)。** 当 SQLite Provider 日志提交分发标记之后传输层抛出异常时，原始异常被直接重新抛出，节点重试可能再次发送请求。现在日志在这种情况下，以及稍后再次调用尚未解决的槽位时，抛出 `ProgramProviderDispatchUncertainError`（一个不带结果的 `ProviderOutcomeError`）；它既不会被重试也不会被路由。
- **Program：`LocalProgramHost.resume(run_id, value)` 无需 pending id 即可使用 (#359)。** 只有一个等待中的调用时恢复该调用；没有或有多个时抛出指向 `result.interrupt.pending_input.call_id` 的 `ValueError`。显式 id 仍保持精确匹配的约定。
- **Program：失败的运行会保留已完成节点的 trace (#359)。** `execution_trace` 列出失败前已完成的节点，`failure.core_node` 指出抛出异常的节点。失败结果的 identity 随之变化。
- **Program：编译期 JavaScript 遵守运行预算的 wall time (#360)。** 不终止的 `define()` 会在编译器上限与预算 `wall_time_ms` 中较小者处以 `P_JS_TIMEOUT` 停止，而不是 60 秒之后。Python 通过 `ProgramCompiler` 和 `LocalProgramHost` 的 `javascript_limits=` 传入 `ng.ProgramJavaScriptCompileLimits` 来设置编译上限。
- **Program：编写期诊断指向源码位置 (#361)。** JavaScript 构建器诊断在编译返回的诊断中（`ProgramBundle::compile_diagnostics()`，Python `bundle.compile_diagnostics`）携带调用位置（pointer、行、列）。封存进 bundle 的诊断保持不变，因此 bundle id 保持稳定。编译失败会抛出 `ng.ProgramCompileError`（`RuntimeError` 子类），其消息以 `source:line:col` 结尾，`.diagnostics` 携带详细信息。注册表缺失提示会给出 Python 的注册调用名。
- **Python：`ProgramRegistryBuilder` 以同一种错误类型拒绝未注册的可执行单元 (#370)。** `add_registered_node` 现在会像 `add_registered_reducer` 和 `add_registered_condition` 一样检查节点类型是否已注册，而不是把缺失留到准入(admission)阶段。三个方法都会抛出 `ng.ProgramRegistryError`：其 `kind` 为 `"node"`、`"reducer"` 或 `"condition"`，`name` 为缺失的名称，消息会指出 Python 注册调用。它同时继承 `ValueError` 和 `RuntimeError`；此前 reducer 和条件方法抛出的是 `RuntimeError`。使用注册表快照中不存在的名称的 Program 仍会以带有 `P_CORE_GC_REGISTRY_MISS` 的 `ng.ProgramCompileError` 编译失败。
- **停在再也无法触发的 barrier 上的运行不再是 `Completed` (#357)。** 当没有就绪节点、最后一个超步没有显式到达 `__end__`，且某个 join 仍持有 signal 时（例如 barrier 成员走了绕过 join 的 conditional route），`RunResult::status()` 返回新的 `RunStatus::BarrierStalled`（Python 为 `ng.RunStatus.BarrierStalled` 和 `RunResult.barrier_stalled`），`output["_neograph"]["barrier_stalled"]` 以 `{node, wait_for, received, missing}` 列出每个停滞的 join。Program 运行以 `P_CORE_BARRIER_STALLED` 结束为 `Failed`，`failure.core_node` 设为该 join，witness 列出停滞的 join。只有最后一个超步中通往 `__end__` 的显式 route 才会抑制停滞：通往 `__end__` 的 static edge（由 `.exit()` 写入）、通往 `__end__` 的 conditional route 或 `Command.goto("__end__")`。隐式结束（例如没有后继的节点或 Send worker）永远不会抑制，更早超步中的显式结束也不会。运行保存的每个检查点都会记录停滞，因此从中恢复会再次报告。`interruptAfter` 检查点会保存受 barrier 约束的下一批节点，因此恢复时不再在最后一个成员到达前运行 join。Core 耗尽 step 预算的 JavaScript `callCore` 现在以 `P_CORE_STEP_BUDGET` 将运行结束为 `BudgetExhausted`；此前它会作为已完成返回 `main()`。**行为变更：** 在这些情况下原本以 `Completed` 结束的运行，现在以 `BarrierStalled` 结束，在 Program 中则以 `Failed` 结束。
- **保留节点名和没有入口的图不再能编译 (#358)。** 名为 `__start__` 或 `__end__` 的节点会被同名的路由 sentinel 遮蔽：指向 `__end__` 的 edge 会结束运行，JavaScript 构建器会封存一个 `__end__` → `__end__` 自环。有节点但没有从 `__start__` 出发的 edge 的图什么都不运行，却报告 `Completed`。图解析器现在在所有 schema 版本中报告 `GC_RESERVED_NODE` 和 `GC_NO_ENTRY`（在 Program 中为 `P_CORE_GC_RESERVED_NODE` 和 `P_CORE_GC_NO_ENTRY`）；没有节点的图仍被接受。JavaScript 构建器在 `.node()` 调用处以 `P_JS_GRAPH_ARGUMENT` 拒绝保留名称，`define()` 以 `P_JS_GRAPH_ENTRY` 拒绝没有 `.entry()` 的图。evolution 的 `remove_edge` mutation 会保留图唯一的入口 edge。**行为变更：** 这些图此前可以编译；现在 `GraphEngine::compile` 和 Program 编译会拒绝它们。
- **Program：Python 节点函数体不再占用 Program scheduler (#367, #368)。** Python 节点函数体在整个执行期间都运行在 Program scheduler 线程上，因此 join 成员只能依次运行：两个 800 ms 成员的 `ng.all` 耗时 1.6 秒，`ng.race` 返回先声明的慢成员，`ng.quorum` 会等待慢成员并将其计入，而在默认 `scheduler_threads=1` 下 `ng.await` 的 deadline 无法触发。在 Program 运行中，Python 节点函数体现在运行在 binding 拥有的工作线程池上（32 个线程；`NEOGRAPH_PYTHON_NODE_THREADS` 可设为 1 到 1024），因此成员可以并发推进，计时器也能触发；Python 代码仍由 GIL 串行化。普通 `GraphEngine` 运行仍在调用线程上执行 Python 函数体。`ng.race` 和 fail-fast join 以最先完成的成员而非最先声明的成员定局，`ng.quorum` 恰好保留达成 quorum 的那些成功结果。`ng.await` 会记录其 deadline：在其之后到达的完成即使计时器尚未触发也算超时，且超时优先于被取消子任务抛出的错误。取消仍是协作式的，因此 await 会在子任务停止后返回。**行为变更：** 迟到的完成现在会以 `P_AWAIT_TIMEOUT` 将运行结束为 `TimedOut` 而不是 `Completed`，且 `ng.race` 可能返回不同的成员。
- **Program：没有 provider 的模型调用节点不再中止进程 (#366)。** 未配置 provider 而构建的 `llm_call` 或 `intent_classifier` 节点会在首次运行时解引用它并中止进程（SIGABRT），无论是在普通 `GraphEngine` 中，还是在通过 `ProgramRegistryBuilder.add_registered_node` 注册该节点的 Program 中都是如此。现在这两种节点都会抛出指明节点的 `std::invalid_argument`，executor 不会重试该异常，因此运行以 `NodeExecutionError` 失败；`GraphEngine` 仍会为只关心结构的工具编译此类图。Program 准入(admission)以 `P_ADMIT_PROVIDER` 拒绝它们，Python 将其作为 `ng.ProgramAdmissionError` 抛出。自 #372 起，Python 通过 `LocalProgramHost(node_context=...)` 绑定 provider。
- **Python：Program 准入(admission)错误与运行时诊断错误现已类型化 (#369)。** 准入失败此前只抛出一个裸的 `RuntimeError("Program admission failed with 1 diagnostic(s)")`，`P_START_BUDGET` 等运行时诊断错误会丢失其代码和 witness。现在所有 binding 入口都会抛出 `ng.ProgramAdmissionError` 和 `ng.ProgramDiagnosticError`；两者都与 `ng.ProgramCompileError` 一样是 `RuntimeError` 的子类。消息中每条诊断占一行，包含其代码、JSON pointer 和消息，有源码位置时还附带 `source:line:col`；`.diagnostics` 保留代码、severity、phase、消息、源码位置和 witness。预算错误会指出资源及其限制，例如 `Program invocation budget wall_time_ms=9000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`；每条结构性 start-budget 规则、准入的 policy-ceiling 与 structural-floor 检查以及 Catalog 的 `worker_count` 检查也是如此。
- **Program：面向 Python 宿主的消息与 trace (#370)。** 恢复 `LocalProgramHost` 未持有的运行会抛出带有 `P_RUN_NOT_FOUND` 的 `ng.ProgramDiagnosticError`，消息会说明运行保存在启动它的宿主的内存中。wall-time 超时以 `Program wall-time budget (N ms) expired` 失败；被其中断的操作的文本移到 `failure.witness["cause_message"]`。`TimedOut`、`Cancelled` 和诊断失败会与节点失败已有的行为一样，保留已完成节点的 `execution_trace`；这些结果的 identity 随之变化。对于 JavaScript 源码，编译返回的 E9 fan-in 提示会给出 `graph.barrier("<node>", [...])`；封存的副本保留 Core 的措辞，因此 bundle id 不变。**行为变更：** 超时的 `failure.message` 是新文本；请检查 `status` 或 `failure.code`，而不是消息。
- **Program：第二次被 interrupt 的 `callCore` 不再终止进程 (#371)。** 在恢复之后或首次 `run()` 期间，第二个 `callCore` 被 interrupt 的 JavaScript `main()` 会以 `std::terminate` 结束。被 interrupt 的预留的释放是对照 attempt 的累计用量检查的，而累计用量还包含先前 command 已结算的工作，因此 store 拒绝了 publication，fallback 构造了无效结果。in-memory、SQLite 和 PostgreSQL transition store 现在用 `is_valid_program_journal_reservation_release` 检查该释放：每个带用量的资源退回的量可以是从预留减去 attempt 用量到整个预留之间的任意值。现在每个被 interrupt 的 `callCore` 都返回一个可由 `resume()` 继续的 `Interrupted` 结果。无法恢复的结果不再携带 interrupt，逃出 terminalization 的异常会以 `P_RUNTIME_INTERNAL` 将运行结束为 `Failed`，而不是终止宿主。
- **严格上下文组装接受带多个 required artifact 的短 history (#373)。** mandatory token 数累加每个 required artifact 的消息 projection JSON，而输入 token 数测量的是更小的已准备 wire body。history 较短且有两个或更多 required artifact 时，mandatory 数超过输入数，`StrictRuntimeProfile.invoke`（以及 `RuntimeTurnAssembler::assemble`）以 `Mandatory token count exceeds estimated input tokens` 失败。现在两个数都按每 3 字节 1 个 token 计算已准备请求所携带的字节：required artifact 计其文本，role 和 JSON 框架只计入输入数。超过 `max_input_tokens` 的 required 内容仍会抛出 `ContextBudgetBlocked`，mandatory 数超过输入数的 receipt 会被拒绝并给出两个数，例如 `Mandatory input tokens (41) exceed the estimated input tokens (40) they are part of`。新的 assembly receipt 记录更小的 `mandatory_input_tokens`，因此其 identity 与之前构建的 receipt 不同；已存储的 receipt 仍可 parse。
- **Program：`main()` 的每个结果列出它运行的 Core 节点 (#374)。** 对会 interrupt 的 Core 调用两次 `callCore` 时，每个结果的 `execution_trace` 都晚了一个 attempt：`["draft"]`、`["draft"]`、`["deploy", "deploy"]`。被 interrupt 的 command 只带着自己的 trace 结束 attempt，因此丢掉了同一 attempt 中先完成的 command；而 resume 又把先前 attempt 已完成的每个 command 的已记录 trace 再加一遍。现在每个 `Interrupted` 或 `Completed` 结果按顺序准确列出自上一个结果以来运行的 Core 节点：`["draft"]`、`["deploy", "draft"]`、`["deploy"]`。由后续 command 以其他 status 结束的结果，也会保留该 attempt 中先运行的 command 的节点。失败的结果也像只用 `define()` 的 run 一样，列出 Core 失败前已完成的节点；此前 Core 失败的 `callCore` 的 `execution_trace` 为空。受影响结果的 identity 随之变化。
- **适用于所有 schema 版本的拓扑错误不再建议删除 `schema_version` (#375)。** `GraphEngine::compile` 会在每个错误列表末尾附上 `Remove 'schema_version' to fall back to lenient parsing`，没有 `schema_version` 的文档也是如此。对于 lenient 解析不会放宽的 `GC_NO_ENTRY` 和 `GC_RESERVED_NODE`，摘要现在改为说明要修改什么：添加一条从 `"__start__"` 出发的 edge，或重命名该节点。只有列表中有未知键（`GC_UNKNOWN_FIELD`）或节点 config schema 错误（`GC_NODE_CONFIG`）时，才会出现 lenient 提示和 `strict` 前缀。诊断代码、pointer 和消息保持不变。
- **按节点的引擎设置拒绝未知节点名 (#376)。** `set_node_retry_policy`、两个 `set_node_cache_enabled` 重载，以及 `EngineConfig` 的 `node_retry_policies`、`cached_nodes` 和 `node_cache_policies` 映射，会接受图中不存在的节点名，而该设置从不生效。现在它们抛出 `std::invalid_argument`（Python 中为 `ValueError`），并指明设置、图、未知节点以及图中的节点，例如 `set_node_retry_policy: graph 'demo' has no node 'fecth' (nodes: 'fetch', 'parse')`；`build()` 和 `link()` 会在构造引擎之前检查 `EngineConfig` 映射。**行为变化：** 为不存在或拼错的节点设置策略的代码，过去会被静默忽略，现在会失败。
- **重试 backoff 不再溢出 (#364)。** 下一次重试延迟是 `delay_ms * backoff_multiplier` 先转换为 `int`，再限制到 `max_delay_ms`。超出 `int` 范围的乘积属于未定义行为，在 x86 上会变为 `INT_MIN`，导致之后的每次重试都不等待就执行；加入 jitter 的延迟也有同样的转换。现在两者都以 `double` 计算：NaN 和大于 `max_delay_ms` 的值限制为最大值，负值限制为 0，转换放在最后。
- **Program：为零的预算上限会指出相应字段 (#370)。** `PolicySnapshot` 和 Python `LocalProgramHost` 会以 `Every policy budget ceiling except child budgets must be positive; zero: model_tokens, max_dynamic_compiles` 的形式拒绝含零字段的上限，并列出每个为零的字段。`max_child_depth` 和 `max_total_children` 仍可同时为零。
- **Program：Python 与 QuickJS 文档与运行时一致 (#370)。** Python Program 快速入门重新可以运行：旧示例用绑定不接受的默认值参数调用了 `GraphState.get`。QuickJS 的 `main()` 示例导出 `define()` 并且只调用该 Core，并说明失败的 `callCore` 会结束运行，而不会进入 `try`/`catch`。Python 绑定指南现在涵盖 `callCore` 结果的形状、按排序存储的 barrier `wait_for`、在 `LocalProgramHost` 下以 `P_CHILD_BINDING` 失败的 `ng.spawn`、`compile_admit` 固定的预算、所有字段都必须为正的上限、图检查与 barrier 停滞、类型化的 Program 错误、与封存的 `diagnostics` 并列的 `compile_diagnostics`、以 `P_ADMIT_PROVIDER` 准入(admission)失败的 `llm_call` 节点、`scheduler_threads`、`worker_count` 与 `NEOGRAPH_PYTHON_NODE_THREADS`、超时、只存在于内存中的运行、`resume()` 的默认行为，以及恢复后的 `execution_trace`；故障排除指南列出了对应的错误，参考与概念指南列出了 `RunStatus::BarrierStalled`。README 指向该 Program 示例，而不是不使用 Program API 的 Python 示例。
- **示例可端到端运行。** 所有示例都已用真实 provider 或离线模式运行，并修复了失败：多智能体辩论和自我进化聊天机器人以可移植文本传递轮次，而不是把原生状态重放到配置不同的调用中；the-beast 各变体为模型编写的 `llm_call` 节点指定模型，使用共享的 zero-data-retention provider 工厂并报告运行失败；A2A 客户端默认使用示例 38 的端口；Harness MCP 服务器可在无 token 的 loopback HTTP 上工作；示例 34 为生成图片提高流限制，并在该部分使用支持图片输入的模型；program chatbot 演示策略与 descriptor 策略 revision 4 一致；多租户 live 服务器接受 `NG_MULTI_TENANT_REQUESTS` 和 `NG_MULTI_TENANT_WORKERS`；Crawl4AI 客户端接受带路径的 base URL 并发送 bearer token；RE 智能体在需要密钥之前先检查 GhidraMCP 后端。
- **并行工具 dispatch 的 fallback 捕获的工具失败现在是合法 JSON。** 在多调用 batch 中，若某个调用在逐调用错误处理之外失败，其工具消息以字符串拼接构造为 `{"error": "..."}`，异常文本中的引号、反斜杠或换行会产生非法 JSON。现在改用 JSON 库序列化。
- **所有工具路径上的工具失败遵循同一契约。** 在多调用 batch 中，`HookBoundaryBlocked` 会变成模型可以读取并绕过的 `{"error": ...}` 工具消息，而单个调用则会停止运行；现在两者都会停止运行。工具代码在 C++ 或 Python 中抛出的 `HookBoundaryBlocked` 或 `NodeInterrupt`，此前在所有路径上都被 `ToolExecutionController` 转换为失败的工具结果；现在它会停止或暂停运行，因此工具可以请求人工确认。无法运行的强制工具 Hook（例如 `llm::Agent` 运行没有 owner scope 时）现在会阻断运行，而不是让每个调用都以发给模型的消息失败。抛出异常的 `get_name()` 现在无论在单个调用还是 batch 中都成为该调用的 `failed` 结果，此前单个调用会使节点失败；只携带 `error`、并以非错误结果到达模型的 batch fallback 消息，现在与任何失败调用一样具有 `tool_status` 和 `{"error", "status", "retryable", "effect_uncertain"}` 结构。节点重试策略不再重试 `HookBoundaryBlocked`；当 batch 中多个调用都使运行停止时，按取消、Hook 阻断、中断的顺序优先。Python 的 `HookBoundaryBlocked` 新增 `invocation_id` 与 `state`。参见[工具失败契约](docs/reference-en.md#tool-failure-contract)。
- **声明了 `outputSchema` 的 MCP 工具的错误消息得以保留。** 客户端在检查 `isError` 之前就验证 output schema，因此不带 `structuredContent` 的合法错误结果会以 "MCP tool advertised outputSchema but returned no structuredContent" 失败，`[FUNCTION_NOT_FOUND] No function found named 'x'` 之类的工具消息既到不了调用方，也到不了模型。现在按照 MCP 规范的允许和官方 Python SDK 的做法，错误结果跳过 output schema 验证；成功结果仍会验证。
- **Program publication无需node handle移动赋值即可使用libc++构建。** 在所有index publication之后直接初始化提取的handle，不依赖库特定的`noexcept` trait，同时保留commit原子性。
- **Windows interface-6 archive fixture原子地创建私有custody目录。** 禁止继承、仅允许当前用户的DACL满足native archive admission，保留所有outcome、金额证据、replay和错误binding检查。
- **Anthropic Messages 和 Gemini 的并行 client-tool 结果保持在同一 turn。** family 编码前合并相邻 portable 纯结果消息，保留 mixed、native 和 turn 边界。其他 API family 与已保存 history 不变(#311)。
- **History compaction 保留权限和未完成证据。** 原样传递指定 typed controls，仅接受正常结束且非空的摘要；拒绝的 outcome 仍被记录，但不截断 history。摘要是 user 来源内容而非 system 指令，真实 native replay prefix 保持受保护(#314, #325)。
- **Program fan-out join 不再丢失等待前到达的完成通知。** 持久完成状态处理 signal-before-wait；scoped 取消停止新 dispatch，并取消当前或迟接入的子任务。部分 launch 失败会先 drain 所有借用状态的 producer 再传播。Await 在可能同步进入的子 publication 前注册 deadline(#341)。
- **Child generation 和暂停 migration 使用一致的 durable head。** 依据 immutable admitted lineage 校验 generation，record/journal snapshot 不再混合不同 publication。释放暂停 source 不会更新已消耗 wall-time budget(#324)。 Native PostgreSQL head通过一个MVCC statement读取，admission和recovery移动已拥有的head成员而非复制。
- **子任务预约恢复保留已获准的wall-time grant。** 替换后剩余时间减少时，cold reconnect将保留的预约绑定到原始不可变generation。当前可支出预算、token和金额上限以及子任务的原始deadline均不更新。
- **Synthesis persistence fixture使用父run budget，而非与验证目标无关的本地await deadline。** 专门的timeout和取消测试继续验证operation deadline。
- **Blocking Tool 异常 handoff 只有一个 terminal 所有者。** async-to-blocking bridge 保留 typed 异常和 system error code，publication 后 worker 不再访问异常对象(#339)。
- **示例和 benchmark driver 遇到不可用输出时失败。** partial/refusal 输出不能成为成功的 routing fallback，UTF-8 preview 保留完整 code point；失败尝试不计入成功 timing sample(#317)。
- **Durable Program 集成验证检查隔离和恢复。** cold rematerialization 后保持各 node 最小权限；provider journal 和 Tool effect 区分 run、attempt、slot，拒绝改变的权限，并在 reconnect 后保留未决 dispatch 证据(#293, #295, #297)。
- **使用引擎工作线程池的 multi-Send fan-out 中,某个分支的取消处理函数可能在该分支自身的操作完成期间运行。** multi-Send 步骤的分支运行在裸线程池执行器上,而静态 fan-out 的分支早已各有一个 strand,因此来自另一个线程池线程的取消会与分支的协程竞争(`cancellation_signal::emit` 中的数据竞争,已在 ThreadSanitizer 下复现)。现在每个使用线程池的 Send 分支都在自己的 strand 上运行;没有线程池的运行保持不变。参见 #344 和 #345。
- **启用 HTTP 的 `neograph-harness-mcp` 和 `example_harness_mcp_server` 现在会链接 `OpenSSL::Crypto`。** 这两个可执行文件直接调用 OpenSSL（`SHA256`、`CRYPTO_memcmp`），却从未自行链接它；SchemaProvider 0.2.0 不再链接 libcrypto 后，`NEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` 的共享库构建会因 `SHA256` 的未定义引用而链接失败。
- **同步图执行现在等待 scheduler 的 post 调用完全结束后才销毁 private `io_context`。** 完成处理函数即使已运行，线程池中的线程仍可能处于唤醒调用中，使 `pthread_cond_signal` 与 `pthread_cond_destroy` 发生竞态(#346)。同步 bridge 会等待外部线程的 post 与 tracked-work 释放，关闭取消通知的接收，再处理迟到的排队通知，最后销毁 context。调用线程执行、嵌套调用和父级取消权限均保留；post 处理函数抛出异常也不会丢弃仍由 bridge 所有的 coroutine frame。
- **SDK 固定提交前移到 SchemaProvider `8f32af7`，SDK 仍为 0.2.0，interface/shared-library 代际仍为 5。** SDK 现在会在 provisioning 失败的所有路径上清零 archive 激活密钥（SDK #10）；不再把 libcurl 8.21 合成的完成计时当作“请求已发送”的证据（连接被拒绝和不受信任的 TLS 保持 `NotSent`，因此其安全重试可用）；把字面地址写入 libcurl 的 DNS 缓存，使未启用 c-ares 的 libcurl（macOS 上的 Homebrew）不再创建超出配置工作线程数的解析线程；并把 qualification meter 的固定表移出线程栈。SDK 自身的测试现在可在 native Windows 和真实的 macOS 运行器上执行。这是源码固定，不是新的 SDK 发布或版本。

## [0.13.1] - 2026-10-07
NeoGraph `0.13.1` 面向 alpha SDK `0.1.1`。interface revision 与 shared-library generation 仍为 4，因此使用者只需选择 SDK `0.1.1`（已安装的 package config 要求精确版本）并重新构建。Native archive 仍为 v3 / `spna3`，portable JSON 仍为 v2。功能性变更是下面的 Windows transport 修复：作为 0.13.0 发布的 Windows wheel 存在该缺陷，应替换为 0.13.1。修复提交的 hosted CI 与 Windows wheel gate 已通过（GitHub Actions run 37576205702、37576205395，仅 CPython 3.12）；完整的 CPython 3.9–3.13 矩阵在发布 tag 上运行。
### 修复
- **Windows：较大的请求体可能一直卡到操作截止时间。** SDK transport 等待 `FD_WRITE`，而 Winsock 只在 `send()` 以 `WSAEWOULDBLOCK` 失败之后才记录该事件。一次成功但把 socket 填满的发送不会产生事件，因此约 4%～6% 的 4 MB 上传（在 loopback 上测得）停在约 131 KB 或 197 KB，并以重试安全性为 `PossiblyAccepted` 的 `DeadlineExceeded` 结束。固定的 SDK 现在在等待写入期间还会用短定时器探测 `select()`。已发布的 0.13.0 Windows wheel 存在该缺陷。回归测试：`test_large_post_does_not_stall_when_the_send_buffer_fills`（Windows 上重复 150 轮新连接；在 0.13.0 wheel 上 8 次运行全部失败）。Issue #340。

### 变更
- **CI 已分层。** 现在每次 push 和 pull request 只运行 `ci.yml`：Linux、macOS、Windows 的完整 native suite 加 2 个 installed-consumer 行，仅改文档时跳过。sanitizer、fuzz canary、性能 gate、真实 ARM64、gRPC、Visual Studio generator 以及其余 8 个 installed-consumer 行原样移到 `ci-extended.yml`，每晚及按需运行。Wheel 在 packaging 变更时为每个平台构建 CPython 3.12，在发布 tag、每周 canary 和手动运行时构建 CPython 3.9–3.13，每个解释器一个 job。Windows 依赖复用 vcpkg binary archive。`verify_ci.py` 支持 `--jobs auto`、`wheel --python` 和 `native-windows --generator`，`native-windows` 现默认用 Ninja 构建（请在 x64 MSVC 环境运行，或指定 `--generator "Visual Studio 17 2022"`）。所有 runner 仍为标准 GitHub-hosted runner。
- **ctest 以按测试的超时运行。** `verify_ci.py` 传入 `--timeout 600`（ASan 与 MSVC ASan 为 1200），因此卡住的测试会按名称失败，而不会耗尽 job 上限（Issue #341）。

## [0.13.0] - 2026-10-07

NeoGraph `0.13.0` 使用 alpha SDK `0.1.0`，loaded interface revision/shared generation 4。所有 native consumer 和 Python wheel 必须以匹配 header/library 重建。Native archive 保持 v3 / `spna3`，portable JSON 保持 v2。Retained-feature 集成已通过本地 Linux x86_64 native 与 installed wheel 验证。提交 `79c21e1` 的 hosted Wheels 与 CI(GitHub Actions run 37459723655、37459723545)构建了 Linux x86_64、Linux aarch64、macOS arm64 和 Windows AMD64 wheel,并通过了除 hosted 原生 Windows job(`build-windows`,在 ctest 之前超过 30 分钟限制)之外的所有 CI job。发布需经受保护的 release 批准。此前付费观测和 benchmark cohort 保留 SDK3 范围。

SDK4 Linux x86_64 验证运行了全部 27 个注册 case：初次 full run 有 25 个通过，随后两个修正 case 在限定重跑中通过。这不是第二次 full-suite 27/27 运行。未改动 Stop CLI 的 buffered/SSE valid/invalid client-call 四个 case，以及准确 installed README consumer 的三个 variant（reported zero、unknown count、HTTP400 failure）通过。这些本地证明不新增 vendor 或 Windows/macOS/ARM64 验证。

NeoGraph SDK4 验证范围为初次与修正后限定运行合计的 Core 1261 case、Program 691 pass 与设计上的 Memory process-loss skip 14 case，以及 installed Python 初次 499 pass 加修正后限定 5 pass，共 504 case。不声称第二次 full-suite 运行。实际运行了本地 A2A 0.x/1.0 peer、evolution file mode、plan replay、修改的 C++ helper 和 11 个 Python application。

### 变更
- **Windows provider 构造与 byte-budget Bridge。** SDK 门控 `NativeSocketRuntime`(原名 `PosixRuntime`)现在也授予 Windows,其 transport 已有 Winsock 事件路径。已发布的 `max_host_connections` 准入上限为 `INT64_MAX`,大于 Windows 的 32 位 `long`,Windows 运行中被拒绝;现为 2147483647,并由回归测试固定。NeoGraph 的 SDK Bridge 现在在构造 pmr 容器之前接收 byte budget:MSVC 的 `unordered_map` 在构造函数中分配内存,之前的 0 budget 因此在此处抛出 `bad_alloc`。原生 MSVC Release 中 1915 个已注册用例全部通过(2 个设计内 skip),变更前 1916 个中有 209 个失败。仅测试的改动:3 处 `exception_ptr` 同一性断言使用平台感知的辅助函数,因为 MSVC 的 `current_exception()` 返回副本;1 个 trusted-archive fixture 测试只设置 POSIX 权限位,因此与同类测试一样在 Windows 上跳过;Images/Veo 的 HTTPS peer 同时监听 `::1`,因为 `localhost` 先解析到它,而 Windows 拒绝 loopback 连接需要约 2 秒。1915 个用例的执行在开发机上完成,因为 hosted 原生 Windows CI job 在 ctest 之前就超时;hosted Windows wheel job 为 CPython 3.9-3.13 完成了构建、repair 和 installed-wheel 测试。SDK 自身仅限 POSIX 的测试不会在 Windows 上运行。
- **Windows wheel 在 hosted CI 上构建并测试。** 首次 hosted Windows wheel 运行需要四处修复:wheel job 现在自行准备与清单 `builtin-baseline` 对应的 vcpkg 检出(runner 镜像的检出比它旧);CMake define 通过 `SKBUILD_CMAKE_DEFINE` 传递,因为带引号的 `CMAKE_ARGS` 值在 cibuildwheel 下被拆成无用变量,CMake 只在 `<vcpkg root>/installed` 中查找(工具链仅在 manifest 模式下遵循 `VCPKG_INSTALLED_DIR`);`verify_ci.py` 为 TLS 测试 fixture 指定 vcpkg 的 `openssl.cnf`;一个归档保管的 Python 测试在 Windows 上跳过(`mkdir(mode=0o700)` 不会创建仅限所有者的 DACL)。wheel 包含 libcurl 8.22.0、OpenSSL 3.6.5、libpq 18.4、nghttp2 1.70.0、SQLite 3.53.4、zlib 1.3.2 以及 Microsoft Visual C++ 运行时 DLL `msvcp140`、`msvcp140_1`、`msvcp140_atomic_wait`;这三个 DLL 不参与 delvewheel 的名称改写,以按 Microsoft 的 Distributable Code 条款保持未修改并保留有效的 Microsoft 签名。`THIRD_PARTY_LICENSES.md` 记录了从产物观测到的 macOS、Windows 和 Linux 构成以及仍缺少的内容(新增组件的逐版本声明全文)。hosted run 37459723655 为四个平台生成了 CPython 3.9-3.13 wheel,已发布 Windows 产物的签名检查属于发布检查。
- **Local/hosted 共用验证入口。** `python scripts/verify_ci.py <profile> --work-dir <fresh-output> --jobs N` 集中十四个既有 CI gate，不移除 native platform、sanitizer、installed consumer、wheel、archive 或 release gate。Installed consumer 不再依赖 Git/Bash；独立的 immutable QuickJS performance gate 仍需 Linux/Git provenance。Native Linux 在 native PostgreSQL test 后串行运行完整 Python/protocol suite。Benchmark checker 保留原四个 workload 和数值边界，拒绝歧义或无效 metric。Source review 与 CLI help 不构成 runtime/platform/performance qualification，仍需集成执行。
- **单一 QuickJS matrix 命令。** 将重复 shell build driver 替换为 `python scripts/verify_ci.py quickjs-performance --work-dir <fresh-build-root> --jobs 4`，保留 `<fresh-build-root>/quickjs-performance.json`。自有新输出已存在时会被拒绝。不修改 immutable performance runner、preregistration 和源码根目录 Git provenance 要求。Native Windows toolchain 转发与 work-dir 专用 wheel vcpkg root 保留声明的 bootstrap/repair gate；PostgreSQL 前提检查向 `psql` 传递显式 DB URL。这些是源码变更，不宣称新 profile 已通过。
- **已完成取消 context 的寿命。** Detach 在 context 销毁前释放已完成的 Asio slot handler；generation 检查丢弃旧的排队 emit，callback 可在不重入 token lock 的情况下 rebind。取消状态不重置。必须一起重建 NeoGraph 与所有 native consumer。本地取消/fork 23 个 case 和实际 plain/ASan retained-token consumer 已通过。[Commit `24d3166` CI](https://github.com/fox1245/NeoGraph/actions/runs/37275542831/job/111651716661) 的 1,333-case TSan 运行也通过，包含 retained-parent-token teardown 回归及三个 skip。证明限于既有 dependency suppression 和禁用 ASLR 的环境，不宣称普遍 race-freedom。Detach 前必须完成 operation，不能在正在执行的 signal callback 内 detach。
- **Platform wheel 依赖与 cold loading。** 将 pybind11 固定为 `2.13.6`，解决 SDK 直接 OpenSSL Crypto 依赖，并为 macOS 14 使用匹配的 LLVM 20 header/runtime。保留 CPython 3.9–3.13、所有 platform 行和 installed wheel full suite；随后运行 Linux replacement 与拒绝 Homebrew 访问的 macOS cold-loader 检查。准确的最终 artifact 仍是 release gate，不以 source-only 检查宣称 portability。
- **匹配的 Linux LGPL library source。** SDist 的 `deps/redistribution/` 包含 library-only keyutils/libxcrypt source、downstream patch 和 build/replacement material。保留 file-scoped grant 与接收者的修改权；grant 未确认的 upstream test 不属于 library input，因此排除。仅提供 source/notice 不能证明最终 wheel 的 replacement arrangement 或已完成法律审查。
- **修复 Python cookbook import。** Native OpenRouter 示例从 installed package 的 `neograph_engine.llm` 导入 `SchemaProvider`，不再使用已删除的顶层 export。
- **显式 SDist 允许列表。** 使用 scikit-build-core 1.0 以上、`explicit` inclusion mode 和 root 锚定文件 pattern。实际 1131-file SDist 已通过允许列表、private/cache 排除、SDK4 pin 与必需内容检查；`manual` mode 曾包含无关文件。Wheel command 使用解包后的 package directory，并将包含空格或 Windows backslash 的 test path 作为独立参数传入。
- **保留的 typed controls 和模型规则 (#309)。** 将 newer-master `c708e51a`、`165dbbbd`、`5553d9ea` 集成到各 family controls：Chat reasoning/include/usage/替代模型，Responses provider-held cursor/parallel tools/verbosity/truncation/include，Messages manual/adaptive/disabled thinking/effort/cache/tool choice/routing，以及 Gemini thinking level/safety/tool choice/temperature。错误 family/origin/value 在 I/O 前拒绝。准入 model prefix 规则不区分大小写，包含 gateway suffix，并拒绝禁止的显式 temperature；Messages thinking 省略 temperature。不恢复 JSON knob interpreter。
- **显式 history 和 deployment headers (#305/#306, #310)。** 集成 `9e161fc8`、`c5035e00`、`02b4c275`。真实 same-route native reasoning 保持所有权。Gemini `PortableForeign` 接受 unsigned assistant text/client-tool history，仅首个 foreign function call 使用文档化 bypass，失败 native seal 从不降级。Responses `previous_response_id` 使用新 input，真实 tool ownership 需要时接收非 wire 的 `previous_response_history`；provider-held state 不是 full native/archive replay 权限。Deployment environment header 在 admission 前显式 host preprocessing，支持 optional 省略和不区分大小写的 override 优先级。
- **保留的 outcome、usage 和 retry 语义 (#307/#308/#312)。** 新 failure/terminal 修复（`782e23ea`、`0eb271de`、`f89afe08`、`99fa81a2`）保留为 typed 所有权 failure/partial evidence，包括 HTTP-200 error。`ec69b845` 的 cache/reasoning 意图使用 nullable 完整报告，不把缺失计数变为零。`054a2c1c`/`b67ccf4c` 通过 SDK no-output-replay safety 和 bounded exponential full-jitter policy 保留，不恢复旧 retry wrapper。`51374ce8` 的 client-tool intent 在 valid/invalid call 的 Stop event 与最终 outcome 中一致；具体 stop reason 与 server 执行 tool 保持区分。
- **保留的 A2A wire 和 HTTP streaming 修复。** 集成 `08c4df70`、`cd21e3e3`。Card 选择的 0.x/1.x dialect 同时改变 method/body/version header；SSE task/status/artifact 组装保留 output，已观察 event 后不 dialect retry。Server encoding 由 version header 决定。答案优先级为 terminal/interrupted agent status、artifact、agent history。Fixed-length stream reply 保留 bounded body/status，拒绝 early EOF/buffered surplus。保留 bound discovery/restart 行为。本地 native/Python peer 验证通过，macOS 验证尚未完成。
- **保留的 research、fork 和 example 修复。** 将 `861619a4`、`03e78f6d`、`5553d9ea`、`9b4b2776` 集成到 typed 路径。Research 仅对已完成且无 tool call 的 empty `MaxTokens` 额外调用最多两次，在原 bank/deadline 下将 cap 倍增至 16384，保留独立 call identity 与每次 outcome/usage report。可见 partial report 为 `Incomplete` 而不重试；empty final report 失败。Exception/已交付 stream part 不授权 redispatch。Fork 保留 pending continuation；example 08 保持 new-turn 流程。Evolution 在两种 mode 注册 `pnoop`，plan replay 节省四次调用。Research、file mode 与修改 helper 的限定检查已在本地通过。
- **Breaking 类型化 Python provider API。** Python 使用 `make_provider_request`、`Provider.prepare`、单次使用的 `Provider.dispatch` 和拥有所有权的 `ProviderOutcome` completion/failure view。`SchemaProvider` 接受 `ValidatedDescriptor`、`ProviderRuntimeOptions` 与 `SchemaProviderDefaults`；类型化 `ProviderMessage` 独立于 graph 便捷类型 `ChatMessage`。缺失 usage 保持 `None`。旧 completion API 与 `OpenAIProvider`/`RateLimitedProvider` 已删除，无 shim。Installed Linux wheel 与实际示例执行在上述限定范围通过，不是所有 platform/hosted vendor 的新验证。
- **事件驱动 provider handoff；C++ 必须重建。** 将SDK Bridge的1ms完成/取消polling替换为capacity-one coalesced Asio通知。Active drain无需额外wake提交便重新观察publication hint，context shutdown在channel teardown前分离non-owning publisher endpoint。`CancelToken::stop_token()`与`is_cancelled()`共用`std::stop_source`状态，保留fork/Asio-slot取消，unbound/shared token也不占用调用者slot。所有C++ consumer必须以匹配header/library重建，不能仅替换pre-v1 shared library。Local TLS GraphEngine A/B：48 process record、4800 measured graph run、失败0；256B padding p50为1.302474→0.952992ms，吞吐738.90→1022.96 graph run/s。Tool/SSE吞吐下降2.54%/6.69%，并非普遍speedup。43项targeted regression、200轮ASan/UBSan publisher-teardown、300次installed-SDK consumer通过；TSan无法在此host执行。未改变Parser/ISA flag及paid/Python qualification。参见[实测结果](benchmarks/provider-notification-summary.json)。
- **E2E发现的 protocol/demo 修正。** `neograph_get` 不再忽略 URI/cursor page，而是选择 retained 结果；stdio/HTTP lifecycle、URI及SQLite重启验证通过。空 A2A discovery URL 跟随真实 bind endpoint 和重启，保留显式 public URL；uppercase示例广告已验证 SSE capability。MCP时间demo使用所请求的 IANA timezone。ProgramChat mock显式选择提供的 model-fact policy，不绕过 bounded admission。新验证覆盖39个编号target、9个offline Beast模式、Assembly A2A、Jarvis持久化、专用mock topology load、retrieval admission和ProgramChat SQLite/PostgreSQL/browser；measurement-only/live/voice/GUI/延期Python边界单独保留。
- **文档/注释一致性。** 已归档 Stage 3 提案明确标记已删除的 Provider completion API 已被取代，并链接当前类型化 prepare/dispatch 指南。Cookbook 状态区分已实际运行的离线 Assembly/Jarvis/Beast 及 ProgramChat PostgreSQL/browser 范围与历史耗时、未验证 live/voice/Python 变体和独立 multitenant load recipe。当前 cancellation/HTTP/isolation 注释不再描述已删除的 provider API。Runtime 行为和 qualification 边界均未改变。
- **Breaking C++ typed provider/custody 契约。** 所有 consumer/custom provider 必须以匹配的新 header/library 重建。即使 `NEOGRAPH_BUILD_LLM=OFF`，Core 也要求外部 `SchemaProvider::runtime`。Provider 使用拥有所有权的 `ProviderRequest` → move-only `PreparedProviderRequest` → 不可变 `sp::runtime::Result`、显式 Collect/Stream 和声明的 family control。Completion 兼容 type、JSON descriptor interpreter、primitive registry、Responses WebSocket 已删除，无 alias/shim。SDK `0.1.0` 是 alpha（interface revision/shared generation 4），不是稳定 interface。下方此前 C++ runtime/archive 验证是 SDK3 Linux/POSIX 记录，没有执行 Python wrapper 或 retained SDK4 变更。

  Fresh installed find_package Program C++/C ABI/dualQuickJS consumer 与 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer pass。Interface/ABI 声明本身不同于实际 package 结果；不声称更广 platform 或稳定 release。
  Native custody 为 closed v3 / `spna3`（拒绝 v2），绑定全部 semantic descriptor field 和精确 owner/custody。诊断 duplicate key 保留原始 raw evidence，但不能成为可执行 config；non-2xx JSON/named SSE error 保留为事实 SDK 证据。Host-positive `observer_limits` 只能降低上限，digest v3 绑定实际上限。PMR ownership 包括 queued/draining container，cancellation 在 queue mutex 外进行。Generic messages 保持 Generic，mapped native history 保留 C++ sidecar。
  共同 `ProviderOutcomeError`、`ProviderFailure`、`ProgramFailure` 保留全部 SDK 结果及原始/次要 live cause，不发生 outer Node/Program 重新 dispatch。Native failure witness 需要真实 archive custody/Runtime hydration；预填 data-only witness 不能绕过 admission，nonpersistable 失败不能 publish/replay，重启后的原始 exception pointer 为 null。
  `RecordedBindingSet` 是 trusted Catalog 从 source event capture 并认证的 source-bound data。`replay_recorded` 检查原始 selected-source permission 后以 durable CAS 转移实际 remaining credit；inherited spend 不是新 grant。续期型 `start_recorded` API 已删除；精确 owned Program lease 不因 expiry 续期，captured JavaScript command 不能重复 external effect。持有 Archive 不授予 spending 权限。分别批准的最小 Images/Veo/Decisions 三次调用成功不是广泛 qualification 或 invoice 证据.实际集成验证与剩余限制见下文，此前 M5 测量为历史。

  **Recorded-control causal fix 已在 full suite 实证。** Captured command replay 在执行前仅为新的 CPU wall-time/Core work 建立 durable reservation，再通过 result CAS publish 测量 work 与新产生的 Core checkpoint。不消耗新的 model、money、Program-operation allowance，也不重新 dispatch captured external effect。未结算 reservation 保持 debit。Reservation 选择认证 settlement transition，而非曾拒绝首个新 Core checkpoint 的普通 Running→Running transition。Await channel receive、timer wait/cancel、handoff wait 的开始/release 在所属 executor/strand 上串行化；既有 Recorded CPU/Memory await/handoff scenario 在 full suite pass；remote TSan coverage 限制如下明确保留。

  **付费观测已完成；不是普遍 qualification。** 原始 `SPQUAL1` base630/1000000 microUSD 不变；同一原始 ledger 中 ONE hash-chained `A` 接纳批准的 extension480/3000000，aggregate1110/4000000。Calls/spent/hold/settlement 累积，不产生新 grant ID/header/reset。精确 declaration byte/file identity 和 original authorization/baseline/catalog/activation/ledger-prefix hash/totals 仍固定；删除、替换、变更均 fail closed。最终 canonical ledger 为 calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 是 LOCAL catalogue meter，不是 invoice。记录的 five-family60-pair baseline 完成600 request：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4次）、Interactions57/60（incorrect-vision buffered1次/SSE2次）；合计293/300 pair，不是300/300。其他 old600 financial record 保留，但不是完整 behavioral proof。此前 M5/media one-shot cohort 不变。此前 Google3-round prerequisite 保留 invalid-tool2次/unreadable-positive1次失败状态。不批准更多付费调用。最终 SDK 证据与 native-axis 限制不同于 baseline 成功。 此前 activation/reopen smoke 保留为两次 reopen 后 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass；这是限定的历史 checkpoint，不是最终 ledger totals。此前验证的 Chat60-pair cohort 保留实际 attempt120、UpperBound charge120、无 UnknownHold。

  **Native-axis 观测不是 cryptographic 验证或 native consumption/equivalence。** Generate 接纳 mutation/omission/duplication。Interactions 接纳 isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication。删除全部 thought/signature 返回 generic400；保留 THOUGHT item 而删除全部 signature field 也返回 generic400。最后一次 capture 只有 local encoded-original retention control，没有 same-capture server positive；此前 positive cohort 仍是真实证据。这仅建立 aggregate-carrier-absence boundary，不证明 issuer/signature 验证或 vendor consumption。实际 report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`；prerequisite-failed/not-run/negative-inconclusive 状态保持为事实。 Thought-only/carrier-only omission 在仍有其他 carrier 时被接纳；这不加强 issuer-validation/native-consumption 声明。

  **实际集成证明及剩余限制。** 最新 Core full run：2242 test、失败0、skip16（RAM process-loss 不适用14项/live-credential gate2项）、130.17秒。`PgNestedJsonRoundTrips` 精确保留 duplicate key/order/null metadata、blob、residual，0.18秒 pass。未修改的原始 shared-bank fork 和既有 Recorded CPU/Memory await/handoff scenario 均 pass。真实 wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe 在 plain 和 ASan+UBSan pass。LOCAL Memory/SQLite/PostgreSQL TSan scope7项 pass、warning0。包含 system Abseil/Protobuf 的 full mixed gRPC TSan 为 exit66，dependency/generated-RPC stack 有 race warning402项。这是 instrumentation/coverage 限制，不是已证明的 false positive；不声称 remote TSan/race-free，不 suppress warning。Installed find_package Program C++/C ABI/dualQuickJS3个 consumer pass。Fresh installed NeoGraph/SchemaProvider typed consumer 实际2个 HTTP request、coroutine 开始前 provider 销毁、native/tool replay、refusal、known-zero/raw 保留、实际 LinkedMismatch 拒绝均 pass。Browser Alice/Bob isolation、generation2 replacement 已实际目视验证；PostgreSQL Program Chat black-box6项18.989秒 pass。最新 SDK26/26、失败0、74.07秒 pass。最终 ReleaseGraph16配置 ×fresh process3次/48记录以38.29秒、失败0、全部 actual protocol/owned-outcome check pass 完成。NeoGraph `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保留独立最终 cohort。测量期间未执行 compiler/付费 model；历史 cohort 不变，不声明 semantic/resource equivalence。Unstable SDK/ABI3 不是稳定 release 或更广 platform qualification。

  已完成的 chat pair 不证明 downstream vendor 对 native-continuation 的消费。
- **Standalone bank journal 修正——当前契约已修订；实际 runtime 证据如下。** Owner-approved protocol 要求单调 trusted-store namespace obligation，以及真实不可变 original owner/thread/graph scope、ceiling、deadline/clock identity、generation。只有对全部 checkpoint commitment/revision 的精确 durable head CAS 才可发放 host-owned opaque lease。精确 pending effect window 必须在 provider I/O 前持久化；结算必须采用真实 SDK outcome 及实际 charge、nullable report、hold、dedup identity。Checkpoint 与 next head 必须在同一 owned actor/revision 下原子 publish。删除 bank metadata、prune checkpoint、replay old authenticated snapshot、覆盖同一 ID 或失去 actor 都不能授予 credit。已有 65 hold 时将 ceiling 130 降至 129，不能再批准另一个 65；已证明 no-effect 的失败可 release unchanged head，使 authentic 130 恢复仍可进行。Crash/unknown/lost-lease window 保持 hold，不 refund/retry/fallback。Plain/pristine archive 配置不授予 money/native spending lease；当前 `config.usage` 不能替换既有 standalone obligation，Program/external-bank journal 所有权不变。这是要求契约。实际 currency/custody 证据与 instrumentation 限制见下文，不是稳定 released API 保证。
  实际声明包括 `ManagedBudgetLeaseScope`、`OwnedManagedBudgetLease`、`ManagedBudgetEffectReceipt` 及 checkpoint-store acquire/begin/settle/publish/release 操作。`begin_managed_budget_effect` 绑定精确 claim/prepared request digest。实际 backend/runtime currency probe 与 installed-consumer 结果已记录；声明本身不是 runtime 证据。

  **真实 InMemory shared-bank fork 已保留并实证。** 原始真实 C++ fork 使用 ONE original financial journal 和 trusted current branch head，不复制 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 及 `_async` 要求 authentic current source/full commitment 与实际 same-bank native C++ pointer；durable standalone fork 仍明确 unsupported。`OwnedManagedBudgetLease::scope()` 及 original owner/thread/graph、ceiling、deadline/clock、generation 保持不可变。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` 单独选择 execution branch；`GraphState::budget_original_thread_id()` 标识原始 financial bank。精确 selected-branch head CAS 和 global actor/revision 将所有 branch 对 canonical current counter、pending effect、burned identity 串行化。Original/fork branch 保持可用但不补充额度。Stale snapshot、checkpoint copy、imported JSON 不能发放 alias 或回退 head。原始 root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank 证明在未修改 test_graph_engine.cpp:810–913 中 PASSED；saved original ceiling30 不同于 effective fork ceiling20；widening31 和 JSON-only restore 必须拒绝。Unbounded reported observation 是事实 data，不是 finite grant。只有已证明 zero-effect 的 lease 能 release unchanged head；unknown/pending effect 保留 obligation。

  **当前 release-error 契约；实际 suite/probe 如下。** `<neograph/graph/engine.h>` 中 `graph::ManagedBudgetLeaseReleaseError` 继承 `ProviderOutcomeError`。`cause()` 保留原始 execution exception，`release_error()` 暴露次要 durable lease-disposition 失败。`outcome()` 在存在真实 SDK 证据时保留它，若没有 SDK outcome 则为 null；release 失败不能伪造结果或授权重新 dispatch。Closed `_neograph_managed_budget_scope` metadata 描述原始 logical scope/cap/deadline clock/generation，但只是 data，不是 backend CAS 权限。

  **Archive-owner/retention 契约；实际 suite/probe 如下。** 只有 finite standalone root 或 authenticated finite source 才从真实配置的 `sp::NativeArchive::owner_scope()` 继承省略的 original owner；unbounded/plain owner metadata 语义不变。显式冲突的 archive owner 在 lease acquire 前拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 及对应 Core/Async storage capability 默认 false；真实 InMemory backend override 为 true，wrapper 必须委托真实 retention。此 read-only 描述允许合法 unleased/plain/unbounded C++ native checkpoint custody，但不授予 spending credit 或 native replay authority。Leased custody 使用真实 store-issued receipt，而非 JSON flag 或猜测的 store type。

  **Native-custody pre-I/O gate；实际 suite/probe 如下。** Managed effect begin 在任何 pending-effect/slot/held-window 修改前要求真实绑定的 NativeArchive 或实际 local store-issued private C++ retention capability。Private capability 不从 JSON import，也不经 wire 传输。C++ sidecar 无法跨越边界，因此即使 remote backend 是 InMemory，gRPC 仍要求真实 client/server archive。Archive 未提供 finite source owner 时，原始 anonymous owner scope 保持空值；真实 archive binding 必须匹配 original scope。Financial head/lease 证据本身不证明 native-custody readiness。

### 新增
- **按所有者作用域划分的实验性活动 Program 启动。** C++ `ProgramRuntime`
  和 Python `LocalProgramHost` 在新运行时只选择一次已批准的不可变激活记录，
  同时返回所选激活记录和固定于对应版本的句柄。回滚仅影响后续启动。
  图迁移不再假定未知的节点局部状态可迁移，而是拒绝不透明的检查点元数据。
  跨存储持久 GC 和生产性能验证仍是独立的准入条件。
- **Program Core Tool grant 的持久身份。** 宿主可在 SQLite 中批准受所有者
  作用域约束的精确 Tool grant，并在每次 Program 操作及重连时重新绑定
  gate/controller。运行时还校验已批准的可执行绑定 fingerprint；撤销在重启后
  仍然有效，并阻止该 grant ID 的所有尝试。效果结果仍需单独的逐调用持久
  Tool broker。
- **性能构建配置。** 新增面向本机 GCC/Clang 构建的可选
  `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` 开关；当单配置生成器未设置
  `CMAKE_BUILD_TYPE`、因而遗漏 Release 优化标志时发出警告。Release 加固
  默认保持启用。
- **Program 驱动 A2A 服务的严格认证。** Program A2A 构造函数新增可选的
  `require_authenticated_requests` 标志。启用后，除协作 envelope 外，普通
  message、stream、task 查询和取消 RPC 也必须通过已配置的 authenticator。
  为保持兼容，默认值仍为 false。
- **宿主拥有的 A2A 控制消息拦截。** `ProgramAgentAdapter` 现在可在启动
  `ProgramRuntime` 前，把已认证的 typed message 交给宿主 callback。callback
  必须保持 task/context identity，返回的 Task 会保留用于常规 task 观测。
- **不受信任的补充运行时上下文。** 新增
  `ContextArtifactKind::UntrustedSupplemental` 和
  `ContextPlacement::AfterHistory`。现有 artifact kind 的 system message
  行为保持不变，同时可以将显式不受信任的 RAW 或 derived context 绑定到
  receipt，并在完整的 Human/AI/Tool 时间顺序历史之后作为 user data 传递。

### 修复
- **子图恢复边界。** 管理状态更新保留中断执行的续行身份；保留型子图区分新的
  父调用与同一次调用的恢复。Stateless 静态中断在产生效果前拒绝，中断恢复使用
  异步检查点读取。
- **MCP 与宿主进程隔离。** 采用的工具需要独立固定审批和精确参数值；撤销或
  schema 变化会终止仍被引用的 client。加固启动与 stderr 限额、handle/FD
  隔离、后代清理及输出读取上限。
  macOS 启动支持 Darwin 信号宏，并在 fork 前通过平台访问器读取继承环境。
  POSIX close 回退使用 descriptor 硬限制而非 65536 上限，也会关闭降低软限制前
  已打开的高编号 FD。Windows 继承测试检查父进程原始 event 的状态，
  不再把可复用的 handle 数值当作对象身份。
- **干净 runner 上的契约检查。** CI 显式安装 Protobuf 开发头文件与库，
  而不只是编译器和运行时包。
  隔离 stdio fixture 显式批准用于发现 Python 的搜索路径。
- **租户安全的持久 Harness 记录。** File/SQLite 中的作用域 ID 和引用可往返，
  长 File 键使用固定长度 hash 文件名。SQLite 保留策略不会删除其他 namespace
  的记录或 journal。
- **可运行的 Harness CLI。** 启动策略使用 Program 支持的子深度上限，真实 stdio
  compile/start/get 回归验证覆盖交付的 binary。
- **新目录中的活动 Program 启动。** 在固定运行版本之前恢复已选定的持久版本，
  不再重新读取激活指针。
- **生成 artifact 的持久重放。** Provider receipt 在重新打开和结果核对后仍
  保留 artifact 顺序、payload 和嵌套 metadata。易失性 SQLite 路径以及缺少
  artifact 证据的旧 receipt 会被明确拒绝，不会伪造空结果或重新发送调用。
- **宣传视频依赖安全。** 将 `docs/promo/package-lock.json` 中的间接依赖
  `js-yaml` 从 4.3.1 更新至 4.3.2，修复通过合并空映射耗尽 CPU 的
  拒绝服务漏洞（CVE-2026-84375 / GHSA-2883-xcg3-v3hh）。
- **TSan CI RSS 检查。** 由于 shadow memory 和运行时管理开销可能超过
  普通构建的阈值，在 GCC/Clang ThreadSanitizer 下跳过仅适用于普通构建的
  RSS 上限检查。保留 sanitizer 下的 200 路并发 stress test，以及非
  sanitizer 构建中原有的 10 MiB RSS 上限。

## [0.12.1] - 2026-08-23

### 修复
- **Python 示例运行时契约。** 路由通道示例改用保留的 `__route__`，声明动态
  `Send` 载荷通道，并将纯缓存示例设为 `CacheScope::Reusable`。同时保留
  OpenRouter 基础 URL，使 Windows 控制台输出具备 UTF-8 安全性，并拒绝或重试
  状态成功但内容为空的 reasoning 模型响应。
- **Responses 传输演示。** 官方 OpenAI 使用真实的 WebSocket 流式入口，兼容
  网关则选择构建中可用的 HTTP/2 或 HTTP/1.1。深度研究演示采用有界的令牌和
  时间预算以及三路、两 worker 的 fan-out，并已通过 OpenRouter 实际验证。
- **可操作的 Windows 网络错误。** Python 现公开 `_HAVE_LIBCURL`，并把采用
  本地代码页编码的 `std::system_error` 消息转换为含 category 与 code 的安全
  ASCII 诊断。因此 Winsock 10060 等根因不会再被 `UnicodeDecodeError` 覆盖。

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

- **补全 Python Program wheel 依赖。** PyPI 构建现会启用 QuickJS 控制运行时，在扩展模块旁捆绑 `neograph_program` 加载器，并导出 Windows 上跨 Program/Harness DLL 边界使用的私有求值器。这样可防止 `P_JS_UNAVAILABLE`、因缺少加载器导致的 import 失败，以及共享构建中的 `LNK2019` 失败。命令 identity 的构建也会避免跨组件 dylib 边界的嵌套 initializer-list JSON 复制，从而防止在 Program 执行期间可能发生的 macOS arm64 崩溃。Windows wheel repair 会对外部 DLL 名称进行哈希改名，防止预先加载的系统 `sqlite3.dll` 覆盖捆绑版本并在构建 `SQLiteContextStore` 时导致崩溃。
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
      public build guidance — only the option toggle was
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
