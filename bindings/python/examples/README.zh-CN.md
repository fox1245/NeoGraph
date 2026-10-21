<!-- neograph-i18n: source=bindings/python/examples/README.md locale=zh-CN source_sha256=cdc4300edb845368b427f2b90eb0051e74fae14610f7039ae415a5bf6ed69281 -->
# Python API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这 47 个脚本演示图状态、路由、工具、provider 请求、Program API 和协议托管，每个只演示一件事。大多数能离线运行：由脚本化模型代替 provider，因此既不需要 API key，也不需要网络。也能调用真实模型的示例接受 `--live`。

**Offline** 列说明每一行需要什么。`yes`：无需 key 和网络即可运行，并由 `bindings/python/tests/test_examples_offline.py` 自动测试。`key`：需要 provider API key。`service`：需要其他进程、服务器或额外的包。`paired`：与该行中提到的示例一起离线运行。

## 设置

```bash
pip install neograph-engine
python 01_minimal.py
```

请在此目录中运行命令。离线示例不需要其他东西。若要改用源码构建而不是 wheel 运行，请用 `-DNEOGRAPH_BUILD_PYBIND=ON` 配置，并把 `PYTHONPATH` 设为构建目录和 `bindings/python`。构建目录不会安装这个包的 Python 依赖，请先运行 `pip install 'pydantic>=2' certifi`。`import neograph_engine` 需要 pydantic；没有 certifi 时，TLS 使用主机的信任库。

## 从这里开始

请按顺序阅读，每个示例只增加一个概念。它们都能离线运行。

| 步骤 | 示例 | Offline | 学到的内容 |
|---|---|---|---|
| 1 | [`01_minimal.py`](01_minimal.py) | yes | 第一个图：用 `@ng.node` 注册的函数读取一个通道、写入另一个通道，21 变成 42。 |
| 2 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python `Tool` 与内置 `tool_dispatch` 节点：脚本化的工具调用返回 `42`。 |
| 3 | [`03_send_fanout.py`](03_send_fanout.py) | yes | `Send` 在运行时为每个条目创建一个分支，各分支再合并：得到八个平方值，顺序不定。 |
| 4 | [`05_openai_provider.py`](05_openai_provider.py) | yes | 通过内置 `llm_call` 节点调用一次模型。脚本化模型回答 `hello world`；`--live` 调用托管模型。 |
| 5 | [`06_react_agent.py`](06_react_agent.py) | yes | ReAct 循环：`llm_call`、`tool_dispatch` 与 `has_tool_calls` 条件边。`calc` 返回 `4053`，然后模型作答。 |
| 6 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | Human-in-the-Loop：节点抛出 `NodeInterrupt`，检查点保存现场，`resume` 把人工回答交还给节点。 |
| 7 | [`29_program_hello.py`](29_program_hello.py) | yes | Program：一段简短的 JavaScript Program 定义带有 Python 节点的图，并在 `LocalProgramHost` 上运行。 |

完成第 7 步后，可以到下一张表按目标查找，或按层级浏览索引。

## 按目标查找

选一个目标，阅读对应的示例。各行由基础排到协议。

