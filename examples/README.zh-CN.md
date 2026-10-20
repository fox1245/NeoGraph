<!-- neograph-i18n: source=examples/README.md locale=zh-CN source_sha256=c91d45d63240864a0784379f2d83e77b71cae1ad10655d83eeb3e31377ee2d81 -->
# C++ API 示例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

每个示例是一个文件，只演示一件事，从头到尾几分钟即可读完。大多数示例都能离线运行：由脚本化模型代替 provider，因此既不需要 API key，也不需要网络。也能调用真实模型的示例接受 `--live`。把其中一个复制到自己的项目并链接 `neograph::core`（由模型驱动的再加上 `neograph::llm`），就能从可运行的代码起步。

**Offline** 列说明每一行需要什么。`yes`：无需 key 和网络即可运行，并有自动冒烟测试（`ctest -L examples-offline`）。`key`：需要 provider API key。`service`：需要其他进程、服务器、数据库、显示器或模型。`paired`：与该行中提到的示例一起离线运行。

## 构建与运行

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_TESTS=ON
cmake --build build -j$(nproc)
./build/example_react_agent            # scripted model: no key, no network
./build/example_react_agent --live     # real model: OPENROUTER_API_KEY must be set in the environment
ctest --test-dir build -L examples-offline --output-on-failure   # run every offline example and check its output
```

可执行文件生成在构建目录中，名为 `example_<name>`；多数源文件在开头的 `Usage:` 行给出确切命令。基于 Program 的示例（63 到 69）需要 `-DNEOGRAPH_BUILD_PROGRAM=ON`；其中的 JavaScript 示例（65 到 69）还需要 `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`，69 还需要 SQLite。示例 76 需要 `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON`。gRPC 示例（45、53、54、55、57）需要 `-DNEOGRAPH_BUILD_GRPC=ON` 以及 `grpc++`/`protoc`。示例 11 需要 `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`。示例所属的组件被关闭时，它不会参与构建，也不会让构建失败。

## 从这里开始

请按顺序阅读，每个示例只增加一个概念。它们都能离线运行。

| 步骤 | 示例 | Offline | 学到的内容 |
|---|---|---|---|
| 1 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | 已安装的 `neograph::core` 目标：一个节点、一个严格图、一个带类型的通道。运行后打印 `HELLO`。 |
| 2 | [`51_minimal.cpp`](51_minimal.cpp) | yes | 通道，也就是图运行所用的状态：一个节点读取一个通道、写入两个通道，调用方用带类型的 `ChannelKey` 把两者读回来。 |
| 3 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | 条件边：`route_channel` 根据标签选出下一个节点，`default` 路由接住映射里没有列出的所有标签。 |
| 4 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | fan-out 与 fan-in：三个节点在同一个超步内运行，`barrier` 把它们汇合后交给后面的节点。 |
| 5 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | 一个模型、一个工具、一个循环：`llm::Agent` 在脚本化模型上运行 ReAct 智能体，工具是真实的计算器。`--live` 调用真实模型。 |
| 6 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | Human-in-the-Loop：节点暂停运行，检查点保存现场，`resume` 把人工回答交还给节点。 |
| 7 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | Program：JavaScript 的 `define()` 和 `main()` 从 C++ 宿主驱动一个 Core 图（需要 `-DNEOGRAPH_BUILD_PROGRAM=ON` 和 `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`）。 |

完成第 7 步后，可以到下一张表按目标查找，或按层级浏览索引。

## 按目标查找

选一个目标，阅读对应的示例。各行由基础排到协议。

| 我想要…… | 阅读 | Offline |
|---|---|---|
| 运行我的第一个图 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes |
| 把状态存进具名通道，并按类型读回 | [`51_minimal.cpp`](51_minimal.cpp) | yes |
| 不用协程写一个简短的节点 | [`61_sync_node.cpp`](61_sync_node.cpp) | yes |
| 按标签路由请求，并设置后备路由 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes |
| 把请求路由给合适的专家 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes |
| 并行运行多个步骤并合并结果 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes |
| 在运行时生成任务并回到前面的节点 | [`10_send_command.cpp`](10_send_command.cpp) | yes |
| 注册我自己的归约器或边条件 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes |
| 选择通道合并写入的方式（覆盖或追加） | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes |
| 把一个图嵌入另一个图 | [`06_subgraph.cpp`](06_subgraph.cpp) | yes |
| 给模型一个工具并循环到它给出答案 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes |
| 一次调用构建 ReAct 图 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes |
| 边运行边观察 | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes |
| 用退避策略重试失败的节点 | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes |
| 让节点在继续之前先询问人工 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes |
| 在某个节点之前停下图，之后再恢复 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes |
| 终止失控的循环 | [`103_max_steps.cpp`](103_max_steps.cpp) | yes |
| 读取、修改并分叉已保存的状态 | [`08_state_management.cpp`](08_state_management.cpp) | yes |
| 崩溃后无需重做已完成的工作即可恢复 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes |
| 跨多次调用延续对话 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes |
| 在多个线程间共享记忆 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes |
| 让长历史保持在预算之内 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes |
| 从另一个线程取消运行 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes |
| 跳过节点的重复计算 | [`47_node_cache.cpp`](47_node_cache.cpp) | yes |
| 把追踪发送到 OpenInference 后端 | [`49_openinference.cpp`](49_openinference.cpp) | yes |
| 编译并运行 Program | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes |
| 用 JavaScript 编写 Program | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes |
| 把失败的 Program 节点转交给操作员 | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes |
| 限定 Program 能花费的额度 | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes |
| 暂停、恢复或分叉 Program 运行 | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes |
| 崩溃后恢复 Program 运行 | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes |
| 把运行中的 Program 迁移到更好的拓扑 | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes |
| 从种子出发演化 harness | [`32_evolution.cpp`](32_evolution.cpp) | yes |
| 把工具作为 MCP 服务器提供出去 | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes |
| 通过 HTTP 提供需要 Bearer 令牌的 MCP 服务 | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes |
| 在 ReAct 循环中使用 MCP 工具 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes |
| 处理失败的 MCP 工具 | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes |
| 在 MCP 工具调用执行前先批准 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes |
| 通过 A2A 向其他智能体提供我的图 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes |
| 调用远程 A2A 智能体 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes |
| 把暂停的图变成等待输入的 A2A 任务 | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes |
| 通过 ACP 向编辑器提供我的图 | [`39_acp_server.cpp`](39_acp_server.cpp) | yes |
| 在有风险的步骤之前向编辑器请求许可 | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes |
| 通过 gRPC 提供图 | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes |
| 通过 gRPC 调用另一个进程中的工具 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes |
| 尝试 Reflexion 之类的推理模式 | [`15_reflexion.cpp`](15_reflexion.cpp) | key |
| 对接本地模型服务器运行 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service |

## 按层级索引

### Core 引擎

节点、通道、路由、fan-out、流式输出、重试和中断。它们只链接 `neograph::core`，只有 02、06、07 因为使用脚本化 provider，还会链接 `neograph::llm`。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 62 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | 已安装的 `neograph::core` 目标：一个节点、一个严格图、一个带类型的通道。 |
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes | 一次 `create_react_graph()` 调用即可构建 llm、tools、llm 的 ReAct 图，`run_stream` 打印节点事件。 |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | fan-out 与 fan-in：三个 researcher 节点同时等待定时器（用计数器验证），由 barrier 汇合后交给 summarizer。 |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | yes | 子图：外层 supervisor 图把工作委托给内层 ReAct 图。 |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes | 分类节点加条件边（`route_channel`），把每个请求送给 math、translate 或 general 专家。 |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | yes | 五项引擎功能的巡览，每项只有几行（`NodeInterrupt`、`RetryPolicy`、`StreamMode`、`Command`、`Store`），并各自指向讲解它的示例。 |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | yes | `Send` fan-out 与 `Command` 路由覆盖：planner、researcher、evaluator 循环到结果足够为止。 |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes | 在 C++ 中注册自己的通道归约器和边条件。 |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) | yes | 通道：一个节点读取一个通道、写入两个通道，调用方用带类型的 `ChannelKey` 读回两者（`result.channel(key)`、`try_channel`）。 |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) | yes | `NodeFactory::export_schema()` 打印拓扑的 JSON Schema，可视化编辑器用它生成自己的组件面板。 |
| 61 | [`61_sync_node.cpp`](61_sync_node.cpp) | yes | `SyncGraphNode`：两个无需协程样板代码的简短节点（去空白、转大写）；阻塞 I/O 仍应写在 `GraphNode::run()` 中。 |
| 100 | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes | `StreamMode`：用 VALUES、UPDATES、EVENTS、DEBUG 四种模式观察同一次运行，并统计每种模式送达的事件。 |
| 101 | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes | `RetryPolicy`：节点失败两次后，引擎按指数退避重试；把重试预算调小，可以看到重试用尽时调用方得到什么。 |
| 102 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | `NodeInterrupt`：节点自己暂停运行，载荷告诉调用方要问什么，`resume` 把答复（批准或拒绝）交还给节点。 |
| 103 | [`103_max_steps.cpp`](103_max_steps.cpp) | yes | `max_steps`：步数上限截停循环，结果会如实说明（`max_steps_exhausted()`、`RunStatus::StepLimit`）；恰好在最后一步结束的运行不会被标记。 |
| 104 | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes | 通道与归约器：`overwrite` 与 `append` 的对比、`initial` 初始值，以及通道版本（所有通道共用一个写入计数器）。 |
| 105 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | 条件边：`route_channel` 根据标签选出下一个节点，`default` 路由接住映射里没有列出的所有标签。 |

### 智能体模式

循环中的模型加工具，以及建立在此之上的已发表推理模式。`key` 行会调用真实模型。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | ReAct：一行 Thought、一次工具调用、Observation，然后给出最终答案。`--live` 调用真实模型。 |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | yes | 用 `Send` fan-out 出五个小型分类器，耗时等于最慢的那个，而不是总和。判定结果是预先写好的。 |
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | key | Reflexion：actor 与 critic 循环，直到 critic 接受（Shinn et al. 2023）。 |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | key | Tree of Thoughts：生成、评分、保留并扩展候选思路。 |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | key | Self-Ask：为多跳推理显式提出后续问题（Press et al. 2022）。 |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | key | researcher、skeptic、judge：三个系统提示词共享同一份对话记录。 |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | key | REWOO：planner 写出占位符，worker 并行执行工具，solver 汇总证据。 |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | key | 使用真实嵌入向量和内存余弦检索的检索增强生成。 |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | key | Corrective RAG：评估检索结果，然后精炼、追加网络搜索或只用网络搜索（Yan et al. 2024）。 |
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | key | 深度研究：supervisor 把工作 fan-out 给各自运行 ReAct 循环的子 researcher，再撰写报告。需要 `OPENROUTER_API_KEY` 和 Crawl4AI 服务器。 |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | key | 异步 ReAct：外层协程等待 `run_stream_async` 并打印带类型的文本增量。 |
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | key | 带有有序 `sp::Event` 观察值和自有 `sp::Outcome` 的类型化 Responses SSE 请求。 |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | key | 调试辅助：为一次工具调用请求转储完整的 Responses 信封和每个有序输出项。 |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | key | 对同一提示词依次尝试 `none`、`low`、`medium`、`high` 推理强度，比较延迟与推理 token。 |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | key | 通过 SSE 运行全部七个类型化托管工具部分，保留完整 outcome 与有序 wire 观察值。 |
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service | 指向 `127.0.0.1:8090` 上本地 OpenAI 兼容服务器（llama.cpp、vLLM）的类型化 Chat 客户端。 |

### Program API

Program 为 Core 图加上一套生命周期，带有持久标识、预算和准入(admission)。JavaScript Program API（join、失败路由、预算、暂停与恢复、分叉、崩溃恢复）由 [`bindings/python/tests/program_e2e/`](../bindings/python/tests/program_e2e/README.md) 中的 Python 端到端场景覆盖。示例 65 到 69 从 C++ 宿主运行 JavaScript Program，与 Python 示例 29 到 36 相对应，还需要 `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`。它们共用的注册表、准入(admission)和运行时设置放在 `program_support.h` 中；示例 63 用它运行以 C++ 构建器 JSON 写成的 Program。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 63 | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes | 编译带有一个 `call_core` 操作的 `ProgramSource`，准入后通过 `ProgramRuntime` 运行。 |
| 64 | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes | 检索到的拓扑候选，在持久的安全点上成为一条运行中谱系的已验证迁移。 |
| 65 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | C++ 宿主编译并运行一个带有 `define()` 和 `main()` 的 JavaScript Program。`main()` 用 `ng.callCore` 调用图两次：21 变成 42，再变成 84。 |
| 66 | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes | Python 示例 32 的 C++ 对应版本。TEST 抛出带作者自定代码的 `NodeFailureError`，重试两次后路由到带有 `interruptBefore` 的处理节点。用 `retry` 调用 `resume`，Program 完成；用 `abort`，则以 `failed` 和 `plan.user_abort` 结束。 |
| 67 | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes | C++ 中的三道预算限制：准入(admission)拒绝申请量超过宿主 `BudgetLimits` 上限的 Program（`P_ADMIT_POLICY`），启动时拒绝使用另一个 `RunBudget` 的运行（`P_START_BUDGET`），用完 `max_core_steps` 的循环以 `budget_exhausted` 结束。 |
| 68 | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes | Python 示例 34 和 35 的 C++ 对应版本。`interruptBefore` 让运行暂停，`resume` 带着操作员的回答继续，`fork` 则从检查点出发，用另一个回答回应同一个暂停的运行。已完成节点的函数体总共只执行一次。 |
| 69 | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes | Python 示例 36 的 C++ 对应版本，使用 SQLite 文件。进程 A 回答问题、执行 `charge` 后终止；进程 B 打开同一目录并调用 `reconnect`。运行在第二次尝试时完成；`charge` 执行了两次，所以它的副作用由账本保护。适用于 Linux 和 macOS。 |
| 70 | [`70_program_plan_intake.cpp`](70_program_plan_intake.cpp) | yes | 脚本化的规划者用扁平的 `step-plan/v1` 作答。入口节点拒绝第一次回答并给出三项违规，规划者收到违规后重新作答。第二次回答被降级为任务图：求出两个和，把它们相加，再把一个单词转成大写。 |
| 32 | [`32_evolution.cpp`](32_evolution.cpp) | yes | Harness 演化循环：针对任务对严格 Core 种子做变异，并以 JSON 输出谱系（`--smoke` 使用内置夹具）。 |
| 58 | [`58_harness_bench.cpp`](58_harness_bench.cpp) | yes | 针对三种反馈模式的 harness 收敛基准；`--smoke` 运行内置任务。 |
| 60 | [`60_harness_mcp_server.cpp`](60_harness_mcp_server.cpp) | yes | Harness MCP 服务器：六个工具编译并运行范围受限的多 worker 工作流。`NEOGRAPH_HARNESS_SMOKE=1` 会换成离线夹具 provider。 |

### 协议：MCP、A2A、ACP、gRPC

这些示例都能离线运行。MCP 客户端示例会启动只依赖标准库的服务器（`mcp_demo_server.py`，无需安装），并使用脚本化模型；A2A 与 ACP 示例把客户端和服务器放在同一进程里，或启动伴随进程。需要可选 CMake 选项的示例，会在对应行写明，例如 `-DNEOGRAPH_BUILD_GRPC=ON`。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 59 | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes | 基于 stdio 的 MCP 服务器：一个 `echo` 工具、输入输出 schema 和结构化内容。 |
| 76 | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes | 基于 streamable HTTP 的 MCP 服务器：在空闲端口上要求 Bearer 令牌，由 NeoGraph 自带的客户端调用；不带令牌的调用得到 HTTP 401（需要 `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON`）。 |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes | 基于 stdio 的 MCP：服务器是不带网络栈的子进程，由 ReAct 循环调用它的工具。使用脚本化模型和只依赖标准库的服务器。 |
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | yes | 基于 streamable HTTP 的 MCP：发现服务器的工具并交给 ReAct 循环。演示服务器只用 Python 标准库，模型是脚本化的。 |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes | 带人工批准的 MCP：`interrupt_before` 在 MCP 工具调用前暂停图，展示待执行的调用，再用 `resume()` 继续，不会重复模型调用。 |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | yes | 结合 `Send` 的 MCP：planner 为每个工具发出一次调用，三次调用在同一个超步内重叠执行。不使用模型。 |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | yes | 一个智能体把两个 MCP 服务器（HTTP 与 stdio）的工具合并为一个列表，它的两次调用分别发往不同的服务器。 |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | yes | MCP 上的操作员反馈：第一个回答只是猜测，操作员的回复进入历史记录，第二轮调用工具。 |
| 75 | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes | MCP 的失败：`isError` 结果和 JSON-RPC 错误是两种不同的响应。智能体读取两条消息后自行纠正。 |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | yes | 逆向工程智能体：通过 stdio 上的 GhidraMCP 为已剥离符号的二进制文件命名函数。用替身桥接和脚本化模型即可离线运行，`--live` 使用 Ghidra 和真实模型。 |
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes | A2A 服务器：以 SSE 流式输出，通过 A2A 0.3 与 1.0 提供图，并用自带客户端调用。传入 `serve` 可持续监听。 |
| 77 | [`77_a2a_dialects.cpp`](77_a2a_dialects.cpp) | yes | A2A 方言：智能体卡片列出 JSON-RPC 1.0 和 0.3；一个客户端据此选用 1.0，另一个客户端拿到只列出 0.3 的卡片，就选用 0.3。 |
| 78 | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes | A2A 的 `input-required`：在 `interrupt_before` 处停下的图成为等待输入的任务，客户端在同一任务上作答，并以工件的形式收到结果。 |
| 79 | [`79_a2a_collaboration.cpp`](79_a2a_collaboration.cpp) | yes | A2A 协作：两个智能体建立链接（先提议，再接受），通过邮箱交换带序号的信封；重复投递只生效一次。 |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes | A2A 客户端：获取智能体卡片、发送消息，并用 `A2ACallerNode` 在图中调用远程智能体。 |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) | yes | 基于 stdio 的 ACP 智能体，即编辑器驱动的形态；`--self-test` 在进程内运行 initialize、session/new 和 session/prompt。 |
| 80 | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes | ACP 权限请求：节点在删除前通过 `session/request_permission` 询问编辑器；脚本化的编辑器允许一次调用，拒绝下一次。 |
| 81 | [`81_acp_elicitation.cpp`](81_acp_elicitation.cpp) | yes | ACP elicitation：客户端声明了该能力时，节点用 `elicitation/create` 向用户提出表单问题；没有声明时则回退到默认值。 |
| 45 | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes | gRPC：通过 `GraphService` 提供 `GraphEngine`。`--self-test` 在一个进程内用空闲端口运行服务器和客户端，示例 53 是跨进程的客户端（需要 `-DNEOGRAPH_BUILD_GRPC=ON`）。 |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) | yes | gRPC：从普通客户端调用 `GraphService`（`Health`、`RunGraph`、流式）（需要 `-DNEOGRAPH_BUILD_GRPC=ON`）。 |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) | yes | gRPC：远程 `CheckpointStore` 即 `GrpcCheckpointStore`，并给出实测延迟（需要 `-DNEOGRAPH_BUILD_GRPC=ON`）。 |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) | yes | 在回环上测量并对比工具调用的 gRPC 与 JSON-RPC；这是测量而非通过/失败检查（需要 `-DNEOGRAPH_BUILD_GRPC=ON`）。 |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes | gRPC：把另一个进程中的工具当作本地 `neograph::Tool` 使用（需要 `-DNEOGRAPH_BUILD_GRPC=ON`）。 |

### 持久化与运维

检查点、状态、记忆、并发与可观测性。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes | Human-in-the-Loop：在支付节点前 `interrupt_before` 暂停、持久化检查点，批准后恢复。 |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | yes | 对已保存检查点使用 `get_state`、`update_state`、`fork`，并查看检查点历史。 |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes | 在 fan-out 中途模拟失败的 Plan and execute：恢复时只重跑失败的那个兄弟节点（pending writes）。 |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | key | 基于 PostgreSQL 的 deep-research HITL，在报告与恢复之间进程退出后仍能继续。需要 key、PostgreSQL 和 Crawl4AI（Docker Compose）。 |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes | 使用 `resume_if_exists` 的多轮对话：下一轮会重新加载已保存的检查点并在其后追加。 |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes | 在节点内读写跨线程的 `Store`，为每个用户个性化回复。 |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | yes | `:memory:` 上的 `SqliteCheckpointStore`：检查点、恢复与线程隔离（不演示文件持久化）。 |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes | 对话历史压缩：只有摘要干净时，才用摘要替换旧的文本轮次；原生分组会整体保留。 |
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | yes | 三次智能体运行通过 `engine->run_async()` 在一个 `io_context` 线程上交错执行。 |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) | yes | 带背压的固定 worker 池（`RequestQueue`）：限制在途工作，拒绝溢出的请求。 |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes | 协作式取消：每个子任务一个 `CancelToken::fork()`，父级 `cancel()` 会停止所有子任务。 |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) | yes | 按节点缓存结果：相同输入的第二次运行命中缓存，新输入则未命中。 |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) | yes | `AsyncTool`：协程形态的工具，同步的 `Tool::execute` 约定仍然接受它。 |
| 49 | [`49_openinference.cpp`](49_openinference.cpp) | yes | OpenInference 追踪：`graph.run > node.* > llm.complete` 与模拟 provider 一起组成一棵 span 树。 |

### 展示与 Cookbook

组合多项功能的较大程序。每个 Cookbook 项目都有自己的 README，参见 [Cookbook 索引](cookbook/README.md)。

| # | 示例 | Offline | 演示内容 |
|---|---|---|---|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | service | 用 C++ 写的桌面聊天：Clay 负责窗口布局，Raylib 负责绘制，回复由智能体流式输出。默认使用模拟，`--live` 使用 OpenRouter。需要 `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` 和显示器。 |

| 项目 | Offline | 演示内容 |
|---|---|---|
| [`cookbook/the-beast/`](cookbook/the-beast/) | yes | 自演化智能体：编写严格 Core JSON，经编译器把关，演化后通过检查点回滚。离线二进制使用桩作者，实时变体需要 key。 |
| [`cookbook/ai-assembly/`](cookbook/ai-assembly/) | yes | 四个 A2A 成员服务器，以及广播议案并统计投票的 Speaker。`scripts/run_session.sh --mock` 可离线运行，需把 `NEOGRAPH_BUILD_DIR` 设为构建目录。 |
| [`cookbook/jarvis/`](cookbook/jarvis/) | yes | 带有四路路由、MCP 工具、A2A 专家和持久记忆的元编排器。模拟构建：`-DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON -DNEOGRAPH_JARVIS_FORCE_MOCK=ON`。 |
| [`cookbook/minimal-mcp/`](cookbook/minimal-mcp/) | yes | MCP 客户端与一个 60 行、仅用标准库的 Python 服务器之间的一次往返；不使用模型。 |
| [`cookbook/multi_tenant_chatbot/`](cookbook/multi_tenant_chatbot/) | yes | 一个进程通过编译缓存为许多客户的拓扑提供服务。模拟二进制什么都不需要，实时版本需要 key。 |
| [`cookbook/self_evolving_chatbot/`](cookbook/self_evolving_chatbot/) | key | 按客户选择并演化拓扑的聊天服务器。ProgramChat 有 `--mock` 模式，Core 服务器需要 key。 |
| [`cookbook/topology-retrieval/`](cookbook/topology-retrieval/) | yes | 为示例 64 提供输入的候选选择（稠密、BM25、排名融合、重排）。`--mock` 需要 `pip install faiss-cpu numpy`。 |
| [`cookbook/byo-openai/`](cookbook/byo-openai/) 和 [`cookbook/openrouter-provider/`](cookbook/openrouter-provider/) | service | Python provider 配方：在节点内使用自己的 OpenAI 客户端，以及类型化的 OpenRouter 请求。需要 Chat 端点（一个 key，或本地对端，例如 `bindings/python/examples/_scripted.py` 里的脚本化模型）。 |

此表中的 `yes` 表示项目的默认运行不需要 key；做过冒烟测试的二进制会以 `cookbook.*` 的名字出现在 `ctest -L examples-offline` 中。

### 关于示例 11 与 35 的说明

示例 11 从源码构建 Raylib，在 Linux 上需要 X11 和 OpenGL 开发头文件（例如 `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`）。文本使用系统的 TrueType/OpenType 字体，通过 fontconfig 或常见安装路径查找。要指定字体文件，设置 `NEOGRAPH_CHAT_FONT`，也可以再设置 `NEOGRAPH_CHAT_BOLD_FONT` 和 `NEOGRAPH_CHAT_MONO_FONT`；`font.ttc#N` 选择字体集合中的第 N 个 face。按键：Enter 发送，Esc 清空输入，Ctrl+V 粘贴，Ctrl+Q 退出；回复流式输出期间，Send 按钮变为 Stop。

