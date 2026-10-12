<!-- neograph-i18n: source=docs/PROGRAM_COST_MEASUREMENT.md locale=zh-CN source_sha256=95fe571a9d490d4a096a313377958d64fd9c63a35f4ee437d8d8d33876a18799 -->
# 测量 Program 的成本

**Languages:** [English](PROGRAM_COST_MEASUREMENT.md) | [한국어](PROGRAM_COST_MEASUREMENT.ko.md) | [日本語](PROGRAM_COST_MEASUREMENT.ja.md) | [简体中文](PROGRAM_COST_MEASUREMENT.zh-CN.md)

`bench_program_cost` 和 `scripts/run_program_costs.py` 提供一份诊断性的基线，
用来决定该优化 Program 的哪些开销。它们不改变运行时的语义，
也不取代预先登记的 QuickJS 性能闸门。
不需要 LLM、网络上的模型调用或模型凭据。

要对比两种实现，`scripts/compare_program_costs.py` 会以全新的进程对，
交替运行两个冻结的 `bench_program_cost` 二进制文件。
`bench_canonical_json CASE BYTES ITERATIONS` 则单独针对一个标量输出基准结果（oracle），
测试 ASCII、Unicode、混合以及大量转义的字符串。

## 构建与运行

在原生文件系统上，使用经过优化的构建。例如在 Linux 上：

```sh
cmake -S . -B build-cost -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON
cmake --build build-cost -j4 --target \
  bench_program_cost bench_quickjs_primitives bench_quickjs_control
python3 scripts/run_program_costs.py \
  --build-dir build-cost --output-dir /tmp/neograph-cost-results
```

runner 要求使用一个**新的**输出目录。它的默认矩阵使用内存和 SQLite、七次独立的进程重复、
十次预热调用，以及每个生命周期用例 100 次正式测量的调用。`--iterations` 和 `--warmup`
控制生命周期/直接用例；生成器的微基准使用 1,000 条命令。
各用例串行运行，在交替的重复中，按正序和倒序轮流执行。
SQLite 文件存放在输出目录里，保留下来可供检查。

要包含 PostgreSQL，先启动一个专用的本地测试容器，
把 `NEOGRAPH_COST_POSTGRES_URL` 设为它映射出来的 localhost 连接 URL，
再加上 `--postgres-container <container-name>`。runner 会检查映射出来的端口。
它为每个样本创建一个名称唯一的数据库，之后只删除这个数据库。
它不会启动或停止容器，也不会使用或删除 URL 中指明的那个数据库。
这台一次性的服务器，必须允许 `postgres` 通过 `docker exec` 创建和删除数据库。
不要指向应用的服务器。凭据不会写入元数据或命令记录。

## 每个范围包含什么

| 用例 | 包含 | 不包含 |
| --- | --- | --- |
| `direct` | 预热后的 `GraphEngine::run`，3 个顺序递增的节点，2 个状态通道 | Program 准入(admission)、journal、检查点持久化、JS |
| `lifecycle --mode cpp` | 已准入的 C++ 构建器 v1 Program、真实的 Core、结果的构造、journal 和检查点 | JavaScript 控制的执行；编译和准入是单独的冷启动字段 |
| `lifecycle --mode javascript` | 已准入的 `define()` + 生成器 `main()`、真实的 Core、JS/C++ 转换、命令 journal 和检查点 | 编译和准入是单独的冷启动字段 |
| `generator` | 生成器的打开/编译/初始化、第一次 `next`、重复的宿主命令往返、终态转换、清理 | 真实的 Core、调度、journal、数据库 I/O；响应是合成的 |
| `resident` | 多个相互独立的 QuickJS 运行时，在各自的第一条命令之后挂起 | 完整的 Program 智能体、catalog、引擎和 journal 存储 |
| 已有的 primitive/control 用例 | `bench_quickjs_primitives` 和 `bench_quickjs_control` 中已记录的范围 | 这些样本不是开关对比的闸门运行 |

直接、C++ 和 JavaScript 三种运行，使用同样的三节点拓扑、递增行为和 payload 通道。
每次真实的运行，都必须以计数器 `3` 和完整未变的 payload 结束。
payload 的大小为 0、4,096 和 65,536 字节。
JavaScript 多命令的各行，把同一张 Core 图执行 4 次或 16 次，
每次调用都把计数器的输入重置为零。它们最终的计数器是 `3`。

C++ v1 恰好准入一个 Program 操作。JavaScript 生成器使用宿主声明的 64 个操作的上限，
以支持所有命令数量的各行。两者使用同样的其余资源上限和一个调度线程。
它们的源码形态和命令 journal 的语义有所不同：用 JavaScript 的数据减去 C++ 的数据，
**不能**分离出解释器的耗时。

