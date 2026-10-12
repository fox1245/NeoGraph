<!-- neograph-i18n: source=docs/program-graphs.md locale=zh-CN source_sha256=d826f6691e4419255724c79828e57e5bc1d349a20ee0a5c24d67512366199812 -->
# Program 图功能

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

本页介绍 `define()` 程序除了节点、边、屏障和图级别的重试策略之外，还能使用的图功能。每个功能都是严格 Core 定义（`schema_version` 1）中的一个键，并在 `javascript_authoring_capability_manifest()["features"]` 里有一个对应的标志。没有使用某个功能的定义，其字节、`definition_hash` 和 bundle id 都和该功能出现之前完全一样。

只要定义用到这些键中的任何一个，bundle 就不会封存验证器的警告。编译仍然会返回这些警告；参见[诊断指南](diagnostics.md)。

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

每张图只能调用一次 `.metadata(object)`。Core 定义把它存为顶层的 `"metadata"` 对象，空对象等同于没有。

- **限制。** 规范化 JSON 不超过 8 KiB，`metadata` 之下的嵌套深度为 4，对象成员总数为 64，键的长度为 1 到 128 字节。以 `neograph.` 开头的顶层键是保留的。
- **文本。** 键和字符串不能包含不可见字符：除制表符、换行符和回车符之外的 C0 控制字符；双向控制字符 U+061C、U+200E、U+200F、U+202A 到 U+202E，以及 U+2066 到 U+2069；零宽字符 U+200B 到 U+200D、U+2060 和 U+FEFF；以及标签字符 U+E0000 到 U+E007F。
- **身份。** 元数据是定义的一部分，所以它会改变 `definition_hash` 和 bundle id；键的顺序则不会。
- **读取方。** 只有宿主能读取，且是从封存的定义中读取：C++ 中是 `bundle.sealed_core_definitions()[0].definition["metadata"]`，Python 中是 `bundle.sealed_core_definitions[0]["definition"]["metadata"]`，例如对 `LocalProgramHost.compile()` 返回的 bundle。节点工厂、运行状态、检查点、输出和事件都看不到它。
- **权限。** 元数据记录的是作者的声明，不授予任何权限：委托的限制仍由宿主准入的 `RunBudget`（`max_child_depth`、`max_total_children`、`max_concurrency`）决定。
- **迁移。** 元数据不在 `GraphSemanticMigrationAdapter` 的拓扑形状之内，所以修改它不会阻止迁移。`metadata_changes()` 会为批准迁移的宿主列出新增、删除和变更的指针。

错误：第二次调用，或参数不是对象时，是 `P_JS_GRAPH_ARGUMENT`；另有 `P_CORE_GC_FIELD_TYPE`、`P_CORE_GC_METADATA_LIMIT`、`P_CORE_GC_METADATA_TEXT` 和 `P_CORE_GC_FIELD_VALUE`（键为空或为保留键）。

## 带 any_of 的 join 槽位

标志：`barrier.any_of`。

```js
graph.conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
  .edge("FIX", "D").edge("C", "D")
  .barrier("D", [{any_of: ["B", "FIX"]}, "C"]);
```

`barrier(node, waitFor)` 的每个条目，要么是节点名，要么是槽位 `{any_of: [...]}`。join 对它的各个槽位仍然是 AND 关系：

- 槽位中只要有任何一个备选节点向 join 发出信号，这个槽位就算填满。所有槽位都填满后 join 才运行，之后所有信号清空，与普通屏障一样。
- 槽位之外的节点发来的信号会被记录，但永远不计数。`Command.goto` 和 `Send` 仍然绕过 join，与以前一样。
- 同一个超步里，或者 join 运行之前，有两个备选节点到达，不会产生任何变化。join 运行之后才到达的备选节点，会开启一个永远无法完成的新一轮，运行因此以 `P_CORE_BARRIER_STALLED` 结束（失败时关闭）。所以各备选节点的路由应当互斥；如果其中两个都可能运行，`P_CORE_E17` 会发出警告。
- 在 Core 定义中，槽位写成 `{"any_of": [...]}`，其中的名称按 UTF-8 字节排序，槽位之间按各自最小的名称排序。只有一个名称的槽位直接写成该名称，所以 `[{any_of: ["B"]}, "C"]` 与 `["B", "C"]` 的字节和 id 完全相同。
- 停滞的槽位 join 会在 witness 里列出槽位：`wait_for` 和 `missing` 里既有名称，也有 `{"any_of": [...]}` 对象。
- 验证器会逐个检查槽位：没有任何备选节点路由到 join 时是 `P_CORE_E8`；某个成员可以路由到别处时是 `P_CORE_E15`；路由到同一槽位的另一个备选节点，不算绕过。

