<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=zh-CN source_sha256=d649c0a0a5d99d39d6a84ec5a4b48707f6b5f49a7a5143ff3ce3aa13c8b9436b -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)



NeoGraph Harness 会在运行前编译一个有界的多工作器工作流。稳定的 MCP 接口保持为六个工具：

- `neograph_schema` 发现已安装的请求契约和预设。
- `neograph_compile` 展开、编译并验证，但不执行。
- `neograph_start` 启动保留的构件或内联请求。
- `neograph_get` 轮询紧凑状态，或解引用结果构件 URI。
- `neograph_resume` 验证并提交精确的待处理主机结果。
- `neograph_cancel` 协作取消排队、正在运行或等待中的工作流。

随附的预设包括 `fanout_judge`、`pr_review_panel`、`bug_triage` 和
`research_synthesis`。预设生成 strict-Core 图构件；JavaScript 请求保留自己的
`ProgramSource` 封套和源映射。

### JavaScript 创作边界

新发布只接受 `harness.mode` 为 `preset` 或 `javascript`。JavaScript 源码放在
`harness.source` 中，并可用 `harness.source_id` 固定源 ID。`define()` 通过封闭的
`ng` 绑定构造一个图；可选生成器 `main()` 使用普通 JavaScript 循环和分支，并且只
yield `ng.callCore`、`ng.all`、`ng.any`、`ng.race` 等类型化命令。

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

#### 控制流迁移示例

`define()` 只在编译时运行；所有运行时效果都必须位于 yield 的类型化命令之后。
下面是一个完整请求：生成器最多使用两个操作和两路并行，工作节点与主机从请求
中封存的配置完全一致，终止返回值也符合 Harness 结果格式。

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
    max_program_operations: 2,
    max_worker_retries: 1,
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  },
  policy: {read_only: true, evidence_required: []}
};
```

稳定的源码位置字符串属于持久命令坐标的一部分；重试和重启之间必须保持确定性。
当所需数量的成功先完成时使用 `ng.any(...)`，当第一个终止成员应获胜时使用
`ng.race(...)`；两者都会通过结构化并发取消尚未完成的兄弟任务。环境 I/O、
计时器、动态加载、`eval` 和原生句柄仍不可用。

`harness.mode` 必须显式指定。`dsl` 以 `H_MIGRATION_CORE_DSL` 失败，`core` 以
`H_MIGRATION_CORE_JSON` 失败，`program`/`program_json` 以
`H_MIGRATION_PROGRAM_JSON` 失败。strict Core JSON 和受信任的 C++ 构造仅作为
内部表示保留，不是公开的 Harness 创作语言。

模式导出、编译和启动使用同一个不可变 `HarnessAdmissionProfile`。只有作用域限定的
`GraphRegistry` 可参与解析，不会回退到进程全局注册项。被拒绝的输入不会派发节点、
工作器或效果。

预设和 JavaScript 请求都经过 `ProgramSource`、`ProgramCompiler`、
`ProgramCatalog` 和 `ProgramRuntime`；`GraphEngine` 仍是唯一的节点执行器。已保存的
旧构件会显式分类为 `translated`、`drain_only` 或 `rejected`。`drain_only` 不允许
新发布或新运行，只能在完整保留的旧运行时上恢复已有运行。

## 构建并运行

使用兼容 OpenAI 的提供者适配器构建本地 stdio 服务器：
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

`NEOGRAPH_HARNESS_API_KEY` 优先于 `OPENAI_API_KEY`。`NEOGRAPH_HARNESS_BASE_URL` 可选择任何 OpenAI 兼容端点。服务器同时接受未带版本号的基础地址（例如 `https://openrouter.ai/api`）和提供者文档中的带版本地址（例如 `https://openrouter.ai/api/v1`）；只有地址缺少 `/v1` 时才会补上 `/v1`。服务器只把协议消息写入标准输出，只把诊断信息写入标准错误。当前端点格式请参阅 [OpenRouter 快速入门](https://openrouter.ai/docs/quickstart)。

仅在主机互操作性冒烟测试中设置 `NEOGRAPH_HARNESS_SMOKE=1`。该显式模式使用确定性的进程内提供者，返回有效的零发现审查结果，不需要 API key，也不得用作 LLM 质量测试。