示例 35 用 `--bridge examples/ghidra_stand_in.py`（GhidraMCP 桥接的替身）和脚本化模型离线运行。用 `--live` 运行时，须通过 `GHIDRA_MCP_BRIDGE`（或 `--bridge`）指定 `bridge_mcp_ghidra.py` 脚本的路径。`GHIDRA_MCP_PYTHON` 选择解释器，默认值为 `python3`；`GHIDRA_SERVER_URL` 选择 GhidraMCP 后端端点，默认值为 `http://127.0.0.1:18080/`。实时运行前须启动带 GhidraMCP 后端的 Ghidra，并打开一个程序。示例在调用模型之前会发现工具并调用一次 `list_methods`，后端无响应时以状态 2 退出，因为某些 GhidraMCP 版本即使后端未运行也会列出全部工具。实时运行还需要非空的 `OPENROUTER_API_KEY`（在此之后检查），并会调用付费模型。

## 离线、脚本化与实时

由模型驱动的示例需要模型。离线示例使用 `scripted_provider.h`，它是一个 `neograph::Provider`，回复直接写在示例里：文本、工具调用、流式增量、用量和类型化失败。它与真实 provider 走同样的 prepare 和 dispatch 路径，所以图、工具和检查点的代码与最终发布的代码相同。它不是模型：说什么由脚本决定。

