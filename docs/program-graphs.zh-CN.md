<!-- neograph-i18n: source=docs/program-graphs.md locale=zh-CN source_sha256=8082d68c638414c879b2854352e5fd4c95bb82f0ac014d1efa11b852de5991e1 -->
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
