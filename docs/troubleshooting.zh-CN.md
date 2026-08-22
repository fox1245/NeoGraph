<!-- neograph-i18n: source=docs/troubleshooting.md locale=zh-CN source_sha256=ac341ae5a04c54a36f6e1d4e165be17f5cf789b924e015626c3a01c9c3a447b9 -->
# 故障排查

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

先列症状，再讲根因和修复。如果你遇到这里没有的问题，请用症状开一个 issue——之后它很可能会被加到这个列表里。

> **五秒快速检查。**在做其他事情之前，请确认
> 你当前使用的是最新补丁版本:
> ```bash
> pip install --upgrade neograph-engine
> python -c "import neograph_engine; print(neograph_engine.__version__)"
> ```
> 下面的大多数问题都在特定版本中已修复。请先升级版本，
> 调试第二步。

---

## 安装 / 导入

### `pip install neograph-engine` 成功但 `import` 失败

可能是 Python 版本 / 平台不匹配。我们为以下版本提供 wheels：

| 平台 | 版本 |
|---|---|
| Linux x86_64 (manylinux_2_34) | Python 3.9 – 3.13 |
| Linux aarch64 (manylinux_2_34) | Python 3.9 – 3.13 |
| macOS arm64（14+） | Python 3.9 – 3.13 |
| Windows x64 (MSVC) | Python 3.9 – 3.13 |

此矩阵之外的任何内容都会回退到 sdist（源码构建），这需要 CMake 3.16+、OpenSSL 和 C++20 工具链。如果你的平台未在列表中列出且源码构建失败，请提交问题。

### `ImportError: ... GLIBC_2.32 not found` 在 Linux 上

Linux wheel 是 `manylinux_2_34`——需要 glibc ≥ 2.34（Ubuntu 22.04+、Debian 12+、RHEL 9+）。在更旧的发行版上，请从源码构建。

### `ImportError: DLL load failed` 在 Windows 上

Windows wheel 自带其依赖项，但 Python 安装必须匹配 wheel 架构 (x64)。请确认：

```powershell
python -c "import platform; print(platform.architecture())"
```

如果它打印 `('32bit', ...)`，说明你用的是 32 位 Python——请安装 64 位版本。

---

## TLS / 网络

### Provider 调用挂起 60 秒，然后报错 `ConnPool::async_post: timeout`

**受影响：** `neograph-engine` wheels v0.1.0 – v0.1.6。

**根因：** 捆绑的 OpenSSL 编译时内置的 CA 存储路径指向 `/etc/pki/tls/...`（RHEL 约定）。在 Ubuntu、Debian、macOS 上，CA 存储位于别处（`/etc/ssl/certs/...`），因此 wheel 的 libssl 无法验证任何对端证书，TLS 握手会静默等待完整的请求超时后才报错。

**修复（≥ v0.1.7）：** wheel 的 `__init__.py` 现在在导入时自动将 `SSL_CERT_FILE` 指向 `certifi.where()`。请升级：

```bash
pip install --upgrade neograph-engine
```

**较旧 wheel 版本的变通方案：**

```bash
# Debian / Ubuntu
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Cross-distro
export SSL_CERT_FILE=$(python -c "import certifi; print(certifi.where())")
```

**要在 v0.1.7+ 上选择退出自动修复**（例如你有自定义 CA 包）：请在导入前设置 `NEOGRAPH_SKIP_CERT_AUTOFIX=1`。

### `urllib` 可用，NeoGraph 不可用

根本原因同上——`urllib` 使用系统 OpenSSL，而 wheel 使用其捆绑的 OpenSSL，且 CA 路径错误。修复方法相同：升级到 ≥ v0.1.7 或设置 `SSL_CERT_FILE`。

### WebSocket 响应（`use_websocket=True`）立即以 `close=1000` 关闭

三个常见原因，按出现频率排序：

1. **你的 API key / 组织未启用 WebSocket 访问。** 某些 OpenAI tier 1 账户尚不具备 WebSocket 模式访问权限。通过设置 `use_websocket=False` 回退到 HTTP/SSE。
2. 某些代理路径上缺少 `User-Agent` 头。已在提交 `d7c61d0` 中修复。升级到 ≥ v0.1.4。
3. **`temperature` 字段被某些 Responses-API 模型拒绝。** 同一提交在受支持的模型上将其从 WS 握手中移除。

### 通过 WASM 从浏览器运行时出现 CORS 错误

