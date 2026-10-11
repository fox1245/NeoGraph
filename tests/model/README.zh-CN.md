<!-- neograph-i18n: source=tests/model/README.md locale=zh-CN source_sha256=39036d07e843a8120cb5ee7f36521353e0c42c468b608c4b0f7909b22286dac0 -->
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
