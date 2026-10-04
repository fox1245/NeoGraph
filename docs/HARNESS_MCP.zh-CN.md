<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=zh-CN source_sha256=01b4115c436243f90da697e06b1474fbbbc430bfa05e8dc301b252d9b518f7db -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harness 在运行前编译有界的多工作者工作流。稳定的 MCP 接口提供六个工具：

- `neograph_schema` 查询已安装的请求契约和预设。
- `neograph_compile` 编译并验证而不执行。
- `neograph_start` 启动一个保留的工件或一个内联请求。
- `neograph_get` 轮询紧凑状态或解引用一个结果工件 URI。
- `neograph_resume` 验证并提交与待处理调用精确对应的主机结果。
- `neograph_cancel` 协作式取消排队、运行中或等待中的工作流。

已发布的预设是 `fanout_judge`、`pr_review_panel`、`bug_triage` 和 `research_synthesis`。预设生成普通的严格 Core 图工件；JavaScript 请求保留其自身的 `ProgramSource` 封装和源映射。

### JavaScript 开发边界

新发布的请求可以将 `harness.mode` 设为 `preset` 或 `javascript`。JavaScript 请求通过 `harness.source` 携带源文本，也可固定 `harness.source_id`：

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

转换器将该文本包装为规范的 `ProgramSource` JavaScript 封装（语言为 `javascript`，采用 QuickJS 引擎、冻结的主机 API、导入和源映射），再交给 `ProgramCompiler`、`ProgramCatalog` 和 `ProgramRuntime`。`define()` 通过封闭的 `ng` 绑定构造一个图；可选的生成器 `main()` 负责普通控制流，并通过 `yield` 提交现有的类型化 Program 命令。JavaScript 不调度 Core 节点、不选择提供者或工具，也不绕过准入、预算、日志或重放。

求值后的模块也决定结果契约。仅导出 `define()` 的源保留 Core 根契约，包括 `channels.final_result.value` 包装。若源导出运行时 `main(input)`，则直接按 Harness 结果模式声明并验证其最终返回值。

#### 控制流迁移示例

让 `define()` 只在编译时构图，并通过 `yield` 提交类型化命令来执行每个运行时副作用。这个完整请求为生成器分配三个操作（`ng.all` 汇合及两个 Core 调用），并允许两个任务并行。工作者节点配置与请求中封存的配置完全一致，最终返回值符合 Harness 结果结构：

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

稳定的源位置字符串是持久化命令坐标的一部分。重试和重启时必须保持一致。需要最先达到指定成功数量的任务胜出时，使用 `ng.any(...)`；需要最先进入终态的任务胜出时，使用 `ng.race(...)`。两者都通过结构化并发取消尚未完成的同级任务。环境 I/O、定时器、动态加载、`eval` 和原生句柄仍然不可用。

`harness.mode` 必须是显式的。`dsl` 返回 `H_MIGRATION_CORE_DSL`，`core` 返回 `H_MIGRATION_CORE_JSON`，而 `program`/`program_json` 返回 `H_MIGRATION_PROGRAM_JSON`；所有都指向 `/harness/mode`，并且永远不会从请求的 JSON 形状或缺失字段中选择。严格的 Core JSON 仍然是已验证 Core 和 Program 工件的内部/交换表示，受信任的 C++ 进程内构造仍然受支持；两者都不是公开的 Harness 编写语言。

模式导出、编译和启动现在都使用同一个不可变的 `HarnessAdmissionProfile`。其作用域内的 `GraphRegistry` 和清单列出所有可用节点、归约器（reducer）和条件，以及实现、转换和兼容性元数据。进程全局注册表条目不属于这组可用项，Harness 准入不能解析它们。编译在已验证的声明式 `TopologySpec` 处停止，因此被拒绝的输入不会创建 `GraphNode`，也不会调度工作者或副作用。保留的工件绑定配置档 ID 和指纹；配置档不同或早于配置档机制的工件会在启动或恢复时被拒绝，不会被重新解释。

C++ 嵌入方在构造时通过 `HarnessServiceResources` 传入非默认配置档。新增的资源边界保留现有 `HarnessServiceConfig` 布局。配置档指纹涵盖清单和作用域注册表导出的语义投影。每个 `implementation_identity` 都是受信任的声明；对应可调用对象的行为一旦改变，该标识也必须改变。

这是当前由 Program 支持的 Harness 兼容适配器。已接受的 Harness 请求仍转换为旧 `ProgramSource`，通过 `ProgramCompiler` 编译、`ProgramCatalog` 准入，再由 `ProgramRuntime` 执行；`GraphEngine` 仍是唯一的节点执行器。

通用编写所采用的替代方案是嵌入式 QuickJS 上的标准 JavaScript。旧 `dsl`、独立 `core` 和 `program` 模式在新发布时被拒绝，并返回明确的迁移诊断；严格 Core JSON 仍是内部或交换数据。参见 [`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) 和 [`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md)。本文描述保留的兼容行为和迁移诊断，不授权引入新的旧式源语义。

## 本地已认证主机工作者

`neograph-harness-mcp` 可以将工作者推理委托给显式选择的、已完成认证的本地 CLI。这是**模型委托**：NeoGraph 不会打开主机认证文件、接收 OAuth 令牌，也不会将订阅转换成提供者 API 密钥。官方主机进程使用自己保存的登录信息。进程组隔离需要 Linux；其他平台会拒绝启动该主机后端。没有 `auto` 选择：即使多个主机都已认证，也不能静默决定计费或策略。

```bash
opencode auth login                 # or claude auth login / codex login, once
neograph-harness-mcp --executor opencode --host-status
neograph-harness-mcp --executor opencode --host-model openai/YOUR_MODEL
```

