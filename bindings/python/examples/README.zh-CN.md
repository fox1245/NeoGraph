<!-- neograph-i18n: source=bindings/python/examples/README.md locale=zh-CN source_sha256=5ec25b467bef853ac9df52be725f8c16b6c63aef9ab5f6f5b64c974749579bee -->
# Python API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这 28 个脚本演示了图状态、路由、工具、provider 请求和协议托管。建议从离线的示例开始；调用托管模型需要明确提供凭据，并且会产生 provider 费用。

## 准备

```bash
pip install neograph-engine
python 01_minimal.py
```

在这个目录下运行命令。`python-dotenv` 是可选的：如果希望 `_common.py` 加载离得最近的示例或仓库的 `.env`，就安装它。已导出的环境变量优先。缺少托管服务的凭据会报错，不算验证成功。

如果要改为针对源码构建来运行，用 `-DNEOGRAPH_BUILD_PYBIND=ON` 配置，并把 `PYTHONPATH` 设为构建目录。构建目录不会安装包的 Python 依赖：请先运行 `pip install 'pydantic>=2' certifi`。`import neograph_engine` 需要 pydantic；没有 certifi 时，TLS 使用宿主机的信任库。

运行托管服务的示例时，设置 `OPENAI_API_KEY`，还可以选择性地设置 `OPENAI_MODEL`（默认 `gpt-4.1-mini`）。`_common.py` 会准入(admission)一份封闭的 OpenAI Chat 或 Responses 描述符，然后构造 `SchemaProvider`。带类型的 SDK 使用 libcurl 的 HTTP；已移除的 provider 类、completion 参数、WebSocket 路径和传输后端选择器都不再可用。

要在没有密钥的情况下做协议验证，把 `OPENAI_API_BASE` 设为一个忠实的回环 Chat/Responses 对端。它必须实现示例所用的路由和响应形态，而不是返回一个 Python mock。本地 TLS 的 CA 用 `NG_EXAMPLE_CA_FILE` 指定。也可以用 `NG_PROVIDER_DESCRIPTOR` 指向一份完整的、已准入的描述符 JSON 文件，其中包括网关前缀、路由和允许的 header。基础 URL 不是完整的 `/v1/chat/completions` 路由。带类型的 request/outcome 契约，见 [Python 绑定指南](../../../docs/python-binding.md)。

## 索引与预期行为

