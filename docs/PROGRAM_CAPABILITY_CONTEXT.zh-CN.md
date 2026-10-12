<!-- neograph-i18n: source=docs/PROGRAM_CAPABILITY_CONTEXT.md locale=zh-CN source_sha256=19977a5adc5fc8fac5dafb2f604519c6974ccafa509aa01ccafd393cf0c7c962 -->
# Program 节点的能力与受中介的副作用

**Languages:** [English](PROGRAM_CAPABILITY_CONTEXT.md) | [한국어](PROGRAM_CAPABILITY_CONTEXT.ko.md) | [日本語](PROGRAM_CAPABILITY_CONTEXT.ja.md) | [简体中文](PROGRAM_CAPABILITY_CONTEXT.zh-CN.md)

`ProgramCatalog` 只绑定一次整体的可执行闭包，但节点工厂应当只看到该节点已准入的配置所声明的能力。
因此，`RegistrySnapshotBuilder` 依据节点 manifest 中直接的可执行需求，
以及它针对具体配置的需求解析器，来收窄构造时的 `NodeContext`。
别的节点所用的、已绑定的 Provider 或 Tool，并不是这个节点隐含具备的能力。

| 已注册的节点 | Provider 指针 | Tool 指针 | 安全的元数据 |
| --- | --- | --- | --- |
| 任意 `Brokered` C++ 工厂 | 无 | 无 | 确切的 `provider_name` 和 `tool_definitions` |
| 经宿主审阅的 `add_host_brokered_node` | 确切声明的 Provider | 确切声明的 Tool | 确切的元数据 |
| 固定的 `add_core_llm_call` | 确切声明的 Provider | 确切声明的、用于模型定义的 Tool | 确切的元数据 |
| 固定的 `add_core_tool_dispatch` | 无 | 确切声明的 Tool | 确切的元数据 |
| `TrustedNative` C++ 工厂 | 确切声明的 Provider | 确切声明的 Tool | 确切的元数据 |

这两个固定的 Core 注册，构造的是 NeoGraph 自己的 `LLMCallNode` 和 `ToolDispatchNode`；
它们不接受调用方提供的工厂。注册它们时，要配上确切的、不可变的需求解析器。
模型编写的拓扑，可以选择已准入的类型和节点配置，但无法让任意的 brokered 工厂拿到原始的 Tool 指针。
标准的分发节点仍然调用 `dispatch_tool_calls`，在调用已绑定的 Tool 之前，
它会先询问这次运行的 `ToolGate` 和 `ToolExecutionController`。

`TrustedNative` 是一道单独的、由宿主担保的边界。Program 的准入(admission)，已经要求 `TrustedEmbedding` 模式，
以及与该副作用模式相匹配的、经过认证的 Catalog 宿主标识。
原生 C++ 代码可以持有在 `NodeContext` 之外捕获的资源，也可以自己产生副作用；
这些规则是对能力的收窄，不是进程沙箱，也不能证明任意的原生代码是纯的。
宿主必须审阅并固定原生工厂，并如实声明它们的副作用。

Program Core 的操作，要使用受中介的 Tool 分发，需要一份由宿主持有的 `ProgramCoreToolGrant`。
运行时会检查 owner、Program 版本、运行、操作、尝试、**已准入的可执行绑定指纹**、
非空的授权 ID、闸门和控制器。授权缺失或已过期，会拒绝这次 Tool 调用，重新连接之后也是如此。
被拒绝的 Tool 调用是一种 Tool 结果，Program 本身仍可能是 `Completed`；
判断所请求的工作是否成功时，要检查副作用回执。

