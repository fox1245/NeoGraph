<!-- neograph-i18n: source=docs/plan-intake.md locale=zh-CN source_sha256=69600014da30bc764c1fde85c2622f45ee13bd5f0a98c35f647aeff50dc58cc9 -->
# Plan intake 与 plan repair

**Languages:** [English](plan-intake.md) | [한국어](plan-intake.ko.md) | [日本語](plan-intake.ja.md) | [简体中文](plan-intake.zh-CN.md)

要产出 task graph 的 planner 模型必须写出一份封闭的、带类型的文档。step plan 是做同一件事的更小的文档：一个有序的、扁平的 step 列表。一个纯函数依据宿主的模板 allow-list 检查它，把每个问题都以模型能据以修正的形式报告出来，并把合格的计划转换成 `TaskGraphProposal`。之后 proposal 原样走既有的可信路径：`TaskGraphProposal::parse`、`TaskGraphFragmentCompiler::compile`、发布、`expand_task_graph`。

step plan 是数据，不带任何权限。通过 intake 只说明计划格式正确、符合策略，不说明它解决了请求；一份计划可能通过全部检查，却选错了工具或顺序。

```
planner text --intake_step_plan--> violations | TaskGraphProposal JSON
TaskGraphProposal JSON --parse--> --compile--> --publish--> expand_task_graph
```

本页介绍 step plan、它的 violation、它转换成的 schema 2 proposal（显式预算、字面量输入、尝试次数）、task 的重试，以及 plan repair 循环。修改一个已在运行的图不在本次发布范围内。

## step plan

```json
{
  "schema": "step-plan/v1",
  "steps": [
    {"id": "s1", "use": "web.search/v2",
     "params": {"query": "weather in Seoul tomorrow", "limit": 3},
     "retry": {"max_attempts": 2}},
    {"id": "s2", "use": "text.summarize/v1",
     "params": {"text": {"from_step": "s1", "field": "items/0/snippet"}, "style": "one sentence"}},
    {"id": "s3", "use": "notes.save/v1",
     "params": {"title": "Seoul weather", "body": {"from_step": "s2", "output": "summary", "field": "text"}},
     "after": ["s1"]}
  ]
}
```

格式是封闭的：下表没有列出的键一律是 violation。

| 键 | 类型 | 规则 |
|---|---|---|
| `schema` | string | 必须等于 `step-plan/v1` |
| `steps` | array | 至少 1 个 step，至多 `StepPlanLimits::max_steps`（默认 32）个 |
| `steps[].id` | string | `[a-z][a-z0-9_]{0,31}`，唯一 |
| `steps[].use` | string | 宿主 allow-list 中的模板 id。只有它决定行为 |
| `steps[].params` | object | 可选。键是模板的输入名，即 `input_fields` 条目去掉开头的 `/`（嵌套输入 `/filter/key` 写作 `filter/key`）。值是字面量或引用 |
| 引用 | object | 含 `from_step`（前面某个 step 的 id）。`output` 是 artifact 名，默认取生产者的 `out`，没有则取它唯一的 artifact。`field` 是去掉开头 `/` 的输出字段，默认取该 artifact 唯一的字段 |
| `steps[].out` | string | 可选。这个 step 预期产出的 artifact 名；之后的引用默认用它作 `output`。必须是模板声明过的 artifact |
| `steps[].after` | id 数组 | 可选。只能指向前面的 step 的额外顺序边 |
| `steps[].retry` | object | 可选，`{"max_attempts": n}`，n 从 1 到 `StepPlanLimits::max_retry_attempts`（3）与 `TaskGraphProposalLimits::max_attempts` 中较小的一个 |

字面量可以是任意 JSON 值：字符串、有限的数字、boolean、null、数组或对象。单个字面量的规范 JSON 不超过 `max_literal_bytes`（4096），一份计划的全部字面量合计不超过 `max_total_literal_bytes`（32768），嵌套不超过 `max_literal_depth`（6）。它们也不能超过宿主的 `TaskGraphProposalLimits::max_literal_bytes` 和 `max_total_literal_bytes`，字面量内的任何对象都不能含有键 `from_step`。