错误：条目是其他形状时，是 `P_JS_GRAPH_ARGUMENT`；`any_of` 为空或不是字符串、`any_of` 旁边还有别的键、嵌套的 `any_of`，或者某个节点在槽位内或槽位之间重复出现时，是 `P_CORE_GC_BARRIER`。

## witness 路径与路由表

- **绕过路径。** `P_CORE_E15` 的 witness 带有 `paths`：对于每条绕过路由，给出从该成员出发、经过这条路由的路由序列；如果这条路由又回到了 join，序列就终止于 join。回到 join 时，join 前面的那个节点可以代替该成员：witness 会增加 `substitute` 和 `suggested_wait_for`，返回的诊断则建议用它替换 `wait_for`（适用性 `maybe`）。对于 #357 的形态，建议是 `[{"any_of": ["B", "FIX"]}, "C"]`。已经填充了另一个槽位的节点，永远不会被建议。
- **停滞路径。** 停滞 join 的 witness（`output["_neograph"]["barrier_stalled"]`，在 Program 失败中是 `stalled_barriers`）增加了 `paths`：它根据 trace 和路由后继映射，给出每个已运行但仍缺失的成员通向某个向 join 发过信号的节点的路由；如果存在 `suggested_wait_for`，也会一并给出。
- **路由表。** `ProgramCompilerConfig::echo_routes`（Python：`ProgramCompiler(..., echo_routes=True)`）会返回一条 `P_CORE_ROUTES` 提示，其 `witness.routes` 把每个节点映射到调度器可能路由到的目标：先取它的第一条条件边的路由，没有条件边则取静态边。它不会被封存，所以不论是否开启，bundle 都相同。

## 失败路由

标志：`retry_policy.per_node`、`failure_edges`、`failure_exits` 和 `retry.attempt_timeout`。

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

TEST 会尝试三次。仍然失败时，运行并不会失败：TEST 会路由到 ASK_USER，它带着失败记录暂停，等待人工处理。回答 `retry`，会再次运行 IMPLEMENT 和 TEST；回答 `abort`，会运行 ABORT（一个失败出口），运行以 `plan.user_abort` 作为错误码 `Failed` 结束，绝不会报告 `Completed`。

`retryPolicy(node, {...})` 会展开成两个 Core 键：时间和策略字段写入 `node_retry_policies`，`on_failure` 和 `error_channel` 写入 `failure_edges`。`failureExit(node, {code})` 写入 `failure_exits`。不用的键就不出现，图级别的 `retryPolicy({...})` 不变。

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

### 键

- **节点级策略。** 只存储写出的字段：`max_retries` 0 到 10，`initial_delay_ms` 10 到 60 000，`backoff_multiplier` 1.0 到 10.0，`max_delay_ms` 从 `initial_delay_ms` 到 300 000，`non_retryable`（最多 16 个互不相同的错误码），`max_visits` 1 到 100，`error_text`，以及 `attempt_timeout_ms` 100 到 3 600 000。超出范围的值是 `P_CORE_GC_FIELD_VALUE`，附带 `{min, max, actual}`。
- **错误码。** 作者自定义的错误码要符合 `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`；`NG.` 前缀是保留的。`non_retryable` 也可以列出分类器的错误码（`NG.TaskFailed`、`NG.Timeout`、`NG.SubgraphFailed`，以及参考文档中节点失败契约里的其他错误码）。
- **失败边。** 每个节点最多一条，按 `from` 排序。目标不能是 `__start__`、`__end__`、节点自身、某个 join，也不能是没有边或路由的节点，除非它是失败出口（`P_CORE_GC_FAILURE_EDGE`）。`error_channel` 必须使用 `append` reducer。
- **失败出口。** 错误码默认为 `NG.Aborted`。失败出口节点没有边，也没有路由。
- **验证。** 未知的节点和通道是 `P_CORE_E3`。失败边会加入路由后继映射，所以可达性、终止性和屏障绕过（`P_CORE_E15`，路由为 `on_failure`）的检查都能看到它们，`error_channel` 也算作有写入。节点配置里出现该节点类型没有声明的 `retry_policy` 键，会报告 `P_CORE_E18`。在图级别的 `retryPolicy({...})` 里写 `on_failure`，是 `P_CORE_GC_UNKNOWN_FIELD`，并提示改用节点级的写法。
- **迁移。** `failure_edges` 和 `failure_exits` 属于迁移适配器拓扑形状的一部分；`node_retry_policies` 不属于。

