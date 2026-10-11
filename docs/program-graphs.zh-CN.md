<!-- neograph-i18n: source=docs/program-graphs.md locale=zh-CN source_sha256=591a0e3c95e94867174067a6ec417eab92f57dfe3ceb8af5cb4f844c92b127b1 -->
# Program 图功能

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

本页介绍 `define()` 程序在节点、边、barrier 和图级重试策略之外可以使用的图功能。每项功能都是严格 Core 定义（`schema_version` 1）中的一个键，并在 `javascript_authoring_capability_manifest()["features"]` 中有对应标志。不使用某项功能的定义，其字节、`definition_hash` 和包 id 与该功能出现之前完全相同。

使用其中任何一个键的定义不会把校验器警告封存进包中。警告仍会作为编译结果返回；参见[诊断指南](diagnostics.md)。

## 图元数据

标志：`graph.metadata`。

```js
export function define() {
  return ng.graph("plan")
    .metadata({owner: "agent:planner", requirements: ["REQ-12"]})
    .node("draft", {type: "my.task"})
    .entry("draft").exit("draft");
}
```

每个图只能调用一次 `.metadata(object)`。Core 定义将其存为顶层 `"metadata"` 对象，空对象等同于没有元数据。

- **限制。** 规范 JSON 最多 8 KiB，`metadata` 以下嵌套深度 4，对象成员总数 64，键长 1 到 128 字节。以 `neograph.` 开头的顶层键是保留的。
- **文本。** 键和字符串不能包含不可见字符：制表符、换行和回车以外的 C0 控制字符；bidi 控制字符 U+061C、U+200E、U+200F、U+202A 至 U+202E 和 U+2066 至 U+2069；零宽字符 U+200B 至 U+200D、U+2060 和 U+FEFF；以及标签字符 U+E0000 至 U+E007F。
- **标识。** 元数据是定义的一部分，因此会改变 `definition_hash` 和包 id；键的顺序不会。
- **读取方。** 只有宿主从封存的定义中读取它：C++ 中为 `bundle.sealed_core_definitions()[0].definition["metadata"]`，Python 中为 `bundle.sealed_core_definitions[0]["definition"]["metadata"]`，例如在 `LocalProgramHost.compile()` 返回的包上读取。节点工厂、运行状态、检查点、输出和事件都看不到它。
- **权限。** 元数据只记录作者的声明，不授予任何权限：委派上限仍由宿主准入的 `RunBudget`（`max_child_depth`、`max_total_children`、`max_concurrency`）决定。
- **迁移。** 元数据不属于 `GraphSemanticMigrationAdapter` 的拓扑形状，因此修改它不会阻止迁移。`metadata_changes()` 为批准迁移的宿主列出新增、删除和更改的指针。

错误：第二次调用或参数不是对象时为 `P_JS_GRAPH_ARGUMENT`，此外还有 `P_CORE_GC_FIELD_TYPE`、`P_CORE_GC_METADATA_LIMIT`、`P_CORE_GC_METADATA_TEXT` 和 `P_CORE_GC_FIELD_VALUE`（空键或保留键）。

## 使用 any_of 的 join 槽位

标志：`barrier.any_of`。

```js
graph.conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
  .edge("FIX", "D").edge("C", "D")
  .barrier("D", [{any_of: ["B", "FIX"]}, "C"]);
```

`barrier(node, waitFor)` 的每一项是一个节点名，或一个槽位 `{any_of: [...]}`。join 仍是按槽位计算的 AND join：

- 槽位中任一备选项向 join 发出信号即填满该槽位。所有槽位都填满后 join 运行，随后与普通 barrier 一样清空全部信号。
- 槽位之外节点的信号会被记录，但从不计入。`Command.goto` 和 `Send` 仍像以前一样绕过 join。
- 两个备选项在同一 super-step 到达，或在 join 运行前到达，不会改变任何结果。在 join 运行之后才到达的备选项会开启一个永远不会完成的新轮次，因此运行以 `P_CORE_BARRIER_STALLED` 结束（失败即关闭）。所以备选项应在路由上互斥；当其中两个都可能运行时，`P_CORE_E17` 会发出警告。
- 在 Core 定义中，槽位输出为名称按 UTF-8 字节排序的 `{"any_of": [...]}`，槽位按各自最小的名称排序。只有一个名称的槽位输出为该名称本身，因此 `[{any_of: ["B"]}, "C"]` 与 `["B", "C"]` 的字节和 id 相同。
- 停滞的槽位 join 会在其 witness 中列出槽位：`wait_for` 和 `missing` 包含名称和 `{"any_of": [...]}` 对象。
- 校验器逐个槽位检查：没有任何备选项路由到 join 时报告 `P_CORE_E8`，成员可能路由到别处时报告 `P_CORE_E15`；路由到同一槽位的另一个备选项不算绕过。