```cpp
#include "scripted_provider.h"

auto scripted = std::make_shared<examples::ScriptedProvider>(std::vector<examples::Reply>{
    examples::call_tool("calculator", {{"expression", "2 + 3"}}),   // call 1: ask for a tool
    examples::say("The answer is 5."),                              // call 2: the final answer
});
auto provider = examples::choose_provider(argc, argv, scripted);    // --live switches to OpenRouter
```

脚本也可以是请求的函数：`ScriptedProvider([](const examples::ScriptedTurn& turn) { ... })` 能看到历史记录、最后一条用户文本和工具结果，循环与并行 fan-out 需要这种写法。规则列在 `scripted_provider.h` 开头的注释里。

`--live`（或 `NG_EXAMPLE_LIVE=1`）会用 `OPENROUTER_API_KEY` 构建真实 provider。key 只从进程环境读取，这个辅助工具不读取任何文件。实时调用由 provider 计费。较早的仅实时示例仍通过 `cppdotenv` 加载 `.env` 文件，改写时会改用这个辅助工具。示例 25 和 26 还会使用本地 Crawl4AI 服务器；当前的安全 Crawl4AI 镜像需要非空的 `CRAWL4AI_API_TOKEN`，见示例 26 的 `.env.example`。

## 添加示例

编号绝不复用，新示例请在现有最大编号的基础上加 1。新增一个示例只需要一个源文件、一行 CMake 和表格中的一行。

