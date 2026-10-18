# C++ API examples

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

Each example is one file that shows one thing and reads top to bottom in a couple of minutes. Most of them run offline: a scripted model stands in for the provider, so they need no API key and no network. Examples that can also call a real model take `--live`. Copy one into your own project and link `neograph::core` (and `neograph::llm` for model-driven ones) to start from working code.

The **Offline** column says what a row needs. `yes`: runs with no key and no network and has an automated smoke test (`ctest -L examples-offline`). `key`: needs a provider API key. `service`: needs another process, server, database, display or model. `paired`: runs offline together with the example named in its row.

## Build and run

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_TESTS=ON
cmake --build build -j$(nproc)
./build/example_react_agent            # scripted model: no key, no network
./build/example_react_agent --live     # real model: OPENROUTER_API_KEY must be set in the environment
ctest --test-dir build -L examples-offline --output-on-failure   # run every offline example and check its output
```

Binaries land in the build directory as `example_<name>`; most source files give the exact command in a `Usage:` line at the top. Program-backed examples (63 to 69) need `-DNEOGRAPH_BUILD_PROGRAM=ON`; the JavaScript ones (65 to 69) also need `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`, and 69 needs SQLite. Example 76 needs `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON`. The gRPC examples (45, 53, 54, 55, 57) need `-DNEOGRAPH_BUILD_GRPC=ON` and `grpc++`/`protoc`. Example 11 needs `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`. An example whose component is switched off is left out of the build; it does not fail it.

## Start here

Read these in order; each one adds a single idea. All of them run offline.

| Step | Example | Offline | You learn |
|---|---|---|---|
| 1 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | The installed `neograph::core` target: one node, one strict graph, one typed channel. It prints `HELLO`. |
| 2 | [`51_minimal.cpp`](51_minimal.cpp) | yes | Channels, the state a graph runs on: one node reads a channel and writes two, and the caller reads both back with typed `ChannelKey`s. |
| 3 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | Conditional edges: `route_channel` picks the next node from a label, and a `default` route catches every label the map does not list. |
| 4 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | Fan-out and fan-in: three nodes run in the same super-step and a `barrier` joins them for the node that follows. |
| 5 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | A model, a tool and a loop: `llm::Agent` runs a ReAct agent on a scripted model with a real calculator tool. `--live` calls a real model. |
| 6 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | Human in the loop: a node pauses the run, a checkpoint keeps it, and `resume` hands the person's answer back to the node. |
| 7 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | A Program: JavaScript `define()` and `main()` drive a Core graph from a C++ host (needs `-DNEOGRAPH_BUILD_PROGRAM=ON` and `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`). |

After step 7, pick by goal in the next table, or browse the index by tier.

## I want to ...

Pick the goal, read the example. The rows run from the basics to the protocols.

| I want to ... | Read | Offline |
|---|---|---|
| run my first graph | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes |
| keep state in named channels and read it back with types | [`51_minimal.cpp`](51_minimal.cpp) | yes |
| write a short node without coroutines | [`61_sync_node.cpp`](61_sync_node.cpp) | yes |
| route a request by a label, with a fallback | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes |
| route a request to the right expert | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes |
| run steps in parallel and join the results | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes |
| create work at run time and loop back | [`10_send_command.cpp`](10_send_command.cpp) | yes |
| register my own reducer or edge condition | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes |
| choose how a channel combines writes (overwrite or append) | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes |
| put one graph inside another | [`06_subgraph.cpp`](06_subgraph.cpp) | yes |
| give a model a tool and loop until it answers | [`01_react_agent.cpp`](01_react_agent.cpp) | yes |
| build a ReAct graph in one call | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes |
| watch a run as it happens | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes |
| retry a failing node with backoff | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes |
| let a node ask a person before it goes on | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes |
| stop a graph in front of a node and resume it | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes |
| stop a runaway loop | [`103_max_steps.cpp`](103_max_steps.cpp) | yes |
| read, edit and fork saved state | [`08_state_management.cpp`](08_state_management.cpp) | yes |
| recover from a crash without redoing finished work | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes |
| continue a chat across calls | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes |
| share memory across threads | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes |
| keep a long history inside a budget | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes |
| cancel a run from another thread | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes |
| skip recomputing a node | [`47_node_cache.cpp`](47_node_cache.cpp) | yes |
| send traces to an OpenInference backend | [`49_openinference.cpp`](49_openinference.cpp) | yes |
| compile and run a Program | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes |
| write a Program in JavaScript | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes |
| send a failing Program node to an operator | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes |
| bound what a Program may spend | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes |
| pause, resume or fork a Program run | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes |
| recover a Program run after a crash | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes |
| move a running Program to a better topology | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes |
| evolve a harness from a seed | [`32_evolution.cpp`](32_evolution.cpp) | yes |
| offer tools as an MCP server | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes |
| serve MCP over HTTP with a bearer token | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes |
| use MCP tools inside a ReAct loop | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes |
| handle an MCP tool that fails | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes |
| approve an MCP tool call before it runs | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes |
| serve my graph to other agents over A2A | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes |
| call a remote A2A agent | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes |
| turn a paused graph into an A2A task that waits for input | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes |
| serve my graph to an editor over ACP | [`39_acp_server.cpp`](39_acp_server.cpp) | yes |
| ask the editor for permission before a risky step | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes |
| serve a graph over gRPC | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes |
| call a tool in another process over gRPC | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes |
| try a reasoning pattern such as Reflexion | [`15_reflexion.cpp`](15_reflexion.cpp) | key |
| run against a local model server | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service |

## Index by tier

### Core engine

Nodes, channels, routing, fan-out, streaming, retries and interrupts. They link only `neograph::core`, except 02, 06 and 07, which also link `neograph::llm` for their scripted provider.

| # | Example | Offline | Shows |
|---|---|---|---|
| 62 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | The installed `neograph::core` target: one node, one strict graph, one typed channel. |
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes | `create_react_graph()` builds the llm, tools, llm ReAct graph in one call; `run_stream` prints node events. |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | Fan-out and fan-in: three researcher nodes wait on timers at the same time (a counter proves it) and a barrier joins them for the summarizer. |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | yes | A subgraph: an outer supervisor graph delegates to an inner ReAct graph. |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes | A classifier node and conditional edges (`route_channel`) send each request to a math, translate or general expert. |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | yes | A tour of five engine features in a few lines each (`NodeInterrupt`, `RetryPolicy`, `StreamMode`, `Command`, `Store`), each pointing to the example that teaches it. |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | yes | `Send` fan-out and a `Command` route override: planner, researchers and evaluator loop until enough findings exist. |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes | Register your own channel reducer and edge condition from C++. |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) | yes | Channels: one node reads one channel, writes two, and the caller reads both back with typed `ChannelKey`s (`result.channel(key)`, `try_channel`). |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) | yes | `NodeFactory::export_schema()` prints the topology JSON Schema that a visual editor builds its palette from. |
| 61 | [`61_sync_node.cpp`](61_sync_node.cpp) | yes | `SyncGraphNode`: two short nodes (trim, then uppercase) without coroutine boilerplate; blocking I/O still belongs in `GraphNode::run()`. |
| 100 | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes | `StreamMode`: the same run observed in VALUES, UPDATES, EVENTS and DEBUG mode, with the events each mode delivers counted. |
| 101 | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes | `RetryPolicy`: a node fails twice and the engine retries it with exponential backoff; a smaller budget shows what the caller gets when the retries run out. |
| 102 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | `NodeInterrupt`: a node pauses the run itself, the payload tells the caller what to ask, and `resume` hands the answer back to the node (approve or refuse). |
| 103 | [`103_max_steps.cpp`](103_max_steps.cpp) | yes | `max_steps`: a loop is stopped by the step limit and the result says so (`max_steps_exhausted()`, `RunStatus::StepLimit`); a run that ends on the last step is not flagged. |
| 104 | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes | Channels and reducers: `overwrite` against `append`, `initial` values, and channel versions as one shared write counter. |
| 105 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | Conditional edges: `route_channel` picks the next node from a label, and the `default` route catches every label the map does not list. |

### Agent patterns

A model plus tools in a loop, and the published reasoning patterns built on that. The `key` rows call a real model.

| # | Example | Offline | Shows |
|---|---|---|---|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | ReAct: a Thought line, a tool call, an Observation, then the final answer. `--live` calls a real model. |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | yes | Five small classifiers fan out with `Send`; wall time is the slowest one, not the sum. Verdicts are canned. |
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | key | Reflexion: an actor and a critic loop until the critic accepts (Shinn et al. 2023). |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | key | Tree of Thoughts: generate, score, keep and expand candidate thoughts. |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | key | Self-Ask: explicit follow-up questions for multi-hop reasoning (Press et al. 2022). |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | key | Researcher, skeptic and judge: three system prompts over one shared transcript. |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | key | REWOO: a planner writes placeholders, workers run the tools in parallel, a solver combines the evidence. |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | key | Retrieval-augmented generation with live embeddings and an in-memory cosine search. |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | key | Corrective RAG: grade the retrieval, then refine it, add web search, or search the web only (Yan et al. 2024). |
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | key | Deep research: a supervisor fans out sub-researchers that each run a ReAct loop, then writes a report. Needs `OPENROUTER_API_KEY` and a Crawl4AI server. |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | key | Async ReAct: an outer coroutine awaits `run_stream_async` and prints typed text deltas. |
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | key | A typed Responses SSE request with ordered `sp::Event` observations and an owned `sp::Outcome`. |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | key | Debug aid: dump the full Responses envelope and every ordered output item for one tool-call request. |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | key | Sweep `none`, `low`, `medium` and `high` reasoning effort on one prompt and compare latency and reasoning tokens. |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | key | All seven typed hosted-tool sections over SSE, with full outcomes and ordered wire observations. |
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service | A typed Chat client pointed at a local OpenAI-compatible server (llama.cpp, vLLM) on `127.0.0.1:8090`. |

### Program API

Program wraps a Core graph in a lifecycle with durable identities, budgets and admission. The JavaScript Program API (joins, failure routing, budgets, pause and resume, fork, crash recovery) is exercised by the Python end-to-end scenarios in [`bindings/python/tests/program_e2e/`](../bindings/python/tests/program_e2e/README.md). Examples 65 to 69 run JavaScript Programs from a C++ host and mirror the Python examples 29 to 36. They also need `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`. `program_support.h` holds the registry, admission and runtime setup they share; example 63 uses it with a Program written as C++ builder JSON.

| # | Example | Offline | Shows |
|---|---|---|---|
| 63 | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes | Compile a `ProgramSource` with one `call_core` operation, admit it, run it through `ProgramRuntime`. |
| 64 | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes | A retrieved topology candidate becomes a verified migration of one running lineage at a durable safe point. |
| 65 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | A C++ host compiles a JavaScript Program with `define()` and `main()` and runs it. `main()` calls the graph twice with `ng.callCore`: 21 becomes 42, then 84. |
| 66 | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes | The C++ twin of Python 32. TEST throws a `NodeFailureError` with an author code, is retried twice and routed to a handler with `interruptBefore`. `resume` with `retry` completes the Program; `abort` ends it `failed` with `plan.user_abort`. |
| 67 | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes | Three budget limits in C++: admission refuses a Program that asks for more than the host's `BudgetLimits` ceiling (`P_ADMIT_POLICY`), a run with another `RunBudget` is refused at start (`P_START_BUDGET`), and a loop that spends its `max_core_steps` ends `budget_exhausted`. |
| 68 | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes | The C++ twin of Python 34 and 35. `interruptBefore` pauses a run, `resume` continues it with the operator's answer, and `fork` answers the same paused run a second way from its checkpoint. The finished node's body runs once in total. |
| 69 | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes | The C++ twin of Python 36, on SQLite files. Process A answers a question, runs `charge` and dies; process B opens the same directory and calls `reconnect`. The run completes on its second attempt; `charge` ran twice, so its effect is guarded by a ledger. Linux and macOS. |
| 70 | [`70_program_plan_intake.cpp`](70_program_plan_intake.cpp) | yes | A scripted planner answers with a flat `step-plan/v1`; the intake node rejects the first answer with three violations, the planner is asked again with them, and the second answer is lowered into a task graph that computes two sums, adds them and upper-cases a word. |
| 32 | [`32_evolution.cpp`](32_evolution.cpp) | yes | Harness evolution loop: mutate a strict Core seed against a task and print the lineage as JSON (`--smoke` uses a built-in fixture). |
| 58 | [`58_harness_bench.cpp`](58_harness_bench.cpp) | yes | Harness convergence benchmark over three feedback modes; `--smoke` runs a built-in task. |
| 60 | [`60_harness_mcp_server.cpp`](60_harness_mcp_server.cpp) | yes | The Harness MCP server: six tools compile and run a bounded multi-worker workflow. `NEOGRAPH_HARNESS_SMOKE=1` swaps in an offline fixture provider. |

### Protocols: MCP, A2A, ACP, gRPC

These examples run offline. The MCP client examples start a standard-library server (`mcp_demo_server.py`, nothing to install) and use a scripted model; the A2A and ACP ones keep client and server in one process or start a companion. A row names the opt-in CMake option its example needs, such as `-DNEOGRAPH_BUILD_GRPC=ON`.

| # | Example | Offline | Shows |
|---|---|---|---|
| 59 | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes | MCP server over stdio with one `echo` tool, input and output schemas, and structured content. |
| 76 | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes | MCP server over streamable HTTP with a bearer token on a free port, called by NeoGraph's own client; a call without the token gets HTTP 401 (needs `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON`). |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes | MCP over stdio: the server is a child process with no network stack, and a ReAct loop calls its tools. Scripted model, standard-library server. |
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | yes | MCP over streamable HTTP: discover a server's tools and give them to a ReAct loop. The demo server is standard-library Python and the model is scripted. |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes | MCP with human approval: `interrupt_before` stops the graph ahead of an MCP tool call, the pending call is shown, and `resume()` continues without repeating the model call. |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | yes | MCP with `Send`: a planner emits one call per tool and the three calls overlap in one super-step. No model. |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | yes | One agent, two MCP servers (HTTP and stdio) merged into one tool list; its two calls go to different servers. |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | yes | Operator feedback over MCP: the first answer is a guess, the operator's reply joins the history, and the second round calls the tool. |
| 75 | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes | MCP failures: an `isError` result and a JSON-RPC error are different responses; an agent reads both messages and corrects itself. |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | yes | Reverse-engineering agent: GhidraMCP over stdio names the functions of a stripped binary. A stand-in bridge and a scripted model run it offline; `--live` uses Ghidra and a real model. |
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes | A2A server: serve a graph over A2A 0.3 and 1.0 with SSE streaming, then call it with its own client. Pass `serve` to keep listening. |
| 77 | [`77_a2a_dialects.cpp`](77_a2a_dialects.cpp) | yes | A2A dialects: the agent card lists JSON-RPC 1.0 and 0.3; one client takes 1.0 from it, and a second client, given a card with only 0.3, takes that. |
| 78 | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes | A2A `input-required`: a graph that stops at `interrupt_before` becomes a task that waits; the client answers on the same task and receives the result as an artifact. |
| 79 | [`79_a2a_collaboration.cpp`](79_a2a_collaboration.cpp) | yes | A2A collaboration: two agents link (proposed, then accepted) and exchange sequenced envelopes through a mailbox; a redelivery is applied once. |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes | A2A client: fetch the agent card, send a message, and call a remote agent from a graph with `A2ACallerNode`. |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) | yes | ACP agent over stdio, the shape an editor drives; `--self-test` runs initialize, session/new and session/prompt in process. |
| 80 | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes | ACP permission: a node asks the editor with `session/request_permission` before a deletion; a scripted editor allows one call and rejects the next. |
| 81 | [`81_acp_elicitation.cpp`](81_acp_elicitation.cpp) | yes | ACP elicitation: a node asks the user a form question with `elicitation/create` when the client declared the capability, and falls back to a default when it did not. |
| 45 | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes | gRPC: serve a `GraphEngine` over `GraphService`. `--self-test` runs server and client in one process on a free port; example 53 is the cross-process client (needs `-DNEOGRAPH_BUILD_GRPC=ON`). |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) | yes | gRPC: call `GraphService` (`Health`, `RunGraph`, streaming) from a plain client (needs `-DNEOGRAPH_BUILD_GRPC=ON`). |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) | yes | gRPC: `GrpcCheckpointStore`, a remote `CheckpointStore`, with a measured latency (needs `-DNEOGRAPH_BUILD_GRPC=ON`). |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) | yes | gRPC against JSON-RPC for tool calling, measured on loopback; a measurement, not a pass/fail check (needs `-DNEOGRAPH_BUILD_GRPC=ON`). |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes | gRPC: a tool in another process used as a local `neograph::Tool` (needs `-DNEOGRAPH_BUILD_GRPC=ON`). |

### Persistence and operations

Checkpoints, state, memory, concurrency and observability.

| # | Example | Offline | Shows |
|---|---|---|---|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes | Human in the loop: `interrupt_before` a payment node, persist a checkpoint, resume after approval. |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | yes | `get_state`, `update_state` and `fork` on saved checkpoints, and the checkpoint history. |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes | Plan and execute with a simulated failure mid fan-out: on resume only the failed sibling runs again (pending writes). |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | key | PostgreSQL-backed deep-research HITL that survives a process exit between report and resume. Needs a key, PostgreSQL and Crawl4AI (Docker Compose). |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes | A multi-turn chat with `resume_if_exists`: the next turn reloads the saved checkpoint and appends to it. |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes | A cross-thread `Store` read and written from inside nodes to personalise replies per user. |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | yes | `SqliteCheckpointStore` on `:memory:`: checkpoint, resume and thread isolation (no file durability shown). |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes | Conversation history compaction: old text turns become a summary only when the summary is clean; native groups are kept whole. |
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | yes | Three agent runs interleave on one `io_context` thread through `engine->run_async()`. |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) | yes | A fixed worker pool with backpressure (`RequestQueue`): bounded in-flight work, rejected overflow. |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes | Cooperative cancellation: `CancelToken::fork()` per child, a parent `cancel()` stops all of them. |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) | yes | A per-node result cache: the second run with the same input hits the cache, a new input misses. |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) | yes | `AsyncTool`: a coroutine-shaped tool that the sync `Tool::execute` contract still accepts. |
| 49 | [`49_openinference.cpp`](49_openinference.cpp) | yes | OpenInference tracing: `graph.run > node.* > llm.complete` as one span tree with a mock provider. |

### Showcases and cookbook

Larger programs that compose several features. Each cookbook project has its own README; see the [cookbook index](cookbook/README.md).

| # | Example | Offline | Shows |
|---|---|---|---|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | service | A desktop chat in C++: Clay lays out the window, Raylib draws it, replies stream from an agent. Mock by default, `--live` for OpenRouter. Needs `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` and a display. |

| Project | Offline | Shows |
|---|---|---|
| [`cookbook/the-beast/`](cookbook/the-beast/) | yes | A self-evolving agent that writes strict Core JSON, gates it through the compiler, evolves it and rolls back through checkpoints. The offline binaries use stub authors; the live variants need a key. |
| [`cookbook/ai-assembly/`](cookbook/ai-assembly/) | yes | Four A2A member servers and a Speaker that broadcasts a bill and tallies votes. `scripts/run_session.sh --mock` runs it offline; set `NEOGRAPH_BUILD_DIR` to your build directory. |
| [`cookbook/jarvis/`](cookbook/jarvis/) | yes | A meta-orchestrator with a four-way router, MCP tools, A2A specialists and stored memory. Mock build: `-DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON -DNEOGRAPH_JARVIS_FORCE_MOCK=ON`. |
| [`cookbook/minimal-mcp/`](cookbook/minimal-mcp/) | yes | An MCP client round trip against a 60-line standard-library Python server; no model. |
| [`cookbook/multi_tenant_chatbot/`](cookbook/multi_tenant_chatbot/) | yes | One process serving many customers' topologies through a compile cache. The mock binary needs nothing; the live one needs a key. |
| [`cookbook/self_evolving_chatbot/`](cookbook/self_evolving_chatbot/) | key | Chat servers that choose and evolve topologies per customer. ProgramChat has a `--mock` mode; the Core servers need a key. |
| [`cookbook/topology-retrieval/`](cookbook/topology-retrieval/) | yes | Candidate selection (dense, BM25, rank fusion, rerank) feeding example 64. `--mock` needs `pip install faiss-cpu numpy`. |
| [`cookbook/byo-openai/`](cookbook/byo-openai/) and [`cookbook/openrouter-provider/`](cookbook/openrouter-provider/) | service | Python provider recipes: your own OpenAI client inside a node, and typed OpenRouter requests. They need a Chat endpoint (a key, or a local peer such as the scripted model of `bindings/python/examples/_scripted.py`). |

In this table `yes` means the project's default run needs no key; the binaries that are smoke-tested show up in `ctest -L examples-offline` as `cookbook.*`.

### Notes on examples 11 and 35

Example 11 builds Raylib from source; on Linux that needs the X11 and OpenGL development headers (for example `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`). Text uses a system TrueType/OpenType font found through fontconfig or common install paths. Set `NEOGRAPH_CHAT_FONT`, and optionally `NEOGRAPH_CHAT_BOLD_FONT` and `NEOGRAPH_CHAT_MONO_FONT`, to choose the files; `font.ttc#N` selects face N of a collection. Keys: Enter sends, Esc clears the input, Ctrl+V pastes, Ctrl+Q quits, and while a reply streams the Send button becomes Stop.

