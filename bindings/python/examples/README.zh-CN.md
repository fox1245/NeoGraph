<!-- neograph-i18n: source=bindings/python/examples/README.md locale=zh-CN source_sha256=f83696b352140f2c77392207424b16e3d297e7b183c67f5d1fd26347c1d7e911 -->
# Python API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

二十八个脚本覆盖了整个绑定表面的端到端应用。

## 设置

```bash
pip install neograph-engine python-dotenv
cp .env.example .env  # edit OPENAI_API_KEY for examples that hit a real LLM
```

`_common.py` 从此目录或任何父目录自动加载`.env`。需要 API 密钥的示例在密钥缺失时会优雅跳过（不会崩溃）。

## 索引

| # | 文件 | 网络 | Pattern |
|---|------|---------|---------|
| 01 | [`01_minimal.py`](01_minimal.py) | 离线 | `GraphNode` 子类 + `engine.run()`。最小的可用图形。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | 离线 | `Tool` 子类 + 内置 `tool_dispatch`。手工构造的 tool_call（无真实LLM）。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | 离线 | `run(input)` 返回 `NodeResult` 并带有 `Send` 列表 + `set_worker_count(4)`。映射归并。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | 离线 | `engine.run_async` + `asyncio.gather`，共 8 个并发运行 + `run_stream_async`。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | **OpenAI** | `OpenAIProvider` + 内置 `llm_call` 节点。一次性完成。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | **OpenAI** | ReAct 循环:`llm_call` ↔ `tool_dispatch`，带 `has_tool_calls` 条件。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | 离线 | 两阶段提出/批准工作流程，使用 mock LLM 发射器。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | **OpenAI** | 分类器节点 + 条件边 → 数学 / 翻译 / 通用专家。 |
| 09 | [`09_state_management.py`](09_state_management.py) | 离线 | `set_checkpoint_store(InMemoryCheckpointStore())` + `get_state` + `fork`. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | 离线 | `run(input)` 返回 `Command(goto_node=…, updates=[…])`。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | **OpenAI** | Actor + critic 循环与反思提示(Shinn 等人，2023)。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | **OpenAI** | Self-Ask 跟进问题分解(Press 等人，2022)。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | **OpenAI** | 双辩论者 + 裁判。辩论者通过 `Send` 进行 fan-out。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | 离线 | 将图定义序列化为 `.json` 文件。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 离线 | 加载一个`.json`图并运行它（14的配套）。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | **Responses（WS/HTTP）** | 多轮 Gradio 对话，在 `조사해줘 / research / investigate` 上切换到三路并行深度研究子图。官方 OpenAI 默认使用 WebSocket；兼容网关会自动选择构建中可用的 HTTP/2 或 HTTP/1.1。可通过 `NG_RESPONSES_TRANSPORT` 覆盖。需要 `pip install gradio`。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | **Responses + Crawl4AI + Postgres** | 与16相同的传输感知聊天结构，但研究人员会通过本地 Crawl4AI 容器（`docker run unclecode/crawl4ai`）实际搜索网络，状态则持久化到 Postgres（`PostgresCheckpointStore`）。两者都可通过环境变量选配，缺失时会正常降级。Postgres 路径需要使用 `-DNEOGRAPH_BUILD_POSTGRES=ON` 从源码构建。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | **OpenAI** | `engine.set_node_cache_enabled("ask", True, CacheScope.Reusable)` — 后续相同输入的运行会在0毫秒内重放缓存的 `NodeResult`，不再调用 LLM。可通过 `engine.node_cache_stats()` 查看统计信息。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | 离线 | `from neograph_engine import message_stream` — 包装一个回调，使`LLM_TOKEN`事件以LangChain形状的消息字典（`{role, content, content_so_far, node, metadata}`）形式到达。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | 离线 | `from neograph_engine.tracing import otel_tracer` — 将引擎事件桥接到OpenTelemetry spans。附带ConsoleSpanExporter；替换为OTLP以发送到Jaeger / Tempo / Honeycomb / Datadog。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | **OpenAI** | `SchemaProvider(..., prefer_libcurl=True)` — 对比可选的 HTTP/2（libcurl）传输与默认 ConnPool（HTTP/1.1 keep-alive）。构建中包含 libcurl 时，会以五路并行突发进行 A/B 测试；不含该后端的 wheel 则执行一次 HTTP/1.1 smoke 调用，并说明如何从源码构建 HTTP/2。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | **OpenAI** | 目标驱动的自我进化：智能体运行，将其输出与 JSON 形状的目标进行评分，并请求 LLM 提出修订后的图定义。当分数 ≥ 1.0 或达到 max_iters 时循环结束。演示了 JSON-as-program，其中修改器的唯一输出是新图规范。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | **OpenAI** | 每线程进化聊天智能体：持续的多轮对话；在轮次之间，智能体的 JSON 定义根据累积的历史被重写。演示了跨进化的检查点恢复（先前的消息得以保留）、`__graph_meta__` 审计通道模式，以及验证器边界（白名单节点类型、必需通道、边连接）。需要 `OPENAI_API_KEY`；没有它也会干净地退出。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | 离线 | 工具门（#89）：`engine.set_tool_gate(...)` 在**任何工具运行之前**对每次工具调用进行咨询，返回 Allow / Allow-with-rewritten-args / Deny / Interrupt。展示了规范的批准提示 — *“智能体想要运行 `rm -rf build/`。允许吗？”* — 并且，关键在于，无害的兄弟调用在人类做决定时**尚未**运行，因此拒绝真的意味着什么都没发生，而批准也不会重新运行它。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | 离线 | 并发工具（#96）：`ng.AsyncTool` 代替 `ng.Tool`，三个 300 毫秒的工具用时 0.30 秒而不是 0.90 秒。同时在同一次运行中测量边界 — 三个 *CPU-bound* 工具耗时是一个工具的 3.2 倍，因为 Python 函数在运行时持有 GIL，再多的线程也无法改变这一点。并发是可选的，因此现有有状态工具不会突然与自身产生竞态。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | 离线 | MCP (#95)： `ng.mcp.MCPClient(url).get_tools()` 拉取远程工具目录并将其直接交给 `NodeContext`。它启动自己的 MCP 服务器，因此无需网络即可运行。重复的命名调用默认保持串行；线程化演示通过 `fetch` 显式标记为 Reentrant，经由 `ToolExecutionPolicyRegistry`，然后测量三次 0.4 秒的 HTTP 调用在 0.41 秒内完成。stdio 也会复用 JSON-RPC ID，但重叠需要同时具备该主机策略和并发服务器。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | 本地主机 | A2A 托管（#120）：官方 `a2a-sdk` 拥有 JSON-RPC、任务状态、智能体卡片和取消功能。`ProtocolHostAdapter.stream()` 将引擎 token 事件映射为分片的 A2A 工件，同时保留检查点上下文。需要 Python 3.10+ 和 `pip install "neograph-engine[a2a]"`。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | 标准输入输出 | ACP 托管（#120）：流式传输令牌更新，为图保留文本/图像/音频/资源内容块，并支持持久化 `session/load` 当 `NEOGRAPH_ACP_POSTGRES_URL` 或 `NEOGRAPH_ACP_SQLITE_PATH` 被设置时。需要 Python 3.10+ 和 `pip install "neograph-engine[acp]"`. |

