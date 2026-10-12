<!-- neograph-i18n: source=tests/model/README.md locale=zh-CN source_sha256=da1d60a2c19590a22ffc97a53d9a3d8ed0817cae7f42d76bc9520ddd42b6831a -->
# Join 模型 (#363)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`BarrierJoin.tla` 对 BSP super-step（超步）建模，其中有一个 AND join，其 `wait_for` slot（槽）可以包含多个
`any_of` 备选项（alternative）。这是 #363 批准的范围（join 模式被推迟）。`tests/test_join_exhaustive.cpp`
以参考解释器的形式表述相同的规则，并在更大的图上以它为基准检查真实的 `Scheduler` 和 `GraphEngine`。
语义变化时，请保持两者同步。

## 规则

- 当某个 slot 的任一备选项向 join 发出信号（signal）时，该 slot 即被填满。当每个 slot 都被填满时 join
  触发（fire），随后清除所有累积的信号。
- 来自非成员的信号会被记录，但从不计入。
- 条件路由是非确定性的标签选择。
- `Command.goto(__end__)` 会抢占其所在 super-step 的所有路由。
- 当没有任何内容就绪时，运行（run）结束。如果 join 仍持有信号（无论是否来自成员），则该运行处于停滞（stall）
  状态并为 `Failed`，除非最后一个 super-step（此后不再有任何内容就绪的那个 super-step）通过显式路由到达了
  `__end__`：指向 `__end__` 的静态边（即 `.exit()` 写入的边）、指向 `__end__` 的条件路由，或
  `Command.goto(__end__)`。隐式结束（没有后继的节点）从不抑制停滞，较早 super-step 中的显式结束也不会。

因此，若剩余的 `any_of` 备选项在 join 退出的那个 super-step 到达，运行完成；若它晚一个 super-step 到达，
且没有其他内容就绪，运行失败。

C++ 测试框架还覆盖失败路由（#364）：沿 failure edge 失败的节点只路由到该边的目标，`Command.goto` 仍然优先于它。失败目标不能是 join 或 `__end__`，因此上述规则保持不变；本 TLA+ 模型不包含 failure edge。

## 不变式

| 名称 | 含义 |
|---|---|
| `NoDoubleFirePerRound`, `JoinRunsOnlyWhenFired`, `FireClearsSignals` | 每次触发都会消耗自上次触发以来到达并填满每个 slot 的信号；join 只在触发之后立即运行，且触发会清除所有信号。 |
| `StallImpliesFailed`, `FailedOnlyOnStall` | 结束时仍有信号在 join 处等待的运行会失败，且只有这样的运行会失败。 |
| `OnlyExplicitEndSuppressesStall` | 在有信号等待的情况下完成的运行，必然在其最后一个 super-step 中显式到达了 `__end__`。 |
| `LeftoverAlternativeStalls` | 在 join 触发之后到达的备选项，若之后没有再次触发，且最后一个 super-step 中没有显式结束，则会使运行失败（fail-closed）。 |
| `NamedOutcomes` | #357 的两个非互斥变体：剩余备选项随退出一同到达时运行完成；晚一个 super-step 到达时运行失败。 |
| `LegacyEquivalence` | 当所有 slot 都是单元素（singleton）时，slot 规则即为现有的 barrier：`wait_for` 是已接收信号的子集。 |
| `Termination`（性质） | 每次运行都会完成、失败或达到步数上限。 |

## 运行 TLC

TLC 不属于 CI。它需要 Java 11 或更高版本，以及 TLA+ 发布版（<https://github.com/tlaplus/tlaplus/releases>）
中的 `tla2tools.jar`：

```sh
cd tests/model
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC \
    -workers auto -config BarrierJoin.cfg BarrierJoin.tla
```

`BarrierJoin.cfg` 检查节点数不超过三个的所有图，覆盖 join 成员的每一种 `any_of` 划分，另外还检查 spec 中
命名的 #357 图（legacy barrier、`any_of` slot、两个非互斥变体、通过 `Command.goto` 的显式结束）。TLC 找到
335,521 个不同状态且没有错误；在八个 worker 上约两分钟的运行时间大部分用于计算 21,707 个初始状态。
`MaxNodes = 4` 超出了 TLC 的能力范围；C++ harness 最多覆盖五个节点。

要观察这些不变式真正起作用，可以让隐式结束抑制停滞（`StallImpliesFailed` 失败），或者让 join 在任一成员
信号到达时就触发（`NoDoubleFirePerRound` 失败）。

## 恢复模型

`tests/resume_model.h` 和 `tests/test_resume_model.cpp` 像 join 测试框架检查 join 那样，将
`GraphEngine` 的中断、恢复和重放契约与参考解释器对照检查；`tests/test_program_resume_model.cpp`
对 Program 运行时做同样的事。参考解释器陈述文档化的契约（`docs/concepts.md`）：super-step 先运行
就绪节点，再运行它们的 Send；调用提问的 step 在所有提问处暂停，并保留已完成的工作；恢复回答它指定的
中断或第一个中断，只有该调用收到值（#388、#393）；`interrupt_before` 的批准送达该闸门拦住的节点，
step 的每个闸门单独回答，只有已回答的闸门放行（#382、#396）；失败优先于同一 step 的普通中断（#392）。

### Core 测试框架

