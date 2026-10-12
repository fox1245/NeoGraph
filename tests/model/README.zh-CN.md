<!-- neograph-i18n: source=tests/model/README.md locale=zh-CN source_sha256=ff8e590d7785c7ce1b9c737b233df1d9367d921ad10e276d55d2565169d2c88d -->
# Join 模型（#363）

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`BarrierJoin.tla` 为 BSP 超步建模：只有一个 AND join，它的 `wait_for`
槽位里可以有多个 `any_of` 备选，这是 #363 获批的范围（join 的各种模式推迟处理）。
`tests/test_join_exhaustive.cpp` 把同样的规则写成一个参考解释器，并在更大的图上，
对照它检查真实的 `Scheduler` 和 `GraphEngine`。语义发生变化时，要让两者保持同步。

## 规则

- 只要槽位中的任何一个备选节点向 join 发出信号，这个槽位就算填满。
  所有槽位都填满后 join 触发，之后所有累积的信号清空。
- 非成员发来的信号会被记录，但永远不计数。
- 条件路由是非确定性的标签选择。
- `Command.goto(__end__)` 会抢占它所在超步的所有路由。
- 没有任何就绪节点时，运行结束。如果此时 join 仍持有信号（不论来自成员与否），
  运行就是停滞的，状态为 `Failed`；除非最后一个超步（即之后没有任何就绪节点的那一步）
  通过显式路由到达了 `__end__`：指向 `__end__` 的静态边（`.exit()` 写出的就是这种）、
  指向 `__end__` 的条件路由，或者 `Command.goto(__end__)`。隐式的结束（节点没有后继）
  永远不能消除停滞，在更早的超步里显式结束也不行。

因此，剩下的 `any_of` 备选节点如果在 join 退出的那个超步里到达，运行正常完成；
如果晚一个超步到达、且没有别的节点就绪，运行就失败。

C++ 测试框架还覆盖了失败路由（#364）：沿失败边失败的节点，只路由到那条边的目标，而 `Command.goto` 仍然抢占它。失败边的目标永远不是 join 或 `__end__`，所以上面的规则保持不变；这个 TLA+ 模型不包含失败边。

## 不变式

| 名称 | 含义 |
|---|---|
| `NoDoubleFirePerRound`、`JoinRunsOnlyWhenFired`、`FireClearsSignals` | 每次触发都会消耗自上次触发以来、填满每个槽位的那些到达信号；join 只在刚触发之后才运行，触发会清空所有信号。 |
| `StallImpliesFailed`、`FailedOnlyOnStall` | 结束时 join 处仍有信号在等待的运行会失败，并且只有这样的运行才会失败。 |
| `OnlyExplicitEndSuppressesStall` | 带着信号等待而完成的运行，在它最后一个超步里显式到达了 `__end__`。 |
| `LeftoverAlternativeStalls` | 备选节点在 join 触发之后才到达，而且之后既没有再次触发，最后一个超步里也没有显式结束，则运行失败（失败时关闭）。 |
| `NamedOutcomes` | #357 的两个并不互斥的变体：剩下的备选节点与退出同时到达，则完成；晚一个超步到达，则失败。 |
| `LegacyEquivalence` | 槽位都是单元素时，槽位规则就是今天的屏障：`wait_for` 是已收到信号的子集。 |
| `Termination`（性质） | 每次运行要么完成，要么失败，要么到达步数上限。 |

## 运行 TLC

TLC 不属于 CI。它需要 Java 11 或更高版本，以及 TLA+ 发布页上的
`tla2tools.jar`（<https://github.com/tlaplus/tlaplus/releases>）：

```sh
cd tests/model
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC \
    -workers auto -config BarrierJoin.cfg BarrierJoin.tla
```

`BarrierJoin.cfg` 检查所有节点数不超过三个的图，对 join 成员做每一种 `any_of` 划分，
另外还有规格里点名的 #357 的几张图（旧式屏障、`any_of` 槽位、两个并不互斥的变体、
通过 `Command.goto` 显式结束）。TLC 找到 335,521 个不同的状态，没有发现错误；
在 8 个 worker 上用掉的两分钟里，大部分时间花在计算 21,707 个初始状态上。
`MaxNodes = 4` 超出了 TLC 的能力范围；C++ 测试框架最多覆盖五个节点。