| 我想要…… | 阅读 | Offline |
|---|---|---|
| 运行我的第一个图 | [`01_minimal.py`](01_minimal.py) | yes |
| 给图添加 Python 工具 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes |
| 用 Send 做分支 fan-out | [`03_send_fanout.py`](03_send_fanout.py) | yes |
| 用 asyncio 同时运行多个图 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes |
| 从图中调用模型 | [`05_openai_provider.py`](05_openai_provider.py) | yes |
| 构建会用工具的智能体循环 | [`06_react_agent.py`](06_react_agent.py) | yes |
| 按意图路由 | [`08_intent_routing.py`](08_intent_routing.py) | yes |
| 一步内选择下一个节点并写入状态 | [`10_command_routing.py`](10_command_routing.py) | yes |
| 选择通道合并写入的方式（覆盖或追加） | [`79_channels.py`](79_channels.py) | yes |
| 注册我自己的归约器或边条件 | [`80_custom_registries.py`](80_custom_registries.py) | yes |
| 边运行边观察 | [`75_stream_modes.py`](75_stream_modes.py) | yes |
| 用退避策略重试失败的节点 | [`76_retry_policy.py`](76_retry_policy.py) | yes |
| 让节点在继续之前先询问人工 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes |
| 在步数上限处终止循环 | [`78_max_steps.py`](78_max_steps.py) | yes |
| 暂停等待批准后恢复 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes |
| 允许、拒绝或中断工具调用 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes |
| 读取并分叉已保存的状态 | [`09_state_management.py`](09_state_management.py) | yes |
| 把图保存为 JSON 并重新加载 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes |
| 把 token 流式输出为 LangChain 风格的块 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes |
| 用 OpenTelemetry 追踪运行 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes |
| 并发运行 I/O 密集型工具 | [`25_async_tools.py`](25_async_tools.py) | yes |
| 运行 JavaScript Program | [`29_program_hello.py`](29_program_hello.py) | yes |
| 合并并行分支，并弄清 join 为什么会卡住 | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes |
| 等待两个冗余来源中最先到的一个 | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes |
| 把失败的 Program 节点转交给操作员 | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes |
| 限定 Program 能花费的额度 | [`33_program_budgets.py`](33_program_budgets.py) | yes |
| 暂停并恢复 Program 运行 | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes |
| 分叉 Program 运行而不重复模型调用 | [`35_program_fork.py`](35_program_fork.py) | yes |
| 崩溃后恢复 Program 运行 | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes |
| 给 Program 节点绑定模型并读取它的用量 | [`37_program_model_node.py`](37_program_model_node.py) | yes |
| 根据编译器的诊断修复 Program | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes |
| 让两次运行竞速，并取消较慢的一个 | [`39_program_main_race.py`](39_program_main_race.py) | yes |
| 让耗时过长的节点尝试超时并重试 | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes |
| 使用 MCP 服务器的工具 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes |
| 通过 A2A 托管我的智能体 | [`27_a2a_server.py`](27_a2a_server.py) | yes |
| 从 Python 调用 A2A 智能体 | [`45_a2a_client.py`](45_a2a_client.py) | yes |
| 通过 ACP 托管我的智能体 | [`28_acp_agent.py`](28_acp_agent.py) | yes |
| 让智能体改写自己的图 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes |
| 尝试 Reflexion、Self-Ask 或辩论 | [`11_reflexion.py`](11_reflexion.py) | key |

## 按层级索引

### Core 引擎

节点、通道、路由、流式输出、重试和中断，不涉及模型。示例 14 会在脚本旁写出 `my_graph.json`，15 再读取它，请按此顺序运行。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 01 | [`01_minimal.py`](01_minimal.py) | yes | 用 `@ng.node` 注册的函数读取一个通道，翻倍后写回：21 变成 42。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python `Tool` 与内置 `tool_dispatch` 节点：脚本化的工具调用返回 `42`。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | yes | 八个 `Send` 分支合并出平方值 `[0, 1, 4, 9, 16, 25, 36, 49]`；不假定 append 的顺序。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes | 用 `asyncio.gather` 并发八次异步运行，得到 `[0, 2, 4, 6, 8, 10, 12, 14]`；流式运行会发出节点事件。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | yes | `Command` 选择下一个节点并写入状态：输入 200、50、-10 分别进入 accept、manual、reject。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | 图定义就是普通 JSON：先从 dict 运行，再保存为 `my_graph.json`。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | yes | 加载示例 14 生成的 `my_graph.json`，把 5 翻倍为 10、100 翻倍为 200。请先运行 14；自定义节点类型在代码中注册。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes | `message_stream` 把 token 事件转成 LangChain 风格的块；五个脚本化 token 组成 `Octopuses have three hearts.` |
| 75 | [`75_stream_modes.py`](75_stream_modes.py) | yes | 流模式：用 VALUES、UPDATES、EVENTS、DEBUG 四种模式观察同一次运行，并统计每种模式送达的事件。 |
| 76 | [`76_retry_policy.py`](76_retry_policy.py) | yes | `RetryPolicy`：节点失败两次后，引擎按指数退避重试；把重试预算调小，可以看到重试用尽时调用方得到什么。 |
| 77 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | `NodeInterrupt`：节点抛出它，载荷告诉调用方要问什么，`resume` 把答复（批准或拒绝）交还给节点。 |
| 78 | [`78_max_steps.py`](78_max_steps.py) | yes | `max_steps`：步数上限截停循环，结果会如实说明（`max_steps_exhausted`、`RunStatus.StepLimit`）；恰好在最后一步结束的运行不会被标记。 |
| 79 | [`79_channels.py`](79_channels.py) | yes | 通道与归约器：`overwrite` 与 `append` 的对比、`initial` 初始值，以及通道版本（所有通道共用一个写入计数器）。 |
| 80 | [`80_custom_registries.py`](80_custom_registries.py) | yes | 自定义归约器与条件：`ReducerRegistry.register_reducer` 无论顺序如何都保留三位评委的最高分，`ConditionRegistry.register_condition` 再按这个分数选择路径。 |