每个生命周期用例，都用选定的后端构造 ProgramStore、CheckpointStore 和 ProgramTransitionStore。
内存模式保留真实的内存 journal/检查点值；它不会绕过持久化接口。
SQLite 使用一个数据库文件，各存储持有独立的连接。
已有的 CheckpointStore 设置了 WAL 和 `synchronous=NORMAL`；
Program 存储的连接保持 SQLite 同步的默认值。PostgreSQL 使用已有的存储实现，
以及一个四连接的检查点连接池。对比结果时，要记录实际的服务器设置。
这些是各后端的默认值，并不表示它们具有相同的断电保证。

## 如何解读耗时和内存字段

`samples` 包含每一次正式测量的调用；`first_run` 在预热完成之前单独记录。
`cold` 包含存储的构造、Program 的编译、准入和 Runtime 的构造。
这些字段不含部分夹具的准备工作，因此不是对进程启动的完整划分。

在一次顺序调用之内，`before_core_us`、`core_span_us`、`after_core_us` 和 `wake_us`，
依据 Program 事件的时间戳，划分了从 start 到 wait 返回的挂钟时间。
**Core 事件的跨度不是纯粹的 Core CPU 时间：** 它包含检查点的工作，
在多命令的各行里，还包含 Core 调用之间的间隔。
`start_us` 和 `post_start_us` 给出第二种划分。绝不要把这两种划分加在一起；
Core 的工作可能在 `start()` 返回之前就已开始。

事件 sink 记录的是原子的时间戳。因此，生命周期的结果包含了这个观察者本身的开销，
而直接/生成器用例没有事件 sink。这些是诊断性的耗时，并不是不含观察者开销的最小延迟。

runner 先对每个进程做汇总，再对这些相互独立的进程结果做汇总。
`invocation_median.total_us.median` 是各进程中位数的中位数。
`invocation_p95.total_us.median` 是各进程内部 p95 的中位数。
百分位数采用最近秩（nearest rank）；七个进程样本时，进程层面的 p95 就是其中的最大值。
原始样本、MAD 和极值仍然可用。不要由中位数之间的微小差异，推断统计显著性。

Linux 的 RSS 是进程的内存，不是 JS 的分配核算。生命周期用例里 RSS 的增长，
包含保留下来的运行历史和分配器的行为。常驻生成器的 RSS，包含运行时/引导的开销和分配器的粒度；
要估算增量的占用，请使用更大的生成器数量之间的斜率。
两者都不是对每个递归智能体总内存的测量。在其他平台上，内存字段为零，
表示仅限 Linux 的探针不可用。

runner 会记录二进制/源码的哈希、源码提交和已跟踪差异的哈希、CMake 选项、CPU 以及进程亲和性。
任何报告都要附上原始证据，并且在一次测量运行期间，不要重新构建二进制文件。

## Linux 上可选的数据库 API 诊断

`benchmarks/program_store_profile.c` 可以在不改动 Program 运行时的情况下，
统计数据库库调用的次数和耗时。要在主要的、不带性能分析的矩阵**完成之后**，再单独构建它：

```sh
cc -O2 -shared -fPIC -I/usr/include/postgresql \
  benchmarks/program_store_profile.c -o /tmp/program_store_profile.so -ldl -pthread
NEOGRAPH_COST_STORE_PROFILE=/tmp/new-store-profile.json \
LD_PRELOAD=/tmp/program_store_profile.so \
  build-cost/bench_program_cost --case lifecycle --backend sqlite \
  --storage /tmp/new-cost-profile.sqlite --iterations 40 --warmup 0
```

这个分析器只记录类别以及汇总的次数和耗时，从不记录 SQL 文本、参数或凭据。
它测量 `sqlite3_step`，或者同步的 `PQexec` 和 `PQexecParams`，以及 `sqlite3_exec`（包括事务控制）。
为避免重复计数，`exec` 内部嵌套的 SQLite step 不计入。
一次查询的逐行迭代，可能会多次调用 `step`。异步的 libpq 调用、连接的建立、
SQLite 的 prepare 时间，以及调用方一侧的序列化，都不在这些 API 的区间之内。
整个进程的总数，包含构造函数、创建 schema、准入和预热。性能分析会带来额外开销，
并发的 API 区间也可能重叠，所以不能把这些数值，当作精确的 CPU 划分，从基线挂钟时间中减去。
要把固定的初始化调用次数，与每次运行的调用次数区分开，
可以用不同的调用次数，在全新的数据库上分别运行并对比。

## 这份基线的边界

这个矩阵测量的是顺序的合成执行，以及挂起的生成器所占用的资源。
它不能证明租户公平性、并发吞吐量、递归 spawn、在线替换、进程丢失后的恢复、
生产环境的存储，或 JIT 后端合格。生成器重放的各行，重放的是记录下来的合成命令响应；
它们不对完整的持久重连计时。要判断 JIT 的潜力，需要做一次包含预热、编译和内存开销的真实对比。
