<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=zh-CN source_sha256=423c15a6a32be53e05ca179df671ed5243a6684be346d3e69a23e8c2b78d581b -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harness 会在运行之前，先编译一个有界的多 worker 工作流。稳定的 MCP 接口保持为六个工具：

- `neograph_schema` 查询已安装的请求契约和预设。
- `neograph_compile` 只编译和验证，不执行。
- `neograph_start` 启动一个已保留的制品，或一个内联请求。
- `neograph_get` 轮询精简的状态，或根据 URI 取出结果制品。
- `neograph_resume` 校验并提交与待处理调用恰好对应的宿主结果。
- `neograph_cancel` 以协作方式取消排队中、运行中或等待中的工作流。

自带的预设有 `fanout_judge`、`pr_review_panel`、`bug_triage` 和 `research_synthesis`。预设生成的是普通的严格 Core 图制品；JavaScript 请求则保留它自己的 `ProgramSource` 封装和源码映射。

### JavaScript 编写边界

新发布的请求，`harness.mode` 可以取 `preset` 或 `javascript`。JavaScript 请求在 `harness.source` 中携带源码文本，还可以固定 `harness.source_id`：

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

转换器把这段文本包装成规范的 `ProgramSource` JavaScript 封装（语言为 `javascript`，QuickJS 引擎，冻结的宿主 API，imports 和源码映射），再依次交给 `ProgramCompiler`、`ProgramCatalog` 和 `ProgramRuntime`。`define()` 通过封存的 `ng` 绑定构造唯一一张图；可选的生成器 `main()` 负责普通的控制流，并 yield 出现有的带类型 Program 命令。JavaScript 不会分发 Core 节点，不会选择 provider 或工具，也无法绕过准入(admission)、预算、journal 或重放。

求值后的模块同时决定结果契约。只导出 `define()` 的源码沿用 Core 的根契约，包括 `channels.final_result.value` 这层包装。导出运行时 `main(input)` 的源码，则直接按 Harness 结果 schema 声明并校验它最终的返回值。

#### 控制流迁移示例

让 `define()` 只在编译时运行，所有运行时副作用都放在 yield 出来的带类型命令之后。下面这个完整的请求，给生成器安排了三个操作（`ng.all` 汇合，加上它的两个 Core 调用）和两路并行。它的 worker 节点原样重复了从请求中封存的配置，最终返回值具有 Harness 的结果形状：

```javascript
const source = String.raw`
function workerConfig() {
  return {
    type: "neograph_harness_worker",
    worker_id: "reviewer",
    instructions: "Return structured findings",
    tool_ids: [],
    tool_descriptions: {},
    output_schema: {type: "object", additionalProperties: true},
    provider_timeout_ms: 30000,
    max_output_tokens: 512,
    input_token_ceiling: 16384,
    max_retries: 1,
    max_provider_tool_rounds: 8,
    evidence_required: [],
    read_only: true
  };
}

export function define() {
  const graph = ng.graph("review");
  graph.channel("task", {reducer: "overwrite", initial: {}});
  graph.channel("worker_results", {reducer: "append", initial: []});
  graph.channel("final_result", {reducer: "overwrite", initial: null});
  graph.node("reviewer", workerConfig());
  graph.node("judge", {
    type: "neograph_harness_judge",
    barrier: {wait_for: ["reviewer"]}
  });
  graph.edge("__start__", "reviewer");
  graph.edge("reviewer", "judge");
  graph.edge("judge", "__end__");
  return graph;
}

export function* main(input) {
  const results = yield ng.all([
    ng.callCore("review", {task: input.task}, "review:first"),
    ng.callCore("review", {task: input.task}, "review:second")
  ], {max_in_flight: 2}, "review:all");
  return results[0].channels.final_result.value;
}
`;

const request = {
  task: {
    objective: "Review the change",
    acceptance: ["Return structured, evidence-backed findings"]
  },
  harness: {mode: "javascript", source_id: "review:main.js", source},
  workers: [{
    id: "reviewer",
    instructions: "Return structured findings",
    tools: [],
    output_schema: {type: "object", additionalProperties: true},
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  }],
  tool_catalog: [],
  budgets: {
    max_steps: 40,
    timeout_seconds: 60,
    max_parallel_workers: 2,
    max_program_operations: 3,
    max_worker_retries: 1,
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  },
  policy: {read_only: true, evidence_required: []}
};
```

稳定的 source-site 字符串是持久命令坐标的一部分。重试和重启时，要保证它们是确定的。希望最先凑够所需成功数的结果胜出时，用 `ng.any(...)`；希望最先结束的成员胜出时，用 `ng.race(...)`；两者都会通过结构化并发取消尚未结束的兄弟成员。环境层面的 I/O、定时器、动态加载、`eval` 和原生句柄依然不可用。

`harness.mode` 必须显式给出。`dsl` 返回 `H_MIGRATION_CORE_DSL`，`core` 返回 `H_MIGRATION_CORE_JSON`，`program` 和 `program_json` 返回 `H_MIGRATION_PROGRAM_JSON`；这些诊断都指向 `/harness/mode`，且绝不会根据请求 JSON 的形状或缺失的字段去推断模式。严格的 Core JSON 仍是已验证的 Core 和 Program 制品的内部/交换表示，受信的 C++ 进程内构造也仍然受支持；但两者都不是面向公众的 Harness 编写语言。

schema 导出、编译和启动现在使用同一个不可变的 `HarnessAdmissionProfile`。它带作用域的 `GraphRegistry` 和 manifest 列出了所有可用的节点、reducer 和条件，以及各自的实现、降级（lowering）和兼容性元数据。进程全局的注册表条目不属于这份可选清单（palette），Harness 的准入也无法解析它们。编译到经过验证的声明式 `TopologySpec` 为止，所以被拒绝的输入不会构造任何 `GraphNode`，也不会分发任何 worker 或副作用。已保留的制品绑定了 profile 的 ID 和指纹；不同的、或早于 profile 机制的制品，会在 start/resume 时失败时关闭，而不是被重新解释。

C++ 嵌入方在构造时，通过 `HarnessServiceResources` 传入非默认的 profile。这个增量式的资源边界保持现有 `HarnessServiceConfig` 的内存布局不变。profile 指纹覆盖 manifest，以及带作用域的注册表所导出的语义投影。每个 `implementation_identity` 都是一项受信的声明，只要对应的可调用对象行为发生变化，它就必须随之改变。

这是目前以 Program 为后端的 Harness 兼容适配器。被接受的 Harness 请求仍会转换成旧版 `ProgramSource`，经 `ProgramCompiler` 编译，经 `ProgramCatalog` 准入，再由 `ProgramRuntime` 执行；`GraphEngine` 仍是唯一执行节点的组件。

通用编写方式已确定的替代方案，是嵌入式 QuickJS 上的标准 JavaScript。以前的 `dsl`、独立的 `core` 和 `program` 模式，在新发布时会被拒绝，并给出明确的迁移诊断；严格的 Core JSON 仍是内部/交换数据。参见 [`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) 和 [`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md)。本文档描述的是保留下来的兼容行为和迁移诊断，并不授权新增任何旧版源码语义。

## 本地已认证的宿主 worker