需要持久授权标识的宿主，可以在用显式的运行 ID 启动运行之前，
在 `SQLiteProgramCoreToolGrantStore` 里准入一条确切的 `ProgramCoreToolGrantRecord`。
绑定指纹是 `capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings)`。
把 `make_durable_core_tool_grant_resolver(store, policy_factory)` 安装为
`RuntimeConfig.core_tool_grant_resolver`：它会为每个操作（包括重新连接）重新加载这条记录，
并且只接受授权 ID、owner、版本、运行、操作、尝试和绑定都完全一致的、全新的宿主策略。
只有对完全相同的、仍然有效的记录，准入才是幂等的；ID 或绑定相冲突的记录无法替换它。
恢复的 Program 会推进它的尝试，所以宿主在恢复之前，必须为该次尝试，显式准入那份仍然有效的授权。
同一个授权 ID，只有在 owner/版本/运行/操作/绑定都相同的前提下，才可以覆盖这些尝试；
撤销会让它所有已准入的尝试一并失效。已存储的记录里不含凭据、Tool 指针或回调。

是否准入，以及如何重建每个 Tool 的闸门和控制器策略，只由宿主决定。
这条记录是权限的证据，**不是**副作用的结果：宿主仍然必须通过逐次调用的副作用 broker，
把每次 Tool 分发、完成和状态不确定的结果记入 journal。
这并不承诺外部执行恰好一次，也不约束任意受信的原生代码绕开受中介的分发去调用 Tool。

`neograph::sqlite` 中的 `SQLiteToolEffectBroker`，为 `dispatch_tool_calls` 实现了持久的预写式（write-ahead）分发：
用持久的 SQLite 路径，以及宿主注册的 `{Tool*, executable_id}` 绑定来构造它。
可执行 ID 必须固定住实际的代码、配置、远端目的地和凭据权限，而不只是模型可见的 Tool 名称。
把这个 broker 放进 Program 的授权里，或者放进独立 Core 的 `RunResources::tool_effect_broker`；
独立 Core 还要设置匹配的 `tool_effect_grant`。使用 `make_tool_execution_context` 的内置节点和宿主注册的节点，
会继承这些以调用为作用域的资源，而不需要改动共享的引擎。
独立的 `llm::Agent::run`/`run_stream` 接受每次运行的 `ToolExecutionContext`，
其中带有同样的 broker、稳定的 owner/运行/线程标识、操作和宿主授权。
调用方必须保留完整的、有序的 `sp::Message` 历史（包括 native 和 Tool 的 parts），
才能在重新连接时重放待处理的批次；仅供显示的 `ChatMessage` 投影，无法重建 native 续接的权限。
持久的 native 托管，另外还需要宿主的 `sp::NativeArchive`。
旧的、没有 broker 的 Agent/Core 路径，仍然明确属于较低保证级别。

对于每个 Core 任务和批次序号，broker 在调用控制器之前，先用 SQLite FULL 同步提交一个标记；
标记失败，就不会执行任何 Tool。回执绑定 owner/运行/线程/任务/序号、Program 版本/绑定、
操作/授权、确切注册的可执行对象，以及规范化改写后的参数。
模型给出的 `ToolCall.id` 只用于关联。已确认的回执会直接重放，不再分发；
标识、权限或参数相冲突，则被拒绝。待处理的标记、超时、取消、分发之后执行失败，
或者回执提交失败，都需要外部对账，并且在重启之后也阻止重新分发。
尝试的变化，只作为来源信息记录，不构成新的调用槽位。不要声称外部结果恰好一次：
即使有持久的标记，也无法判断外部服务在崩溃之前是否已经提交。

原先使用 `NodeContext.provider` 或 `NodeContext.tools` 的、brokered 的自定义工厂，
必须把执行迁移到固定的 Core 节点上，或者使用单独审阅过的宿主 broker，
或者按宿主的受信嵌入策略，作为 `TrustedNative` 准入。
`add_host_brokered_node` 是 Harness worker 所用的、显式的原生宿主 broker 注册路径。
它并不会让调用方提供的工厂变得安全：宿主必须审阅并固定它的副作用，
并让每个操作都留在已准入的 provider/tool 闭包之内。普通的 `add_node` 仍然只允许元数据这一道边界。
Program 之外的直接 Core 图，保持它们原有的 `NodeContext` 行为。
剩余的持久化和原生代码方面的边界，见 [#291](https://github.com/fox1245/NeoGraph/issues/291)、
[#292](https://github.com/fox1245/NeoGraph/issues/292) 和
[#293](https://github.com/fox1245/NeoGraph/issues/293)。
