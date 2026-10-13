<!-- neograph-i18n: source=docs/diagnostics.md locale=zh-CN source_sha256=05a405c99f388774993482b2cb175a7c22343d727442659756368bae50402791 -->
# Program 诊断

**Languages:** [English](diagnostics.md) | [한국어](diagnostics.ko.md) | [日本語](diagnostics.ja.md) | [简体中文](diagnostics.zh-CN.md)

Program 编译会返回诊断信息：编译失败时通过 `ProgramCompileError::diagnostics()` 获取，成功时通过 `ProgramBundle::compile_diagnostics()` 获取，Python 异常上则是 `.diagnostics`。本页逐个说明这些代码的含义和修复方法。每条返回的诊断都通过 `help.doc` 链接到本页中对应的条目。

- [help 对象](#the-help-object)
- [Program 文档](#program-document)
- [JavaScript 前端](#javascript-frontend)
- [Core 解析器](#core-parser)
- [Core 校验器](#core-validator)
- [健全性检查](#soundness-check)
- [注册表](#registry)
- [运行时与准入](#runtime-and-admission)

<a id="the-help-object"></a>
## help 对象

返回的诊断带有 `help`，工具和模型无需解析消息文本，就可以直接据此处理：

| 字段 | 内容 |
|---|---|
| `doc` | 指向本页中该代码条目的链接；没有单独条目时，指向它所属代码族的小节。 |
| `related` | 同一个根本原因涉及的其他位置，每个位置带一行 `label`。 |
| `suggestions` | 修复建议。每条建议包含 `message`、`edits` 和 `applicability`。 |
| `candidates` | 与未知名称相差不超过两次编辑的已声明名称，由近到远排列。 |

`edits` 是作用在编译器所解析的 Program 文档上的 RFC 6902 JSON Patch 操作（`add`、`replace`、`remove`、`move`）。对于 JavaScript 源码，这份文档是 `define()` 生成的，诊断的 span 会定位到对应的构建器调用。`applicability` 有三种取值：`machine`（可原样应用）、`maybe`（先审阅再应用；默认值）、`placeholder`（需要填入 `<node>` 之类的值）。

封存进 bundle 的诊断从不带有 `help`，所以 bundle id 与它无关。一次编译对每个根本原因只返回一条诊断：多条诊断报告的是同一个未知名称时，只保留第一条，其余的变成它的 `related` 条目。最多返回 20 条诊断，错误排在前面；剩下的数量由一条 `P_DIAGNOSTICS_TRUNCATED` 提示给出。

消息里使用的是作者实际写下的名称：JavaScript 源码用 `graph.barrier("D", [...])` 这样的构建器调用，JSON 或 C++ 构建器文档则用 Core 的键。

<a id="program-document"></a>
## Program 文档

| 代码 | 含义与修复 |
|---|---|
| <a id="p_schema_version"></a>`P_SCHEMA_VERSION` | Program、契约或内嵌 Core 定义的 schema 版本缺失或不受支持。请使用编译器在 witness 中列出的版本。 |
| <a id="p_schema_unknown_field"></a>`P_SCHEMA_UNKNOWN_FIELD` | 封闭的 Program 对象里有它没有定义的字段。请删除该字段，或改用候选名称。 |
| <a id="p_schema_required"></a>`P_SCHEMA_REQUIRED` | 缺少必需的 Program 字段。请补上。 |
| <a id="p_schema_type"></a>`P_SCHEMA_TYPE` | Program 字段的 JSON 类型错误，或者源码映射（source map）中重复出现同一个指针。请使用 witness 中给出的类型。 |
| <a id="p_schema_operation"></a>`P_SCHEMA_OPERATION` | 该操作需要更新的 Program schema 版本。请调高 `program_schema_version`，或改用其他操作。 |
| <a id="p_contract_schema"></a>`P_CONTRACT_SCHEMA` | 输入或输出契约使用了无效的或不受支持的 JSON Schema 关键字。 |
| <a id="p_root_name"></a>`P_ROOT_NAME` | 根名称缺失、为空，或与 Core 定义的 `name` 不一致。请使两者相同。 |
| <a id="p_root_operation"></a>`P_ROOT_OPERATION` | `call_core` 根里没有 Core 节点。请至少声明一个节点。 |
| <a id="p_plan_operation"></a>`P_PLAN_OPERATION` | 不支持该操作。 |
| <a id="p_plan_core"></a>`P_PLAN_CORE` | 计划调用了封存根之外的 Core，或者既不调用任何 Core，也不分发任何子 Program。 |
| <a id="p_plan_bound"></a>`P_PLAN_BOUND` | 循环、重试或超时的上限超出了允许范围。 |
| <a id="p_plan_condition"></a>`P_PLAN_CONDITION` | 计划条件不是恰好有一个谓词，或者它的路径不是 JSON Pointer。 |
| <a id="p_plan_race_arity"></a>`P_PLAN_RACE_ARITY` | race 的分支数不是两个。 |
| <a id="p_plan_quorum"></a>`P_PLAN_QUORUM` | `min_success` 为零，或大于分支数。 |
| <a id="p_plan_spawn_shape"></a>`P_PLAN_SPAWN_SHAPE` | spawn 带有内联的函数体，或者不是 await 的直接函数体。 |
| <a id="p_plan_cancel_scope"></a>`P_PLAN_CANCEL_SCOPE` | cancel 只支持 `run` 作用域。 |
| <a id="p_plan_reference"></a>`P_PLAN_REFERENCE` | 封存的操作引用了不存在的操作。这是编译器的缺陷，请报告。 |
| <a id="p_plan_source_pointer"></a>`P_PLAN_SOURCE_POINTER` | 封存的操作带有无效的源码指针。这是编译器的缺陷，请报告。 |
| <a id="p_plan_typed"></a>`P_PLAN_TYPED` | 计划无法封存为带类型的操作。详细信息见 witness。 |
| <a id="p_parallel_map_source"></a>`P_PARALLEL_MAP_SOURCE` | `item_source` 必须是有界的字面量数组，或者是输入制品中的字段。 |
| <a id="p_parallel_map_binding"></a>`P_PARALLEL_MAP_BINDING` | `parallel_map` 的绑定或端点缺失，或者不是 JSON Pointer。 |
| <a id="p_parallel_map_bound"></a>`P_PARALLEL_MAP_BOUND` | `parallel_map` 的某个上限超出范围，或者 `max_in_flight` 大于 `max_items`。 |
| <a id="p_parallel_map_failure"></a>`P_PARALLEL_MAP_FAILURE` | `failure_policy` 必须是 `fail_fast` 或 `collect`。 |
| <a id="p_expand_source"></a>`P_EXPAND_SOURCE` | `proposal_source` 必须是内联对象或输入的 JSON Pointer。 |
| <a id="p_expand_bound"></a>`P_EXPAND_BOUND` | `expand_task_graph` 的某个上限超出范围。 |
| <a id="p_expand_failure"></a>`P_EXPAND_FAILURE` | `failure_policy` 必须是 `fail_fast` 或 `collect`。 |
| <a id="p_budget_invalid"></a>`P_BUDGET_INVALID` | 预算记录不完整、重复、上下限颠倒，或低于结构性下限。请把九项资源各声明一次。 |
| <a id="p_budget_v1"></a>`P_BUDGET_V1` | Program schema 1 把 `max_program_operations` 固定为恰好 1。 |
| <a id="p_budget_static"></a>`P_BUDGET_STATIC` | 声明的最大值不足以覆盖计划必然会执行的操作。请调高。 |
| <a id="p_migration_program_json"></a>`P_MIGRATION_PROGRAM_JSON` | 不再接受 Program JSON 操作树。请用 JavaScript 或受信任的 C++ 构建器来编写。 |
| <a id="p_compiler_internal"></a>`P_COMPILER_INTERNAL` | 编译器遇到内部错误，已按失败时关闭处理。请连同源码一起报告。 |
| <a id="p_diagnostics_truncated"></a>`P_DIAGNOSTICS_TRUNCATED` | 发现的诊断超过 20 条；`witness.omitted` 给出其余的数量。请先修复已显示的，再重新编译。 |

<a id="javascript-frontend"></a>
## JavaScript 前端

| 代码 | 含义与修复 |
|---|---|
| <a id="p_js_evaluation"></a>`P_JS_EVALUATION` | 源码抛出了异常，或无法解析。span 指向出错的那一行。 |
| <a id="p_js_timeout"></a>`P_JS_TIMEOUT` | 求值超过了编译时限，或超过了预算的 `wall_time_ms`。 |
| <a id="p_js_resource_limit"></a>`P_JS_RESOURCE_LIMIT` | 求值超出了内存或栈的限制。 |
| <a id="p_js_cancelled"></a>`P_JS_CANCELLED` | 求值被取消。 |
| <a id="p_js_runtime"></a>`P_JS_RUNTIME` | QuickJS 运行时或某个构建器调用在内部失败。 |
| <a id="p_js_runtime_identity"></a>`P_JS_RUNTIME_IDENTITY` | 源码是为另一个 QuickJS 构建或 profile 生成的，与编译器使用的不一致。 |
| <a id="p_js_unavailable"></a>`P_JS_UNAVAILABLE` | 此构建不含 QuickJS 前端。 |
| <a id="p_js_ambient_denied"></a>`P_JS_AMBIENT_DENIED` | 源码使用了 profile 禁止的环境权限（时钟、随机数、eval 等）。 |
| <a id="p_js_define_missing"></a>`P_JS_DEFINE_MISSING` | 模块没有导出 `define()`。 |
| <a id="p_js_define_async"></a>`P_JS_DEFINE_ASYNC` | `define()` 必须是同步的。 |
| <a id="p_js_define_value"></a>`P_JS_DEFINE_VALUE` | `define()` 必须恰好返回一个开放的图构建器。 |
| <a id="p_js_graph_argument"></a>`P_JS_GRAPH_ARGUMENT` | 构建器方法收到的参数形状不对、名称是保留名或重复，或者引用了未声明的节点。消息里会指明是哪个方法。 |
| <a id="p_js_graph_value"></a>`P_JS_GRAPH_VALUE` | 构建器的参数不是规范的 JSON 数据（函数、类实例、非有限数等）。 |
| <a id="p_js_graph_limit"></a>`P_JS_GRAPH_LIMIT` | 生成的图超出了字节数限制。 |
| <a id="p_js_graph_owner"></a>`P_JS_GRAPH_OWNER` | 在一个并非本定义的构建器的对象上调用了构建器方法。 |
| <a id="p_js_graph_sealed"></a>`P_JS_GRAPH_SEALED` | 在 `define()` 返回之后调用了构建器方法。 |
| <a id="p_js_graph_entry"></a>`P_JS_GRAPH_ENTRY` | 图里有节点却没有 `.entry(node)`，所以什么都不会运行。 |
| <a id="p_js_control_character"></a>`P_JS_CONTROL_CHARACTER` | 配置字符串中含有控制字符，常见情况是把正则表达式写成了普通字符串。请改用 `/.../.source` 或 ``String.raw`...` ``。 |
| <a id="p_js_control_main"></a>`P_JS_CONTROL_MAIN` | 导出的 `main` 不是函数。 |
| <a id="p_js_control_async"></a>`P_JS_CONTROL_ASYNC` | `main` 必须是同步生成器。 |
| <a id="p_js_control_value"></a>`P_JS_CONTROL_VALUE` | `main` yield 或返回的值，既不是封存的命令，也不是规范的 JSON。 |
| <a id="p_js_control_command"></a>`P_JS_CONTROL_COMMAND` | 构建命令时参数无效。 |
| <a id="p_js_control_finished"></a>`P_JS_CONTROL_FINISHED` | 控制生成器在已经结束之后又被恢复。 |
| <a id="p_js_import_dynamic"></a>`P_JS_IMPORT_DYNAMIC` | 不允许动态 `import()` 和 `require()`。 |
| <a id="p_js_import_invalid"></a>`P_JS_IMPORT_INVALID` | import 说明符格式错误。 |
| <a id="p_js_import_unavailable"></a>`P_JS_IMPORT_UNAVAILABLE` | import 的模块不在已封存的模块闭包内。 |
| <a id="p_import_conflict"></a>`P_IMPORT_CONFLICT` | 同一个 import 来源出现了两种内容标识。 |
| <a id="p_import_dynamic"></a>`P_IMPORT_DYNAMIC` | 源码或某个已封存的模块动态加载了模块。 |
| <a id="p_import_unresolved"></a>`P_IMPORT_UNRESOLVED` | import 没有绑定到经过验证的、确切的模块回执。请带上模块解析结果进行编译。 |

<a id="core-parser"></a>
## Core 解析器

这些诊断来自 Core 定义的解析器；witness 里的 `core_code` 给出对应的 Core 错误码。

| 代码 | 含义与修复 |
|---|---|
| <a id="p_core_gc_schema_version"></a>`P_CORE_GC_SCHEMA_VERSION` | Core 的 `schema_version` 不是受支持的非负整数。 |
| <a id="p_core_gc_unknown_field"></a>`P_CORE_GC_UNKNOWN_FIELD` | 严格的 Core 对象里有引擎不读取的键。请删除它，改用候选名称，或把注释移到以 `_` 或 `x-` 开头的注解键下。 |
| <a id="p_core_gc_field_type"></a>`P_CORE_GC_FIELD_TYPE` | Core 字段的 JSON 类型错误。 |
| <a id="p_core_gc_field_value"></a>`P_CORE_GC_FIELD_VALUE` | Core 字段的值无效：名称为空、数值超出范围，或选项未知。 |
| <a id="p_core_gc_field_required"></a>`P_CORE_GC_FIELD_REQUIRED` | 缺少必需的 Core 字段，例如 `type`、`from` 或 `to`。 |
| <a id="p_core_gc_edge"></a>`P_CORE_GC_EDGE` | 边或条件边不是对象，或者它的路由不是对象。 |
| <a id="p_core_gc_barrier"></a>`P_CORE_GC_BARRIER` | 屏障的 `wait_for` 缺失、为空或格式错误。 |
| <a id="p_core_gc_node_config"></a>`P_CORE_GC_NODE_CONFIG` | 节点的配置不符合其类型 schema 中的 `required`、`type` 或 `enum` 规则。 |
| <a id="p_core_gc_reserved_node"></a>`P_CORE_GC_RESERVED_NODE` | `__start__` 和 `__end__` 是保留名称，不能用作节点名。 |
| <a id="p_core_gc_no_entry"></a>`P_CORE_GC_NO_ENTRY` | 没有任何边从 `__start__` 出发，所以什么都不会运行。请添加 `.entry(node)`。 |
| <a id="p_core_gc_registry_miss"></a>`P_CORE_GC_REGISTRY_MISS` | 注册表快照中缺少某个节点类型、reducer 或条件。请注册它，或改用候选名称。 |
| <a id="p_core_gc_metadata_limit"></a>`P_CORE_GC_METADATA_LIMIT` | 图元数据超出了限制：规范化 JSON 的 8 KiB、嵌套深度 4、对象成员总数 64，或 128 字节的键。witness 给出 `kind`、`limit` 和 `actual`。 |
| <a id="p_core_gc_metadata_text"></a>`P_CORE_GC_METADATA_TEXT` | 元数据的键或字符串中含有不可见字符（除制表符、换行符、回车符之外的 C0 控制字符，双向控制字符，零宽字符或标签字符），或者含有格式错误的 UTF-8。witness 列出这些码点。 |
| <a id="p_core_gc_failure_edge"></a>`P_CORE_GC_FAILURE_EDGE` | 失败边违反了某条目标规则。`witness.reason` 为：`sentinel_source`；`duplicate_from`（每个节点只能有一条失败边）；`sentinel_target`（应改用失败出口来结束运行）；`self_target`；`join_target`（失败到达永远不会填充 join 槽位）；`implicit_end_target`（给目标节点加一条边或路由，或者把它改成失败出口）；或 `error_channel_reducer`（错误通道必须使用 `append` reducer）。 |
| <a id="p_core_roundtrip"></a>`P_CORE_ROUNDTRIP` | 解析后的图无法重新生成原定义：有内容被丢弃或被改接。这是编译器的缺陷，请报告。 |

<a id="core-validator"></a>
## Core 校验器

这些检查分析的是图的含义。警告只随编译返回；bundle 封存的是它最初封存时所用规则对应的警告。

| 代码 | 含义与修复 |
|---|---|
| <a id="p_core_e3"></a>`P_CORE_E3` | 边、路由、中断或屏障引用了未知的节点，或者屏障在等待自己。请使用候选名称。 |
| <a id="p_core_e4"></a>`P_CORE_E4` | 节点的副作用契约写入（错误）或读取（警告）了未声明的通道。请声明该通道。 |
| <a id="p_core_e5"></a>`P_CORE_E5` | fan-out 的兄弟节点写入同一个 overwrite 通道；最后写入者胜出，结果不确定。请使用 append reducer。 |
| <a id="p_core_e6"></a>`P_CORE_E6` | 某个通道从未被读取或写入、只写不读，或者只读不写且没有初始值。 |
| <a id="p_core_e7"></a>`P_CORE_E7` | 从 `__start__` 出发，沿边和路由无法到达某些节点。只由 `Send` 或 `Command.goto` 启动的节点也会列出，因为图定义里没有声明动态目标的地方，此时这条警告是预期内的。不要为了消除它而加边，否则该节点会多运行一次。 |
| <a id="p_core_e8"></a>`P_CORE_E8` | 屏障等待的某个节点没有路由到它，或者某个 `any_of` 槽位的备选节点都没有路由到它，所以 join 永远不会触发。 |
| <a id="p_core_e9"></a>`P_CORE_E9` | 节点有多条普通入边却没有屏障；每个到达的超步它都会运行一次。要实现 AND join，请添加屏障；如果是 XOR 汇合，可以忽略这条警告。 |
| <a id="p_core_e10"></a>`P_CORE_E10` | 条件边没有路由，路由了条件永远不会返回的标签，或者有标签没有被路由。 |
| <a id="p_core_e11"></a>`P_CORE_E11` | 有节点困在循环里，没有通往 `__end__` 的路径。 |
| <a id="p_core_e12"></a>`P_CORE_E12` | 本地注册表缺少某个 reducer、节点类型或条件。 |
| <a id="p_core_e13"></a>`P_CORE_E13` | 静态边从一个通过条件边路由的节点出发；它永远不会触发。请删除它，或把它加为一条路由。 |
| <a id="p_core_e14"></a>`P_CORE_E14` | 节点有第二条条件边；只有第一条会参与路由。请合并它们。 |
| <a id="p_core_e15"></a>`P_CORE_E15` | 屏障的成员可能路由到 join 之外的地方；一旦发生，运行会以屏障停滞结束。路由到同一个 `any_of` 槽位的另一个备选节点，不算绕过。`witness.paths` 给出每条绕过路由；某条路由回到 join 时，`suggested_wait_for` 和返回的建议会让 join 前面的那个节点代替该成员。 |
| <a id="p_core_e16"></a>`P_CORE_E16` | 图中有环，但这次编译要求图是无环的。 |
| <a id="p_core_e17"></a>`P_CORE_E17` | 同一个 `any_of` 槽位的两个备选节点可能都会运行：fan-out 在不经过 join 的情况下到达了两者，或者一个备选节点既路由到 join 又路由到另一个备选节点。第二个到达者会开启一个永远无法完成的新一轮，运行以屏障停滞结束。请让各备选节点的路由互斥，例如通过条件边。witness 给出 fan-out 节点和两条路径。 |
| <a id="p_core_e18"></a>`P_CORE_E18` | 节点配置里有该节点类型没有声明的 `retry_policy` 键，所以没有任何东西会读取它。请用 `graph.retryPolicy(node, {...})` 设置节点的策略（Core 中是 `node_retry_policies`）。 |
| <a id="p_core_routes"></a>`P_CORE_ROUTES` | 设置 `ProgramCompilerConfig::echo_routes`（Python：`echo_routes=True`）时返回的提示。`witness.routes` 把每个节点以及 `__start__` 映射到调度器可能路由到的目标。它不会被封存。 |

<a id="soundness-check"></a>
## 健全性检查

这些诊断来自需要主动开启的健全性检查，即 `ProgramCompilerConfig::soundness_check`（Python：`soundness_check=True`）；参见 [Program 图功能](program-graphs.md#soundness-check)。它们只会返回，不会被封存。`witness.trace` 列出能复现某个发现的最短运行所经过的超步：`ready` 是运行过的节点，`routes` 是每个有多种去向的节点所选的路由（失败边用 `on_failure` 表示，`max_visits` 强制改道时用 `visit_limit`）。`witness.choices` 列出这些路由选择，并附上声明每条路由的边的 Core 指针，`help.related` 指向它们。

| 代码 | 含义与修复 |
|---|---|
| <a id="p_core_s1"></a>`P_CORE_S1` | 可完成性（option to complete）：从轨迹所到达的状态（`witness.state`）起，没有任何运行能够完成。每一种后续走向，要么在 `stalls_at` 中的某个 join 处屏障停滞，要么在访问上限处失败，要么陷入循环；`ends` 说明是哪一种。请让 join 等待每条路由都能提供的东西，例如使用 `any_of` 槽位，或者把该分支路由回 join。 |
| <a id="p_core_s2"></a>`P_CORE_S2` | 正确完成（proper completion）：运行结束时某个 join 仍持有信号。`kind` 为 `stalled`（运行以 `P_CORE_BARRIER_STALLED` 失败）或 `completed`（显式结束丢弃了这些信号，运行报告为已完成）。witness 给出 `received` 和 `missing`。当停滞只缺一个槽位，且槽位之外恰好有一个节点发过信号时，`suggested_wait_for` 会让该节点填充这个槽位，返回的建议会应用这一改动。 |
| <a id="p_core_s3"></a>`P_CORE_S3` | 某个 join 在任何运行中都不会触发。`kind` 为 `dead`（没有任何东西向它发信号）或 `unfireable`（有信号到达，但永远无法同时填满所有槽位）；`slots_never_filled` 列出没有任何运行向其发信号的槽位。只有穷举了所有运行时才会报告。 |
| <a id="p_core_s4"></a>`P_CORE_S4` | 运行可能进入一个永远出不来的循环，因此只会在步数上限处停止。`nodes` 列出这个循环，`trace` 通向它，`cycle` 绕它一圈。请给循环中的某个条件节点增加一条走出循环的路由；返回的建议是占位符。 |
| <a id="p_core_s5"></a>`P_CORE_S5` | 写入同一个 overwrite 通道的两个节点可能在同一个超步里运行（`P_CORE_E5` 已报告的 fan-out 兄弟节点除外），最后写入者胜出，结果不确定。请使用 append reducer（返回的建议），或者让这些写入方串行执行。只有当每个节点类型都有副作用契约时才会报告。 |
| <a id="p_core_soundness"></a>`P_CORE_SOUNDNESS` | 汇总。`status` 为 `checked`（一条提示）表示穷举了所有运行，否则为 `partial` 或 `not_checked`（一条警告）。witness 给出 `states`、`transitions`、`state_cap`、已探索的运行所到达的 `outcomes`，以及按错误码分类的 `findings`。`partial` 会在 `not_analyzable` 中列出其类型声明了 `Command.goto` 或 `Send` 的节点，以及到达每个节点的轨迹；它们之后的运行没有被探索。`not_checked` 表示已达到 `soundness_state_cap`。 |

<a id="registry"></a>
## 注册表

| 代码 | 含义与修复 |
|---|---|
| <a id="p_registry_missing"></a>`P_REGISTRY_MISSING` | 引用的可执行对象不在注册表快照里。请注册它，或改用候选名称。 |
| <a id="p_registry_dependency_missing"></a>`P_REGISTRY_DEPENDENCY_MISSING` | 某个可执行对象需要一个注册表快照里没有的依赖。 |
| <a id="p_registry_identity_mismatch"></a>`P_REGISTRY_IDENTITY_MISMATCH` | 两个引用解析成了同一个可执行对象的不同确切身份。 |
| <a id="p_registry_requirement_kind"></a>`P_REGISTRY_REQUIREMENT_KIND` | 节点的需求解析器返回了不支持的可执行对象种类。 |
| <a id="p_registry_requirement_resolver"></a>`P_REGISTRY_REQUIREMENT_RESOLVER` | 节点的需求解析器抛出了异常。 |

<a id="runtime-and-admission"></a>
## 运行时与准入

准入失败和运行失败在 Python 中表现为 `ng.ProgramAdmissionError` 和 `ng.ProgramDiagnosticError`，运行结果则带有一个含错误码的 `failure`。除非该错误码有自己的条目，否则它们的 `help.doc` 链接到本节。

Core 节点的失败不属于诊断：自 #364 起，它的 `failure.code` 是节点失败分类器给出的错误码（`NG.TaskFailed`、`app.compile_error` 这样的作者自定义错误码、`NG.ProviderFailed` 等），`failure.witness.node_failure` 保存对应的记录；失败出口则以它声明的错误码结束运行（参见 [Program 图功能](program-graphs.md#failure-routing)）。没有节点失败记录的 Core 失败，仍然使用 `P_RUNTIME_CORE_FAILURE`。

| 代码 | 含义与修复 |
|---|---|
| <a id="p_core_barrier_stalled"></a>`P_CORE_BARRIER_STALLED` | Core 运行停止时，某个 join 仍在等待永远不会再到达的信号。witness 列出每个停滞的 join 的 `received` 和 `missing`，以及已运行的缺失成员的 `paths`；当 trace 显示存在可代替的节点时，还会给出 `suggested_wait_for`。 |
