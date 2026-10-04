<!-- neograph-i18n: source=bindings/python/examples/README.md locale=zh-CN source_sha256=a1ffbe746f41909d860beac33ef1f3ea473e10ad6162985e4fba7b3810b4dd22 -->
# Python API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这28个脚本展示图状态、路由、工具、提供方请求和协议托管。请先从离线示例开始；调用托管模型需要明确配置凭据，并会产生提供方费用。

## 设置

```bash
pip install neograph-engine
python 01_minimal.py
```

请在此目录下运行命令。`python-dotenv` 是可选依赖：如果希望 `_common.py` 加载示例或仓库中最近的 `.env`，请安装它。已导出的环境变量优先。缺少托管服务凭据属于错误，不能算作验证成功。

运行托管服务示例时，请设置 `OPENAI_API_KEY`，也可以设置 `OPENAI_MODEL`（默认为 `gpt-4.1-mini`）。`_common.py` 接受封闭形式的 OpenAI Chat 或 Responses 描述符，再构造 `SchemaProvider`。类型化 SDK 使用 libcurl HTTP；已移除的提供方类、补全参数、WebSocket 路径和传输后端选择器均不可用。

进行无密钥协议验证时，将 `OPENAI_API_BASE` 设置为忠实实现 Chat/Responses 协议的回环端点。它必须实现示例所用的路由和响应格式，而不是返回 Python mock。使用 `NG_EXAMPLE_CA_FILE` 指定本地 TLS CA。也可以用 `NG_PROVIDER_DESCRIPTOR` 指定完整且已获准的描述符 JSON 文件，其中包含网关前缀、路由和允许的请求头。基础 URL 并不是完整的 `/v1/chat/completions` 路由。类型化请求和 outcome 的契约见 [Python 绑定指南](../../../docs/python-binding.md)。

## 索引与预期行为