1. 文件命名为 `NN_short_name.cpp`，开头写头部注释：几行说明、一行 `Usage:`，然后是测试注解。
2. 在该注释块里写明如何测试。离线示例写出必须打印的一行；需要 key 或服务的示例写明为何不能离线运行（注解语法见 `examples/run_example_smoke.py` 开头）。
3. 在 `examples/cmake/` 下的文件里用 `neograph_add_example(short_name NN_short_name.cpp LINK neograph::core neograph::llm)` 注册目标。ctest 条目由注解生成。
4. 在所属层级的表格里添加一行，并填上 Offline 值。
5. 运行 `ctest -L examples-offline` 和 `python scripts/check_examples.py`。如果示例缺少注解、编号重复，或表格与注解不一致，后者会失败。

```cpp
// smoke-expect: Final answer: 427
// smoke-skip: needs OPENROUTER_API_KEY (live model call)
```

上面两种注解二选一：示例要么有 `smoke-expect` 行，要么有一行 `smoke-skip`，不能同时存在。

## 类型化 C++ 迁移状态

当前 C++ recipe 使用五个类型化 SDK family 的自有 `ProviderRequest`、
有序 `sp::Event` 和不可变 `std::shared_ptr<const sp::Outcome>`（`Completion`/`Failure`）。
`ChatMessage`/`ChatTool` 是 portable 投影，不是 native 重放权限。
只 prepare 一次；durable 调用者将 claim/receipt 绑定到
`Provider::request_digest(prepared)`，并 dispatch 同一个 handle。
不要从打印的 JSON 重建 history，也不要把 failure 简化为最终文本。

