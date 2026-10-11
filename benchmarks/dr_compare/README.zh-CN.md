<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=zh-CN source_sha256=04ec3704b620015a6f954234c5ee83256367bbd43dbd1f84150346e03a6ab528 -->
# dr_compare：深度研究编排比较

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

运行器用相同提示和模型选择实现路由 → 规划 → 研究员 Send 分支 → 综合。引擎、绑定、客户端和检查点实现不同，端到端时间不能隔离引擎或传输成本。以下 2026 年 4 月记录是历史证据，不是当前 cutover 的重新运行。

## 文件与依赖

`dr_neograph.py`, `dr_langgraph.py`, `bench.py`, `bench_mock.py`, `mem_probe.py`, `mem_prod_stack.py`, `sweep.sh`, `_run_single.py`涵盖真实调用、纯文本模拟、内存探测、扫描和单次诊断。请按[Python 绑定指南](../../docs/python-binding.md)安装与当前源码匹配的 wheel；含 `CompletionParams`/`OpenAIProvider` 的旧 wheel 不是当前 API。Core 源码构建也需要外部 SchemaProvider SDK。
当前构建需要匹配 SDK `0.3.0`、interface revision/shared generation 6 及已合并 SchemaProvider PR #21（`83112573ba59e3b561fc33c22394638be7aa5294`）的 wheel/native 构建。当前集成验证尚未完成；此比较 runner 与内置 Deep Research 恢复路径分开。

工作流导入 requests、LangGraph、langchain-openai；内存探测使用 psutil。PostgreSQL 模式还需对应检查点包和数据库。`mem_prod_stack.py` 还导入各栈的 Web/数据库/观测包，因此不是仅测引擎的 RSS 探测。

## 当前环境控制

| Variable | Default | Purpose |
|---|---|---|
| `LLM_MOCK_MS` | `-1` | 负数为真实调用，>=0 为带 sleep 的文本处理，无 Provider/outcome。 |
| `MOCK_SEARCH` | `0` | 1 跳过 Crawl4AI，返回固定证据。 |
| `FANOUT` | `5` | 研究员分支数/上限。 |
| `USE_INMEMORY_CP` | `0` | 1 选择内存检查点，模拟模式也选择内存。 |
| `NG_TRANSPORT` | `http-chat` | NG: http-chat 或 http-responses，无 WebSocket；Responses 是不同 API。 |
| `NG_WORKER_COUNT` | `4` | NG fan-out 工作线程数。 |
| `DR_MODEL` | `gpt-5.4-mini` | 双方真实调用的显式模型。 |
| `NG_EXAMPLE_MAX_TOKENS` | `1600` | NG/LG 双侧每次调用的输出上限；不会自动提高。 |
| `NEOGRAPH_PG_DSN` | `empty` | NG PostgreSQL DSN，为空则使用内存。 |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG PostgreSQL DSN 覆盖。 |
| `CRAWL4AI_URL` | `empty` | 搜索服务；为空时除模拟外无法搜索。 |

`OPENAI_API_BASE` 选择真实调用的已准入 origin/gateway prefix；NG 的 `NG_PROVIDER_DESCRIPTOR` 可提供完整 descriptor。凭据与 CA 属于运行时选项（`OPENAI_API_KEY`、`NG_EXAMPLE_CA_FILE`），不是 descriptor 数据。NG 默认 HTTP Chat 与 LG Chat API 对齐，HTTP Responses 则刻意比较不同 API。HTTP/2 取决于 libcurl 和对端；运行器不证明多路复用或固定连接数。

## 历史记录：2026-04-26

1. 模拟 LLM、FANOUT=5：记录的中位数为 NG 1.0 ms、LG 5.9 ms（该工作负载的 5.9× 比值）。
2. 首轮远程模型运行：NG p50 23.90 s（sd 5.90 s），LG 21.95 s（sd 1.23 s）。
3. 历史连接诊断在七次模型调用的 NG 运行中记录 21 次 `connect()`。该次数不能独自证明 TLS 会话数、HTTP 版本或载荷等价。
4. 历史 `6da4810` / `bc2ab4f` 连接池修改关联的记录为 NG p90 35.34 s → 25.28 s、sd 5.90 s → 1.28 s。当时的供应商实现已移除；当前 typed SchemaProvider 使用外部 SDK/libcurl。
5. FANOUT=50、LLM_MOCK_MS=100、NG_WORKER_COUNT=50 实验报告 NG 307 ms、LG asyncio 711 ms。这是独立负载，不是通用服务器容量结果。