### 智能体模式

图中的模型（有无工具均可）。`key` 行会调用托管模型。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | yes | 通过 `llm_call` 调用一次模型；脚本化模型回答 `hello world`，`--live` 调用托管模型。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | yes | 基于 Responses 协议的 ReAct：模型发出的 `calc` 调用返回 `4053`，随后模型作答。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | yes | 三个问题通过 `route_channel` 分别路由到 math、translate、general 专家，每位专家写出答案。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | key | Reflexion：actor 与 critic 调用把反思带入下一次尝试；这是范围受限的教学版本。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | key | Self-Ask：中间问题和答案先累积在草稿区，再给出最终答案；没有接入搜索服务。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | key | 两个 `Send` 分支各执一方，一个裁判读取合并后的论据。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | key | 带研究模式的 Gradio 聊天：并行 researcher 使用模型知识而非网络搜索，生成三个问题的报告。需要 `gradio`。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | key | 同一个研究图，加上通过 Crawl4AI 的真实网络搜索和可选的 PostgreSQL 检查点。需要 `gradio`、`requests` 和 Crawl4AI 服务器。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes | 图给自己的输出打分，失败后请模型给出修订后的定义。含未注册节点类型的改写会被拒绝并给出原因；通过准入的改写会进入编译，第二次迭代达成目标。`--live` 调用托管模型。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | yes | 聊了几轮之后，evolver 模型提出新的智能体定义。`validate_agent` 准入该定义，新引擎共用检查点存储，因此之前的四条消息得以保留，`__graph_meta__` 记录版本和哈希（应用元数据，不是重放证据）。 |

### Program API

