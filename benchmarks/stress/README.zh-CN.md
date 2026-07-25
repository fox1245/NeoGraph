<!-- neograph-i18n: source=benchmarks/stress/README.md locale=zh-CN source_sha256=87cc091ee71ab14f86ff9642a147a3d80e1452c56020c0073aba63e125258190 -->
# NeoGraph 压力测试工具集

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

补充 engine-overhead 和 single-shot concurrent benchmarks 的运行就绪度 gates。`benchmarks/bench_neograph` 测量 **per-call cost**，`benchmarks/concurrent/...` 测量 **single 10k burst**，而本目录让 NeoGraph **随时间** 运行。

## 内容说明

### `bench_sustained_concurrent`

在 M wall-clock seconds 内保持 N 个 graph runs in flight。一个 run 完成就立即提交新的 run，使 inflight 保持目标值。每 `--sample-s` 秒采样 RSS 和 per-window latency；如果 RSS 在 warm baseline（warmup 后）和 final sample 之间上漂超过 `--rss-tolerance-pct`，则退出 1。

捕捉 burst bench 无法捕捉的三种 failure modes：

- **Steady-state leaks** — coroutines / pending writes / cached state 无界增长。drift gate 是 best-effort（Valgrind / LSan 仍是权威工具，见 ASan / TSan CI），但 60 s 内 RSS 爬升 25% 是很强的"看这里"信号。
- **Latency drift** — pool 预热后 mean / max-per-window 上行。通常指向 thread-pool starvation 或 scheduler back-pressure，而这些不会出现在 t=0 burst tests 中。
- **Pool exhaustion under churn** — completions 与新 submissions 重叠，因此 worker pool 看到的是混合 inflight pattern，而不是 burst 的 all-drain。

#### 用法

```bash
cmake -B build-stress -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_TESTS=OFF \
    -DNEOGRAPH_BUILD_EXAMPLES=OFF
cmake --build build-stress -j$(nproc) --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
    --concurrency        1000 \
    --duration-s         60   \
    --sample-s           5    \
    --warmup-s           5    \
    --rss-tolerance-pct  25
```

冒烟结果（concurrency=100，duration=15s，Ryzen 7 5800X）：
- 持续运行 15 s 完成 15.3 M graph runs，约 **1.0 M runs/s**
- 每次运行的平均延迟：~55 µs
- 预热 RSS：9.3 MB → final：7.4 MB（漂移 ‑20 %，退出码 0）

#### 输出格式

每个 sample 一行 JSON，最后一行 summary：

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

### `bench_sustained_concurrent` 在 `prlimit` 下（内存上限测试）

用 virtual-memory cap 包住 harness，以证明 NeoGraph 能干净地处理 allocation pressure：

```bash
# Cap address space at 256 MB. Allocations beyond this fail with
# std::bad_alloc — NeoGraph's audit-Round-5 typed catch in
# graph_executor (commit ead703e) rethrows bad_alloc instead of
# silently retrying, so the workload should error out instead of
# crashing.
prlimit --as=$((256*1024*1024)) \
    ./build-stress/bench_sustained_concurrent \
        --concurrency 200 --duration-s 30
```

通过标准：进程干净退出（return code 0 或 1；不是 SIGABRT / SIGSEGV），`err_total` 可以非零（这些是 bad_alloc rethrows 作为 failed runs 暴露出来）。

## 尚未实现

- **24-hour soak** — 同一个 harness，更长 wall window。在 dedicated host 上运行；观察 `peak_rss_kb` 是否在数小时内呈 monotone-non-decreasing trend。
- **cgroup-bounded run** — `systemd-run --scope -p MemoryMax=512M`，比 `prlimit` 更严格的 resource cap（kernel-side enforcement，不只是 allocation-time check）。WSL2 systemd support 有限；请在真实 Linux host 上测试。