- **图族。** 主节点 `n0`..`n{N-1}` 由向前的边相连（每个节点最多两个后继），每个节点都可达。节点是
  `Ok`、`Ask`（在得到回答前抛出 `NodeInterrupt`；有两个后继时可按回答选择路径）、`Fail`（有或没有
  失败边，#364）、`Flaky`（先失败，然后在自己的 attempt 命名空间中重试成功）或 `Map`（向一个 worker
  发送一个或两个 Send，其项为 `Ok`、`Ask` 或 `Fail`）。有两个静态前驱的最后一个节点可以是 barrier
  join，一个 `Ok` 或 `Ask` 节点可以带 `interrupt_before` 或 `interrupt_after`（扩展范围中为两个
  `interrupt_before` 闸门）。
- **调度。** 参考解释器的每种回答顺序：按中断 id 并按默认方式回答每个待处理的问题，并对按回答选路的
  `Ask` 给出两个标签。每个调度还会在每个持久操作处崩溃（每次检查点 `save` 和 `put_writes`；在每个
  操作之前和最后一个操作之后，扩展范围中在每个操作的前后，并在恢复期间再崩溃一次），以及在每个暂停处
  fork 后运行。崩溃会停止进程：存储拒绝之后的所有操作，宿主像真实宿主一样从最新检查点恢复。若该检查点
  正是它在回答的那个，就重做正在执行的恢复；对没见过的 `interrupt_before` 或 `interrupt_after` 暂停，
  把它作为问题呈现；其余情况不带值恢复；没有检查点的线程则重新运行。每个调度都在
  `InMemoryCheckpointStore` 上运行，并在使用粗粒度时钟（所有检查点以同一时间戳保存）的
  `SqliteCheckpointStore` 上无崩溃地运行（至多两个节点、扩展范围中至多三个节点的图也会崩溃）。
- **Ask。** 每个调度运行两次：一次是只接受发给自己调用 id 的回答的 Ask，一次是像审批节点那样接受任何
  恢复值的 Ask。
- **性质。** P1 恰好一次：节点体的完成一旦持久化（其待写入，或其 step 的提交），就不会再次完成；只有在
  崩溃丢失了该完成之后，节点体才会再次运行。P2 每个问题都被提出：暂停及其待处理问题和 payload 与参考
  一致，每个 `Ask` 恰好消费给它的回答。P3 结果相同：状态、日志、失败记录和路径与参考一致。P4 崩溃透明
  与 P5 fork 等价：P1-P3 在每个崩溃调度和 fork 调度上成立。

### Program 测试框架

JavaScript `main()` 程序（`callCore`，同时或逐个运行两个成员的 `ng.all`、`ng.race`、`ng.quorum`，
`ng.await`，以及其 Core step 运行两个都可能提问的节点的 `callCore`，#388 和 #394）和带类型的 plan（`parallel`、三取二的 `quorum`、`sequence`），运行在只有一个节点的
Core 上，该节点可以调用已绑定的 provider，也可以提问。宿主按 `LocalProgramHost` 的方式装配：使用进程内
计价代理的内存宿主，或把所有存储放在一个 SQLite 文件中并使用 SQLite provider 日志的宿主。provider 是
运行在本地传输上的真实 SDK 客户端，该传输会统计它发送的请求。P1 还覆盖"只发送一次、只计费一次"：
请求数以及 run 被计费的 token 和金额与参考一致（使用进程内代理时，按文档所述，再次运行的节点会再次调用、
再次付费）。崩溃在 SQLite 宿主上针对一次只运行一个调用的程序进行：在每次检查点保存、`put_writes`、
Program 转换、provider 分发和已记入日志的调用处复制数据库，并由副本上的新运行时重新连接该 run。

对于遵循 P4 或 P5 未覆盖的文档化行为的调度，测试框架不将其判为失败，而是计数并报告它们对契约提出的
问题：JavaScript 命令中的崩溃会把 run 交回以便协调；在第一个 Core 检查点之前停止的 plan 不会被恢复
（`P_RUN_RECOVERY_BLOCKED`）；请求发出期间的崩溃会挂起该调用，且该调用不会再次发送；对已暂停 run 的
fork 会再次发送、再次计费被暂停的调用在提问前发出的 provider 调用，而源 run 的恢复会重放该调用。

### 范围与变量

| | CI 范围 | `NEOGRAPH_RESUME_MODEL_BOUND=extended` |
|---|---|---|
| 主节点、特殊节点 | 3、2 | 4（至多三个节点时特殊节点 3 个，四个节点时 2 个） |
| 每个调度的回答、崩溃 | 4、1 | 6、2 |
| Send 项 | `ok`、`ask`、`ask ok`、`ask ask`、`ask fail` | 一个或两个的所有模式 |
| 闸门 | 在 `Ok` 或 `Ask` 上一个 | 任意节点上一个，或两个 `interrupt_before` |

在 CI 范围中，Core 测试框架覆盖 2,196 个图，每种 Ask 约 6,100 个回答调度，各约 83,000 次运行；Program
测试框架运行 124 个 JavaScript 程序和 18 个 plan，约 920 个调度。每个测试用四个工作线程在一分钟内完成。

变量：`NEOGRAPH_RESUME_MODEL_NODES`、`_SPECIAL`、`_SPECIAL_LARGEST`、`_ANSWERS` 和 `_CRASHES` 覆盖单项
上限；`NEOGRAPH_RESUME_MODEL_SQLITE=0` 去掉 SQLite；`NEOGRAPH_RESUME_MODEL_THREADS` 设置工作线程数（默认
4）。对 Program 测试框架，`NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended` 增加两条命令的程序、所有 fork
以及每个操作之后的崩溃；`_ONLY` 只保留描述中包含该字符串的程序，`_CRASHES=0` 去掉崩溃，`_THREADS`
设置工作线程数（默认 4），`_DEBUG` 打印每个结果和持久操作。