## 为什么托管使用官方 SDK

C++ 库拥有自己的 `A2AServer` 和 `ACPServer`，但直接暴露这些类会让 Python 用户获得第二个协议实现，且其与 Python 官方 SDK 的集成度更弱。特别是，官方 SDK 已经拥有当前的线格式兼容性、服务器传输、任务或会话生命周期，以及 asyncio 取消机制。NeoGraph 仅提供这些 SDK 无法提供的部分：对 C++ 图引擎的检查点感知调用。

| 条目 | 决策 |
|------|----------|
| 看似缺失的 C++ 功能 | `A2AServer`、`ACPServer`，且它们的生命周期方法未以 Python 类形式镜像。 |
| Python 替代方案 | Official `a2a-sdk` 1.x 和 `agent-client-protocol` 0.12.1 服务器运行时。 |
| NeoGraph 集成 | `ProtocolHostAdapter` 将协议会话ID映射到 `RunConfig.thread_id`，启用 `resume_if_exists`，流式传输 `LLM_TOKEN` 事件，接受自定义JSON安全输入载荷，并取消当前活动的asyncio任务。 |
| 依赖策略 | 两个SDK都是可选的，因为它们需要Python 3.10+，而`neograph-engine`支持Python 3.9。请安装`neograph-engine[a2a]`、`neograph-engine[acp]`或`neograph-engine[protocols]`。 |
| 持久 ACP 会话 | 为支持wheel的持久后端设置`NEOGRAPH_ACP_POSTGRES_URL`。使用`NEOGRAPH_BUILD_SQLITE=ON`的源码构建可设置`NEOGRAPH_ACP_SQLITE_PATH`。仅在配置了`session/load`时，代理才通告该属性；新会话在首次完成的提示创建检查点后才变为可加载。会话ID是服务器生成的凭证，检查点使用私有`acp:`线程命名空间。每个会话保持一个活动代理进程；检查点存储不跨进程序列化并发写入者。 |
| 当前限制 | 尚无法从共享的NeoGraph Python工具安全地调用ACP编辑器回调（`fs/read_text_file`、终端调用、权限提示）：今日的`AsyncTool`在工作线程上运行同步函数，且不携带当前协议会话ID。伪造桥接可能错误地调用编辑器会话。 |
| 重新审视直接绑定，当 | 用户必须在 Python 中嵌入完全相同的 C++ 服务器，否则官方 SDK 路径无法保留所需的 NeoGraph 取消、检查点、追踪或工具调用行为。 |