引用和 `after` 只能指向排在前面的 step。这样依赖图在构造上就是无环的：格式里没有环的错误码，模型也只需遵守一条规则。互相独立的 step 保持并行，只有引用或 `after` 才会产生边。

### step plan 不能表达的内容

| 格式中没有的内容 | 原因 |
|---|---|
| provider、模型、endpoint、URL、凭据、请求头 | 信任与权限属于宿主策略。唯一的选择器是模板 id。 |
| 代码、脚本、命令、表达式、模板语言 | 计划是数据。引用只是选取字段，从不求值。 |
| 预算、超时、capability 与 effect 列表、child binding | 由宿主根据模板契约和策略决定。 |
| join 策略 | task graph 按 all-of 汇合。 |
| 条件、循环、嵌套计划 | 让格式保持小巧。分支由执行计划的 Program 负责。 |

指向权限的键（`provider`、`endpoint`、`url`、`api_key`、`token`、`secret`、`credential`、`command`、`code`、`script`、`model`、`budget`、`timeout`）会得到错误码 `STEP_FORBIDDEN_FIELD`，以及“这由宿主决定”的提示。这份列表只是让提示更准确，真正把权限挡在计划之外的是封闭的键集合。

## 检查计划

```cpp
StepPlanIntakeOptions options;
options.source_id          = "planner:turn-1";
options.proposal_limits    = policy.limits;              // the limits the host gives TaskGraphProposal::parse
options.template_allowlist = policy.template_allowlist;  // the same trusted list

const auto result = intake_step_plan(model_text, options);
if (result.ok()) {
    const json& proposal = *result.proposal;             // schema 2, canonical; result.proposal_hash is its identity
} else {
    const auto feedback = format_violations_for_model(result.violations, 2048);
}
```

`intake_step_plan` 接受 planner 的文本，或已解析的 `json`。对文本，它取第一个顶层 JSON 对象，对象可以在代码围栏里，也可以跟在说明文字后面。`result.envelope` 说明对象是怎样找到的：整段文本就是该对象时为 `Strict`，否则为 `Recovered`。同一个对象里出现两次的键是 `STEP_DUPLICATE_KEY`；接受已解析 `json` 的重载看不到重复键。

结果包含排好序的 violation、`plan_digest`（规范化计划的 sha256 identity）、转换后的 `proposal` 和 `proposal_hash`、把转换后 proposal 的指针映射回计划 step 的 `source_map`，以及记录 `steps`、`edges`、`depth`、`width`、`literal_bytes` 数目的 `report`。intake 是确定性的：同样的文档值（键顺序和空白无关）加同样的选项，得到同样的 violation 和同样的 proposal 字节。它不会因计划内容抛出异常，只有选项本身无效时才抛出 `std::invalid_argument`。

`StepPlanIntakeOptions::template_info` 只是参考信息，只能增加拒绝：`required_params` 把缺失的输入变成 violation，`summary` 是给 planner 提示词用的文本。`step_plan_json_schema(templates, limits)` 生成针对某个目录的 JSON Schema（`use` 是允许 id 的 enum，`params` 列出每个模板的输入名），供强制结构化输出的 provider 使用。它只是辅助，最终仍由校验器说了算。[`schemas/step-plan-v1.schema.json`](../schemas/step-plan-v1.schema.json) 是与目录无关的形状。

## violation

每个 violation 是 `{path, code, message, hint, detail}`。`path` 是指向所提交文档的 JSON Pointer（`""` 表示整份文档）。消息里只出现 id、模板名和错误码。取自计划的字符串先截到 64 字节、只保留可打印字符再回显，字面量的值则从不回显，所以反馈带不回模型写的指令，不会进入下一轮提示词。提示是一句祈使句，最多列出五个有效名称。

列表先按 path 排序，逐段比较，两个数字段按整数比较（`/steps/2` 排在 `/steps/10` 前面），然后按错误码，再按 detail。超过 `max_violations`（32）就截断，有遗漏时以 `STEP_VIOLATIONS_TRUNCATED` 结尾。只要结构上发现任何 violation，就不计算计划级限额（`STEP_LIMIT_EDGES`、`STEP_LIMIT_DEPTH`、`STEP_LIMIT_BUDGET`），因为计算它们需要一张健全的图。