### 运行时

- **重试时间参数** 逐字段解析：先取宿主默认值（`EngineConfig::retry_policy`、`set_retry_policy`），没有就取图的 `retryPolicy`，再没有就取内置默认值；然后再用节点自己的字段覆盖。对于没有节点级策略的节点，宿主的节点级策略（`EngineConfig::node_retry_policies`、`set_node_retry_policy`）会替换上面的结果，这一点与以前相同。对于有节点级策略的节点，宿主只能收紧每个字段：`max_retries` 取较小值，延迟和乘数取较大值；这样的节点重试次数永远不会超过 10 次。`GraphEngine::resolved_retry_policy(node)` 返回最终结果。
- **哪些失败会被重试和路由**，由分类器决定（参考文档中的节点失败契约）。列在 `non_retryable` 中的错误码不会重试。节点内部抛出的 Program 诊断（`ProgramDiagnosticError`，或编译、准入错误）属于 `NG.ConfigError`：既不重试，也不路由。
- **已处理的失败。** 节点在可路由的失败上放弃、且有失败边时，运行继续。该节点失败的各次尝试不写入任何内容；如果它有 `error_channel`，会在该节点所在的超步里向其追加一条记录；节点只路由到失败边的目标，不会向自己的边或路由发出信号。同一超步里的兄弟节点照常应用它们的结果，而该超步中的 `Command.goto` 仍然会抢占所有路由，失败路由也不例外。失败到达不会填充 join 的槽位；运行过的处理器可以代替该成员，如 #357 的形态。该节点在 trace 和 `interruptAfter` 看来算作已运行，它的待定写入记录的是已处理的结果，所以崩溃后的恢复会重放这条路由，而不会再次运行该节点。静态节点、fan-out 分支、一个或多个 `Send` 目标，以及子图节点的行为都一样。不可路由的失败，或没有失败边的节点的失败，仍和以前一样让运行失败。
- **记录** 只包含结构化字段：
  ```json
  {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed", "attempts": 3, "retryable": true,
   "step": 1, "routed_to": "ASK_USER", "effects": []}
  ```
  `kind` 是 `exhausted`、`non_retryable`、`effect_committed`、`ceiling`、`visit_limit` 或 `deadline`（退避等待会越过运行的截止时间）。`effects` 列出最后一次尝试经中介的 Tool 副作用，形式为 `{tool, ordinal, status}`。
