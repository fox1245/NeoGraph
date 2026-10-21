<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=zh-CN source_sha256=acab2f08683f42b29a9e0b385b13fe6d1107a87938bfb97a6b14ee86286d5ca6 -->
# 严格的运行时拦截

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph 的严格运行时路径，把强制的上下文、生命周期 Hook 和 provider 分发的证据，
从模型的自由裁量中拿了出来。直接发起带类型的 provider 调用，仍然可用于受信的嵌入场景；
`StrictRuntimeProfile` 把严格路径所需的依赖装配在一起。

## 保证的边界

```text
durable RAW history + admitted artifacts + required Skills/constraints
  -> immutable ContextEpoch
  -> RuntimeTurnAssembler
  -> ContextAssemblyReceipt
  -> mandatory BeforeProviderRequest Hooks
  -> durable ProviderDispatchReceipt
  -> provider
  -> ProviderDispatchOutcomeReceipt
  -> mandatory AfterProviderResponse Hooks
```

这项保证涵盖：上下文的精确构造、强制制品的存在、请求的标识、分发的准入(admission)，
以及已知的和需要对账的 provider 结果。它并不声称 LLM 关注或遵从了每一个 token。

宿主自己编写的自定义原生节点，仍然是受信的代码。给这样的节点一个原始的 `Provider`，
就有意地离开了严格 profile；生成出来的拓扑只能得到已注册的节点，无法凭空造出这种权限。

## 严格 profile

`StrictRuntimeProfileConfig` 要求具备：

- 一个 provider；
- 一个 `DurableContextStore`；
- 一个支持终态 outcome 的 `DurableProviderDispatchReceiptStore`；
- 一个 `HookRuntime`；
- 一个内容寻址的 provider 绑定标识；
- 一个非零的输入 token 上限；以及
- 可选的、确切的必需上下文和 Skill 制品标识。

