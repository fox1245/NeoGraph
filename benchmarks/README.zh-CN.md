<!-- neograph-i18n: source=benchmarks/README.md locale=zh-CN source_sha256=bbfba1a87c6a9a4f4f7c7a732df64ce07c6ddacc41c9b8ac7024042f05a40fba -->
# NeoGraph 与 Python 图/流水线框架对比：引擎开销基准测试

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

在两个没有 I/O、休眠、模型调用和持久化检查点的小型工作负载上，测量 NeoGraph 与五个 Python 编排框架的每次运行的引擎开销，
同时测量各自的内存、启动时间和体积。测量范围包括节点派发、状态通道写入和 reducer 调用。

本页的所有表格和图表，都由 `scripts/render_bench_report.py` 根据一份已入库的记录（[`records/CURRENT`](records/CURRENT) 所指的文件）生成。
记录包含每个单元的原始样本和会话的来源信息：提交、编译器与标志、CPU、内核、软件包版本、平均负载以及脚本的 sha256。
表格与记录不一致时，测试会失败。此前在不同会话中、混用版本测得的表格，连同对现已确认无效之处的说明，保留在
[2026-04 历史记录](history/engine-overhead-2026-04.md)中，不能与本页的表格相比较。
当前 NeoGraph `0.16.0` 的构建方案需要 alpha SDK `0.4.0`、接口修订/共享代数 7，以及与之匹配的头文件和库。

关于 Program 准入、JavaScript 控制以及 SQLite/PostgreSQL 运行时开销，请参阅[Program 成本测量](../docs/PROGRAM_COST_MEASUREMENT.md)。
那个独立的测量矩阵包含日志/检查点工作，不能当作下面不含检查点的 Core 对比来呈现。

## 同批测量（cohort）

