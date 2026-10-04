<!-- neograph-i18n: source=benchmarks/stress/README.md locale=zh-CN source_sha256=245bd9f1555c8f45feba5119b20683c54700e0b74468f8857b4707267680b859 -->
# NeoGraph 持续并发压力基准

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本运行器在一段时间内重复执行三节点计数器图，测量本地引擎持续运行，而非供应商调用、持久化或生产就绪度。

## 测量与退出状态

`bench_sustained_concurrent` 默认值为 `--concurrency 1000`、`--duration-s 60`、`--sample-s 5`、`--warmup-s 5`、`--rss-tolerance-pct 25`。调用方线程数等于目标运行数，完成时提交替代任务。样本报告平均与最大延迟，不报告 P99；计时从工作线程内部开始，不含队列等待。`ok_total` 计数的是未抛出异常的调用，而不是对返回图状态的验证。

退出 1 表示最终当前 RSS 相比预热基线增长超出容差；退出 0 不证明无泄漏或无运行错误。请单独查看 `err_total`。最终 RSS 在停止并 join 线程池后读取，受线程退出影响。基线仅在首个样本达到 `warmup-s` 时记录，请使预热时间不超过首个采样间隔。没有基线时零漂移不能作为内存门槛合格的证据。

Windows 使用 working-set 计数器，Linux 使用 `/proc/self/status`。其他平台返回 0 可能表示无法测量。峰值 RSS 不会下降，调查增长时应查看当前 RSS 和基线有效性。

## 构建与运行

安装外部 SchemaProvider SDK 并设置 prefix。关闭 LLM 和 NeoGraph 可选 libcurl 后端后，Core 仍需要 `SchemaProvider::runtime`。可用 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 显式指定源码代替 prefix；源码构建需要 C++20、Python、libcurl ≥7.88 和 OpenSSL Crypto。依赖与平台限制参见[构建指南](../../README.md)。以下命令禁用网络获取和未使用的 NeoGraph 集成。
NeoGraph `0.13.0` 需要 alpha SDK `0.1.0`、interface revision/shared generation 4，并以匹配 header/library 重建。当前集成验证尚未完成。

```bash
# Set SCHEMAPROVIDER_PREFIX to the installed SDK prefix.
cmake -B build-stress -S . \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_PROGRAM=OFF -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-stress --parallel --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
  --concurrency 1000 --duration-s 60 --sample-s 5 \
  --warmup-s 5 --rss-tolerance-pct 25
```

## 保留的历史观测

旧 README 中未注明日期的 Ryzen 7 5800X 记录为：并发 100，15 秒内 15.3 M 次运行（约 1.0 M runs/s），平均延迟约 55 µs，预热 RSS 9.3 MB 到最终 7.4 MB（约 −20%，退出 0）。未记录日期与 SDK 修订。保留为历史证据，不是新 cutover 验证或吞吐保证。以下为该观测的输出摘录，省略号不是 JSON。

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

## 分配压力实验

`prlimit` 限制 Linux 虚拟地址空间。每个调用槽有一个线程，栈和线程池创建可能在图执行前耗尽上限。运行内的 catch 记录 `engine->run` 异常，但线程池创建在 catch 外。分配压力下正常退出是需测量的验收标准，不是脚本保证。

```bash
# Linux: cap virtual address space, not resident memory.
prlimit --as=$((256*1024*1024)) \
  ./build-stress/bench_sustained_concurrent \
  --concurrency 200 --duration-s 30
```

## 额外实验

24 小时运行或 cgroup 内存限制需单独记录环境与结果。比较稳态区间的当前 RSS，并记录 `err_total` 和终止信号。区分 cgroup 常驻内存限制与 `prlimit` 地址空间限制。本页未报告这些运行结果。