受限的 JavaScript Program API 运行在 Core 图之上：`ProgramRegistryBuilder`、`ProgramCompiler`、`LocalProgramHost`。它在 [Program 与 QuickJS](../../../docs/python-binding.md#program-and-quickjs) 中有说明，[`bindings/python/tests/program_e2e/`](../tests/program_e2e/README.md) 里的端到端场景覆盖了 join、失败路由、预算、暂停与恢复、分叉和崩溃恢复。下面的示例各演示其中一项，运行在 `LocalProgramHost` 上。它们都能离线运行，节点调用模型的地方由脚本化模型作答。共用的设置放在 `_program.py` 里，示例 29 把这些设置完整写出一次。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 29 | [`29_program_hello.py`](29_program_hello.py) | yes | 一段 JavaScript Program 定义带有 Python 节点的图并在 `LocalProgramHost` 上运行，输出 `status: Completed`、`value: 1`、`trace: ['work']`。 |
| 30 | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes | 三个审阅者并行运行，`barrier` 等待全部到齐。有一个审阅者改道后，Program 以 `Failed` 结束，给出 `P_CORE_BARRIER_STALLED` 和指明缺失节点的 witness。 |
| 31 | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes | 等待两个普通成员的 join，在有修正节点接替其中一个成员时会卡住。把编译器自己给出的建议，即 `{any_of: [...]}` 槽位，应用到源码后，修复好的 Program 顺利完成。 |
| 32 | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes | TEST 失败并重试两次后，最终失败被路由到带有 `interruptBefore` 的处理节点。操作员以 `retry` 恢复，Program 完成；以 `abort` 恢复，失败出口让 Program 以 `Failed` 和 `plan.user_abort` 结束。 |
| 33 | [`33_program_budgets.py`](33_program_budgets.py) | yes | 在 1,000 token 的预算下，两个已绑定的 `llm_call` 节点各报告 600 个 token，所以运行以 `BudgetExhausted` 结束。预算与已准入预算不同的运行会遭到拒绝，错误码 `P_START_BUDGET`。 |
| 34 | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes | `interruptBefore` 让运行停在节点 b 之前，`host.resume(run_id, value)` 接着运行。暂停时的 trace 是 `['a']`，恢复后是 `['b', 'c']`，`main()` 带着操作员的回答结束。 |
| 35 | [`35_program_fork.py`](35_program_fork.py) | yes | 紧接在已完成的模型调用之后暂停的运行，用另一个回答分叉。已完成的调用从检查点重放，因此脚本化模型总共只收到一次请求，分叉不再计入 token。 |
| 36 | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes | 进程 A 把 Program 运行到一个提问处，得到回答，发出模型调用后终止。进程 B 打开同一个 `sqlite_directory` 并调用 `reconnect`：运行完成，日志中记录的回复得到重放，这次调用只计费一次。适用于 Linux 和 macOS。 |
| 37 | [`37_program_model_node.py`](37_program_model_node.py) | yes | Program 的 `llm_call` 节点由宿主通过 `node_context` 绑定 provider。脚本化模型报告的 30 + 12 个 token 体现在 `usage.model_tokens: 42`、`remaining_budget` 和一条已计价的金额记录里。没有该绑定的宿主会以 `P_ADMIT_PROVIDER` 拒绝这个 Program。 |
| 38 | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes | 脚本化模型在边里写错了一个节点名。编译以 `P_CORE_E3`、修复建议和候选名失败。修复循环应用建议，用 `soundness_check=True` 重新编译并准入该 Program，模型只调用一次。 |
| 39 | [`39_program_main_race.py`](39_program_main_race.py) | yes | `main()` 在 `ng.race`、`ng.quorum` 和 `ng.await` 下启动多个 Core 运行。最先完成的获胜，慢的那个会看到取消令牌翻转，超过截止时间的 `ng.await` 让 Program 以 `TimedOut` 和 `P_AWAIT_TIMEOUT` 结束。 |
| 40 | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes | 耗时五秒的节点在 `attempt_timeout_ms` 到期后停止，以 `NG.Timeout` 失败，重试后成功，`usage.node_retries` 为 1。始终很慢的节点会让 Program 以 `Failed` 和 `NG.Timeout` 结束。 |

### 协议：MCP、A2A、ACP

这里的 MCP 和 A2A 是客户端（`ng.mcp`、`ng.a2a`）。A2A 和 ACP 智能体用官方 Python SDK 与 `ProtocolHostAdapter` 托管。这一层的示例都能离线运行：在空闲端口上启动自己的服务器，或把智能体作为子进程运行。使用 SDK 的示例需要 `neograph-engine[a2a]` 或 `neograph-engine[acp]`，缺少时会跳过；服务器由 `bindings/python/tests/test_protocol_sdk_e2e.py` 做更深入的测试。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes | 启动本地 MCP 服务器，发现 `fetch`，并用显式可重入策略执行三次调用。需要启用 MCP 的构建，冒烟测试在 Linux 上进行。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | yes | 通过官方 A2A SDK 托管智能体：`127.0.0.1:9999` 上的智能体卡片与 JSON-RPC，以及流式工件。`--self-check` 在空闲端口上启动服务器，并用 SDK 自带的客户端进行两轮对话。需要 `neograph-engine[a2a]`。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | yes | 通过官方 ACP SDK 在 stdio 上托管智能体：initialize、创建会话、prompt、token 更新、`end_turn`。`--self-check` 把智能体作为子进程运行，并用 SDK 的客户端进行两轮对话。需要 `neograph-engine[acp]`。 |
| 45 | [`45_a2a_client.py`](45_a2a_client.py) | yes | A2A 客户端：`ng.a2a.A2AClient` 获取智能体卡片、发送消息，并读取示例 27 的 SDK 智能体产生的流；服务器由脚本在空闲端口上用线程启动。需要 `neograph-engine[a2a]`。 |

### 持久化与运维

检查点、状态、缓存、追踪、传输与工具并发。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | 检查点在支付分发前暂停；批准后恰好恢复一次模拟支付。 |
| 09 | [`09_state_management.py`](09_state_management.py) | yes | `get_state` 与 `fork`：alpha 的 count 达到 11，beta 分叉该值，未知线程没有检查点。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | key | 跨两个主题的五次运行中，依赖 provider 的节点只执行两次；重复输入会重放缓存的写入。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes | 控制台输出该次运行和三个节点的 OpenTelemetry span，最终轨迹为 `['A', 'B', 'C']`。需要 `opentelemetry-api` 与 `opentelemetry-sdk`。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | key | 通过同一个 SDK 对比 HTTP/1.1 与 HTTP/2；需要 TLS Chat 端点和支持 HTTP/2 的 libcurl。结果只描述该端点，不构成排名。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes | 工具门在任一同级工具运行之前暂停；拒绝时只运行 `list_files`，批准时两者各运行一次。Shell 操作是模拟的。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | yes | 在 I/O 密集型任务上对比同步 `Tool` 与可重叠的 `AsyncTool`，再在 CPU 密集型任务上展示 GIL 的边界；时间比例只是观察值，冒烟测试在 Linux 上运行。 |

## 离线、脚本化与实时

由模型驱动的示例通过 `_common.py` 的 `example_provider(script)` 构建 provider。没有 `--live` 时由 `_scripted.ScriptedModel` 应答：它是 `127.0.0.1` 上的小型服务器，按脚本以 OpenAI Chat 或 Responses 协议作答（文本、工具调用、流式、用量、HTTP 失败）。真实 `SchemaProvider` 的请求构建、SSE 解析和 outcome 处理，都是针对它运行的。它不是模型：说什么由脚本决定。

```python
from _common import example_provider
from _scripted import call_tool, say

script = [call_tool("calc", expression="2 + 3"), say("5")]   # call 1 asks for a tool, call 2 answers
with example_provider(script) as chosen:
    provider = chosen.provider        # a SchemaProvider; chosen.scripted is None with --live
```

脚本也可以是读取 `turn.last_user_text`、`turn.system_text` 和 `turn.last_tool_result` 的函数 `(turn) -> Reply`。`chosen.scripted.call_count` 可用来断言发生了多少次模型调用。规则写在 `_scripted.py` 的 docstring 里。

`--live`、`NG_EXAMPLE_LIVE=1`、`OPENAI_API_BASE` 或 `NG_PROVIDER_DESCRIPTOR` 会改为选择已配置的端点。托管端点需要 `OPENAI_API_KEY`，并可选 `OPENAI_MODEL`（默认 `gpt-6-luna`），两者都只从进程环境读取；`_common.py` 不加载 `.env` 文件，缺少 key 是错误，而不是一次成功的运行。`_common.py` 先准入封闭的 OpenAI Chat 或 Responses 描述符，再构造 `SchemaProvider`。类型化 SDK 使用 libcurl HTTP；已移除的 provider 类、completion 参数、WebSocket 路径和传输后端选择器不再可用。`OPENAI_API_BASE` 是 origin 或网关前缀，不是完整的 `/v1/chat/completions` 路由；若 HTTPS 对端的 CA 不在系统信任库中，还需要 `NG_EXAMPLE_CA_FILE`。也可以改用 `NG_PROVIDER_DESCRIPTOR` 指定一个完整的已准入描述符 JSON 文件。类型化请求与 outcome 契约见 [Python 绑定指南](../../../docs/python-binding.md)。

## 状态与调度

节点读取当前超步的状态并返回通道写入。引擎在下一个超步之前，通过声明的归约器合并这些写入。`Send` 提供分支专用输入；`Command` 更新状态并选择下一个节点。在 CPython GIL 下，工作线程池不会使 CPU 密集型 Python 回调并行执行。

`GraphEngine.compile()` 既接受从 JSON 加载的字典，也接受用 Python 编写的字典。定义存储的是连接关系，而不是可执行的 Python 类：编译保存的定义前，请注册自定义节点工厂。示例 14 和 15 在脚本所在目录写入/读取 `my_graph.json`；验证这一对示例时，请使用可丢弃的检出副本或此目录的副本。

## 交互与协议场景

为 16/17 安装 `gradio`，启动脚本并访问打印出的本地 URL。先提交 `hello`，再提交 `research apples`。在 17 中，真实的 Crawl4AI `/md` 服务或忠实实现协议的本地对端必须返回 `{"success": true, "markdown": "..."}`。只有运行中的构建支持 Postgres 且数据库可达时，才配置 Postgres。请分别检查引擎状态和 UI 历史。

示例 27/28 用官方 Python SDK 处理 wire 层，用 `ProtocolHostAdapter` 完成能感知检查点的引擎执行。A2A 验证需要用 SDK 客户端获取智能体卡片、发送消息并消费流。ACP 验证需要客户端子进程连接，用来执行 `initialize`、`session/new` 和 `session/prompt`；stdout 专用于协议消息。在同一上下文/会话中发送第二条提示，预期得到 `(turn 2)`。取消应当停止正在进行的请求，而不是生成看起来像成功的兜底结果。

要使用持久化的 ACP 会话，请恰好设置 `NEOGRAPH_ACP_POSTGRES_URL` 和 `NEOGRAPH_ACP_SQLITE_PATH` 中的一个。第一条提示完成后，会创建 `session/load` 所需的检查点。每个会话只保留一个活动的智能体进程；检查点存储不会对跨进程的并发写入做串行化。这个示例会回显富内容块，但不解释图像/音频内容，也不提供编辑器的文件系统/终端回调。

## 发行名称与导入名称

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

发行包名为 `neograph-engine`；PyPI 上的 `neograph` 是无关项目。

## 添加示例

编号绝不复用，新示例请在现有最大编号的基础上加 1。新增一个示例只需要一个文件和表格中的一行。

1. 文件命名为 `NN_short_name.py`，开头写模块 docstring：几行说明、`Run:` 命令，然后是测试注解。
2. 在该 docstring 里写明如何测试。离线示例写出必须打印的一行；需要 key 或服务的示例写明为何不能离线运行（注解语法见 `examples/run_example_smoke.py` 开头）。
3. 由模型驱动的 provider 用 `example_provider(script)` 构建，使示例默认离线并接受 `--live`。
4. 在所属层级的表格里添加一行，并填上 Offline 值。
5. 运行 `python -m pytest bindings/python/tests/test_examples_offline.py` 和 `python scripts/check_examples.py`。如果示例缺少注解、编号重复，或表格与注解不一致，后者会失败。

```python
"""NN — one line saying what this shows.

Run:
    python NN_short_name.py           # scripted model, no key
    python NN_short_name.py --live    # hosted model; OPENAI_API_KEY in the environment

smoke-expect: provider: scripted
smoke-expect: final answer: 5
"""
```

示例无法离线运行时，用 `smoke-skip: <reason>` 代替 `smoke-expect` 行；`smoke-requires: module1, module2` 会在缺少模块时跳过测试。
