<!-- neograph-i18n: source=bindings/python/tests/program_e2e/README.md locale=zh-CN source_sha256=5244fb2b21fedea0ecc885bd7f9d932335e0ebbf3a5965744fd2e020d1d4e4c5 -->
# Program API 端到端场景

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这些测试通过 Python 绑定来驱动封存的 v1 JavaScript Program API，做法就是用户读完 [Python 指南](../../../../docs/python-binding.md)之后会采用的做法：Python 的 `GraphNode` 类、注册的条件、`ProgramRegistryBuilder` 快照、`ProgramCompiler` 和 `LocalProgramHost`。它们需要一个带有 Program 和 QuickJS 控制功能的 `neograph_engine` 构建；缺少这些时，整个包会被跳过。

## 覆盖范围

| 文件 | 场景 |
|---|---|
| `test_01_quickstart.py` | 文档中的示例，一张带有 Python 节点和一个条件的 `define()` 图，跨进程的标识，激活与回滚 |
| `test_02_hitl.py` | `interruptBefore`、`NodeInterrupt`，默认的和显式指定的 pending id，`main()` 内部的中断，进程重启后的恢复 |
| `test_03_failures.py` | 节点抛出异常，fan-out 分支失败，`main()` 抛出异常，`callCore` 失败，没有 provider 的 `llm_call` |
| `test_04_budgets.py` | 编译期和运行期的 JavaScript 超时，步数和操作数预算，挂钟时间的取消，已准入的预算，分叉从源运行的预算中分得的一份（#414） |
| `test_05_diagnostics.py` | 语法错误和构建器错误的源码位置，注册表缺失，lint 的 span，控制字符，准入错误 |
| `test_06_graph_shapes.py` | 屏障 join，条件路由，`Send` 的 map-reduce，子图，无效的图形态 |
| `test_07_main_programs.py` | `callCore`、`all`、`race`、`quorum`，`await` 超时，`cancelScope`，外部取消，`spawn` |
| `test_08_live_llm.py` | 节点会调用模型的 Program：默认用本地对端，启用时用 OpenRouter；Python 节点和绑定的 `llm_call` |
| `test_09_bound_llm.py` | 通过 `node_context` 绑定的 `llm_call`（#372）：上报的成本和估算的成本，固定为 `provider.max_price` 的路由上限，发送之前的拒绝（预算、未知模型、路由别名），零预算，没有 provider 的 id，针对同一份预算的 `all` 和 `quorum`，调用写入 journal 之后发生崩溃、以及只计费一次的恢复 |
| `test_10_failure_routing.py` | 失败路由（#364）：失败的 TEST 被路由到带 `interruptBefore` 的处理器并附带它的记录，沿 retry 和 abort 路由恢复，作者自定义的错误码，脱敏后的错误文本，`max_visits`，`max_node_retries` 上限，以及有无 `main()` 时的剩余值（#419），节点内部的 Program 诊断，以及用 SQLite 在新进程里恢复 |
| `test_11_attempt_timeout.py` | 尝试超时（#364）：一个很慢的 Python 节点超时后在重试时恢复正常，一个总是超时的节点以 `NG.Timeout` 路由到它的处理器（没有处理器时让运行失败），迟到的完成永远不会进入结果或 trace，一个在停止期限之后仍然无视取消的节点（`NG.EffectUncertain`），以及会越过 `wall_time_ms` 的退避以 `deadline` 类别停止 |
| `test_12_send_interrupts.py` | 抛出 `NodeInterrupt` 的 `Send` 目标（#385）：Program 在它上面暂停，恢复时重放 map 节点和已完成的 Send，用 SQLite 在重启后恢复，以及调用过绑定 `llm_call` 的已完成 Send 既不会重发，也不会再次计费 |
| `test_13_fork_replays_the_paused_step.py` | 对一个在已完成的绑定 `llm_call` 旁边暂停的运行执行 `host.fork()`（#390）：fork 回答问题，并根据该步的待定写入重放已完成的调用，所以它的请求不会再发送，它的 token 和费用也不会再次计费；覆盖 fan-out 步骤和 Send，分别在内存中和使用 SQLite journal 的情形 |
| `test_14_await_deadline.py` | `ng.await(..., timeout_ms)` 保持它的绝对截止时间（#391）：在截止时间之内恢复会正常完成，在之后恢复则立即以 `P_AWAIT_TIMEOUT` 超时（内存中、SQLite 上，以及从重启后的宿主恢复），在恢复后的 await 内部发生崩溃之后，`reconnect()` 会执行该 await 最初开始时的截止时间 |
| `test_15_send_isolation.py` | `Send` 的 payload 只对它自己的 `Send` 可见（#389）：一个 Send 和三个 Send，分别用一个和四个 worker，在运行时以及在 SQLite 上恢复之后，都不会给下一步或最终状态留下任何 payload |
| `test_16_interrupt_identity.py` | 同一步里的两个 Core 中断，旁边有一个已完成的绑定 `llm_call`（#388）：pending input 指明第一个 Core 中断，一次恢复回应它，第二个重新提出它自己的问题，下一次恢复回应那一个，已完成的调用永远不会重发或再次计费；分别在内存中和使用 SQLite journal |
| `test_17_resume_value_scope.py` | 回应只送达它所回应的问题（#393）：绑定的 `llm_call` 之后，`first` 提问，接着 `second` 在下一个 Core 步骤中提问；回应 `first` 的那次恢复，让 `second` 提出它自己的问题，下一次恢复回应它，已完成的调用永远不会重发或再次计费；分别在内存中和使用 SQLite journal |
| `test_18_step_failure_outranks_interrupt.py` | 不论提问节点的序号如何，失败都会结束它所在的步骤（#392）：一个 Core 步骤里，一个绑定的 `llm_call` 旁边有一个提问的节点和一个失败的节点，两种顺序都测；Program 以失败节点的分类后的失败而失败，没有问题残留在待处理状态，模型调用只发送并计费一次；分别在内存中和使用 SQLite journal |
| `test_19_fork_interrupt_identity.py` | 对一个 Core 步骤在两个中断上暂停、且旁边有一个已完成的绑定 `llm_call` 的运行执行 `host.fork()`（#394）：fork 回答待处理调用的 `core_interrupt_id` 所指明的那个中断，另一个节点在 fork 中重新提问，已完成的调用永远不会重发或再次计费；分别在内存中和使用 SQLite journal |
| `test_20_javascript_fork.py` | 对一个 JavaScript `main()` 执行 `host.fork()`，它在第二个 `callCore` 中暂停，其中各有一个绑定的 `llm_call`（#395）：fork 从命令 journal 重放第一条命令，带着回应恢复暂停的调用并完成，两次模型调用都不会再发送或再次计费；分别在内存中和使用 SQLite journal |
| `test_21_fork_recorded_failure.py` | 对一个 JavaScript `main()` 执行 `host.fork()`，它的收集式 `ng.all` 记录了一个绑定 `llm_call` 被拒绝的成员（#398）：fork 把已记录的 provider 证据重新绑定到自己，回应暂停的成员，并连同证据一起报告已记录的失败，两次调用都不会再发送；分别在内存中和使用 SQLite journal |

