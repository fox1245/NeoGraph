<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=zh-CN source_sha256=28ea3137d4e9a60940cf0193feba3c0bb112948387f6e46dcc95360ef28ce6e3 -->
# JARVIS 编排基准测试 — NeoGraph vs LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

在 NeoGraph（C++ mock 构建）和 LangGraph（Python 对等实现 `langgraph_twin.py`）中复刻相同拓扑（mic→stt→merge→memory→router→4-way→synth/skip→commit→tts），并在相同约束（`--cpus=2 --memory=2g`）的容器中测量。

```bash
GROQ_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + groq 20 turns × both
```

## 测量结果（2026-07-05，WSL2，--cpus=2 --memory=2g）

| 指标 | NeoGraph | LangGraph | 差值 |
|---|---|---|---|
| 纯图开销/turn（mock 0ms LLM，200 turns） | **0.38ms** | 3.07ms | +2.7ms (8.1×) |
| Groq 真实推理/turn（8b router+70b synth，20 turns） | 684ms | 706ms | +22ms (~3%) |
| Groq p99 | 775ms | 870ms | +95ms（n=20，噪声范围） |
| 冷启动 | 7.9ms | 716ms | ~90× |
| RSS（mock） | 7.5MB | 68MB | ~9× |

解读：
- 和 LLM 相比，两边的图引擎本身都很轻（0.4ms vs 3ms）。Groq 的 +22ms 差值中约 19ms 是 HTTP 客户端栈差异（langchain-openai httpx+pydantic vs asio）。
- turn-to-turn 差距属于 **增长型差距** — 推理越快，差距越大 — 对 200ms-turn（Cerebras 级 / 单次调用路径）会达到 10%+，对本地小模型（~50ms/call）会达到 20-30%。
- 90× 启动 · 9× RSS 是与推理速度无关的 **固定差距** — 对边缘端常驻 · 冷启动 · 多租户立即相关（100 个 JARVIS = <1GB）。

## E2E 轮次 — 包含真实 MCP 工具往返（2026-07-05）

```bash
GROQ_API_KEY=... bash bench/run_bench_e2e.sh
```

共享 demo MCP server 容器（time/calc/weather）+ 24-turn 混合集（直接工具调用 · 并行 fan-out · 聊天 · 记忆回忆），每边 2 轮并用 ABBA 顺序交错：

| 轮次（执行顺序） | 平均值 | p50 | 最大值 | 备注 |
|---|---|---|---|---|
| neograph r1 | 810ms | 791 | 1052 | |
| langgraph r1 | 673ms | 667 | 934 | |
| langgraph r2 | 1442ms | 1025 | 3830 | 最后 7 turns 2.4~3.8s — Groq throttle window |
| neograph r2 | 689ms | 665 | 983 | 即使紧跟 LG r2 之后运行仍稳定 |

**结论：在这些条件下（韩国→Groq WAN，~700ms/turn），provider-side dispersion（轮次之间 ±130~770ms）完全吞没 framework delta（mock 测得 ~3ms + HTTP stack ~19ms）。** 切换顺序会反转赢家 — e2e turn latency 不能判定框架优劣，只有受控 mock 轮次能测量固定开销和启动/内存。E2E 已验证：两个 harness 都能正确使用真实工具（routing mode match 21/24，direct/parallel 真实往返），启动 74ms vs 1944~2483ms，RSS 14MB vs 122MB 已确认。

含义：框架差异只有在 **低离散 + 低绝对延迟**（本地推理、同数据中心推理）下才有意义 — 不只是“fast inference”。跨 WAN 的云推理会让网络成为主导，无论使用什么框架。

## 边界测量轮次 — 消除 Provider 离散（2026-07-05）

```bash
GROQ_API_KEY=... bash bench/run_bench_proxy.sh
```

用代理边界测量解决 E2E 的“离散吞没差值”问题：把 nginx 放在 Groq 前面，用来 **记录每次调用的 upstream（WAN+Groq）时间**，并从 turn round-trip 中减去它，只比较 residual（graph + HTTP client serialization + local MCP + pipe）。这不是统计绕法（增加 ABBA/retry 次数），而是直接测量并扣除噪声源本身 — 即使不同轮次碰到不同 Groq 窗口，结果也不会摇摆。

