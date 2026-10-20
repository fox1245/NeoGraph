<!-- neograph-i18n: source=benchmarks/README.md locale=zh-CN source_sha256=ad94d181c2cac62338e50b62f279053c11de72ffdeba6dec2249f4e3ee7fef7d -->
# NeoGraph 与 Python 图/流水线框架对比：引擎开销基准测试

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

在没有 I/O、sleep 或模型调用的、规模相当的小型工作负载上，
对比 NeoGraph 与 Python 编排框架每次调用的引擎开销。
测量范围包括节点分发、状态通道的写入和 reducer 调用。
带日期的表格保留的是历史上的引擎标签和依赖版本；
它们不代表当前的包版本。
NeoGraph `0.16.0` 当前的构建方法，需要 alpha 版 SDK `0.4.0`、接口修订版/共享库
代数 7，以及匹配的头文件和库。

Program 的准入(admission)、JavaScript 控制，以及 SQLite/PostgreSQL 运行时的开销，
见[测量 Program 的成本](../docs/PROGRAM_COST_MEASUREMENT.md)。那是另一套独立的矩阵，
其中包含 journal 和检查点的工作，不能把它当作下面不带检查点的 Core 对比来使用。

参与对比的框架：

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

![引擎开销基准测试：每次迭代的延迟和峰值 RSS](../docs/images/bench-engine-overhead.png)

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

## 复现

当前从源码构建，需要外部的 `SchemaProvider::runtime` 包，只测 Core 的基准也一样。
CMake 3.20 及以上会按顺序选择：显式指定的
`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、已安装的运行时、固定版本的公开 GitHub 源码归档。
`NEOGRAPH_FETCH_SCHEMAPROVIDER` 默认为 ON；
使用已安装的包或显式指定的源码做离线构建时，把它设为 OFF。
复现用的命令构建的是仍在维护的目标；它们不会重建历史上的二进制文件，
也不会自动固定表格中的 Python 版本。
显式指定的检出或已安装的包，必须提供接口/共享库代数 7。
新的输出要另行保存；历史上的表格和 JSON 文件不是用 SDK 0.4.0 测得的结果。

```bash
# Build native Core + v1 Program benchmarks (Release is required for
# representative timings; the default CMake build type is not optimized).
cmake -B build-program-bench -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_ASYNC=ON
cmake --build build-program-bench --target \
    bench_neograph bench_program bench_program_dispatch \
    bench_program_serialization_poc bench_program_binary_poc -j

# Positional arguments are iterations, warmup runs, and measured samples.
# bench_neograph additionally accepts par_workers before warmup/samples.
# bench_program's optional fourth argument measures closed-batch outer-run concurrency.
# Its burst rows are throughput-equivalent time, not individual request latency.
./build-program-bench/bench_neograph 10000 5000 1 10 5
./build-program-bench/bench_neograph 10000 5000 auto 10 5
./build-program-bench/bench_program 1000 10 5
./build-program-bench/bench_program 1000 10 5 8
./build-program-bench/bench_program_dispatch 100000 10 5
./build-program-bench/bench_program_serialization_poc 25 100
./build-program-bench/bench_program_binary_poc 25 100

# Build the opt-in protobuf/Cap'n Proto transport-envelope experiment.
# This target alone requires protoc/libprotobuf and capnp/libcapnp.
cmake -S . -B build-program-codec-poc -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_PROGRAM_CODEC_POC=ON
cmake --build build-program-codec-poc --target bench_program_codec_poc -j
taskset -c 0 ./build-program-codec-poc/bench_program_codec_poc 25 100
```

每个原生基准都会输出 `config`、`runtime`、`header` 和 `result` 记录。
请报告显式预热之后测量样本的中位数；不要拿单次的短时间运行去比较。
`bench_program` 使用内存存储，不调用 provider，也不访问网络。
`bench_program_dispatch` 只测量不可变的 `ProgramPlan` 查找和描述符遍历，不含 Core 的执行。

序列化的 POC 是离线的、在内存中进行的测量。它们捕获已完成的 Program 运行所产生的
不可变发布内容；序列化 POC 测量的是规范字节的复用，
二进制 POC 则是把当前的规范 JSON 信封，与保留每条嵌套记录规范字节的、
带长度前缀的信封做对比。二进制的结果是下界的实验，
不是用来取代现有持久化契约的方案。

`bench_program_codec_poc` 需要单独主动开启，它在同样的嵌套规范字节之上，
对比 protobuf 和 Cap'n Proto 的**传输信封**。
`*_envelope_only_*` 只测量这些字节已经就绪之后构造信封的开销。
`*_transport_total_lower_bound_*` 还包含构造嵌套规范字节的开销，但有意跳过了
`ProgramTransitionPublication` 外层的跨记录校验；它不是持久化或身份格式的基准。
恢复相关的指标会解析回持有数据的 Program 记录，
是唯一模拟了完整接收方的编解码数字。这些 POC 都不测量 SQLite/Postgres 的事务延迟，
也不测量端到端的 `ProgramRuntime` 延迟。

Python 框架的对比仍然是可选的，并且需要第三方包：

```bash
# Shared Python venv for every Python framework:
python3 -m venv /tmp/bench_venv
/tmp/bench_venv/bin/pip install \
    langgraph \
    haystack-ai \
    pydantic-graph \
    llama-index-core \
    "autogen-agentchat" "autogen-core" "autogen-ext"

