<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=zh-CN source_sha256=9a8d8defc5b23d66cb6abe96f28e5a3dd82b273a8833a472cf355d9f5b836b35 -->
# JARVIS 编排基准测试 — NeoGraph 与 LangGraph 对比

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

镜像相同的拓扑(mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)位于NeoGraph(C++ mock构建)和LangGraph(Python孪生 `langgraph_twin.py`)中，在相同约束(`--cpus=2 --memory=2g`)容器内测量。

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 历史结果（2026-07-05，OpenRouter 迁移前；Groq运行）

| 指标 | NeoGraph | LangGraph | 差值 |
|---|---|---|---|
| 纯图开销/轮(mock 0ms LLM，200轮) | **0.38ms** | 3.07ms | +2.7ms (8.1×) |
| Groq真实推理/轮(8b router+70b synth，20轮) | 684ms | 706毫秒 | +22ms (~3%) |
| Groq p99 | 775毫秒 | 870毫秒 | +95毫秒（n=20，噪声边际） |
| 冷启动 | 7.9毫秒 | 716毫秒 | ~90× |
| RSS（模拟） | 7.5 MB | 68MB | ~9× |

解读：
- 图引擎本身在两侧相对于LLM都更便宜（0.4毫秒对比3毫秒）。Groq差异+22毫秒（相对于约19毫秒）是HTTP客户端栈的差异（langchain-openai 的 httpx+pydantic vs asio）。
- 回合间间隔是**增长型**，随着推理速度的提升而变大——对200毫秒/回合（Cerebras级别/单次调用路径）产生10%以上的差异，对本地小模型（约50毫秒/调用）产生20-30%的差异。
- 90倍的启动时间、9倍的RSS是**固定缺口**，与推理速度无关——对于边缘常驻·冷启动·多租户场景立即相关（100 JARVIS = <1GB）。

## 端到端轮次——包括真实 MCP 工具往返（2026-07-05）

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_e2e.sh
```

共享演示 MCP 服务器容器（时间/计算/天气）+ 24轮混合集（直接工具调用·并行 fan-out·聊天·记忆召回），ABBA 顺序交错，每轮各执行 2 次：

| 轮次（执行顺序） | 平均 | p50 | max | 备注 |
|---|---|---|---|---|
| neograph r1 | 810ms | 791 | 1052 |  |
| langgraph r1 | 673ms | 667 | 934 |  |
| langgraph r2 | 1442ms | 1025 | 3830 | 最近7轮 2.4~3.8s — Groq 限流窗口 |
| neograph r2 | 689ms | 665 | 983 | 尽管紧随 LG r2 运行，仍然稳定 |

**结论：在这些条件下（Korea→Groq WAN，约700ms/轮），供应商侧离散度（各轮之间±130~770ms）完全掩盖了框架差异（模拟测得约3ms + HTTP 栈约19ms）。** 切换顺序会反转胜负——端到端轮次延迟无法决定框架优劣，只有受控的模拟轮次才能衡量固定开销和启动/内存。端到端已验证：两种框架均能正确配合真实工具（路由模式匹配21/24，直接/并行真实往返），启动时间 74ms 对比 1944~2483ms，RSS 14MB 对比 122MB 已确认。

启示：框架差异仅在**低离散度+低绝对延迟**（本地推理、同数据中心推理）下才有意义——不仅限于“快速推理”。跨 WAN 的云推理使得网络因素无论在哪种框架下都占据主导地位。

## 边界测量轮次 — 消除供应商离散度 (2026-07-05)

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_proxy.sh
```

用代理边界测量解决端到端测试“离散度掩盖差异”的问题：在 Groq 前面放置 nginx，以**记录每次调用的上游（WAN+Groq）时间**，并将轮次往返时间减去该时间后仅比较残差（图+HTTP客户端序列化+本地MCP+管道）。这不是统计学的变通方法（增加 ABBA/重试次数），而是直接测量并减去噪声源本身——即使各轮次落在 Groq 不同的时间窗口，结果也不会发生波动。

