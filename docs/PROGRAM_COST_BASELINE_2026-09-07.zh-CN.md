<!-- neograph-i18n: source=docs/PROGRAM_COST_BASELINE_2026-09-07.md locale=zh-CN source_sha256=a0929a5678214c2e42de563f8e5eb1d0137f06092e7ca01d27aa3a727b351339 -->
# Program 成本 baseline — 2026-09-07

**Languages:** [English](PROGRAM_COST_BASELINE_2026-09-07.md) | [한국어](PROGRAM_COST_BASELINE_2026-09-07.ko.md) | [日本語](PROGRAM_COST_BASELINE_2026-09-07.ja.md) | [简体中文](PROGRAM_COST_BASELINE_2026-09-07.zh-CN.md)

小 payload baseline 指向 Program 记账及持久存储工作，作为首批优化目标。它没有确立 JavaScript 解释是主要成本，也不测量 JIT 加速。

## 环境与方法

- 源基础：`319f8748c986a2f345ae6de88abb42d89eef9837`，加上 `metadata.json` 的 source/binary hashes 所记录的仅用于测量的增补。
- AMD Ryzen 7 5800X、WSL2 Ubuntu 24.04，可用 16 logical CPUs；GCC 13.3、Release `-O3 -DNDEBUG`，启用 repository hardening。
- 二进制和 SQLite databases 位于 WSL ext4。PostgreSQL 16.15 在原生 WSL Docker 中运行，使用本地 Docker volume，经 localhost TCP 连接。`fsync`、`synchronous_commit` 和 `full_page_writes` 开启；WAL sync method 为 `fdatasync`。
- 每案例七个新进程，每个 lifecycle/direct 进程十次 warmup、四十次测量调用。一个 Runtime scheduler thread；无并发测试任务或 model calls。桌面/VM 不是隔离的裸机 benchmark host。
- 308 个成功进程样本，44 个案例。每次真实调用检查 counter 和 payload 输出。案例顺序按重复轮次交替正向/反向。
- 主要数值是七个进程中位数的中位数。下列 p95 是七个进程内部 nearest-rank p95 的中位数，不是统计独立的合并 p95。
- 原始 metadata、cases、samples 和 summaries 本地保留在 `artifacts/program-costs-20260907/`。SQLite databases 留在 WSL 的 `/tmp/neograph-program-costs-20260907/` 下；数据库文件不提交。

见 [测量定义与复现](PROGRAM_COST_MEASUREMENT.md)。此诊断矩阵不替代现有 QuickJS acceptance gate。

## 一次 Core 调用，空 payload

| 路径 | 中位数 (ms) | 进程内 p95 (ms) | 跨进程中位数的 MAD (ms) |
| --- | --- | --- | --- |
| Core directly, no checkpoints | 0.0074 | 0.0076 | 0.0000 |
| C++ Program / memory | 2.0031 | 2.2607 | 0.0355 |
| JavaScript Program / memory | 3.3192 | 3.4898 | 0.0196 |
| C++ Program / SQLite | 8.5885 | 13.5360 | 0.1155 |
| JavaScript Program / SQLite | 16.4288 | 20.9731 | 0.0253 |
| C++ Program / PostgreSQL | 49.0355 | 51.2952 | 0.5896 |
| JavaScript Program / PostgreSQL | 80.1248 | 82.8138 | 0.5005 |

直接 Core 行排除 Program lifecycle 和 checkpoint 语义。它与 Program 的比值是总包络比较，不是相同持久保证的 engine 比较，也不证明回归。C++ Program 也有真实 journal/checkpoint 工作。JS–C++ 差异包含额外命令 journaling 和 conversion，不只是 interpreter。

## Payload 扩展

| 路径 | 0 字节 (ms) | 4 KiB (ms) | 64 KiB (ms) |
| --- | --- | --- | --- |
| Core directly | 0.0074 | 0.0079 | 0.1308 |
| C++ Program / memory | 2.0031 | 2.8567 | 12.2975 |
| JavaScript Program / memory | 3.3192 | 5.2922 | 30.8116 |
| JavaScript Program / SQLite | 16.4288 | 23.9060 | 130.6772 |
| JavaScript Program / PostgreSQL | 80.1248 | 93.5064 | 252.1259 |

payload 经真实 channel state 传递，并检查未改变。它不是 model output 或模拟 network delay。

## Generator 与桥接成本

| Payload | 打开 (µs) | 首个命令 (µs) | Warm 命令往返 (µs) | 终止 next (µs) | 关闭 (µs) |
| --- | --- | --- | --- | --- | --- |
| 0 | 330.34 | 45.65 | 26.66 | 5.73 | 35.85 |
| 4096 | 349.72 | 171.95 | 153.32 | 30.99 | 39.99 |
| 65536 | 811.60 | 2038.30 | 2088.19 | 423.18 | 41.01 |

这些隔离调用使用合成 Core responses。warm round-trip time 包含原生 JSON serialization/parsing、宿主 command creation 和 JS execution。它不是纯 bytecode execution time，不能直接从 lifecycle 行减去。大 payload 暴露了实质数据转换成本。

## 冷准备

| 模式/后端 | Store 打开 (ms) | 编译 (ms) | 准入 (ms) | Runtime 创建 (ms) | 首次运行 (ms) |
| --- | --- | --- | --- | --- | --- |
| memory-cpp-0 | 0.0054 | 0.6768 | 1.0492 | 0.1405 | 4.4991 |
| memory-javascript-0 | 0.0054 | 1.2498 | 1.2180 | 0.1420 | 5.9273 |
| sqlite-javascript-0 | 20.0656 | 1.3064 | 2.9385 | 0.1705 | 25.2293 |
| postgres-javascript-0 | 169.2527 | 1.2941 | 5.9809 | 0.1714 | 98.6726 |