| 错误码 | path | 含义 |
|---|---|---|
| `STEP_NOT_JSON` | `""` | 文本里没有完整且合法的 JSON 对象 |
| `STEP_DUPLICATE_KEY` | 第二个键 | 对象里的键重复 |
| `STEP_TYPE` | 该值 | JSON 类型错误；`detail.expected` 给出正确类型 |
| `STEP_SCHEMA` | `/schema` | 缺失，或不是 `step-plan/v1` |
| `STEP_UNKNOWN_FIELD` | 该键 | 不在封闭集合内的键 |
| `STEP_FORBIDDEN_FIELD` | 该键 | 指向权限的词，由宿主决定 |
| `STEP_EMPTY`、`STEP_LIMIT_STEPS` | `/steps` | 没有 step，或太多 |
| `STEP_LIMIT_PARAMS`、`STEP_LIMIT_AFTER`、`STEP_LIMIT_DOCUMENT` | 该列表或 `""` | 某个 step 的输入或 `after` 条目超过限额，或文档大于 `max_document_bytes` 或嵌套过深 |
| `STEP_ID_REQUIRED`、`STEP_ID_FORMAT`、`STEP_ID_DUPLICATE` | `/steps/i/id` | id 的问题 |
| `STEP_USE_REQUIRED`、`STEP_USE_UNKNOWN` | `/steps/i/use` | 缺失，或不在 allow-list 中；提示会给出最接近的 id |
| `STEP_PARAM_UNKNOWN`、`STEP_PARAM_MISSING` | `/steps/i/params/key` | 模板没有这个输入，或缺少必需的输入 |
| `STEP_LITERAL_TOO_LARGE`、`STEP_LITERAL_DEPTH`、`STEP_LITERAL_RESERVED_KEY`、`STEP_LITERAL_NUMBER` | 该 param | 字面量限额（包括不接受字面量的宿主）、字面量内的 `from_step` 键、非有限数 |
| `STEP_REF_SYNTAX` | 该引用 | 引用对象格式错误 |
| `STEP_REF_UNKNOWN_STEP`、`STEP_REF_FORWARD` | `.../from_step` | 不是某个 step，或不是前面的 step（step 不能引用自己） |
| `STEP_REF_OUTPUT_UNKNOWN`、`STEP_REF_FIELD_UNKNOWN`、`STEP_REF_FIELD_AMBIGUOUS`、`STEP_REF_OUT_MISMATCH` | `.../output`、`.../field` 或该 param | 输出或字段不在生产者的契约中；有多个而未指明（提示会列出）；`output` 与生产者的 `out` 不同 |
| `STEP_OUT_UNKNOWN` | `/steps/i/out` | 不是模板声明过的 artifact |
| `STEP_AFTER_UNKNOWN`、`STEP_AFTER_FORWARD`、`STEP_AFTER_DUPLICATE` | `/steps/i/after/j` | 顺序边的问题 |
| `STEP_RETRY_FIELD`、`STEP_RETRY_RANGE` | `/steps/i/retry/...` | `max_attempts` 以外的键，或超出允许范围的值 |
| `STEP_LIMIT_EDGES`、`STEP_LIMIT_DEPTH`、`STEP_LIMIT_BUDGET` | `/steps` | 计划级上限：依赖数、最长链，或各 step 预留预算之和（重试每次尝试各计一次） |
| `STEP_LOWERING_REJECTED` | 对应的 step | `TaskGraphProposal::parse` 拒绝了转换结果；`detail.proposal_code` 含 `P_PROPOSAL_*` 错误码。说明宿主策略与 intake 检查的限额不一致，或 intake 有疏漏；最终仍由 parse 说了算 |
| `STEP_VIOLATIONS_TRUNCATED` | `""` | 列表被截断 |

模型收到的内容示例：

```json
[
  {"path": "/steps/2/use", "code": "STEP_USE_UNKNOWN",
   "message": "Template 'web.serach' is not available.",
   "hint": "Use one of: text.summarize/v1, web.search/v2. Closest: web.search/v2.",
   "detail": {"given": "web.serach", "choices": ["text.summarize/v1", "web.search/v2"]}},
  {"path": "/steps/3/params/text/from_step", "code": "STEP_REF_FORWARD",
   "message": "Step 's4' does not come before this step.",
   "hint": "Reference only steps listed earlier in 'steps'; reorder the steps or drop the reference."}
]
```