WASM 构建尚未实现用于浏览器 CORS 的绕过头。跟踪 [WASM/CORS 问题](https://github.com/fox1245/NeoGraph/issues) 以获取状态。

---

## 图编译/运行

### `RuntimeError: Unknown reducer: <name>`

绑定中附带两个 reducer：`"overwrite"` 和 `"append"`。除非你已注册，否则其他任何内容都无法编译。

**注册自定义归约器（自 v0.1.9 起，从 Python 注册）：**

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
```

重新注册现有名称将替换之前的归约器。可调用对象在 GIL 下运行；并发 Send fan-out 会在其上序列化，与 Python 自定义节点的行为相同。

如果你输入了 `"last_value"`（一个常见的 LangGraph 别名）——在这里是 `"overwrite"`。语义相同，名称不同。

### `RuntimeError: Unknown condition: <name>`

内置条件：`has_tool_calls`、`route_channel`。其他名称必须注册。

**注册自定义条件（从 Python 中，自 v0.1.9 起）：**

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

可调用对象接收实时的 `GraphState`（带有可用的 `state.get(channel)` / `state.get_messages()`），并且必须返回一个与条件边的 `routes` 键之一匹配的字符串。

### `RuntimeError: Write to unknown channel: <name>`

你的 `ChannelWrite` 中的通道名称与 `definition["channels"]` 中的任何内容都不匹配。通道名称是精确的；`messages` 和 `Messages` 是不同的。

### `RuntimeError: Unknown node type: <name>`

你某个节点的 `type` 字段引用了工厂注册表中不存在的内容。对于内置类型（`llm_call`、`tool_dispatch`、`intent_classifier`、`subgraph`），类型名称已在上方拼写出来。对于你自己的类型，你必须在编译之前调用 `ng.NodeFactory.register_type(type_name, factory)`。

### 我的 ReAct 循环只运行一次——`execution_trace == ['llm']`

**受影响：** `neograph-engine` wheels v0.1.0 – v0.1.7。

**根本原因：** 图编译器静默丢弃了顶层 `conditional_edges` 块。README 快速入门和每个 Python 示例都使用此形式，因此 ReAct 循环退化为单次 LLM 调用（无工具分发）。

**修复（≥ v0.1.8）：** 编译器现在接受两种形式——顶层 `conditional_edges` 数组或内联在 `edges` 中并带有 `condition` 字段。升级并验证：

```python
result = engine.run(...)
print(result.execution_trace)
# Expected for ReAct: ['llm', 'dispatch', 'llm']
```

**旧版 wheels 的变通方案：** 将条件内联放置：

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "llm",
     "condition": "has_tool_calls",
     "routes": {"true": "dispatch", "false": ng.END_NODE}},
]
# (no separate conditional_edges block)
```

### `result.execution_trace` 为空 / 仅显示起始节点

图立即路由到 `__end__`。最常见的原因：

1. **缺少来自 `__start__` 的边。** 每个图至少需要一条 `{"from": ng.START_NODE, "to": "..."}` 边。
2. **条件返回的值不在 `routes` 映射中。** 当条件的返回值与任何键都不匹配时，开放或未指定的条件使用显式的 `"default"` 路由。如果该路由映射到 `__end__`，则正常退出。没有 `"default"` 时，路由会抛出包含源节点、条件和返回标签的错误。封闭条件始终拒绝其声明集合之外的标签。
3. **`max_steps=0` 或 `max_steps=1`** — 运行立即达到上限。默认值为 25；ReAct 循环通常需要 10 次以上。

### 编译错误: `RuntimeError: Cycle detected: a -> b -> a`

NeoGraph 允许循环（ReAct 循环是循环），但编译器会捕获 *无条件* 循环 — 没有条件逃逸的 `a → b → a`。添加一条可以路由到 `__end__` 的条件边。

---

## 性能

### Fan-out 比预期慢

两个常见原因：

1. **没有引擎拥有的工作线程池。** `compile()` 默认为 `set_worker_count(1)` — 没有池，fan-out 分支在内联分发后于调用者的执行器上串行运行。在 `compile()` 之后（且在 `run()` 之前）选择一次池：

   ```python
   engine.set_worker_count(N)        # exact fan-out width
   engine.set_worker_count_auto()    # hardware_concurrency()
   ```

NeoGraph 还会在第一次在没有池的情况下运行多 Send（或多出边）fan-out 时打印一次性 stderr 警告，因此静默串行的情况是可见的。如果 worker=1 快速路径是有意为之，则使用 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` 抑制。
2. **Python 自定义节点在其主体期间持有 GIL。** 如果您的 `@ng.node` 函数执行 CPU 密集型 Python 工作，fan-out 不会加速。ONNX / PyTorch / numpy / `requests.get` 在原生调用期间释放 GIL，因此它们确实可以并行化。对于纯 Python 评分循环，无论您设置多少工作线程都没有关系。

### `bench_neograph par` 报告 200+ µs

**v1.0 之前的 wheel。** v0.1.4–v0.x 将工作线程池默认值保持在 `hardware_concurrency()`，这会在每次 fan-out 时支付跨线程提交成本。v1.0 将默认值恢复为 `set_worker_count(1)`（无池，无提交成本）— `par` 在全新的 `compile()` 上回到了翻转前的水平。当您的工作负载的 fan-out 分支确实受益于真正的线程池（CPU 密集型主体、大 fan-out 宽度）时，使用 `engine.set_worker_count(N)` / `engine.set_worker_count_auto()` 选择启用一个池。

### 我的流式回调触发两次

**受影响：** Python `@ng.node` 只写节点。在 `re-agent` 提交 `2a5c5dc` / `5993935` 中修复，并在 NeoGraph master 中复现。

**v1 之前版本的根本原因：** 纯写 `GraphNode` 子类（没有 `Command`，没有 `Send`）可能为结果运行一次，为流 Hook 运行一次。升级并实现单个 `run(NodeInput)` 覆盖；v1 调用该方法一次，并将可选的流接收器暴露为 `in.stream_cb`。

如果您使用 `@ng.node` 装饰器（而非子类化），这一点已得到处理。

---

## 检查点 / Postgres

### `PostgresCheckpointStore` 未找到 / 导入错误

PyPI wheels 中已启用 `PostgresCheckpointStore`（自 v0.1.3 起捆绑了 libpq）。`import neograph_engine; neograph_engine.PostgresCheckpointStore` 应可直接使用。

如果未启用 `-DNEOGRAPH_BUILD_POSTGRES=ON` 而从源码构建，则该类在绑定中不存在。请重新运行带该标志的 CMake 配置，然后重新构建。

### Postgres 连接：`FATAL: password authentication failed`

`PostgresCheckpointStore` 连接字符串遵循 libpq：

```
postgresql://user:password@host:port/dbname
```

如果密码包含 URL 特殊字符（`@`、`:`、`/`、`%`），请进行 URL 编码——或使用 `key=value` 形式：

```
host=localhost user=neo password=p@ss dbname=neograph
```

### 异步 Postgres 重连在 30 秒后超时

异步初始/替换连接使用单一的生产安全期限来覆盖整个尝试过程。直接写在连接字符串中的正数 `connect_timeout=N` 以秒为单位设置该全局预算，其中 `connect_timeout=1` 向上取整至 PostgreSQL 的最小两秒。如果显式值缺失、为零或为负，NeoGraph 将使用 30 秒。`PGCONNECT_TIMEOUT` 和服务文件超时值的解析时间太晚，无法约束初始异步连接步骤，因此它们也使用 30 秒默认值；当异步截止时间必须不同时，请将值直接放入连接字符串中。

该预算涵盖多主机连接字符串中的每个主机和已解析 IP；它不是按主机相乘的。这与同步 libpq 有意不同，在同步 libpq 中 `connect_timeout` 分别应用于每个主机。同步 `PostgresCheckpointStore` 构造和替换不变。

例如，这为完整的异步替换尝试提供了 60 秒：

```
host=pg-a,pg-b dbname=neograph connect_timeout=60
```

### Postgres `relation "neograph_checkpoints" does not exist`

存储会在首次使用（`CREATE TABLE IF NOT EXISTS`）时创建其表。如果您的数据库用户没有 CREATE 权限，请手动运行架构——SQL 在 [`include/neograph/graph/postgres_checkpoint.h`](../include/neograph/graph/postgres_checkpoint.h) 的 `kSchema` 下。

---

## 示例 / docker

### `docker compose run agent` 例如 26 无法找到 PG

compose 文件期望 `db` 服务可通过 `postgres://neograph:neograph@db:5432/neograph` 访问。如果你不在 docker-compose 环境中，请将 `PG_URL` 设置为你的可访问主机。完整的环境变量表请参见 [`examples/26_postgres_react_hitl/README.md`](../examples/26_postgres_react_hitl/README.md)。

### Crawl4AI 示例拒绝启动

Crawl4AI 是一个可选的 Docker 容器：

```bash
docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
    unclecode/crawl4ai:latest
```

示例 17、25、26 在 `CRAWL4AI_URL`（默认 `http://localhost:11235`）不可访问时优雅地回退。

### 未找到 `example_clay_chatbot` 构建目标

示例 11 在 CMake 配置时需要 `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`：

```bash
cmake -B build -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON ..
make example_clay_chatbot
```

它引入了Clay（UI布局）+ Raylib（渲染器）——这就是它位于标志之后的原因。

---

## 流式事件

### `event.node` 引发 `AttributeError`

该属性为 `event.node_name`（与 C++ 字段名匹配）。`event.type`（枚举）和 `event.data`（JSON 字典）同理。

```python
def cb(event):
    print(f"{event.type.name} on {event.node_name}: {event.data}")
```

### 我的 `StreamMode.TOKENS` 回调从未触发

提供者必须支持流式传输。目前：

| 提供者 | 流式传输? |
|---|---|
| `OpenAIProvider` | ✓ HTTP/SSE |
| `SchemaProvider("openai_responses")` | ✓ SSE |
| `SchemaProvider("openai_responses", use_websocket=True)` | ✓ WS |
| `SchemaProvider("claude")` | ✓ SSE |
| 自定义 Python `Provider` 子类 | 取决于你的 `complete_stream` 实现 |

对于自定义 Python `Provider`，重写 `complete_stream`；Python 子类不暴露异步虚方法覆盖。对于新的 C++ 后端，从 `CompletionProvider` 派生并处理 `request.streaming()` 中的 `do_invoke()`。现有的 C++ `Provider` 子类可以继续重写 `complete_stream()` 或 `complete_stream_async()`。如果没有流式实现，默认会将收集到的响应作为单个块发出，而不是增量令牌。

---

## OpenTelemetry

### 我的 OTel 跨度显示为 `parent_id=None`（4 个单独的跟踪而不是 1 个）

**受影响：** `neograph_engine.tracing` 在提交 `9073671` 之前。

**根本原因：** `tracer.start_span` + `use_span(...).__enter__()` 依赖 contextvars，而 contextvars 不会跨 C++ → Python pybind 回调边界传播。

**修复：** `otel_tracer` 辅助函数现在通过 `set_span_in_context(root_span)` 快照父上下文，并将其显式传递给每个子节点的 `start_span`。升级到 `9073671` 以上。

如果您在自行构建 OTel 集成，做法相同：不要跨绑定边界依赖 contextvars。

### 我的 LLM 追踪片段在不同的追踪 ID 下显示，与我的节点追踪片段不同

**受影响：** `neograph_engine.openinference` 在 v0.6.0 最终版之前（提交 `fa8ed50`）。

**根本原因：** `openinference_tracer` 设置了 `parent_ctx` （一个快照），但从未将节点 span *附加* 为 OTel 当前上下文。因此，当节点主体调用 `provider.complete()` 且 `OpenInferenceProvider` 通过 `llm.complete` 打开了一个 `tracer.start_as_current_span(...)`span 时，新 span 回退到全局根，导致每次 LLM 调用的 trace 被分割成独立的 trace ID。

**修复：** `openinference_tracer` 现在对 `otel_context.attach(set_span_in_context(span))` 执行 `NODE_START` 并将生成的令牌与跨度一起暂存； `NODE_END` / `ERROR` / `INTERRUPT` 在结束跨度前分离令牌，恢复之前的当前跨度。已在 v0.6.0 中针对 Phoenix 验证——单一跟踪树，具有 `graph.run > node.X > llm.complete` 层级。

如果你在 v0.6.0+ 上*仍然*看到拆分跟踪，说明你的提供者没有被包装——确保 `ctx.provider = OpenInferenceProvider(inner, tracer)` 在 `engine.compile(...)` **之前**运行，否则引擎会绑定到未包装的提供者。

### `pip install opentelemetry-api` 在我导入 `openinference` 时引发 ImportError

`neograph_engine.openinference` 延迟导入 `opentelemetry`。ImportError 仅在首次使用时触发，并带有一行安装提示：：

    pip install opentelemetry-api opentelemetry-sdk

添加 `opentelemetry-exporter-otlp` 如果你想通过 OTLP 将 span 推送到 Phoenix / Langfuse / Tempo。

### 我的自定义 `Tracer` 适配器在 `session.close()` 之后挂起 / 崩溃 / 打印垃圾数据（issue #24）

你编写了一个 `neograph::observability::Tracer` 适配器（C++），将 span 记录到内存列表中，然后在调用 `OpenInferenceTracerSession::close()` **之后**遍历该列表。遍历读取了已释放的内存。

`close()` 重置根 span 上的内部 `unique_ptr<Span>`（以及每个节点 span 的栈）。如果你的适配器从 `start_span` 返回的包装器对象中分发了**原始指针**，那么这些指针在 `close()` 返回的那一刻就悬空了——包装器由调用者拥有，而调用者刚刚释放了它们。

**修复：**适配器必须自行拥有记录的 span 数据，而不是只跟踪指向调用方拥有的 wrapper 的原始指针。形状如下：

```cpp
// Owned by the tracer (lives until tracer drops):
struct RecordedSpan {
    std::string name;
    RecordedSpan* parent = nullptr;
    std::map<std::string, std::string> attrs;
    // ...status, events, ended flag...
};

// Owned by the OpenInference layer (may be reset on close):
class WrapperSpan : public obs::Span {
    RecordedSpan* rec_;        // pointer into the tracer-owned data
public:
    void set_attribute(...) override { rec_->attrs[...] = ...; }
    // ...
};

class MyTracer : public obs::Tracer {
    std::vector<std::unique_ptr<RecordedSpan>> records_;  // ← owns data
    // start_span builds a fresh RecordedSpan, returns a Wrapper
    // pointing at it. Walk records_ for inspection — never the
    // wrappers.
};
```

参考文献： `tests/test_openinference_cpp.cpp::InMemoryTracer` （规范测试夹具）和 `examples/49_openinference.cpp::PrintTracer` （stderr 打印演示）都使用此确切模式。相同的警告位于 `@warning` 块中，在 `Tracer` 和 `OpenInferenceTracerSession::close()` 的头部。

**Bug 如何显现：** 可观察的失败模式包括：inspection loop 内部干净的崩溃（best case）、打印 span name 中途 hang（freed span 缓冲区中恰好含有可能会循环的 string formatter 的内容）、或只是读取 incorrect attribute values）。这三者的 root cause 相同。

---

## 构建错误

### GCC 13 内部编译器错误：`build_special_member_call`，`cp/call.cc:11096`（问题 #23）

你在Ubuntu 24.04自带的GCC 13（或任何GCC 13.x）上。构建失败，报错如下：

```
internal compiler error: in build_special_member_call, at cp/call.cc:11096
```

…在协程内执行 `co_await x.foo_async(...)` 的行上（通常是传递给 `asio::co_spawn` 的 lambda 体，来自 `main()`）。这是 GCC 13 前端错误，不是你的代码问题。GCC 14+、Clang 18+ 和 MSVC 19.40+ 都能编译相同的源代码而无需修改。

**三种逃脱方式**，按优先顺序排列：

1. **升级编译器** —— 在 Ubuntu 24.04 上使用 `sudo apt install gcc-14 g++-14`（24.10 默认自带 GCC 14），然后 `cmake -DCMAKE_CXX_COMPILER=g++-14 ...`。最干净的修复方案；让你能以自然的方式编写代码。

2. **通过 `neograph::async::run_sync`** 而不是 `asio::co_spawn` 从 `main()` 驱动协程。相同的可观察行为，无前端 ICE：

   ```cpp
   // Instead of:
   asio::co_spawn(io,
       [&]() -> asio::awaitable<void> {
           result = co_await tool.execute_async(args);   // ← GCC 13 ICEs here
       },
       asio::detached);
   io.run();

   // Do:
   #include <neograph/async/run_sync.h>
   result = neograph::async::run_sync(tool.execute_async(args));
   ```

`run_sync` 构建自己的私有 `io_context` 并将可等待对象驱动至完成——在内部与 `co_spawn + io.run()` 所做的完全相同，但从编译器的角度来看调用点是同步的，因此 ICE 永远不会触发。

3. **重构协程**，使 `co_await` 发生在普通类的成员函数内部，而不是自由函数或 lambda 体中。这在某些情况下有效，但诊断信息并不总是指向正确的形状变更——选项 1 或 2 更可靠。

**本仓库中此问题的影响：** CMakeLists 中有一个按示例划分的工具链门控，围绕 `example_03` （原始 ICE 位置），并且 `examples/50_async_tool.cpp` 通过使用 `run_sync` 替代 `co_spawn` （来自 `main()`）来规避此问题。遵循自然的 `co_spawn`-from-main 形态的新协程示例/测试将在同一工具链上遇到相同的 ICE — 只需应用选项 1 或 2。

## Python 类型恒定性（v0.5.0+）

### `isinstance(params.messages, list)` 返回 False

**受影响范围：** v0.5.0 及更高版本，涉及五个向量属性接口：`CompletionParams.messages`、`.tools`、`ChatMessage.tool_calls`、`NodeResult.writes`、`.sends`。

**原因：** v0.5.0 修复了 `params.messages.append(...)` 上的静默无操作问题，通过将这些向量绑定为不透明类型（`PYBIND11_MAKE_OPAQUE` + `py::bind_vector`），以便 `.append` 修改实时的 C++ 向量。权衡之处在于：该属性的类型现在例如为 `ChatMessageList` （一个 pybind 类），而不是普通的 Python `list`.

**仍然可用的功能：**
- `params.messages = [m1, m2]` —— `py::implicitly_convertible<py::list, …>` 在赋值时自动转换 Python 列表。
- `for m in params.messages`——迭代协议。
- `len(params.messages)`、`params.messages[i]`、`params.messages[i] = m`、切片。
- `params.messages.append(...)`、`.extend(...)`、`.insert(...)`、`.pop(...)`、`.clear()`——全部实时推送至 C++ 向量。

**已破坏的功能（罕见）：**
- `isinstance(x, list) → False`。如果你确实需要一个普通的 Python 列表，请物化：`list(params.messages)`。
- `json.dumps(params.messages)` — 绑定的类不能直接进行 JSON 序列化。请转换：`json.dumps([{"role": m.role,
  "content": m.content} for m in params.messages])`。

`ChatMessage.image_urls` (`std::vector<std::string>`) *未*被迁移 — `vector<string>` 在绑定中使用过于广泛，无法在不进行调用点清扫的情况下全局使用 OPAQUE。`.append()` 的 no-op 仍保留在那里，作为文档化的限制；v0.6+ 候选方案通过 `add_image_url()` 便捷方法实现。

---

## 从源码构建

### CMake 配置：Windows 上的 `Could NOT find SQLite3`

当前Windows wheel已启用SQLite，并捆绑与其匹配的运行时DLL。自定义源码构建应通过同一套vcpkg/MSVC工具链安装SQLite。如果确实不需要SQLite，可以传递`-DNEOGRAPH_BUILD_SQLITE=OFF`；不要混用面向不兼容MSVC运行时构建的DLL。

### CMake 配置：Linux 上的 `Could NOT find CURL`

可选依赖。通过您的包管理器安装：

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev
# RHEL / Fedora
sudo dnf install libcurl-devel
# macOS
brew install curl
```

或者禁用：`-DNEOGRAPH_USE_LIBCURL=OFF`。没有 libcurl，`SchemaProvider` 的 `prefer_libcurl=True` 模式（HTTP/2）不可用——默认的 ConnPool（HTTP/1.1）仍然可以工作。

### Pybind binding 绑定失败，出现未定义引用

你可能在拉取新代码后重新运行 `make`，但没有重新运行 CMake。构建目录中编译的对象文件引用了旧头文件中的符号。要么 `make clean && make`，要么删除并重新配置构建目录。

### 从脚本启动 A2A 服务器时的 `OPENAI_API_KEY not set`

`cppdotenv::auto_load_dotenv()` 读取调用它的二进制文件内部的 `.env`，但从启动脚本 fork 出的子进程**不会**继承启动器尚未导出的任何内容。如果你的脚本执行：

```bash
./member_server 8101 ...   # forks before any env is set up
```

…每个子进程都会看到空环境并拒绝启动。先在启动器中执行 source `.env`，以便变量被导出到执行 fork 的 shell 中：

```bash
set -a; . ./.env; set +a            # marks every assignment as exported
./member_server 8101 ... &
```

食谱中的 `scripts/run_session.sh` 展示了完整模式，并回退到同级 `.env`。

### 多-persona / 多-process A2A: 共享一个OpenAI provider 的位置

使用 `OpenAIProvider::create_shared(cfg)`（返回 `shared_ptr<Provider>`）而不是 `create(cfg)`（返回 `unique_ptr`）。共享形式可捕获为 `NodeFactory` lambda，并可在每个图节点和 A2A 请求中复用——`create()` 的 `unique_ptr` 会迫使你手动 `release()` 并重新包装。

---

## C++ 消费者——`httplib.h` 宏一致性（承重，issue #16）

如果你构建一个**链接 NeoGraph** 的 C++ 应用程序，并且还在自己的翻译单元中 `#include <httplib.h>`（例如运行你自己的 `httplib::Server` SSE 端点），那么每个包含 `<httplib.h>` 的 TU 都必须在 include **之前** `#define CPPHTTPLIB_OPENSSL_SUPPORT`。即使只有一个 TU 缺少该宏，也会在 `getaddrinfo` 中首次 `SchemaProvider::complete_stream` 命中 LLM 端点时静默产生 SEGV。

### 为何发生

`cpp-httplib` 是仅头文件的。类 `httplib::ClientImpl` 在定义 `CPPHTTPLIB_OPENSSL_SUPPORT` 时**有条件地更大**（它增加了SSL相关成员；布局偏移约8字节）。由于库的所有函数都是 `inline`，链接器为每个内联函数保留一个实例并丢弃重复项。如果二进制中的两个翻译单元针对不同的 `ClientImpl` 布局进行编译（因为一个定义了宏，另一个没有），链接器会选择其中一个定义；*另一个*翻译单元的编译侧会以错误的偏移访问成员——这是典型的ODR违规。损坏落在相邻字段上（例如， `proxy_host_` 最终从实际是 `path_`尾部的偏移处读取），并且 `httplib::ClientImpl::create_client_socket` 分支进入“使用代理”路径，带有野指针 `proxy_host_.c_str()` → `getaddrinfo` → `internal_strlen` → SEGV。

### 症状

在 ASan 下：

```
==NNNN==ERROR: AddressSanitizer: SEGV on unknown address
    #0 internal_strlen (...)
    #1 getaddrinfo
    #2 httplib::detail::create_socket
    #3 httplib::detail::create_client_socket
    #4 httplib::ClientImpl::create_client_socket
    #5 httplib::SSLClient::create_and_connect_socket
    ...
    #N neograph::llm::SchemaProvider::complete_stream
```

没有 ASan：通过 gdb 得到相同堆栈，通配指针值*可能*看起来像文本（它只是错误偏移槽中的任何字节——在 ASan 下通常是 `0xBE` 隔离区毒药；没有 ASan 时，它可能是未初始化的堆栈内容，恰好解码为 JSON / UTF-8 片段，*看起来*像是来自真实罪魁祸首的内存损坏——这就是误导性症状）。

### 修复

在每个包含 `<httplib.h>` 的 TU 中：

```cpp
// your main.cpp / sse_handler.cpp / wherever
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
```

或在CMake中全局设置（首选——保证整个目标的一致性）：

```cmake
target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
```

即使你自己的 httplib 使用只需要 `Server`（而不是 `SSLClient`），该宏也是无害的——它只是**添加**成员；没有任何东西要求你实际在你的端做 SSL。

### 如何在没有ASan的情况下审计

```bash
grep -rn 'include.*httplib\.h\|CPPHTTPLIB_OPENSSL_SUPPORT' src/
```

如果 `<httplib.h>` 的任何包含位置**没有**在（同一 TU 中，或通过编译标志定义）前面加上 `#define CPPHTTPLIB_OPENSSL_SUPPORT`，那几乎可以肯定是这个 bug。

### 为什么NeoGraph不能为你修复这个

NeoGraph 自己的 .cpp 文件都一致地定义了该宏。违规只发生在下游 TU 也拉入 httplib.h *而没有*该宏时。在编译时检测到这一点需要 (a) NeoGraph 在公共头文件中暴露 `httplib::ClientImpl`（我们故意不这样做——httplib 保持在 `SchemaProvider.cpp` 内部），或 (b) 跨翻译单元对结构体大小进行链接时 `static_assert`，而 C++ 不支持这一点。记录这个陷阱是我们能做的最好的；本节就是文档。Issue #16 已关闭。

---

## 一个构建拓扑JSON的工具/编辑器与引擎漂移

### 症状

你编写（或使用）了一个生成器、GUI 或可视化块编辑器，它发出 NeoGraph 拓扑 JSON。它提供了一个节点类型、reducer 或条件，然后引擎在 `compile()` 处用 `Unknown node type:` / `Unknown reducer:` / `Unknown condition:` 拒绝它——或者你绘制的一个分支静默地永远不会触发。

### 为何发生

该工具的调色板是手工维护的，落后于实际链接的 NeoGraph 版本。分支情况是经典的顶层 `conditional_edges` 回归（在 v0.1.0–v0.1.7 中静默丢弃，v0.1.8 修复）——发出该块的工具必须验证它能在加载器→编译往返中存活。

### 修复

不要手工维护调色板。引擎会发出一个机器可读的schema，精确说明它接受什么——将工具固定到它：

- C++：`neograph::graph::NodeFactory::instance().export_schema()`。
- CLI: `./example_export_schema > schema.json` (`examples/52_export_schema.cpp`).
- Python: `neograph_engine.export_schema()` → dict。

文档带有`neograph_version`；让工具将其与缓存的模式进行比较，并在不匹配时发出警告。`node_types` 反映的是调用时在`NodeFactory`中注册的任何内容，因此请在导出*之前*注册你的自定义节点类型/归约器/条件，就像在`compile()`之前所做的那样。（背景：issue #56。）

---

## 严格的拓扑验证

### 症状

`compile()` 抛出`strict topology validation failed (schema_version 1)`，列出诸如`$: unknown or unconsumed key 'conditionnal_edges'`、`nodes.X.barrier: 'wait_for' is missing or empty`或`translation validation failed: compiled graph does not round-trip`之类的键。

### 为何发生

您的拓扑声明了`"schema_version": 1`，这使其进入严格编译模式：编译器拥有的每个对象的每个键都必须被解析器*消费*。未被消费的键几乎总是拼写错误（`conditionnal_edges`、`max_retry`、`promt`）或引擎会**静默丢弃**的构造——这正是 v0.1.0–v0.1.7 `conditional_edges` 回归背后的失败模式。往返（翻译-验证）错误意味着重新以 JSON 形式输出的已编译图不再与您的输入匹配：编译器丢失或重新连接了某些内容，而消息恰好列出了这些内容。

### 修复

- 修复所列键——每个错误都携带其 JSON 路径。
- 注释和编辑器元数据属于注释命名空间：以`_`或`x-`开头的键（例如`_comment`、`x-studio-pos`）始终允许且从不验证。
- 屏障需要非空的`wait_for`数组；内联条件边通过`routes`路由，因此其上的`to`是无效的——将目标移入`routes`或将其删除。
- 注册了声明配置模式（3 参数`register_type`）的自定义节点类型以封闭世界方式检查；向模式添加`"additionalProperties": true`以将类型排除在外。
- 要回退到历史宽松解析，请移除`schema_version`——未知键随后再次被忽略，往返不匹配仅在 stderr 上警告。新文档应保持严格。

### 兼容时间线

- 每个`0.x`版本都保留缺失或零`schema_version`文档在宽松兼容路径上。没有`0.x`更新会静默地将它们重新解释为严格文档。
- 新定义、内置图工厂和维护的示例声明当前版本（`TOPOLOGY_SCHEMA_VERSION`，当前为`1`）。
- 计划的`1.0.0`边界拒绝缺失或零版本，并给出迁移诊断，而不是静默更改其路由或解析语义。
- 使用`GraphCompiler::upgrade_to_latest()`升级C++输入，或使用`ng.upgrade_topology()`升级Python输入。被忽略的遗留数据在collision-safe的`x-upgraded-*`注释下保留。严格的Core JSON是保留的交换工件；JavaScript源码应通过QuickJS `define()`重新编译。

---

## 报告 bug

如果你的症状不在上述之列：

1. 先运行`pip install --upgrade neograph-engine`——许多问题都是补丁级别的修复。
2. 捕获最小复现器：
   - 图定义
   - 使用的节点类型
   - 确切的`engine.run(...)`调用
   - `result.execution_trace`以及（如果流式传输）你看到的事件
3. 注意你的平台、Python版本和`neograph_engine.__version__`。
4. 在<https://github.com/fox1245/NeoGraph/issues>处打开一个问题。

如果该bug仅针对特定的LLM端点出现，请同时包含线路级形状（对于OpenAI Responses为`example_responses_envelope`；如果相关，对于原始HTTP跟踪为`tcpdump`/`wireshark`）。