Example 35 runs offline with `--bridge examples/ghidra_stand_in.py`, a stand-in for the GhidraMCP bridge, and a scripted model. With `--live` it needs `GHIDRA_MCP_BRIDGE` (or `--bridge`) to name your `bridge_mcp_ghidra.py` script. `GHIDRA_MCP_PYTHON` selects its interpreter (default `python3`); `GHIDRA_SERVER_URL` selects the GhidraMCP backend endpoint (default `http://127.0.0.1:18080/`). Start Ghidra with the GhidraMCP backend and a program open before a live run. Before any model call, the example discovers the tools and makes one `list_methods` call; it exits with status 2 when the backend does not answer, because some GhidraMCP releases list every tool while their backend is down. A live run also requires a nonempty `OPENROUTER_API_KEY`, checked after that, and makes paid model calls.

## Offline, scripted and live

A model-driven example needs a model. The offline ones use `scripted_provider.h`, a `neograph::Provider` whose replies are written in the example itself: text, tool calls, streamed deltas, usage and typed failures. It goes through the same prepare and dispatch path as the real provider, so the graph, tool and checkpoint code is the code you would ship. It is not a model: the script decides what is said.

```cpp
#include "scripted_provider.h"

auto scripted = std::make_shared<examples::ScriptedProvider>(std::vector<examples::Reply>{
    examples::call_tool("calculator", {{"expression", "2 + 3"}}),   // call 1: ask for a tool
    examples::say("The answer is 5."),                              // call 2: the final answer
});
auto provider = examples::choose_provider(argc, argv, scripted);    // --live switches to OpenRouter
```

