<!-- neograph-i18n: source=benchmarks/history/engine-overhead-2026-04.md locale=zh-CN source_sha256=7379e680c0a142b37f70fe841077af0cdb397cba81ed31910d50a4f185fa7fa2 -->
# 引擎开销基准测试，2026-04（x86_64）与 2026-07（Arm64）：已被取代

**Languages:** [English](engine-overhead-2026-04.md) | [한국어](engine-overhead-2026-04.ko.md) | [日本語](engine-overhead-2026-04.ja.md) | [简体中文](engine-overhead-2026-04.zh-CN.md)

这些表格于 2026-04（x86_64）和 2026-07（Arm64）在不同会话中测得，为留存记录而原样保留。在单一会话中测得的当前 cohort 见[基准测试指南](../README.md)。请勿将下列数值与之比较。下列各项现已确认无效或不可靠。

1. 第一张表混用了不同会话：NeoGraph 的行来自 2026-04-22 和 2026-04-29，Python 的行来自 2026-04-29，比值是跨会话的中位数相除。表上方的文字、框架列表、环境块和图表各自列出了不同的版本。
2. LangGraph 使用同步的 `invoke` API，它在每次调用时新建的线程池上执行扇出。因此 `par` 的 157 倍比较的是线程池与 NeoGraph 的内联派发（`worker_count=1`）；旁边列出的池对池 8.1 倍才是匹配的比较。Executor 一列把这条路径称为“单线程 asyncio”，这并不正确。
3. pydantic-graph 使用了当前发布版已不再提供的旧版 `Graph(nodes=[...])` API，其 `par` 一行是六个节点的串行链。“pydantic-graph 无法扇出”的说法是错误的：`GraphBuilder` API 可以向并发运行的步骤广播。
4. LlamaIndex 的各行在每次计时迭代中都构造并校验一个新的工作流实例，没有记录 llama-index-workflows 的版本，并使用了让 summarizer 步骤运行五次的汇合写法。工作流实例可以复用。
5. 内存各列把原生进程与包含解释器和已导入框架的 Python 进程相比较。为复现耗时一列而给出的命令会运行 5 行、每行 5 个样本，工作量远大于产生该数值的二进制文件。
6. 没有任何脚本检查运行结果是否正确，也没有记录重复运行之间的离散程度。构建被描述为 `-O3 -DNDEBUG`，而默认构建还带有加固标志。
7. pydantic-graph 的引擎数值（约每次运行 227 µs）与突发测试工具的 70 至 100 µs 不可能同时描述同一次调用。它们来自未记录环境中的不同会话；当前指南把这两种形态连续测量，并显示安装 `logfire` 包会如何改变一次运行的开销。

| 框架 | 版本 | 抽象模型 |
|-----------|---------|-------------|
| NeoGraph | 3.0 | 状态通道图，C++20 协程 + asio |
| LangGraph | 1.1.9 | 状态通道图（Python） |
| Haystack | 2.27 | 带类型 socket 的组件流水线 |
| pydantic-graph | 1.84 | 单后继节点的状态机 |
| LlamaIndex Workflow | 0.14 | 事件驱动的异步工作流 |
| AutoGen GraphFlow | 0.7.5 | 消息传递的多智能体图 |

## 工作负载

六个实现移植的是同样两种工作负载，在适用的地方只编译一次，然后在热循环中反复调用。
状态和拓扑上的适配列在下面。

| Id | 形态 | 状态 |
|----|-------|-------|
| `seq` | 3 个节点的链 `a → b → c` | 单个 `counter` 通道，每个节点写入 `counter+1`（overwrite reducer） |
| `par` | 5 个 worker 的 fan-out，然后在 `summarizer` 处 join | `results: list`（append reducer）加 `count: int`；每个 worker 追加自己的序号，summarizer 写入 `len(results)` |

所有框架都关闭了检查点。

有三个移植版本需要针对框架调整工作负载：

* **Haystack** 没有 append reducer：每个 worker 通过自己的带类型 socket 输出，
  summarizer 对各个列表的长度求和。每次运行分发的组件数量相同。
