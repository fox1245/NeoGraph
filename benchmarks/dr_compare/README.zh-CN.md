<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=zh-CN source_sha256=505672efc95e54e5c55c2204f9bff828e13151f825659a9473eceab788c37149 -->
# dr_compare：深度研究编排对比

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这些 runner 实现的是同一条链路：router → plan → 通过 Send 分出的 researcher 分支 → synthesis，使用相同的提示词和模型选择。它们的引擎、绑定、客户端和检查点实现各不相同，所以端到端的耗时，无法单独分离出引擎开销或传输层开销。下面 2026 年 4 月的结论是历史记录，没有用当前的 SDK 重新运行过。

## 文件与依赖

`dr_neograph.py`、`dr_langgraph.py`、`bench.py`、`bench_mock.py`、`mem_probe.py`、`mem_prod_stack.py`、`sweep.sh`、`_run_single.py` 覆盖了真实调用、纯文本的 mock 工作负载、内存探测、参数扫描和一次性的问题诊断。请按 [Python 绑定指南](../../docs/python-binding.md)安装与当前源码匹配的 wheel；带有 `CompletionParams`/`OpenAIProvider` 的旧 wheel，不是当前的 API。从源码构建时，即使只用 Core，也需要外部的 SchemaProvider SDK。
当前的构建需要与 SDK `0.3.0`、接口修订版/共享库代数 6 相匹配的 wheel 或原生构建，
对应 SchemaProvider `maint/interface6` 提交
`f7ed8161fbcf39a6bf9358f78cfdcfda133537a2`。
这个对比用的 runner，与内置的 Deep Research 恢复路径是分开的。

这个工作流会 import requests、LangGraph 和 langchain-openai；内存探测使用 psutil。PostgreSQL 模式还需要相应的检查点包，以及一个正在运行的数据库。`mem_prod_stack.py` 会为它命名的各个技术栈 import 额外的 web、数据库和可观测性包；它不是只针对裸引擎的 RSS 探测。

## 当前的环境变量控制项

| 变量 | 默认值 | 用途 |
|---|---|---|
| `LLM_MOCK_MS` | `-1` | 小于 0 时发起真实调用；>=0 时是带 sleep 的纯文本节点工作，不涉及 Provider 或 outcome。 |
| `MOCK_SEARCH` | `0` | 为 1 时跳过 Crawl4AI，返回预置的证据。 |
| `FANOUT` | `5` | researcher 分支的数量/上限。 |
| `USE_INMEMORY_CP` | `0` | 为 1 时选用内存检查点；mock 模式下也会选用。 |
| `NG_TRANSPORT` | `http-chat` | NG：http-chat 或 http-responses。没有 WebSocket；Responses 会改变所用的 wire API。 |
| `NG_WORKER_COUNT` | `4` | NG 用于 fan-out 的 worker 数量。 |
| `DR_MODEL` | `gpt-5.4-mini` | 两侧真实调用所用的显式模型。 |
| `DR_PLAN_TEMPERATURE` | 未设置 | 两侧 planner 的 temperature；未设置时不发送，因为 `gpt-5.4-mini` 这类 reasoning 模型会拒绝它。 |
| `NG_EXAMPLE_MAX_TOKENS` | `1600` | NG 和 LG 两侧每次调用的输出上限；从不自动调高。 |
| `NEOGRAPH_PG_DSN` | `empty` | NG 的 PostgreSQL DSN；未设置时回退到内存。 |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG 的 PostgreSQL DSN 覆盖值。 |
| `CRAWL4AI_URL` | `empty` | 搜索服务；没有该服务时，除非使用 mock，否则搜索不可用。 |

`OPENAI_API_BASE` 为真实调用选定一个已准入的 origin 或网关前缀；`NG_PROVIDER_DESCRIPTOR` 可以在 NeoGraph 一侧提供完整的描述符。凭据和自定义 CA 应放在运行时选项里（`OPENAI_API_KEY`、`NG_EXAMPLE_CA_FILE`），而不是描述符数据里。NG 默认的路由是 HTTP Chat，与 LG 的 Chat wire API 一致。HTTP Responses 则有意对比另一套 API。能否使用 HTTP/2，取决于 libcurl 和对端；这些 runner 并不证明有多路复用，也不证明连接数固定。

## 历史上的发现：2026-04-26

1. mock 的 LLM、FANOUT=5：记录到的中位数是 NeoGraph 1.0 ms、LangGraph 5.9 ms（该工作负载下的比值为 5.9×）。
2. 第一轮托管模型的测试：NG p50 23.90 s（标准差 5.90 s），LG 21.95 s（标准差 1.23 s）。
3. 历史上的连接诊断，在一次包含七次模型调用的 NG 运行中记录到 21 次 `connect()` 系统调用。仅凭这个数字，并不能证明有 21 个 TLS 会话，也不能证明 HTTP 版本或载荷是等价的。
4. 历史上的连接池改动，即提交 `6da4810` / `bc2ab4f`，与 NG 的 p90 从 35.34 s 降到 25.28 s、标准差从 5.90 s 降到 1.28 s 相关联。这些改动针对的是已经移除的 provider 实现；当前带类型的 SchemaProvider 使用外部的 SDK/libcurl 运行时。
5. 一次 worker 数量的实验，在 FANOUT=50、LLM_MOCK_MS=100、NG_WORKER_COUNT=50 的条件下，报告 NG 为 307 ms，LG asyncio 为 711 ms。这是另一种工作负载，不是通用的服务器容量结论。