A script can also be a function of the request: `ScriptedProvider([](const examples::ScriptedTurn& turn) { ... })` sees the history, the last user text and the tool results, which is what loops and parallel fan-out need. The comment at the top of `scripted_provider.h` lists the rules.

`--live` (or `NG_EXAMPLE_LIVE=1`) builds the real provider from `OPENROUTER_API_KEY`. The key is read from the process environment only and the helper reads no file. Live calls are billed by the provider. Older live-only examples still load a `.env` file through `cppdotenv`; they move to the helper as they are rewritten. Examples 25 and 26 also use a local Crawl4AI server; current secure Crawl4AI images need a non-empty `CRAWL4AI_API_TOKEN`, see example 26's `.env.example`.

## Add an example

Numbers are never reused, so take one higher than every existing number. A new example is one source file, one line of CMake and one table row.

1. Name the file `NN_short_name.cpp` and start it with a header comment: what it shows in a few lines, a `Usage:` line, then the test annotations.
2. Say how it is tested, in that comment block. An offline example names a line it must print; one that needs a key or a service says why it cannot run offline (the annotation grammar is at the top of `examples/run_example_smoke.py`).
3. Register the target with `neograph_add_example(short_name NN_short_name.cpp LINK neograph::core neograph::llm)` in a file under `examples/cmake/`. Its ctest entry comes from the annotations.
4. Add a row, with its Offline value, to the table of its tier.
5. Run `ctest -L examples-offline` and `python scripts/check_examples.py`. The second fails when an example has no annotation, a number is used twice, or a table disagrees with the annotations.