# Run each bench (10k seq + 5k par matches the C++ side):
/tmp/bench_venv/bin/python benchmarks/bench_langgraph.py      10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_haystack.py       10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_pydantic_graph.py 10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_llamaindex.py     10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_autogen.py        10000 5000

# Peak RSS + wall time:
/usr/bin/time -f "%e s, %M KB" ./build-program-bench/bench_neograph
```

依赖外部服务的检查点基准、HTTP 基准和并发 Docker 基准是各自独立的实验；
上面确定性的原生 Core/Program 运行并不需要它们。

输出格式是以制表符分隔的 `config`、`runtime`、`header`、`result` 或 `metric` 记录。
原生的 result 行包含总耗时的中位数和每次迭代的耗时；Python 脚本仍然输出历史上的
`workload<TAB>iters<TAB>total_ms<TAB>per_iter_us` 行。

## CI 性能关卡

`CI extended` 工作流中的两个作业会检查基准测试结果。该工作流每晚运行，也可以手动触发，并且在关卡文件发生变化时运行。

* **`bench-ab`** 在同一台运行器上构建被测提交和基准（base）提交，在两者上运行 [`ci_ab_gates.json`](ci_ab_gates.json) 中列出的基准测试，并逐个用例比较中位数。托管运行器之间的差异大于值得发现的回归，所以只有在同一台机器上并排测量两个构建，才能看出几十个百分点的变慢。
* **`bench-regression`**（`scripts/check_ci_benchmarks.py`）把 HTTP、扇出和核心基准各运行一次，并与绝对限值比较。这些限值在托管运行器上留有很大余量，所以只作为最后一道防线保留。

`bench-ab` 运行的内容：

| 工作负载 | 基准测试 | 用例 |
|---|---|---|
| 核心运行路径 | `bench_neograph` | 顺序链；同一条链改用 sync 节点；在调用方 executor 和引擎工作线程池上执行的五路扇出；挂上空闲流回调和接收全部事件的流回调的链 |
| 异步扇出 | `bench_async_fanout` | 50,000 个定时器协程的吞吐量和峰值内存 |
| HTTP | `bench_async_http` | 通过回环接口，向进程内服务器发送池化调用的吞吐量 |
| Program | `bench_program_dispatch`、`bench_program` | 遍历计划的分派描述符；在内存存储上运行已准入的 Program 时的 `start()` 和整个运行 |
| 检查点存储 | `bench_checkpoint_store` | 写入内存存储和 SQLite（进程自己的临时目录）的每秒保存次数 |
| 规范 JSON 与标识 | `bench_canonical_json`、`bench_sha256` | ASCII、混合和转义的 64 KiB 字符串的规范字节；64 KiB 记录的 SHA-256 标识 |
| 提供方 outcome | `bench_provider_outcome_codec` | 聊天 outcome 和带工具调用的轮次的编码与解码 |

它们都不需要回环接口之外的网络、密钥或数据库服务。

### 比较如何进行

1. **基准。** 分支或拉取请求与它在 master 上的 merge base 比较，master 与其之前最近的发布标签比较。手动运行时可以指定其他提交或标签，也可以指定 `head`，即把构建与自身比较（A/A 运行）。基准用它自己的源码树，以相同的选项构建。
2. **样本。** 每个工作负载按条目中写明的次数运行，每个样本使用一个新进程。head 与 base 按样本序号交替先后顺序，所以运行期间机器速度的缓慢漂移会对两者产生同样的影响。
3. **判定。** 每个用例比较 head 样本的中位数与 base 样本的中位数。当比值高于 `tolerance`（越低越好时）或低于它（越高越好时），或者 head 中位数超出 `floor` 中的绝对限值，用例即为失败。
4. **复核。** 含有失败用例的工作负载会用新的样本再运行一次，只由重跑的结果决定。先失败后通过的用例会作为警告报告。
5. **无 base。** base 构建中没有该基准测试或输出的工作负载，会被报告为无 base，且不算失败，因此可以添加新的基准测试。它的绝对限值仍然适用。
6. **报告。** 作业日志和步骤摘要中有比值表，作业还会上传 `bench-ab-report`，这是一个 JSON 文件，包含每个样本、比值、机器信息和每个二进制文件的 SHA-256。

### 容差

[`ci_ab_gates.json`](ci_ab_gates.json) 包含关卡依赖的一切：每个工作负载的基准目标、参数和样本数；每个用例要从输出中读取的值、方向、容差（tolerance）、可选的绝对限值，以及校准容差所依据的噪声（`a_a_noise`，来自 `calibrated_on` 所指的那次运行）。`a_a_noise` 是 A/A 运行中中位数比值偏离 1 的最大距离。如果某个容差留出的空间小于该距离的 `minimum_noise_margin`（1.5）倍，或者缺少任何一个原有关卡的绝对限值，运行器都会拒绝这个文件。

噪声因机器而异，所以要在作为关卡的运行器上校准：触发 `CI extended` 并把 `bench_ab_base` 设为 `head`。这次运行会把构建与自身比较五次，不做重跑，报告会列出每次的比值。容差是 1 加上 0.15 与噪声两倍中较大者（越高越好时则是 1 减去它）。如果某项改动是有意让某个用例超出容差，就在同一个拉取请求里修改容差和对应的校准值来记录。

### 在本地运行

```bash
python3 scripts/verify_ci.py bench-ab --work-dir /tmp/bench-ab --jobs 4 --base v0.15.1
python3 scripts/verify_ci.py bench-ab --work-dir /tmp/bench-ab-aa --jobs 4 --base head --calibrate 5
# Two build directories that already exist:
python3 scripts/run_ci_ab_gates.py --head-dir build --base-dir build-base --output report.json
```

`verify_ci.py bench-ab` 只构建文件中列出的基准目标。机器同时在编译时，测量值会波动：请看比值而不是绝对时间，或者换一台安静的机器。

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

## 带类型 provider 的切换：真实 GraphEngine 的前后对比

下面的切换、最终验证和交接（handoff）测量，是使用 SDK 接口/共享库代数 3 记录的。
这些记录里的“当前”和“最终”，指的是它们各自原来的 cohort，而不是当前的发布版本。

这是另一项**本地 TLS HTTP/SSE** 的测量，不是上面不带检查点的 Python 框架基准，也不是模型推理。
它通过生产环境的 `GraphEngine.llm_call/tool_dispatch`，在面向 Linux x64 的静态 Release / GCC 13.3 构建上，
测量了 **16 种配置 × 3 个全新进程 = 48 条记录**。
其中涵盖三种通用的 H1 工作负载、五个请求族的缓冲/SSE native 续接，以及三个 H2 用例。
没有调用任何付费 API。

[最初的九条记录](provider-cutover-legacy-results.json)来自未经修改的 `7b47ad43`；
[当前的原始记录](provider-cutover-current-results.json)和[标量对比](provider-cutover-summary.json)保留了
生效的控制项、分布、RSS/线程数、对端的实际计数器，以及持有所有权的 outcome。

| 通用的图工作负载 | 之前 p50 ms | 之后 p50 ms | 之前图运行/秒 | 之后图运行/秒 | 之前峰值 RSS MiB | 之后峰值 RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| H1 缓冲文本 | 0.707 | 1.320 | 1,375.58 | 733.65 | 12.617 | 15.465 |
| H1 缓冲工具循环 | 29.337 | 30.510 | 800.30 | 702.64 | 17.832 | 20.219 |
| H1 SSE 工具循环 | 600.540 | 37.593 | 51.43 | 615.38 | 14.414 | 20.367 |

数值是三次重复的中位数，**按每次图运行**计，不是按每个 HTTP 请求计。文本场景的并发为 1，
对端没有延迟；工具循环的并发为 32，对端每个请求延迟 5 ms，每次图运行包含两个生成请求。
每个 cohort 预热 10 次、测量 100 次运行；工具和 native 的 cohort 在每次重复中还会发起八次取消。

在当前所有 cohort 中，没有任何一次测量运行失败。

取舍：完整的、持有所有权的原始/native/可空结果，以及权限闸门，会增加文本场景的延迟和内存；
缓冲工具循环的 p50 略有上升。SSE 则不再有旧的缓冲路径带来的延迟。
旧版本不支持 native/可空的权限，也不支持若干 worker 参数，所以这些只是同一工作负载下的测量，
并不等于语义或资源上的等价，也不是对模型加速的承诺。当前的 p95/p99、首个语义事件、
取消的收尾耗时和扩展请求族的各行，都在 JSON 摘要里。

```sh
cmake -S . -B build-provider-bench -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DNEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=OFF \
  -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_PROGRAM=OFF -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE_DIR" \
  -DSP_BUILD_TESTS=OFF -DSP_BUILD_BENCHMARKS=OFF