- **自由文本** 需要在节点上用 `error_text: true` 显式开启，并且需要宿主提供脱敏函数：`EngineConfig::failure_text_redactor`、`CatalogConfig::failure_text_redactor`，或者 Python 的 `GraphEngine.compile` 和 `LocalProgramHost` 上的 `failure_text_redactor=`。文本的唯一来源是带类型的公开消息（`NodeFailureError` 的 `public_message`，或 Python 异常的 `public_message` 属性），绝不是异常文本。脱敏函数返回要保留的文本；引擎会去掉 C0、双向、零宽和标签字符，在 UTF-8 边界处把文本截断到 512 字节，并存为 `"detail": {"untrusted_text": "..."}`。没有脱敏函数时，不写入任何文本。
- **中断。** 目标节点带有 `interruptBefore` 时，中断值是 `{"reason": "failure_route", "failure": record}`。在 Program 里，它是 `result.interrupt.value` 和 `result.interrupt.pending_input.core_interrupt_value`。
- **事件。** 放弃的节点的 `ERROR` 事件带有 `handled`（失败已被路由时为 true），已路由时还带有 `routed_to`。
- **循环上限：`max_visits`。** 声明了它的节点最多运行这么多次；下一次激活不会运行，而是以 `NG.VisitLimit` 失败（`kind` 为 `visit_limit`，0 次尝试），沿节点的失败边路由，没有失败边则让运行失败。失败边的目标完成时，计数器重置。计数保存在检查点元数据的 `_neograph.node_visits` 里，只针对声明了它的节点。步数预算仍是全局兜底。
- **失败出口。** 失败出口节点完成时，运行在该超步结束后终止，`output["_neograph"]["failure_exit"] = {node, code}`，状态为 `RunStatus::FailureExit`（Python 中是 `RunStatus.FailureExit`；`RunInvocation` 报告为 `Error`）。Program 运行以声明的错误码 `Failed` 结束，`core_node` 为该节点。
- **重试总上限。** `RunBudget::max_node_retries`（Python 中是 `ProgramRunBudget.max_node_retries`）限制整个运行中节点重试的总次数，涵盖每次 Core 调用、节点、分支、`Send` 和子图。达到上限后停止重试，`kind` 为 `ceiling`。已进行的重试次数是 `ProgramUsage::node_retries`，剩余次数是 `remaining_budget.max_node_retries`。未设置表示没有上限，这两个字段只在设置时才序列化。由 `ng.spawn` 启动的子运行继承父运行的上限。在 Program 之外，把 `NodeRetryCeiling` 作为 `RunMetadata::node_retry_ceiling` 传入。
- **Program 中未路由的失败。** 没有失败边的节点失败时，Program 以分类后的错误码（`NG.TaskFailed`、作者自定义的错误码、`NG.ProviderFailed` 等）`Failed` 结束，而不是 `P_RUNTIME_CORE_FAILURE`，`failure.witness.node_failure` 为 `{node, code, kind, attempts, retryable, routable}`。编排计划中的 `retry` 操作用的是同一个分类器：只有在可重试的失败之后，它才会再次运行自己的主体；已提交副作用、不重试的类别、已达到重试上限、因截止时间而停止之后，都不会再运行。

### 尝试超时与截止时间

标志：`retry.attempt_timeout`。

```js
graph.retryPolicy("TEST", {attempt_timeout_ms: 30000, max_retries: 2, on_failure: "ASK_USER"});
```

- **超时的尝试。** `attempt_timeout_ms` 限制节点的每次尝试。超时之后，该次尝试通过它自己的取消令牌被取消（节点的 `ctx.cancel_token`，它是运行令牌的子令牌），并以 `NG.Timeout` 失败；它和普通失败一样可以重试和路由；想在第一次超时就停止，把 `NG.Timeout` 列入 `non_retryable`。以先发生的终止事件为准：某次尝试在超时时刻或之后才完成，就算作已超时，不论定时器是否已经触发。
- **迟到的完成会被丢弃。** 超时的尝试之后做的任何事都不算数：它的写入和结果被丢弃，它发出的事件到不了运行的回调，它新发起的、经中介的 Tool 调用会被拒绝，因为它的令牌已被取消。下一次尝试只会在被取消的那次停止之后才开始，所以同一个节点的两次尝试永远不会重叠。
- **停止期限。** 被取消的尝试，可用于停止的时间和它原来可用于运行的时间一样长，且至少 1 秒。到那时还没有停止的尝试，可能仍在产生副作用：节点以 `NG.EffectUncertain` 失败，这种失败既不重试也不路由，所以运行失败。这次尝试会继续留在后台，并让它的节点一直保持存活，直到它返回；`wait_for_node_attempts()` 会等待这类尝试，Python 绑定在解释器退出时会调用它。尝试在提交了非幂等副作用之后才停止的，仍然以 `NG.EffectCommitted` 失败；留下了经中介的不确定副作用的，则让运行暂停以便对账。
- **所有路径。** 静态节点、fan-out 分支、一个或多个 `Send` 目标，以及子图节点走的是同一条路径；子图节点的子运行会随它的尝试一起被取消。超时尝试之后发生崩溃，会从当时正在运行的那次尝试恢复，恢复时绝不会应用已超时尝试的结果。
- **宿主超时。** `GraphEngine::set_node_attempt_timeout_ms(node, ms)`（Python 中是 `set_node_attempt_timeout_ms`，传 `None` 表示清除），给定义中没有超时的节点设置超时。对于已有超时的节点，宿主只能缩短它：两者中较短的那个生效。`resolved_attempt_timeout_ms(node)` 返回最终结果。
- **节点函数体在哪里运行。** 驱动运行的协程从不运行带超时的函数体；在 Program 运行中，它根本不运行任何节点函数体：这样的函数体运行在进程范围的 worker 线程池上（`NEOGRAPH_NODE_ATTEMPT_THREADS`，1 到 1024，默认 32），所以一个阻塞自己线程的函数体，既不会让它自己的超时失效，也不会拖住 Program 调度器（join 成员和 `await` 定时器共用这个调度器）。这对 C++ 节点和 Core 子图同样适用，对 Python 节点也一样。其他 `GraphEngine` 运行仍在调用线程上运行不带超时的函数体。
- **感知截止时间的退避。** 下一次尝试之前的退避等待如果会越过运行的截止时间（Program 的 `wall_time_ms`；Program 之外是 `RunMetadata::deadline`），节点会立即停止重试，`kind` 为 `deadline`，而不是睡到预算耗尽并终止运行。失败保留它的错误码（`NG.TaskFailed`、`NG.Timeout` 等），沿节点的失败边路由；没有失败边则让运行失败。