canonical persistence 使用 `provider-message-v3`/`runtime-history-record-v3`；
portable 摘要不能替代 native record。optional control 由调用者选择，不暗中 clamp。
bounded call 需要真实 model fact；缺失为 `LimitUnknown`。
reservation/charged/held 与 nullable provider usage 独立，不更新 budget，也不是价格、forecast 或 invoice。

header-only `examples/provider_example_support.h` 使用真实 SDK runtime。
所有 Core 构建，包括 `NEOGRAPH_BUILD_LLM=OFF`，都需要 `SchemaProvider::runtime`。
CMake 3.20+ 按显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、已安装 runtime package、
固定 public GitHub source archive 的顺序选择 SDK。download fallback 默认启用；
使用已安装 package 或显式 source 的 offline 构建应设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。
安装 include root 是 `include/SchemaProvider`；执行 interface/capability 检查，
SDK package 为 alpha `0.4.0`（interface revision/shared ABI 7）。

保留的 interface-3 model-free C++ E2E 执行验证了 39 个编号 target：29 个 finite offline target 与实际
MCP/ACP/A2A/Harness 和 gRPC graph/检查点/tool 路径。gRPC-vs-JSON-RPC 测量示例也运行了，
但不检查返回值，不计为 behavioral E2E pass。22 个 live/外部模型路径与禁用的 Clay GUI 未验证，
没有调用公开 vendor 或授予新 grant。历史测量不是新迁移的 qualification；live 需要密钥、
network、model access 并产生费用。密钥、prompt、artifact 保持私密，不要把敏感 envelope/native
输出发布到公开 log。native archive 是 owner-private 认证 custody，不是加密或 vendor issuer 认证。