NeoGraph 仍需添加 HTTP/2 支持的旧说法已过时。这些时间不能验证新 SDK、当前 wheel、其他平台或远程推理加速。模拟模式只测编排和配置 sleep，不产生供应商证据。真实模式从 typed 所有权 outcome 提取可见文本，但不测 native replay、portable 历史导出或供应商报告量/预算计费量结算。

## 运行新批次

以下模拟命令不调用远程模型或持久化。新结果应附源码/SDK/wheel 修订、Python/依赖版本、主机限制、工作线程数、预热、迭代数、检查点模式和失败数。保留历史文件不变。
两个 harness 均拒绝空报告，若任一预热或测量运行失败则以非零状态退出；时间统计仅包含成功样本。报告通过前，`Failed runs` 必须为 0。在仓库 root 运行 `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py` 可执行无供应商调用的统计回归测试。

`bench_mock.py` 将未设置的 `LLM_MOCK_MS` 设为 0，并在 import 前强制模拟搜索/内存检查点。负延迟或已按真实模式导入的 runner 会在派发前被拒绝，继承的凭据不会触发付费调用。测量迭代必须为正数，预热不能为负数。LG 只提取真实文本块，不将空响应/计划作为成功样本。
真实路径不会把截断/拒绝/tool/unknown 终止作为已完成报告计时。NG 错误保留原始拥有的 Outcome，LG 错误保留原始 response。无供应商回归测试只验证消费者投影策略，不是 transport/SDK/实时模型证据。

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
env -u NG_WORKER_COUNT LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

若要观测 fan-out 重叠，请以 `LLM_MOCK_MS=100` 再运行，保持 `FANOUT=5`、预热、迭代及检查点配置相同，对比 unset `NG_WORKER_COUNT`（默认 4）与显式 `NG_WORKER_COUNT=1`。记录 stderr 和失败。没有串行 fan-out 警告本身不能证明重叠。这是无模型负载，不是付费供应商证据。

以下远程模型命令可能产生费用，请有意设置凭据。它使用内存检查点；PostgreSQL 比较需对应 DSN、包配置与单独记录的耐久性范围。系统调用或抓包本身不能证明语义等价或供应商账单。

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```

此命令不是消费限制。`FANOUT=5` 的研究查询每侧最多 7 次逻辑模型调用（plan、5 个研究者、synthesis）。双侧预热 2 次加测量 5 次共 98 次逻辑调用。当前 NG SDK 默认禁用传输重试；LG 显式设置 `max_retries=0`。双侧输出上限均为 `NG_EXAMPLE_MAX_TOKENS=1600`，显式值保持不变。调用数及配置的输出额度总和须在基准外另行授权/预留。仅使用获准的模型、endpoint、凭据及搜索服务；缩减 cohort 必须与历史多次迭代测量区分。

有限的付费供应商诊断（不是延迟比较）如下：

```sh
# From benchmarks/dr_compare, only after a separate spending reservation.
: "${OPENAI_API_KEY:?Requires an intentionally authorized hosted credential}"
env -u NG_WORKER_COUNT LLM_MOCK_MS=-1 MOCK_SEARCH=1 USE_INMEMORY_CP=1 \
  FANOUT=5 NG_TRANSPORT=http-chat NG_EXAMPLE_MAX_TOKENS=8192 \
  python bench.py --only neograph --warmup 0 --iters 1
```

最多 7 次模型调用、配置的输出 token 共 57,344，输入 token 另计。它不执行真实网页搜索，必须标记为付费供应商/模拟搜索，而不是真实搜索 qualification。通过要求 exit 0、1 个成功样本、非空报告及 `Failed runs: 0`；计划、回答或报告任一失败或为空即为失败。8,192 token 额度是显式诊断选择，不是提高默认值、价格估算或消费授权。如有获准且正在运行的 Crawl4AI 服务，可用 `MOCK_SEARCH=0` 在相同模型调用上限内执行真实搜索。