```cpp
// smoke-expect: Final answer: 427
// smoke-skip: needs OPENROUTER_API_KEY (live model call)
```

The two annotations above are alternatives: an example has `smoke-expect` lines or one `smoke-skip` line, not both.

## Typed C++ cutover status

The current C++ recipes use owned `ProviderRequest` payloads from the five typed
SDK families, ordered `sp::Event` observations and immutable
`std::shared_ptr<const sp::Outcome>` terminals (`Completion` or `Failure`).
`ChatMessage` / `ChatTool` are portable projections, not native replay authority.
Prepare exactly once; durable callers bind their claim/receipt to
`Provider::request_digest(prepared)` and dispatch that same prepared handle.
Do not reconstruct history from printed JSON or flatten failures to final text.

Canonical persistence uses `provider-message-v2` and `runtime-history-record-v2`;
portable summaries never replace native records. Optional controls are caller-chosen,
not silently clamped. Bounded calls require genuine model facts; missing facts are
`LimitUnknown`. Reservation/charged/held amounts are distinct from nullable provider
usage and do not renew a budget or represent a price, forecast or invoice.

The header-only `examples/provider_example_support.h` helper uses the real SDK
runtime; it is not a replacement transport. Every Core build, including
`NEOGRAPH_BUILD_LLM=OFF`, requires `SchemaProvider::runtime`. With CMake 3.20+,
SDK resolution uses an explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` first,
then an installed runtime package, then the pinned public GitHub source archive.
The download fallback is enabled by default; set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`
for offline builds with an installed package or explicit source checkout.
Installed SDK include root is `include/SchemaProvider`. Interface/capability
checks are enforced; the SDK package is alpha `0.3.5` (interface revision/shared ABI 6).

