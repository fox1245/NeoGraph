<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/mcp-panels.md locale=zh-CN source_sha256=ce9166dfce50a6fe8b400d1e21c0f1eba65d4af1e7f990ee9a5d4b03a7f68e8a -->
# MCP 专家组流程

**Languages:** [English](mcp-panels.md) | [한국어](mcp-panels.ko.md) | [日本語](mcp-panels.ja.md) | [简体中文](mcp-panels.zh-CN.md)

1. 调用 `neograph_schema`；仅使用此构建返回的预设和字段。
2. 构建一个请求，提供明确的目标、验收标准、有界预算，以及每个工作者的 JSON 输出模式。
3. 审查工作使用 `pr_review_panel`，将 `policy.read_only` 设为 true，并将 `policy.evidence_required` 设为每个发现项模式都要求的证据字段。
4. 只向每个工作者提供它需要的工具 ID。标记只读工具，并在 `path_arguments` 中列出承载路径的字符串参数。只要存在此类参数，就显式设置 `policy.workspace_roots`。
5. 调用 `neograph_compile`。如果 `ok` 为 false，根据 `phase`、`path` 和 `source` 修复诊断；绝不能对被拒绝的请求调用 start。
6. 使用保留的 `artifact_id` 调用 `neograph_start`。
7. 使用 `run_id` 轮询 `neograph_get`。如果状态为 `awaiting_tool_results` 或 `input_required`，只完成返回的 `pending` 调用，然后使用同一 `run_id`、准确的 `call_id` 和符合 `result_schema` 的结果调用 `neograph_resume`。相同的重复提交应视为已获确认；绝不能换用其他调用 ID。
8. 持续轮询，直到状态为终态。在主上下文中保留精简结果。
9. 只有最终回答需要工作者详情或执行轨迹时，才通过带有其 `run_id` 的 `neograph_get` 解引用返回的 `neograph://runs/...` URI。
10. 如实报告部分完成、零发现、超时、取消、过期、达到最大步数和失败的结果；不要将它们统一描述为成功。

## 反模式

- 不要对内联请求跳过 `neograph_compile`。
- 不要给每个工作者都附上广泛的工具目录。
- 不要在未设置工作区根目录时配置承载路径的工具。
- 不要将格式错误或空的工作者输出当作空的发现项列表。
- 不要在精简结果证明有必要之前获取详细轨迹。
- 不要给只读审查添加可写工具。
- 不要在宿主结果的调用 ID 已被消费后，修改数据再重试该结果。
- 不要假定 MCP Tasks 属于核心协议支持。使用稳定的 `neograph_get` 轮询，除非服务器和该单个请求明确选择启用实验性的 `io.modelcontextprotocol/tasks` 扩展。

## 示例

进行 PR 审查时，用宿主的仓库工具收集差异，将其放入任务目标，并使用两个工作者，分别给予正确性和安全性方面的指令。要求每个发现项包含 `file`、`line` 和 `evidence`。完整请求和宿主设置命令见仓库中的
[HARNESS_MCP.md](../../../docs/HARNESS_MCP.md)。
