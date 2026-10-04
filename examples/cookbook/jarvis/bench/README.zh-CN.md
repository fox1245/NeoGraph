<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=zh-CN source_sha256=3985b561c728357e75ec0cab68d2711e19334c47a588c07f2b836c6ce3fa8eb3 -->
# JARVIS 编排基准测试 — NeoGraph 与 LangGraph 对比

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 类型化提供方迁移 — 源码状态

C++ 路由器、合成器和专家夹具使用类型化 `ProviderRequest`、`sp::Message`、`sp::Event` 及完整不可变 `sp::Outcome` (`sp::runtime::Result`)，不是旧字符串响应 API。`src/provider_support.h` 的 Jarvis/coder/researcher mock 提供固定路由 JSON、用户文本 echo 和明确的模拟研究回复。不需要密钥或网络提供方，但不是真实研究或推理。本次文档更新不会创建或修复下文既有配置/配置档路径。

本地语音是可选项，需要选定的 whisper/Moonshine 模型、ONNX Runtime/Supertonic 资源、miniaudio 和可用的麦克风/扬声器。文本/mock 运行不能证明语音可用。无需云端仅适用于本地/mock。实时请求需要获授权的 `OPENROUTER_API_KEY`、网络与提供方容量，并将提示、对话记忆和附带工具/委派结果发送给 OpenRouter。模型已固定，原生请求设置的 ZDR 不是地域驻留保证。不要将密钥写入日志或版本库。可空 token 用量不是账单金额；费用需要当前端点/模型定价与实际计费用量。

`[jarvis:ttft]` 仅在首个非空 `sp::PartDelta` 且为 `PartKind::Text`、`DeltaChannel::Content` 时发出，不由用量、推理、响应头等事件触发。它表示首次合成文本，不是首次可听见的 TTS 播放。Python REPL driver 仍为 protocol client；pybind benchmark 使用已迁移类型化 binding，需要单独运行证据。当前 [Jarvis CLI 运行证据](../README.md)仅涵盖问候、已持久化的合成记忆 turn 和正常 EOF 退出，不涵盖这些基准轮次。下文所有基准耗时及执行主张仍为历史记录；CLI 运行不验证麦克风、ASR、TTS、pybind 基准或 vendor 推理。

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
- 此历史 container 配置的 startup/RSS 比值约90×/9×。不保证 production JARVIS100个的 memory capacity。

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

此历史 round 将两个 synthesis call 改为 streaming，使用当时 C++ streaming provider 和 LangGraph `SYNTH_LLM.stream()`；当前 C++ 路径用 `ProviderMode::Stream`/`sp::Event`。driver 以 `[jarvis:ttft]` 测 turn-send → first synthesis text。nginx 的 `proxy_buffering off` 透传 SSE，`$upstream_header_time` 是 first byte。各 round 独立 log(mv + `nginx -s reopen`)区分边界。

|  | 感知TTFT p50 | 完成时间 p50 | 每轮均值/上游 |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **感知TTFT实质持平（差值 −2ms）。** NeoGraph之前看似较慢的TTFT（800 vs 603）纯粹是供应商分布差异——此次Groq为两者提供了公平的测量窗口（上游726 vs 753），消除了差距。通过复现证实了“NG轮次仅为运气不佳”的怀疑。
- **历史 completion residual** — NeoGraph4.1ms/LangGraph14.6ms(以前 proxy3.5/14.7)。residual 包含 client serialization、local MCP 和 pipe overhead，不是仅 graph computation 的直接测量。
- **TTFT残余值在±数十毫秒噪声范围内为0**（甚至出现了负值）。与感知TTFT 625ms相比，上游合计673ms，减法运算中合并两个独立时钟（客户端单调时钟 vs nginx墙钟）的误差（±50ms）大于框架贡献（毫秒级）。即，**在TTFT路径中框架差异低于观测极限**——仅在总残余值/mock中信号才会显现于噪声之上。
- **Streaming observation** — 历史 first-synthesis-text 为631ms、completion 为744ms。`[jarvis:ttft]` 是 text marker，不测首个 audible TTS playback 或0.6秒开始听到声音。

历史 streaming text-marker TTFT 持平。此测量不验证当前 SDK transport、audio latency 或 production tenant capacity。

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
