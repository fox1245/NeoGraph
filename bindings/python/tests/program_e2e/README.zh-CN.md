<!-- neograph-i18n: source=bindings/python/tests/program_e2e/README.md locale=zh-CN source_sha256=7b7c2d31c9130445d402371c02e5f533e14937b4c8cb0455bc5a0679df21122c -->
# Program API 端到端场景

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这些测试按照用户阅读 [Python 指南](../../../../docs/python-binding.md)后的使用方式，通过 Python 绑定驱动 sealed-v1 JavaScript Program API：Python `GraphNode` 类、已注册的条件、`ProgramRegistryBuilder` 快照、`ProgramCompiler` 和 `LocalProgramHost`。它们需要启用 Program 和 QuickJS 控制构建的 `neograph_engine`；缺少其中任何一项时，此包会被跳过。

## 覆盖范围

| 文件 | 场景 |
|---|---|
| `test_01_quickstart.py` | 文档中的示例、使用 Python 节点和条件的 `define()` 图、跨进程的标识、激活与回滚 |
| `test_02_hitl.py` | `interruptBefore`、`NodeInterrupt`、默认和显式的 pending id、`main()` 内的中断、进程重启后的恢复 |
| `test_03_failures.py` | 节点抛出异常、fan-out 分支失败、`main()` 抛出异常、失败的 `callCore`、没有提供方的 `llm_call` |
| `test_04_budgets.py` | 编译期和运行期的 JavaScript 超时、步数和操作预算、墙钟时间取消、准入预算 |
| `test_05_diagnostics.py` | 语法错误和构建器错误的源码位置、注册表缺失、lint 范围、控制字符、准入错误 |
| `test_06_graph_shapes.py` | 屏障汇合、条件路由、`Send` 映射归约、子图、无效的图形状 |
| `test_07_main_programs.py` | `callCore`、`all`、`race`、`quorum`、`await` 超时、`cancelScope`、外部取消、`spawn` |
| `test_08_live_llm.py` | 节点调用模型的 Program：默认使用本地对端，启用后使用 OpenRouter；Python 节点与绑定的 `llm_call` |
| `test_09_bound_llm.py` | 通过 `node_context` 绑定的 `llm_call`（#372）：报告费用与估算费用、发送前的拒绝（预算、未知模型、router alias）、零预算、无提供商 Program 的 id、同一预算下的 `all` 与 `quorum`、调用记入 journal 后的崩溃以及只收费一次的恢复 |
| `test_10_failure_routing.py` | 失败路由（#364）：失败的 TEST 带着记录被路由到有 `interruptBefore` 的处理节点，沿 retry 和 abort 路由恢复，作者代码，经过脱敏的错误文本，`max_visits`，`max_node_retries` 上限，节点内的 Program 诊断，以及使用 SQLite 在新进程中恢复 |
| `test_11_attempt_timeout.py` | 尝试超时（#364）：超时后在重试中恢复的慢 Python 节点，总是超时并以 `NG.Timeout` 路由到处理器的节点（没有处理器时运行失败），从不出现在结果和轨迹中的迟到完成，超过停止截止时间仍忽略取消的节点（`NG.EffectUncertain`），以及会越过 `wall_time_ms` 的退避以 kind `deadline` 停止 |
| `test_12_send_interrupts.py` | 抛出 `NodeInterrupt` 的 `Send` target（#385）：Program 在其上暂停，恢复时重放 map 节点和已完成的 Send，使用 SQLite 在重启后恢复，调用了已绑定 `llm_call` 并已完成的 Send 既不会重发也不会再次计费 |
| `test_13_fork_replays_the_paused_step.py` | 在已完成的绑定 `llm_call` 旁暂停的运行的 `host.fork()`（#390）：fork 回答问题并从该步的 pending write 重放已完成的调用，因此请求不会再次发送，令牌和费用也不会再次计费；覆盖 fan-out 步骤与 Send、内存与 SQLite 日志 |

`support.py` 包含共享的 Python 节点、条件、注册表、预算和 JavaScript 构建器。

## 运行

在仓库根目录下，使用启用了 Python 绑定、Program 和 QuickJS 控制的构建运行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_PYBIND=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build
PYTHONPATH=build:bindings/python python -m pytest -q bindings/python/tests/program_e2e
```

离线场景约需 20 秒；`-m "not slow"` 会跳过有意等待一秒或更久的场景。CI 在每次 push 和 pull request 时，在原生 Linux 通道（`scripts/verify_ci.py native-linux`）中作为 `bindings/python/tests/` 的一部分运行它们。

## 在线模型测试

`test_live_openrouter_model_call_inside_a_program` 与 `test_live_openrouter_bound_llm_call_is_priced` 各发送一次付费的 OpenRouter 请求（`max_output_tokens=64`，零数据保留路由）。第二个测试在 1,000,000 microunit（1 USD）的金额预算下运行通过 `LocalProgramHost(node_context=...)` 绑定的内置 `llm_call`，并检查结算后的记录。除非两个变量都已设置，否则会被跳过，CI 从不设置它们：

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

密钥只从环境变量读取。`OPENROUTER_MODEL`、`OPENROUTER_PRICED_MODEL`（目录条目带 route ceiling 的模型，默认 `deepseek/deepseek-v4-flash-0731`）和 `OPENROUTER_BASE_URL` 可覆盖模型和端点。

## 已知问题

预期行为已确定但尚未实现的场景，会标记为 `support.known_issue("#NNN", reason)`，即严格（strict）的 `xfail`。修复合入后测试会通过，严格模式会将其报告为失败，因此必须在同一变更中移除该标记。查看当前的失败：

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
