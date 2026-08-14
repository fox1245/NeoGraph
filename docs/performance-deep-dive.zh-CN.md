<!-- neograph-i18n: source=docs/performance-deep-dive.md locale=zh-CN source_sha256=6420c556d0eca29b919ff60a7327ec851d1dab1488dddf29e215d7e427849a8a -->
# 性能深入探讨

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)


>**性能**和**轻量化**背后的详细测量
>轴。README有标题数字；这是完整的证据。

---

## 生产经济性

四个点（单dep树、不需要Docker、冻结ABI当您实际扩展时，单轮部署）会复合成一个明显不同的成本结构AWS / GCP/ 天蓝色。有两种机制 - **自动扩展时的队列安全**和 **每个实例的工作人员** - 推动了数字的增长。

### 无需 Heisenbugs 即可自动缩放

LangChain在AWS有效地要求`docker image hash`一直固定在堆栈中 -ECR- 不可变的图像，ASG启动固定到图像哈希的模板，该哈希的多区域复制。没有它，每一次舰队变更事件都是一颗定时炸弹：

|事件| LangChain风险| NeoGraph行为|
|---|---|---|
| ASG推出新产品EC2 | `pip install`可能会拉动更新的及物次要→舰队行为漂移|Wheel 在 PyPI 上是哈希不可变的；新实例 = 字节相同的二进制文件|
|拉姆达冷启动|5–15 秒（`langchain-community`导入图）|ms-class — 没有传递性导入|
|现场中断+卡彭特重建|操作系统包 + 可传递的 Python dep 漂移|静态链接 C++；仅有的`libc.so.6`事情|
|蓝/绿部署|在部署时重建的映像 = 与昨天不同的运行时| `pip install neograph-engine==X.Y.Z`可以单独通过版本字符串重现|
|多区域推出|PyPI 镜像滞后 +ECR复制时间 → 区域出现分歧|跨区域、周期的轮哈希相等|
|“代码 0 行已更改，产品损坏”|经常出现（Pydantic v1→v2 / 2024）|结构上不可能——没有可漂移的过渡表面|

→ NeoGraph删除SOP那LangChain产品*需要*。裸机`pip install neograph-engine`在一个EC2用户数据脚本本身就是产品级的。

### 每个实例的工作人员 —RAM边三角洲

| | LangGraph | NeoGraph |
|---|---|---|
|刚进口（零工人）|**80 MB**|**5.5 MB**|
|1024名闲置工人|（通常OOM-班级）|**31 MB**|
|每个工作线程的开销（空闲，无用户状态）|~200–500 MB 真实产品|测量 ~30 KB|
|t3.medium (4 GB) — 工作人员/实例|7–17|**700–3,500**|
|1K并发请求所需的实例|60–140|**1–3**|
|us-east-1 支出（24/7，点播 t3.medium）|**~$1,800–4,300/月**|**~$30–90/月**|

对于相同的并发用户数，基础设施成本比为 **50–150×**。每个工作线程数量背后的机制是下面的 L3 缓存适配故事：NeoGraph无论N如何，热工作集都是277 KB，因此垂直尺度上限由物理设置RAM本身，而不是缓存压力。

>*”LangChain运行时成本：1 K 并发用户约为 4 K 美元/月。
> NeoGraph：~50 美元/月。代码形状相同，相同LLM, 冷冻ABI."*

这是 SRE/平台团队在否决时关心的角度LangChain在产品中。这不是“Python 很慢”——而是“成本曲线使得SLA不可能的。”

### 测量：10,000 个并发工作人员，一个进程，一个GPU

上表是保守的。直接压力测试确定了真实的数字——*测量*，而不是推断。设置：