较早的 interface-4 证据单独记录：Linux x86_64 已执行 native research recovery、
ToT/Forge/rewrite helper 和 A2A 0.3/1.0 peer；11 个已安装 Python application 在分批运行中
完成 48 个无需凭据的 localhost 请求。已跟踪 evolution file 模式与 Plan resume 也按下文执行。
这些是限定范围的 local 运行，不是保留的完整 C++ suite 的重跑。
参见 [SDK interface-4 执行记录](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)。
不声称新的 hosted vendor、Windows/macOS/ARM64、HTTP/3 或 sanitizer 验证；
remote CI 与公开发布仍待完成。

## 保留的示例契约

这些源码契约基于 SDK `0.4.0`（interface 7）构建。上面记录的 C++ 运行使用的是较早的 SDK
interface，仍属历史证据，不是对当前 SDK 的验证，也不包含新的 live 调用。各 family 控制是 `ProviderControls` 的封闭类型化字段；
不支持的 family/origin/model 组合在 I/O 前拒绝。reasoning/sampling/tool 控制、
Responses 服务端保存 cursor、deployment header 和显式 portable Gemini history 见
[provider 参考](../docs/reference-en.md)。cursor 与 portable history 不授予 native 重放权限。

### 研究与慢推理路径

Deep Research (25 / 26) 对 supervisor、researcher、compression、final-report 的每个请求，
仅在已完成的空 `MaxTokens` outcome 没有 visible text，也没有有效或无效 client tool call 时，
允许最多两次额外 semantic call。输出 cap 加倍，但不超过 16,384。research-brief 调用不属于此
ladder。每次额外调用使用新 ordinal，通过原 bank 的准入(admission)，保留 outcome 和 usage；
不会更新 grant、hold 或 deadline。没有显式 deadline 时，通过一次无 effect 的 preparation
取得已配置 deadline，在 mediated invoke 前释放 preparation，然后固定 deadline。
显式 deadline 跳过这一步。Failure、observer/settlement 错误和已交付 streaming part 不触发额外调用。