NeoGraph 与五个框架在同一台机器上、同一个会话中完成安装、构建和测量。**HEAD** 是框架默认分支的最新提交，
按表中所示的提交从 git 安装；**release** 是当时 PyPI 上的最新发布版。release 构建只运行匹配单元，读者可借此看出 HEAD 与发布版是否有差别。
若某个 HEAD 构建无法安装或导入，就用发布版代替，并在表中注明。NeoGraph 是表中所示的 `origin/master` 提交，
采用默认的 Release 配置构建，其中包含加固标志（见[引擎上限](#engine-ceiling-and-the-hardening-delta)）。

<!-- bench-record:begin cohort -->
| 框架 | 最新发布版 | HEAD（提交、日期、版本） |
| :--- | :--- | :--- |
| LangGraph | langgraph 1.2.16, langchain-core 1.6.12 | langgraph 1.2.16, langchain-core 1.6.12; `langchain@e6d83ef` (2026-10-20), `langgraph@3cd57c0` (2026-10-21) |
| Haystack | haystack-ai 3.4.0 | haystack-ai 3.5.0-rc0; `haystack@276746c` (2026-10-21) |
| pydantic-graph | pydantic-graph 2.59.0 | pydantic-graph 2.59.1.dev4+e0ccafa8a; `pydantic-ai@e0ccafa` (2026-10-21) |
| LlamaIndex Workflow | llama-index-core 0.14.25, llama-index-workflows 2.26.0 | llama-index-core 0.14.25, llama-index-workflows 2.26.0; `llama_index@619d746` (2026-10-14), `workflows-py@f3528e8` (2026-10-21) |
| AutoGen GraphFlow | autogen-agentchat 0.7.5, autogen-core 0.7.5 | autogen-agentchat 0.7.5, autogen-core 0.7.5; `autogen@004749e` (2026-10-16) |

主机：ARM Neoverse-N1，4 核，内核 6.17.0-1018-oracle，Python 3.12.3，构建 NeoGraph 的编译器 g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0；CPU 调频策略：主机未提供。

会话：2026-10-21T13:24:06Z 至 2026-10-21T13:59:49Z UTC；1 分钟平均负载在第一阶段开始时（等待主机空闲之后）为 0.26，结束时为 0.97（含会话自身的进程）；12 轮交错运行；记录 `2026-10-21-compare-arm-neoverse-n1.json`。

NeoGraph 引擎：提交 `db0fc651e2c9`；测试工具树：提交 `10d6c77a7d42`。该测试工具提交是运行本次会话的本地提交，可能不在本仓库的历史中；记录不会被修改，其中保存了每个测试工具脚本的 sha256，据此可确定文件。本树中的文件与已哈希的文件逐字节相同，例外是 `scripts/render_bench_report.py`（渲染这些表格的版本的哈希见下方）以及 `benchmarks/compare/` 中的 `port_llamaindex.py` 和 `port_autogen.py`（会话之后各修正了一处注释）。

表格由 `scripts/render_bench_report.py`（sha256 `db933a39bedf3733`…）渲染。
<!-- bench-record:end cohort -->

## 工作负载

所有框架移植同样的两个工作负载。图、流水线或工作流在计时区间之外只构建一次（框架允许时复用实例），计时循环负责调用。
状态与拓扑上的调整列在下面。

| Id | 形态 | 状态 |
|----|-------|-------|
| `seq` | 3 节点链 `a → b → c` | 单个 `counter` 通道，每个节点写入 `counter+1`（overwrite reducer） |
| `par` | 扇出 5 个 worker，再在 `summarizer` 汇合 | `results: list`（append reducer）与 `count: int`；每个 worker 追加自己的索引，summarizer 写入 `len(results)` |

每次运行都会检查：最终状态必须符合预期（counter 为 3；五个索引且 count 为 5）。检查针对每个样本的最后一次运行，以及计时器停止后额外的一次运行。
答案错误、运行失败或没有任何完成运行的单元属于错误单元：不公布它的计时，并在记录中列出。

所有框架都没有配置持久化检查点。AutoGen 会累积一次运行的消息线程，这是该框架的每次运行开销。

移植说明：

* **Haystack** 没有 append reducer：每个 worker 在自己的类型化 socket 上输出，summarizer 对列表长度求和。每次运行派发的组件数量相同。基准进程中关闭了遥测。
* **pydantic-graph** 现在只有一套 API，即 `GraphBuilder`：步骤是 `async def`，指向多个步骤的边会广播，汇合（join）等待它们。`par` 是真正的扇出：
  从起始节点出发的五个步骤，用 `reduce_list_append` 汇合，再交给 summarizer 步骤。文档中最先展示的 `BaseNode` 类每次只返回一个下一节点，
  因此对它而言 `par` 是六个节点的串行链；该变体只出现在惯用默认值部分。
* **LlamaIndex** 用 `ctx.collect_events` 汇合，因此 summarizer 步骤函数对每个结果事件运行一次（共五次，等待期间的四次返回 `None`）。
  `Workflow` 实例可以跨多次运行、也可以并发复用（见探针），所以匹配行复用同一个实例；惯用默认值行每次运行新建实例。
* **AutoGen** 是消息传递而非状态通道。计数器编码在消息文本中，summarizer 报告它收到的 worker 消息，并且每次运行前都调用 `reset()`，
  这与 AutoGen 文档中开始新任务前重置团队的做法一致；它会清除管理器的消息线程。不调用 `reset()` 而重复运行同样会返回正确答案
  （0.7.5：图完成时会自行重置执行状态），这种情况在此未计时。
* **LangGraph** 的节点在 `ainvoke` 下是 `async def`，在 `invoke` 下是普通 `def`；其余图结构相同。

## 匹配的运行方式：唯一带比值的部分

比较两种执行方式，各有自己的 NeoGraph 行：

* **单线程。** `worker_count=1` 的 NeoGraph 对比各框架使用 `async def` 节点的协程路径：LangGraph 的 `ainvoke`、使用协程组件的 Haystack `run_async`，
  以及 pydantic-graph（其唯一路径）、使用 `async def` 步骤的 LlamaIndex 和 AutoGen（其唯一路径）。节点在调用方线程上运行。
* **线程池。** `set_worker_count_auto()` 的 NeoGraph 对比存在线程池路径且已计时的框架的该路径：LangGraph 的 `invoke`（扇出步骤在 langchain-core 每次调用时新建的线程池上运行）
  和使用纯同步组件的 Haystack `run_async`（每个组件通过 `asyncio.to_thread` 运行）。没有此类路径或未计时的框架给出带原因的“不适用”行，不做模拟。
  LlamaIndex Workflow 属于后者：它接受普通 `def` 步骤并在事件循环的默认执行器上运行，但该路径不在本次测量中。线程池保持默认宽度
  （NeoGraph：每个硬件线程一个，此处为 4；Python 执行器：`min(32, 核数 + 4)`）；[`dr_compare` 指南](dr_compare/README.md)则把二者对齐。
  对 `seq` 而言，每个步骤都只有一个就绪节点，NeoGraph 和 LangGraph 的 `invoke` 都以内联方式运行它，因此两边的线程池都没有被用到；
  LangGraph 这一行仍是该框架默认的同步路径。

比值是框架的中位数除以同一运行方式、同一工作负载下 NeoGraph 行的中位数。NeoGraph 的 `par` 取决于其 worker 模式，其范围写在第二张表下方，不用单一比值概括。

表头之下的各行是同一框架的 HEAD 构建和最新发布版。

### 单线程

<!-- bench-record:begin matched_single -->
| 框架 | 来源 | `seq` 每次运行 µs | `par` 每次运行 µs | `seq` 相对 NeoGraph | `par` 相对 NeoGraph |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **NeoGraph `worker_count=1`** |  | **9.72 [9.68, 10.1]** | **21.5 [21.4, 23.6]** | 1× | 1× |
| Haystack — Pipeline.run_async，协程组件 | HEAD `276746c` (3.5.0-rc0) | 272 [266, 275] | 499 [497, 507] | 27.9× | 23.2× |
| Haystack — Pipeline.run_async，协程组件 | 发布版 3.4.0 | 273 [266, 276] | 501 [499, 505] | 28.1× | 23.3× |
| pydantic-graph — GraphBuilder 步骤，真正的扇出 | HEAD `e0ccafa` (2.59.1.dev4+e0ccafa8a) | 483 [476, 494] | 1,277 [1,252, 1,308] | 49.7× | 59.3× |
| pydantic-graph — GraphBuilder 步骤，真正的扇出 | 发布版 2.59.0 | 487 [480, 501] | 1,271 [1,256, 1,294] | 50.0× | 59.1× |
| LangGraph — ainvoke，async def 节点 | HEAD `3cd57c0` (1.2.16) | 1,248 [1,232, 1,273] | 1,777 [1,754, 1,808] | 128× | 82.6× |
| LangGraph — ainvoke，async def 节点 | 发布版 1.2.16 | 1,258 [1,242, 1,280] | 1,773 [1,750, 1,792] | 129× | 82.4× |
| LlamaIndex Workflow — 复用同一个工作流实例 | HEAD `619d746` (0.14.25) | 2,272 [2,219, 2,297] | 7,069 [6,937, 7,136] | 234× | 329× |
| LlamaIndex Workflow — 复用同一个工作流实例 | 发布版 0.14.25 | 2,364 [2,339, 2,396] | 7,393 [7,235, 7,458] | 243× | 344× |
| AutoGen GraphFlow — GraphFlow 先 reset 再 run | HEAD `004749e` (0.7.5) | 4,192 [4,135, 4,286] | 9,543 [9,346, 9,734] | 431× | 444× |
| AutoGen GraphFlow — GraphFlow 先 reset 再 run | 发布版 0.7.5 | 4,224 [4,115, 4,274] | 9,521 [9,405, 9,645] | 434× | 443× |

数值是 12 轮交错运行的中位数，括号内是各轮之间的 [最小, 最大]；每轮以打乱的顺序把每个单元各运行一次，每个单元使用一个全新的进程。比值为中位数相除。
<!-- bench-record:end matched_single -->

![引擎开销基准测试，匹配的单线程方式：每次运行的微秒数](../docs/images/bench-engine-overhead.svg)

### 线程池

<!-- bench-record:begin matched_pool -->
| 框架 | 来源 | `seq` 每次运行 µs | `par` 每次运行 µs | `seq` 相对 NeoGraph | `par` 相对 NeoGraph |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **NeoGraph `set_worker_count_auto()`** |  | **10.2 [10.1, 10.5]** | **86.0 [82.8, 86.9]** | 1× | 1× |
| Haystack — Pipeline.run_async，在线程中运行的同步组件 | HEAD `276746c` (3.5.0-rc0) | 759 [745, 779] | 1,424 [1,389, 1,460] | 74.8× | 16.6× |
| Haystack — Pipeline.run_async，在线程中运行的同步组件 | 发布版 3.4.0 | 757 [738, 780] | 1,426 [1,388, 1,459] | 74.5× | 16.6× |
| pydantic-graph |  | 不适用 — 没有线程池路径：步骤是协程，普通函数会被拒绝 |  |  |  |
| LangGraph — invoke，普通 def 节点 | HEAD `3cd57c0` (1.2.16) | 1,063 [1,049, 1,079] | 3,184 [3,146, 3,224] | 105× | 37.0× |
| LangGraph — invoke，普通 def 节点 | 发布版 1.2.16 | 1,066 [1,053, 1,088] | 3,194 [3,163, 3,301] | 105× | 37.1× |
| LlamaIndex Workflow |  | 不适用 — 未测量：接受普通 `def` 步骤并在事件循环的默认执行器上运行，这条线程池路径不在本次测量范围内 |  |  |  |
| AutoGen GraphFlow |  | 不适用 — 没有线程池路径：单线程的智能体运行时 |  |  |  |

NeoGraph 的 `par` 在 `worker_count=1` 时为 21.5 µs，在 `set_worker_count_auto()` 时为 86.0 µs（4 个工作线程）。
<!-- bench-record:end matched_pool -->

## 惯用默认值：只给绝对时间，不给比值

各框架自己的文档最先展示的 API。这些行彼此做的事情不同（每次运行构建对象、线程切换、串行链），因此只给绝对时间，不形成比值。

<!-- bench-record:begin idiomatic -->
| 框架 | 测量的 API | `seq` 每次运行 µs | `par` 每次运行 µs |
| :--- | :--- | ---: | ---: |
| **NeoGraph** | **NeoGraph `run()`（默认 `worker_count=1`）** | **9.72 [9.68, 10.1]** | **21.5 [21.4, 23.6]** |
| Haystack | Pipeline.run，同步组件 | 143 [143, 144] | 321 [318, 325] |
| pydantic-graph | GraphBuilder 步骤，真正的扇出 | 483 [476, 494] | 1,277 [1,252, 1,308] |
| pydantic-graph | BaseNode 类（par 为串行链） | 535 [525, 547] | 818 [802, 823] |
| LangGraph | invoke，普通 def 节点 | 1,063 [1,049, 1,079] | 3,184 [3,146, 3,224] |
| LlamaIndex Workflow | 每次运行新建工作流实例 | 2,615 [2,577, 2,656] | 7,536 [7,408, 7,729] |
| AutoGen GraphFlow | GraphFlow 先 reset 再 run | 4,192 [4,135, 4,286] | 9,543 [9,346, 9,734] |
| AutoGen GraphFlow | 每次运行新建 GraphFlow | 5,274 [5,239, 5,372] | 11,280 [11,082, 11,549] |
<!-- bench-record:end idiomatic -->

<a id="engine-ceiling-and-the-hardening-delta"></a>
## 引擎上限与加固带来的差异

上面的 NeoGraph 行是默认的 Release 构建，它在 `-O3 -DNDEBUG` 之上加入了 `-D_GLIBCXX_ASSERTIONS`、`-fstack-protector-strong`、
`-D_FORTIFY_SOURCE=2`、位置无关代码和 RELRO。去掉这些的第二个构建（`-DNEOGRAPH_ENABLE_HARDENING=OFF`，另加 `-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection`，
因为发行版编译器默认会开启这些）就是引擎上限。两个构建的标志，以及每个二进制文件编译时看到的宏，都打印在表格下方。

<!-- bench-record:begin ceiling -->
| 构建 | `seq`, `worker_count=1` | `par`, `worker_count=1` | `par`, `set_worker_count_auto()` |
| :--- | ---: | ---: | ---: |
| 默认 Release 构建（加固）— 基准数值 | 9.72 [9.68, 10.1] | 21.5 [21.4, 23.6] | 86.0 [82.8, 86.9] |
| 未加固的引擎上限 | 9.88 [9.86, 10.6] | 20.7 [20.5, 22.2] | 84.6 [81.9, 86.5] |
| 加固 ÷ 未加固 | 0.98× | 1.04× | 1.02× |

| 构建 | µs |
| :--- | ---: |
| `seq`，生成每个生命周期事件并交给回调 | 10.7 [10.7, 11.6] |

默认构建的标志与编译器宏（由二进制文件和构建树输出）：

```text
[hardened] build type: Release; g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
[hardened] NEOGRAPH_ENABLE_HARDENING: ON
[hardened] compile: /usr/bin/c++ -DASIO_NO_DEPRECATED -DASIO_STANDALONE -DNEOGRAPH_BENCH_BUILD_TYPE=\"Release\" -DNEOGRAPH_STATIC_BUILD -I<work>/ng/include -I<work>/ng/deps/yyjson -I<work>/ng/deps/asio/include -I<work>/build-hardened/generated -I<work>/build-hardened/_deps/neograph_schemaprovider-src/src -I<work>/build-hardened/_deps/neograph_schemaprovider-build/generated -O3 -DNDEBUG -std=gnu++20 -fPIE -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -D_FORTIFY_SOURCE=2 -o CMakeFiles/bench_neograph.dir/benchmarks/bench_neograph.cpp.o -c <work>/ng/benchmarks/bench_neograph.cpp
[hardened] macros seen by the binary: _GLIBCXX_ASSERTIONS=True, _FORTIFY_SOURCE=2, stack protector=strong, PIE=True
[unhardened] build type: Release; g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
[unhardened] CMAKE_CXX_FLAGS: -U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection
[unhardened] NEOGRAPH_ENABLE_HARDENING: OFF
[unhardened] compile: /usr/bin/c++ -DASIO_NO_DEPRECATED -DASIO_STANDALONE -DNEOGRAPH_BENCH_BUILD_TYPE=\"Release\" -DNEOGRAPH_STATIC_BUILD -I<work>/ng/include -I<work>/ng/deps/yyjson -I<work>/ng/deps/asio/include -I<work>/build-unhardened/generated -I<work>/build-unhardened/_deps/neograph_schemaprovider-src/src -I<work>/build-unhardened/_deps/neograph_schemaprovider-build/generated -U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection -O3 -DNDEBUG -std=gnu++20 -fPIE -o CMakeFiles/bench_neograph.dir/benchmarks/bench_neograph.cpp.o -c <work>/ng/benchmarks/bench_neograph.cpp
[unhardened] macros seen by the binary: _GLIBCXX_ASSERTIONS=False, _FORTIFY_SOURCE=0, stack protector=none, PIE=True
```
<!-- bench-record:end ceiling -->

## 内存

所有数值都是整个进程的，Python 的数值**包含解释器和已导入的框架**（NeoGraph 则包含 C++ 运行时和 SchemaProvider 运行时），
并不是已部署应用的内存。

* **空闲**指框架导入之后（NeoGraph 为引擎构建之后）、尚未运行任何任务时的进程。前两行是空基线：未导入任何内容的 Python 解释器，以及 NeoGraph 二进制文件。
* **峰值**是匹配单元计时循环结束时的 `VmHWM`；增量为峰值减去空闲。
* **每个在途运行的边际内存**把 N 个运行停在同一个节点内（`seq` 为中间节点；`par` 为五个 worker 写完结果之后的 summarizer），位于单个事件循环上
  （NeoGraph 为一个 `io_context` 线程），并读取此时进程的 `VmRSS` 和 `Pss`。数值是在完成一次运行并请求分配器归还空闲内存之后，N=64 与 N=1 之差除以 63。
  AutoGen 每个在途运行需要一个 team，并在测量窗口内构建；其他框架共用一个已编译的图或工作流实例。

<!-- bench-record:begin memory -->
| 框架 (HEAD) | 空闲：仅导入，未运行 (MiB) | `seq` 循环期间峰值 (MiB) | `par` 循环期间峰值 (MiB) | 相对空闲的增量，`seq` / `par` (MiB) |
| :--- | ---: | ---: | ---: | ---: |
| 仅 Python 解释器，未导入任何内容 | 10.9 | — | — | — |
| **NeoGraph**：已构建引擎的二进制文件，未运行 | 5.3 | 6.1 | 6.2 | 0.8 / 0.9 |
| Haystack | 63.5 | 70.2 | 70.2 | 6.7 / 6.7 |
| pydantic-graph | 25.1 | 26.2 | 26.2 | 1.1 / 1.2 |
| LangGraph | 62.8 | 63.0 | 63.1 | 0.2 / 0.3 |
| LlamaIndex Workflow | 35.5 | 36.7 | 36.7 | 1.1 / 1.2 |
| AutoGen GraphFlow | 42.2 | 42.7 | 42.8 | 0.5 / 0.5 |

**每个在途运行的边际内存**

| 框架 (HEAD) | `seq` 每次运行 KiB (RSS / PSS) | `par` 每次运行 KiB (RSS / PSS) |
| :--- | ---: | ---: |
| **NeoGraph** | 33.9 / 33.8 | 49.9 / 50.0 |
| Haystack | 14.2 / 14.2 | 27.3 / 27.4 |
| pydantic-graph | 24.3 / 24.4 | 57.8 / 57.7 |
| LangGraph | 37.1 / 37.3 | 93.8 / 93.8 |
| LlamaIndex Workflow | 58.5 / 58.3 | 119.5 / 119.5 |
| AutoGen GraphFlow | 96.4 / 96.3 | 174.1 / 174.1 |
<!-- bench-record:end memory -->

## 启动时间与体积

**冷启动**是从父进程创建子进程开始，到其第一次 `seq` 运行完成并通过检查为止的时间。父进程在创建子进程之前读取 `CLOCK_MONOTONIC`，
子进程在运行完成时打印 `CLOCK_MONOTONIC`。它涵盖加载器或解释器启动、导入（Python）或静态初始化（NeoGraph）、构建图以及一次运行，
此时页缓存已预热、字节码缓存已存在。每个框架使用二十个全新进程，在各框架之间交错运行；数值为中位数及 [最小, 最大]。

**体积。** 原生二进制文件在 `strip` 之后测量；`libneograph_core.a` 是按原样构建的目标文件归档；WebAssembly 模块取自 WebAssembly 工作流的体积关卡；
Python 环境是在包含该框架及其全部依赖的全新虚拟环境中，`site-packages` 的磁盘大小减去空虚拟环境，并附上软件包数量。
原生可执行文件、WebAssembly 模块和 Python 环境是**不同的部署单位**；表中把它们并列呈现，但不在它们之间形成比值。

<!-- bench-record:begin footprint -->
| 框架 (HEAD) | 从进程启动到第一个 `seq` 结果，ms | 导入 / 构建 / 首次运行（中位数 ms） |
| :--- | ---: | ---: |
| **NeoGraph** | **2.49 [2.40, 10.7]** | — / 0.55 / 0.09 |
| Haystack | 561 [550, 583] | 395 / 0.84 / 99.6 |
| pydantic-graph | 153 [149, 157] | 72.0 / 0.56 / 12.8 |
| LangGraph | 756 [735, 775] | 683 / 0.72 / 4.13 |
| LlamaIndex Workflow | 314 [309, 330] | 219 / 22.8 / 4.63 |
| AutoGen GraphFlow | 331 [326, 345] | 257 / 0.73 / 8.05 |

| 项目 | 部署单位 | 大小 | 统计内容 |
| :--- | :--- | ---: | :--- |
| NeoGraph `bench_footprint`（默认加固 Release） | 一个原生可执行文件 | 7.15 MB | 最小应用：注册一个节点，编译 `seq` 并运行一次；strip 之后 |
| NeoGraph `bench_neograph`（默认加固 Release） | 一个原生可执行文件 | 7.15 MB | 基准二进制文件；strip 之后 |
| NeoGraph `libneograph_core.a`（默认加固 Release） | 一个库文件 | 25.84 MB | 按原样构建的静态归档 |
| NeoGraph `bench_footprint`（MinSizeRel 加 LTO（`-Os`）） | 一个原生可执行文件 | 2.82 MB | 最小应用：注册一个节点，编译 `seq` 并运行一次；strip 之后 |
| NeoGraph `bench_footprint`（未加固 Release） | 一个原生可执行文件 | 6.49 MB | 最小应用：注册一个节点，编译 `seq` 并运行一次；strip 之后 |
| NeoGraph `bench_neograph`（未加固 Release） | 一个原生可执行文件 | 6.49 MB | 基准二进制文件；strip 之后 |
| NeoGraph `libneograph_core.a`（未加固 Release） | 一个库文件 | 23.86 MB | 按原样构建的静态归档 |
| NeoGraph `bench_footprint`（共享 `libneograph_core.so`） | 一个原生可执行文件 | 0.26 MB | 仅可执行文件；strip 之后 |
| NeoGraph 最小应用的共享库部署 | 可执行文件及其共享库 | 10.44 MB | 可执行文件加 7 个 NeoGraph 与 SDK 共享库；strip 之后 |
| NeoGraph 图冒烟程序 `smoke.wasm` (Emscripten 6.0.13, -O3 -flto, Release) | 一个 WebAssembly 模块 | 5.53 MB / 1.61 MB gzip | CI 中 WebAssembly 工作流的体积关卡，单线程任务，提交 db0fc651 |
| NeoGraph JavaScript 模块 `neograph_module.wasm` (Emscripten 6.0.13, -O3 -flto, Release) | 一个 WebAssembly 模块 | 6.29 MB / 1.84 MB gzip | CI 中 WebAssembly 工作流的体积关卡，单线程任务，提交 db0fc651 |
| Haystack（HEAD 安装） | Python 环境：框架及其全部依赖 | 140.75 MB | 42 个软件包；site-packages 减去空 venv |
| pydantic-graph（HEAD 安装） | Python 环境：框架及其全部依赖 | 12.03 MB | 9 个软件包；site-packages 减去空 venv |
| LangGraph（HEAD 安装） | Python 环境：框架及其全部依赖 | 65.46 MB | 38 个软件包；site-packages 减去空 venv |
| LlamaIndex Workflow（HEAD 安装） | Python 环境：框架及其全部依赖 | 216.19 MB | 62 个软件包；site-packages 减去空 venv |
| AutoGen GraphFlow（HEAD 安装） | Python 环境：框架及其全部依赖 | 36.93 MB | 11 个软件包；site-packages 减去空 venv |
<!-- bench-record:end footprint -->

## 探针

决定如何解读各行的问题，在同一会话中给出答案：（1）LlamaIndex 的工作流实例能否跨多次运行复用；（2）LangGraph 扇出开销中每次调用新建线程池占多少，
Haystack 的 worker 在哪些线程上运行；（3）pydantic-graph 是否只支持 async，其扇出是否真实；（4）加固给引擎带来多少开销；
（5）此前的两个测试工具为什么把同一次 pydantic-graph 调用测成了 227 µs 和 70 至 100 µs。

<!-- bench-record:begin probes -->
| # | 结果 |
| :--- | :--- |
| 1 | LlamaIndex：同一个 `Workflow` 实例可以重复运行，也可以并发运行 — 依次运行 [3, 13]，并发运行 [3, 103]，llama-index-workflows 2.26.0。 |
| 1 | LlamaIndex 实测：`seq` 复用实例 2,272 µs，每次运行新建实例 2,615 µs；`par` 为 7,069 µs 与 7,536 µs。 |
| 2 | LangGraph：`invoke` 在 1 个线程池线程上运行五个工作节点，不使用调用线程；使用 `async def` 节点的 `ainvoke` 在调用线程上运行。实测：`par` 的 `invoke` 为 3,184 µs，`ainvoke` 为 1,777 µs（1.79 倍）；`seq` 为 1,063 µs 对 1,248 µs（0.85 倍）。 |
| 2 | Haystack：使用同步组件时，`run_async` 把工作节点交给调用线程之外的 3 个线程；使用 `run_async` 组件时，与 `run` 一样在调用线程上运行。 |
| 3 | pydantic-graph 2.59.1.dev4+e0ccafa8a：`Graph.run` 是协程，`run_sync` 只是在事件循环上驱动它；普通步骤函数会被拒绝（TypeError: object int can't be used in 'await' expression）；五个各等待 50 ms 的步骤在 1 个线程上于 52 ms 内完成，因此扇出是真实的，且为单线程。 |
| 4 | `bench_neograph` 的加固差异：默认加固构建相对未加固构建，`seq` 0.98 倍，`worker_count=1` 的 `par` 1.04 倍，工作线程池的 `par` 1.02 倍。 |
| 5 | pydantic-graph 1.x，旧版 `Graph(nodes=[...])` API，在本主机上连续测量：await 循环 79.3 µs/次，同样的运行以 gather 任务方式 87.2 µs，单个任务内的循环 79.2 µs，关闭循环垃圾回收 79.2 µs。测试工具的形态无法解释此前的 227 µs 对 70 至 100 µs：这两个数值来自不同的会话与环境。 |
| 5 | 同样的旧版 pydantic-graph 循环，在环境中安装可选包 `logfire` 后为每次运行 498 µs，是干净环境（79.3 µs）的 6.3 倍。没有该包时埋点 span 是空操作，有该包时是真实的 span，因此包含它的共享环境会改变数值。 |
<!-- bench-record:end probes -->

## 这些数字说明什么

1. 比值只适用于其所属的运行方式配对、工作负载、主机以及同批测量表中的版本。NeoGraph 的 `par` 在两种 worker 模式之间相差线程池表下方所述的倍数，
   因此没有一个单一的“快 N 倍”可以描述这个引擎。
2. 各行是框架与工作负载的测量值，并非对其开销成因的单独解释。其中包含各框架默认在每次运行中做的事情（回调、span、状态快照、消息线程）。
3. worker 为 1 的派发与引擎自有线程池是不同的执行方式。请针对预期的工作负载两者都测一遍；没有实际工作的扇出基准能暴露协调开销，
   但不能预测模型或 I/O 延迟。
4. 内存数值包含解释器和已导入的框架。对已部署的 Python 服务而言，引擎的占用是相对空闲的增量和每次运行的边际数值，而不是整个进程的峰值。

## 注意事项：这个基准没有测量的内容

* **真实的智能体工作负载。** 框架对比中没有模型推理、网络请求或工具 I/O。这些开销可能主导一个应用，因此这些表格不能证明端到端的智能体加速。
* **适合各框架的工作负载。** AutoGen、LlamaIndex 和 pydantic-graph 各自针对的范式（多智能体对话、事件驱动的长时间工作流、状态机控制流）并未被本基准覆盖。
* **检查点吞吐量。** 在各框架上启用持久化会让序列化开销占主导；那是另一个基准。
* **一台主机、一个会话。** 同批测量运行在一台 4 核 Arm 机器上。比值会随平台和框架版本变化，HEAD 与发布版两行显示了仅凭版本不同就会变动多少。
* **公平性。** NeoGraph 是带加固标志的默认 Release 构建（见上文打印）；未加固的一行是上限。每个 Python 框架都在各自的虚拟环境中使用原样的 CPython 3.12，
  依赖版本记录在记录文件中，没有任何自定义调优，这样为某个框架安装的软件包就不会改变另一个框架的开销。

## 复现

当前的源码构建需要外部的 `SchemaProvider::runtime` 软件包，仅构建 Core 的基准也不例外。CMake 3.20+ 依次选择显式指定的
`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、已安装的运行时、固定版本的公开 GitHub 源码归档。`NEOGRAPH_FETCH_SCHEMAPROVIDER` 默认为 ON；
使用已安装的软件包或显式指定的源码进行离线构建时，请将其设为 OFF。显式指定的检出或已安装的软件包必须提供接口/共享代数 7。
请把新的输出另行保存；`records/` 下的记录写入之后不再修改。

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

框架对比需要为每个框架、每个来源各建一个虚拟环境，这样为某个框架安装的东西就不会改变另一个框架的开销。版本固定在
[`requirements-compare.txt`](requirements-compare.txt)（最新发布版）和 [`requirements-compare-head.txt`](requirements-compare-head.txt)（git 提交）中。
`compare/driver.py` 以交错轮次运行整个会话，每个单元使用一个全新进程，并写出记录；只要有任何错误单元，退出状态就不为 0。

```bash
# One virtual environment per framework and per source, from the pinned requirement files
# (creates $WORK/venvs/<framework>-release and $WORK/venvs/<framework>-head):
benchmarks/compare/setup_venvs.sh "$WORK"

# NeoGraph: the default hardened Release build and the unhardened engine ceiling.
cmake -S . -B "$WORK/build-hardened" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$WORK/build-hardened" --target bench_neograph bench_footprint
cmake -S . -B "$WORK/build-unhardened" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_ENABLE_HARDENING=OFF \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "-DCMAKE_CXX_FLAGS=-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection" \
    "-DCMAKE_C_FLAGS=-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection"
cmake --build "$WORK/build-unhardened" --target bench_neograph bench_footprint

# One cell by hand; the last line of standard output is the record:
"$WORK/venvs/langgraph-head/bin/python" benchmarks/compare/cell.py --framework langgraph \
    --variant ainvoke_async --workload seq --iters 600 --samples 3
"$WORK/build-hardened/bench_neograph" 80000 36000 1 10 5 --rows=seq --record

# The whole session on a quiet machine (it waits for a load average under 0.3), then the tables and the chart:
python3 benchmarks/compare/driver.py run --work "$WORK" --out record.json \
    --harness-commit "$(git rev-parse HEAD)" --engine-commit "$(git rev-parse HEAD)" \
    --harness-dirty "$([ -n "$(git status --porcelain)" ] && echo true || echo false)"
python3 scripts/render_bench_report.py --record record.json
```

依赖外部服务的检查点基准、HTTP 基准和并发 Docker 基准是各自独立的实验；
上面确定性的原生 Core/Program 运行并不需要它们。

输出格式是以制表符分隔的 `config`、`runtime`、`header`、`result` 或 `metric` 记录。原生结果行包含总时间的中位数和每次迭代的时间。
加上 `--record` 时，`bench_neograph` 和 `bench_footprint` 还会打印一行 `record<TAB>{json}`，`benchmarks/compare/cell.py` 的每个单元也会把这样一行作为最后一行打印：
其中有原始样本、完成数与失败数、结果检查和进程内存。答案错误、运行失败或没有完成任何运行的单元，其 `status` 为 `error`，没有计时，且退出状态非 0。

## 此前的结果

2026-04（x86_64）的表格和 2026-07 的 Arm64 基线，连同其版本、日期以及对现已确认无效之处的说明，已移到
[历史记录](history/engine-overhead-2026-04.md)。2026-04-19 的测量环境块也在那里。下面带类型 provider 的各节只测量了 NeoGraph，没有改动。

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