持久的主机代理式调用需要记录和检查点持久性。该示例通过一个显式目录启用两者：
```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

这会把不可变构件、可变运行记录和只追加的因果日志存入 `runs.db`，并把图检查点存入 `checkpoints.db`。日志行和每个由 Harness 创建的检查点，都会把运行绑定到它的不可变构件、已编译的修订摘要、MCP 协议版本和 Harness 配置文件。工作器尝试包括持续时间、验证/重试结果，以及把提供者调用、能力调用和主机代理调用连接到发起尝试的关联 ID。两个 SQLite 存储都使用 WAL 模式和有界的 busy timeout。现有版本 1 记录数据库会在打开时以事务方式迁移到版本 3。该目录会在服务器重启后保留。缺少任一存储时，`host_brokered` 目录条目会在编译时被拒绝，因此工作流不能宣称自己具备实际没有的可恢复性。

自定义嵌入可以从可选的 `neograph::mcp_sqlite` 目标中使用 `SqliteHarnessRecordStore`，构建相同的后端。默认日志模式会在 SQLite 看到数据之前，递归地把常见的秘密字段和内容字段替换为 `[REDACTED]`。`METADATA_ONLY` 会丢弃每个事件负载；`FULL` 会原样保留提供者内容、工具参数和结果，并且只应为已批准存储的数据启用。事件可以通过 `HarnessJournal::list_events(run_id, after_sequence, limit)` 按运行顺序读取。`FileHarnessRecordStore` 仍可供偏好原子 JSON 文件的部署使用；它不实现日志边界。

### 保留

SQLite 存储实现了可选的兄弟接口 `HarnessRetentionStore`；稳定的 `HarnessRecordStore` vtable 没有改变。在保留构件或启动运行之前，`HarnessService` 会应用 `HarnessServiceConfig` 中的 `max_artifacts` 和 `max_runs`。两者的默认值都是 128。

清理只会删除已终止的叶运行。排队、正在运行、等待输入的运行，以及尚未完成日志终结的进程内执行都会受到保护。重播行或分叉行会记录 `source_run_id`，因此只要依赖项仍被保留，就不能删除其来源。如果需要释放空间，会先删除依赖叶；只有在没有任何保留行引用源之后，该源才会在后续步骤中变为可删除。因此，当每个候选项都处于活动状态、受到显式保护或仍被引用时，限制是软性的。

在 `runs.db` 内，同一个事务会先删除运行的日志行，再删除运行行；只有在没有任何运行引用某个构件之后，才会删除该构件。提交后，Harness 会从单独配置的检查点存储中删除已删除运行的检查点线程。第二阶段期间的崩溃或检查点后端故障可能留下不可达的检查点存储，但不会让保留的重播/分叉指向已删除的源记录。后续的管理清理或后端专用孤儿清理可以回收这种仅剩检查点的残留物。

`FileHarnessRecordStore` 不实现持久清理；它原有的内存中构件缓存驱逐和硬性运行容量行为仍然保留。

## 调试器视图

`neograph_get` 保留 `status` 作为紧凑默认视图，并在不增加另一个 MCP 工具的情况下添加四个调试视图：

|视图|结果|
|---|---|
| `attempts` |写入日志的工作器尝试启动/完成/中断事件|
| `trace` |现有的有序 GraphEngine 节点跟踪，加上因果日志时间线|
| `checkpoints` |不含负载的检查点元数据：ID、父级、节点、阶段、步骤和通道名称|
| `diff` |每个检查点与其父级之间发生变化的通道值和版本|

`attempts` 和 `trace` 接受 `after_sequence` 作为不透明的前向游标。四个视图都接受范围为 1 到 1000 的 `limit`。返回的构件 URI 可以携带与查询相同的分页参数，例如：
```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

这些 URI 只接受 `after_sequence` 和 `limit`。未知或格式错误的查询字段会导致失败，而不是被忽略。由日志支持的视图会返回与持久保存内容完全相同的负载，因此会保留已配置的脱敏模式。`diff` 视图根据检查点存储而不是日志计算，并且可能包含完整的通道值；应像访问现有详细运行结果一样看待对它的访问。

## 重播模式