要亲眼看到这些不变式起作用，可以让隐式结束消除停滞（`StallImpliesFailed` 会失败），
或者让 join 在收到任何一个成员的信号时就触发（`NoDoubleFirePerRound` 会失败）。

## 恢复模型

`tests/resume_model.h` 和 `tests/test_resume_model.cpp` 对照一个参考解释器，检查 `GraphEngine` 的中断、恢复和重放契约，做法与 join 的测试框架检查 join 一样；`tests/test_program_resume_model.cpp` 对 Program 运行时做同样的检查。参考解释器所表述的，是文档中规定的契约（`docs/concepts.md`）：一个超步先运行它的就绪节点，再运行它们的 Send；某一步的调用如果发出了提问，就在所有这些调用上暂停，并保留已完成的部分；一次恢复回应它指明的中断，未指明则回应第一个，而且只有那一次调用会收到这个值（#388、#393）；`interrupt_before` 的批准送达被它拦住的节点，一步中的每个闸门单独回应，并且只有被回应的闸门才会放行（#382、#396）；失败的优先级高于同一步里的普通中断（#392）。

### Core 测试框架

- **图族。** 主节点 `n0`..`n{N-1}` 带有向前的边（每个节点最多两个后继），每个节点都可达。节点是以下几种之一：`Ok`；`Ask`（抛出 `NodeInterrupt`，直到得到回应；有两个后继时，可以根据回答来路由）；`Fail`（带或不带失败边，#364）；`Flaky`（失败，之后重试在它自己的尝试命名空间里成功）；或者 `Map`（向一个 worker 发出一个或两个 Send，其条目是 `Ok`、`Ask` 或 `Fail`）。可选地，有两个静态前驱的最后一个节点是屏障 join，并且有一个 `Ok` 或 `Ask` 节点带有 `interrupt_before` 或 `interrupt_after`（在扩展的上限下，是两个 `interrupt_before` 闸门）。
- **调度。** 参考解释器的每一种回答顺序：每个待处理的问题，既按其中断 id 回答，也按默认方式回答，对根据回答来路由的 `Ask`，两个标签都要走到。每个调度还要在每个持久化操作处发生崩溃的情况下运行（每次检查点 `save` 和 `put_writes`；在每一次之前以及最后一次之后，扩展上限下则是每一次的前后，并且在恢复期间再崩溃一次），还要对每一个暂停点做一次 fork。崩溃会让进程停止：存储拒绝之后的所有操作，宿主像真实的宿主那样，从最近的检查点恢复。如果这个检查点正是它所回应的那个，就重复它正在执行的那次恢复；它没见过的 `interrupt_before` 或 `interrupt_after` 暂停，就把暂停呈现出来；其他情形不带值地恢复；没有检查点的线程则重新运行。每个调度都在 `InMemoryCheckpointStore` 上运行；在 `SqliteCheckpointStore` 上则是无崩溃的情形（节点数不超过两个的图也带崩溃，扩展上限下是三个），并使用粗粒度时钟：所有检查点都以同一个时间戳保存（`tests/fixtures/coarse_clock_store.h`）。
- **提问节点。** 每个调度都运行两遍：一遍中的提问节点，只接受寄给自己调用 id 的回答；另一遍中的提问节点，接受任何恢复值，就像批准节点那样。
- **性质。** P1 恰好一次：一个节点函数体的完成一旦持久化（它的待定写入，或它所在步骤的提交），就不会再次完成；函数体只会在崩溃丢失了它的完成记录之后才再次运行。P2 每个问题都被问到：暂停点及其带 payload 的待处理问题，与参考解释器一致，并且每个 `Ask` 恰好消耗了交给它的那个回答。P3 结果相同：状态、日志、失败记录和路由，都与参考解释器一致。P4 崩溃透明性和 P5 fork 等价性：P1 到 P3 在每个崩溃调度和 fork 调度上都成立。