使用 `python <file>` 运行各项。Linux x86_64 冒烟运行通过无需凭据的 localhost 对端执行了部分类型化 provider 应用，包括两个研究应用。这不代表所有脚本、托管供应商或发布平台均已验证；远程 CI 和发布仍未完成。范围见[当前发布证据](../../../CHANGELOG.md#unreleased)。表格描述预期行为，模型生成的措辞不具有确定性。

| # | 文件 | 前提条件 | 预期行为 |
|---|------|---------------|-------------------|
| 01 | [`01_minimal.py`](01_minimal.py) | 无 | 自定义节点将21翻倍为42。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | 无 | 脚本指定的工具调用经过 `tool_dispatch`，计算器返回 `42`。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | 无 | 八个 `Send` 分支合并平方数 `[0, 1, 4, 9, 16, 25, 36, 49]`；不假定追加顺序。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | 无 | 八次异步运行产生 `[0, 2, 4, 6, 8, 10, 12, 14]`；流式运行发出节点事件。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | Chat 端点或托管服务密钥 | `llm_call` 写入内容为 `hello world` 的助手消息。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | Responses 端点或托管服务密钥 | 模型发起的 `calc` 调用返回 `4053`，随后模型给出最终答案。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | 无 | 检查点在支付分派前暂停；批准后恢复且仅执行一次模拟支付。不收取任何款项。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | Chat 端点或托管服务密钥 | 三个问题分别路由到数学、翻译和通用专家；各专家写入自己的答案。 |
| 09 | [`09_state_management.py`](09_state_management.py) | 无 | Alpha 的计数达到11；beta 从该值分叉；未知线程没有检查点。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | 无 | 输入200、50和-10通过 `Command` 分别选择 accept、manual 和 reject 节点。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | Chat 端点或托管服务密钥 | Actor/critic 调用把反思带入下一次尝试，并在 `ok` 或达到超步上限时停止。这是限制了次数的教学改编。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | Chat 端点或托管服务密钥 | 在给出最终答案前，将中间问题和答案累积到暂存区。不连接搜索服务。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | Chat 端点或托管服务密钥 | 两个 `Send` 分支生成相反的论点；一名评判者读取合并后的论点。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | 无 | 得到翻倍后的结果42，并保存 `my_graph.json` 定义。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 先运行14 | 保存的定义将5翻倍为10，将100翻倍为200；自定义节点类型另行注册。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | Responses 端点/密钥；`gradio` | 普通聊天或围绕三个问题的研究报告。研究者使用模型知识，而不是网络搜索。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | Responses 端点/密钥；`gradio`、`requests`；可选 Crawl4AI/Postgres | `CRAWL4AI_URL` 启用真实的 `/md` 搜索；`NEOGRAPH_PG_DSN` 启用持久化引擎检查点。配置的服务发生故障时，不会被悄悄替换。两项均未设置时，脚本明确报告仅使用模型进行研究以及状态保存在内存中。Gradio 历史不会自动恢复。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | Chat 端点或托管服务密钥 | 两个主题上的五次运行仅执行两次由提供方支持的节点；重复输入重放缓存的写入。不承诺固定延迟。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | 无 | 五个脚本指定的 token 事件组成 `Octopuses have three hearts.` 和消息流分块。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | `opentelemetry-api`、`opentelemetry-sdk` | 控制台 span 覆盖整次运行和三个节点；最终轨迹为 `['A', 'B', 'C']`。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | TLS Chat 端点/密钥；libcurl HTTP/2 支持 | 通过同一个 SDK 比较 HTTP/1.1 和 HTTP/2 请求。请另行确认协商出的协议；耗时取决于端点。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | Chat 端点或托管服务密钥 | 为 profile JSON 评分，失败后请求修订图，重新编译并重试。只接受已注册的节点类型。模型可能在未成功的情况下耗尽迭代次数上限。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | Chat 端点或托管服务密钥 | 在获准的图重写前后保留会话检查点；在 `__graph_meta__` 中记录版本/哈希。这些元数据属于应用层，不是权威的重放证据。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | 无 | 在两个同级工具中的任何一个运行前暂停；拒绝时只执行 `list_files`，批准时两个工具各执行一次。Shell 操作为模拟操作。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | 无 | 比较串行 `Tool` 和重叠执行 I/O 密集型工作的 `AsyncTool`，再展示 CPython GIL 的边界。耗时比例是观测结果，不是通过标准。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | 启用 MCP 的构建 | 启动真实的本地 JSON-RPC MCP 端点，发现 `fetch`，以明确的可重入策略执行三次调用。不需要外部网络或密钥。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | Python 3.10+；`neograph-engine[a2a]` | 在 `127.0.0.1:9999` 提供智能体卡片和 A2A JSON-RPC；流式工件组成 `NeoGraph received: hello (turn 1)`。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | Python 3.10+；`neograph-engine[acp]` | 通过 stdio 提供 ACP；初始化、创建会话、发送提示、接收 token 更新和 `end_turn`。持久化 `session/load` 需要配置 Postgres 或 SQLite 后端。 |

## 状态与调度

节点读取当前超步的状态并返回通道写入。引擎在下一个超步之前，通过声明的归约器合并这些写入。`Send` 提供分支专用输入；`Command` 更新状态并选择下一个节点。在 CPython GIL 下，工作线程池不会使 CPU 密集型 Python 回调并行执行。

`GraphEngine.compile()` 既接受从 JSON 加载的字典，也接受用 Python 编写的字典。定义存储的是连接关系，而不是可执行的 Python 类：编译保存的定义前，请注册自定义节点工厂。示例14和15在脚本所在目录写入/读取 `my_graph.json`；验证这一对示例时，请使用可丢弃的检出副本或此目录的副本。

## 交互与协议场景

为16/17安装 `gradio`，启动脚本并访问打印出的本地 URL。先提交 `hello`，再提交 `research apples`。在17中，真实的 Crawl4AI `/md` 服务或忠实实现协议的本地端点必须返回 `{"success": true, "markdown": "..."}`。只有运行中的构建支持 Postgres 且数据库可达时，才配置 Postgres。请分别检查引擎状态和 UI 历史。

示例27/28使用官方 Python SDK 处理线上协议，使用 `ProtocolHostAdapter` 执行支持检查点的引擎调用。A2A 验证需要 SDK 客户端来获取智能体卡片、发送消息并消费流。ACP 验证需要客户端连接子进程，以执行 `initialize`、`session/new` 和 `session/prompt`；stdout 专用于协议消息。在同一上下文/会话中发送第二条提示，预期得到 `(turn 2)`。取消应停止活动请求，而不是生成看似成功的替代结果。

对于持久化 ACP 会话，请仅设置 `NEOGRAPH_ACP_POSTGRES_URL` 或 `NEOGRAPH_ACP_SQLITE_PATH` 中的一项。第一条提示完成后会创建 `session/load` 所需的检查点。每个会话只保留一个活动智能体进程；检查点存储不会将跨进程的并发写入串行化。此示例回显富内容块，但不解释图像/音频内容，也不提供编辑器文件系统/终端回调。

## 发行名称与导入名称

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

发行包名为 `neograph-engine`；PyPI 上的 `neograph` 是无关项目。