`neograph-harness-mcp` 可以把 worker 的推理委托给一个显式选定的、已经认证过的本地 CLI。这是**模型委托**，不是凭据继承：NeoGraph 绝不会打开宿主的认证文件、接收 OAuth 令牌，或把订阅转换成 provider 的 API key。官方的宿主进程使用它自己保存的登录状态。进程组隔离要求 Linux；在其他平台上，宿主后端会失败时关闭。没有 `auto` 选项：存在多个已认证的宿主时，不能悄悄做出计费或策略方面的决定。

```bash
opencode auth login                 # or claude auth login / codex login, once
neograph-harness-mcp --executor opencode --host-status
neograph-harness-mcp --executor opencode --host-model openai/YOUR_MODEL
```

使用其他 CLI 时，选择 `--executor claude` 或 `--executor codex`；省略 `--host-model`，则采用该适配器的宿主默认模型。也可以用环境变量 `NEOGRAPH_HARNESS_EXECUTOR` 和 `NEOGRAPH_HARNESS_HOST_MODEL` 代替。查询状态和正常启动时，会先做一次有时限的、不含机密的版本与登录预检；OpenCode 会通过 `opencode models` 校验显式给出的模型名。Claude 和 Codex 没有提供同样的、可移植的模型列表 API，它们的模型选择错误是从实际发出的请求中报告的。CLI 缺失、已登出、输出格式不受支持、配额、策略和模型方面的失败，都不会被悄悄转换成 provider 调用。确切的模型名（或 `host default`）、CLI 版本、executor 身份和模式，都会出现在状态和宿主元数据中，但不含任何凭据。

嵌入方，以及将来共享进程的 MCP 配置，可以通过 `<neograph/mcp/harness_host_agent.h>` 构造出同样的边界：先调用 `preflight_host_agent(config)`，再设置 `HarnessProgramHostConfig::worker_executor = make_host_agent_executor(config)`。`HostAgentExecutorConfig` 的工作区是一个显式给出的规范根目录；请把其中不含机密的 executor 与模型身份绑定到 `provider_host_configuration` 里，这样已保留的制品就不能换一条路由来恢复。这个边界不会修改直接使用 `Provider` 的 executor，也不要求对外的 MCP Sampling。

子进程 profile 有意设计为只读，并且只支持本地 stdio。OpenCode 以 `--pure` 运行，使用全新的临时配置目录，只放行 read/glob/grep；`.env` 和已知的宿主凭据路径仍然禁止模型的 read 工具访问，工作区根目录则被授予只读的外部路径访问权限。Claude 以 `-p --safe-mode --permission-mode plan` 运行，只启用 Read/Glob/Grep，不使用 `--bare`（它会跳过订阅登录）。Codex 以 `exec --ignore-user-config --ignore-rules --ephemeral --sandbox read-only` 运行。每个子进程都只获得一份有针对性的环境变量白名单，直接 provider 的凭据以及无关的仓库/云凭据都被排除在外；另有一个深度标记，禁止嵌套的 NeoGraph 宿主委托；提示词、事件、stdout 和 stderr 的捕获量有上限；并设有截止时间，取消时会终止整个进程组。没有任何 shell 会对提示词或模型 ID 求值。Program 级别的 schema 校验和有限次重试仍然适用；CLI 生成的 JSON 在通过 worker 的 schema 闸门之前不被信任。用量按宿主机器可读的完成事件来核算。这些 CLI 传输方式并不都提供通用的、生成阶段的硬性 token 上限：超出预算的响应会被拒绝，绝不会送给 judge，但上游可能已经为它计费了。工作区的只读权限是宿主的策略边界，不是操作系统的挂载命名空间；如果不可信的仓库或宿主策略需要强文件系统隔离，请使用隔离的操作系统账户或容器。本地 CLI 适配器不接受 Harness 的 capability 工具；需要能力丰富的 worker 时，请使用直接 Provider 的 executor。

实机测试需要主动开启，且只在受信的、已认证的私有 runner 上进行：`NEOGRAPH_HARNESS_LIVE_HOST=claude|codex|opencode` 选定已安装的那一个 CLI，`NEOGRAPH_HARNESS_LIVE_MODEL` 可以选择性地固定它的模型。OpenCode 的 OAuth 任务还要设置 `NEOGRAPH_HARNESS_LIVE_OPENCODE_OAUTH=1` 和一个可用的 `openai/...` 模型。测试在执行之前，会先确认 `opencode auth list` 报告了 OpenAI OAuth（不含机密）。这个标记只是测试闸门，绝不是凭据。缺少登录、模型或闸门时，测试会明确跳过，而不是伪造推理结果。

可以按各宿主标准的本地 stdio 配置，把这个 MCP 服务注册到每个宿主上，其中的可执行文件换成 PATH 中已安装的 `neograph-harness-mcp`（不要把凭据粘贴进 MCP 条目）：

```bash
claude mcp add neograph-harness -- neograph-harness-mcp --executor claude
codex mcp add neograph-harness -- neograph-harness-mcp --executor codex
```

对于 OpenCode，运行 `opencode mcp add`，在引导提示中选择本地服务器，命令填 `neograph-harness-mcp --executor opencode`。等价的、受支持的 `opencode.json` 条目如下：

```json
{"mcp":{"neograph-harness":{"type":"local","command":["neograph-harness-mcp","--executor","opencode"],"enabled":true}}}
```