## 转换（lowering）

转换是只由文档和选项决定的纯函数。

- task 按 step 的顺序排列。引用变成输入 binding，字面量变成 schema 2 字面量，`after` 和引用的生产者变成 `depends_on`（已排序、无重复）。
- planner 不决定预算。每个 step 在每个维度上预留模板上限与宿主的单 task 上限中较小的那个，四个维度全部显式。尝试 `n` 次的重试会把它预留 `n` 次。
- proposal 一律是 schema 2，因此宿主的启动预算是精确的（见下），并且在 intake 返回之前已由 `TaskGraphProposal::parse` 解析。返回的 `proposal` 已是规范形式。

## schema 2 proposal

`TaskGraphProposal` schema 2 是 step plan 转换的目标，planner 也可以直接写它（参见 [`schemas/task-graph-proposal-v2.schema.json`](../schemas/task-graph-proposal-v2.schema.json)）。schema 1 的文档保持原有的规范字节和 identity；声明 schema 1 却使用 schema 2 成员的文档是 `P_PROPOSAL_SCHEMA_VERSION`。

### 显式预算（#412）

在 schema 1 中，把某个预算维度留成 0 的 task 在 proposal 和 fragment 的上限里被算作 0，但它的子运行启动时却拿到该维度 link 的全部上限，而留空的 `output_bytes` 允许任意输出。schema 2 堵上了这个缺口：

- task 必须为模板上限非 0 的每个维度写明非 0 的值（`P_PROPOSAL_BUDGET_ZERO`）。只有模板上限为 0 的维度才可以保持为 0。
- schema 2 的 fragment 按预留的值原样启动每个 task。与 `RunBudget` 一样，0 的维度不授予任何额度，输出被限制在写明的 `output_bytes` 内。

| | schema 1 | schema 2 |
|---|---|---|
| 留空的预算维度 | 接受；算作 0；子运行拿到 link 上限 | 模板可以花费的维度出现 `P_PROPOSAL_BUDGET_ZERO` |
| `output_bytes` 为 0 | 任意输出 | 任何输出都放不下 |
| 既有文档的规范字节与 identity | 不变 | 不适用 |

接收 planner 提案的宿主把 `TaskGraphProposalLimits::min_schema_version` 设为 2，即可拒绝 schema 1。

### 字面量输入

task 可以带 `literals`：`[{"to": {"field": "/question"}, "value": ...}]`。字面量把一个 JSON 值赋给模板声明过的一个输入字段，在子运行的输入里排在 binding 之后。目标必须是模板的输入（`P_PROPOSAL_LITERAL_TARGET`），并且不能再接收其他 binding 或字面量（`P_PROPOSAL_LITERAL_DUPLICATE_TARGET`）。字面量是数据，从不求值。规范形式中按目标排序。

### 尝试次数

task 可以声明 `max_attempts`（从 1 到宿主的 `TaskGraphProposalLimits::max_attempts`；`P_PROPOSAL_ATTEMPTS`）。每次尝试都会启动一个带有 task 全部预算的新子运行，所以汇总按尝试次数预留预算：task 的 `reserved_budget` 等于 `budget` 乘以 `max_attempts`，proposal 和 fragment 都拿这个总和与上限比较。

### 宿主限额

| `TaskGraphProposalLimits` | 默认值 | 含义 |
|---|---|---|
| `min_schema_version` | 1 | 接受的最低 schema |
| `max_attempts` | 1 | task 可声明的最多尝试次数。1 表示不重试 |
| `max_literal_bytes` | 0 | 单个字面量的规范 JSON 字节数。0 表示拒绝字面量 |
| `max_total_literal_bytes` | 0 | 一份 proposal 全部字面量的规范 JSON 字节数之和 |

被拒绝的 expansion 会在 `failure.witness.diagnostics` 中以 `{code, pointer}` 报告前几条 diagnostic。

## task 的重试

一批 task 结束后，如果某个 task 的子运行以节点失败分类器认为可重试的失败告终（`failure.witness.node_failure.retryable` 为 true，且 `kind` 不是 `effect_committed`、`ceiling` 或 `deadline`；与 plan 级 `retry` 操作的规则相同），并且还有剩余尝试次数，就用下一个尝试编号再次派发。被取消或超时的 task，以及不属于节点失败的失败（缺少输入、输出超出契约），从不重试。