错误：其他形状的项报告 `P_JS_GRAPH_ARGUMENT`；空的或非字符串的 `any_of`、与 `any_of` 并列的其他键、嵌套的 `any_of`、在槽位内或槽位之间重复的节点报告 `P_CORE_GC_BARRIER`。

## witness 路径与路由表

- **绕过路径。** `P_CORE_E15` 的 witness 带有 `paths`：针对每条绕过路由，给出从成员经过该路由的路由序列；如果它回到 join，则以 join 结束。回到 join 时，join 之前的节点可以替代该成员：witness 会增加 `substitute` 和 `suggested_wait_for`，返回的诊断会建议用它替换 `wait_for`（适用性 `maybe`）。对于 #357 的结构，建议是 `[{"any_of": ["B", "FIX"]}, "C"]`。已经填充其他槽位的节点不会被建议。
- **停滞路径。** 停滞 join 的 witness（`output["_neograph"]["barrier_stalled"]`，以及 Program 失败中的 `stalled_barriers`）会增加 `paths`：对每个已运行的缺失成员，给出它到某个已向 join 发信号的节点的路由，取自执行轨迹和路由后继映射；存在时也会给出同样的 `suggested_wait_for`。
- **路由表。** `ProgramCompilerConfig::echo_routes`（Python：`ProgramCompiler(..., echo_routes=True)`）会返回一个 `P_CORE_ROUTES` 注记，其 `witness.routes` 将每个节点映射到调度器可以路由到的目标：其第一条 conditional edge 的路由，否则为其静态 edge。它从不被封存，因此两种情况下包都相同。

## 失败路由

标志：`retry_policy.per_node`、`failure_edges` 和 `failure_exits`。

```js
ng.graph("fix")
  .channel("failures", {reducer: "append", initial: []})
  .node("IMPLEMENT", {type: "t.task"}).node("TEST", {type: "t.task"})
  .node("ASK_USER", {type: "t.review"}).node("ABORT", {type: "t.task"})
  .entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")
  .conditionalEdge("ASK_USER", "t.decision", {retry: "IMPLEMENT", abort: "ABORT"})
  .retryPolicy("TEST", {max_retries: 2, non_retryable: ["app.compile_error"], max_visits: 3,
                        on_failure: "ASK_USER", error_channel: "failures"})
  .interruptBefore("ASK_USER")
  .failureExit("ABORT", {code: "plan.user_abort"});
```

TEST 会尝试三次。如果仍然失败，运行并不会失败：TEST 被路由到 ASK_USER，ASK_USER 带着失败记录暂停，等待人工回答。回答 `retry` 会再次运行 IMPLEMENT 和 TEST；回答 `abort` 会运行 failure exit 节点 ABORT，因此运行以 `plan.user_abort` 结束为 `Failed`，绝不会报告 `Completed`。

`retryPolicy(node, {...})` 会降级为两个 Core 键：时间和策略字段进入 `node_retry_policies`，`on_failure` 和 `error_channel` 进入 `failure_edges`。`failureExit(node, {code})` 写入 `failure_exits`。每个键在未使用时都不存在，图级的 `retryPolicy({...})` 保持不变。

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

### 键

- **节点级策略。** 只保存写出的字段：`max_retries` 0 到 10，`initial_delay_ms` 10 到 60 000，`backoff_multiplier` 1.0 到 10.0，`max_delay_ms` 从 `initial_delay_ms` 到 300 000，`non_retryable`（最多 16 个不重复的代码），`max_visits` 1 到 100，以及 `error_text`。暂不接受 `attempt_timeout_ms`。超出范围报告带 `{min, max, actual}` 的 `P_CORE_GC_FIELD_VALUE`。
- **代码。** 作者代码须匹配 `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`；`NG.` 前缀是保留的。`non_retryable` 还可以列出分类器的代码（`NG.TaskFailed`、`NG.Timeout`、`NG.SubgraphFailed`，以及参考文档节点失败契约中的其他代码）。
- **failure edge。** 每个节点最多一条，按 `from` 排序。目标不能是 `__start__`、`__end__`、节点自身、join，也不能是既不是 failure exit 又没有 edge 或路由的节点（`P_CORE_GC_FAILURE_EDGE`）。`error_channel` 必须使用 `append` reducer。
- **failure exit。** 代码默认为 `NG.Aborted`。failure exit 节点没有 edge 或路由。
- **校验。** 未知节点和通道报告 `P_CORE_E3`。failure edge 会加入路由后继映射，因此可达性、终止和 barrier 绕过（`P_CORE_E15`，路由 `on_failure`）检查都能看到它们，`error_channel` 也算作已写入。节点类型未声明的节点配置中的 `retry_policy` 键会报告为 `P_CORE_E18`。在图级 `retryPolicy({...})` 中写 `on_failure` 会报告 `P_CORE_GC_UNKNOWN_FIELD`，并提示使用节点级形式。
- **迁移。** `failure_edges` 和 `failure_exits` 属于迁移适配器的拓扑形状；`node_retry_policies` 不属于。