`neograph_start` 可以在不添加另一个 MCP 工具的情况下重播已完成的运行：
```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded` 会使用源日志中已完成的工作器尝试结果，重新执行由编译器锁定的图。它绝不会调用已配置的工作器、提供者、MCP、A2A 或能力执行器。源构件修订、协议和配置文件必须仍然匹配，并且日志必须使用 `FULL` 负载模式。`REDACTED` 和 `METADATA_ONLY` 日志会故意禁止重播，因为它们不保留精确的工作器输出。中断的尝试会被丢弃；已完成的恢复后工作器调用会被重播。

使用 `mode: "live"` 可通过实时提供者和工具执行相同的保留构件。快照和日志生命周期事件会把运行标记为 `recorded_replay` 或 `live_replay`，并包含 `source_run_id`；普通启动仍标记为 `live`。

## 兼容分叉

先编译修复后的 Harness，然后通过现有 `neograph_start` 工具，把精确的前置检查点分叉到该目标构件：
```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

源检查点必须属于 `source_run_id`。在分配运行之前，Harness 会针对目标构件验证检查点架构、源修订、MCP 协议、Harness 配置文件、每个恢复通道和归约器、每个延续节点，以及任何活动屏障接口。不兼容的分支会返回 `started: false`、`status: "incompatible_fork"`，以及带有 `path` 和 `witness` 的机器可读 `H_FORK_*` 诊断；它不会创建运行或分叉检查点。

需要检查点存储。如果没有记录存储，分叉只能引用仍驻留在当前服务进程中的源运行和构件；如果分叉谱系必须在重启后继续存在，请同时配置两种存储。

兼容的分支会标记为 `compatible_fork`，并在启动响应、快照和生命周期日志事件中同时携带 `source_run_id` 和 `source_checkpoint_id`。执行会从所选检查点的 `next_nodes` 恢复；已经提交的前序节点不会再次运行。目标构件提供修复后的拓扑、工作合约和工具目录，而恢复的通道值（包括原始任务通道）来自源检查点。当任务输入本身必须改变时，请使用全新启动而不是分叉。

源运行、构件和选定的检查点都是分叉引用的对象；只要兼容性检查或分叉执行还可能使用它们，就必须继续保留。保留清理必须先删除依赖项，或保留被引用的源；绝不能在预检和分支创建之间删除源检查点。

## 可流式传输的 HTTP

远程传输是可选的，因此现有的 stdio-only 目标仍然很小，也不会悄悄引入 HTTP/OpenSSL 依赖项：
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

端点是 `http://127.0.0.1:8080/mcp`。它实现已发布的 MCP 2025-11-25 Streamable HTTP POST 契约，提供按会话划分的 MCP 生命周期和 JSON 响应。通知返回 HTTP 202。DELETE 会终止会话。可选的独立 GET/SSE 通道故意不实现并返回 HTTP 405；传输规范明确允许这样做。

安全默认值位于传输层，并且不会把认证耦合到 `GraphEngine` 或 `HarnessService`：

- 默认绑定地址是 `127.0.0.1`；除非已配置 Bearer 授权器，否则非环回绑定会被拒绝。
- 每个提供的 `Origin` 都会被拒绝，除非它与 `NEOGRAPH_HARNESS_ALLOWED_ORIGINS`（在可执行文件中用逗号分隔）中的某个条目完全匹配。
- `NEOGRAPH_HARNESS_BEARER_TOKEN` 会为该可执行文件启用单主体 Bearer 边界。库嵌入可以使用 `MCPHttpServerConfig::bearer_authorizer` 进行 OAuth/JWT 验证，并返回稳定的主体/范围。
- 会话绑定到返回的授权范围。不同的有效主体不能复用泄露的 `Mcp-Session-Id`。
- `MCPHttpServer` 工厂接收经过验证的范围，并返回 `MCPHttpServerSession` 所有者。多租户嵌入必须使用该范围选择隔离的 Harness 记录/检查点存储；没有任何认证状态会进入图运行时本身。
- 请求负载、HTTP 工作器、队列、会话和响应等待限制都受 `MCPHttpServerConfig` 约束。