- 一个过程，一个RTX 4070 Ti，1 个Gemma 4 E2B Q4 GGUF（约 1.5 GB
模型权重通过 llama.cpp）。
- 单一共享`LocalProvider`序列化推理
GPU边界（代表典型的“你的LLM端点是瓶颈”生产形状）。
- N并发NeoGraph工作人员，每个人运行一个 1 节点图
（`llm_call` → `__end__`） 和`engine.run_async()`，都在争夺同一个提供商。
- 真实生成：输入`"Hi"`，输出例如
`"Hello! How can I help you today?\n"`。

|工人人数|墙钟时间|吞吐量（rps）|p50（毫秒）|p99（毫秒）|峰值 RSS (MB)|引擎开销 (MB)|每个工人增量|
|---:|---:|---:|---:|---:|---:|---:|---:|
|**1**|0.64|1.6|642|642|2 464|+294¹| — |
|**10**|0.94|10.6|184|686|2 529|+359|7.2 MB/工作人员|
|**100**|4.81|20.8|343|855|2 549|+379|222 KB/工人|
|**1 000**|44.1|22.7|347|673|2 564|+394|**6 KB/工作人员**|
|**5 000**|213.7|23.4|338|657|2 570|+400|**1.2 KB/工作人员**|
|**10 000**|**424**|**23.6**|**337**|**648**|**2 572**|**+403**|**≈ 1 KB/工作人员**|

¹ 一次性 KV 缓存和 llama.cpp 激活缓冲区。分配后在所有 N 个工作线程之间摊销。

**数字说明了什么：**

- **10,000 个工作人员的成本增加了 9 MBRAM超过 1,000 名工人**
(2 564 → 2 572 MB)。额外一名工人的边际成本*收敛到大约 1 KB* — 一个`RunConfig`加一个`thread_id`细绳。
- **吞吐量是GPU-以 23 rps** 结合，对于 N = 100 和
N = 10 000。引擎将 10 000 个空闲工作人员安排在队列中 7 分钟，对挂起时间没有任何贡献。
- **p99 延迟平坦**（N = 10 000 时为 648 毫秒，而 N = 10 时为 686 毫秒）。
队列深度不会累积延迟——调度程序公平地释放工作线程GPU排水沟。
- **工人/实例上限由物理设定RAM，不由
引擎。** 在 32 GB 主机上，N 之前可以增长到 ≈ 3000 万个工作线程RAM饱和。

对于 1 K 工人LangGraph之前的成本预测中，每个工人的隐含假设为 200-500 MB。 **这NeoGraph测量值为 6 KB。** 该比率不是 100× — 它是 ≈ 30 000–80 000×。