`ProtocolHostAdapter.run_payload()` 将任何 JSON 安全的值通过所配置的 `input_builder` 传递。默认的 `message_input` 将富内容块保留为用户消息的 `content`；如果某个图的提供方期望其他形状，则应传入自定义构建器。`ProtocolHostAdapter.stream()` 产生 `ProtocolStreamEvent(kind="token", ...)` 值，随后紧跟一个最终事件。实时 token 会被禁用，除非 `stream_node` 指定了其 token 恰好构成最终答案的图节点。这可防止规划器/工具节点的输出通过协议响应泄漏。asyncio 消费队列是有界的（默认 1,024 个块）；溢出将取消引擎运行。原生流事件首先调度到 asyncio 事件循环上，因此该队列是针对慢速协议传输的回压，而不是针对无界原生生产者的硬性整个进程级内存上限。

使用以下任一方式运行：

```bash
python 01_minimal.py
```

## 心智模型

从 Python 视角看，NeoGraph 类似于从 Python 视角看 LangGraph：一个由节点、带归约器的通道、通过 `Send` 实现的动态 fan-out、通过 `Command` 实现的路由覆盖，以及通过命名条件（`route_channel`、`has_tool_calls` 等）实现的边的图。使用相同的原语，相同的 JSON 形状的图定义。区别在于运行它的是什么——一个以每步几微秒执行超步循环、调度和检查点的 C++ 引擎，而 LangGraph 约为 600 µs。

三种模式贯穿于各示例中反复出现：

1. **Python 自定义节点**（01、03、04、07、09、10、11、12、13）继承 `neograph_engine.GraphNode` 并实现 `run(input)`。从 `input.state` 读取通道，在存在时使用 `input.stream_cb`，并返回写入、`Command`、`Send` 或 `NodeResult`。引擎在持有 GIL 的情况下调度进入 Python，从而并发自定义节点不会出现死锁。

2. **Python 工具**（02、06、07）继承 `neograph_engine.Tool` 并将实例传入 `NodeContext(tools=[…])`。引擎在编译时接管所有权；之后 Python 引用可以被释放。

3. **异步**（04）— 每个`*_async`绑定返回绑定到调用线程运行循环的`asyncio.Future`。流回调通过`loop.call_soon_threadsafe`被转移到循环线程上，因此你的`cb(ev)`在asyncio期望的位置运行。

## 图定义是 JSON

`GraphEngine.compile(definition, ctx)` 接受两种形式：一个你在代码中构建的 Python `dict`，或者一个你从文件中 `dict` 的 `json.loads()` —— 两者形状相同。示例 14 和 15 展示了往返过程。自定义节点*类型*仍然需要在代码中注册（Python 类无法编码为 JSON），但连线——通道、按类型区分的节点、边、条件边——是数据。

## 发行名称 vs 导入名称

PyPI 上的包是 **`neograph-engine`**（裸名称 `neograph` 已在 PyPI 上被无关项目占用）。Python 导入名称为 `neograph_engine`：

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
