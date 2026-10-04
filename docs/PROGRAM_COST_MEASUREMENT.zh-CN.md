<!-- neograph-i18n: source=docs/PROGRAM_COST_MEASUREMENT.md locale=zh-CN source_sha256=f22d74f413d9d1665f1444d3a29a05212e4d0ff9c515be846e5439fabe2177e1 -->
# 测量 Program 成本

**Languages:** [English](PROGRAM_COST_MEASUREMENT.md) | [한국어](PROGRAM_COST_MEASUREMENT.ko.md) | [日本語](PROGRAM_COST_MEASUREMENT.ja.md) | [简体中文](PROGRAM_COST_MEASUREMENT.zh-CN.md)

`bench_program_cost` 和 `scripts/run_program_costs.py` 提供诊断 baseline，帮助决定优化哪些 Program 成本。它们不改变 runtime 语义，不替代预先登记的 QuickJS performance gate。无需 LLM、network model call 或 model credential。

实现比较时，`scripts/compare_program_costs.py` 在新进程对中交替运行两个冻结的 `bench_program_cost` 二进制。首次比较记录于 [命令发布 head 优化](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)。后续 [规范 JSON 优化](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md) 通过 AgentX 反汇编及 CPU sampling 选择原生字符串处理变更。之后的 [SHA-256 优化](PROGRAM_SHA256_OPTIMIZATION.md) 加入运行时条件启用的 CPU acceleration，以及独立验证的 portable fallback。`bench_canonical_json CASE BYTES ITERATIONS` 单独将 ASCII、Unicode、混合及大量转义字符串与标量输出 oracle 比较。

## 构建与运行

在原生 filesystem 上使用优化构建。例如 Linux：

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

runner 要求**新**输出目录。默认矩阵使用 memory 和 SQLite，七次独立进程重复，十次 warmup invocations，每 lifecycle case 测量 100 次 invocations。`--iterations` 和 `--warmup` 控制 lifecycle/direct cases；generator microbenchmarks 使用 1,000 commands。案例串行执行，重复轮次交替正向/反向顺序。SQLite 文件位于输出目录并保留以便检查。

包括 PostgreSQL 时，先启动专用本地 test container，将 `NEOGRAPH_COST_POSTGRES_URL` 设为其发布的 localhost connection URL，加入 `--postgres-container <container-name>`。runner 检查 published port，为每个 sample 创建唯一命名 database，之后只删除该 database。它不启动或停止 containers，也不使用或删除 URL 中命名的 database。一次性 server 必须允许 `postgres` 经 `docker exec` 创建/删除 databases。不要指向 application server。凭据不写入 metadata 或 command records。

## 每个范围包含什么

| 案例 | 包含 | 排除 |
| --- | --- | --- |
| `direct` | Warm `GraphEngine::run`、3 个顺序 increment nodes、2 个 state channels | Program admission、journal、checkpoint persistence、JS |
| `lifecycle --mode cpp` | 已准入 C++ builder v1 Program、真实 Core、结果构造、journal 和 checkpoints | JavaScript control execution；compilation/admission 是独立 cold fields |
| `lifecycle --mode javascript` | 已准入 `define()` + generator `main()`、真实 Core、JS/C++ conversion、command journal 和 checkpoints | Compilation/admission 是独立 cold fields |
| `generator` | Generator open/compile/init、首次 `next`、重复 host-command round trips、terminal conversion、teardown | 真实 Core、scheduling、journal、database I/O；responses 是合成值 |
| `resident` | 首个 command 后暂停的多个独立 QuickJS runtimes | 完整 Program agents、catalogs、engines 和 journal storage |
| 现有 primitive/control cases | `bench_quickjs_primitives` 和 `bench_quickjs_control` 已记录范围 | 这些 samples 不是 enabled/disabled gate run |

直接、C++ 和 JavaScript runs 使用相同三节点 topology、increment behavior 和 payload channel。每个真实 run 必须以 counter `3` 和完整未改变 payload 结束。payload sizes 为 0、4,096、65,536 bytes。JavaScript multi-command 行执行同一 Core graph 4 或 16 次，每次将 counter input 重置为零，最终 counter 为 `3`。

C++ v1 准入恰好一个 Program operation。JavaScript generator 使用宿主声明的 64-operation ceiling，以支持所有 command-count 行。两者其余 resource ceilings 相同，均使用一个 scheduler thread。源形式及 command-journaling 语义不同：JavaScript 减去 C++ **不能**隔离 interpreter time。