|  | 每轮均值/上游 | **残余p50** | 残余p90 | 残差 min~max |
|---|---|---|---|---|
| NeoGraph | 1613ms | **3.5ms** | 19.1ms | 1.9~80.5 |
| LangGraph | 1417ms | **14.7ms** | 25.1ms | 10.8~33.3 |

- 原始 wall-clock 显示本次“LG 快 189ms”（Groq 给了 NG 较差的窗口——上游平均延迟 +196ms）。残余 residual 显示 **NG p50 为 −11.1ms**——清晰证明了方法恢复了信号，无论噪声方向 noise direction。
- 残余 p50 与模拟回合预测匹配（图 0.4 vs 3.1ms + HTTP 栈差异）—— 载荷交叉验证成功。
- Call↔turn 映射基于顺序（验证调用计数 = 2×回合计数，日志顺序 = 回合顺序）。时间窗口映射具有 WSL2 墙钟步进（运行期间测得 -0.8s 逆转），仅作为后备。驱动时间戳也源自单调锚点。
- 陷阱提示：Groq(Cloudflare) 以403阻止`Python-urllib` UA——容易误认为是代理问题。真正的冒烟测试使用curl/httpx系列UA。

## 流式TTFT轮次（2026-07-05）

现代LLM服务均采用流式传输，因此基准测试与之匹配：两次合成调用均改为流式（C++ `invoke(p, on_chunk)`、LangGraph `SYNTH_LLM.stream()`），驱动程序测量**发送请求回合 → 第一个合成token**的时间，并带有`[jarvis:ttft]`标记。nginx通过`proxy_buffering off`透传SSE，因此`$upstream_header_time`即为真实首字节。每轮分别记录日志（mv + `nginx -s reopen`）以消除轮次切分的猜测。

|  | 感知TTFT p50 | 完成时间 p50 | 每轮均值/上游 |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **感知TTFT实质持平（差值 −2ms）。** NeoGraph之前看似较慢的TTFT（800 vs 603）纯粹是供应商分布差异——此次Groq为两者提供了公平的测量窗口（上游726 vs 753），消除了差距。通过复现证实了“NG轮次仅为运气不佳”的怀疑。
- **完成时间残余值（纯框架差异）得到复现**：NeoGraph 4.1ms vs LangGraph 14.6ms（与之前的代理轮次3.5 vs 14.7相匹配）。框架开销的结论可靠。
- **TTFT残余值在±数十毫秒噪声范围内为0**（甚至出现了负值）。与感知TTFT 625ms相比，上游合计673ms，减法运算中合并两个独立时钟（客户端单调时钟 vs nginx墙钟）的误差（±50ms）大于框架贡献（毫秒级）。即，**在TTFT路径中框架差异低于观测极限**——仅在总残余值/mock中信号才会显现于噪声之上。
- **流式传输优势**：感知TTFT（631）远小于完成时间（744）——用户能在0.6秒内开始听到回答。确认相比等待完整完成后再输出的非流式传输，感知速度有所提升。

摘要：框架纯性能方面NeoGraph更优（总余差·mock，可复现），但感知TTFT在流式传输中持平，且provider分布差异占主导。边缘/多租户场景（启动时间90×·RSS 9×）仍是NeoGraph的真正战场。

## 公平性条件

- 提示词（共享persona.txt）·决策验证（聊天降级）·内存格式（JsonFileStore）·逐字保护·stdout标记一致。仅框架和语言不同。
- LangGraph侧使用惯用技术栈（langgraph + langchain-openai）。
- 测量是容器内部的 `driver.py`（stdin注入 → `[jarvis:tts]` 标记往返传递）。

## 文件

- `langgraph_twin.py` — LangGraph 对应实现（相同拓扑·协议，当 MCP_URL 设置时通过官方 mcp SDK 持久会话进行真实工具调用）
- `driver.py` / `analyze.py` — 测量 · 对比表
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` — 基准图像
- `run_bench.sh`(core) / `run_bench_e2e.sh`(real tool E2E) — 运行程序或Runner
- `turns_mock.txt`(200) / `turns_openrouter.txt`(20) / `turns_e2e.txt`(24) — 轮次集合
- `../config-bench/` — 空目录(聊天路径已修复) / `../config-bench-e2e/` — 共享MCP目录