* **pydantic-graph** 是单后继节点的状态机，无法 fan-out。
  `par` 工作负载被模拟成 6 个节点的串行链
  （`w1 → w2 → w3 → w4 → w5 → summ`）。结果中已标注这一点：
  它不是对等的并行 fan-out 测量。
* **AutoGen** 是消息传递模型，不是状态通道模型。计数器被编码成文本消息的内容。
  summarizer 统计收到的 worker 消息数。图的形态相同，状态模型不同。

## 结果

下面的**基准运行**于 2026-04-22 测得，针对 NeoGraph v3.0.0，
环境为 x86_64 Linux、g++ 13 Release `-O3 -DNDEBUG`、CPython 3.12.3。
NeoGraph 取 `bench_neograph` 10 次运行的中位数；
Python 一侧每个框架取 3 次运行的中位数。版本：neograph v3.0.0、
langgraph 1.1.9、haystack-ai 2.28.0、pydantic-graph 1.85.1、
llama-index-core 0.14.21、autogen-agentchat 0.7.5。

基准运行之后，还附有在当时 master 上的一次重新测量（2026-04-29）。
`par` 一行把两种受支持的执行方式分开列出：当前默认的 `worker_count=1`，
以及由 `set_worker_count_auto()` 启用的、引擎自己的线程池。
为什么不含 I/O 的微基准应当把这两组数字分开，见表后的*说明*。

### 每次迭代的开销（µs，越低越好）

| 框架 | `seq`（3 节点链） | `par`（fan-out 5 + join） | `seq` 相对 NeoGraph | `par` 相对 NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
| **NeoGraph v3.0.0** *（基准，2026-04-22）* | **5.0** | **11.8** | 1× | 1× |
| **NeoGraph master** *（2026-04-29，默认 `worker_count=1`）* | **5.25** | **14.4** | 1× | 1× |
| **NeoGraph master** *（2026-04-29，`set_worker_count_auto()`）* | **5.25** | **278** | 1× | 1× |
| Haystack 2.28.0 | 139.85 | 278.48 | 28.0× / 26.6× / 26.6× | 23.6× / 19.3× / 1.0× |
| pydantic-graph 1.87.0 | 227.14 | 280.26¹ | 45.4× / 43.3× / 43.3× | 23.7×¹ / 19.5×¹ / 1.0×¹ |
| LangGraph 1.1.10 | 642.62 | 2,261.55 | 128.5× / 122.4× / 122.4× | 191.7× / 157.1× / 8.1× |
| LlamaIndex Workflow 0.14.21 | 1,564.54 | 4,373.76 | 312.9× / 298.0× / 298.0× | 370.7× / 303.7× / 15.7× |
| AutoGen GraphFlow 0.7.5 | 3,126.86 | 7,281.08 | 625.4× / 595.6× / 595.6× | 617.0× / 505.6× / 26.2× |

最右两列各给出三个比值：相对 v3.0.0 基准 / 相对 master 的 worker=1 默认模式 /
相对 master 的 auto-worker 模式。

带日期的 `seq` 测量值很接近：NeoGraph 从 5.0 µs 变为 5.25 µs。
`par` 的测量值随执行模式而不同（11.8、14.4 和 278 µs）；
它们没有把基准里的每一行都复现在 ±10% 以内，测的也不是当前的 HEAD。

¹ pydantic-graph 的 `par` 是对 6 个节点的串行模拟，它不支持 fan-out。
这不是并行的工作负载，列出来只是为了完整。

### 关于 `par` 一行的说明（`seq` 没有变化）

`par` 从 14.4 µs 到 278 µs 的差距，是为五个什么实际工作都不做的节点
启用引擎自有线程池所测得的代价：

* **当前 `build()` / `compile()` 的默认值：`worker_count=1`。** 不安装引擎自己的线程池，
  所以 fan-out 在调用方的 executor 上分发。这就是 14.4 µs 那一行；
  对于 CPU 开销很小的节点，或者节点持有非线程安全状态的图，它是正确的回归信号。