每个 lifecycle case 从选定 backend 构造 ProgramStore、CheckpointStore 和 ProgramTransitionStore。memory mode 保留真实内存 journal/checkpoint values，不绕过 persistence interfaces。SQLite 使用一个 database file 和独立 store connections。现有 CheckpointStore 设置 WAL 和 `synchronous=NORMAL`；Program store connections 保留 SQLite synchronous default。PostgreSQL 使用现有 store implementations 和四连接 checkpoint pool。比较结果时记录实际 server settings。这些是 backend defaults，不表示 power-loss guarantees 相同。

## 理解 timing 与 memory 字段

`samples` 含每个测量 invocation；`first_run` 在 warmup 完成前单独记录。`cold` 含 store construction、Program compilation、admission 和 Runtime construction。这些字段排除部分 fixture setup，因此不是完整 process-startup 分区。

单次顺序 invocation 中，`before_core_us`、`core_span_us`、`after_core_us` 和 `wake_us` 用 Program event timestamps 划分 start-to-wait-return wall time。**Core event span 不是纯 Core CPU time：**它包括 checkpoint 工作，以及 multi-command 行中的 Core calls 间隔。`start_us` 和 `post_start_us` 提供第二分区。绝不可将两个分区相加；Core 工作可能在 `start()` 返回前开始。

event sink 记录 atomic timestamps。因此 lifecycle results 包含该 observer，而 direct/generator cases 无 event sink。这是诊断计时，不声称无 observer 的最小延迟。

runner 先汇总每个进程，再汇总独立进程结果。`invocation_median.total_us.median` 是 process medians 的中位数。`invocation_p95.total_us.median` 是进程内 p95 的中位数。percentiles 用 nearest rank；七个进程样本时 process-level p95 是最大值。保留 raw samples、MAD 和 extrema。不要从中位数的小差异推断统计显著性。

Linux RSS 是进程内存，不是 JS allocation accounting。lifecycle RSS growth 包含保留 run history 和 allocator behavior。resident-generator RSS 包含 runtime/bootstrap overhead 和 allocator granularity；用较大 generator counts 间斜率估计增量占用。两者都不是每个 recursive agent 总内存测量。其他平台 zero memory fields 表示 Linux-only probe 不可用。

runner 记录 binary/source hashes、source commit、tracked diff hash、CMake options、CPU 和 process affinity。原始证据应与报告一起保留，测量期间不要重新构建二进制。

## Linux 可选数据库 API 诊断

`benchmarks/program_store_profile.c` 可计数并计时 database-library calls，不改变 Program runtime。在主未插桩矩阵完成**之后**单独构建：

```sh
cc -O2 -shared -fPIC -I/usr/include/postgresql \
  benchmarks/program_store_profile.c -o /tmp/program_store_profile.so -ldl -pthread
NEOGRAPH_COST_STORE_PROFILE=/tmp/new-store-profile.json \
LD_PRELOAD=/tmp/program_store_profile.so \
  build-cost/bench_program_cost --case lifecycle --backend sqlite \
  --storage /tmp/new-cost-profile.sqlite --iterations 40 --warmup 0
```

profiler 只记录 categories 和 aggregate counts/time，绝不记录 SQL text、parameters 或 credentials。它测量 `sqlite3_step`，或同步 `PQexec` 和 `PQexecParams`，以及 `sqlite3_exec`（包含 transaction control）。`exec` 内 nested SQLite steps 排除，以免重复计数。SQLite row iteration 可为一个 query 多次调用 `step`。async libpq calls、connection establishment、SQLite prepare time 和调用方 serialization 在这些 API intervals 外。whole-process totals 包含 constructors、schema creation、admission 和 warmup。profiling 增加 overhead，并发 API intervals 可能重叠，因此不能将这些值从 baseline wall time 中减去作为精确 CPU 分区。比较不同 invocation counts 的 fresh-database runs，以分离固定 setup call counts 和逐 run calls。

## 此 baseline 的边界

矩阵测量顺序合成执行及暂停 generator 占用。不验证 tenant fairness、concurrent throughput、recursive spawn、live replacement、process-loss recovery、production storage 或 JIT backend。generator replay 行重放已记录合成 command responses，不计时完整持久重连。判断 JIT 潜力需要真实比较，包含 warmup、compilation 和 memory costs。