对于任何非环回部署，请在受信任的反向代理处终止 TLS，并使用其 OAuth/OIDC 验证或等效的 `bearer_authorizer`。转发原始 `Authorization` 和 `Origin` 标头，不要暴露明文公共监听器，并为每个 Harness 状态目录部署一个授权域。

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

针对这个受信任的本地服务器运行非交互式 `codex exec` 时，请在 Codex 的 `config.toml` 中设置 `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"`。没有它时，Codex 会正确地取消 `neograph_compile`，因为保留构件没有标注为只读。交互式会话可以保留默认提示。

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

使用 `opencode mcp list` 验证。这些形式遵循各主机的官方 MCP 配置契约；审核日期为 2026 年 7 月 21 日。

## PR 审查工作流

要求主机使用其常规存储库工具收集 PR diff，然后使用 Harness 工具。一个合适的请求是：
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

### 提供者预算

`budgets.provider_timeout_seconds` 会把一次提供者完成尝试限制在 1--600 秒内。`budgets.max_output_tokens` 会把一次完成限制为 1--128000 个生成令牌。两者都是可选的：省略任一字段都会保留原有行为，即没有 Harness 截止时间，并使用提供者现有的输出限制。

工作器可以把任一字段设置为更小的值。高于 Harness 全局值的工作器值会在编译时被拒绝。到达截止时间时，Harness 只会取消提供给该提供者调用的子取消令牌；它不会取消兄弟工作器或外层运行。提供者必须遵守该令牌，因此无法被中断的提供者仍可能在截止时间后返回。

主机应遵循以下顺序：

1. 调用 `neograph_compile`，如果 `ok` 为 false 则停止。
2. 使用返回的 `artifact_id` 调用 `neograph_start`。
3. 使用 `run_id` 轮询 `neograph_get`；这只返回结果和计数。
4. 如果需要详情，请用相同的 `run_id` 调用 `neograph_get`，并把返回的 `neograph://runs/...` URI 作为 `uri`。默认情况下，不要把跟踪拉入上下文。

### 发现来源

详情构件会在 `workers` 中保留每个通过模式验证的工作器响应，并为现有客户端保留既有的平坦 `findings` 数组。`finding_sources` 是一个等长的并行数组：每个条目都包含聚合后的 `finding_index`、来源 `worker_id`，以及该工作器的 `local_index`。可用它识别重复本地 ID（例如 `F1`）的来源；不要把来源字段添加到工作器声明的 finding 对象中。

## 主机代理式恢复

当能力由 MCP 主机而不是工作器进程拥有时，使用 `executor.kind: "host_brokered"`。将 `executor.interaction` 设为 `"tool_result"`（默认）或 `"input"`。提供者执行器会验证请求参数，并返回两个非终止运行状态之一：

- `awaiting_tool_results`：主机必须执行指定的能力。
- `input_required`：主机必须收集输入值。

`neograph_get` 会包含 `pending` 对象，其中带有唯一的 `call_id`、`tool_id`、已验证的 `arguments` 和 `result_schema`。按如下方式精确提交该调用：
```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume` 会拒绝不匹配的调用 ID、违反声明模式的结果、过期调用，以及针对非等待运行的迟到结果。完全相同的重复提交会被确认，而不会重新执行图；冲突的重复提交会被拒绝。接受的恢复意图会在调度执行之前持久化，因此进程崩溃后再次轮询时，会从 `NodeInterrupt` 检查点恢复，而不会重复已经成功的兄弟工作器。

### 外部影响与协调

普通的主机代理契约是向后兼容的：没有 `executor.effect` 的目录条目在进程重启后仍会保持 `awaiting_tool_results`，并接受相同的 `{run_id, call_id, result}` 恢复请求。

对于可能产生外部可见、非幂等更改的主机能力，请明确声明该风险。影响元数据只在默认的 `host_brokered` `tool_result` 交互中有效；它不是输入收集元数据。
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

随后，挂起的调用会包含一个持久的 `effect` 对象。其中的 `effect_id` 和 `idempotency_key` 作用域限定在 Harness 运行内，并且不同于提供者的工具调用 ID。`status_query` 和 `fencing` 描述主机能力；Harness 会记录它们，但不会发明特定于提供者的查询或重试协议。