## 健全性检查

标志：`diagnostics.soundness_check`。`ProgramCompilerConfig::soundness_check`（Python：`ProgramCompiler(..., soundness_check=True)` 或 `LocalProgramHost(..., soundness_check=True)`）会穷举根图在其路由允许下的每一种运行，并返回发现的问题。它默认关闭，并且和路由表一样不会被封存：不论是否开启，bundle 都相同。直接调用 `GraphValidator` 的 C++ 调用方，设置 `ValidationOptions::soundness_check`。

检查按调度器的方式逐个推进超步。一个状态由就绪节点的集合、每个 join 持有的信号（包括 `any_of` 槽位），以及每个声明了 `max_visits` 的节点的访问计数组成；访问计数与运行时一样，只有在该节点失败边的目标完成时才会重置。条件边可以走它的任何一条路由，带失败边的节点可以改走失败边，失败出口则终止运行。每个发现都是一条警告，其 `witness.trace` 是能复现该问题的最短运行，每个超步一项：运行过的节点（`ready`）和它们走过的路由（`routes`，失败边用 `on_failure` 表示）。

| 代码 | 发现 |
|---|---|
| `P_CORE_S1` | 从某个时刻起，没有任何运行能够完成：每一种后续走向都以屏障停滞结束、在访问上限处失败，或者陷入循环。 |
| `P_CORE_S2` | 运行结束时，某个 join 仍持有信号：屏障停滞，或者通过显式结束丢弃了这些信号。 |
| `P_CORE_S3` | 某个 join 在任何运行中都不会触发。 |
| `P_CORE_S4` | 运行可能进入一个没有出口的循环，最后只能在步数上限处停止。 |
| `P_CORE_S5` | 写入同一个 overwrite 通道的两个节点，可能在同一个超步里运行（`P_CORE_E5` 已经报告的 fan-out 兄弟节点除外）。 |

`P_CORE_SOUNDNESS` 汇总会说明检查进行到了哪一步：穷举了所有运行时是一条提示，没有穷举时是一条警告。对没有探索过的部分，它从不下结论：

- 可能返回 `Command.goto` 或 `Send` 的节点类型，要在它的副作用契约里声明，写成 `"control": ["goto"]` 或 `["send"]`。这类节点就绪之后的运行无法分析：汇总会列出这些节点；`P_CORE_S1` 和 `P_CORE_S4` 只在所有后续走向都已探索的地方才会报告，`P_CORE_S3` 则完全不报告。检查信任这些契约，所以返回了 `Command.goto` 却没有声明的节点类型，会按它声明的路由来分析。
- `soundness_state_cap`（默认 100 000 个状态）限制工作量。检查达到上限时，汇总会写 `not_checked`，并且只返回此前已经发现的 `P_CORE_S2` 和 `P_CORE_S5`。

对于 #357 的形态，若使用 `barrier("D", ["B", "C"])`：

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

`P_CORE_S1` 对同一张图，从 `B` 走了 `fail` 的那一刻起报告问题。改用 `barrier("D", [{any_of: ["B", "FIX"]}, "C"])` 后，检查没有发现任何问题，汇总提示说明它穷举了所有运行。每种 witness 的含义，见[诊断指南](diagnostics.md#soundness-check)。
