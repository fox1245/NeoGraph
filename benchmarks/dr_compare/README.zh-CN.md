<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=zh-CN source_sha256=5d304e3d89c0bb2ffc6cdcddf4d194f2fc1d8bccc0773d0b4c4f9ea49e8392f1 -->
# dr_compare：深度研究编排比较

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

运行器用相同提示和模型选择实现路由 → 规划 → 研究员 Send 分支 → 综合。引擎、绑定、客户端和检查点实现不同，端到端时间不能隔离引擎或传输成本。以下 2026 年 4 月记录是历史证据，不是当前 cutover 的重新运行。

## 文件与依赖

`dr_neograph.py`, `dr_langgraph.py`, `bench.py`, `bench_mock.py`, `mem_probe.py`, `mem_prod_stack.py`, `sweep.sh`, `_run_single.py`涵盖真实调用、纯文本模拟、内存探测、扫描和单次诊断。请按[Python 绑定指南](../../docs/python-binding.md)安装与当前源码匹配的 wheel；含 `CompletionParams`/`OpenAIProvider` 的旧 wheel 不是当前 API。Core 源码构建也需要外部 SchemaProvider SDK。
NeoGraph `0.13.0` 需要匹配 alpha SDK `0.1.0`、interface revision/shared generation 4 的 wheel/native 构建。当前集成验证尚未完成；此比较 runner 与内置 Deep Research 恢复路径分开。

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

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

以下远程模型命令可能产生费用，请有意设置凭据。它使用内存检查点；PostgreSQL 比较需对应 DSN、包配置与单独记录的耐久性范围。系统调用或抓包本身不能证明语义等价或供应商账单。

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```