compilation/admission 每进程一次，在 warm invocation 计时之外。每个新启动 JS Program 会再次 generator open。这些 cold fields 省略部分 fixture setup，不计时 live replacement 或 migration。

## Event-marker 分区

仅此表每格是所有测量调用的平均值，因此四个 marker intervals 相加得到平均总时间（除舍入误差）。首次与最后 Core event 之间包含 checkpoint 工作，不是纯 Core CPU time。

| 后端 (JavaScript，0 字节) | 首次 Core event 前 (ms) | Core event 跨度 (ms) | 最后 Core event 后 (ms) | 终止到 wait 返回 (ms) |
| --- | --- | --- | --- | --- |
| memory | 1.1931 | 0.4062 | 1.7080 | 0.0340 |
| sqlite | 7.0816 | 1.2735 | 8.7646 | 0.0353 |
| postgres | 29.8013 | 9.1486 | 41.4887 | 0.0357 |

## 单个 generator 中多次 Core 调用

| 后端 | 每 run 1 次调用 (ms) | 每 run 4 次调用 (ms) | 每 run 16 次调用 (ms) |
| --- | --- | --- | --- |
| memory | 3.319 | 9.041 | 31.596 |
| sqlite | 16.429 | 55.527 | 331.089 |
| postgres | 80.125 | 244.105 | 1034.104 |

这些是具有真实 journals 和 checkpoints 的顺序调用。除以调用数可摊薄 run startup/termination，但不能隔离 dispatch cost。

## 暂停 generator 的内存与 replay

| 暂停 generator 数 | RSS 增量中位数 (MiB) |
| --- | --- |
| 1 | 1.285 |
| 32 | 6.309 |
| 128 | 21.934 |

| 重放的合成已记录命令数 | 全新打开 + replay (ms) |
| --- | --- |
| 0 | 0.1276 |
| 10 | 0.5526 |
| 100 | 3.4148 |
| 1000 | 33.4788 |

上述内存对应独立暂停的 QuickJS runtimes，包括 bootstrap/allocator 效应，不包含完整 agents、catalogs、Core engines 和持久 histories。replay 排除 ProgramRuntime scheduling 和 journal I/O，不是进程丢失恢复延迟。

## 补充数据库 API profile

baseline 之后，72 次额外运行比较 profiling 开/关、一次与 21 次调用、三次重复，以及所有 backends 的两种 control modes。表格使用 `(21-run process total − 1-run process total) / 20`，再取重复中位数。三次重复的 call-count differences 完全相同；timing differences 仍是受 cold-process variation 影响的估计。

| 路径 | 每 run SQLite step 调用数 | 每 run SQLite exec 调用数 | 每 run SQLite 提交数 | 每 run 同步 libpq 调用数 |
| --- | --- | --- | --- | --- |
| memory-cpp | 0 | 0 | 0 | 0 |
| memory-javascript | 0 | 0 | 0 | 0 |
| sqlite-cpp | 73 | 14 | 7 | 0 |
| sqlite-javascript | 117 | 18 | 9 | 0 |
| postgres-cpp | 0 | 0 | 0 | 64 |
| postgres-javascript | 0 | 0 | 0 | 108 |

SQLite steps 可因 rows 重复，不是独立 SQL statements。`exec` 包含 transaction control；排除 nested steps。libpq counts/times 只覆盖同步 `PQexec`/`PQexecParams`，不含 async checkpoint 路径和 connection establishment。client CPU 排除 PostgreSQL server。API wall intervals 和 client CPU 重叠，不得相加或视为 baseline latency 的完整分区。

内存 controls 记录零 database calls。JS SQLite 案例每次 invocation 有九次 commits 和 117 次外部 step calls；这些计时数据库调用外的 CPU 工作仍值得 profiling。JS PostgreSQL 案例每次 invocation 增加 108 次同步 calls，因此 round-trip count 是 JIT 实验前的具体目标。

补充证据在 `artifacts/program-costs-20260907/profile/`。首个 profiler 遗漏 SQLite `exec` transaction calls，未使用其初步输出。停止文件复制后重新运行纠正后的 profile。然而，之后的 profiled **及 unprofiled** runs 都明显慢于主 baseline，包括 memory controls。没有隔离出环境漂移原因。因此这里只确认稳定 call counts；保留 raw API 和 CPU times，但**不用于归因主 baseline latency**。主 baseline 数据早于复制问题，保留全部七次重复及其离散程度。

现有 `bench_program` 在三个新进程中也给出 C++ Program 路径 2.21–2.31 ms（Core：6.41–6.65 µs）。该 fixture 有一个 channel 和不同 bounds，因此只是毫秒包络的交叉检查，不是主矩阵的匹配替代。

## 后续测量与优化顺序

1. Profile Program startup/termination 和 publication serialization。内存存储及 C++ control 的小 payload 成本仍实质存在，因此只替换 JS engine 无法处理完整包络。
2. 用测得 SQLite/libpq call counts 调查冗余 reads 和 round trips，同时保留相同 journal、owner、authority、budget 和 atomicity 保证。不要靠禁用 durable commits 获得更好数字。
3. 用 payload sweep 调查重复 canonical JSON conversion 和 copying。compiled-source reuse 应与 warm generator execution 分开 benchmark。
4. 隔离真正 JS CPU 工作后才比较 JIT backend，包括真实 agent 数量下 cold-start cost 和 memory。

完整 CPU attribution、并发 multi-tenant throughput/fairness、recursive spawn、live replacement 和真实 JIT comparison 仍未测量。此 baseline 未作生产 runtime 优化。
