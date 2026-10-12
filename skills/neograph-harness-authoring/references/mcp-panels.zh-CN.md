<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/mcp-panels.md locale=zh-CN source_sha256=ce9166dfce50a6fe8b400d1e21c0f1eba65d4af1e7f990ee9a5d4b03a7f68e8a -->
# MCP 面板流程

**Languages:** [English](mcp-panels.md) | [한국어](mcp-panels.ko.md) | [日本語](mcp-panels.ja.md) | [简体中文](mcp-panels.zh-CN.md)

1. 调用 `neograph_schema`；只使用本次构建返回的预设和字段。
2. 构造一个请求，包含明确的目标、验收标准、有界的预算，
   以及每个 worker 的 JSON 输出 schema。
3. 审阅类的工作，使用 `pr_review_panel`，把 `policy.read_only` 设为 true，
   并把 `policy.evidence_required` 设为每个 finding schema 中都要求的证据字段。
4. 只给每个 worker 它所需要的工具 ID。标记只读工具，并把带路径的字符串参数
   列在 `path_arguments` 里。只要存在这类参数，就要显式设置
   `policy.workspace_roots`。
5. 调用 `neograph_compile`。如果 `ok` 为 false，按 `phase`、
   `path` 和 `source` 修复诊断；绝不要用被拒绝的请求去调用 start。
6. 用保留下来的 `artifact_id` 调用 `neograph_start`。
7. 用 `run_id` 轮询 `neograph_get`。如果状态是 `awaiting_tool_results` 或
   `input_required`，只完成返回的 `pending` 调用，然后用同一个 `run_id`、
   确切的 `call_id`，以及符合 `result_schema` 的结果，调用
   `neograph_resume`。完全相同的重复提交视为已确认；
   绝不要换成别的 call ID。
8. 继续轮询，直到状态进入终态。把精简的结果保留在主上下文里。
9. 只有当最终答案需要 worker 的细节或执行 trace 时，才用 `neograph_get`
   和对应的 `run_id`，去取返回的 `neograph://runs/...` URI。
10. 如实报告部分完成、零发现、超时、已取消、已过期、达到最大步数和失败这几种结果；
   不要把它们笼统地当成成功。

## 反模式

- 内联请求也不要跳过 `neograph_compile`。
- 不要把一份宽泛的工具目录挂给每个 worker。
- 配置了带路径的工具时，不要不设工作区根目录。
- 不要把格式错误或为空的 worker 输出当作空的 findings 列表。
- 在精简结果证明确实需要之前，不要去取详细的 trace。
- 不要给只读的审阅加入具有写能力的工具。
- call ID 已被使用之后，不要用修改过的数据重试宿主的结果。
- 不要以为 MCP Tasks 是核心协议的一部分。除非服务端和单个请求都明确选用了
  实验性的 `io.modelcontextprotocol/tasks` 扩展，否则使用稳定的 `neograph_get`
  轮询。

## 示例

做 PR 评审时，用宿主的仓库工具收集 diff，把它放进任务目标里，
并用两个 worker，分别给出关于正确性和安全性的不同指令。
要求每个 finding 都包含 `file`、`line` 和 `evidence`。完整的请求和宿主配置命令，
见仓库中的 [HARNESS_MCP.md](../../../docs/HARNESS_MCP.md)。