其他 CLI 分别使用 `--executor claude` 或 `--executor codex`；省略 `--host-model` 时，请求该适配器的主机默认模型。也可通过环境变量 `NEOGRAPH_HARNESS_EXECUTOR` 和 `NEOGRAPH_HARNESS_HOST_MODEL` 设置。状态查询和正常启动都会先进行有界、不含秘密的版本及登录预检；OpenCode 还通过 `opencode models` 验证显式模型名称。Claude 和 Codex 不提供同样的可移植模型列表 API，因此在实际请求失败时报告模型选择错误。CLI 缺失、退出登录、输出不受支持、配额、策略和模型错误都不会被静默改为提供者调用。状态和主机元数据会列出确切模型或 `host default`、CLI 版本、执行器标识和模式，不包含凭证。

嵌入方及未来的共享进程 MCP 配置可通过 `<neograph/mcp/harness_host_agent.h>` 构造相同边界：先调用 `preflight_host_agent(config)`，再设置 `HarnessProgramHostConfig::worker_executor = make_host_agent_executor(config)`。`HostAgentExecutorConfig` 的工作区必须是显式指定的规范根目录；将不含秘密的执行器及模型标识绑定到 `provider_host_configuration`，防止保留工件通过另一条路由恢复。这个边界不修改直接调用 `Provider` 的执行器，也不要求出站 MCP Sampling。

子进程配置限定为只读、本地 stdio。OpenCode 从新建的临时配置目录以 `--pure` 运行，只允许 read/glob/grep；模型的读取工具仍无法访问 `.env` 和已知主机凭证路径，工作区根目录获得只读的外部路径访问权限。Claude 使用 `-p --safe-mode --permission-mode plan`，只允许 Read/Glob/Grep，不使用会跳过订阅登录的 `--bare`。Codex 使用 `exec --ignore-user-config --ignore-rules --ephemeral --sandbox read-only`。每个子进程都使用专门的环境变量允许列表，排除直接提供者凭证和无关的仓库及云凭证；同时具备禁止嵌套 NeoGraph 主机委托的深度标记、有界的提示及事件和 stdout/stderr 捕获，以及截止时间。取消会终止进程组。提示和模型 ID 不经 shell 求值。Program 级模式验证和有界重试仍然生效；CLI 生成的 JSON 必须通过工作者模式检查后才受信任。用量按主机的机器可读完成事件记账。这些 CLI 传输不提供通用的生成时硬性令牌上限：超预算响应会被拒绝且不会发送给裁判，但上游用量可能已产生费用。工作区只读权限属于主机策略边界，并非操作系统挂载命名空间；不受信任的仓库或主机策略需要强文件系统隔离时，应使用隔离的操作系统账户或容器。本地 CLI 适配器不接受 Harness 能力工具；需要丰富能力工具的工作者应使用直接 Provider 执行器。

实时测试只在可信、已认证的私有运行器上显式启用：`NEOGRAPH_HARNESS_LIVE_HOST=claude|codex|opencode` 选择一个已安装 CLI，`NEOGRAPH_HARNESS_LIVE_MODEL` 可选地固定模型。OpenCode OAuth 任务还设置 `NEOGRAPH_HARNESS_LIVE_OPENCODE_OAUTH=1` 和可用的 `openai/...` 模型。测试执行前确认 `opencode auth list` 报告 OpenAI OAuth（不含秘密）。该标记只控制测试准入，不是凭证。登录、模型或准入标记缺失时会明确跳过，不会伪造推理。

可通过各主机的标准本地 stdio 配置注册 MCP 服务器，使用 PATH 中已安装的 `neograph-harness-mcp` 二进制文件（不要将凭证粘贴到 MCP 条目中）：

```bash
claude mcp add neograph-harness -- neograph-harness-mcp --executor claude
codex mcp add neograph-harness -- neograph-harness-mcp --executor codex
```

OpenCode 用户可运行 `opencode mcp add`，按交互提示选择本地服务器，并使用命令 `neograph-harness-mcp --executor opencode`。等效且受支持的 `opencode.json` 条目为：

```json
{"mcp":{"neograph-harness":{"type":"local","command":["neograph-harness-mcp","--executor","opencode"],"enabled":true}}}
```