cmake --build build-provider-bench --target neograph_provider_cutover_benchmark
for config in benchmarks/provider_cutover_h1_*.json benchmarks/provider_cutover_extended_*.json; do
  build-provider-bench/neograph_provider_cutover_benchmark --config "$config" || exit "$?"
done
```

在 NeoGraph 根目录下运行，需要 Node.js、OpenSSL 命令行和 SDK 的常规依赖。
对端使用临时的私有 CA；不会替换系统信任库或任何依赖库。


## 最终验证 cohort：全新的带类型 provider GraphEngine 测量

在同样的 GraphEngine 和本地 TLS HTTP/SSE 配置上，后来重新运行的 Release cohort，完成了
**16 种配置 × 3 次全新进程重复 = 48 条记录，0 次失败，用时 38.29 秒**。
没有调用付费模型。它不能取代上面的历史表格。
[最终的标量摘要](provider-cutover-final-summary.json)和
[最终的原始合成对象（持有所有权）](provider-cutover-final-results.json)，
与[旧版 9 条记录的 cohort](provider-cutover-legacy-results.json)、
[较早的当前版 48 条记录的 cohort](provider-cutover-current-results.json)以及
[较早的对比](provider-cutover-summary.json)相互独立。
大约 60 MB 的原始文件里是实际的合成 outcome 对象，不含 provider 的机密。

下面的数值，是摘要中三次独立进程重复的中位数，**按每次图运行**计，不是按每个模型 token
或每个 HTTP 请求计。
资源/控制项以及 native/可空权限方面的差异依然存在：**语义和资源上的等价并不成立**；
这些是本地实测的取舍，不是模型加速。

### 最终 16 种配置的测量值

| 请求族 | HTTP | 工作负载 | p50 ms | p95 ms | p99 ms | 图运行/秒 | 峰值 RSS MiB | 峰值线程数 |
|---|---|---|---:|---:|---:|---:|---:|---:|
| `openai.chat` | H1 | 缓冲文本 | 1.296943 | 1.349401 | 1.397812 | 769.975584 | 15.898438 | 3 |
| `openai.chat` | H1 | 缓冲工具 | 30.672507 | 100.492667 | 112.663949 | 678.507993 | 20.585938 | 19 |
| `openai.chat` | H1 | SSE 工具 | 35.800113 | 105.431034 | 117.559931 | 627.047345 | 21.042969 | 19 |
| `openai.chat` | H1 | native 缓冲 | 32.546693 | 102.680190 | 115.244483 | 683.637830 | 20.925781 | 19 |
| `openai.chat` | H1 | native SSE | 38.135188 | 115.463732 | 126.668501 | 615.357297 | 21.902344 | 19 |
| `anthropic.messages` | H1 | native 缓冲 | 30.612805 | 103.979300 | 123.110448 | 678.785191 | 20.324219 | 19 |
| `anthropic.messages` | H1 | native SSE | 44.021986 | 71.548005 | 93.941146 | 604.744184 | 22.125000 | 19 |
| `openai.responses` | H1 | native 缓冲 | 35.070430 | 100.615954 | 114.597387 | 647.751880 | 22.179688 | 19 |
| `openai.responses` | H1 | native SSE | 46.670934 | 77.636952 | 89.664406 | 571.458359 | 25.511719 | 19 |
| `google.generate` | H1 | native 缓冲 | 31.449930 | 99.319459 | 112.385528 | 698.783682 | 21.726562 | 19 |
| `google.generate` | H1 | native SSE | 37.774704 | 105.703383 | 120.826886 | 627.040287 | 22.500000 | 19 |
| `google.interactions` | H1 | native 缓冲 | 32.588486 | 101.365734 | 109.841457 | 690.967042 | 21.664062 | 19 |
| `google.interactions` | H1 | native SSE | 46.786625 | 76.036675 | 92.590316 | 573.469300 | 23.058594 | 19 |
| `openai.chat` | H2 | 缓冲文本 | 1.311873 | 2.443117 | 2.604176 | 701.785481 | 16.148438 | 3 |
| `openai.chat` | H2 | SSE 工具 | 40.349311 | 54.813608 | 59.912728 | 714.064349 | 20.386719 | 19 |
| `google.interactions` | H2 | native SSE | 44.561127 | 56.692743 | 64.665852 | 663.966743 | 22.750000 | 19 |

### 三个共有的 H1 工作负载：未改动的旧版与最终 cohort

| H1 图工作负载 | 之前 p50 ms | 最终 p50 ms | 之前图运行/秒 | 最终图运行/秒 |
|---|---:|---:|---:|---:|
| 缓冲文本 | 0.706631 | 1.296943 | 1375.578826 | 769.975584 |
| 缓冲工具 | 29.336983 | 30.672507 | 800.301406 | 678.507993 |
| SSE 工具 | 600.539987 | 35.800113 | 51.429767 | 627.047345 |

“之前”的数值是原来 7b47ad43 的 cohort。文本场景的延迟上升、吞吐下降；缓冲工具循环的变化不大；
SSE 去掉了旧缓冲路径带来的延迟。扩展请求族/协议、首个语义事件、取消、native 重放和保留的 outcome，
都保存在最终的摘要和原始记录里。

## 事件驱动的 Provider 交接：对应的轮询与合并通道

[实测摘要](provider-notification-summary.json)：每个 cohort 8 种配置 × 3 个全新进程 = 48 条记录，
4800 次测量的图运行，0 次失败。
构建为 GCC 13.3 Release，静态链接、开启加固，native 优化关闭；使用同样的本地 TLS 检验对端和已准入的控制项，
每个进程预热 10 次、测量 100 次运行；测量期间没有并行的编译任务，也没有模型推理。
数值是各进程统计量的中位数，不是置信区间，也不是模型的 token 速率。下面的负载大小是测试夹具中文本的填充长度；
填充为 0 时，响应信封仍有 253 B。

| H1 工作负载 | 之前 p50 ms | 之后 p50 ms | 之前图运行/秒 | 之后图运行/秒 |
|---|---:|---:|---:|---:|
| 文本 256，并发 1 | 1.302474 | 0.952992 | 738.901074 | 1022.958838 |
| 文本 0，并发 1 | 1.286646 | 0.912571 | 761.940993 | 1070.657541 |
| 文本 4 KiB，并发 1 | 1.387636 | 1.128831 | 709.024147 | 870.631703 |
| 文本 64 KiB，并发 1 | 4.873767 | 4.144619 | 196.243991 | 240.268946 |
| 文本 256，对端延迟 5 ms | 6.601920 | 6.425313 | 146.906115 | 155.052016 |
| 文本 256，并发 32 | 7.641085 | 6.939675 | 1645.559595 | 1803.087419 |
| 缓冲工具，并发 32 | 30.548201 | 31.348443 | 710.487696 | 692.440963 |
| SSE 工具，并发 32 | 33.597342 | 38.607118 | 655.586834 | 611.740359 |

小文本场景的 p50 下降了 26.83%，吞吐上升了 38.44%；工具/SSE 的吞吐分别下降了 2.54% 和 6.69%。
“之后”一侧不再轮询，而是通过容量为 1 的并发通道合并 provider 的通知。
另有单独的、带插桩的 180 次操作的运行，测得 SDK 从发布到排空的 p50 在之前为 383.7125 µs，之后为 36.4165 µs；
定时器等待从 298 次降到 0 次，通知等待为 180 次。这些探针数据不包含在生产环境的延迟表中。

资源峰值取自 1 ms 间隔的采样。这些测量只在 Linux/POSIX 上进行。