如果服务重新连接时 `idempotency: "unsupported"` 调用仍在等待，它只会把该运行改为 `ambiguous_effect`。这意味着主机可能已经在进程停止之前执行了该外部效果，但 Harness 无法证明任一结果。紧凑状态会包含 `pending` 和 `ambiguity`，日志会记录 `host_brokered.effect.ambiguous`，并且 Harness 既不会重放该工具，也不会把外部效果报告为失败或已完成。

主机检查自己的权威系统后，通过 `neograph_resume` 解决歧义：
```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```
```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```
```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed` 会验证并消费 `result`，然后从检查点恢复。`failed` 会记录终止性的 Harness 故障，而不再次执行工作器。`unknown` 会让运行保持在 `ambiguous_effect`，以便稍后协调。完全重复的 completed、failed 或 unknown 提交是幂等的；冲突的 completed 或 failed 提交会被拒绝。每个非重复协调都会记录为 `host_brokered.effect.reconciled`。

模糊影响被有意设计为不可取消且不会过期。取消或超时无法确定外部影响是否已经发生；如果权威系统暂时无法解决，请提交 `unknown`。

该协议不要求在主机崩溃时提供 exactly-once 交付保证。支持幂等键或状态查询的主机应在提交协调之前使用这些系统来确定实际结果。

运行快照包括 `created_at`、`updated_at`、`expires_at` 和 `poll_after_ms`。默认 TTL 为 24 小时，默认轮询间隔为 1 秒；嵌入方可以通过 `HarnessServiceConfig` 覆盖两者。

## 实验任务简介

MCP Tasks 不是 MCP 2025-11-25 核心协议的一部分，并且上游扩展仍标记为实验性。因此 NeoGraph 默认禁用它，并将它与稳定的 `run_id` 加 `neograph_get` 轮询契约分开。

要选择加入示例服务器，还必须启用持久状态：
```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

随后服务器会声明 `io.modelcontextprotocol/tasks`，标记 `neograph_start` 具有可选任务支持，并服务 `tasks/get`、`tasks/update` 和 `tasks/cancel`。只有当单个 `tools/call` 请求包含以下内容时，它才会返回 `CreateTaskResult`：
```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

没有在请求级别选择加入的客户端会收到普通的 `CallToolResult`，并继续轮询 `neograph_get`；启用该配置文件不会改变稳定后备路径。任务状态包括 `working`、`input_required`、`completed`、`failed` 和 `cancelled`。`tasks/update.inputResponses` 以待处理的 `call_id` 为键，轮询客户端应遵守 `pollIntervalMs` 和 `ttlMs`。

## 能力后端

`make_provider_harness_executor` 会通过任意 NeoGraph `Provider` 驱动工作器。如果模型请求了已声明的工具，执行器会在分派前后根据目录验证其参数和输出。

对已初始化的下游 `MCPClient` 实例使用 `make_mcp_harness_capability_executor`，对 A2A agent 使用 `a2a::make_harness_capability_executor`。请求仍是权威来源：工作器只能看到其 `tools` 数组中列出的工具 ID。

对于文件系统工具，请在 `path_arguments` 中声明每个携带路径的输入，并设置 `policy.workspace_roots`。相对路径会解析到第一个根之下；分派前会拒绝所有位于任一已配置根之外的规范路径，包括通过现有符号链接逃逸的路径。传给能力后端的是规范路径，而不是模型提供的原始写法。下游 MCP 和 A2A 服务仍是独立的信任边界，也应强制执行相同的根策略，以关闭文件系统检查时间/使用时间竞争。使用 `policy.read_only: true` 时，编译会拒绝每个未标记为 `read_only: true` 的目录条目。

## 分发与协议配置

受支持的本地分发路径是上面的可安装 `neograph-harness-mcp` 二进制文件。源构建可以继续使用示例目标，Python wheel 仍然是库/运行时包，而不是隐式安装远程守护程序。MCPB 和官方注册表发布仍然是发布/发现包装选项；它们不是传输协议所必需的，并且只应通过签名发布构件和显式远程认证部署清单添加。

NeoGraph 目前只发布带日期的 MCP `2025-11-25` 配置。描述未来无状态协议的最终 SEP 不会创建新的传输协议版本；在 MCP 项目发布新的带日期规范之前，不会公布后继配置。
