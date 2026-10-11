<!-- neograph-i18n: source=docs/program-graphs.md locale=zh-CN source_sha256=d4875bd07b38d778142082c08e06659ddac7ee8fac8a704450b82c343e44d9eb -->
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