| | Avg/turn upstream | **Residual p50** | Residual p90 | Residual min~max |
|---|---|---|---|---|
| NeoGraph | 1613ms | **3.5ms** | 19.1ms | 1.9~80.5 |
| LangGraph | 1417ms | **14.7ms** | 25.1ms | 10.8~33.3 |

- 原始 wall-clock 这次显示“LG 快 189ms”（Groq 给了 NG 轮次更差的窗口 — upstream avg +196ms）。Residual 显示 **NG p50 −11.1ms** — 清楚证明该方法能不受噪声方向影响地恢复信号。
- Residual p50 与 mock 轮次预测一致（graph 0.4 vs 3.1ms + HTTP stack diff）— payload 交叉验证成功。
- Call↔turn 映射是 **基于顺序** 的（验证 call count = 2×turn count，log order = turn order）。Time-window 映射有 WSL2 wall-clock step（运行中测到 -0.8s reversal），只作为 fallback。Driver timestamps 也从 monotonic anchor 推导。
- 陷阱说明：Groq（Cloudflare）会用 403 阻挡 `Python-urllib` UA — 很容易误认为是 proxy 问题。真实 smoke tests 使用 curl/httpx-family UA。

## Streaming TTFT 轮次（2026-07-05）

现代 LLM 服务都会 streaming，所以基准也匹配这一点：两边 synth 调用都改为 streaming（C++ `invoke(p, on_chunk)`，LangGraph `SYNTH_LLM.stream()`），driver 用 `[jarvis:ttft]` marker 测量 **发送 turn → 首个 synth token** 时间。nginx 通过 `proxy_buffering off` 透传 SSE，因此 `$upstream_header_time` 就是真实首字节时间。每轮使用独立日志（mv + `nginx -s reopen`），消除轮次切分猜测。

| | 感知 TTFT p50 | 完成时间 p50 | 每 turn 平均 upstream |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **感知 TTFT 实际打平（delta −2ms）。** NeoGraph 之前看似更慢的 TTFT（800 vs 603）纯属 provider dispersion — 这次 Groq 给了双方公平窗口（upstream 726 vs 753），差距消失。复现实验证实了“NG 轮次只是运气差”的怀疑。
- **完成时间 residual（纯框架）复现**：NeoGraph 4.1ms vs LangGraph 14.6ms（匹配上一轮 proxy 的 3.5 vs 14.7）。框架开销结论稳固。
- **TTFT-residual 在 ±几十 ms 噪声内为 0**（甚至会出现负值）。相比 perceived TTFT 625ms vs upstream sum 673ms，从两个独立时钟（client monotonic vs nginx wall-clock）相减的分辨率（±50ms）大于框架贡献（ms）。也就是说，**在 TTFT 路径上，框架差异低于观测极限** — 信号只会在 total residual/mock 中浮出噪声。
- **Streaming 收益**：感知 TTFT（631）≪ 完成时间（744）— 用户 0.6s 就开始听到回答。已确认相对必须等待完成的非 streaming 模式，感知速度更快。

总结：纯框架性能支持 NeoGraph（total residual·mock，可复现），但 **streaming 下 perceived TTFT 打平，provider dispersion 占主导**。边缘端/多租户（90× startup · 9× RSS）仍然是 NeoGraph 的真正战场。

## 公平性条件

- Prompt（共享 persona.txt）· decision validation（chat downgrade）· memory format（JsonFileStore）· verbatim guard · stdout marker 完全相同。只有框架和语言不同。
- LangGraph 侧使用惯用栈（langgraph + langchain-openai）。
- 测量使用容器内部 `driver.py`（stdin injection → `[jarvis:tts]` marker round-trip）。

## 文件

- `langgraph_twin.py` — LangGraph 对等实现（相同拓扑·协议；设置 MCP_URL 时通过 official mcp SDK persistent session 进行真实工具调用）
- `driver.py` / `analyze.py` — 测量 · 对比表
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` — 基准镜像
- `run_bench.sh`（core）/ `run_bench_e2e.sh`（real tool E2E）— 运行器
- `turns_mock.txt`（200）/ `turns_groq.txt`（20）/ `turns_e2e.txt`（24）— turn 集合
- `../config-bench/` — 空 catalog（固定 chat path）/
  `../config-bench-e2e/` — 共享 MCP server catalog