### 运行时

- **重试时间**按字段确定：先取宿主默认值（`EngineConfig::retry_policy`、`set_retry_policy`），没有则取图的 `retryPolicy`，再没有则取内置默认值；然后用节点自己的字段覆盖。宿主的节点级策略（`EngineConfig::node_retry_policies`、`set_node_retry_policy`）对没有节点级策略的节点仍像以前一样替换结果。对有节点级策略的节点，宿主只能收紧每个字段：`max_retries` 取较小值，延迟和倍数取较大值；这样的节点最多重试 10 次。`GraphEngine::resolved_retry_policy(node)` 返回最终结果。
- **哪些失败会被重试和路由**由分类器决定（参考文档的节点失败契约）。`non_retryable` 中列出的代码不会重试。节点内部抛出的 Program 诊断（`ProgramDiagnosticError`，或编译、准入错误）属于 `NG.ConfigError`：从不重试，也从不路由。
- **已处理的失败。** 当节点以可路由的失败放弃且有 failure edge 时，运行会继续。失败的尝试不写入任何内容；如果有 `error_channel`，会在该节点的 super-step 中向其追加一条记录；节点只路由到该边的目标，不向自己的 edge 或路由发送信号。同一 super-step 中的兄弟节点照常应用结果，该 super-step 中的 `Command.goto` 仍然优先于所有路由，包括失败路由。失败到达不会填充 join 槽位；实际运行的处理节点可以代替该成员，如 #357 的形状。该节点在执行轨迹和 `interruptAfter` 中计为已运行，其 pending write 记录了已处理的结果，因此崩溃后恢复会重放路由而不会再次运行该节点。静态节点、扇出分支、一个或多个 `Send` 目标以及子图节点的行为完全相同。不可路由的失败，或没有 failure edge 的节点的失败，仍像以前一样使运行失败。
- **记录**只包含结构化字段：
  ```json
  {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed", "attempts": 3, "retryable": true,
   "step": 1, "routed_to": "ASK_USER", "effects": []}
  ```
  `kind` 是 `exhausted`、`non_retryable`、`effect_committed`、`ceiling`、`visit_limit` 或 `deadline`（为尝试超时保留）之一。`effects` 以 `{tool, ordinal, status}` 列出最后一次尝试经中介的 Tool 效果。
- **自由文本**需要按节点用 `error_text: true` 选择启用，并且需要宿主的 redactor：`EngineConfig::failure_text_redactor`、`CatalogConfig::failure_text_redactor`，或 Python `GraphEngine.compile` 和 `LocalProgramHost` 的 `failure_text_redactor=`。其唯一来源是带类型的公开消息（`NodeFailureError` 的 `public_message`，或 Python 异常的 `public_message` 属性），从不使用异常文本。redactor 返回要保留的文本；引擎去除 C0、双向、零宽和标签字符，在 UTF-8 边界处截断到 512 字节，并保存为 `"detail": {"untrusted_text": "..."}`。没有 redactor 时不写入文本。
- **中断。** 当目标有 `interruptBefore` 时，中断值带有 `{"reason": "failure_route", "failure": record}`。在 Program 中它是 `result.interrupt.value` 和 `result.interrupt.pending_input.core_interrupt_value`。
- **事件。** 放弃的节点的 `ERROR` 事件带有 `handled`（失败被路由时为 true），以及被路由时的 `routed_to`。
- **循环上限：`max_visits`。** 声明了它的节点最多运行这么多次；下一次激活不会运行，而是以 `NG.VisitLimit`（`kind` 为 `visit_limit`，0 次尝试）失败，沿该节点的 failure edge 路由；没有 failure edge 时使运行失败。failure edge 的目标完成时计数器重置。计数器只对声明了它的节点保存在检查点元数据的 `_neograph.node_visits` 中。步数预算仍是全局的最后保障。
- **failure exit。** failure exit 节点完成时，运行在该 super-step 之后结束，带有 `output["_neograph"]["failure_exit"] = {node, code}` 和 `RunStatus::FailureExit`（Python 为 `RunStatus.FailureExit`；`RunInvocation` 报告 `Error`）。Program 运行以声明的代码结束为 `Failed`，`core_node` 为该节点。
- **重试上限。** `RunBudget::max_node_retries`（Python 为 `ProgramRunBudget.max_node_retries`）限制整个运行中的节点重试：所有 Core 调用、节点、分支、`Send` 和子图。达到上限时以 `kind` `ceiling` 停止重试。已用的重试为 `ProgramUsage::node_retries`，剩余的为 `remaining_budget.max_node_retries`。不设置表示没有上限，这两个字段只在设置时才序列化。由 `ng.spawn` 启动的子运行继承父运行的上限。在 Program 之外，把 `NodeRetryCeiling` 作为 `RunMetadata::node_retry_ceiling` 传入。
- **Program 中未被路由的失败。** 没有 failure edge 的节点失败会使 Program 以分类后的代码（`NG.TaskFailed`、作者代码、`NG.ProviderFailed` 等）而不是 `P_RUNTIME_CORE_FAILURE` 结束为 `Failed`，`failure.witness.node_failure` 中保存 `{node, code, kind, attempts, retryable, routable}`。编排计划的 `retry` 操作也询问同一个分类器：只有在可重试的失败之后才再次运行其主体，提交过的效果、不重试的类别或已达到的重试上限之后都不会再次运行。