The preserved interface-3 model-free C++ E2E run verified 39 numbered targets: 29 finite offline
targets plus actual MCP/ACP/A2A/Harness and gRPC graph/checkpoint/tool paths.
The gRPC-vs-JSON-RPC measurement example also ran, but its unchecked return values
are not a behavioral E2E pass. Twenty-two live/external-model scopes and the
disabled Clay GUI remain unqualified; no public vendor request or new grant was used.
Historical timings below are not new-cutover qualification. Live runs incur
provider charges and require explicit keys/network/model access. Keep keys,
prompts and artifacts private; envelope/native inspection demos print sensitive
payloads and must not feed public logs. A native archive provides authenticated,
owner-private custody, not encryption or vendor-issuer authentication.

Earlier interface-4 evidence is separate: Linux x86_64 runs exercised native
research recovery, ToT/Forge/rewrite helpers and A2A 0.3/1.0 peers, plus 11 installed
Python applications across 48 credential-free localhost requests in split cohorts.
The tracked evolution file mode and Plan resume were also executed as described
below. These are scoped local runs, not a repeat of the preserved full C++ suite.
See the [SDK interface-4 execution record](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record).
No new hosted-vendor, Windows/macOS/ARM64, HTTP/3 or sanitizer qualification is
claimed; remote CI and publication remain pending.