`support.py` 放着共用的 Python 节点、条件、注册表、预算和 JavaScript 构建器。

## 运行

在仓库根目录下，使用启用了 Python 绑定、Program 和 QuickJS 控制功能的构建来运行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_PYBIND=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build
PYTHONPATH=build:bindings/python python -m pytest -q bindings/python/tests/program_e2e
```

离线的场景大约需要 20 秒；`-m "not slow"` 会跳过那些有意等待一秒或更久的场景。CI 在每次 push 和 pull request 时都会运行它们，它们是原生 Linux 流水线（`scripts/verify_ci.py native-linux`）中 `bindings/python/tests/` 的一部分。

## 实机模型测试

`test_live_openrouter_model_call_inside_a_program` 和 `test_live_openrouter_bound_llm_call_is_priced` 各发出一次付费的 OpenRouter 请求（`max_output_tokens=64`，零数据留存路由）。第二个测试运行通过 `LocalProgramHost(node_context=...)` 绑定的内置 `llm_call`，预算为 1,000,000 microunit（1 美元），并检查结算后的记录。它的请求会把目录中的路由上限作为 `provider.max_price` 带上，所以 OpenRouter 会让请求失败，而不是把它路由到标价高于该上限的 endpoint。除非两个变量都已设置，否则这两个测试会被跳过，CI 从不设置它们：

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

密钥只从环境变量读取。`OPENROUTER_MODEL`、`OPENROUTER_PRICED_MODEL`（目录条目带有路由上限的模型，默认是 `deepseek/deepseek-v4-flash-0731`）和 `OPENROUTER_BASE_URL` 可以覆盖模型和 endpoint。

## 已知问题

预期行为已经确定、但尚未实现的场景，用 `support.known_issue("#NNN", reason)` 标记，它是一个严格模式的 `xfail`。修复落地后，这个测试会通过，严格模式会把这种情况报告为失败，因此标记必须在同一个变更里移除。查看当前的失败项：

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