空最终报告是错误。有内容的 `MaxTokens` 报告只在 public 投影中加 `Incomplete`，
不修改不可变 outcome，不重试部分文本。空 compression 耗尽 ladder 后返回 diagnostic，
不会伪造成功的 provider result。

示例 16 保持默认 8,192 cap 和每次 ask 的 300 秒 deadline，只对已完成的空响应最多调用三次；
不加倍 cap，也不重试 failure。空中间响应或无效 evaluator 分数不会被替换成零；截断的最终回答作为不完整结果以 1 退出。
示例 28 的 rewrite 请求 low effort 和 512 输出 token，
空/空白响应时原样返回原问题，provider timeout 为 180 秒。
这些路径设置不改变共享 factory 的默认值。

多客户 self-evolving chatbot 在有效 UTF-8 边界内将 judge 历史前缀限制为 200 字节，
并在异常时报告错误且以 1 退出。The Beast 的 `baldwin_llm --llm` 必须提供密钥，
不会改用 offline oracle。类型化请求使用 low effort 和默认 1,824 cap；只有已完成的空
`MaxTokens` 响应会以默认 3,648 cap 重新请求一次。原始 Outcome 和每次实际调用的 usage 均保留。
Provider failure、仍为空的响应或无法解析的运算名以 1 退出，而不会伪装成已完成的 learner。这些源码修复不是实时验证或解题成功声明。

### issue #190 / #317 的有限诊断

`NG_EXAMPLE_MAX_TOKENS` 是 ToT、Baldwin、Jarvis、`server_multi` 以及 inspection 示例 29/30 的显式正数单次调用输出上限。它包含隐藏的 reasoning，显式提供时永不加倍。未设置时，原始配方默认值保持不变。`NG_EXAMPLE_EMPTY_REASKS=0` 禁用现有的已完成空 `MaxTokens` 重新请求；最大/默认值对 ToT 为 2，对 Baldwin/Jarvis 为 1。这些是新的语义调用，绝不是对 failure 或已交付 tool-call 输出的重试。Jarvis 保留独立的 broker ordinal、原始准入(admission) bank/deadline 以及每个实际 Outcome/report。不会为了通过而禁用任何 thinking 设置，不会虚构模型上限信息，也不会续期金钱 grant。

以下控制项只会减少原始示例的工作量：

| 配方 | 控制项与原始默认值 | 禁用重新请求时的最大调用数 |
|---|---|---|
| ToT | `NG_TOT_DEPTH=3` (1..3), `NG_TOT_BRANCHING=3` (1..3), `NG_TOT_BEAM_WIDTH=5` (1..5) | 原始 37；1/1/1 时 3 |
| Baldwin（两种模式） | `NG_BALDWIN_POPULATION=6` (2/4/6), `NG_BALDWIN_GENERATIONS=4` (1..4) | 原始 48；2/1 时 4 |
| 多客户服务器 | `NG_MULTI_CUSTOMERS=5`, `NG_MULTI_TURNS=5` (均为 1..5) | 原始 90；1 个客户/4 轮时 14 |

在**另行预留宿主消费额度**后，从仓库 root 运行：

```sh
# Representative canaries, not substitutes for the unchanged original workload.
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_TOT_DEPTH=1 NG_TOT_BRANCHING=1 NG_TOT_BEAM_WIDTH=1 \
  ./build/example_tree_of_thoughts
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_BALDWIN_POPULATION=2 NG_BALDWIN_GENERATIONS=1 \
  ./build/cookbook_the_beast_baldwin_llm --llm
NG_EXAMPLE_MAX_TOKENS=8192 NG_MULTI_CUSTOMERS=1 NG_MULTI_TURNS=4 \
  ./build/cookbook_self_evolving_chatbot_multi
# These inspection commands retain sensitive native/provider payloads.
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_responses_envelope "Reply briefly to hello."
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_reasoning_effort
```

