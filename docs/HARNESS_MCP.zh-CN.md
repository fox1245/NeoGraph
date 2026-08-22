<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=zh-CN source_sha256=ca4698088011b7213d7e665a73d15a483aae48056f7a18dd2fc504a92f589ed6 -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harness 在运行之前编译一个有界的多工作者工作流。稳定的 MCP 表面保持在六个工具：

- `neograph_schema` 发现已安装的请求契约和预设。
- `neograph_compile` 编译并验证而不执行。
- `neograph_start` 启动一个保留的工件或一个内联请求。
- `neograph_get` 轮询紧凑状态或解引用一个结果工件 URI。
- `neograph_resume` 验证并提交确切的挂起主机结果。
- `neograph_cancel` 协作地取消一个排队、运行或等待的工作流。

已发布的预设是 `fanout_judge`、`pr_review_panel`、`bug_triage` 和 `research_synthesis`。预设生成普通的严格 Core 图工件；JavaScript 请求保留其自身的 `ProgramSource` 封装和源映射。

### JavaScript 开发边界

`harness.mode` 接受 `preset` 或 `javascript` 用于新发布。JavaScript 请求在 `harness.source` 中携带源文本，并可能固定 `harness.source_id`：

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

翻译器将该文本包装在规范的 `ProgramSource` JavaScript 信封中（语言 `javascript`、QuickJS 引擎、冻结的主机 API、导入和源映射），并通过 `ProgramCompiler`、`ProgramCatalog` 和 `ProgramRuntime` 发送。`define()` 通过密封的 `ng` 绑定构造一个图；可选的生成器 `main()` 拥有普通控制流并产生现有的类型化 Program 命令。JavaScript 不调度 Core 节点、选择提供者/工具，或绕过准入(admission)、预算、日志或重放。

被评估的模块还选择结果契约。仅导出 `define()` 的源保留 Core 根契约，包括 `channels.final_result.value` 包装器。改为导出运行时 `main(input)` 的源则直接针对 Harness 结果模式宣传并验证其终端返回。

#### 控制流迁移示例

将 `define()` 编译时和每个运行时效果保持在产生的类型化命令之后。此完整请求给生成器三个操作——`ng.all` 连接及其两个 Core 调用——以及双向并行。其工作节点完全重复从请求中密封的配置，其终端返回具有 Harness 结果形状：

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

稳定的源站点字符串是持久命令坐标的一部分。在重试和重启之间保持它们确定性。当第一批必需成功应获胜时使用 `ng.any(...)`，当第一个终端成员应获胜时使用 `ng.race(...)`；两者都通过结构化并发取消未完成的兄弟任务。环境 I/O、定时器、动态加载、`eval` 和原生句柄仍然不可用。

`harness.mode` 必须是显式的。`dsl` 返回 `H_MIGRATION_CORE_DSL`，`core` 返回 `H_MIGRATION_CORE_JSON`，而 `program`/`program_json` 返回 `H_MIGRATION_PROGRAM_JSON`；所有都指向 `/harness/mode`，并且永远不会从请求的 JSON 形状或缺失字段中选择。严格的 Core JSON 仍然是已验证 Core 和 Program 工件的内部/交换表示，受信任的 C++ 进程内构造仍然受支持；两者都不是公开的 Harness 编写语言。

Schema 导出、编译和启动现在都使用相同的不可变 `HarnessAdmissionProfile`。其作用域内的 `GraphRegistry` 和清单（manifest）列出每个可用的节点、reducer 和条件，以及实现、降级和兼容性元数据。进程全局注册表条目不属于此 palette，不能被 Harness 准入(admission)解析。编译在已验证的声明性 `TopologySpec` 处停止，因此被拒绝的输入构造不会创建 `GraphNode`，也不会调度 worker 或副作用。保留的工件绑定 profile ID 和指纹；不同或 profile 之前的工件在启动/恢复时失败时关闭（fail closed），而不是被重新解释。

C++ 嵌入者在构造时通过非默认的 profile 传递 `HarnessServiceResources`。这个添加的资源边界保持了现有 `HarnessServiceConfig` 布局。Profile 指纹涵盖 manifest 和作用域注册表导出的语义投影。每个 `implementation_identity` 都是受信任的声明，每当对应的可调用行为发生变化时必须随之更新。

这是当前的 Program 支持的 Harness 兼容性适配器。接受的 Harness 请求仍然转换为 legacy `ProgramSource`，通过 `ProgramCompiler` 编译，通过 `ProgramCatalog` 准入，并通过 `ProgramRuntime` 执行；`GraphEngine` 仍然是唯一的节点执行器。