- 终态一旦确定就不再改变，所以对将要重试的尝试不会写入 `Failed`。下一次派发把 task 以下一个尝试编号和新的尝试 identity 写为 `Active`，并在记录里保留上一次尝试的失败，直到下一次结果把它替换。尝试编号从不减小。
- 每次尝试都是一个独立的子运行，run id 由尝试 identity 推出，拿到 task 的全部预算。
- 重试发生在下一次扫描图的时候，与新就绪的 task 一样处理：依赖被重试 task 的 task 会等待，在 `fail_fast` 策略下，可重试的失败也不会取消其他 task。
- 下一次尝试开始前发生崩溃，会重新启动同一次尝试；它的子运行已经带着那个失败，所以会再次得出同样的判定。

## plan repair

模型的第一份计划往往无效。plan repair 把 violation 放进提示词再问一次，最多问到设定的次数。它是建立在既有机制上的模式，不是新的 plan 操作：一张 Core 图 `plan -> intake -> (plan again)`，其中 `intake` 是一个注册的纯节点。run 的预算、provider 调用日志和 Core 检查点给了这个循环所需的一切：有界的次数、token 与费用的上限、期限，以及被记录的状态，所以恢复后的 run 不会为已经拿到的答案再去问模型。

```json
{
  "schema_version": 1,
  "name": "main",
  "channels": {
    "messages": {"reducer": "overwrite", "initial": [{"role": "user", "content": "..."}]},
    "proposal": {"reducer": "overwrite", "initial": null},
    "repair": {"reducer": "overwrite", "initial": {}}
  },
  "nodes": {
    "plan": {"type": "llm_call"},
    "intake": {"type": "neograph.step_plan_intake/v1", "max_attempts": 3, "feedback": "replace"}
  },
  "edges": [
    {"from": "__start__", "to": "plan"},
    {"from": "plan", "to": "intake"}
  ],
  "conditional_edges": [
    {"from": "intake", "condition": "neograph.step_plan_route/v1",
     "routes": {"ok": "__end__", "retry": "plan", "give_up": "__end__"}}
  ],
  "node_retry_policies": {"plan": {"max_visits": 3}}
}
```

在 Program 中，Core 定义的 `name` 必须等于嵌入它的根操作的名字（这里是 `main`）。`plan` 可以是任何读取 `messages` 并把模型的回答作为 assistant 消息追加进去的节点。之后 Program 用 `"proposal_source": {"artifact": "input", "field": "/channels/proposal/value"}` 从 planner 的输出里 expand 被接受的 proposal。[`examples/65_program_plan_intake.cpp`](../examples/65_program_plan_intake.cpp) 用脚本化的 planner 离线运行这条流程：先是带三处错误的第一次回答，再经过一轮修正，最后被接受的计划被 expand 成子 Program。

### 注册的可执行体

`add_step_plan_repair(builder, options)` 向 `RegistrySnapshotBuilder` 注册三个可执行体。`StepPlanRepairNodeOptions::intake` 保存可信的 allow-list 和限额；封印后的图无法更改它们，可执行体的 identity 也涵盖它们，所以用某个 allow-list 准入的 Program 不会用另一个 allow-list 运行。

| 可执行体 | 作用 |
|---|---|
| `neograph.step_plan_intake/v1`（节点） | 读取 `messages` 中最后一条 assistant 消息，运行 `intake_step_plan`，写入 `proposal`（转换后的 proposal，或 null）和 `repair`；还有下一次尝试时，也写入 `messages`。配置项是 `max_attempts`（1 到 `max_attempts_ceiling`，默认 3）和 `feedback` |
| `neograph.step_plan_route/v1`（条件） | 根据 `repair.status` 返回 `ok`、`retry` 或 `give_up`，其他值一律是 `give_up` |
| `neograph.step_plan_report/v1`（节点） | 把 `repair.status` 重写为 `exhausted`，别的什么都不做，供以 failure exit 结束循环的图使用 |