### Program 测试框架

测试对象有：JavaScript 的 `main()` 程序（`callCore`；两个成员的 `ng.all`、`ng.race` 和 `ng.quorum`，成员一起运行或逐个运行；`ng.await`；以及 Core 步骤里的两个节点都可能提问的 `callCore`，#388 和 #394），以及带类型的计划（`parallel`、三选二的 `quorum`、`sequence`），它们作用在只有一个节点的 Core 上，这个节点可能调用绑定的 provider，也可能提问。宿主的接线方式与 `LocalProgramHost` 相同：要么在内存中配合进程内的计价 broker，要么把所有存储放进同一个 SQLite 文件，检查点用同样的粗粒度时钟保存，再加上 SQLite 的 provider journal。provider 是一个真正的 SDK 客户端，运行在本地传输层之上，这个传输层会统计它发出的请求数。P1 还涵盖“只发送一次、只计费一次”：一次运行所发出的请求、所计费的 token 和金额，与参考解释器一致（使用进程内 broker 时，重新运行的节点会再次调用、再次付费，这与文档所述一致）。对于一次只运行一个调用的程序，崩溃测试在 SQLite 宿主上进行：每次检查点 save、`put_writes`、Program 转换、provider 分发和已写入 journal 的调用时，都会复制一份数据库，然后在这份副本上用一个新的运行时重新连接这次运行。

保存过 Core 检查点的调用，崩溃后会从该检查点继续（#402）；fork 会重放被暂停的调用已经发出的 provider 调用（#405）。做不到这两点的调度判为失败。另有两种文档所述的行为不在 P4 的覆盖范围内，遵循它们的调度，测试框架只做报告而不判失败。一种是：无论在 JavaScript 程序还是计划里，在第一个 Core 检查点之前停止的调用，都会交回宿主去协调（#402、#404），测试框架只在崩溃没有给该调用留下检查点时才接受这种情况。另一种是：请求发出期间的崩溃会冻结该调用，它永远不会再次发送（#364）。宿主能以“已完成”来协调的调用（既不提问也不失败的调用、被 await 的调用，或调用对，且其后没有计费的调用），测试框架用参照结果去协调，之后运行必须和参照一样结束（#403）。

### 上限与变量

| | CI 上限 | `NEOGRAPH_RESUME_MODEL_BOUND=extended` |
|---|---|---|
| 主节点，特殊节点 | 3，2 | 4（三个节点以内为 3 个特殊节点，四个节点时为 2 个） |
| 每个调度的回答数，崩溃数 | 4，1 | 6，2 |
| Send 条目 | `ok`、`ask`、`ask ok`、`ask ask`、`ask fail` | 一个或两个条目的每一种组合 |
| 闸门 | 一个，放在 `Ok` 或 `Ask` 上 | 任意节点上一个，或者两个 `interrupt_before` |

在 CI 的上限下，Core 测试框架对每一种提问方式，运行 2,196 张图和约 6,100 个回答调度，各约 83,000 次运行；Program 测试框架运行 124 个 JavaScript 程序和 18 个计划，约 950 个调度。每个测试在 4 个 worker 上用时不到一分钟。

变量：`NEOGRAPH_RESUME_MODEL_NODES`、`_SPECIAL`、`_SPECIAL_LARGEST`、`_ANSWERS` 和 `_CRASHES` 用来覆盖单项上限；`NEOGRAPH_RESUME_MODEL_SQLITE=0` 会去掉 SQLite；`NEOGRAPH_RESUME_MODEL_THREADS` 设置 worker 数量（默认 4）。对于 Program 测试框架，`NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended` 会加入两条命令的程序、每一种 fork，以及每个操作之后的崩溃；`_ONLY` 只保留描述中包含该字符串的程序，`_CRASHES=0` 去掉崩溃，`_THREADS` 设置 worker 数量（默认 4），`_DEBUG` 打印每个结果和每个持久化操作。