对于通用编写，接受的替代方案是嵌入式 QuickJS 上的标准 JavaScript。之前的 `dsl`、独立 `core` 和 `program` 模式将被拒绝用于新的发布，并带有明确的迁移诊断；严格的 Core JSON 保持内部/交换数据。参见 [`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) 和 [`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md)。本文档描述了保留的兼容行为和迁移诊断；它不会为新的 legacy 源语义授权。

## 构建与运行

使用 OpenAI 兼容的提供者适配器构建本地 stdio 服务器：

```bash
cmake -S . -B build-harness \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON
cmake --build build-harness --target example_harness_mcp_server -j
export OPENAI_API_KEY=your-key
export NEOGRAPH_HARNESS_MODEL=gpt-4o-mini
```

`NEOGRAPH_HARNESS_API_KEY` 优先于 `OPENAI_API_KEY`。`NEOGRAPH_HARNESS_BASE_URL` 选择任何 OpenAI 兼容的端点。服务器同时接受无版本号的 base，如 `https://openrouter.ai/api`，以及提供者文档中记录的版本化形式，如 `https://openrouter.ai/api/v1`；仅当缺失时才添加 `/v1`。服务器仅将协议消息写入 stdout，将诊断信息仅写入 stderr。请参阅 [OpenRouter quickstart](https://openrouter.ai/docs/quickstart) 获取其当前端点格式。

仅用于宿主互操作冒烟测试，设置 `NEOGRAPH_HARNESS_SMOKE=1`。这种显式模式使用进程内确定性的提供者，返回一个有效的零发现审查，要求无 API 密钥，并且绝不能用作 LLM 质量测试。

持久化宿主代理的调用需要同时具备记录和检查点的持久化。示例通过一个显式目录实现了两者：

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

该目录存储不可变工件、可变运行记录，以及 `runs.db` 中的追加型因果日志，以及 `checkpoints.db` 中的图检查点。日志行和每个 Harness 创建的检查点将运行绑定到其不可变工件、编译修订摘要、MCP 协议版本和 Harness profile。Worker 尝试包括持续时间、验证/重试结果，以及关联 ID，用于加入提供者、能力和宿主代理调用到发出它们的尝试中。两个 SQLite 存储都使用 WAL 模式和有限的忙超时。在打开时，版本 1 的记录数据库会事务性地迁移到版本 3。该目录在服务器重新启动后存活。当任一存储缺失时，`host_brokered` 目录条目在编译时被拒绝，因此工作流不能宣传它所没有的可恢复性。

自定义嵌入可以通过 `SqliteHarnessRecordStore` 从可选的 `neograph::mcp_sqlite` 目标构造相同的后端。默认日志模式会递归地将常见密钥和内容字段替换为 `[REDACTED]` 之后才让 SQLite 看到它们。 `METADATA_ONLY` 丢弃所有事件负载； `FULL` 精确保留提供者内容、工具参数和结果，并且只应针对已批准存储的数据启用。事件可以通过 `HarnessJournal::list_events(run_id, after_sequence, limit)`. `FileHarnessRecordStore` 持续可供偏好原子 JSON 文件的部署使用，它不实现日志边界。

### 保留

SQLite 存储实现了可选的 `HarnessRetentionStore` 兄弟接口；稳定的 `HarnessRecordStore` vtable 保持不变。在保留工件或启动运行之前， `HarnessService` 应用 `max_artifacts` 和 `max_runs` 从 `HarnessServiceConfig`。默认值各为 128。

清理仅移除终端叶子运行。排队、运行中及输入等待的运行受保护，进程内执行尚未完成日志最终化的运行也如此。重放或派生行记录 `source_run_id`，因此在其依赖项仍受保护时，其源不能移除。如果需要空间，则先移除依赖叶；源仅在其后，在没有保留行引用后才可被移除。因此，当每个候选项都在活动、显式受保护或仍被引用时，限制是软性的。

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

`recorded` 使用源日志的已完成 worker 尝试结果重新执行编译器锁定的图。它永远不会调用配置的 worker、提供者、MCP、A2A 或能力执行器。源工件修订版、协议和配置文件必须仍然匹配，且日志必须使用 `FULL` payload 模式。`REDACTED` 和 `METADATA_ONLY` 日志刻意无法重放，因为它们不保留精确的 worker 输出。中断的尝试被丢弃；已完成的恢复后 worker 调用会被重放。

使用 `mode: "live"` 以实时提供方和工具执行相同的保留工件。快照和日志生命周期事件将运行标记为 `recorded_replay` 或 `live_replay`，并包含 `source_run_id`；普通启动保持 `live`。

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

源检查点必须属于 `source_run_id`。在分配运行之前，Harness 会对照目标工件验证检查点架构、源修订版本、MCP 协议、Harness 配置文件、每个恢复的通道和 reducer、每个延续节点以及任何活动屏障接口。不兼容的分支返回 `started: false`, `status: "incompatible_fork"`、机器可读的 `H_FORK_*` 诊断信息，其中包含 `path` 和 `witness`；它不会创建任何运行或分叉检查点。

检查点存储是必需的。没有记录存储，分支可能仅引用当前服务进程内仍驻留的源运行和工件；为必须跨重启存活的分支血统配置两种存储。

兼容的分支标记为`compatible_fork`，并在启动响应、快照和生命周期审计事件中携载`source_run_id`和`source_checkpoint_id`。执行从所选检查点的`next_nodes`恢复；已经提交的前驱不会再次运行。目标工件提供修复的拓扑、worker契约和工具目录，而从源检查点恢复的通道值（包括原始任务通道）来自源检查点。当任务输入本身必须更改时，使用全新启动而不是分支。

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

对于任何非回环部署，在受信任的反向代理处终止TLS，并使用其OAuth/OIDC验证或等效的 `bearer_authorizer`。转发原始的 `Authorization` 和 `Origin` 头，不要暴露明文公共监听器，并为每个Harness状态目录部署一个授权域。

## 主机设置

为 `SERVER` 使用绝对路径：

```bash
SERVER=/absolute/path/to/build-harness/example_harness_mcp_server
```

Claude Code，本地项目范围：

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER"
claude mcp get neograph-harness
```

Codex CLI：

```bash
codex mcp add neograph-harness -- "$SERVER"
codex mcp list
```

对于非交互式 `codex exec` 针对此受信任的本地服务器，设置 `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"` 在 Codex 中 `config.toml`。没有它，Codex 会正确地取消 `neograph_compile` ，因为保留工件未被标注为只读。交互式会话可以保留默认提示。

OpenCode，在项目 `opencode.json` 或用户配置中：

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "neograph-harness": {
      "type": "local",
      "command": ["/absolute/path/to/example_harness_mcp_server"],
      "enabled": true,
      "environment": {
        "OPENAI_API_KEY": "{env:OPENAI_API_KEY}",
        "NEOGRAPH_HARNESS_MODEL": "gpt-4o-mini"
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

`budgets.provider_timeout_seconds`将一次提供商完成尝试限制为1–600秒。`budgets.max_output_tokens`将一次完成限制为1–128000个生成令牌。两者均为可选：省略任一将保留先前行为，即无Harness截止时间及提供商现有输出限制。

工作线程可以将任一字段设置为更小的值。工作线程的值高于 Harness 全局值将在编译时被拒绝。在截止时间时，Harness 仅取消提供给该提供方调用的子取消令牌；它不会取消同级工作线程或外层运行。提供方必须尊重该令牌，因此无法被中断的提供方可能会在截止时间后返回。

宿主应遵循此顺序：

1. 调用`neograph_compile`，若`ok`为假则停止。
2. 调用`neograph_start`并传入返回的`artifact_id`。
3. 轮询 `neograph_get` 使用 `run_id`；这仅返回结果和计数。
4. 如果需要详细信息，请调用 `neograph_get` 使用相同的 `run_id` 并返回一个 `neograph://runs/...` URI 作为 `uri`。默认不要将跟踪信息拉取到上下文中。

### 查找溯源 (Finding Provenance)

详细信息工件在`workers`中保留每个通过模式验证的worker响应，并为现有客户端保持已建立的扁平`findings`数组。`finding_sources`是等长并行数组：每个条目包含聚合`finding_index`、源`worker_id`以及该worker的`local_index`。使用它来识别重复本地ID（如`F1`）的来源；不要向worker声明的发现对象添加溯源字段。

## 主机托管的恢复

使用 `executor.kind: "host_brokered"` 当MCP主机（而非工作进程）拥有某项能力时。将 `executor.interaction` 设置为 `"tool_result"` （默认值）或 `"input"`。提供方执行器验证所请求的参数，并返回两种非终止运行状态之一：

- `awaiting_tool_results`: 宿主必须执行指定的能力。
- `input_required` 主机必须收集一个输入值。

`neograph_get` 包括一个 `pending` 对象，具有唯一的 `call_id`, `tool_id`、经过验证的 `arguments`以及 `result_schema`。通过以下方式精确提交该调用：

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume` 拒绝不匹配的调用 ID、违反声明模式的结果、已过期的调用，以及针对非等待运行的迟到结果。相同的重复项被确认时不重新执行图；冲突的重复项被拒绝。已接受的恢复意图在执行调度之前持久化，因此进程崩溃后的轮询会从 `NodeInterrupt` 检查点重新启动恢复，而不会重复执行成功的兄弟Worker。

### 外部效应与对账

普通的宿主中介契约是向后兼容的：一个没有 `executor.effect` 的目录条目在进程重启后仍然 `awaiting_tool_results` 保持，并接受相同的 `{run_id, call_id, result}` 恢复请求。

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

`completed` 对 `result` 进行语义验证并消费之，然后从检查点恢复。`failed` 在不再次执行工作器的情况下记录终端 Harness 失败。`unknown` 将运行留在 `ambiguous_effect` 中，以便后续对账。完全重复的已完成、失败或未知提交是幂等的；冲突的已完成或失败提交会被拒绝。每个非重复的对账都作为 `host_brokered.effect.reconciled` 记录在日志中。

模糊效果刻意不可取消且不会过期。取消或超时无法确定外部效果是否已发生；若权威系统尚无法解决，则提交 `unknown`。

本协议不声称在主机崩溃时实现 exactly-once 投递。支持幂等键或状态查询的主机应使用这些系统来确定真实结果，然后再提交核对。

运行快照包括 `created_at`、`updated_at`、`expires_at` 以及 `poll_after_ms`。默认 TTL 为 24 小时，默认轮询间隔为一秒；这两项均可通过 `HarnessServiceConfig` 覆盖。

## 实验性 Tasks Profile

MCP Tasks 不属于核心 MCP 2025-11-25 的一部分，且上游扩展仍将其标记为实验性。因此，NeoGraph 默认将其禁用，并将其与稳定的 `run_id` 及 `neograph_get` 轮询契约分开。

要在示例服务器上选择启用（opt in），还必须启用持久状态：

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

服务器随后通告 `io.modelcontextprotocol/tasks`，标记 `neograph_start` 支持可选任务，并提供 `tasks/get`, `tasks/update`和 `tasks/cancel`。它仅在单个 `CreateTaskResult` 请求包含以下内容时才返回 `tools/call` ：

```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

未选择该请求选项的客户端会收到普通的 `CallToolResult` 并继续轮询 `neograph_get`；启用该配置文件/描述档不会改变稳定的回退。任务状态为 `working`, `input_required`, `completed`, `failed`及 `cancelled`. `tasks/update.inputResponses` 以挂起的 `call_id`为键，轮询客户端应遵循 `pollIntervalMs` 以及 `ttlMs`.

## 能力后端

`make_provider_harness_executor`通过任何NeoGraph `Provider`驱动workers。如果模型请求已声明的工具，执行器将在调度前后根据目录验证其参数和输出。

使用 `make_mcp_harness_capability_executor` 初始化下游 `MCPClient` 实例，或使用 `a2a::make_harness_capability_executor` 用于 A2A 智能体。请求仍然是权威：工作进程只能看到其 `tools` 数组中列出的工具 ID。

对于文件系统工具，在 `path_arguments` 中声明每个包含路径的输入，并设置 `policy.workspace_roots`。相对路径在第一个根目录下解析；在任何配置的根目录之外的规范路径在分派前被拒绝，包括通过现有符号链接的逃逸。规范路径被传递给能力后端，而不是模型提供的拼写。下游 MCP 和 A2A 服务仍然是独立的信任边界，应强制执行相同的根策略以消除文件系统检查时间/使用时间竞态。使用 `policy.read_only: true`，编译会拒绝每个未标记为 `read_only: true` 的目录条目。

## 分发与协议配置文件

支持的本地分发路径是上述可安装的 `neograph-harness-mcp` 二进制文件。源码构建可以继续使用示例目标，Python wheel 仍然是库/运行时包，而不是隐式安装远程守护进程。MCPB 和官方注册表发布仍然是发布/发现打包选项；它们不是线路协议所必需的，只应在带有签名发布工件和显式远程认证部署清单的情况下添加。

NeoGraph 目前仅发布带日期的 MCP `2025-11-25` 配置文件。描述未来无状态协议的最终 SEP 不会创建新的线版本；在 MCP 项目发布新的带日期规范之前，不会公布任何后续配置文件。