## Retained example contracts

These source contracts build against SDK `0.3.5` (interface 6). The recorded C++ runs above were
made with older SDK interfaces and remain historical evidence; they are not a qualification of
the current SDK or new live calls.
Family-specific controls are closed typed fields on `ProviderControls`; unsupported
family/origin/model combinations reject before I/O. See the
[provider reference](../docs/reference-en.md) for reasoning/sampling/tool controls,
Responses provider-held cursors, deployment headers and explicit portable Gemini
history. A cursor or portable history does not grant native replay authority.

### Research and slow reasoning routes

Deep Research (25 / 26) permits at most two additional semantic calls for each
supervisor, researcher, compression or final-report request only after a completed
empty `MaxTokens` outcome with no visible text and no valid or invalid client tool
call. The output cap doubles, never above 16,384. The research-brief call is outside
this ladder. Each additional call has a new ordinal, passes the original bank's
admission and retains its outcome and usage; no grant, hold or deadline is renewed.
Without an explicit deadline, one effect-free preparation discovers the configured
deadline and is released before mediated invocation; that deadline stays pinned.
An explicit deadline skips discovery. Failure, observer/settlement errors and
already-delivered streaming parts do not trigger another call.

An empty final report is an error. Nonempty `MaxTokens` report text is published
with an `Incomplete` annotation only in the public projection; the immutable outcome
is unchanged and the partial text is not retried. Exhausted empty compression yields
a diagnostic, not a fabricated successful provider result.