基准源位于姐妹项目中 [`neoclaw`](https://github.com/fox1245/neoclaw)：[`benchmarks/bench_concurrent_workers_local_llm.cpp`](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp)。再现与`-DNEOCLAW_BUILD_BENCHMARKS=ON -DNEOCLAW_BUILD_CUDA=ON`。

---

## 适合 L3 缓存的代理运行时

NeoGraph的热代码路径足够小，以至于 N 个并发代理共享一个 L3 驻留工作集。我们在 Ryzen 7 5800X（Zen 3：32 KB L1i/d 8 路，**32 MB L3 16 路**）上使用 Valgrind cachegrind 进行测量，扫描 N = 1 → 10,000 个并发请求`benchmarks/concurrent/bench_concurrent_neograph`：

|氮|我参考|**L3 指令缺失**|L3i 未命中率|天然 p50|
|---:|---:|---:|---:|---:|
|1|5.3M|**4,313**|0.08%|17微秒|
|10|5.9M|**4,304**|0.07%|16微秒|
|100|11.8M|**4,320**|0.04%|6微秒|
|1,000|69.7M|**4,327**|0.01%|6微秒|
|10,000|**648米**|**4,329**|**0.00%**|**5 微秒**|

**L3 指令未命中率在 N 的四个数量级上保持稳定在 ~4,320**。独特的热代码工作集大致为`4,330 × 64 B = 277 KB`— **32 MB L3 的 0.85%**。当 N = 10,000 时，我们处理了 **6.48 亿条指令**，但只有 **4,329 条指令达到了DRAM**（约每 150,000 条指令 1 次丢失）。

随着 N 的增长，每个请求的本机延迟从 17 µs（冷）下降到 5 µs（热）——3.4 倍的改进纯粹是 I-cache 预热。单线程池上 N = 10,000 时的吞吐量约为 1.1 M req/s，峰值为 5.2 MBRSS（≈ 100 B / 代理边际成本）。

**为什么这很重要：**DRAMZen 3 上的访问约为 250 个周期，而 L3 命中约为 46 个周期 — 每次访问大约慢 5.5 倍。如果NeoGraph的工作集已经溢出了 L3（正如 Python 解释器 + dict-heavy 状态通常所做的那样），相同的 N = 10,000 次扫描将在内存停顿中付出 **+420 到 +840 毫秒的代价**，而不是测量的 **9 毫秒的总挂壁时间** — 慢 47–94 倍，具体取决于未命中链达到的程度DRAM。整个 L3 仍然可用于*您的*工作负载（对话历史记录、嵌入、工具响应）：引擎本身是一个舍入误差。

_复制：_
```bash
g++ -std=c++20 -O2 -DNDEBUG -Iinclude -Ideps -Ideps/yyjson -Ideps/asio/include \
    -DASIO_STANDALONE benchmarks/concurrent/bench_concurrent_neograph.cpp \
    build-release/libneograph_core.a build-release/libyyjson.a -pthread -o bench_ng

valgrind --tool=cachegrind --cache-sim=yes \
    --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 ./bench_ng 10000
```

### 与真实的端对端保持一致LLM在循环中

L3 故事在全栈生产中幸存下来：我们指出NeoGraph在本地托管的 Gemma-4E2B（Q4_K_M, 4.65 B 参数, 2.9 GBGGUF）在 OpenAI 兼容的背后HTTP终点——只需设置`OpenAIProvider::Config::base_url = "http://127.0.0.1:8090"`和显式的本地开发选项`allow_insecure_loopback = true`。看 [`examples/31_local_transformer.cpp`](../examples/31_local_transformer.cpp)。

| |纯的NeoGraph | **NeoGraph+ 本地Gemma (HTTP)** |
|---|---:|---:|
|L3 指令未命中|4,320|**7,262**|
|热代码工作集|277 KB|**465 KB**（L3 的 1.42%）|
|按请求TTFT | — |**25–27 ms**（卷曲基线 9–10 ms → ~15 msNeoGraph开销）|
|每个请求总计| — |146–213 ms @ 19–27 个令牌（~130 tok/s）|
| **NeoGraph代理人RSS** |5.2MB|**7.6 MB**（+2.4 MB 用于 httplib +JSON流式传输）|
|Gemma服务器RSS |不适用|2.45 GB（内存映射GGUF）|
| VRAM（RTX 4070 Ti）|不适用|3.06GB|

推理过程位于**单独的地址空间**，因此其 2.5 GB 的模型权重永远不会触及NeoGraph的 L3 缓存线。无论模型有多大，代理的 465 KB 工作集都会驻留在 L3 中。这就是两进程拆分的架构回报：您可以交换 70 B 模型，而无需膨胀代理。

经过 5 个并发突发测试NeoGraph针对同一服务器的代理：聚合墙 1.58 秒/5 个请求（协程重叠带来 2.65 倍加速）。每个代理的吞吐量在队列压力下会下降，因为 Gemma 服务器没有实现连续批处理 - 这是推理服务器的问题，而不是代理的问题。NeoGraph干净地调度了所有 5 个，没有资源压力，并且RSS保持在约 7 MB 不变。

---

## 基准测试

### 引擎开销与 Python 图形/管道框架

匹配拓扑、零 I/O 工作负载：图形编译一次，在热循环中调用。衡量引擎本身的成本（调度、状态写入、归约器调用）- 否LLM，没有睡眠，没有网络。

![NeoGraph与 Python 框架对比 — 每次迭代延迟和峰值RSS](images/bench-engine-overhead.png)

每次迭代引擎开销（微秒，越低越好）。所有行均于 2026 年 4 月 22 日在同一 x86_64 Linux 主机上进行测量。NeoGraph使用发布构建`-O3 -DNDEBUG`（10 次运行中位数）； Python 行是 CPython 3.12.3 中 3 次运行的中位数。

|框架| `seq`（3节点链）| `par`（扇出 5 + 连接）| `seq`与NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|
| **NeoGraph掌握**|**5.0 微秒**|**11.8 微秒**|1×|
|干草堆2.28.0|144.1微秒|290.0 微秒|28.8×|
|pydantic-graph 1.85.1|235.9微秒|286.1微秒1|47.2×|
| LangGraph1.1.9|656.7 微秒|2,348.7 µs|131.3×|
| LlamaIndex工作流程 0.14.21|1,780.3 µs|4,683.5 µs|356.1×|
| AutoGen GraphFlow0.7.5|3,209.2 微秒|7,292.7 µs|641.8×|

1 pydantic-graph 是一个单下一个节点状态机，不能扇出；`par`是串行 6 节点仿真。

全流程指标（预热+两个工作负载，10k seq + 5k par iters）：

| | NeoGraph |最好的Python（干草堆）|最坏的（AutoGen）|
|---|----------|------------------------|-----------------|
|**总经过时间**|**~0.16秒**|2.91秒|68.29秒|
|**峰值 RSS** |**4.8 MB**|80.3MB|52.4MB²|
|**并行扇出执行器**| `asio::experimental::make_parallel_group` |单线程异步（GIL）|单线程异步（GIL）|

²AutoGen有一个较小的RSS比LlamaIndex但它的每迭代成本高出 64 倍——不同的权衡轴。完整矩阵 [`benchmarks/README.md`](../benchmarks/README.md)。

**引擎开销消失在LLM** 500 毫秒的 OpenAI 往返时间淹没了每个引擎；每迭代间隔仅出现在非LLM节点（数据转换、路由决策、纯计算工具调用）和密集代理编排。无论它出现在哪里，它都会表现得很大：在 Raspberry Pi 4 / Jetson Nano / 任何设备上SBC- 级目标，10–20×RAMdelta 是“fits”和“swapthrash”之间的差异。

再现和方法：[`benchmarks/README.md`](../benchmarks/README.md)。

### 突发并发（1CPU/ 512 MB 沙箱）

在数千个同时请求的情况下会发生什么？突发测试：在 t=0 时向每个引擎提交 N 个请求，全输入/全等待，在 Docker cgroup 内限制为 **1CPU和 512 MBRAM** — 大致相当于 Raspberry Pi 4 的流程预算。

![尾部延迟 —P99根据要求](images/bench-concurrent-latency.png)

![并发负载下的吞吐量](images/bench-concurrent-throughput.png)

![峰值驻留内存](images/bench-concurrent-rss.png)

在 asyncio 模式下 **N=10,000 个并发请求**（每个 Python 框架的默认部署形状）：

|引擎|墙钟时间| P99延迟|峰值 RSS |地位|
|--------|-----:|------------:|---------:|:-------|
| **NeoGraph掌握**|**52 毫秒**|**7 微秒**|**5.5 MB**|✅ 10000 / 0|
|pydantic-graph|886 毫秒|**158 微秒**|42.6MB|✅ 10000 / 0|
|草垛|3.1秒|2.9秒|130.7 MB|✅ 10000 / 0|
| LangGraph |23.4秒|23.0秒|416.2 MB|✅ 10000 / 0|
| LlamaIndex | — | — | — | ❌ **OOM被杀**|
| AutoGen | — | — | — | ❌ **OOM被杀**|

**两个框架尚未完成** —LlamaIndex工作流程和AutoGen GraphFlow耗尽 512 MB cgroup 并得到OOM- 在 10k 并发协程耗尽之前被杀死。其余的 Python 框架会退化而不是消亡，但是它们的P99延迟随 N 线性增长，因为 CPythonGIL序列化每个协程的CPU工作。 **这不是一个LangGraph-特定的病理学**——它出现在每个Python asyncio运行时中。

NeoGraph在吞吐量、尾部延迟和性能方面优于所有 Python 异步运行时RSS：7微秒P99N=10k 时，约低 76×RSS比LangGraph相同负载下，领先3个数量级GIL-序列化的Python曲线。即使是 pydantic-graph（最精简的 Python 状态机）也只有 158 µsP99和~8×NeoGraph的RSS。

`multiprocessing.Pool`模式绕过GIL跨工作进程，但在池大小上饱和并支付 fork + pickle 开销；完整的数字和 mp 模式的故事在 [`benchmarks/concurrent/CONCURRENT.md`](../benchmarks/concurrent/CONCURRENT.md)。

### 尺寸和冷启动足迹（计划和执行器演示）

以下所有数字均在 x86_64 Linux 上测量（GCC13）使用`example_plan_executor`— 一个独立的计划和执行器演示，运行 5 路发送扇出，崩溃子主题#2在第一次运行时，并在故障清除后恢复。不LLM打电话，没有API钥匙，无网络。

|构建配置|尺寸|
|---|---|
| **MinSizeRel `-Os`, 静态 libstdc++,`--gc-sections`，剥离**|**1,203 KB (1.2 MB)**|

这MinSizeRel二进制文件唯一的动态依赖是`libc.so.6` — `libstdc++`和`libgcc_s`静态链接。将其拖放到任何具有匹配 libc 的 Linux 主机上即可运行。

|公制|价值|
|---|---|
|峰值 RSS（完整的计划和执行器运行，包括崩溃+恢复）|**2.9 MB**|
|墙钟时间（冷启动→两个阶段均完成）|**~720 毫秒**|
|动态依赖| `libc.so.6`仅有的|

`example_plan_executor`每个发送目标休眠 120 毫秒以模拟LLM称呼。该示例通过以下方式选择加入硬件大小的扇出池`EngineConfig::worker_count`前`GraphEngine::build()`，因此五个目标同时执行。稳定状态RSS不受影响。
```bash
cmake -B build-minsize -S . \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DNEOGRAPH_BUILD_MCP=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_POSTGRES=OFF \
    -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections -static-libstdc++ -static-libgcc"
cmake --build build-minsize --target example_plan_executor -j$(nproc)
strip --strip-all build-minsize/example_plan_executor
ls -la build-minsize/example_plan_executor   # size
ldd    build-minsize/example_plan_executor   # libc only
/usr/bin/time -v build-minsize/example_plan_executor   # RSS + wall
```

### 这些数字对于嵌入式/机器人意味着什么

- **1.2 MB 静态二进制文件** 适合 Docker`scratch`图像大小约为 1 MB，适合
Pixhawk 配套计算机的板载闪存可轻松装入 Jetson Orin 启动分区。 Python+LangGraph没有。
- **2.9 MBRSS** 表示您可以托管 **100+ 个并发代理会话**
在 RPi Zero 2W 上 (512 MBRAM）通过跨线程共享一个编译引擎 - 请参阅[`docs/concurrency.md`](concurrency.md)对于模式。

- **< 250 毫秒冷启动** 可在无人机看门狗重置窗口内完成；一个 Python LangGraph 进程到那时连 `import` 都还没完成。
- **仅依赖 `libc.so.6`** 使交叉编译极为简单：选择 `glibc` 或 `musl` 进行链接——没有传递依赖噩梦。