这五条命令最多 3 + 4 + 14 + 1 + 4 = 26 次模型调用和 212,992 个配置输出 token，输入 token 另计。它们既不证明美元价格，也不授权消费。请使用获准在其上花费的模型和路由，并保持日志私密。原始工作量 qualification 只去掉 shape override，仍记录显式 cap/重新请求选择；在这些显式设置下，完整的 ToT + Baldwin + 服务器最多 175 次调用 / 1,433,600 个配置输出 token。

ToT 必须输出非空的最终 `EXPR`/`CHECK`；请独立计算实际表达式，确认恰好使用一个 4、一个 7 和两个 8，且结果为 24。模型的评分不是算术 oracle。Baldwin 必须在两种模式下显示实际的 pipeline fitness 和命中数；不要强制特定的进化轨迹，也不要用 offline oracle 替代其 live learner。服务器必须以非空的模型/judge 输出到达所选的第四轮，且没有序列化/provider failure。30 是观察四种 effort 的 sweep：不承诺 reasoning token、耗时和正确性单调；空回答以 exit 2 退出并保留其 Outcome。29 可能合理地只返回 tool call 而没有回答文本；它是 envelope inspection，不是天气回答 benchmark。Provider failure 仍为非零退出。

### 检查点与演化

示例 08 保留 terminal 检查点 fork 后启动新 user turn 的流程，不演示暂停 reviewer 的 resume。
示例 14 根据实际 executor 次数计算节省数：首次五次调用后只重跑一个失败 sibling，节省四次。

示例 32 在选择 smoke/file 模式前注册 `pnoop`。从 repository root 使用已跟踪 seed/task 文件：

```bash
./build/example_evolution --smoke
./build/example_evolution examples/32_evolution_seed.json examples/32_evolution_task.json
```

检查实际 JSON 字段 `best.compiled`、`best.validated`、`best.executed`、`best.correct`；
仅有 `compile_passed` 不能证明正确执行。file 模式提供 built-in node type 和此 demo 的 `pnoop`，
custom type 仍需 host 注册。当前 Linux x86_64 file 模式使用此 seed/task pair，
四个 `best` flag 均为 true；未注册 node type 则失败，没有成功的 compile/execute/correct 结果。

### A2A dialect 与 task snapshot

示例 37 打印 card interface 和首次 RPC 选择的 dialect。client 选择兼容的 JSON-RPC 0.x/1.0
card interface；card URL 不会重定向已配置 RPC endpoint。未 fetch card 时，仅数值 `-32601`
允许初始 dialect probe，交付 SSE 后不会重发。示例 38 广告两个 dialect，并标注初始/更新
task snapshot；初始 task 不是已完成回答。server response encoding 由 `A2A-Version` header
选择，与 method 拼写无关。1.0 使用 PascalCase method、flat part、`returnImmediately`，
stream 累积 status/artifact update。caller 按完成/中断的 agent status text、首个 artifact text、
最后 agent history text 的顺序选择答案。

## 心智模型 — 三层，中间是 JSON

每个示例都属于三种设置之一：

1. **只用内置 node**（02, 04, 07, 14）：`llm_call` / `tool_dispatch`
   / mock-provider node — 图完全由 JSON wiring，不需要 subclassing。最接近 `create_react_graph()` 产生的内容。
2. **自定义 `GraphNode` subclass**（05, 09, 10, 25）：完全控制
   `run(NodeInput)` body — 通过 `NodeOutput` 发出 `ChannelWrite`、`Send` 或 `Command`。
   Send fan-out 与 Command 路由覆盖就在这里。
3. **类型化 provider 请求与 Outcome** (13, 15, 16, 17)：SDK 准入(admission)、有序事件与不可变 Outcome；没有 descriptor interpreter 或 WebSocket adapter。

图定义采用 JSON 形式（`std::map<std::string, json>`）。
[Python examples](../bindings/python/examples/) 使用相同的拓扑格式；
provider 请求与 Outcome 是独立类型化对象，不是拓扑 JSON。

## Responses inspection 契约 (13 / 29 / 30 / 34)

13 提交类型化 Responses streaming request；事件不是字符串 callback。
29 保留成功/失败的完整 Outcome、`wire_envelope`、所有有序 `wire_output` item 和类型化 part、
function argument、citation、reasoning、opaque hosted output 与 artifact。
raw inspection 输出含敏感内容，不是安全的 telemetry/export 格式。
30 扫描 `none`、`low`、`medium`、`high` 并保留每个完整 Outcome。
reasoning/input/output/total/provider-reported-total 及 extra count 是 nullable 64-bit evidence，
包含 stage、quality、conflict。缺失不等于 zero；visible text 与预留不是 usage。
34 保留全部七部分：function(calculator)、web search、image generation、file search、
tool search、`shell.environment` 的 skills 和 shell(`container_auto`)。
`OPENROUTER_VECTOR_STORE_ID` 是 file search 的前提；`OPENROUTER_SKILL_ID` 覆盖默认
`openai-spreadsheets` skill。本 inspection demo 只声明 function tool，不在本地执行。
保留全部有序类型化/raw event 和完整 terminal。hosted tool 可能得不到所选 route 的支持，或产生额外费用；
类型化准入(admission)不是 live 兼容保证。不保留 WebSocket/primitive 可运行示例。
图像、视频和 decisions 是 SchemaProvider 端点，通过 `Provider::complete_endpoint` 调用
（参见参考文档的“端点调用”）。NeoGraph 0.16.0 已移除类型化客户端
`ImagesClient`、`VeoClient` 和 `DecisionsClient`；参见 CHANGELOG。