参见 [OpenCode CLI](https://opencode.ai/docs/cli/)、[Claude CLI](https://code.claude.com/docs/en/cli-reference)、[Claude 认证](https://code.claude.com/docs/en/authentication)、[Codex 非交互模式](https://learn.chatgpt.com/docs/non-interactive-mode)和 [Codex 认证](https://learn.chatgpt.com/docs/auth)。宿主的订阅配额、速率限制、模型资格、数据留存、组织策略和数据处理规定，仍然适用于被委托的调用。第三方产品使用 Claude.ai 订阅，可能需要 Anthropic 的批准；这条需要主动开启的、使用已安装 CLI 的路径，**不**构成重新分发托管式 Claude 订阅后端的批准。不要由登录了某个宿主，就推断可以访问另一个厂商或另一个模型。这个本地后端不启用远程 HTTP 委托，也不启用 MCP Sampling（可移植的宿主基线不支持）；远程/服务端部署应当使用直接的 Provider 凭据。

### 直接使用 API 的 provider（独立运行/服务端）


构建并安装这个需要主动开启的本地服务器（宿主 CLI 模式本身不需要 NeoGraph 专用的模型密钥）：

```bash
cmake -S . -B build-harness \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON \
  -DNEOGRAPH_BUILD_HARNESS_MCP_BINARY=ON
cmake --build build-harness --target neograph_harness_mcp -j
cmake --install build-harness --prefix "$HOME/.local"
export NEOGRAPH_HARNESS_API_KEY=your-key
neograph-harness-mcp --executor provider
```

对于自带的 OpenRouter 示例，`NEOGRAPH_HARNESS_API_KEY` 的优先级高于 `OPENROUTER_API_KEY`。与本地宿主执行不同，直接使用 provider 的模式需要它自己的 API key。服务器只把协议消息写到 stdout，只把诊断信息写到 stderr。

仅用于宿主互操作的冒烟测试时，配合 `--executor provider` 设置 `NEOGRAPH_HARNESS_SMOKE=1`。它使用一个确定性的进程内 provider，返回一份有效的、零发现的评审，不需要 API key，也不是对 LLM 质量的测试。
### 采用无凭据的 OpenCode 全局 MCP

单用户的本地安装，可以通过 `<neograph/mcp/adoption.h>`，显式采用 OpenCode 用户全局 `opencode.json` 中选定的、无凭据的 stdio 服务器。发现阶段只做检查：它只读取那个普通的、归当前用户所有的全局文件，绝不读取项目/工作区配置或 OAuth 存储，也绝不启动任何服务器。HTTP 条目、已禁用的条目、导入的环境变量或文件引用、不受支持的字段、shell 执行器，以及递归的 NeoGraph 条目，都会被拒绝。

采用需要三样东西：对 `argv` 不含凭据的显式声明（attestation）、一份独立批准的启动记录，以及一份针对选定工具和 schema 的 capability manifest。`pinned` 记录会绑定源内容、规范的 cwd、可执行文件和解释器的身份、argv，以及能识别出的脚本或包。无法验证的启动形式，需要显式的 `trusted_mutable` 批准；它们绝不会被悄悄降级。发现阶段不会批准它刚刚检查过的那些字节。

每个工具的 manifest 都必须使用 `argument_policy: "exact-arguments-v1"`，并配上 `argument_predicate: {"allowed_arguments": [<complete approved argument objects>]}`。比较的对象是参数的值（包括资源），而不只是 schema 的形状。笼统的 `read-only`，或 SQL/路径之类的标签会被拒绝：它们不能证明任意输入都是安全的。Harness 的权限仍然是 worker 声明、所采用的 manifest、静态策略和进程边界这四者的交集。MCP 的 annotation 无法扩大它。

在构造宿主或 provider worker 之前，先调用 `HardenedMcpClientRegistry::configure_harness(host_config, provider_config)`。它会一并安装不可变的、带命名空间的工具元数据和已批准的 executor；`tool_catalog()` 提供与之匹配的请求元数据。撤销批准或 schema 发生漂移时，会隔离已保留的 executor，并显式关闭共享的客户端，即使另一个持有者仍然持有对它的引用。schema 刷新和工具分发都遵守调用方的截止时间和取消令牌。

被采用的客户端使用绝对路径的可执行文件、规范的 cwd、替换式的白名单环境变量、有上限的协议帧和 stderr，并在关闭时终止整个进程树。Windows 上的启动使用继承句柄白名单和 kill-on-close 的 Job Object。状态记录里只有哈希和名称，没有 argv、原始配置、stderr 或凭据。带凭据的模式（`secret_injected`、`host_brokered`）、远程 HTTP MCP，以及任意的 shell/CLI 执行，仍然不受支持。

可安装的示例只支持把采用机制与 provider worker executor 搭配使用。它读取的是单独审阅过的 JSON 批准文件，而不是自动同意的环境变量开关。`NEOGRAPH_HARNESS_MCP_APPROVAL` 保存的是该文件的路径（文件不超过 1 MiB），而不是它的内容：

```bash
export NEOGRAPH_HARNESS_MCP_SOURCE="$HOME/.config/opencode/opencode.json"
export NEOGRAPH_HARNESS_MCP_APPROVAL="$PWD/approved-mcp.json"
neograph-harness-mcp --executor provider
```

批准对象有 `launch` 和 `tools` 两个成员，分别对应 `McpLaunchApproval` 和 `McpToolApproval`。`launch` 包含 `trust_mode`、显式的布尔值 `argv_no_credentials_attested`、`source_path`、`source_content_hash`、`cwd`、`executable`、`executable_identity`、`argv_hash`、`interpreter_identity` 和 `package_identity`。宿主一侧的 `make_mcp_launch_approval()` 辅助函数会计算出这些身份，供审阅使用。`tools` 包含 `server_name`、`launch_identity`（即已批准启动记录的 `executable_identity`）、`selected_tools`、`schema_hashes`、`manifest` 和 `policy_version`。记录 schema 批准时，请对规范化后的 `ToolDefinition::from_json(definition).to_json()` 值调用 `mcp_tool_schema_hash()` 来计算哈希。

显式覆盖来源时，指定的仍然必须是文档中规定的用户全局文件；项目路径不能冒充全局配置。配置了来源后，`--host-status` 会在凭据检查之前，只打印发现阶段的、经过脱敏的记录，并且绝不会启动下游服务器。

宿主代理（host-brokered）的持久调用，同时需要记录和检查点的持久化。示例用同一个显式指定的目录同时启用这两者：

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

不可变的制品、可变的运行记录，以及只追加的因果 journal 存放在 `runs.db`，图检查点存放在 `checkpoints.db`。journal 行和 Harness 创建的每一个检查点，都会把运行绑定到它不可变的制品、编译修订版的摘要、MCP 协议版本和 Harness profile。worker 的每次尝试都记录耗时、校验/重试的结果，以及把 provider、capability 和宿主代理的调用关联回发起它们的那次尝试的关联 ID。这两个 SQLite 存储都使用 WAL 模式和有上限的忙等待超时。已有的版本 1 记录数据库在打开时，会以事务方式迁移到版本 3。这个目录在服务器重启后依然保留。任一存储缺失时，`host_brokered` 的目录条目在编译时就会被拒绝，因此工作流不会宣称自己具备其实并不具备的可恢复性。

自定义的嵌入方可以通过可选目标 `neograph::mcp_sqlite` 中的 `SqliteHarnessRecordStore`，构造出同样的后端。默认的 journal 模式会在数据进入 SQLite 之前，递归地把常见的机密字段和内容字段替换为 `[REDACTED]`。`METADATA_ONLY` 丢弃所有事件的 payload；`FULL` 原样保留 provider 内容、工具参数和结果，只应该在经批准可以存储的数据上启用。可以通过 `HarnessJournal::list_events(run_id, after_sequence, limit)` 按运行顺序读取事件。偏好原子 JSON 文件的部署，仍然可以使用 `FileHarnessRecordStore`；它没有实现 journal 边界。

### 留存

SQLite 存储实现了可选的同级接口 `HarnessRetentionStore`；稳定的 `HarnessRecordStore` 虚函数表保持不变。保留制品或启动运行之前，`HarnessService` 会应用 `HarnessServiceConfig` 中的 `max_artifacts` 和 `max_runs`，默认值都是 128。

清理只会删除已处于终态的叶子运行。排队中、运行中和等待输入的运行受到保护，尚未完成 journal 收尾的进程内执行也是如此。重放或 fork 的行记录了 `source_run_id`，所以只要依赖它的那一行仍被保留，它的来源就不能删除。需要腾出空间时，先删除依赖它的叶子；来源要等到之后的某一步、不再有任何保留的行引用它，才会成为可清理的对象。因此，当所有候选对象都是活动的、被明确保护的，或仍被引用时，上限只是软限制。

在 `runs.db` 内，同一个事务先删除某个运行的 journal 行，再删除它的运行行；制品则只有在没有任何运行引用它之后才会删除。这次提交之后，Harness 再到单独配置的检查点存储里，删除该运行的检查点线程。第二阶段中发生崩溃或检查点后端故障，可能留下无法再访问的检查点存储，但不会出现已保留的重放/fork 指向一条已被删除的来源记录的情况。之后可以通过管理操作，或后端专有的孤儿清扫，回收这种只剩检查点的残留。

`FileHarnessRecordStore` 没有实现持久化的清理；它沿用以前的内存制品缓存淘汰，以及运行数量的硬上限行为。

## 调试器视图

`neograph_get` 仍以精简的 `status` 为默认视图，并新增四个调试器视图，而不需要再增加 MCP 工具：

| 视图 | 结果 |
|---|---|
| `attempts` | journal 中记录的 worker 尝试的开始/完成/中断事件 |
| `trace` | 现有的、有序的 GraphEngine 节点 trace，加上因果 journal 的时间线 |
| `checkpoints` | 不含 payload 的检查点元数据：ID、父检查点、节点、阶段、步数和通道名 |
| `diff` | 每个检查点相对其父检查点发生变化的通道值和版本 |

`attempts` 和 `trace` 接受 `after_sequence`，它是一个不透明的向前游标。四个视图都接受 `limit`，取值范围 1 到 1000。返回的制品 URI 可以用查询参数携带同样的分页信息，例如：

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

这些 URI 中只接受 `after_sequence` 和 `limit`。未知或格式错误的查询字段会导致失败，而不是被忽略。由 journal 支撑的视图原样返回持久化时的 payload，所以配置的脱敏模式得以保留。`diff` 视图是根据检查点存储而不是 journal 计算出来的，可能包含完整的通道值；对它的访问，应当按现有的详细运行结果的访问权限来对待。

## 重放模式

`neograph_start` 可以重放一个已完成的运行，同样不需要增加 MCP 工具：

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded` 用来源 journal 中已完成的 worker 尝试结果，重新执行经编译器锁定的图。它绝不会调用已配置的 worker、provider、MCP、A2A 或 capability executor。来源制品的修订版、协议和 profile 仍然必须一致，且 journal 必须使用 `FULL` payload 模式。`REDACTED` 和 `METADATA_ONLY` 的 journal 缺少确切的 worker 输出，无法重放。需要确切的已捕获操作坐标、原始的 owner/版本/输入权限，以及持久的 provider-bank 托管；不兼容的旧托管会被拒绝，绝不会根据 token 总数或尝试序号重建。

用 `mode: "live"`，可以让同一个已保留的制品改用真实的 provider 和工具来执行。快照和 journal 的生命周期事件会把运行标为 `recorded_replay` 或 `live_replay`，并带上 `source_run_id`；普通的启动仍标为 `live`。

recorded 重放会单独认证来源原始的、不可变的调用权限，与来源剩余的消费额度分开。一次来源谱系（lineage）的 CAS，把保留下来的剩余额度转移给重放运行；第二次重放无法再花掉同一份来源剩余额度。已捕获的调用沿用它们历史上的操作坐标，不会补充操作槽位。重放过程中真正新增的 Core 和挂钟时间工作，消耗的是转移过来的剩余额度。

provider 的报告、已知的计费、不确定的预留，以及副作用的标识，会恰好一次地装入一个单独的、只用于观测的 bank。回放过程不能预留、退款、结算、增加报告，也不能分发新的副作用。丢失的重放执行由它的持久租约隔离，并保留已转移/不确定的额度；它不会通过真实分发来修复。运行快照会给出当前的谱系剩余额度，包括额度已转移给某次重放的那个来源。

`ChatMessage` / `ChatTool` 和 JSON 只是可移植的投影（portable projection），并不具备 native 权限。可移植格式仍然是 [`provider-message-v2`](../schemas/provider-message-v2.schema.json) 和 [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真正的 C++ 检查点 sidecar 把 native seal 保存在内存里。要持久保存 native 历史，需要由宿主持有的 `sp::NativeArchive`：封闭的 v3 / `spna3` 格式，带经过认证的、归 owner 私有的托管（custody），并使用独立的密钥。archive v2 会被拒绝，既不会升级，也不会被解释。认证绑定的内容包括：描述符里每一项语义选择（origin、路径、header、policy、请求字段映射、usage 路径和 stop 映射）、owner，以及确切的托管绑定。它既不是加密，也不是对 vendor 签发方的认证；不要公开 archive 的正文、密钥、native blob 或原始的 wire 观测数据。archive 只是证据存储，不是资金 grant，也不是消费租约（spending lease）。Program 和外部的 bank 仍由各自独立的 journal 持有；复制快照不能凭空产生额度（credit）。

**独立 bank journal 的修正：当前契约已修订，实际运行的证据见下文。** 经 owner 批准的协议要求：有一个单调的、受信存储命名空间义务（obligation），以及真实且不可变的原始 owner/thread/graph 作用域、上限（ceiling）、截止时间/时钟标识和 generation。只有对完整检查点承诺（commitment）和 revision 做精确的持久 head CAS，才能签发由宿主持有的不透明租约（lease）。精确的 pending effect 窗口必须在 provider I/O 之前持久化；结算必须使用真实的 SDK outcome 和实际计费，以及可空的 report、hold 和去重标识。检查点和下一个 head 必须在同一个 owned actor/revision 下原子发布。删除 bank 元数据、裁剪（prune）检查点、重放旧的已认证快照、覆盖同一个 ID 或丢失 actor，都不能产生额度（credit）。已有 65 的 hold 时，把上限 130 收紧到 129，不能再准入另一个 65；已证明没有副作用的失败，可以释放保持不变的 head，使真实的 130 恢复仍能进行。崩溃、未知、租约丢失的窗口一直处于 hold 状态，不退款、不重试、不降级（fallback）。普通/原始（plain/pristine）的 archive 配置不授予资金或 native 消费租约，当前的 `config.usage` 也不能替换已有的独立义务；Program 和外部 bank 的 journal 所有权不变。这是必须满足的契约；实际的币种/托管证据和测量手段的局限见下文，它不是稳定发布的 API 保证。

**当前声明；集成运行的证据见下文：** `<neograph/graph/checkpoint.h>` 声明了 `ManagedBudgetLeaseScope`，包含 `owner_scope`、逻辑 `thread_id`、私有的后端 `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks` 和 `deadline_clock_identity`。`OwnedManagedBudgetLease` 提供只读的 `scope()`、`actor_id()`、不可变的 `bank_generation()`、`revision()`、`head_checkpoint_id()` 和 `head_commitment()`；它没有可导入权限的公开构造函数。`ManagedBudgetEffectReceipt` 提供 `active()`、`effect_id()`、`claim_amount()` 和 `request_digest()`；默认构造的 receipt 不授予任何东西。`CheckpointStore` 声明了 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)` 和 `release_managed_budget_lease(lease)`，以及对应的 `_async` 版本。同步的 `CheckpointStoreCore` 和 `AsyncCheckpointStore` 各自提供相应的变体。`managed_budget_checkpoint_commitment(checkpoint)` 覆盖完整的持久检查点，而不只是 bank 的 JSON。这些声明本身并不能证明后端的 CAS、币种安全、已安装 ABI 的兼容性，或某条运行路径已被成功演练。

**真实的 InMemory shared-bank fork 已保留并经过验证。** 最初那个真实的 C++ fork 使用唯一一份原始的 financial journal 和受信的当前分支 head，而不是复制出来的 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（及 `_async`）要求提供真实的当前源及其完整 commitment，以及指向同一个 bank 的实际 native C++ 指针；持久化的独立 fork 仍明确不支持。`OwnedManagedBudgetLease::scope()` 以及原始 owner/thread/graph、上限、截止时间/时钟和 generation 保持不可变。只读的、由存储签发的 `execution_thread_id()` / `execution_storage_thread_id()` 单独指定执行分支；`GraphState::budget_original_thread_id()` 标识原始的 financial bank。对所选分支 head 做精确 CAS，再加全局的 actor/revision，使所有分支针对规范的当前计数器、pending effect 和已作废（burned）的标识串行化。原分支和 fork 分支都保持可用，无需补充额度；过期的快照、复制的检查点和导入的 JSON 都不能产生别名（alias）或回退 head。原有的 root30 → charge3 → original continuation6 → fork lower20 → continuation9 同一 bank 的验证，在未改动的 test_graph_engine.cpp:810–913 中通过（PASSED）；保存的原始上限 30 与 fork 的有效上限 20 是两回事；放宽到 31 以及仅用 JSON 恢复，都必须被拒绝。上报的无上限观测值是事实数据，不是有限的 grant。只有已证明零副作用的租约才能释放保持不变的 head；未知或 pending 的副作用会保留其义务。

**当前的释放错误（release-error）契约；实际演练的测试集/探针见下文。** `<neograph/graph/engine.h>` 中的 `graph::ManagedBudgetLeaseReleaseError` 派生自 `ProviderOutcomeError`。`cause()` 保留原始的执行异常，`release_error()` 暴露次要的、持久化租约处置阶段的失败。`outcome()` 在有真实 SDK 证据时保留该证据，没有 SDK outcome 时为 null；释放失败既不能凭空构造 outcome，也不允许重新分发。封闭的 `_neograph_managed_budget_scope` 元数据描述原始的逻辑作用域、上限、截止时间时钟和 generation，但它只是数据，不具备后端 CAS 的权威。

**Archive owner 与留存（retention）契约；实际演练的测试集/探针见下文。** 只有有限的独立根（finite standalone root）或经认证的有限来源，才会从真实配置的 `sp::NativeArchive::owner_scope()` 继承被省略的原始 owner；无限（unbounded）或普通（plain）的 owner 元数据语义不变。显式冲突的 archive owner 会在获取租约之前被拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 以及 Core/Async 对应的存储能力，默认返回 false；真实的 InMemory 后端重写为 true，包装器必须把真实的留存能力转发下去。这个只读的描述，允许合法的、未加租约/普通/无限的 C++ native 检查点托管，但它既不授予消费额度（credit），也不授予 native replay 权限。加了租约的托管使用存储实际签发的 receipt，而不是 JSON 标志或猜测出来的存储类型。

**native 托管的 I/O 前闸门（pre-I/O gate）；实际演练的测试集/探针见下文。** 开始一个受管的副作用（managed effect）之前，在修改任何 pending effect、slot 或 held 窗口之前，必须有真正绑定的 NativeArchive，或者存储实际签发的本地私有 C++ 留存能力。这个私有能力不会从 JSON 导入，也不会通过 wire 传输。即使远端后端是 InMemory，gRPC 也要求客户端和服务端各有真实的 archive，因为 C++ sidecar 无法跨越这条边界。没有 archive 提供有限的来源 owner 时，原始的匿名 owner 作用域保持为空；真实的 archive 绑定必须与原始作用域一致。只有 financial head/租约的证据，并不能证明 native 托管已经就绪。

`ProviderOutcomeError` 是保留 outcome 的宿主错误的公共基类；`ProviderObserverError` 和 `ProviderDispatchOutcomePersistenceError` 保留已完全排空的 SDK 结果和原始的 `cause()`。持久化错误还会在 `delivery_error()` 中保留次要的 observer 失败。`ProviderFailure::outcome()` 保留 SDK 失败本身。这些都是证据，不意味着 Node 或 Program 可以重新分发：传输层的重试完全由 SDK 负责，调用方选定的 `max_output_tokens` 也绝不会被悄悄截断。上限更大的语义调用，需要新的已准备摘要、不同的确定性调用序号，并且要从原来的资源 bank、按原来的截止时间重新通过准入；具备 native replay 的资格，并不会赋予重新续期的额度。

`ProgramFailure` 保留存活的 `provider_outcome` 和 `provider_cause`。它规范的、事实性的 SDK witness，把真实的 archive 托管绑定到 owner/run/version/bundle/operation/attempt；Runtime 在暴露恢复出来的失败之前，会先主动恢复配置好的托管。公开的、纯数据的 `ProgramResult::create()` 不能用预先填好的 witness 绕过这一步，未解析完成的 parsed seal 也不是可执行的结果。进程重启之后，原来的异常指针不再可用（`provider_cause == nullptr`），也不会根据文本重建。无法持久化的失败，也就无法序列化、发布或重放。

`RecordedBindingSet` 是绑定到来源（source-bound）、只能移动的数据，不是调用方提供的 dispatcher。受信的 Catalog `recorded_capability_binder` 根据真实持久化的来源事件，独立地物化出只含已捕获内容的能力（captured-only capabilities）。`ProgramRuntime::replay_recorded()` 先检查原来所选来源的权限，再通过持久的 CAS 转移实际剩余的 bank；继承下来的花费不是新的模型 grant。旧的 `start_recorded` 续期 API 已经移除。InMemory、File、SQLite 和 PostgreSQL 的 Program 存储，在整个执行期间保留那份确切的、不可变的、归自己所有的租约；过期不会使它续期。受控的 JavaScript 仍会校验底层的 capability manifest，并消费确切的已完成命令结果，不会重新分发外部副作用。

**Recorded-control 因果修复已在完整测试集中验证。** 重放已捕获的命令时，执行之前只为新增的 CPU 挂钟时间和 Core 工作做持久的预留，然后通过结果 CAS 发布测得的工作量，以及新产生的 Core 检查点。它不消耗新的模型、资金或 Program 操作额度，也不会重新分发已捕获的外部副作用。未对账的预留仍然记为已扣减。这笔预留会选择经认证的结算转换，而不是曾经拒绝首个新 Core 检查点的普通 Running→Running 转换。await 通道的接收、定时器的等待/取消、以及 handoff 等待的发起/释放，都在各自所属的 executor/strand 上串行化；现有的 Recorded CPU/Memory await/handoff 场景在完整测试集中通过；远端 TSan 覆盖的局限仍在下文明确列出。

下面这些付费观测、native-axis 观测和集成运行观测，是 provider 切换（cutover）时期的历史 cohort。其中的数量、失败、跳过和局限都保持不变。保留下来的记录里所说的“最近一次”，指的是该 cohort 中的最后一次运行，并不表示对当前 Python 绑定或本次文档变更做过验证。

**已完成的付费观测；并非通用的资格认证（qualification）。** 最初的 `SPQUAL1` 基线为 630/1000000 microUSD，保持不变；同一份原始账本（ledger）里，由一个哈希链接的 `A` 接纳了获批的扩展额度 480/3000000，合计 1110/4000000，调用数、已花费额度、hold 和结算都是累计的，没有新的 grant ID、header，也没有重置。确切的声明字节/文件标识，以及原始的授权/基线/catalog/激活/账本前缀的哈希和总额都保持固定；删除、替换或改动都按“失败时关闭”处理。最终的规范账本为：调用 1110 次、已花费 437958、hold 1287828 microUSD，eventA1，上限 1110/4000000；已花费加 hold 共计 1.725786 美元，这是本地 catalogue 的计量，不是发票。文档记录的五个 family、每个 60 对的基线共完成 600 次请求：Chat 60/60、Responses 60/60、Messages 60/60、Generate 56/60（4 次 vision 判断错误，均为 SSE）、Interactions 57/60（1 次 buffered、2 次 SSE 的 vision 判断错误）；合计 293/300 对，不是 300/300。其他旧的 600 次相关财务记录被保留，但并不构成完整的行为证明。更早的 M5/media 一次性 cohort 保持不变。更早的三轮 Google 前置条件检查，仍保留 2 次 invalid-tool 和 1 次 unreadable-positive 失败。不再授权任何付费调用。最终的 SDK 证据和 native-axis 的局限，与基线的成功是两回事。更早的激活/重新打开（reopen）冒烟测试，仍记录为两次 reopen 后调用 610、已花费 219159、hold 751233，SDK 的 meter/canary/vision 4 个测试在 19.38 秒内通过；这些都是特定范围内的历史检查点，不是最终的账本总数。更早验证过的 Chat 60 对 cohort 仍保留 120 次实际尝试、120 次 UpperBound 计费，没有 UnknownHold。

**native-axis 观测：不是密码学验证，也不代表 native 消费或等价。** Generate 接受了篡改（mutation）、省略（omission）和重复（duplication）。Interactions 接受了孤立的真实来源/正向对照、单个 owner 的签名篡改、省略 thought-carrier、省略 call-carrier，以及重复。删除全部 thought 和签名，返回 generic 400；删除全部签名字段、但保留 THOUGHT 项，同样返回 generic 400。最后一次采集（capture）只有本地的、编码后原件的留存对照，没有同一次采集中的服务端正向结果；更早的正向 cohort 仍然是真实的。这些观测只确立了“整体 carrier 缺失”的边界，不能证明 issuer/签名校验或 vendor 的消费。实际报告：SDK 的 `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json` 和 `qualification-signature-presence-results.json`；前置条件失败、未运行、负向结果不确定这几种状态，如实保留。只要还剩另一个 carrier，仅省略 thought 或仅省略 carrier 的请求就被接受；这并不能加强 issuer 校验或 native 消费方面的任何说法。

**实际的集成验证与遗留局限。** 最近一次 Core 完整运行：2242 个测试，0 失败，16 个跳过（14 个 RAM 进程丢失用例不适用，2 个需要真实凭据的闸门），耗时 130.17 秒。`PgNestedJsonRoundTrips` 精确保留了重复的键、顺序、null 元数据、blob 和 residual，用时 0.18 秒。未改动的原始 shared-bank fork 和现有的 Recorded CPU/Memory await/handoff 场景均通过。针对真实包装的 Memory/SQLite/PostgreSQL/gRPC，执行了 finite130/hold65/lower129/strip/old-head/pruning/no-archive/import 这组探针，普通构建和 ASan+UBSan 构建下均通过。本地 Memory/SQLite/PostgreSQL 的 TSan 范围：7 项通过，0 个警告。含系统 Abseil/Protobuf 的完整混合 gRPC TSan 以退出码 66 结束，在依赖库和生成的 RPC 栈里有 402 条 race 警告：这是插桩/覆盖范围的局限，不是已证实的误报；我们不声称远端 TSan 无 race，也没有屏蔽任何警告。已安装的 find_package Program C++/C ABI/双 QuickJS 这 3 个使用方均通过。全新安装的 NeoGraph/SchemaProvider 类型化使用方通过了：2 次真实的 HTTP 请求、协程启动前销毁 provider、native/工具重放、refusal、已知为零/原始值的保留，以及真实的 LinkedMismatch 拒绝。浏览器中 Alice/Bob 的隔离和 generation 2 的替换，已经目视验证；PostgreSQL Program Chat 的 6 个黑盒测试在 18.989 秒内通过。最近一次 SDK：26/26 通过，0 失败，74.07 秒。最终的 ReleaseGraph：16 种配置 ×3 次全新进程重复，共 48 条记录，用时 38.29 秒完成，0 失败，所有实际的协议/owned-outcome 检查均通过。NeoGraph 的 `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保存了这个独立的最终 cohort。测量期间没有运行编译器，也没有调用付费模型；历史 cohort 保持不变，也不声称语义或资源上等价。不稳定的 SDK/ABI3 既不是稳定发布，也不是对更多平台的资格认证。


## 兼容 Fork

先编译修复后的 Harness，再通过现有的 `neograph_start` 工具，把确切的某个先前检查点分支到那个目标制品里：

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

来源检查点必须属于 `source_run_id`。在分配运行之前，Harness 会对照目标制品，校验检查点的 schema、来源修订版、MCP 协议、Harness profile、每个被恢复的通道和 reducer、每个后续节点，以及仍然生效的屏障的接口。不兼容的分支会返回 `started: false`、`status: "incompatible_fork"`，以及带有 `path` 和 `witness` 的、机器可读的 `H_FORK_*` 诊断；它不会创建运行，也不会创建 fork 检查点。

必须配置检查点存储。没有记录存储时，fork 只能引用仍然驻留在当前服务进程中的来源运行和制品；如果要让 fork 的谱系在重启后依然有效，就要同时配置这两种存储。

兼容的分支会被标为 `compatible_fork`，并在启动响应、快照和生命周期 journal 事件中同时带上 `source_run_id` 和 `source_checkpoint_id`。执行从所选检查点的 `next_nodes` 继续；已经提交的前驱节点不会再次运行。修复后的拓扑、worker 契约和工具目录来自目标制品，而恢复的通道值（包括原始的任务通道）来自来源检查点。如果任务输入本身需要改变，应当重新启动一个新的运行，而不是 fork。

来源运行、制品和所选检查点都是 fork 的引用对象；在兼容性检查或 fork 执行可能用到它们期间，必须一直保留。留存清理要么先删除依赖方，要么保留被引用的来源；绝不能在预检和创建分支之间删除来源检查点。

## Streamable HTTP

远程传输需要主动开启，这样现有的、只支持 stdio 的目标仍然小巧，也不会悄悄多出对 HTTP/OpenSSL 的依赖：

```bash
cmake -S . -B build-harness-http \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON \
  -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON \
  -DNEOGRAPH_BUILD_HARNESS_MCP_BINARY=ON
cmake --build build-harness-http --target neograph_harness_mcp -j
cmake --install build-harness-http --prefix "$HOME/.local"

export NEOGRAPH_HARNESS_TRANSPORT=http
export NEOGRAPH_HARNESS_HTTP_HOST=127.0.0.1
export NEOGRAPH_HARNESS_HTTP_PORT=8080
"$HOME/.local/bin/neograph-harness-mcp"
```

endpoint 是 `http://127.0.0.1:8080/mcp`。它实现了已发布的 MCP 2025-11-25 Streamable HTTP POST 契约，每个会话有各自的 MCP 生命周期，响应为 JSON。通知返回 HTTP 202。客户端已取消的调用以一个空的事件流结束，所以不会有响应送达。会话在以下情况结束：收到 DELETE；30 分钟内没有任何请求（`session_idle_timeout`）；或者在 `max_sessions`（64）已满时有新客户端需要位置，而它恰好是最近最少使用、且没有请求在处理中的会话。此后它的 id 会返回 404，提示客户端重新初始化。可选的独立 GET/SSE 通道有意没有实现，会返回 HTTP 405，传输规范明确允许这样做。

默认的安全措施都在传输层，认证不会与 `GraphEngine` 或 `HarnessService` 耦合：

- 默认绑定到 `127.0.0.1`；除非配置了 bearer 授权器，否则非回环地址的绑定会被拒绝。
- 任何带有的 `Origin`，只要不与 `NEOGRAPH_HARNESS_ALLOWED_ORIGINS` 中的某一项完全一致，就会被拒绝（可执行文件中以逗号分隔）。
- `NEOGRAPH_HARNESS_BEARER_TOKEN` 启用可执行文件的单主体 bearer 边界。作为库嵌入时，可以用 `MCPHttpServerConfig::bearer_authorizer` 做 OAuth/JWT 校验，并返回稳定的主体/作用域。
- 会话绑定到返回的授权作用域。另一个同样合法的主体，也不能复用泄露的 `Mcp-Session-Id`。
- `MCPHttpServer` 工厂接收已验证的作用域，并返回一个 `MCPHttpServerSession` 所有者。多租户的嵌入方必须用这个作用域来选择相互隔离的 Harness 记录/检查点存储；认证状态不会进入图运行时本身。没有授权器时（仅限回环绑定），作用域为空，可执行文件仍使用它单一的本地 owner 作用域。
- 请求 payload、HTTP worker、队列、会话和响应等待的上限，都由 `MCPHttpServerConfig` 限定。HTTP 线程按需启动，最多 `max_http_threads`（64）个，因为一次工具调用会占用一个线程，直到调用返回。
- 如果嵌入方的 bearer 令牌来自 OAuth 授权服务器，需要设置 `MCPHttpServerConfig::protected_resource_metadata`；服务器随后会发布 RFC 9728 元数据，并在每个 401 质询中指明它。

`neograph::mcp::ScopedHarnessStore` 把公开 ID，以及由 schema 管辖的记录/journal 引用，映射到一个可还原的租户命名空间；不透明的请求、结果和事件 payload 则不会被改写。File 和 SQLite 存储接受这些私有 ID。过长的 File 键使用定长的哈希文件名，较短键的路径不变。SQLite 的留存在一个事务内，把数量上限和删除候选都限定在所选的命名空间内，其他租户和受保护的来源引用不受影响。这一层存储边界不能替代已认证的作用域，也不能替代应用自己的 provider/工具/配额策略。

任何非回环的部署，都应当在受信的反向代理处终止 TLS，并使用它的 OAuth/OIDC 校验，或等价的 `bearer_authorizer`。要转发原始的 `Authorization` 和 `Origin` 头，不要暴露明文的公网监听端口，并且每个 Harness 状态目录只对应一个授权域。

## 宿主配置

`SERVER` 填已安装的 `neograph-harness-mcp` 二进制文件的绝对路径。这些条目选用的是直接使用 Provider 的 executor；要用上文的本地宿主模式，则改为传入对应的 `--executor claude|codex|opencode`。

```bash
SERVER=/absolute/path/to/neograph-harness-mcp
```

Claude Code，本地项目作用域：

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER" --executor provider
claude mcp get neograph-harness
```

Codex CLI：

```bash
codex mcp add neograph-harness -- "$SERVER" --executor provider
codex mcp list
```

要让非交互式的 `codex exec` 使用这个受信的本地服务器，请在 Codex 的 `config.toml` 里设置 `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"`。不设置的话，Codex 会取消 `neograph_compile`，这是正确的行为，因为保留制品并没有被标注为只读。交互式会话可以保留默认的提示。

OpenCode，写在项目的 `opencode.json` 或用户配置中：

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "neograph-harness": {
      "type": "local",
      "command": ["/absolute/path/to/neograph-harness-mcp", "--executor", "provider"],
      "enabled": true,
      "environment": {
        "NEOGRAPH_HARNESS_API_KEY": "{env:NEOGRAPH_HARNESS_API_KEY}"
      }
    }
  }
}
```

用 `opencode mcp list` 验证。这些写法遵循各宿主官方的 MCP 配置约定，核对日期为 2026-07-21。

## PR 评审工作流

先让宿主用它平常的仓库工具收集 PR 的 diff，再使用 Harness 的工具。一个合适的请求如下：

```json
{
  "task": {
    "objective": "Review this PR diff. Report only actionable correctness, security, or regression findings. Include the diff after this sentence.",
    "acceptance": [
      "Every finding identifies a file and line",
      "Every finding quotes concrete evidence",
      "Return an empty findings array when no issue is proven"
    ]
  },
  "harness": {"mode": "preset", "preset": "pr_review_panel"},
  "workers": [
    {
      "id": "correctness",
      "instructions": "Review behavior, edge cases, and regressions.",
      "tools": [],
      "output_schema": {
        "type": "object",
        "required": ["status", "findings"],
        "properties": {
          "status": {"enum": ["ok", "partial", "failed"]},
          "findings": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["file", "line", "evidence", "message"],
              "properties": {
                "file": {"type": "string"},
                "line": {"type": "integer"},
                "evidence": {"type": "string"},
                "message": {"type": "string"}
              },
              "additionalProperties": false
            }
          }
        },
        "additionalProperties": false
      }
    },
    {
      "id": "security",
      "instructions": "Review trust boundaries, validation, and unsafe side effects.",
      "tools": [],
      "output_schema": {
        "type": "object",
        "required": ["status", "findings"],
        "properties": {
          "status": {"enum": ["ok", "partial", "failed"]},
          "findings": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["file", "line", "evidence", "message"],
              "properties": {
                "file": {"type": "string"},
                "line": {"type": "integer"},
                "evidence": {"type": "string"},
                "message": {"type": "string"}
              },
              "additionalProperties": false
            }
          }
        },
        "additionalProperties": false
      }
    }
  ],
  "tool_catalog": [],
  "budgets": {
    "max_steps": 10,
    "timeout_seconds": 600,
    "max_parallel_workers": 2,
    "max_worker_retries": 1,
    "provider_timeout_seconds": 60,
    "max_output_tokens": 4096
  },
  "policy": {
    "read_only": true,
    "evidence_required": ["file", "line", "evidence"]
  }
}
```

### Provider 预算

`budgets.provider_timeout_seconds` 把单次已准备的 provider 操作的绝对截止时间设为 1--600 秒。`budgets.max_output_tokens` 把它的输出上限设为 1--128000 个 token。两者都是可选的：省略时该项控制不做设置，沿用已准入的 SDK/默认策略，而不是不限时间或不限输出。在已准备操作的截止时间之内，provider 的任何重试都由 SDK 负责。

worker 可以把其中任一字段设成更小的值。worker 的值大于 Harness 全局值的，会在编译时被拒绝。到达截止时间时，Harness 只取消提供给那次 provider 调用的子取消令牌，不会取消同级的 worker，也不会取消外层的运行。provider 必须响应这个令牌，所以无法被中断的 provider 可能在超过截止时间之后才返回。

宿主应当按以下顺序操作：

1. 调用 `neograph_compile`，如果 `ok` 为 false 就停止。
2. 用返回的 `artifact_id` 调用 `neograph_start`。
3. 用 `run_id` 轮询 `neograph_get`；它只返回结果和计数。
4. 需要细节时，用同一个 `run_id`，再用返回的 `neograph://runs/...` URI 作为 `uri`，调用 `neograph_get`。默认不要把 trace 拉进上下文。

