<!-- neograph-i18n: source=examples/cookbook/program-coding-swarm/README.md locale=zh-CN source_sha256=cd6756c6ba1b7b59c59c32a59a1b1f97d2bd05b49a37e998fc635870b85d52a6 -->
<!-- neograph-i18n: source=examples/cookbook/program-coding-swarm/README.md -->
# Program 编码群智 (`program-coding-swarm`)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这是一个实验性概念验证（experimental proof of concept），展示 NeoGraph Program API 如何将多智能体编码工作流作为具有显式预算约束的 Program 拓扑层级运行，而不是单一的线性“编写-测试-审查”循环。本示例不声称自合成或替换的拓扑本身是安全或可靠的；本示例中的安全性来自宿主（host）拥有的编译、准入（admission）、预算上限以及确定性验证门控。

本示例中的编码目标（`slugify`、`escape_html` 和 `ini_kv`）是玩具目标（toy target）：由宿主拥有的验证节点对照测试向量进行检查的小型确定性文本转换流水线。

## 示例演示的内容

1. **并行工作 (`root -> children`)**：根 Program 运行模型驱动的 `orchestrator` 节点来规划模块拆分，按模块并行生成子 Harness（`ng.spawn` / `ng.await`），汇合结果后执行最终集成审查。传入 `--concurrency <n>` 或 `--sequential`（`--concurrency 1`）可在受限并发或顺序模式下运行相同的拓扑与输入，并在报告中对比墙钟时间重叠情况。
2. **按模块分配资源**：每个模块具有关键度（`standard` 或 `critical`）。关键模块（`escape_html`）从根预算中分得更大的不可再生（non-renewable）令牌与货币预算、更多的尝试次数、更强的实现者角色（`implementer-strong`）以及孙级（grandchild）第二意见 Harness。运行结束时会打印每个节点的剩余预算并记录在报告中。
3. **基于角色的模型映射**：各节点引用模型角色（`orchestrator`、`implementer-cheap`、`implementer-strong`、`reviewer`、`second-opinion`），不在 C++ 代码中硬编码模型 ID 或价格。角色映射、提供商端点以及 `SchemaProvider` 模型目录定义在 [`demo-models.json`](demo-models.json)（离线默认）与 [`live-models.json`](live-models.json)（在线测量）中。
4. **宿主拥有的前置条件门控**：模块子程序仅在宿主拥有的确定性 `verify` 节点通过全部测试用例 **且** 已读取审查员判定并为 `"accept"`（关键模块还需读取孙级第二意见判定且为 `"accept"`）时才能发布结果。在默认离线运行中，`slugify` 的第 1 次尝试未能通过 `verify`，`escape_html` 的第 1 次尝试被 `review` 拒绝；门控阻止这两次尝试发布并触发预算内的重试。
5. **多层监督 (`root -> child -> grandchild`)**：对于关键模块（`escape_html`），当初审通过后，子 Harness 会启动孙级 Harness（`second-opinion:escape_html`）。程序会打印三层层级结构（`root -> child -> grandchild`）及其角色、模型角色、授予与剩余预算以及最终结果。
6. **作为数据的拓扑与 SQLite 持久化（运行中替换与回滚）**：每个编译后的 `ProgramBundle`、准入的 `ProgramVersion`、运行谱系（lineage）与代际（generation）均存储在 SQLite（`SQLiteProgramStore`、`SQLiteProgramTransitionStore`、`SqliteCheckpointStore`）中。运行中途，当 `slugify` 第 1 次尝试被门控拦截后，宿主在持有的 `ng.checkpoint` 处将 `slugify` 的子拓扑替换为更严格的已准入版本（第 2 代），剩余不可再生预算直接结转而不重置，并保留子关系；第 2 次尝试完成后，宿主再回滚到先前的已准入版本（第 3 代），并从 SQLite 读回打印 3 个代际的历史记录。
7. **集中式（central）与委派式（delegated）合成对比 (`--synthesis=central|delegated`，默认 `central`)**：
   - `central`：在运行开始前，宿主一次性编译、链接（`link_module_child`）并准入整棵树（`root`、所有 `child` 模块以及关键模块的 `grandchild`）。子程序只能向预先准入的孙模板填充参数。
   - `delegated`：宿主预先仅准入根与子模块。运行时，关键子模块（`escape_html`）在检查点暂停并从受审模板库提出其孙级 JavaScript 拓扑；宿主先验证超预算提案会被 `authorize_program_child_synthesis` 拒绝（记录 `admissions_refused`），随后通过 `ProgramSynthesisGateway` 与 `ProgramRuntime::prepare_child_synthesis` 在子程序持有的权限和剩余预算内合成并准入孙级拓扑。
   - 每次运行都会在相同任务、模型与预算下执行两种模式，并在输出和 JSON 报告中记录对比结果。