每一项都用 `python <file>` 运行。Linux x86_64 上的冒烟测试，针对不需要凭据的本地对端，运行了选定的几个带类型 provider 的应用，包括两个研究类应用。这并不能证明每个脚本、每个托管 vendor 或每个发布平台都合格；远程 CI 和发布仍待完成。范围见[当前的发布证据](../../../CHANGELOG.md#unreleased)。表中给出的是预期行为，而不是模型确切的措辞。

| # | 文件 | 前置条件 | 预期行为 |
|---|------|---------------|-------------------|
| 01 | [`01_minimal.py`](01_minimal.py) | 无 | 自定义节点把 21 翻倍成 42。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | 无 | 脚本化的工具调用经过 `tool_dispatch`；计算器返回 `42`。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | 无 | 八个 `Send` 分支合并出平方数 `[0, 1, 4, 9, 16, 25, 36, 49]`；不假定 append 的顺序。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | 无 | 八次异步运行产生 `[0, 2, 4, 6, 8, 10, 12, 14]`；流式运行会发出节点事件。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | Chat 对端或托管服务的密钥 | `llm_call` 写入一条内容为 `hello world` 的 assistant 消息。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | Responses 对端或托管服务的密钥 | 模型发出的 `calc` 调用返回 `4053`，然后模型给出最终回答。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | 无 | 检查点在支付分发之前暂停；批准后恰好恢复一次模拟支付。不会扣任何钱。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | Chat 对端或托管服务的密钥 | 三个问题分别路由到数学、翻译和通用专家；每个专家写出自己的回答。 |
| 09 | [`09_state_management.py`](09_state_management.py) | 无 | Alpha 计数到 11；beta fork 了这个值；未知的线程没有检查点。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | 无 | 输入 200、50 和 -10 分别通过 `Command` 选择 accept、manual 和 reject 节点。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | Chat 对端或托管服务的密钥 | Actor/critic 调用把反思带入下一次尝试，在出现 `ok` 或达到超步上限时停止。这是一个有界的教学性改编。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | Chat 对端或托管服务的密钥 | 中间的问题和回答累积在 scratchpad 里，最后才给出答案。没有接入搜索服务。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | Chat 对端或托管服务的密钥 | 两个 `Send` 分支产生相反的论点；一个 judge 读取合并后的论点。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | 无 | 翻倍的结果 42，以及保存下来的 `my_graph.json` 定义。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 先运行 14 | 保存的定义把 5 翻倍成 10，把 100 翻倍成 200；自定义节点类型需要另外注册。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | Responses 对端/密钥；`gradio` | 普通聊天，或者三个问题组成的研究报告。researcher 用的是模型自身的知识，不做网络搜索。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | Responses 对端/密钥；`gradio`、`requests`；可选的 Crawl4AI/Postgres | `CRAWL4AI_URL` 启用真实的 `/md` 搜索；`NEOGRAPH_PG_DSN` 启用持久的引擎检查点。已配置的服务出错时，不会被悄悄替换。两者都不设置时，脚本会明确报告当前是仅用模型的研究/内存状态。Gradio 的历史不会自动恢复。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | Chat 对端或托管服务的密钥 | 跨两个主题的五次运行，只会执行两次由 provider 支撑的节点；重复的输入重放缓存的写入。不承诺固定的延迟。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | 无 | 五个脚本化的 token 事件组成 `Octopuses have three hearts.` 以及消息流的各个块。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | `opentelemetry-api`、`opentelemetry-sdk` | 控制台中的 span 覆盖这次运行和三个节点；最终的轨迹是 `['A', 'B', 'C']`。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | TLS Chat 对端/密钥；libcurl 的 HTTP/2 支持 | 通过同一个 SDK 对比 HTTP/1.1 和 HTTP/2 请求。协商出的协议要另行确认；耗时取决于 endpoint。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | Chat 对端或托管服务的密钥 | 给 profile JSON 打分，失败后请求一张修订过的图，重新编译，再重试。只有已注册的节点类型才会被准入。模型可能用完迭代次数上限也没有成功。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | Chat 对端或托管服务的密钥 | 在一次已准入的图重写前后保留对话检查点；在 `__graph_meta__` 中记录版本/哈希。这个元数据属于应用层，不是权威的重放证据。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | 无 | 在两个同级工具运行之前暂停；拒绝时只执行 `list_files`，批准时两者各执行一次。shell 操作是模拟的。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | 无 | 对比串行的 `Tool` 和 I/O 密集、会重叠的 `AsyncTool`，然后展示 CPython 的 GIL 边界。耗时比值只是观察结果，不是通过的标准。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | 启用了 MCP 的构建 | 启动一个真正的本地 JSON-RPC MCP 对端，发现 `fetch`，在显式的可重入策略下执行三次调用。不需要外部网络，也不需要密钥。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | Python 3.10+；`neograph-engine[a2a]` | 在 `127.0.0.1:9999` 上提供 agent card 和 A2A JSON-RPC；流式制品组成 `NeoGraph received: hello (turn 1)`。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | Python 3.10+；`neograph-engine[acp]` | 通过 stdio 提供 ACP；初始化、创建会话、发送 prompt，接收 token 更新和 `end_turn`。持久的 `session/load` 需要配置 Postgres 或 SQLite 后端。 |

## 状态与调度

节点读取当前超步的状态，并返回对通道的写入。引擎在下一个超步之前，通过声明的 reducer 合并这些写入。`Send` 提供各分支专属的输入；`Command` 更新状态并选择下一个节点。在 CPython 的 GIL 之下，worker 线程池不会让 CPU 密集的 Python 回调并行执行。

`GraphEngine.compile()` 既接受从 JSON 加载的字典，也接受直接用 Python 写出的字典。定义里保存的是连线关系，而不是可执行的 Python 类：编译已保存的定义之前，要先注册自定义节点工厂。示例 14 和 15 会在脚本旁边写入/读取 `my_graph.json`；验证这一对示例时，请使用一次性的检出，或这个目录的副本。

## 交互与协议场景

运行 16/17 需要安装 `gradio`，启动脚本，然后访问打印出来的本地 URL。先提交 `hello`，再提交 `research apples`。对于 17，真实的 Crawl4AI `/md` 服务，或忠实的本地对端，必须返回 `{"success": true, "markdown": "..."}`。只有当正在运行的构建支持 Postgres、且数据库可以连接时，才配置 Postgres。引擎的状态要与界面上的历史分开检查。

示例 27/28 使用官方的 Python SDK 处理线路上的协议，并使用 `ProtocolHostAdapter` 来运行支持检查点的引擎。验证 A2A 需要用 SDK 客户端获取 agent card、发送消息并消费流。验证 ACP 需要通过一个客户端子进程连接，执行 `initialize`、`session/new` 和 `session/prompt`；stdout 专门留给协议消息。在同一个上下文/会话里再发送第二个 prompt，预期会看到 `(turn 2)`。取消应当终止正在进行的请求，而不是造出一个形似成功的兜底结果。

要使用持久的 ACP 会话，`NEOGRAPH_ACP_POSTGRES_URL` 和 `NEOGRAPH_ACP_SQLITE_PATH` 两者中恰好设置一个。第一个 prompt 完成之后，就会创建 `session/load` 所需的检查点。每个会话只保留一个活动的 agent 进程；检查点存储不会对跨进程的并发写入方做串行化。示例会原样回显富内容块，但不会解释图像或音频内容，也不提供编辑器的文件系统/终端回调。

## 分发包名与 import 名

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

分发包名是 `neograph-engine`；PyPI 上的 `neograph` 是一个无关的项目。