以前说 NeoGraph 仍然需要 HTTP/2 支持，这个说法已经过时。这些记录下来的耗时是在新的 SDK 和当前的 Python wheel 之前测得的，不适用于其他平台，也不能用来预测远程推理的加速。mock 模式只测量编排和配置的 sleep；它不产生任何 provider 证据。真实模式会从带类型、持有所有权的 outcome 中提取可见文本，但不对 native 重放、可移植历史的导出，或 provider 上报与预算计费的结算做基准测试。

## 运行新的 cohort

下面的 mock 命令不会发起托管服务的调用，也不涉及持久化。记录新结果时，要同时记录源码、SDK 和 wheel 的修订版本，Python 及各依赖的版本，宿主的限制，worker 数量，预热，迭代次数，检查点模式，以及失败次数。历史文件保持不变。
两个测试脚本都会拒绝空的报告；只要任何一次预热或正式测量的运行失败，就以非零状态退出；
耗时只统计成功的样本。把某个 cohort 报告为通过之前，`Failed runs` 必须为零。
在仓库根目录用 `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py`
运行不涉及 provider 的统计回归测试。

`bench_mock.py` 会在未设置时设置 `LLM_MOCK_MS=0`，并在 import 任何一个 runner 之前，强制使用 mock 搜索和内存检查点。
负的延迟，或者已缓存的非 mock runner，会在分发之前被拒绝；继承下来的凭据无法让它去执行付费的调用。
两个测试脚本都要求正式测量的迭代次数为正数、预热次数不为负。
LG 保留真实的文本块，而不是把空的或富文本响应转成字符串；
中间步骤的回复或计划为空，算作失败，而不是成功的基准样本。
真实调用的路径同样会拒绝被截断、被拒绝（refusal）、工具调用或未知的终止状态，
而不是把它们当作已完成的报告来计时。NG 的错误保留原来持有所有权的 Outcome；
LG 的错误保留原来的响应。不涉及 provider 的回归测试，隔离的是使用方的投影策略；
它们不是传输层/SDK 或托管模型的证据。

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
env -u NG_WORKER_COUNT LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

要观察 fan-out 的重叠情况，用 `LLM_MOCK_MS=100` 重新运行，对比未设置
`NG_WORKER_COUNT`（默认 4）和显式设置 `NG_WORKER_COUNT=1` 两种情况，
同时保持 `FANOUT=5`、预热、迭代次数和检查点模式完全一致。要捕获 stderr
和失败信息；没有出现串行 fan-out 的警告，本身并不证明发生了重叠。
这些是不涉及模型的工作负载，不是付费 provider 的证据。

下面这条托管模型的命令可能产生费用。凭据要有意识地设置；它使用内存检查点。PostgreSQL 的对比需要匹配的 DSN、包的配置，以及单独记录的持久化范围。仅靠系统调用跟踪或抓包，都不能确立语义上的等价，也不能确立 provider 的计费。

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```

这条命令不是花费上限：`FANOUT=5` 时，一次研究查询每一侧最多执行七次逻辑上的模型调用
（plan、五个 researcher、synthesis）。因此，两侧各做两次预热加五次正式测量，最多可以分发 98 次逻辑调用。
当前 NG 的 SDK 默认关闭传输层的重试；LG 显式设置了 `max_retries=0`。
请在基准测试之外，为调用次数以及已配置的各输出上限之和设定限额。
两侧默认都使用 `NG_EXAMPLE_MAX_TOKENS=1600`；显式给出的值会原样采用。
只使用你有权使用的模型、endpoint、凭据和搜索服务。缩小规模的代表性 cohort，
不是历史上多次迭代的测量，必须另行标注。

一个有限的付费 provider 诊断（不是延迟对比）如下：

```sh
# From benchmarks/dr_compare; this makes paid provider calls.
: "${OPENAI_API_KEY:?Requires an intentionally authorized hosted credential}"
env -u NG_WORKER_COUNT LLM_MOCK_MS=-1 MOCK_SEARCH=1 USE_INMEMORY_CP=1 \
  FANOUT=5 NG_TRANSPORT=http-chat NG_EXAMPLE_MAX_TOKENS=8192 \
  python bench.py --only neograph --warmup 0 --iters 1
```

这最多是七次模型调用，以及 57,344 个已配置的输出 token，另加输入 token。
它没有真实的网络搜索，必须标注为“付费 provider / mock 搜索”，而不是真实搜索的运行。
通过的条件是：退出码为 0、有一个成功的样本、报告非空，并且 `Failed runs: 0`；
任何失败的或为空的计划、回答或报告，都算失败。
8,192 个 token 的额度，是明确选定的诊断参数，不是调高后的默认值，不是价格估算，也不是消费授权。
有已获批准、正在运行的 Crawl4AI 服务时，`MOCK_SEARCH=0` 会在同样的模型调用次数上限下，使用真实搜索。