* **并行 fan-out 需要显式开启。** `set_worker_count_auto()` 把线程池大小设为
  `hardware_concurrency`；`set_worker_count(N)` 则选定固定的上限。
  这就是 278 µs 那一行。因为每个 worker 只追加一个整数，协调开销在这里清晰可见；
  但与 100 ms 的 LLM 调用相比，它可以忽略不计，而且让互相独立的节点可以重叠执行。

不修改基准测试的源码，就可以运行这两种模式：

```bash
./build/bench_neograph 10000 5000 1
./build/bench_neograph 10000 5000 auto
```

第三个参数作用于 `par` 的引擎，可取 `1`（默认）、`auto`，或任意正数的 worker 数量。
输出中包含一行 `config\tpar_workers\t...`，所以保存下来的结果会带着它的执行模式。

在带日期的 worker=1 的 `par` 对比中，LangGraph 的 2,261.55 µs 是
NeoGraph 14.4 µs 的 157.1 倍。启用引擎自有线程池后，这个比值是 8.1 倍。
Haystack 和 pydantic-graph 与 NeoGraph 278 µs 的 auto-worker 一行大致持平。
这些比值描述的只是这个工作负载和这套配置。

### 端到端的进程指标

整个二进制文件或脚本的运行时间，包含预热和两种工作负载。
`seq` 迭代 10,000 次，`par` 迭代 5,000 次。用
`/usr/bin/time -f "%e s, %M KB"` 测量。

| 框架 | 总耗时 | 峰值 RSS | Executor |
|-----------|--------------:|---------:|---------:|
| **NeoGraph 3.0** | **0.11 s** | **4.5 MB** | 默认是单线程 io_context |
| pydantic-graph | 3.98 s | 35.1 MB | 单线程 asyncio（GIL） |
| Haystack | 3.85 s | 80.3 MB | 单线程 asyncio（GIL） |
| LangGraph | 18.95 s | 60.1 MB | 单线程 asyncio（GIL） |
| LlamaIndex Workflow | 39.49 s | 101.4 MB | 单线程 asyncio（GIL） |
| AutoGen GraphFlow | 63.29 s | 52.3 MB | 单线程 asyncio（GIL） |

NeoGraph 3.0 默认的超步循环，通过 `run_sync` 在单线程的 io_context 上运行协程；
CPU 并行的 fan-out 需要通过 `engine->set_worker_count(N)` 主动开启。
对于 I/O 密集的节点，单线程仍然可以借助 co_await 挂起来实现重叠。

## Linux ARM64 基线：Neoverse-N1

