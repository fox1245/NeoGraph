<!-- neograph-i18n: source=docs/diagnostics.md locale=zh-CN source_sha256=9606d98c852ee9e7129e759405a6d4701e1af8f99d67456b19b185ccb9e1e3e9 -->
# Program 诊断

**Languages:** [English](diagnostics.md) | [한국어](diagnostics.ko.md) | [日本語](diagnostics.ja.md) | [简体中文](diagnostics.zh-CN.md)

Program 编译会返回诊断：失败时为 `ProgramCompileError::diagnostics()`，成功时为 `ProgramBundle::compile_diagnostics()`，在 Python 异常上为 `.diagnostics`。本页说明每个代码的含义和修复方法。每个返回的诊断都通过 `help.doc` 指向本页中的对应条目。

- [help 对象](#the-help-object)
- [Program 文档](#program-document)
- [JavaScript 前端](#javascript-frontend)
- [Core 解析器](#core-parser)
- [Core 校验器](#core-validator)
- [注册表](#registry)
- [运行时与准入](#runtime-and-admission)

<a id="the-help-object"></a>
## help 对象

返回的诊断带有 `help`，工具和模型无需解析消息即可直接处理：

| 字段 | 内容 |
|---|---|
| `doc` | 指向本页中该代码条目的链接，或指向其代码族章节的链接。 |
| `related` | 同一根本原因出现的其他位置，每个都带有一行 `label`。 |
| `suggestions` | 修复方案。每个都有 `message`、`edits` 和 `applicability`。 |
| `candidates` | 与未知名称相差不超过两次编辑的已声明名称，按距离由近到远排列。 |

`edits` 是作用于编译器所解析的 Program 文档的 RFC 6902 JSON Patch 操作（`add`、`replace`、`remove`、`move`）。对于 JavaScript 源代码，该文档是 `define()` 生成的文档，诊断的 span 会定位到构建器调用。`applicability` 取值为 `machine`（按原样应用）、`maybe`（先审阅；默认值）或 `placeholder`（需要填入 `<node>` 之类的值）。

封存到包中的诊断从不携带 `help`，因此包 id 与它无关。一次编译对每个根本原因只返回一个诊断：多个诊断报告同一个未知名称时，只保留第一个，其余的成为它的 `related` 条目。最多返回 20 个诊断，错误排在前面；其余数量由一个 `P_DIAGNOSTICS_TRUNCATED` 注记给出。

消息按照你书写的方式命名：JavaScript 源代码使用 `graph.barrier("D", [...])` 这样的构建器调用，JSON 或 C++ 构建器文档使用 Core 键。

<a id="program-document"></a>
## Program 文档

| 代码 | 含义与修复 |
|---|---|
| <a id="p_schema_version"></a>`P_SCHEMA_VERSION` | Program、契约或内嵌 Core 定义的 schema 版本缺失或不受支持。请使用 witness 中列出的版本。 |
| <a id="p_schema_unknown_field"></a>`P_SCHEMA_UNKNOWN_FIELD` | 封闭的 Program 对象中有未定义的字段。请删除它或使用候选名称。 |
| <a id="p_schema_required"></a>`P_SCHEMA_REQUIRED` | 缺少必需的 Program 字段。请添加。 |
| <a id="p_schema_type"></a>`P_SCHEMA_TYPE` | Program 字段的 JSON 类型错误，或源映射中重复出现同一指针。请使用 witness 中的类型。 |
| <a id="p_schema_operation"></a>`P_SCHEMA_OPERATION` | 该操作需要更新的 Program schema 版本。请提高 `program_schema_version` 或改用其他操作。 |
| <a id="p_contract_schema"></a>`P_CONTRACT_SCHEMA` | 输入或输出契约使用了无效或不受支持的 JSON Schema 关键字。 |
| <a id="p_root_name"></a>`P_ROOT_NAME` | 根名称缺失、为空或与 Core 定义的 `name` 不同。请使二者一致。 |
| <a id="p_root_operation"></a>`P_ROOT_OPERATION` | `call_core` 根中没有 Core 节点。请至少声明一个节点。 |
| <a id="p_plan_operation"></a>`P_PLAN_OPERATION` | 不支持该操作。 |
| <a id="p_plan_core"></a>`P_PLAN_CORE` | 计划调用了已封存根之外的 Core，或者既不调用 Core 也不分派子任务。 |
| <a id="p_plan_bound"></a>`P_PLAN_BOUND` | loop、retry 或 timeout 的上限超出范围。 |
| <a id="p_plan_condition"></a>`P_PLAN_CONDITION` | 计划条件的谓词不是恰好一个，或其路径不是 JSON Pointer。 |
| <a id="p_plan_race_arity"></a>`P_PLAN_RACE_ARITY` | race 的分支数不是两个。 |
| <a id="p_plan_quorum"></a>`P_PLAN_QUORUM` | `min_success` 为零或大于分支数。 |
| <a id="p_plan_spawn_shape"></a>`P_PLAN_SPAWN_SHAPE` | spawn 带有内联主体，或不是 await 的直接主体。 |
| <a id="p_plan_cancel_scope"></a>`P_PLAN_CANCEL_SCOPE` | cancel 只支持 `run` 作用域。 |
| <a id="p_plan_reference"></a>`P_PLAN_REFERENCE` | 已封存的操作引用了不存在的操作。这是编译器缺陷，请报告。 |
| <a id="p_plan_source_pointer"></a>`P_PLAN_SOURCE_POINTER` | 已封存操作的源指针无效。这是编译器缺陷，请报告。 |
| <a id="p_plan_typed"></a>`P_PLAN_TYPED` | 无法将计划封存为带类型的操作。详情见 witness。 |
| <a id="p_parallel_map_source"></a>`P_PARALLEL_MAP_SOURCE` | `item_source` 需要有界的字面量数组或输入制品字段。 |
| <a id="p_parallel_map_binding"></a>`P_PARALLEL_MAP_BINDING` | `parallel_map` 的绑定或端点缺失，或不是 JSON Pointer。 |
| <a id="p_parallel_map_bound"></a>`P_PARALLEL_MAP_BOUND` | `parallel_map` 的上限超出范围，或 `max_in_flight` 大于 `max_items`。 |
| <a id="p_parallel_map_failure"></a>`P_PARALLEL_MAP_FAILURE` | `failure_policy` 必须是 `fail_fast` 或 `collect`。 |
| <a id="p_expand_source"></a>`P_EXPAND_SOURCE` | `proposal_source` 需要内联对象或输入 JSON Pointer。 |
| <a id="p_expand_bound"></a>`P_EXPAND_BOUND` | `expand_task_graph` 的上限超出范围。 |
| <a id="p_expand_failure"></a>`P_EXPAND_FAILURE` | `failure_policy` 必须是 `fail_fast` 或 `collect`。 |
| <a id="p_budget_invalid"></a>`P_BUDGET_INVALID` | 预算记录不完整、重复、上下限颠倒或低于结构性下限。请把九种资源各声明一次。 |
| <a id="p_budget_v1"></a>`P_BUDGET_V1` | Program schema 1 将 `max_program_operations` 固定为恰好 1。 |
| <a id="p_budget_static"></a>`P_BUDGET_STATIC` | 声明的最大值无法覆盖计划必然执行的操作。请调高。 |
| <a id="p_migration_program_json"></a>`P_MIGRATION_PROGRAM_JSON` | 不再接受 Program JSON 操作树。请用 JavaScript 或受信任的 C++ 构建器编写。 |
| <a id="p_compiler_internal"></a>`P_COMPILER_INTERNAL` | 编译器因内部错误以安全方式失败。请附上源代码报告。 |
| <a id="p_diagnostics_truncated"></a>`P_DIAGNOSTICS_TRUNCATED` | 诊断超过 20 个，其余的只在 `witness.omitted` 中计数。请修复显示的诊断后重新编译。 |

<a id="javascript-frontend"></a>
## JavaScript 前端

| 代码 | 含义与修复 |
|---|---|
| <a id="p_js_evaluation"></a>`P_JS_EVALUATION` | 源代码抛出异常或无法解析。span 指向失败的行。 |
| <a id="p_js_timeout"></a>`P_JS_TIMEOUT` | 求值超出了编译时限或预算的 `wall_time_ms`。 |
| <a id="p_js_resource_limit"></a>`P_JS_RESOURCE_LIMIT` | 求值超出了内存或栈的上限。 |
| <a id="p_js_cancelled"></a>`P_JS_CANCELLED` | 求值已被取消。 |
| <a id="p_js_runtime"></a>`P_JS_RUNTIME` | QuickJS 运行时或构建器调用发生内部错误。 |
| <a id="p_js_runtime_identity"></a>`P_JS_RUNTIME_IDENTITY` | 源代码是为与编译器不同的 QuickJS 构建或配置文件创建的。 |
| <a id="p_js_unavailable"></a>`P_JS_UNAVAILABLE` | 此构建没有 QuickJS 前端。 |
| <a id="p_js_ambient_denied"></a>`P_JS_AMBIENT_DENIED` | 源代码使用了配置文件禁止的环境权限（时钟、随机数、eval 等）。 |
| <a id="p_js_define_missing"></a>`P_JS_DEFINE_MISSING` | 模块没有导出 `define()`。 |
| <a id="p_js_define_async"></a>`P_JS_DEFINE_ASYNC` | `define()` 必须是同步函数。 |
| <a id="p_js_define_value"></a>`P_JS_DEFINE_VALUE` | `define()` 必须恰好返回一个打开的图构建器。 |
| <a id="p_js_graph_argument"></a>`P_JS_GRAPH_ARGUMENT` | 构建器方法收到了形状错误的参数、保留或重复的名称，或未声明的节点。消息中会给出方法名。 |
| <a id="p_js_graph_value"></a>`P_JS_GRAPH_VALUE` | 构建器参数不是规范的 JSON 数据（函数、类实例、非有限数值）。 |
| <a id="p_js_graph_limit"></a>`P_JS_GRAPH_LIMIT` | 生成的图超出了字节上限。 |
| <a id="p_js_graph_owner"></a>`P_JS_GRAPH_OWNER` | 在不属于本定义构建器的对象上调用了构建器方法。 |
| <a id="p_js_graph_sealed"></a>`P_JS_GRAPH_SEALED` | 在 `define()` 返回之后调用了构建器方法。 |
| <a id="p_js_graph_entry"></a>`P_JS_GRAPH_ENTRY` | 图中有节点但没有 `.entry(node)`，因此不会运行任何东西。 |
| <a id="p_js_control_character"></a>`P_JS_CONTROL_CHARACTER` | 配置字符串中含有控制字符，常见原因是把正则表达式写成了普通字符串。请使用 `/.../.source` 或 ``String.raw`...` ``。 |
| <a id="p_js_control_main"></a>`P_JS_CONTROL_MAIN` | 导出的 `main` 不是函数。 |
| <a id="p_js_control_async"></a>`P_JS_CONTROL_ASYNC` | `main` 必须是同步生成器。 |
| <a id="p_js_control_value"></a>`P_JS_CONTROL_VALUE` | `main` yield 或返回的值既不是已封存的命令也不是规范 JSON。 |
| <a id="p_js_control_command"></a>`P_JS_CONTROL_COMMAND` | 用无效参数构造了命令。 |
| <a id="p_js_control_finished"></a>`P_JS_CONTROL_FINISHED` | 控制生成器结束后又被恢复。 |
| <a id="p_js_import_dynamic"></a>`P_JS_IMPORT_DYNAMIC` | 不允许动态 `import()` 和 `require()`。 |
| <a id="p_js_import_invalid"></a>`P_JS_IMPORT_INVALID` | import 说明符格式错误。 |
| <a id="p_js_import_unavailable"></a>`P_JS_IMPORT_UNAVAILABLE` | import 不在已封存的模块闭包中。 |
| <a id="p_import_conflict"></a>`P_IMPORT_CONFLICT` | 同一个 import 源以两个不同的内容标识出现。 |
| <a id="p_import_dynamic"></a>`P_IMPORT_DYNAMIC` | 源代码或已封存的模块动态加载了模块。 |
| <a id="p_import_unresolved"></a>`P_IMPORT_UNRESOLVED` | import 没有绑定到经过验证的精确模块回执。请连同模块解析结果一起编译。 |

<a id="core-parser"></a>
## Core 解析器

这些诊断来自 Core 定义解析器；witness 的 `core_code` 中带有 Core 代码。

| 代码 | 含义与修复 |
|---|---|
| <a id="p_core_gc_schema_version"></a>`P_CORE_GC_SCHEMA_VERSION` | Core 的 `schema_version` 不是受支持的非负整数。 |
| <a id="p_core_gc_unknown_field"></a>`P_CORE_GC_UNKNOWN_FIELD` | 严格的 Core 对象中有引擎不读取的键。请删除它、使用候选名称，或把备注移到以 `_` 或 `x-` 开头的注解键。 |
| <a id="p_core_gc_field_type"></a>`P_CORE_GC_FIELD_TYPE` | Core 字段的 JSON 类型错误。 |
| <a id="p_core_gc_field_value"></a>`P_CORE_GC_FIELD_VALUE` | Core 字段的值无效：空名称、超出范围的数字或未知选项。 |
| <a id="p_core_gc_field_required"></a>`P_CORE_GC_FIELD_REQUIRED` | 缺少 `type`、`from` 或 `to` 等必需的 Core 字段。 |
| <a id="p_core_gc_edge"></a>`P_CORE_GC_EDGE` | edge 或 conditional edge 不是对象，或其 routes 不是对象。 |
| <a id="p_core_gc_barrier"></a>`P_CORE_GC_BARRIER` | barrier 的 `wait_for` 缺失、为空或格式错误。 |
| <a id="p_core_gc_node_config"></a>`P_CORE_GC_NODE_CONFIG` | 节点配置不符合其类型 schema 的 `required`、`type` 或 `enum` 规则。 |
| <a id="p_core_gc_reserved_node"></a>`P_CORE_GC_RESERVED_NODE` | `__start__` 和 `__end__` 是保留名称，不能用作节点名。 |
| <a id="p_core_gc_no_entry"></a>`P_CORE_GC_NO_ENTRY` | 没有从 `__start__` 出发的 edge，因此不会运行任何东西。请添加 `.entry(node)`。 |
| <a id="p_core_gc_registry_miss"></a>`P_CORE_GC_REGISTRY_MISS` | 注册表快照中缺少节点类型、reducer 或 condition。请注册它或使用候选名称。 |
| <a id="p_core_roundtrip"></a>`P_CORE_ROUNDTRIP` | 解析后的图无法原样重新输出定义：有内容被丢弃或被重新连接。这是编译器缺陷，请报告。 |

<a id="core-validator"></a>
## Core 校验器

这些诊断分析图的含义。警告只会被返回；包会封存它首次封存时所用规则产生的警告。

| 代码 | 含义与修复 |
|---|---|
| <a id="p_core_e3"></a>`P_CORE_E3` | edge、route、interrupt 或 barrier 引用了未知节点，或 barrier 在等待它自己。请使用候选名称。 |
| <a id="p_core_e4"></a>`P_CORE_E4` | 节点的 effect 契约写入（错误）或读取（警告）了未声明的通道。请声明该通道。 |
| <a id="p_core_e5"></a>`P_CORE_E5` | fan-out 的兄弟节点写入同一个 overwrite 通道，最后写入者不确定地获胜。请使用 append reducer。 |
| <a id="p_core_e6"></a>`P_CORE_E6` | 通道从未被读写、只写不读，或只读却没有初始值。 |
| <a id="p_core_e7"></a>`P_CORE_E7` | 有节点无法通过 edge 和 route 从 `__start__` 到达。 |
| <a id="p_core_e8"></a>`P_CORE_E8` | barrier 等待一个从不路由到它的节点，因此 join 永远不会触发。 |
| <a id="p_core_e9"></a>`P_CORE_E9` | 节点有多条普通入边却没有 barrier，每个到达的 super-step 都会运行一次。若是 AND join 请添加 barrier，若是 XOR 合并则可忽略。 |
| <a id="p_core_e10"></a>`P_CORE_E10` | conditional edge 没有 route、路由了其 condition 从不返回的标签，或留下了未路由的标签。 |
| <a id="p_core_e11"></a>`P_CORE_E11` | 有节点被困在没有通往 `__end__` 路径的环中。 |
| <a id="p_core_e12"></a>`P_CORE_E12` | 本地注册表缺少 reducer、节点类型或 condition。 |
| <a id="p_core_e13"></a>`P_CORE_E13` | 一个通过 conditional edge 路由的节点上还有静态 edge，该 edge 永远不会触发。请删除它或把它改为一条 route。 |
| <a id="p_core_e14"></a>`P_CORE_E14` | 节点有第二条 conditional edge，只有第一条会路由。请合并它们。 |
| <a id="p_core_e15"></a>`P_CORE_E15` | barrier 成员可能路由到其 join 之外的地方；一旦如此，运行会以 barrier-stalled 结束。 |
| <a id="p_core_e16"></a>`P_CORE_E16` | 图中有环，但本次编译要求无环图。 |

<a id="registry"></a>
## 注册表

| 代码 | 含义与修复 |
|---|---|
| <a id="p_registry_missing"></a>`P_REGISTRY_MISSING` | 注册表快照中没有被引用的可执行单元。请注册它或使用候选名称。 |
| <a id="p_registry_dependency_missing"></a>`P_REGISTRY_DEPENDENCY_MISSING` | 可执行单元需要注册表快照中缺少的依赖。 |
| <a id="p_registry_identity_mismatch"></a>`P_REGISTRY_IDENTITY_MISMATCH` | 两个引用解析到同一可执行单元的不同精确标识。 |
| <a id="p_registry_requirement_kind"></a>`P_REGISTRY_REQUIREMENT_KIND` | 节点的需求解析器返回了不受支持的可执行单元种类。 |
| <a id="p_registry_requirement_resolver"></a>`P_REGISTRY_REQUIREMENT_RESOLVER` | 节点的需求解析器抛出了异常。 |

<a id="runtime-and-admission"></a>
## 运行时与准入

准入失败和运行失败会以 `ng.ProgramAdmissionError` 和 `ng.ProgramDiagnosticError` 传给 Python，运行结果中带有含代码的 `failure`。若代码没有专门条目，其 `help.doc` 指向本节。

| 代码 | 含义与修复 |
|---|---|
| <a id="p_core_barrier_stalled"></a>`P_CORE_BARRIER_STALLED` | Core 运行停止时，join 仍在等待再也不会到达的信号。witness 为每个停滞的 join 列出 `received` 和 `missing`。 |