参见 [OpenCode CLI](https://opencode.ai/docs/cli/)、[Claude CLI](https://code.claude.com/docs/en/cli-reference)、[Claude 认证](https://code.claude.com/docs/en/authentication)、[Codex 非交互模式](https://learn.chatgpt.com/docs/non-interactive-mode) 和 [Codex 认证](https://learn.chatgpt.com/docs/auth)。委托调用仍受主机订阅配额、速率限制、模型资格、保留规则、组织策略及数据处理规则约束。第三方产品使用 Claude.ai 订阅可能需要 Anthropic 批准；显式启用已安装 CLI 的方式**不代表**获准分发托管的 Claude 订阅后端。不能认为登录某个主机就能访问其他厂商或模型。此本地后端不启用远程 HTTP 委托或 MCP Sampling（可移植主机基线不支持）；远程或服务器部署应使用直接 Provider 凭证。

### 直接 API 提供者（独立进程/服务器）

构建并安装需显式启用的本地服务器（主机 CLI 模式本身不需要 NeoGraph 专用模型密钥）：

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

随附的 OpenRouter 示例中，`NEOGRAPH_HARNESS_API_KEY` 优先于 `OPENROUTER_API_KEY`。直接提供者模式需要自己的 API 密钥，与本地主机执行不同。服务器只向 stdout 写入协议消息，只向 stderr 写入诊断。

仅进行主机互操作冒烟测试时，将 `NEOGRAPH_HARNESS_SMOKE=1` 与 `--executor provider` 一起使用。此模式使用确定性的进程内提供者，返回有效且没有发现项的审查结果，不需要 API 密钥，也不能作为 LLM 质量测试。
### 采用无需凭证的 OpenCode 全局 MCP 配置

单用户本地安装可以通过 `<neograph/mcp/adoption.h>`，显式采用 OpenCode 用户全局 `opencode.json` 中选定的、无需凭证的 stdio 服务器。发现过程只检查配置：只读取这个由用户拥有的普通全局文件，不读取项目或工作区配置及 OAuth 存储，也不启动服务器。HTTP 条目、已禁用条目、导入的环境变量或文件引用、不受支持字段、shell 执行器及递归 NeoGraph 条目都会被拒绝。

采用配置需要显式证明 `argv` 不含凭证、独立批准的启动记录，以及选定工具和模式的能力清单。`pinned` 记录绑定源内容、规范 cwd、可执行文件及解释器标识、argv，以及能够识别的脚本或软件包。无法验证的启动形式必须显式批准为 `trusted_mutable`，不能静默降级。发现过程不会批准自己刚检查过的字节。

每个工具清单必须使用 `argument_policy: "exact-arguments-v1"`，并设置 `argument_predicate: {"allowed_arguments": [<complete approved argument objects>]}`。比较涵盖参数值和资源，不仅是模式形状。泛化的 `read-only` 或 SQL/路径标签会被拒绝，因为它们不能证明任意输入都安全。Harness 权限始终是工作者声明、采用的清单、静态策略及进程边界的交集；MCP 注解不能扩大权限。

构造主机或提供者工作者前，调用 `HardenedMcpClientRegistry::configure_harness(host_config, provider_config)`。它会同时安装不可变的命名空间工具元数据和已批准执行器；`tool_catalog()` 提供匹配的请求元数据。撤销批准和模式漂移会阻止保留执行器继续调用，并显式关闭共享客户端，即使其他所有者仍持有引用。模式刷新和工具调度都遵守调用方截止时间及取消令牌。

采用的客户端使用绝对可执行路径、规范 cwd、替换式环境变量允许列表、有界协议帧及 stderr，以及进程树关闭机制。Windows 启动使用继承句柄允许列表和关闭即终止的 Job Object。状态记录仅包含哈希和名称，不包含 argv、原始配置、stderr 或凭证。携带凭证的模式（`secret_injected`、`host_brokered`）、远程 HTTP MCP 和任意 shell/CLI 执行仍不受支持。

可安装示例仅支持与提供者工作者执行器一起采用配置。它读取经过单独审查的 JSON 批准对象，不使用自动同意环境标记：

```bash
export NEOGRAPH_HARNESS_MCP_SOURCE="$HOME/.config/opencode/opencode.json"
export NEOGRAPH_HARNESS_MCP_APPROVAL="$(cat approved-mcp.json)"
neograph-harness-mcp --executor provider
```

批准对象包含与 `McpLaunchApproval` 和 `McpToolApproval` 对应的 `launch` 和 `tools` 成员。`launch` 包含 `trust_mode`、显式的 `argv_no_credentials_attested` 布尔值、`source_path`、`source_content_hash`、`cwd`、`executable`、`executable_identity`、`argv_hash`、`interpreter_identity` 和 `package_identity`。主机侧 `make_mcp_launch_approval()` 辅助函数计算这些标识，供审查使用。`tools` 包含 `server_name`、`launch_identity`（已批准启动记录的 `executable_identity`）、`selected_tools`、`schema_hashes`、`manifest` 和 `policy_version`。记录模式批准时，使用 `mcp_tool_schema_hash()` 对规范化的 `ToolDefinition::from_json(definition).to_json()` 值计算哈希。

显式覆盖源路径时，仍必须指向文档指定的用户全局文件；项目路径不能冒充全局配置。配置源之后，`--host-status` 会在凭证检查前输出仅用于发现的脱敏记录，绝不启动下游服务器。

持久化宿主代理的调用需要同时具备记录和检查点的持久化。示例通过一个显式目录实现了两者：

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

该目录在 `runs.db` 中存储不可变工件、可变运行记录和追加式因果日志，在 `checkpoints.db` 中存储图检查点。日志行和每个 Harness 创建的检查点都将运行绑定到不可变工件、编译修订摘要、MCP 协议版本及 Harness 配置档。工作者尝试记录持续时间、验证及重试结果和关联 ID，将提供者、能力及主机托管调用关联到发起它们的尝试。两个 SQLite 存储都使用 WAL 模式和有界的忙等待超时。已有的版本 1 记录数据库会在打开时通过事务迁移到版本 3。目录在服务器重启后保留。任一存储缺失时，编译会拒绝 `host_brokered` 目录条目，避免工作流声明自己不具备的恢复能力。

自定义嵌入方可通过可选 `neograph::mcp_sqlite` 目标中的 `SqliteHarnessRecordStore` 构造相同后端。默认日志模式在写入 SQLite 前，递归地将常见秘密及内容字段替换为 `[REDACTED]`。`METADATA_ONLY` 丢弃所有事件负载；`FULL` 精确保留提供者内容、工具参数和结果，只应对已批准存储的数据启用。通过 `HarnessJournal::list_events(run_id, after_sequence, limit)` 可按运行顺序读取事件。偏好原子 JSON 文件的部署仍可使用 `FileHarnessRecordStore`；它不实现日志边界。

### 保留

SQLite 存储实现了可选的并列接口 `HarnessRetentionStore`；稳定的 `HarnessRecordStore` 虚函数表保持不变。保留工件或启动运行前，`HarnessService` 应用 `HarnessServiceConfig` 中的 `max_artifacts` 和 `max_runs`，两者默认值均为 128。

清理仅移除已进入终态的叶子运行。排队、运行中和等待输入的运行受保护，尚未完成日志收尾的进程内执行也受保护。重放或分叉记录保存 `source_run_id`，因此只要依赖记录仍被保留，就不能删除其源。需要释放空间时，先移除依赖叶子；只有之后不再有保留记录引用源时，源才可被删除。因此，当所有候选项都在活动、显式受保护或仍被引用时，容量限制是软限制。

在`runs.db`内，一个事务在删除运行行之前删除该运行的日志行，并且仅在没有任何运行引用某个工件之后才删除该工件。在该提交之后，Harness从单独配置的检查点存储中删除已删除运行的检查点线程。在第二阶段期间发生的崩溃或检查点后端故障可能留下不可达的检查点存储，但不可能留下指向已删除源记录的保留重放/分支。后续的管理或后端特定的孤儿清扫可能回收此类仅检查点的残留。

`FileHarnessRecordStore` 不实现持久清理；其历史性的内存中工件缓存驱逐和硬性运行容量行为保持不变。

## 调试器视图

`neograph_get` 保留 `status` 作为其紧凑默认视图，并增加四个调试器视图，而不添加另一个 MCP 工具：

| 视图 | 结果 |
|---|---|
| `attempts` | 日志化 worker 尝试的开始/完成/中断事件 |
| `trace` | 现有有序的 GraphEngine 节点追踪以及因果日志时间线 |
| `checkpoints` | 无负载的检查点元数据：ID、父级、节点、阶段、步骤和通道名称 |
| `diff` | 每个检查点和其父级之间发生变化的通道值和版本 |

`attempts` 和 `trace` 接受 `after_sequence` 作为不透明的前向游标。所有四个视图接受 `limit` 从 1 到 1000。返回的工件 URI 可以携带与查询相同的分页方式，例如：

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

这些 URI 中仅接受 `after_sequence` 和 `limit`。未知或格式错误的查询字段会失败而不是被忽略。日志支持的视图按持久化的方式返回负载，因此配置的掩码模式得以保留。`diff` 视图是从检查点存储而不是日志计算的，可能包含完整的通道值；将其访问视为与访问现有详细运行结果相同。

## 重放模式

`neograph_start` 可以在不添加另一个 MCP 工具的前提下重放一个已完成的运行：

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded` 使用源日志中已完成的工作者尝试结果，重新执行编译器锁定的图。它不调用已配置的工作者、提供者、MCP、A2A 或能力执行器。源工件修订、协议和配置档仍须匹配，日志必须使用 `FULL` 负载模式。`REDACTED` 和 `METADATA_ONLY` 日志缺少精确工作者输出，不能重放。重放还要求精确捕获的操作坐标、原始所有者/版本/输入权限，以及持久化的提供者预算账户保管证据；不兼容的旧保管证据会被拒绝，不能从令牌总量或尝试次数重建。

使用 `mode: "live"` 以实时提供方和工具执行相同的保留工件。快照和日志生命周期事件将运行标记为 `recorded_replay` 或 `live_replay`，并包含 `source_run_id`；普通启动保持 `live`。

记录重放分别认证源的原始不可变调用权限和剩余支出权限。源谱系上的一次比较并交换（CAS）将保留的剩余额度转移到重放运行；第二次重放不能再次使用同一源的剩余额度。已捕获调用复用历史操作坐标，不补充操作名额。重放中新执行的 Core 工作和墙钟时间消耗转移后的剩余额度。

提供者报告、已知费用、不确定预留和副作用标识仅被恢复一次，放入独立的仅供观察账户。回放不能预留、退款、结算、追加报告或调度新的副作用。失去执行的重放会被持久租约阻止继续执行，并保留已转移或不确定的额度；不能通过实时调度修复。运行快照暴露当前谱系剩余额度，包括额度已转移到重放的源。

`ChatMessage` / `ChatTool` 和 JSON 是可移植投影，不授予原生权限。可移植格式仍为 [`provider-message-v2`](../schemas/provider-message-v2.schema.json) 和 [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真实 C++ 检查点的附属对象在内存中保留原生封印。持久化原生历史需要由主机拥有的 `sp::NativeArchive`：封闭的 v3 / `spna3` 格式通过独立密钥认证所有者私有的保管证据。归档 v2 会被拒绝，不升级或解释。认证绑定每项语义描述符选择（来源、路径、请求头、策略、请求字段映射、用量路径及停止映射）、所有者及确切保管绑定。它不提供加密或厂商签发者认证；不得公开归档正文、密钥、原生二进制块或原始线路观测。归档存储证据，不授予资金或支出租约。Program 和外部预算账户仍由独立日志拥有；复制快照不能创造额度。

**独立预算账户日志修正：当前契约已修订，运行时证据见下文。** 所有者批准的协议要求可信存储命名空间中的单调义务记录，以及真实且不可变的原始所有者/线程/图作用域、上限、截止时间及其时钟标识和代次。只有针对完整检查点承诺值和修订执行精确的持久头部 CAS，才能发放主机拥有的不透明租约。精确的待处理副作用窗口必须在提供者 I/O 前持久化；结算必须使用真实 SDK 结果、实际费用、可空报告、冻结额度及去重标识。检查点和下一头部必须在同一拥有者执行体及修订下原子发布。删除预算元数据、裁剪检查点、重放旧的已认证快照、覆盖相同 ID 或失去执行体都不能授予额度。已有 65 冻结额度时，将上限从 130 降到 129 后不能再批准另一个 65；已证明没有副作用的失败可以释放未改变的头部，保留按真实 130 上限恢复的可能。崩溃、结果未知或租约丢失的窗口继续冻结额度，不退款、重试或回退。普通或初始归档配置不授予资金或原生支出租约，当前 `config.usage` 不能替换已有独立账户义务；Program 和外部账户的日志所有权保持不变。这是要求的契约；下文报告实际资金及保管证据和插桩限制，不保证稳定的已发布 API。

**当前声明，集成运行时证据见下文：** `<neograph/graph/checkpoint.h>` 声明 `ManagedBudgetLeaseScope`，包含 `owner_scope`、逻辑 `thread_id`、后端私有的 `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks` 和 `deadline_clock_identity`。`OwnedManagedBudgetLease` 暴露只读的 `scope()`、`actor_id()`、不可变的 `bank_generation()`、`revision()`、`head_checkpoint_id()` 和 `head_commitment()`；没有公开的权限导入构造函数。`ManagedBudgetEffectReceipt` 暴露 `active()`、`effect_id()`、`claim_amount()` 和 `request_digest()`；默认回执不授予任何权限。`CheckpointStore` 声明 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)` 和 `release_managed_budget_lease(lease)`，并提供对应的 `_async` 版本。同步 `CheckpointStoreCore` 和 `AsyncCheckpointStore` 暴露各自版本。`managed_budget_checkpoint_commitment(checkpoint)` 涵盖完整的持久检查点，不仅是预算 JSON。这些声明本身不能证明后端 CAS、资金安全、已安装 ABI 兼容性或成功执行的运行时路径。

**真实 InMemory 共享账户分叉已保留并验证。** 原始真实 C++ 分叉使用同一个原始财务日志和可信的当前分支头部，不复制资金授权。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 及其 `_async` 版本要求真实的当前源、完整承诺值和实际指向同一账户的原生 C++ 指针；持久化独立账户分叉仍明确不受支持。`OwnedManagedBudgetLease::scope()` 和原始所有者/线程/图、上限、截止时间及其时钟和代次保持不可变。存储发放的只读 `execution_thread_id()` / `execution_storage_thread_id()` 单独选择执行分支；`GraphState::budget_original_thread_id()` 标识原始财务账户。精确的所选分支头部 CAS 及全局执行体/修订将所有分支对规范当前计数、待处理副作用及已耗用标识的访问串行化。原始分支和分叉分支仍可使用，但不补充额度；过期快照、复制检查点和导入 JSON 不能创建别名或回退头部。原始 root30 → charge3 → original continuation6 → fork lower20 → continuation9 的同账户证明在未修改的 test_graph_engine.cpp:810–913 中通过；保存的原始上限 30 与分叉实际生效上限 20 分开；扩大到 31 或仅从 JSON 恢复必须被拒绝。无界报告观测是事实数据，不是有限资金授权。只有已证明零副作用的租约才能释放未改变的头部；未知或待处理副作用保留其义务。

**当前释放错误契约，已执行的套件及探针见下文。** `<neograph/graph/engine.h>` 中的 `graph::ManagedBudgetLeaseReleaseError` 继承 `ProviderOutcomeError`。`cause()` 保留原始执行异常，`release_error()` 暴露次要的持久租约处置失败。存在真实 SDK 证据时，`outcome()` 保留该证据；不存在 SDK 结果时为空。释放失败不能伪造结果或允许重新调度。封闭的 `_neograph_managed_budget_scope` 元数据描述原始逻辑作用域、上限、截止时间时钟及代次，但只是数据，不授予后端 CAS 权限。

**归档所有者及保留契约，已执行的套件及探针见下文。** 只有有限的独立根或已认证的有限源，才会从实际配置的 `sp::NativeArchive::owner_scope()` 继承省略的原始所有者；无界或普通所有者元数据语义不变。显式冲突的归档所有者在获取租约前被拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 及相应 Core/Async 存储能力默认返回 false；真实 InMemory 后端覆盖为 true，包装器必须委托实际保留能力。这项只读描述允许合法的无租约、普通或无界 C++ 原生检查点保管，不授予支出额度或原生重放权限。有租约的保管使用真实存储发放的回执，不依赖 JSON 标记或猜测存储类型。

**原生保管的 I/O 前检查，已执行的套件及探针见下文。** 开始受管理副作用前，必须具备真实绑定的 NativeArchive，或实际由本地存储发放的私有 C++ 保留能力；此检查发生在任何待处理副作用、名额或冻结窗口变更之前。私有能力不能从 JSON 导入，也不经线路传输。即使远程后端是 InMemory，gRPC 仍要求真实的客户端及服务器归档，因为 C++ 附属对象不能跨越该边界。没有归档提供有限源所有者时，原始匿名所有者作用域保持为空；真实归档绑定必须匹配原始作用域。财务头部及租约证据本身不能证明原生保管已就绪。

`ProviderOutcomeError` 是保留结果的共同主机错误基类；`ProviderObserverError` 和 `ProviderDispatchOutcomePersistenceError` 保留完整读取后的 SDK 结果及原始 `cause()`。持久化错误还在 `delivery_error()` 中保留次要观察者失败。`ProviderFailure::outcome()` 保留 SDK 失败本身。这些证据不授权 Node/Program 重新调度：SDK 独自负责 transport retry，调用方选定的 `max_output_tokens` 不会被静默降低。更大 cap 的 semantic call 需要新 prepared digest、独立确定性 call ordinal、原 resource bank 的 admission 和原始 deadline；native replay eligibility 不续期 credit。

`ProgramFailure` 保留实时 `provider_outcome` 和 `provider_cause`。其规范的事实性 SDK 见证将真实归档保管绑定到所有者/运行/版本/包/操作/尝试；Runtime 在暴露恢复后的失败前立即恢复已配置的保管证据。公开且仅承载数据的 `ProgramResult::create()` 不能通过预填见证绕过检查，未解析完成的封印也不是可执行结果。进程重启后，原始异常指针不可用（`provider_cause == nullptr`），不能从文本重建。无法持久化的失败不能序列化、发布或重放。

`RecordedBindingSet` 是绑定到源且只能移动的数据，不是调用方提供的调度器。可信 Catalog 的 `recorded_capability_binder` 独立地从真实持久源事件中构造仅供捕获回放的能力。`ProgramRuntime::replay_recorded()` 检查原始所选源权限，再通过持久 CAS 转移实际剩余预算账户；继承的支出额度不代表新的模型授权。旧 `start_recorded` 续期 API 已删除。InMemory、File、SQLite 和 PostgreSQL Program 存储在整个执行期间保留精确、不可变且拥有明确所有者的租约；到期不会续期。受控 JavaScript 仍验证底层能力清单，消费精确的已完成命令结果，不重新调度外部副作用。

**记录控制流的因果修复已在完整套件中验证。** 捕获命令重放在执行前仅为新的 CPU 墙钟时间/Core 工作建立持久预留，再通过结果 CAS 发布测得的工作量及新产生的 Core 检查点。它不消耗新的模型、资金或 Program 操作额度，也不重新调度捕获的外部副作用。未核销的预留保持扣减。预留选择已认证的结算状态转换，不使用曾拒绝首个新 Core 检查点的普通 Running→Running 转换。Await 通道接收、定时器等待/取消和交接等待的发起及释放都在各自所属执行器/strand 上串行化；既有 Recorded CPU/Memory await/handoff 场景在完整套件中通过，远程 TSan 覆盖限制仍明确记录在下文。

以下付费、原生轴及集成运行时观测来自提供者切换的历史批次。其计数、失败、跳过及限制保持不变。保留记录中的“最新”指该批次最后一次运行，不代表已验证当前 Python 绑定或此次文档修改。

**付费观测已完成，但不构成普遍资格验证。** 原始 `SPQUAL1` 基础额度 630/1000000 microUSD 不变；同一原始账本中的一个哈希链 `A` 事件接纳已批准的扩展 480/3000000，总计 1110/4000000。调用次数、支出、冻结和结算累积，不创建新的授权 ID、头部或重置。精确声明字节及文件标识、原始授权/基线/目录/激活/账本前缀哈希与总量仍固定；删除、替换或变更都将被拒绝。最终规范账本为 calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；支出加冻结共 US$1.725786，是本地目录计量值，不是账单。记录的五族、每族 60 对基线完成 600 个请求：Chat60/60、Responses60/60、Messages60/60、Generate56/60（四次 SSE 视觉结果错误）、Interactions57/60（一次缓冲式、两次 SSE 视觉结果错误）；合计 293/300 对，不是 300/300。其他旧的 600 次调用财务记录仍保留，但不是完整行为证明。此前 M5/媒体单次批次不变。此前三轮 Google 前置条件保留两次工具无效和一次正向样本不可读的失败。不批准更多付费调用。最终 SDK 证据及原生轴限制与基线成功分开记录。此前激活/重新打开冒烟记录在两次重新打开后为 calls610/spent219159/held751233，SDK 计量/canary/视觉四项测试以 19.38 秒通过；这些是限定范围的历史检查点，不是最终账本总量。此前已验证的 Chat60 对批次保留 120 次实际尝试、120 次 UpperBound 计费且没有 UnknownHold。

**原生轴观测不证明密码学验证、原生消费或等价性。** Generate 接受修改、遗漏和重复。Interactions 接受隔离的真实源及正向对照、单所有者签名修改、思考载体遗漏、调用载体遗漏和重复。删除所有思考/签名返回通用 400；保留 THOUGHT 项但删除全部签名字段也返回通用 400。最后一次捕获只有本地编码原件保留对照，没有同次捕获的服务器正向对照；此前正向批次仍是真实证据。这些观测只建立所有载体均缺失的边界，不证明签发者/签名验证或厂商消费。实际报告为 SDK 的 `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json` 和 `qualification-signature-presence-results.json`；前置条件失败、未运行及负向结果不确定的状态均按事实保留。仍有其他载体时，仅遗漏思考或单类载体的请求被接受；这不加强签发者验证或原生消费声明。

**实际集成证明及剩余限制。** 最新 Core 完整运行有 2242 项测试、零失败、16 项跳过（14 项 RAM 进程丢失场景不适用、两项实时凭证准入），耗时 130.17 秒。`PgNestedJsonRoundTrips` 在 0.18 秒内精确保留重复键、顺序、空元数据、二进制块及残余数据。未修改的原始共享账户分叉及既有 Recorded CPU/Memory await/handoff 场景通过。真实 wrappedMemory/SQLite/PostgreSQL/gRPC 的 finite130/hold65/lower129/strip/old-head/pruning/no-archive/import 探针在普通构建和 ASan+UBSan 下通过。本地 Memory/SQLite/PostgreSQL TSan 范围有七项通过、零警告。完整混合 gRPC 加系统 Abseil/Protobuf TSan 以 66 退出，在依赖及生成的 RPC 栈中产生 402 项竞态警告：这是插桩及覆盖限制，不是已证明的误报；不声称远程 TSan 已通过或无竞态，也不压制警告。已安装 find_package 的 Program C++/C ABI/dualQuickJS 三个使用方通过。全新安装的 NeoGraph/SchemaProvider 类型化使用方通过两次真实 HTTP 请求、协程启动前销毁提供者、原生及工具重放、拒绝、已知零值及原始数据保留，以及实际 LinkedMismatch 拒绝。浏览器 Alice/Bob 隔离及第 2 代替换已通过目视验证；PostgreSQL Program Chat 六项黑盒测试以 18.989 秒通过。最新 SDK26/26 通过、零失败、耗时 74.07 秒。最终 ReleaseGraph 的 16 种配置各在三个新进程中重复运行，共 48 条记录，以 38.29 秒完成，零失败，全部实际协议及具有所有权的结果检查通过。NeoGraph 的 `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保留这个独立最终批次。测量期间没有运行编译器或付费模型；历史批次保持不变，不声明语义或资源等价性。不稳定 SDK/ABI3 不构成稳定发布或更广的平台资格验证。

## 兼容分叉

先编译修复后的Harness，然后通过现有的`neograph_start`工具将一个确切的前置检查点分支到该目标工件中：

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

源检查点必须属于 `source_run_id`。分配运行前，Harness 对照目标工件验证检查点模式、源修订、MCP 协议、Harness 配置档、每个恢复的通道及归约器、每个后续节点和任何活动屏障接口。不兼容的分支返回 `started: false`、`status: "incompatible_fork"`，以及包含 `path` 和 `witness` 的机器可读 `H_FORK_*` 诊断；不会创建运行或分叉检查点。

检查点存储是必需的。没有记录存储，分支可能仅引用当前服务进程内仍驻留的源运行和工件；为必须跨重启存活的分支血统配置两种存储。

兼容分支标记为 `compatible_fork`，并在启动响应、快照及生命周期日志事件中携带 `source_run_id` 和 `source_checkpoint_id`。执行从所选检查点的 `next_nodes` 恢复；已提交的前驱不再执行。目标工件提供修复后的拓扑、工作者契约和工具目录，恢复的通道值（包括原始任务通道）则来自源检查点。任务输入本身需要改变时，应重新启动而非分叉。

源运行、工件和所选检查点是分支的引用，且必须在兼容性检查或分支执行可能使用它们时保持留存。保留清理必须先移除依赖项或保留引用源；它绝不能在前置检查与分支创建之间删除源检查点。

## Streamable HTTP

远程传输是选择加入的，因此现有的stdio-only目标保持紧凑，且不会静默获得HTTP/OpenSSL依赖：

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

端点是`http://127.0.0.1:8080/mcp`。它实现已发布的MCP 2025-11-25 Streamable HTTP POST契约，采用每会话MCP生命周期和JSON响应。通知返回HTTP 202。DELETE终止会话。可选的独立GET/SSE通道有意不实现并返回HTTP 405，而传输规范明确允许这一点。

安全默认值位于传输层，且不会将身份验证与 `GraphEngine` 或 `HarnessService` 耦合：

- 默认绑定是 `127.0.0.1`；除非配置了 bearer 授权器，否则拒绝非回环绑定。
- 提供的每个 `Origin` 都会遭到拒绝，除非它与 `NEOGRAPH_HARNESS_ALLOWED_ORIGINS`（可执行文件中为逗号分隔）中的条目完全匹配。
- `NEOGRAPH_HARNESS_BEARER_TOKEN` 启用可执行文件的单主体 bearer 边界。库嵌入可使用 `MCPHttpServerConfig::bearer_authorizer` 进行 OAuth/JWT 验证，并返回稳定的主体/作用域。
- 会话与返回的授权作用域绑定。不同的有效主体无法重用泄露的 `Mcp-Session-Id`。
- `MCPHttpServer` 工厂接收该已验证作用域，并返回 `MCPHttpServerSession` 所有者。多租户嵌入必须使用该作用域选择隔离的 Harness 记录/检查点存储；无任何身份验证状态进入图运行时本身。
- 请求有效负载、HTTP 工作线程、队列、会话以及响应等待限制均受 `MCPHttpServerConfig` 约束。

`neograph::mcp::ScopedHarnessStore` 将公开 ID 和由模式定义的记录及日志引用映射到可逆的租户命名空间；不改写不透明的请求、结果或事件负载。File 和 SQLite 存储接受这些私有 ID。过长的 File 键使用定长哈希文件名，不改变短键路径。SQLite 保留机制在一个事务中将计数和删除候选项限定到所选命名空间，保护其他租户及受保护的源引用。这项存储边界不能代替已认证作用域或应用的提供者、工具及配额策略。

对于任何非回环部署，在受信任的反向代理处终止TLS，并使用其OAuth/OIDC验证或等效的 `bearer_authorizer`。转发原始的 `Authorization` 和 `Origin` 头，不要暴露明文公共监听器，并为每个Harness状态目录部署一个授权域。

## 主机设置

将已安装的 `neograph-harness-mcp` 二进制文件的绝对路径设为 `SERVER`。以下条目选择直接 Provider 执行器；若使用上述本地主机模式，应改为对应的 `--executor claude|codex|opencode`。

```bash
SERVER=/absolute/path/to/neograph-harness-mcp
```

Claude Code，本地项目范围：

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER" --executor provider
claude mcp get neograph-harness
```

Codex CLI：

```bash
codex mcp add neograph-harness -- "$SERVER" --executor provider
codex mcp list
```

通过非交互式 `codex exec` 使用这个受信任的本地服务器时，在 Codex 的 `config.toml` 中设置 `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"`。没有此设置，Codex 会正确地取消 `neograph_compile`，因为保留工件没有被标注为只读。交互式会话可以保留默认确认提示。

OpenCode，在项目 `opencode.json` 或用户配置中：

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

使用 `opencode mcp list` 验证。这些形式遵循每个主机官方的 MCP 配置契约，于 2026-07-21 审核。

## PR 审核工作流

让主机使用其常规仓库工具收集 PR diff，然后使用 Harness 工具。合适的请求是：

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

### 提供方预算

`budgets.provider_timeout_seconds` 将一次已准备的提供者操作的绝对截止时间设为 1–600 秒。`budgets.max_output_tokens` 将其输出上限设为 1–128000 个令牌。两者均为可选：省略时不设置对应控制项，沿用已获准的 SDK 或默认策略，并不意味着时间或输出无限。SDK 负责已准备操作截止时间内的所有提供者重试。

工作者可以将任一字段设为更小的值。高于 Harness 全局值的工作者设置会在编译时被拒绝。到达截止时间时，Harness 只取消传给该提供者调用的子取消令牌，不取消同级工作者或外层运行。提供者必须遵守令牌，因此不可中断的提供者可能在截止时间后才返回。

宿主应遵循此顺序：

1. 调用`neograph_compile`，若`ok`为假则停止。
2. 调用`neograph_start`并传入返回的`artifact_id`。
3. 使用 `run_id` 轮询 `neograph_get`；它只返回结果和计数。
4. 需要详情时，调用 `neograph_get`，传入相同 `run_id`，并将已返回的 `neograph://runs/...` URI 作为 `uri`。默认不要将追踪信息拉入上下文。

### 发现项来源

详情工件在 `workers` 中保留每个通过模式验证的工作者响应，并为现有客户端保留既有的扁平 `findings` 数组。`finding_sources` 是等长的并行数组：每项包含聚合后的 `finding_index`、源 `worker_id` 和该工作者的 `local_index`。用它识别重复本地 ID（如 `F1`）的来源，不要向工作者声明的发现项对象添加来源字段。

## 主机托管的恢复

当 MCP 主机而非工作者进程拥有某项能力时，使用 `executor.kind: "host_brokered"`。将 `executor.interaction` 设为 `"tool_result"`（默认）或 `"input"`。提供者执行器验证请求参数，然后返回以下两种非终态之一：

- `awaiting_tool_results`: 宿主必须执行指定的能力。
- `input_required`：主机必须收集输入值。

`neograph_get` 包括一个 `pending` 对象，具有唯一的 `call_id`, `tool_id`、经过验证的 `arguments`以及 `result_schema`。通过以下方式精确提交该调用：

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume` 拒绝不匹配的调用 ID、不符合声明模式的结果、已过期调用，以及非等待运行的迟到结果。完全相同的重复提交会被确认，不重新执行图；冲突的重复提交会被拒绝。已接受的恢复意图在调度执行前持久化，因此进程崩溃后的轮询可从 `NodeInterrupt` 检查点重新开始恢复，不重复执行已成功的同级工作者。

### 外部效应与对账

普通主机托管契约保持向后兼容：没有 `executor.effect` 的目录条目在进程重启后仍保持 `awaiting_tool_results`，并接受同样的 `{run_id, call_id, result}` 恢复请求。

对于能够产生外部可见、非幂等变更的主机能力，请明确声明该风险。效果元数据仅在默认的 `host_brokered` `tool_result` 交互下有效；它并非输入采集元数据。

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

待处理的调用随后包含一个持久的`effect`对象。其`effect_id`和`idempotency_key`的范围限定于Harness运行，并与提供商的工具调用ID不同。`status_query`和`fencing`描述主机能力；Harness记录它们，但不发明提供商特定的查询或重试协议。

如果服务在 `idempotency: "unsupported"` 调用仍处于等待状态时重新连接，则仅将该次运行更改为 `ambiguous_effect`。这意味着宿主可能在进程停止前已执行了该效果，但 Harness 无法证明任何一种结果。紧凑状态包括 `pending` 和 `ambiguity`，日志记录 `host_brokered.effect.ambiguous`，并且 Harness 既不重放该工具，也不将该效果报告为失败或已完成。

通过`neograph_resume`在宿主检查其自身权威系统后解决歧义：

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed` 验证并消费 `result`，然后从检查点恢复。`failed` 记录终态 Harness 失败，不再次执行工作者。`unknown` 让运行保持 `ambiguous_effect`，等待后续对账。完全相同的 completed、failed 或 unknown 重复提交是幂等的；冲突的 completed 或 failed 提交会被拒绝。每次非重复对账都以 `host_brokered.effect.reconciled` 记录到日志。

结果不明的副作用不能取消，也不会过期。取消或超时无法确定外部副作用是否发生；权威系统暂时无法判定时，应提交 `unknown`。

本协议不保证主机崩溃时仍能实现恰好一次投递。支持幂等键或状态查询的主机应先通过这些系统确定真实结果，再提交对账。

运行快照包括 `created_at`、`updated_at`、`expires_at` 以及 `poll_after_ms`。默认 TTL 为 24 小时，默认轮询间隔为一秒；这两项均可通过 `HarnessServiceConfig` 覆盖。

## 实验性 Tasks 配置档

MCP Tasks 不属于核心 MCP 2025-11-25 的一部分，且上游扩展仍将其标记为实验性。因此，NeoGraph 默认将其禁用，并将其与稳定的 `run_id` 及 `neograph_get` 轮询契约分开。

要在示例服务器上显式启用，还必须启用持久状态：

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

服务器随后通告 `io.modelcontextprotocol/tasks`，为 `neograph_start` 标记可选任务支持，并提供 `tasks/get`、`tasks/update` 和 `tasks/cancel`。只有单个 `tools/call` 请求包含以下内容时，才返回 `CreateTaskResult`：

```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

未通过请求显式启用的客户端收到普通 `CallToolResult`，继续轮询 `neograph_get`；启用此配置档不会改变稳定的回退行为。任务状态为 `working`、`input_required`、`completed`、`failed` 和 `cancelled`。`tasks/update.inputResponses` 以待处理的 `call_id` 为键，轮询客户端应遵守 `pollIntervalMs` 和 `ttlMs`。

## 能力后端

`make_provider_harness_executor` 通过任意 NeoGraph `Provider` 驱动工作者。模型请求已声明工具时，执行器在调度前后按照目录验证参数和输出。

提供者执行器构造类型化的 `ProviderRequest`；`prepare` 进行验证和编码，不执行 I/O，`dispatch` 消费准备好的请求。`invoke` 合并这两步。执行器保留具有所有权的 SDK Completion/Failure 结果，包括有序消息及其组成部分和部分失败，不将失败转换成纯文本成功。提供者报告通过可空字段保留未知用量；保守的令牌计费及预留属于独立权限。记录提供者结果的回放不会再次计费或结算这些结果；重放中新执行的 CPU/Core 工作仍消耗转移后的剩余额度。

对于已初始化的下游 `MCPClient` 实例，使用 `make_mcp_harness_capability_executor`；对于 A2A 智能体，使用 `a2a::make_harness_capability_executor`。请求仍决定权限：工作者只能看到自己 `tools` 数组中列出的工具 ID。

对于文件系统工具，在 `path_arguments` 中声明每个包含路径的输入，并设置 `policy.workspace_roots`。相对路径在第一个根目录下解析；在任何配置的根目录之外的规范路径在分派前被拒绝，包括通过现有符号链接的逃逸。规范路径被传递给能力后端，而不是模型提供的拼写。下游 MCP 和 A2A 服务仍然是独立的信任边界，应强制执行相同的根策略以消除文件系统检查时间/使用时间竞态。使用 `policy.read_only: true`，编译会拒绝每个未标记为 `read_only: true` 的目录条目。

## 分发与协议配置档

支持的本地分发路径是上述可安装的 `neograph-harness-mcp` 二进制文件。源码构建可以继续使用示例目标，Python wheel 仍然是库/运行时包，而不是隐式安装远程守护进程。MCPB 和官方注册表发布仍然是发布/发现打包选项；它们不是线路协议所必需的，只应在带有签名发布工件和显式远程认证部署清单的情况下添加。

NeoGraph 目前仅发布带日期的 MCP `2025-11-25` 配置档。描述未来无状态协议的最终 SEP 不会创建新的线路版本；MCP 项目发布新的带日期规范之前，不会通告后继配置档。