只有 `RuntimeGuaranteeProfile::Strict` 的 epoch 才能被激活。把这个 profile 挂到
`GraphEngine` 上，会同时在内置的使用方上，装好 provider 拦截和生命周期 Hook。
但它不会在 provider 调用之间推进上下文；参见[宿主逐步推进契约](#host-stepped-contract)。

<a id="host-stepped-contract"></a>
## 宿主逐步推进（host-stepped）契约

严格路径的每个 provider 请求，都由当前生效的 epoch 构建：它固定的 RAW 范围和选中的制品。
图或智能体持有的对话，路径不会去读。除了 epoch，进入提示词的只有内置节点填写的宿主指令槽位
和受信补充输入（trusted supplemental input）槽位。因此在宿主记录之前，NeoGraph 看不到
模型的回复，也看不到工具结果。契约是：

**宿主每次 provider 调用前，都要追加 RAW 记录，并激活新的 epoch。**

两次调用之间，宿主把上一轮（先是模型的回复，再是工具结果）追加到 `ContextStore`，建一个 RAW 范围
覆盖这一轮的 epoch，再用 `StrictRuntimeProfile::activate` 激活它。第一个 epoch 也按同样方式，
由用户消息建立。下面的代码是其中一步。`head` 是 `store.history_head(feed)`，
`epoch_sequence` 和 `previous_epoch_id` 来自上一个 epoch。

```cpp
for (auto& [message, trust] : turn) {  // the reply (ModelOutput), then the tool results (ToolOutput)
    RuntimeHistoryRecordData record;
    record.feed_id = feed.feed_id;
    record.sequence = ++head.sequence;
    record.message_id = "message-" + std::to_string(record.sequence);
    record.trust = trust;
    record.message = std::move(message);
    record.predecessor_id = head.record_id;
    const auto created = RuntimeHistoryRecord::create(std::move(record));
    if (store.append_history(feed, created, head.record_id) != ContextStoreAppendResult::Appended)
        throw std::runtime_error("the context feed moved");
    head.record_id = created.id();
}
ContextEpochData next;
next.run_id = run_id;
next.sequence = ++epoch_sequence;
next.predecessor_id = previous_epoch_id;
next.feed_id = feed.feed_id;
next.raw_from_sequence = 1;
next.raw_through_sequence = head.sequence;
next.raw_window_digest = store.snapshot_history(feed, 1, head.sequence).digest;
next.guarantee_profile = RuntimeGuaranteeProfile::Strict;
auto epoch = ContextEpoch::create(std::move(next));
previous_epoch_id = epoch.id();
profile.activate(owner_id, std::move(epoch));
```

NeoGraph 目前还没有在一次运行内部推进 epoch 的辅助函数。

**一次运行内部的循环。** 把 profile 挂到普通的 `GraphEngine` 上，并不会推进上下文。
在一次 `engine.run()` 内部的工具循环里（LLM 调用、工具分发、LLM 调用，`create_react_graph` 构建的就是这种结构），
第二次 LLM 调用仍运行在同一个 epoch 之下。工具结果在图状态里，却不在 epoch 里。
provider 会再次收到第一轮的上下文，模型始终看不到工具结果，还可能反复请求同一个工具，
直到步数上限或预算让这次运行停下。

所以 NeoGraph 改为拒绝这样的调用。当 provider 调用到达的 epoch，其所属的一轮已经被图推进过，
就会抛出 `std::logic_error`。调用方提交的消息里，含有 epoch 首次分发之后新增的助手消息或工具消息，
而这次调用又没有把这些消息作为宿主指令或受信补充输入一并带上，就算这一轮已被推进。
错误信息会点明这一契约：

```text
Strict provider dispatch is host-stepped: the host appends RAW records and activates a new epoch for every provider call. This call presents assistant or tool messages added since the active epoch's first dispatch, so the graph or agent loop advanced inside one run and the provider would not receive them. Append those messages to the context feed and activate a new epoch before this call.
```

在图里，失败的节点以 `NodeExecutionError` 报告。logic error 不会重试，所以 `attempts()` 为 1。
通过 `set_runtime_interposition` 设置了控制器的 `llm::Agent`，在第二次 provider 调用时同样会失败。

下面这些情况不会被拒绝：

- 同一轮的重试。节点重试，或 provider 失败后的重复调用，提交的是同样的消息。
- 在 epoch 首次分发时，就已经含有助手消息或工具消息的对话。这些消息是基线。
- 只增加了用户消息。检查的对象是助手消息和工具消息。
- 把不断增长的对话作为受信补充输入传入的内置节点，例如 plan-execute 图的 executor 节点。
  它的回复和工具结果会送达 provider，并由组装回执绑定。

激活 epoch 或调用 `clear()`，都会忘记已经提交过的内容。控制器按调用点（节点名）记住首次提交的内容，
每个 epoch 只保留有限个互不相同的提交。超出上限的提交不再跟踪，此时的检查是尽力而为。

同一场对话的多次运行，请宿主自己串行化。profile 只有一个生效的 epoch，也不看 `thread_id`。
参见[一场对话的多次运行](concurrency.md#runs-of-one-conversation)。

## 选择上下文的管理方式

NeoGraph 有四种方式管理模型调用的上下文。它们的差别在于提示词由谁构建，以及保证什么。

| 方式 | 提示词的来源 | 保证什么 |
| --- | --- | --- |
| `messages` 通道 | 图状态中的 `messages` 通道，保持节点留下的样子。`LLMCallNode` 发送它并追加回复，`ToolDispatchNode` 追加工具结果，因此一次运行内部的工具循环可以正常工作 | 请求由图状态构建，检查点存储可以保存这个状态。没有逐次调用的回执，没有必需制品，也没有上下文上限 |
| `history::compact_history` | 不是独立的方式，而是返回更短消息列表的辅助函数：`estimate_tokens` 超过上限时，把较早的纯文本前缀替换为模型写的摘要，保留最近的部分。结果作为其他方式的输入使用。接受 `RuntimeInterpositionController` 的重载，会把摘要调用交给该控制器发送 | 一个状态，说明历史被保留还是被替换，以及原因；含 native 重放封印的历史原样返回。摘要是模型输出，没有任何东西证明某个事实或约束留在了摘要里。估算值只是启发式。没有 epoch，也没有回执 |
| 记录（Recorded）profile | 经由 `RuntimeInterpositionController` 的当前 epoch（`RuntimeGuaranteeProfile::Recorded`，默认值）：来自 `ContextStore` 的 RAW 范围，加上 epoch 的制品。由宿主推进步骤 | 请求恰好由这个 epoch 组装，并绑定组装回执，每次分发都有回执。存储不必是持久的，输入上限也可以为 0，所以证据保留多久、是否设置上限，由宿主决定 |
| 严格（Strict）profile | 与记录 profile 相同，经由 `StrictRuntimeProfile`。由宿主推进步骤 | 记录 profile 的全部保证，再加上持久的上下文存储与分发存储、围绕每次调用的强制 Hook、必须出现在 epoch 里的必需制品，以及在分发前拦下超预算轮次的非零输入上限。不保证模型关注或遵从了上下文，也不会代为推进 epoch |

从 `messages` 通道换成 epoch，上下文的所有者就变了：图状态不再是提示词的来源，RAW 历史由宿主保管。

## 上下文预算

严格路径会把每个组装好的请求与输入上限（`max_input_tokens`）比较，超预算的轮次在分发前就以
`ContextBudgetBlocked` 拦下。组装回执记录所用的估算值（`estimated_input_tokens`），
以及其中属于必需制品文本的部分（`mandatory_input_tokens`）。

估算值来自 `RuntimeTurnAssembler::estimate_input_tokens`：编码后请求体的字节长度除以三，向上取整。
它是启发式，不是被证明的上界。它不是由任何 provider 的分词器推出来的，所以可能小于，也可能大于 provider 实际统计的数字。
它统计的是编码后的整个请求，而不只是可见文本：工具 schema、字段名和 provider 的封装格式也都计入字节。
请把 `max_input_tokens` 当作防止请求过大的保护措施，不要当作对 provider 所报告或所计费数字的承诺。
`mandatory_input_tokens` 只对必需制品的文本按同样的比率计算。

分发的 token 预留是另一份资源契约。运行设有模型 token 预算时，每次分发会按它可能进行的每次尝试，
预留模型的输入上限加输出额度，不使用这个估算值。

## provider outcome 的生命周期

provider 边界会记录两个相互独立的不可变值：

1. `ProviderDispatchReceipt` 在分发之前写入。
2. `ProviderDispatchOutcomeReceipt` 在尝试之后，记录 `Succeeded`、`Failed` 或
   `ReconciliationRequired`。

成功的结果，会绑定完整的 SDK outcome 观测数据的摘要。
已证明未被接受的带类型 Failure（请求未发送，或被已完成的 HTTP 4xx 响应拒绝），记录为 `Failed`；送达状态不确定的，记录为 `ReconciliationRequired`。
分发之后发生异常，并不能说明远端 provider 是否已经执行，所以控制器不会悄悄重试。
SQLite schema v3 把终态回执单独存储，并在重启后检查它们与已准入分发之间的确切绑定。
回执摘要是证据，不是 native 续接的托管，也不是可以执行的、已存储的 outcome。

控制器接受 `ProviderRequest`，返回不可变的、持有所有权的 `sp::runtime::Result`，
其中保留有序的消息/parts 和部分失败的证据。如果在已有真实结果之后，结算或回执的持久化失败了，
`ProviderDispatchOutcomePersistenceError` 会保留这份结果和原始的 cause；
次要的 observer 失败仍保留在 `delivery_error()` 里。
token 的计费和预留，与可为空的 provider 用量报告是分开的。

## Program Core 的 provider 调用（与独立的严格运行时分开）

Program 使用内置的 Core LLM 节点时，宿主可以设置
`RuntimeConfig::core_provider_call_resolver` 和
`require_core_provider_call_broker = true`。对于每一个确切的
`ProgramCoreProviderCallContext`，从
`<neograph/program/sqlite_provider_call_broker.h>` 返回
`SQLiteProgramProviderCallJournal::bind(context, deployment_identity)`；需要链接 `neograph::program_sqlite`。
部署标识必须是由宿主持有的、对实际 provider 路由、模型部署和权限（包括凭据版本）求出的 SHA-256 标识；
broker 不会从 `Provider` 对象中自行发现这些信息。重启或重新连接时，要重新绑定同一个持久数据库。还要设置 `RuntimeConfig::core_provider_calls_journaled = true`。fork 会从这个 journal 重放 provider 调用，设置该标志后，这个宿主上的 fork 就不会在 `ProgramHandle::notices()` 中报告 `P_FORK_PROVIDER_CALLS_NOT_JOURNALED`。fork 如果找到了 source 在同一位置的调用，却既不能接管它，也无法证明该调用不同，就会被拒绝，除非 `RuntimeConfig::fork_may_resend_provider_calls` 为 true（见参考文档）。

journal 以 owner、不可变的 Program 版本、运行、操作、Core 的线程/任务/节点以及内置调用的序号，
作为每次调用的键，**而不是** Program 的尝试。它在键的旁边记录调用的请求内容摘要和 descriptor policy identity，并按内容来匹配调用（#543），所以同一个请求在另一个 SchemaProvider descriptor policy identity 之下，仍是同一次调用。它在传输之前，用 SQLite FULL 同步提交标记。
这个标记表示传输*可能*已经发生，并不表示 provider 收到了它，也不表示副作用恰好发生了一次。
完整的、不可变的 SDK Completion/Failure outcome，以编码版本 2 存储，用于精确绑定的重放。
已证明未被接受的 Failure（未发送，或被已完成的 HTTP 4xx 响应拒绝）记录为 `Failed`；送达状态不确定、发生异常，或在结算之前崩溃，
都需要对账，并且绝不会悄悄重新分发。用
`inspect(owner, logical_call_id(context, core_identity))` 检查状态；
只有在拿到经独立验证的 provider 一侧证据和完整的 Completion outcome 时，
才能使用 `reconcile_success`。重放流式调用时，返回的是已捕获的 outcome，不会合成流事件。

更大的输出上限，是一次新的语义调用，不是传输层的重试，也不是对同一个 journal 槽位的重放。native replay 的配置中不包含这个上限；已准备请求的摘要和保守的资源预留额度仍然包含它。给每个已准入的调用一个不同的、确定性的序号，保留每次尝试的 outcome 和核算以及原来的截止时间，并从同一个资源 bank 获得准入。已有槽位里的请求内容摘要发生变化，会被拒绝；送达状态不确定、observer 失败或结算失败之后，native 历史和游标都既不能续期额度，也不能授权再次发送。

有序的消息 parts、原始观测、可为空的用量、尝试元数据和 native 续接状态，
仍然是已存储结果的一部分。native outcome 需要把宿主的 `sp::NativeArchive`
传给 `SQLiteProgramProviderCallJournal(database_path, native_archive)`；
可移植的 JSON 投影无法重建这种权限。旧的、有损的回执会被拒绝，而不是被升级，
也不会被悄悄再次分发。journal 把保守的预留额度/已提交的 token 数，与 provider 的报告分开保存。
请使用持久的文件系统数据库路径：空路径、`:memory:` 和 `file:` URI 路径都会被拒绝。
这个 broker 作用于 Core 已有的 ReAct 消息状态，而不是装配好的 `ContextEpoch`，
并且不能与引擎的严格运行时拦截用在同一个内置调用上。宿主自己编写的原生 Provider 调用，
不在它的范围之内。

## 通过 native、stdio 或 HTTP 运行的强制 Hook

`MandatoryHookRunner` 既接受现有的 native 适配器，也接受与传输方式无关的
`HookExecutionBackend`。`RpcHookExecutionAdapter` 把 `HookRpcExecutor` 绑定到该后端。
同一个固定的 `hooks/invoke` JSON-RPC 方法，既可以走 `StdioJsonRpcTransport`，
也可以走 `HttpJsonRpcTransport`。两者都运行在 `MCPClient` 会话之上，
所以 endpoint 是一个同时提供 `hooks/invoke` 的 MCP 服务器。这个方法是 NeoGraph 的扩展，
不属于 MCP；没有实现它的 MCP 服务器会回答 `-32601`。

RPC Hook 的制品是证据，绝不是权限。`ContextStoreHookArtifactPublisher`
只接受满足下列条件的制品：

- 类型是 `HookOutput`；
- `source_digest` 等于确切的 Hook 调用 ID；并且
- 运行时事件与该调用一致。

发布以 owner 为作用域，并且是幂等的。如果外部副作用已经成功，但它的制品无法发布，
该 Hook 就会结算为 `ReconciliationRequired`；它不会被报告为干净的成功。

## 必需的上下文与转换

`RuntimeContextRequirements` 把所有必需的制品 ID，与其中必须是 `RequiredSkill`
制品的那一部分分开。`HardConstraint` 是专门的必需制品类型。每个必需的制品，
都必须被当前生效的 epoch 选中，必须保持 `required=true`，并计入强制 token 的数量。

`ContextTransformReceipt` 在 v1 中有意保持保守。转换器可以替换或压缩可选的证据，
但每个必需的输入制品 ID，都必须以字节完全相同的形式出现在输出集合里。
改写不被接受为约束得以保留的证明。

## 制品的放置位置

制品的 `placement` 决定它的文本放在 RAW 消息之间的哪个位置。组装器按下面的顺序构建提示词。
同一种放置位置内，`priority` 较高的在前，然后按 `producer_id`、`id` 排序。

```text
host instructions
BeforeHistory artifacts
RAW messages before the latest user message
BeforeLatestUser artifacts
latest user message
AfterLatestUser artifacts
RAW messages after the latest user message
AfterHistory artifacts
trusted supplemental input
```

每一轮都保持不变的制品，请放在 `BeforeHistory`。默认的放置位置是 `BeforeLatestUser`，
所以没有指定放置位置而创建的制品，每一轮都会移动：

- `BeforeLatestUser` 和 `AfterLatestUser` 锚定在最新的用户消息上。新的用户消息到来时，制品跟着移动，
  提示词从制品原来的位置起，就与上一次的提示词不同。这两种放置位置要求 RAW 窗口里有用户消息；没有的话，组装会失败。
- `AfterHistory` 始终在最后。它前面的历史会增长，但不会改变它前面那部分提示词。
- `BeforeHistory` 紧跟在宿主指令之后，与历史无关。只要制品集合和优先级不变，它的文本和位置也不变。

制品集合变了，或者 RAW 窗口丢掉了最早的记录，提示词就从那一点起发生变化。会缓存提示词开头部分的 provider，
只能复用从第一个字节起与上一次请求相同的那一部分。放置位置对缓存命中的影响，NeoGraph 还没有测量过。

`RequiredSkill` 和 `HardConstraint` 制品渲染为 System 消息，其他类型渲染为 User 消息。
System 消息怎么处理，取决于 provider 的系列：

- 在 `anthropic.messages`、`google.generate` 和 `google.interactions` 上，请求构建器会把所有 System 消息移到
  请求顶层的 system 字段，不论放置位置如何（以空行分隔，按组装后的顺序）。这样，Skill 或约束的放置位置
  决定的只是它在各段 system 文本中的次序，而不是它在对话里的位置。
- 在 `openai.chat` 和 `openai.responses` 上，System 消息留在其放置位置所决定的位置。

想让请求的开头保持稳定，请不要在各轮之间改动 System 角色制品的文本。以上描述的是旧版（Legacy）布局，除非 epoch 要求别的布局，否则使用它（见“稳定优先的布局”）。

## 稳定性类别、顺序与前缀声明

提供方的提示词缓存能否保住请求的开头，取决于各次调用之间哪些制品保持不变。数据模型记录这一点，稳定优先的布局（下一节）使用它。

- 制品可以声明 `StabilityClass`：`Stable`（整个运行期间字节不变）、`Slow`（只在压缩或其他已声明的前缀中断时才变）或 `Volatile`（每次调用都可能变）。未声明的制品在读取时获得类别：`RequiredSkill` 和 `HardConstraint` 为 `Stable`，producer 为 `COMPACTION_PRODUCER_ID` 的 `DerivedContext` 为 `Slow`，其余一律为 `Volatile`。创建渲染为 System 消息的种类的 `Volatile` 制品，会被 `ContextStabilityError` 拒绝。
- 制品可以声明 `AnchorMode`：`Replace`（默认）或 `KeepInPlace`。目前还没有任何代码使用它。
- epoch 可以记录 `ordered_artifact_ids`（制品的固定顺序）和 `prefix_claim`：`Extends`（请求开头与前驱相同，只在末尾增长），或带有 `break_reason` 的 `Breaks`。激活声明 `Extends` 的 epoch 时，会针对当前生效的 epoch，或宿主传给 `activate` 的前驱检查该声明：`raw_from_sequence` 相同，`raw_through_sequence` 更大，`Stable` 和 `Slow` 制品相同且顺序相同，`KeepInPlace` 制品仍被选中且位于 RAW 窗口之内。没有记录顺序的 epoch（例如在该字段出现之前存储的）无法被延续。`Extends` 所承诺的内容取决于布局。在 `Legacy` 下，它表示 `Stable` 和 `Slow` 制品及其记录的顺序没有变、窗口只是增长了；旧版装配器按放置位置、优先级和 producer 排序，不读取记录的顺序，所以并不承诺请求的开头。在 `StableFirst` 下，记录的顺序就是请求的顺序，epoch 的制品若不构成装配器接受的布局，该声明会被拒绝，直到 `Slow` 制品结束为止的请求开头与前驱相同（宿主指令和工具不归 epoch 所有，不在此列）。
- 装配器写出的回执记录按提示词顺序排列的制品、它们的类别（各自标明是制品声明的还是推导的，并附推导规则的版本）、编码器所属的 family、工具的摘要和 epoch 的中断原因。不是 JSON 的请求体是错误，而不是没有工具的请求。已存储的回执，在类别由制品声明之处严格比对，只在规则版本为现行版本时才重新推导类别并比较，所以以后规则改变，已存储的回执仍然可读。

## 稳定优先的布局

epoch 用 `assembly_layout` 选择如何布置制品。`Legacy` 是默认值，也是在该字段出现之前存储的 epoch 唯一可能有的布局，即“制品的放置位置”一节描述的组装方式。`StableFirst` 需要主动选择，需要 `ordered_artifact_ids`，并按下面的顺序构造提示词：

```text
host instructions
Stable artifacts, in the recorded order
Slow artifacts, in the recorded order
RAW messages
Volatile artifacts, in the recorded order
trusted supplemental input
```

- **epoch 的列表就是顺序。** `order_context_artifacts` 根据制品构造这个列表：先按稳定性类别，再按 `priority`（高者在前），再按 `producer_id`，最后按宿主给出的键。内容的哈希不参与，所以文本变化的同级制品保持原位；四项都相同的两个制品会因含混而被拒绝。
- **放置位置服从类别。** `Stable` 和 `Slow` 制品放在 `BeforeHistory`，`Volatile` 制品放在 `AfterHistory`，列表先列 `Stable`，再列 `Slow`，最后列 `Volatile`。其他情况（例如用默认放置位置 `BeforeLatestUser` 创建的制品）不会被挪动，而是被 `ContextStabilityError` 拒绝。
- **System 文本留在前面。** System 消息只能是宿主指令，或 `Stable`、`Slow` 制品。RAW 或补充输入中的 System 消息，在 `anthropic.messages`、`google.generate` 和 `google.interactions` 上会被提升到 system 字段而挪动请求的开头，所以会被拒绝。被提升的文本按 `Stable`、`Slow` 的顺序排列。
- **布局变更是一次中断。** 相邻的两个 epoch 要使用不同的布局，后一个 epoch 必须声明原因为 `LayoutChange` 的 `Breaks`；否则 `verify_context_epoch_claim` 会拒绝，跨越布局变更的 `Extends` 声明同样被拒绝。
- **从请求体读出的两个边界。** 回执记录 `Stable` 部分（宿主指令和 `Stable` 制品）在哪里结束、只追加的部分（`Volatile` 制品之前的全部内容）在哪里结束，以及到后者为止的请求摘要。边界是编码后的请求体中第一个不在前缀里的消息和部分，因此在会提升 System 文本的系列上，`{0, 0}` 表示只包含工具和 system 字段的前缀。位置是 System 文本被提升、工具结果被重新分组之后的请求体中的位置。装配器用只含前缀消息、后面再加一条标记消息的请求，把请求再编码第二次、第三次来找到它们；如果编码出的请求体彼此对不上，就以 `ContextStabilityError` 拒绝该 epoch。当 `Slow` 的 System 文本与 `Stable` 文本共用 system 字段时，该字段是一个整体，因此不记录 `Stable` 边界。摘要涵盖请求体中除流标志和输出上限之外的所有成员，消息在边界处截断。旧版 epoch 没有边界。

## 运行时的开发者指令

`RuntimeDeveloperInstruction` 是不可变的开发者输入，不是权限。
`RuntimeInstructionController::submit_and_plan` 按下面的顺序执行：

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

封闭的决策集合是：

- `SatisfiedInPlace`；
- `Rejected`；
- `ReplaceAtHandoff`；以及
- `MigrateGraph`。

应用一次转换时，在委托给现有的 `ProgramRuntime::replace` 或 `migrate_graph`
路径之前，会立即重新检查谱系 head。过期的决策无法变成权限。

## 有界的 Program 合成

`ProgramSynthesisGateway` 提供由宿主持有的、生成后继者的路径：

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

预留必须表现为恰好扣减一个不可续期的 `max_dynamic_compiles` 单位，并且不得增加任何其他预算。
预留发生在编译之前，所以被拒绝的源码拿不回它的编译额度。语义验证是强制的，
在编译之后、准入解析器之前运行。它不可变的回执，绑定了提案、预留、编译好的 bundle、
验证器标识、语义契约标识、结论和证据摘要。被拒绝的结论会给出带类型的证据，
并且无法发布 `ProgramVersion`。网关从不激活、绑定、迁移或 spawn 它的结果。
这些仍然是宿主通过现有 Program API 做出的独立决定。

运行时的指令规划器可以调用这个网关，然后在替换或迁移的决策里，
返回确切的已准入版本。这样就保持了：

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

而无需把编译器、Catalog、凭据或激活权限暴露给生成出来的 JavaScript。