## 离线模式（默认，由 CTest 运行）

无需 API 密钥且不发起网络请求即可构建和运行：

```bash
cmake --build build --target cookbook_program_coding_swarm
./build/cookbook_program_coding_swarm
ctest --test-dir build -R cookbook.program_coding_swarm --output-on-failure
```

## 在线模式与测量报告

在线模式使用 [`live-models.json`](live-models.json) 中配置的真实提供商：

- `orchestrator`（根）：Anthropic Messages API（`claude-sonnet-5-5`），读取环境变量 `ANTHROPIC_API_KEY`。
- `implementer`、`implementer-cheap`、`implementer-strong`、`reviewer`（子）：OpenRouter Chat Completions（`deepseek/deepseek-v4.1-flash-1017`），读取环境变量 `OPENROUTER_API_KEY`。
- `second-opinion`（孙）：OpenRouter Chat Completions（`inception/mercury-3`），读取环境变量 `OPENROUTER_API_KEY`。

将 [`.env.example`](.env.example) 复制为 `.env`（或在终端中导出环境变量）后运行：

```bash
export ANTHROPIC_API_KEY="..."
export OPENROUTER_API_KEY="..."

./build/cookbook_program_coding_swarm \
  --live \
  --models examples/cookbook/program-coding-swarm/live-models.json \
  --synthesis=central \
  --runs 3 \
  --max-cost-usd 0.50 \
  --max-cost-usd-other 0.25 \
  --report swarm-report.json
```

### CLI 参数

- `--mock`（默认）/ `--live`：选择确定性离线脚本提供商或在线真实提供商。
- `--models <file.json>`：角色与模型目录配置文件路径（`demo-models.json` 或 `live-models.json`）。
- `--synthesis=central|delegated`：选择主合成模式（默认 `central`）；对端模式也会一并执行并在 `synthesis_comparison` 中对比。
- `--concurrency <n>` / `--sequential`：限制同时运行的模块子程序数量（`--sequential` 将并发数设为 `1`）。
- `--runs <k>`：重复执行整个任务 `k` 次，汇总并行与顺序墙钟时间中位数、单次运行成本中位数以及门控拦截总数。
- `--max-cost-usd <usd>`：通过 `RunBudget::monetary_microunits` 与 `bind_priced_provider_call` 强制执行的总货币预算上限。
- `--max-cost-usd-other <usd>`：针对非 Anthropic 提供商（OpenRouter 子级与孙级调用）的独立货币预算上限。
- `--report <file.json>`：输出机器可读的 JSON 测量报告文件。

### OpenRouter 路由与零数据保留 (`zdr`)

在 [`live-models.json`](live-models.json) 中，OpenRouter 角色默认配置为：

```json
"provider": {
  "sort": "price",
  "allow_fallbacks": true,
  "data_collection": "allow"
}
```

这允许 OpenRouter 在启用回退且不启用 `zdr` 过滤（`zdr` 会过滤掉大量低价端点）的情况下选择低成本端点。每次调用在派发前都由 `bind_priced_provider_call` 使用 `live-models.json` 中的 `ceiling_input` 与 `ceiling_output` 费率计算上限，并在发往 OpenRouter 的请求上固定 `provider.max_price`。注意当前 C++ SDK 中的 `sp::OpenRouterRouting` 转发 `zdr`、`allow_fallbacks`、`require_parameters`、`data_collection`、`only`、`order`、`ignore` 和 `max_price`；`live-models.json` 中的 `"sort"` 会记录在报告的 `openrouter_routing` 元数据中。

