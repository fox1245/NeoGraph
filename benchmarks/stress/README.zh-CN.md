<!-- neograph-i18n: source=benchmarks/stress/README.md locale=zh-CN source_sha256=a324da3b2f664f2e1502ab97372cf855ccd13737463d01df643826f0c05be057 -->
# NeoGraph 持续并发压力基准测试

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这个 runner 在一个挂钟时间窗口内，反复运行一张三节点的计数器图。它测量的是本地引擎的周转开销，不涉及 provider 调用、持久化，也不代表已经具备生产就绪的条件。

## 测量与退出状态

`bench_sustained_concurrent` 的默认参数是 `--concurrency 1000`、`--duration-s 60`、`--sample-s 5`、`--warmup-s 5` 和 `--rss-tolerance-pct 25`。它创建一个调用方线程池，每个目标运行占用一个线程；有请求完成时，就补入新的请求。采样报告的是平均延迟和最大延迟，不含 P99。计时从调用方 worker 内部开始，不包含它在队列中的等待时间。`ok_total` 统计的是没有抛出异常就返回的调用数，并不校验返回的图状态。

退出码 1 表示最终的当前 RSS 超出已记录的预热基线并超过容差；退出码 0 并不能证明没有泄漏，也不能证明没有运行错误。`err_total` 需要单独查看。最终的 RSS 是在停止并 join 线程池之后读取的，所以线程销毁会影响漂移值。只有在第一次采样时已经达到 `warmup-s`，才会记录预热基线；预热时长不要超过第一个采样间隔。缺少基线时漂移为零，不能用来通过内存检查的闸门。

Windows 使用进程工作集的计数器；Linux 使用 `/proc/self/status`。其他平台因为没有这些计数器，可能报告为零。峰值 RSS 是历史最高水位，只增不减；排查增长问题时，要看当前的 RSS 以及基线是否有效。

## 构建与运行

先安装外部的 SchemaProvider SDK，并设置它的前缀。即使禁用了 LLM 和 NeoGraph 可选的 libcurl 后端，Core 仍然需要 `SchemaProvider::runtime`。也可以用 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 指定显式的 SDK 源码检出来代替前缀；构建它需要 C++20、Python、libcurl ≥7.88 和 OpenSSL Crypto。依赖和平台方面的限制，见[构建指南](../../README.md)。这套做法关闭了联网下载，也关闭了用不到的 NeoGraph 集成。
对于 NeoGraph `0.14.1`，请使用 alpha 版 SDK `0.3.2`、接口修订版/共享库
代数 6，并用匹配的头文件和库重新构建。

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

## 保留的旧观测结果

早先有一次没有标明日期的 Ryzen 7 5800X 运行：并发 100、持续 15 s，共运行 15.3 M 次（约 1.0 M 次/秒），平均延迟约 55 µs，预热后 RSS 为 9.3 MB，最终 RSS 为 7.4 MB（约 −20%，退出码 0）。它的日期和 SDK 修订版本没有记录。保留它只是作为一次旧的观测，不是吞吐量保证。下面的输出节选属于那次观测；其中的省略号不是 JSON 的一部分。

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

## 内存分配压力实验

`prlimit` 限制的是 Linux 的虚拟地址空间。每个调用方槽位一个线程，线程栈和线程池的构造，可能在图开始执行之前就耗尽这个限制。每次运行外面的 catch 会记录 `engine->run` 抛出的异常，但创建线程池发生在这个 catch 之外。在分配压力下能否干净地退出进程，是一项需要测量的验收标准，不是这个脚本所能保证的。

```bash
# Linux: cap virtual address space, not resident memory.
prlimit --as=$((256*1024*1024)) \
  ./build-stress/bench_sustained_concurrent \
  --concurrency 200 --duration-s 30
```

## 其他实验

24 小时的运行，或者 cgroup 内存上限的实验，需要单独记录环境和结果。要对比各个稳态窗口之间的当前 RSS，记录 `err_total` 和终止信号，并区分 cgroup 对常驻内存的限制与 `prlimit` 对地址空间的限制。本页不报告这些运行。