`feedback: "replace"`（默认）在每次尝试时用 system 消息、第一条 user 消息、被拒绝的计划和 violation 重建 `messages`，所以上下文不会增长，也不会重读之前的错误。`"append"` 保留整段对话并追加 violation。节点整体写入 `messages`，所以通道的 reducer 无关紧要。violation 由 `format_violations_for_model` 渲染，长度以 `max_feedback_bytes`（2048）为上限。

### 报告

`repair` 通道保存报告，它与 `repair_step_plan` 返回的值相同：

```json
{"status": "exhausted", "attempts": 3, "max_attempts": 3,
 "last_violations": [{"path": "...", "code": "STEP_USE_UNKNOWN", "message": "...", "hint": "...", "detail": {}}],
 "violation_code_histogram": {"STEP_USE_UNKNOWN": 3, "STEP_REF_FORWARD": 1},
 "plan_digests": ["sha256:...", "sha256:...", "sha256:..."],
 "envelope": ["strict", "recovered", "strict"]}
```

`status` 是 `ok`、`retry` 或 `exhausted`。`last_violations` 在 8 KiB 处截断。

### 限额与最终失败

| 限额 | 机制 |
|---|---|
| 尝试次数 | intake 节点的 `max_attempts`，受宿主上限约束；`plan` 上的 `max_visits` 是兜底，上面的逻辑一旦出错就让 run 失败 |
| token 与费用 | run 的预算；把 planner 图作为子 Program 运行（`spawn` 后用 `await` 汇合），就能给规划阶段单独的上限 |
| 耗时 | run 的 `wall_time_ms`，以及每次模型调用的 `attempt_timeout_ms` |
| 模型调用重试 | `plan` 节点的 `max_retries`，与上面的尝试次数无关 |

最后一次尝试之后有两种收尾方式：

- 回退（面向用户的流程推荐）。`give_up` 路由到 `__end__`；报告写着 `exhausted`，父 Program 根据 `/channels/repair/value/status` 分支，直接回答或询问用户。不会发布 fragment。
- 失败。`give_up` 路由到一个 `neograph.step_plan_report/v1` 节点，该节点被声明为错误码 `plan.repair_exhausted` 的 failure exit。run 以该错误码结束为 `Failed`，输出里仍带着报告。

因预算而停止时，状态和错误码仍是既有的 `BudgetExhausted`，所以宿主能区分“模型改不对”和“钱用完了”。

### 在 run 之前规划

在启动 run 之前先做规划的宿主使用 `repair_step_plan`：

```cpp
PlanRepairOptions options;
options.intake       = intake_options;
options.max_attempts = 3;
const auto result = repair_step_plan(options, request_text,
    [&](const std::vector<PlannerMessage>& messages) {
        return PlannerReply{call_model(messages), prompt_tokens, completion_tokens};
    });
if (result.ok) start_run(result.proposal);   // the proposal becomes the run input
```

被接受的 proposal 成为 run 的输入，run 记录会持久化它，所以恢复运行时不会重新规划。费用核算由宿主自己负责：run 的预算不包含规划。如果规划必须计入 run 的开销，请使用 Core 图。

## 兼容性与上线顺序

这棵代码树还处于 v1 之前。这些改动给公开结构体新增了成员，请把 NeoGraph、所有 C++ 使用方和 Python 扩展一起重新构建（参见 [`docs/ABI_POLICY.md`](ABI_POLICY.md) 和变更日志条目）。

| 记录 | 规则 |
|---|---|
| schema 1 proposal | 规范字节与 identity 不变 |
| schema 2 proposal | 新增。旧版二进制以 `P_PROPOSAL_SCHEMA_VERSION` 拒绝它 |
| schema 1 fragment 及其已存记录 | 不变，启动方式也和以前一样（留空的维度拿到 link 上限） |
| schema 2 fragment | 新增。旧版二进制拒绝解析它 |

不需要迁移已存数据。在任何写入方开始产出 schema 2 proposal、字面量或重试之前，先升级所有能恢复 run 的进程；已经出现 schema 2 fragment 之后要回滚，必须先让这些 expansion 排空。

不在本次发布范围内的有：修改正在运行的图、面向 SQLite 或 PostgreSQL 的持久化 fragment 存储，以及面向 planner 的模板费用目录视图。