### 发现的来源

详情制品把每个通过 schema 校验的 worker 响应保存在 `workers` 里，并为现有客户端保留原有的扁平 `findings` 数组。`finding_sources` 是与之等长的并行数组：每一项包含汇总后的 `finding_index`、来源 `worker_id`，以及该 worker 自己的 `local_index`。用它来分辨 `F1` 这类重复的本地 ID 各自来自哪里；不要往 worker 声明的 finding 对象里添加来源字段。

## 宿主代理的恢复

当某个 capability 归 MCP 宿主而不是 worker 进程所有时，使用 `executor.kind: "host_brokered"`。把 `executor.interaction` 设为 `"tool_result"`（默认）或 `"input"`。provider executor 会校验请求的参数，并返回下面两种非终态的运行状态之一：

- `awaiting_tool_results`：宿主必须执行指定的 capability。
- `input_required`：宿主必须收集一个输入值。

`neograph_get` 会返回一个 `pending` 对象，包含唯一的 `call_id`、`tool_id`、已校验的 `arguments` 和 `result_schema`。请恰好提交这一次调用的结果：

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume` 会拒绝以下情况：call ID 不匹配、结果违反声明的 schema、调用已过期，以及对并非处于等待状态的运行迟到的结果。完全相同的重复提交会被确认，但不会重新执行图；内容冲突的重复提交会被拒绝。被接受的恢复意图会在安排执行之前持久化，所以进程崩溃之后再轮询，恢复会从 `NodeInterrupt` 检查点重新开始，而不会重复执行已经成功的兄弟 worker。

### 外部副作用与对账

普通的宿主代理契约保持向后兼容：没有 `executor.effect` 的目录条目，在进程重启之后仍处于 `awaiting_tool_results`，并接受同样的 `{run_id, call_id, result}` 恢复请求。

如果宿主的某个 capability 可能产生外部可见的、非幂等的变更，要显式声明这一风险。副作用元数据只对默认的 `host_brokered` `tool_result` 交互有效；它不是收集输入用的元数据。

```json
{
  "executor": {
    "kind": "host_brokered",
    "effect": {
      "idempotency": "unsupported",
      "status_query": true,
      "fencing": true
    }
  }
}
```

这样，待处理的调用就会带上一个持久的 `effect` 对象。它的 `effect_id` 和 `idempotency_key` 的作用域是该 Harness 运行，与 provider 的 tool-call ID 不同。`status_query` 和 `fencing` 描述的是宿主的能力；Harness 只记录它们，不会自行发明某个 provider 专属的查询或重试协议。

服务重新连接时，如果有一个 `idempotency: "unsupported"` 的调用仍在等待，只会把那个运行改为 `ambiguous_effect`。这表示宿主可能在进程停止之前已经执行了该副作用，而 Harness 无法证明执行与否。精简状态中会包含 `pending` 和 `ambiguity`，journal 会记录 `host_brokered.effect.ambiguous`，Harness 既不会重放这个工具，也不会把该副作用报告为失败或已完成。

宿主在自己的权威系统中核实之后，通过 `neograph_resume` 消除这种不确定性：

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed` 会校验并使用 `result`，然后从检查点恢复。`failed` 会记录一个 Harness 的终态失败，不会再次执行该 worker。`unknown` 让运行继续停留在 `ambiguous_effect`，留待以后对账。完全相同的重复 completed、failed 或 unknown 提交是幂等的；内容冲突的 completed 或 failed 提交会被拒绝。每一次非重复的对账都会以 `host_brokered.effect.reconciled` 记入 journal。