Example 16 defaults to an 8,192-token cap across at most three completed-empty calls
and pins one 300-second deadline per ask; it does not double the cap or retry failures.
Empty intermediate replies and invalid evaluator scores fail instead of becoming
score zero. A truncated final expression remains incomplete and exits 1.
Example 28's rewrite requests low effort and 512 output tokens, returns the original
question unchanged when the reply is empty/whitespace, and uses a 180-second provider
timeout. These path-specific settings leave the shared factory default unchanged.

The multi-customer self-evolving chatbot keeps judge-history prefixes within 200 bytes
at valid UTF-8 boundaries and reports exceptions with exit 1. The Beast's
`baldwin_llm --llm` requires a key instead of substituting its offline oracle. Its
typed request uses low effort and a default 1,824-token cap; only an empty completed
`MaxTokens` reply gets one default 3,648-token re-ask. Every original Outcome and
actual call's usage remains owned. Provider failures, empty exhausted replies and
unparseable operation names exit 1 rather than masquerading as a completed learner.
These source fixes are not a live-provider qualification or a puzzle-success claim.

### Finite diagnostics for issues #190 / #317

`NG_EXAMPLE_MAX_TOKENS` is an explicit positive per-call output cap for ToT,
Baldwin, Jarvis, `server_multi` and inspection examples 29/30. It includes hidden
reasoning and is never doubled when explicitly supplied. When unset, original
recipe defaults remain unchanged. `NG_EXAMPLE_EMPTY_REASKS=0` disables the existing
completed-empty `MaxTokens` re-asks; maximum/default is 2 for ToT and 1 for
Baldwin/Jarvis. These are new semantic calls, never retries of failures or delivered
tool-call output. Jarvis keeps separate broker ordinals, the original admission
bank/deadline and every actual Outcome/report. No thinking setting is disabled
to obtain a pass, no model-limit facts are invented and no monetary grant is renewed.

The following controls only reduce the original example workloads:

| Recipe | Controls and original defaults | Maximum calls with re-asks disabled |
|---|---|---|
| ToT | `NG_TOT_DEPTH=3` (1..3), `NG_TOT_BRANCHING=3` (1..3), `NG_TOT_BEAM_WIDTH=5` (1..5) | 37 original; 3 at 1/1/1 |
| Baldwin, both modes | `NG_BALDWIN_POPULATION=6` (2/4/6), `NG_BALDWIN_GENERATIONS=4` (1..4) | 48 original; 4 at 2/1 |
| Multi-customer server | `NG_MULTI_CUSTOMERS=5`, `NG_MULTI_TURNS=5` (both 1..5) | 90 original; 14 at 1 customer/4 turns |

After a **separate host spending reservation**, from the repository root:

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

These five commands bound 3 + 4 + 14 + 1 + 4 = 26 model calls and 212,992
configured output tokens, with input tokens separate. They neither prove a dollar
price nor authorize spending. Use a model and route you are allowed to spend on, and keep
logs private. Original-workload qualification omits only the shape overrides, leaving
explicit cap/re-ask choices recorded; full ToT + Baldwin + server is at most
175 calls / 1,433,600 configured output tokens at these explicit settings.

ToT must print a nonempty final `EXPR`/`CHECK`; independently evaluate the actual
expression, verify exactly one 4, one 7 and two 8s and a result of 24. The model's
score is not an arithmetic oracle. Baldwin must show actual pipeline fitness and
hit counts for both modes; do not force a particular evolutionary trace or replace
its live learner with the offline oracle. The server must reach the selected fourth
turn with nonempty model/judge output and no serialization/provider failure.
30 is an observational four-effort sweep: reasoning tokens, wall time and correctness
are not promised to be monotonic; an empty answer gives exit 2 with its Outcome
retained. 29 may legitimately return a tool call without answer text; it is envelope
inspection, not a weather-answer benchmark. Provider failures remain nonzero.

### Checkpoints and evolution