如果需要零数据保留（Zero Data Retention）或固定特定上游提供商，可在 `live-models.json` 中修改对应角色的 `"provider"` 对象：

```json
"provider": {
  "zdr": true,
  "allow_fallbacks": false,
  "data_collection": "deny",
  "only": ["deepinfra", "together"]
}
```

### 如何阅读 `--report <file.json>`

- `primary_run.total_wall_time_ms` 与 `primary_run.sum_children_wall_time_ms`：对比总墙钟时间与子程序墙钟时间之和；`children_intervals_overlapped` 确认子程序的 `[wall_clock_start_ms, wall_clock_end_ms]` 时间区间发生了真实重叠。
- `primary_run.cost_by_module` 与 `primary_run.cost_by_model_role`：展示各模块和各模型角色的成本分布。
- `primary_run.nodes`：每个节点（`root`、`child`、`grandchild`）的角色、解析后的提供商/模型、尝试次数、输入/输出令牌、成本（`cost_source`：`"provider_reported"` 或 `"catalogue"`）、起止时间、结果以及授予与剩余预算。
- `primary_run.calls`：每次模型调用的遥测记录，包含 `provider_routed_to`（OpenRouter 实际路由到的上游提供商）。
- `primary_run.gate_decisions`：宿主门控评估的每次尝试，记录 `verify_passed`、`reviewer_verdict_read`、`reviewer_verdict`、`second_opinion_verdict`、`published` 与 `block_reason`。
- `primary_run.generation_transitions`：SQLite 代际历史（`initial` -> `replace_stricter` -> `rollback_initial`）以及每次转换前后的令牌与货币预算。
- `synthesis_comparison`：同一任务、模型和预算下 `central` 与 `delegated` 合成模式的对比。
- `aggregates`：`--runs <k>` 多次运行的中位数汇总。

## 在线运行实测结果 (2026-10-24)

- **配置**：`--live --synthesis=central --runs 3`，预算上限 `--max-cost-usd 2.00 --max-cost-usd-other 0.50`；模型来自 [`live-models.json`](live-models.json)（根 `orchestrator` 使用 Anthropic Messages API，子级与孙级通过配置了 `{"sort":"price","allow_fallbacks":true,"data_collection":"allow"}` 的 OpenRouter 调用）。Linux x86-64。
- **全部 3 次运行**：每个模块均在第 1 次尝试时发布（每次运行 3/3 个模块，`central` 模式下 0 次门控拦截、0 次准入拒绝）；运行验证打印 `PASS`。
- **墙钟时间（3 次中位数）**：模块子程序并行运行为 23.6 秒，相同工作顺序运行为 32.6 秒（约 1.38 倍）。单次运行的并行墙钟时间分别为 23.6 秒、21.6 秒和 28.8 秒。每次运行中子程序的时间区间均发生重叠。
- **单次运行成本（来自运行的货币记录）**：0.0291 至 0.0299 USD，中位数 0.0298 USD；其中 OpenRouter 子级与孙级为 0.0014 至 0.0020 USD，根 `orchestrator` 约为 0.028 USD。单次运行令牌数：输入约 2.7k，输出 3.9k 至 4.7k。
- **运行中拓扑替换与回滚**：与离线运行一致：从 SQLite 读回 3 个代际，不可再生预算结转（令牌 20000 -> 20000），保留子程序。
- **委派（`delegated`）模式（在同一运行中执行以供对比）**：发布 3 个模块，1 次门控拦截与 1 次准入拒绝（超预算的孙级提案按设计被拒绝），墙钟时间 32.6 秒。
- **本次运行未展示的内容**：在使用这些在线模型时，`central` 模式下门控从未需要拦截，因此门控拦截与重试路径由离线运行展示，而非本次在线运行；3 次运行不构成可靠性测量。