处于不确定状态的副作用，有意设计为不可取消，也不会过期。取消或超时都无法确定外部副作用是否发生过；如果权威系统暂时还无法给出结论，就提交 `unknown`。

这套协议不声称在宿主崩溃的情况下做到恰好一次（exactly-once）的送达。支持幂等键或状态查询的宿主，应当先用这些机制确定真实的结果，再提交对账。

运行快照包含 `created_at`、`updated_at`、`expires_at` 和 `poll_after_ms`。默认的 TTL 是 24 小时，默认的轮询间隔是 1 秒；嵌入方可以通过 `HarnessServiceConfig` 覆盖这两个默认值。

## 实验性的 Tasks profile

尚未实现。本指南的早期版本曾描述过一个需要主动开启的 MCP Tasks profile，但实际上并不存在这样的 profile。Harness 服务器不声明 tasks 能力，`HarnessServiceConfig::enable_experimental_tasks` 和 `NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS` 不起任何作用，`tasks/get`、`tasks/update` 和 `tasks/cancel` 都返回 `-32601`。长时间运行的任务使用稳定的 `run_id` 加 `neograph_get` 轮询的约定。

## Capability 后端

`make_provider_harness_executor` 通过任意一个 NeoGraph `Provider` 驱动 worker。模型请求某个已声明的工具时，executor 会在分发前后，对照目录校验它的参数和输出。

