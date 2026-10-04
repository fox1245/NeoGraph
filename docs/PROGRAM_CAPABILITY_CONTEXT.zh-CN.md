<!-- neograph-i18n: source=docs/PROGRAM_CAPABILITY_CONTEXT.md locale=zh-CN source_sha256=19977a5adc5fc8fac5dafb2f604519c6974ccafa509aa01ccafd393cf0c7c962 -->
# Program 节点能力与受中介管理的效果

**Languages:** [English](PROGRAM_CAPABILITY_CONTEXT.md) | [한국어](PROGRAM_CAPABILITY_CONTEXT.ko.md) | [日本語](PROGRAM_CAPABILITY_CONTEXT.ja.md) | [简体中文](PROGRAM_CAPABILITY_CONTEXT.zh-CN.md)

`ProgramCatalog` 一次绑定整个可执行闭包，但节点工厂只能看到该节点已获准配置所声明的能力。因此，`RegistrySnapshotBuilder` 根据节点 manifest 的直接可执行要求和配置专属要求解析器，收窄构造时的 `NodeContext`。另一个节点使用的已绑定 Provider 或 Tool，不会自动成为本节点的能力。

| 注册节点 | Provider 指针 | Tool 指针 | 安全元数据 |
| --- | --- | --- | --- |
| 任意 `Brokered` C++ 工厂 | 无 | 无 | 精确的 `provider_name` 与 `tool_definitions` |
| 经宿主审查的 `add_host_brokered_node` | 精确声明的 Provider | 精确声明的 Tools | 精确元数据 |
| 固定 `add_core_llm_call` | 精确声明的 Provider | 用于模型定义的精确声明 Tools | 精确元数据 |
| 固定 `add_core_tool_dispatch` | 无 | 精确声明的 Tools | 精确元数据 |
| `TrustedNative` C++ 工厂 | 精确声明的 Provider | 精确声明的 Tools | 精确元数据 |

两个固定 Core 注册入口构造 NeoGraph 自有的 `LLMCallNode` 和 `ToolDispatchNode`，不接受调用方提供的工厂。注册时应使用精确且不可变的要求解析器。模型编写的拓扑可以选择获准类型和节点配置，但不能让任意 brokered 工厂接收原始 Tool 指针。标准派发节点仍调用 `dispatch_tool_calls`，后者在调用已绑定 Tool 前检查本次运行的 `ToolGate` 和 `ToolExecutionController`。

`TrustedNative` 是独立的宿主认证边界。Program 准入已要求该效果模式使用 `TrustedEmbedding`，并具有匹配的、经过认证的 Catalog 宿主身份。原生 C++ 代码可以保留在 `NodeContext` 外捕获的资源，或自行执行效果；这些规则只收窄能力，不是进程沙箱，也不证明任意原生代码是纯函数。宿主必须审查并固定原生工厂，如实声明其效果。

Program Core 操作需要宿主所有的 `ProgramCoreToolGrant` 才能通过中介派发 Tool。运行时检查 owner、Program 版本、run、operation、attempt、**获准的可执行绑定指纹**、非空 grant ID、gate 和 controller。缺失或过期的授权会拒绝 Tool 调用，重连后也一样。被拒绝的 Tool 调用是一个 Tool 结果，Program 本身仍可能为 `Completed`；判断请求的工作是否成功时应检查效果回执。

需要持久授权身份的宿主，可在以显式 run ID 启动运行之前，将精确的 `ProgramCoreToolGrantRecord` 准入 `SQLiteProgramCoreToolGrantStore`。绑定指纹为 `capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings)`。将 `make_durable_core_tool_grant_resolver(store, policy_factory)` 安装为 `RuntimeConfig.core_tool_grant_resolver`：它对每次操作（包括重连）重新加载记录，只接受 grant ID、owner、version、run、operation、attempt 和 binding 完全相同的新宿主策略。只有完全相同的活动记录可幂等准入；冲突 ID 或绑定不能替换它。恢复 Program 会推进 attempt，因此宿主必须在恢复前显式准入该 attempt 仍获授权的 grant。同一 grant ID 只能在相同 owner/version/run/operation/binding 内覆盖这些 attempts；撤销会禁用它所有已获准 attempts。存储记录不含凭据、Tool 指针或回调。

只有宿主决定是否准入，以及如何重建每个 Tool 的 gate 与 controller 策略。记录是权限证据，**不是**效果结果：宿主仍必须通过逐调用效果 broker，记录每次 Tool 派发、完成和不确定结果。这不承诺外部执行恰好一次，也不约束绕过中介派发调用 Tool 的任意可信原生代码。

`neograph::sqlite` 中的 `SQLiteToolEffectBroker` 为 `dispatch_tool_calls` 实现持久预写派发：以持久 SQLite 路径及宿主注册的 `{Tool*, executable_id}` 绑定构造它。可执行 ID 必须固定实际代码、配置、远端目的地和凭据权限，而不只是模型可见的 Tool 名称。将 broker 传入 Program grant，或独立 Core 的 `RunResources::tool_effect_broker`；独立 Core 还需设置匹配的 `tool_effect_grant`。使用 `make_tool_execution_context` 的内置及宿主注册节点继承这些调用范围资源，无须修改共享 engine。独立 `llm::Agent::run`/`run_stream` 接受逐运行 `ToolExecutionContext`，其中具有同一 broker、稳定 owner/run/thread 身份、operation 和宿主 grant。调用方必须保留完整且有序的 `sp::Message` 历史（包括 native/tool 部分），才能在重连后重放待处理批次；仅用于显示的 `ChatMessage` 投影不能重建原生续接权限。持久原生保管还需要宿主的 `sp::NativeArchive`。传统无 broker 的 Agent/Core 路径仍明确具有较低保证。

对于每个 Core task 和 batch ordinal，broker 在调用 controller 前提交 SQLite FULL-sync 标记；标记失败不会执行任何 Tool。回执绑定 owner/run/thread/task/ordinal、Program version/binding、operation/grant、精确注册的可执行项和规范化的重写参数。模型 `ToolCall.id` 仅用于关联。已确认回执直接重放，不再派发；身份、权限或参数冲突均被拒绝。待处理标记、超时、取消、派发后执行失败或回执提交失败，都需要外部对账，并跨重启阻止再次派发。attempt 变化记录为来源信息，不产生新的调用槽位。不要声称外部结果恰好一次：即使有持久标记，也无法判断外部服务是否在崩溃前已提交。

现有使用 `NodeContext.provider` 或 `NodeContext.tools` 的 brokered 自定义工厂，必须将执行迁移到固定 Core 节点，使用单独审查的宿主 broker，或依据宿主可信嵌入策略以 `TrustedNative` 准入。`add_host_brokered_node` 是 Harness worker 使用的显式原生宿主 broker 注册路径。它不会自动使调用方工厂安全：宿主必须审查并固定其效果，确保每个操作都在获准 provider/tool 闭包内。普通 `add_node` 保留仅元数据边界。Program 外的直接 Core 图保留原有 `NodeContext` 行为。其余持久性及原生代码边界见 [#291](https://github.com/fox1245/NeoGraph/issues/291)、[#292](https://github.com/fox1245/NeoGraph/issues/292) 和 [#293](https://github.com/fox1245/NeoGraph/issues/293)。