Example 08 retains its terminal-checkpoint fork followed by a new user turn; it
does not demonstrate resuming a paused reviewer. Example 14 computes saved executor
calls from the actual count: five initial calls, one failed-sibling rerun, four saved.

Example 32 registers `pnoop` before selecting either smoke or file mode. From the
repository root, use the tracked seed/task pair:

```bash
./build/example_evolution --smoke
./build/example_evolution examples/32_evolution_seed.json examples/32_evolution_task.json
```

Inspect the actual JSON fields `best.compiled`, `best.validated`, `best.executed`
and `best.correct`; `compile_passed` alone does not establish correct execution.
Built-in node types and this demo's `pnoop` are available in file mode; custom types
still require host registration. The current Linux x86_64 file-mode run using this
seed/task pair returned all four `best` flags true; an unregistered node type
failed without a successful compile/execute/correct result.

### A2A dialects and task snapshots

Example 37 reports the card's interfaces and the dialect selected on its first
RPC. The client selects compatible JSON-RPC 0.x or 1.0 card interfaces; card URLs
do not redirect the configured RPC endpoint. Without a fetched card, a numeric
`-32601` permits the initial dialect probe, never a replay after delivered SSE.
Example 38 advertises both dialects and labels opening/updated task snapshots;
an opening task is not a completed answer. The `A2A-Version` header selects server
response encoding, independently of method spelling. In 1.0, requests use PascalCase
methods, flat parts and `returnImmediately`; streams accumulate status/artifact
updates. Caller answer precedence is final/interrupted agent status text, then first
artifact text, then the last agent history text.

## Mental model — three layers, JSON in the middle

Each example is one of three setups:

1. **Built-in nodes only** (02, 04, 07, 14): `llm_call` / `tool_dispatch`
   / mock-provider node — graph wired entirely from JSON, no
   subclassing. Closest to what `create_react_graph()` produces.
2. **Custom `GraphNode` subclass** (05, 09, 10, 25): you control the
   exact `run(NodeInput)` body — emit `ChannelWrite`, `Send`, or
   `Command` through `NodeOutput`. This is where Send fan-out and
   Command routing overrides live.
3. **Typed provider requests and Outcomes** (13, 15, 16, 17): validated SDK admission, ordered events and immutable Outcomes; no descriptor interpreter or WebSocket adapter.

The graph definition is JSON-shaped (`std::map<std::string, json>`).
The [Python examples](../bindings/python/examples/) use the same topology format;
their provider requests and Outcomes are separate typed objects, not topology JSON.

## Responses inspection contracts (13 / 29 / 30 / 34)

13 submits a typed Responses streaming request; events are not a string-only
callback. 29 retains both successful and failed Outcomes: the full response
`wire_envelope`, every ordered `wire_output` item and typed part, function
arguments, citations, reasoning, opaque hosted output and artifacts. Its raw
inspection output is sensitive, not a sanitized telemetry/export format.
30 retains each full Outcome while sweeping `none`, `low`, `medium`, `high`;
reasoning/input/output/total/provider-reported-total and extra counts are nullable
64-bit evidence with stage, quality and conflicts. Missing is not zero; visible
answer text does not replace the Outcome or turn a reservation into usage.
34 keeps all seven sections: function (calculator), web search, image generation,
file search, tool search, skills mounted in `shell.environment`, and shell
(`container_auto`). `OPENROUTER_VECTOR_STORE_ID` gates file search;
`OPENROUTER_SKILL_ID` overrides the curated `openai-spreadsheets` skill.
Function tools are advertised, not locally executed by this inspection demo.
All ordered typed/raw events and full terminals are retained. Hosted tools may
be unsupported by the chosen route or incur additional charges; typed admission
is not a live compatibility guarantee. No WebSocket/primitive runnable example
is retained. Images/Veo/Decisions are separate typed NeoGraph clients and do not
inherit a chat-run spending grant. They are deprecated, and 0.16.0 removes them in favour of
SchemaProvider endpoints:
- Google shuts down every Veo model `VeoClient` admits on 2026-10-22.
- OpenAI shuts down `gpt-image-1` on 2026-10-23, and `gpt-image-1-mini` and `gpt-image-1.5` on
  2026-12-01.

See the CHANGELOG.

Decisions choice criteria are an object keyed by choice, not a string array.
The current separate typed client expresses the valid shape without a raw
`SchemaProvider::request_json()` body:

```cpp
#include <neograph/llm/decisions_client.h>
neograph::llm::DecisionsChoiceQuestion question;
question.instructions = "Choose whether the supplied answer addresses the question.";
question.criteria = {{"accept", "The answer addresses the question."},
                     {"reject", "The answer does not address the question."}};
neograph::llm::DecisionsRequest request;
request.state = {{"question", "What is 2 + 2?"}, {"answer", "4"}};
request.questions.emplace("decision", std::move(question));
```

This is request construction, not a new paid request or proof of vendor accuracy.