provider executor 构造带类型的 `ProviderRequest`；`prepare` 在不做 I/O 的情况下校验并编码，`dispatch` 消耗已准备的请求，`invoke` 把这两步合在一起。executor 保留持有所有权的 SDK Completion/Failure outcome，包括有序的消息/parts 和部分失败，而不是把失败变成只有文本的成功。provider 的报告用可为空的字段保留未知的用量；保守的 token 计费和预留则属于另一套权限。重放 provider 的记录时，不会对这些 outcome 再次计费或结算；重放中新增的 CPU/Core 工作，仍然消耗转移过来的剩余额度。

对于已初始化的下游 `MCPClient` 实例，使用 `make_mcp_harness_capability_executor`；对于 A2A agent，使用 `a2a::make_harness_capability_executor`。权限仍以请求为准：worker 只能看到它的 `tools` 数组中列出的工具 ID。

对于文件系统工具，要在 `path_arguments` 里声明每一个带路径的输入，并设置 `policy.workspace_roots`。相对路径按第一个根目录解析；规范化之后落在所有已配置根目录之外的路径，会在分发之前被拒绝，通过已存在的符号链接逃逸的情况也包括在内。传给 capability 后端的是规范化后的路径，而不是模型给出的原始写法。下游的 MCP 和 A2A 服务仍然是各自独立的信任边界，应当强制执行同样的根目录策略，以消除文件系统的检查时到使用时（time-of-check/time-of-use）竞态。
设置 `policy.read_only: true` 后，编译会拒绝所有没有标记 `read_only: true` 的目录条目。

## 分发方式与协议 profile

受支持的本地分发方式，是上文介绍的、可安装的 `neograph-harness-mcp` 二进制文件。从源码构建时，可以继续使用示例目标；Python wheel 仍然只是库/运行时的包，不会顺带安装一个远程守护进程。MCPB 和官方注册表的发布，仍然只是发布/发现环节的打包选项，传输协议并不要求它们；只有在有签名的发布制品和明确的远程认证部署清单（manifest）时，才应该加上它们。

NeoGraph 发布的是带日期的 MCP `2025-11-25` profile；客户端如果请求 `2025-06-18`、`2025-03-26` 或 `2024-11-05`，NeoGraph 也会按该修订版定义的字段来应答，并接受 `2025-03-26` 和 `2024-11-05` 客户端发来的 JSON-RPC 批量请求。当前的 MCP 修订版 `2026-07-28` 是无状态的（没有 `initialize`，没有会话，版本号放在每个请求的 `_meta` 里），目前还不支持：官方的双时代客户端会回退到 `2025-11-25`，而固定使用 `2026-07-28` 的客户端无法连接。计划把它作为第二种模式，与 `2025-11-25` 并存提供。