## 健全性检查

标志：`diagnostics.soundness_check`。`ProgramCompilerConfig::soundness_check`（Python：`ProgramCompiler(..., soundness_check=True)` 或 `LocalProgramHost(..., soundness_check=True)`）会探索根图的路由所允许的每一个运行，并返回它的发现。它默认关闭，并且与路由表一样从不被封存：无论开关与否，包都相同。使用 `GraphValidator` 的 C++ 调用方设置 `ValidationOptions::soundness_check`。

检查按调度器的方式推进 super-step。状态包括就绪节点的集合、每个 join 持有的信号（含 `any_of` 槽位），以及每个带 `max_visits` 的节点的访问次数。conditional edge 可以走它的任意一条路由，带失败 edge 的节点可以改走该 edge，failure exit 会结束运行。每个发现都是一个警告，其 `witness.trace` 是展示该发现的最短运行，每个 super-step 一项：运行过的节点（`ready`）以及它们所走的路由（`routes`，失败 edge 为 `on_failure`）。

| 代码 | 发现 |
|---|---|
| `P_CORE_S1` | 从某一时刻起，没有任何运行能够完成：每个后续运行要么以 barrier-stalled 结束，要么在访问上限处失败，要么循环。 |
| `P_CORE_S2` | 运行结束时某个 join 仍持有信号：以 barrier-stalled 结束，或者显式结束丢弃了这些信号。 |
| `P_CORE_S3` | 某个 join 在任何运行中都不会触发。 |
| `P_CORE_S4` | 运行可能进入一个没有出口的循环，此后只会在 step 上限处停止。 |
| `P_CORE_S5` | 写同一个 overwrite 通道的两个节点可能在同一个 super-step 中运行（`P_CORE_E5` 报告的 fan-out 兄弟之外的情形）。 |

`P_CORE_SOUNDNESS` 摘要说明检查进行到了哪一步：探索了每个运行时为注记，否则为警告。它从不声称未探索的内容：

- 可能返回 `Command.goto` 或 `Send` 的节点类型应在其 effect 契约中声明，即 `"control": ["goto"]` 或 `["send"]`。这类节点就绪之后的运行无法分析：摘要会列出这些节点，`P_CORE_S1` 和 `P_CORE_S4` 只在探索了全部后续运行的地方报告，`P_CORE_S3` 则完全不报告。检查信任这些契约，因此未声明却返回 `Command.goto` 的节点类型会按其声明的路由来分析。
- `soundness_state_cap`（默认 100 000 个状态）限制工作量。检查达到该上限时，摘要为 `not_checked`，只返回在此之前找到的 `P_CORE_S2` 和 `P_CORE_S5` 发现。

对于 `barrier("D", ["B", "C"])` 的 #357 结构：

```python
compiler = ng.ProgramCompiler(registry, soundness_check=True)
bundle = compiler.compile(ng.ProgramSource.from_javascript("plan.js", source))
stall = [d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_S2"][0]
stall["witness"]["trace"]
# [{"ready": ["A"], "routes": {}},
#  {"ready": ["B", "C"], "routes": {"B": "fail"}},
#  {"ready": ["FIX"], "routes": {}}]
stall["witness"]["missing"]             # ["B"]
stall["witness"]["suggested_wait_for"]  # [{"any_of": ["B", "FIX"]}, "C"]
```

`P_CORE_S1` 从 `B` 走了 `fail` 的那一刻起报告同一个图。使用 `barrier("D", [{any_of: ["B", "FIX"]}, "C"])` 时检查什么也找不到，摘要注记会说明它探索了每个运行。[诊断指南](diagnostics.md#soundness-check)说明了每种 witness。