这是 [#165](https://github.com/fox1245/NeoGraph/issues/165) 中另行给出的原生 ARM64
平台基线，不是与上面 x86_64 表格的回归对比。它固定在修订版 `d7a6477`
和自己的依赖集上，以便之后的测量可以追溯。

| 项目 | 值 |
|---|---|
| 日期 | 2026-07-22 |
| 操作系统 | Ubuntu 24.04, Linux `6.17.0-1018-oracle` |
| 架构 | `aarch64` |
| CPU | 4 vCPU, ARM Neoverse-N1 |
| 内存 | 23 GiB，无交换分区 |
| 编译器 / CMake | GCC 13.3.0 / CMake 3.28.3 |
| Python | CPython 3.12.3 |
| NeoGraph 修订版 | `d7a6477` |

工作负载和迭代次数与主基准一致：每张图只编译一次，然后 `seq` 迭代 10,000 次，
`par` 迭代 5,000 次。NeoGraph 测量 10 次，每个 Python 实现测量 3 次；
表中给出的是中位数。检查点、网络 I/O、模型调用和 sleep 都已关闭。
整个进程的峰值 RSS 取自 `/usr/bin/time -f "%e s, %M KB"`。

| 框架 | `seq`（µs/次） | `par`（µs/次） | `seq` 相对 NeoGraph | `par` 相对 NeoGraph | 峰值 RSS |
|---|---:|---:|---:|---:|---:|
| **NeoGraph** | **9.50** | **21.80** | 1× | 1× | **4.35 MB** |
| Haystack 3.0.0 | 153.44 | 329.67 | 16.2× | 15.1× | 73.6 MB |
| pydantic-graph 1.87.0 | 342.60 | 405.87¹ | 36.1× | 18.6×¹ | 32.1 MB |
| LangGraph 1.2.9 | 1,037.55 | 3,289.22 | 109.2× | 150.9× | 63.7 MB |
| LlamaIndex 0.14.23 | 2,765.04 | 7,824.85 | 291.1× | 358.9× | 96.9 MB |
| AutoGen 0.7.5 | 4,166.39 | 9,571.11 | 438.6× | 439.0× | 47.8 MB |

NeoGraph 使用的是 worker=1 的默认值，所以 `par` 一行测的是拓扑、reducer 和串行分发的开销，
而不是引擎自有线程池的执行。需要单独测量并行 fan-out 时，请显式使用 `auto` 基准模式；
不要把两种模式合并成一个标题数字。

¹ pydantic-graph 无法表达这种 fan-out 拓扑；它的 `par` 一行，就是上面所说的同样的
六节点串行模拟。固定使用 1.87.0 版本，是因为当时最新的 2.15.0 的 API 已不再支持基准测试
所用的 `Graph(...)` 构造函数。各进程没有绑定 CPU，也没有受 cgroup 约束。

## 这些数字说明什么

1. x86_64 基准里 `seq` 各行的开销，相当于 NeoGraph 每次运行开销的倍数，
   从 Haystack 的 28.0 倍到 AutoGen 的 625.4 倍不等。这些是框架加工作负载的测量结果，
   不是对各自开销成因的单独解释。
2. 基准中整个进程的峰值 RSS，NeoGraph 是 4.5 MB，各 Python 实现是 35.1–101.4 MB。
   这些数值包含各个进程自身的运行时和 import，不是已部署应用的内存占用。
3. worker=1 的分发与引擎自有线程池，是两种不同的执行方式。请针对目标工作负载
   把两者都测一遍；不做实际工作的 fan-out 基准，能暴露协调开销，
   但无法预测模型或 I/O 的延迟。

## 注意事项：这个基准没有测量的内容

* **真实的智能体工作负载。** 框架对比中没有模型推理、网络请求或工具 I/O。
  这些开销可能在应用中占主导，所以这张表并不能说明端到端的智能体加速。
* **适合各框架的工作负载。** AutoGen、LlamaIndex 和
  pydantic-graph 各自针对不同的范式做了优化（多智能体对话、
  事件驱动的长时间工作流、状态机控制流），
  而这个基准并没有覆盖这些范式。本基准是在 NeoGraph 擅长的场景上测量它们的。
* **检查点吞吐量。** 给每个框架都启用持久化，
  序列化成本就会占主导；那是另一个基准。
* **冷启动。** 每个实现在测量之前都有 10 次迭代的预热循环。
  整个进程的数字包含 Python 解释器启动（约 200 ms）和框架的 import 时间，
  后者差异很大（LlamaIndex 和 AutoGen 会 import 很大的依赖树）。
* **公平性。** NeoGraph 用 CMake `-DCMAKE_BUILD_TYPE=Release` 构建，
  在 GCC 上对应 `-O3 -DNDEBUG`。每个 Python 框架都是原版 CPython 3.12，
  使用各个 cohort 记录的依赖版本，没有做任何自定义调优。独立的基准命令用的是 `-O2`；
  CMake 构建里的 `Release` 对应 `-O3`。

## 2026-04-19 的数据所用的环境

```
OS:        Linux 6.6.87.2-microsoft-standard-WSL2 (Ubuntu 24.04 userland)
CPU:       host CPU (8 logical cores exposed to WSL)
Compiler:  g++ 13.x, -std=c++20 -O2 -DNDEBUG
Python:    3.12.3 (system)
Versions:  langgraph 1.1.7, haystack-ai 2.27.0, pydantic-graph 1.84.1,
           llama-index-core 0.14.20, autogen-agentchat 0.7.5
```

硬件、运行时版本、工作负载的形态和 worker 模式，都会同时改变延迟和比值；
本文并没有确立任何跨平台的容差。
