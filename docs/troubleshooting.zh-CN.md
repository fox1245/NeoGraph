<!-- neograph-i18n: source=docs/troubleshooting.md locale=zh-CN source_sha256=699f30d5019b9acf078e0c623c4fbe4093a8b53bcd84479a100d46ac9c1b8658 -->
# 故障排除

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)


首先是症状，然后是根本原因和解决方法。如果你遇到了不在这里的东西，请打开一个带有该症状的问题——之后它可能会出现在这个列表中。

>**五秒健全性检查。** 首先，确认
>您使用的是最新补丁：
> ```bash
> pip install --upgrade neograph-engine
> python -c "import neograph_engine; print(neograph_engine.__version__)"
> ```
>以下大多数问题已在特定版本中修复。先升级，
>调试第二。

---

## 安装/导入

### `pip install neograph-engine`成功了但是`import`失败

可能是 Python 版本/平台不匹配。我们提供适用于以下平台的车轮：

|平台|版本|
|---|---|
|Linux x86_64 (manylinux_2_34)|Python 3.9 – 3.13|
|Linux aarch64 (manylinux_2_34)|Python 3.9 – 3.13|
|macOS arm64 (14+)|Python 3.9 – 3.13|
|Windows x64（MSVC）|Python 3.9 – 3.13|

该矩阵之外的任何内容都会进入 sdist（源构建），它需要 CMake 3.16+、OpenSSL 和 C++20 工具链。如果您的平台未列出并且源代码构建失败，请提出问题。

### `ImportError: ... GLIBC_2.32 not found`在Linux上

Linux 的轮子是`manylinux_2_34`— 需要 glibc ≥ 2.34（Ubuntu 22.04+、Debian 12+、RHEL9+）。在较旧的发行版上，从源代码构建。

### `ImportError: DLL load failed`在 Windows 上

Windows 轮子附带其自己的依赖项，但 Python 安装必须与轮子架构 (x64) 匹配。确认：

```powershell
python -c "import platform; print(platform.architecture())"
```

如果打印出来`('32bit', ...)`你使用的是 32 位 Python — 安装 64 位 Python。

---

## TLS/ 网络

### 提供商调用挂起 60 秒，然后出现错误`ConnPool::async_post: timeout`

**做作的：**`neograph-engine`车轮 v0.1.0 – v0.1.6。

**根本原因：** 捆绑的 OpenSSL 已编译指向 CA 存储路径`/etc/pki/tls/...`（RHEL习俗）。在 Ubuntu、Debian、macOS 上，CA 商店位于其他地方 (`/etc/ssl/certs/...`），因此wheel的libssl无法验证任何对等证书并且TLS在发生错误之前，握手会默默地等待完整的请求超时。

**修复（≥ v0.1.7）：** 车轮的`__init__.py`现在自动积分`SSL_CERT_FILE`在`certifi.where()`关于进口。升级：

```bash
pip install --upgrade neograph-engine
```

**旧轮子的解决方法：**

```bash
# Debian / Ubuntu
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Cross-distro
export SSL_CERT_FILE=$(python -c "import certifi; print(certifi.where())")
```

**要选择退出 v0.1.7+ 上的自动修复**（例如，您有自定义 CA 捆绑包）：设置`NEOGRAPH_SKIP_CERT_AUTOFIX=1`导入之前。

### `urllib`作品，NeoGraph不

根本原因与上面相同 -`urllib`使用系统 OpenSSL，而 Wheel 使用其捆绑的 OpenSSL 以及错误的 CA 路径。相同修复：升级到 ≥ v0.1.7 或设置`SSL_CERT_FILE`。

### WebSocket回应（`use_websocket=True`) 立即关闭`close=1000`

三种常见原因（按出现频率排列）：

1. **WebSocket您的访问权限未启用APIkey / org.** 一些 OpenAI
1 级账户没有WebSocket- 模式访问尚未。回落至HTTP/SSE通过设置`use_websocket=False`。
2. **丢失的`User-Agent`某些代理路径上的标头。** 已修复
犯罪`d7c61d0`。升级到 ≥ v0.1.4。
3. **`temperature`字段被一些响应拒绝-API型号。** 相同
commit 将其从受支持模型上的 WS 握手中删除。

### CORS从浏览器运行时出现错误WASM

当前 WASM 构建还没有浏览器 CORS 绕过标头或浏览器加载器。请在
[WASM/CORS 问题列表](https://github.com/fox1245/NeoGraph/issues)中查看进展。

---

## 图编译/运行

### `RuntimeError: Unknown reducer: <name>`

绑定随附两个归约器：`"overwrite"`和`"append"`。除非您注册了，否则任何其他内容都无法编译。

**注册一个自定义归约器（来自Python，自 v0.1.9 起）：**

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
```

重新注册一个现有的名称会替换之前的归约器。可调用运行在GIL;并发发送扇出在其上序列化的方式与 Python 自定义节点的方式相同。

如果您输入`"last_value"`（一个常见的LangGraph别名）——那就是`"overwrite"`这里。语义相同，名称不同。

### `RuntimeError: Unknown condition: <name>`

内置条件：`has_tool_calls`, `route_channel`。其他名称必须注册。

**注册自定义条件（来自 Python，自 v0.1.9 起）：**

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

可调用者接收直播`GraphState`（和`state.get(channel)` / `state.get_messages()`可用）并且必须返回与条件边之一匹配的字符串`routes`键。

### `RuntimeError: Write to unknown channel: <name>`

您的频道名称`ChannelWrite`与任何内容都不匹配`definition["channels"]`。频道名称准确；`messages`和`Messages`是不同的。

### `RuntimeError: Unknown node type: <name>`

这`type`您的节点之一的字段引用了工厂注册表中未包含的内容。对于内置（`llm_call`, `tool_dispatch`, `intent_classifier`, `subgraph`) 类型名称已在上面拼写出来。对于您自己的类型，您必须调用`ng.NodeFactory.register_type(type_name, factory)` BEFORE编译。

### 我的ReAct循环只运行一次 -`execution_trace == ['llm']`

**做作的：**`neograph-engine`车轮 v0.1.0 – v0.1.7。

**根本原因：**图形编译器删除了顶层`conditional_edges`默默地阻止。两者都README快速入门和每个 Python 示例都使用这种形式，所以ReAct循环退化为单个循环LLM呼叫（无工具调度）。

**修复（≥ v0.1.8）：**编译器现在接受两种形式 - 顶级`conditional_edges`数组或内联-`edges`与一个`condition`场地。升级并验证：

```python
result = engine.run(...)
print(result.execution_trace)
# Expected for ReAct: ['llm', 'dispatch', 'llm']
```

**旧轮子上的解决方法：** 将条件内联：

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

### `result.execution_trace`为空/仅显示起始节点

该图路由到`__end__`立即地。最常见的原因：

1. **缺少边缘`__start__`.** 每个图至少需要一个
`{"from": ng.START_NODE, "to": "..."}`边缘。
2. **条件返回的值不在`routes`映射中。** 当返回值与任何键都不匹配时，开放条件或未声明输出契约的条件会使用显式`"default"`路由。若它指向`__end__`，图会正常结束。没有`"default"`时，错误信息会包含 source node、条件名和返回的 label。封闭条件一定拒绝声明范围外的 label。
3. **`max_steps=0`或者`max_steps=1`** — 运行达到了上限
立即地。默认为 25；ReAct循环通常需要 10 个以上。

### 编译错误：`RuntimeError: Cycle detected: a -> b -> a`

NeoGraph允许循环（ReAct循环是循环），但编译器捕获*无条件*循环 -`a → b → a`没有条件逃脱。添加可以路由到的条件边`__end__`。

---

## 表现

### 扇出比我预期的要慢

两个常见原因：

1. **没有引擎拥有的工作池。**`compile()`默认为
`set_worker_count(1)`— 无池，扇出分支在调用者的执行器上内联调度并串行运行。之后选择加入池`compile()`（以及之前`run()`）：

   ```python
   engine.set_worker_count(N)        # exact fan-out width
   engine.set_worker_count_auto()    # hardware_concurrency()
   ```

NeoGraph第一次在没有池的情况下运行多发送（或多输出边缘）扇出时，还会打印一次性 stderr 警告，因此静默串行情况是可见的。抑制与`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`如果worker=1快速路径是故意的。
2. **Python自定义节点保存GIL** 在他们的身体期间。如果你的
`@ng.node`函数确实CPU-bound Python 工作，扇出不会加速。ONNX / PyTorch/ 麻木 /`requests.get`释放GIL在本机调用期间，因此它们确实是并行的。对于纯Python评分循环，设置多少个worker并不重要。

### `bench_neograph par`报告 200+ µs

**v1.0 之前的轮子。** v0.1.4–v0.x 将工作池默认设置为`hardware_concurrency()`，它支付了每个扇出滴答声的跨线程提交成本。 v1.0 恢复默认为`set_worker_count(1)`（无池，无提交费用）—`par`回到新鲜的预翻转棒球场`compile()`。选择加入池`engine.set_worker_count(N)` / `engine.set_worker_count_auto()`当您的工作负载的扇出分支实际上受益于真正的线程池时（CPU-束缚体，大扇出宽度）。

### 我的流回调每个节点触发两次

**受影响：** Python`@ng.node`只写节点。固定于`re-agent`提交`2a5c5dc` / `5993935`并复制到NeoGraph掌握。

**v1 之前版本的根本原因：** 纯写入`GraphNode`子类（无`Command`， 不`Send`) 可以为结果运行一次，为流钩子运行一次。升级并实施单`run(NodeInput)`覆盖； v1 调用该方法一次并将可选流接收器公开为`in.stream_cb`。

如果您正在使用`@ng.node`装饰器（不是子类化），这已经被处理了。

---

## 检查点/Postgres

### `PostgresCheckpointStore`未找到/导入错误

PyPI 轮附带`PostgresCheckpointStore`启用（libpq 自 v0.1.3 起捆绑）。`import neograph_engine; neograph_engine.PostgresCheckpointStore`应该直接工作。

如果您从源代码构建而没有`-DNEOGRAPH_BUILD_POSTGRES=ON`，该类将不存在于绑定中。使用设置的标志重新运行 CMake 配置，然后重建。

### Postgres 连接：`FATAL: password authentication failed`

这`PostgresCheckpointStore`连接字符串遵循 libpq：

```
postgresql://user:password@host:port/dbname
```

如果您的密码包含URL- 特殊字符（`@`, `:`, `/`, `%`), URL- 对它们进行编码 - 或使用`key=value`形式：

```
host=localhost user=neo password=p@ss dbname=neograph
```

### 异步 Postgres 重新连接在 30 秒后超时

异步初始/替换连接在整个尝试中使用一个生产安全期限。积极的`connect_timeout=N`直接写在连接字符串中以秒为单位设置全局预算，其中`connect_timeout=1`四舍五入到 PostgreSQL 的最小值两秒。如果显式值不存在、为零或负数，NeoGraph使用30秒。`PGCONNECT_TIMEOUT`服务文件超时值解析得太晚，无法绑定初始异步连接步骤，因此它们也使用 30 秒默认值；当异步截止时间必须不同时，将值直接放入连接字符串中。

预算涵盖多主机连接字符串中的每个主机和已解析的 IP；它不会按主机倍增。这有意与同步 lib​​pq 不同，其中`connect_timeout`分别适用于每个主机。同步`PostgresCheckpointStore`建造和更换不变。

例如，这为完整的异步替换尝试提供了 60 秒的时间：

```
host=pg-a,pg-b dbname=neograph connect_timeout=60
```

### Postgres`relation "neograph_checkpoints" does not exist`

商店在首次使用时创建其表（`CREATE TABLE IF NOT EXISTS`）。如果您的数据库用户没有CREATE权利，手动运行模式 -SQL是在[`include/neograph/graph/postgres_checkpoint.h`](../include/neograph/graph/postgres_checkpoint.h)在下面`kSchema`。

---

## 示例/码头工人

### `docker compose run agent`例如26找不到PG

撰写文件需要一个`db`服务可达`postgres://neograph:neograph@db:5432/neograph`。如果您在 docker-compose 之外，请设置`PG_URL`改为您可以访问的主机。看 [`examples/26_postgres_react_hitl/README.md`](../examples/26_postgres_react_hitl/README.md)完整的环境表。

### Crawl4AI 示例拒绝启动

Crawl4AI 是一个可选的 Docker 容器：

```bash
docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
    unclecode/crawl4ai:latest
```

示例 17、25、26 在以下情况下优雅地回退：`CRAWL4AI_URL`（默认`http://localhost:11235`) 无法访问。

### `example_clay_chatbot`未找到构建目标

示例 11 需要`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`在 CMake 配置时：

```bash
cmake -B build -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON ..
make example_clay_chatbot
```

它拉动 Clay（UI 布局）+ Raylib（渲染器）——这就是它位于标志后面的原因。

---

## 流媒体活动

### `event.node`提高`AttributeError`

属性是`event.node_name`（与 C++ 字段名称匹配）。同样适用于`event.type`（枚举）和`event.data`（这JSON字典）。

```python
def cb(event):
    print(f"{event.type.name} on {event.node_name}: {event.data}")
```

### 我的`StreamMode.TOKENS`回调永远不会触发

提供商必须支持流式传输。现在：

|提供者|流媒体？|
|---|---|
| `OpenAIProvider` | ✓ HTTP/SSE |
| `SchemaProvider("openai_responses")` | ✓ SSE |
| `SchemaProvider("openai_responses", use_websocket=True)` |✓ WS|
| `SchemaProvider("claude")` | ✓ SSE |
|定制Python`Provider`子类|取决于你的`complete_stream`暗示|

对于自定义 Python`Provider`, 覆盖`complete_stream`; Python 子类不公开异步虚拟覆盖。对于新的 C++ 后端，派生自`CompletionProvider`并处理`request.streaming()`在`do_invoke()`。现有的C++`Provider`子类可能会继续覆盖`complete_stream()`或者`complete_stream_async()`。如果没有流实现，默认情况下会将收集到的响应作为一个块而不是增量令牌发出。

---

## OpenTelemetry

### 我的 OTel 范围显示为`parent_id=None`（4 个单独的迹线而不是 1 个）

**做作的：**`neograph_engine.tracing`提交前`9073671`。

**根本原因：**`tracer.start_span` + `use_span(...).__enter__()`依赖于 contextvars，它不会跨 C++ → Python pybind 回调边界传播。

**修复：**`otel_tracer`助手现在通过以下方式快照父上下文`set_span_in_context(root_span)`并将其显式传递给每个子节点`start_span`。升级过去`9073671`。

如果您正在滚动自己的 OTel 集成，请执行相同的操作：不要跨绑定边界依赖上下文变量。

### 我的LLM跨度显示在与我的节点跨度不同的跟踪 ID 下

**做作的：**`neograph_engine.openinference`在 v0.6.0 最终版之前（提交`fa8ed50`）。

**根本原因：**`openinference_tracer`放`parent_ctx`（快照）但从未“附加”节点范围作为 OTel 当前上下文。所以当节点体调用`provider.complete()`和`OpenInferenceProvider`开了一个`llm.complete`跨过孔`tracer.start_as_current_span(...)`，新的跨度回退到全局根，并且跟踪分为每个单独的跟踪 IDLLM称呼。

**使固定：**`openinference_tracer`现在确实`otel_context.attach(set_span_in_context(span))`在`NODE_START`并将生成的令牌存储在跨度旁边；`NODE_END` / `ERROR` / `INTERRUPT`在结束跨度之前分离令牌，恢复之前的当前跨度。在 v0.6.0 中针对 Phoenix 进行了验证 — 单一跟踪树`graph.run > node.X > llm.complete`等级制度。

如果您使用的是 v0.6.0+ 并且*仍然*看到分割痕迹，则您的提供程序没有被包装 - 确保`ctx.provider = OpenInferenceProvider(inner, tracer)`**之前**运行`engine.compile(...)`，否则引擎将绑定到未包装的提供程序。

### `pip install opentelemetry-api`提高ImportError当我导入时`openinference`

`neograph_engine.openinference`惰性导入`opentelemetry`。这ImportError仅在第一次使用时触发，并带有一行安装提示::

pip 安装 opentelemetry-api opentelemetry-sdk

添加`opentelemetry-exporter-otlp`如果你想通过以下方式将跨度推送到 Phoenix / Langfuse / TempoOTLP。

### 我的定制`Tracer`适配器挂起/崩溃/打印垃圾`session.close()`（问题#24）

你写了一个`neograph::observability::Tracer`记录跨度到内存列表中的适配器（C++），然后**在**调用之后遍历该列表`OpenInferenceTracerSession::close()`。步行读取释放的内存。

`close()`重置内部`unique_ptr<Span>`在根跨度（以及每个节点跨度堆栈）上。如果您的适配器将**原始指针**分发到它返回的包装器对象中`start_span`，那些指针此时悬空`close()`返回——包装器由调用者拥有，调用者只是释放它们。

**修复：**适配器必须拥有记录的跨度数据本身，而不仅仅是跟踪调用者拥有的包装器的原始指针。形状：

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

参考：`tests/test_openinference_cpp.cpp::InMemoryTracer`（规范测试夹具）和`examples/49_openinference.cpp::PrintTracer`（stderr-printing demo）都使用这个确切的模式。同样的警告也出现在`@warning`阻止`Tracer`和`OpenInferenceTracerSession::close()`在标题中。

**错误如何出现：** 可观察到的故障模式包括检查循环内的彻底崩溃（最好的情况）、打印跨度名称的中间挂起（释放的缓冲区恰好包含循环字符串格式化程序的内容），或者只是不正确的属性值。这三者的根本原因是相同的。

---

## 构建错误

### GCC13 内部编译器错误：`build_special_member_call`, `cp/call.cc:11096`（问题#23）

您使用的是 Ubuntu 24.04 的库存GCC13（或任何GCC13.x）。构建结束时：

```
internal compiler error: in build_special_member_call, at cp/call.cc:11096
```

...在一条线上`co_await x.foo_async(...)`在协程内部（通常是传递给的 lambda 主体）`asio::co_spawn`从`main()`）。这是一个GCC13 前端bug，不是你的代码。GCC14+、Clang 18+，以及MSVC19.40+ 全部编译相同的源代码不变。

**三种逃生**，按优先顺序排列：

1. **升级编译器** —`sudo apt install gcc-14 g++-14`在
Ubuntu 24.04（24.10 发布GCC默认为 14），那么`cmake -DCMAKE_CXX_COMPILER=g++-14 ...`。最干净的修复；让您以自然的方式编写代码。

2. **通过驱动协程`neograph::async::run_sync`** 而不是
`asio::co_spawn`从`main()`。相同的可观察行为，没有前端ICE：

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

`run_sync`建立自己的私人`io_context`并推动等待完成——内部与什么相同`co_spawn + io.run()`确实如此，但从编译器的角度来看，调用站点是同步的，因此ICE从不火灾。

3. **重构协程**所以`co_await`发生在一个
常规类的成员函数，而不是自由函数或 lambda 体。这在某些情况下有效，但诊断并不总是指向正确的形状变化——选项 1 或 2 更可靠。

**此存储库中的内容：** CMakeLists 有一个针对每个示例的工具链门`example_03`（原来的ICE站点），以及`examples/50_async_tool.cpp`通过使用解决该问题`run_sync`而不是`co_spawn`从`main()`。遵循自然规律的新协程示例/测试`co_spawn`-来自主形状将达到相同的效果ICE在同一个工具链上 - 只需应用选项 1 或 2。

## Python 类型标识 (v0.5.0+)

### `isinstance(params.messages, list)`返回 False

**受影响：** v0.5.0 及更高版本，在五个矢量属性表面上：`CompletionParams.messages`, `.tools`, `ChatMessage.tool_calls`, `NodeResult.writes`, `.sends`。

**为什么：** v0.5.0 修复了静默无操作`params.messages.append(...)`通过将这些向量绑定为不透明类型（`PYBIND11_MAKE_OPAQUE` + `py::bind_vector`） 所以`.append`改变实时 C++ 向量。权衡：该属性的类型现在是例如`ChatMessageList`（一个 pybind 类），而不是一个普通的 Python`list`。

**仍然有效的：**
- `params.messages = [m1, m2]` — `py::implicitly_convertible<py::list, …>`
在赋值时自动转换 Python 列表。
- `for m in params.messages`— 迭代协议。
- `len(params.messages)`, `params.messages[i]`, `params.messages[i] = m`,
切片。
- `params.messages.append(...)`, `.extend(...)`, `.insert(...)`,
`.pop(...)`, `.clear()`— 全部推送至 C++ 矢量直播。

**什么东西坏了（罕见）：**
- `isinstance(x, list) → False`。如果你真的需要一个简单的Python
列出，具体化：`list(params.messages)`。
- `json.dumps(params.messages)`— 绑定类不是直接的
JSON-可串行化。转变：`json.dumps([{"role": m.role, "content": m.content} for m in params.messages])`。

`ChatMessage.image_urls`（`std::vector<std::string>`）*未*迁移 —`vector<string>`在全局绑定中使用得太广泛OPAQUE没有呼叫站点扫描。这`.append()`no-op 仍然作为记录的限制存在； v0.6+ 候选人通过`add_image_url()`方便方法。

---

## 从源代码构建

### CMake配置：`Could NOT find SQLite3`在 Windows 上

Windows 轮子构建集`-DNEOGRAPH_BUILD_SQLITE=OFF`因为 SQLite 不是ABI-兼容跨MSVC运行时。如果您在 Windows 上从源代码构建供自己使用，请通过 vcpkg 安装 SQLite 或通过`-DNEOGRAPH_BUILD_SQLITE=OFF`明确地。

### CMake配置：`Could NOT find CURL`在Linux上

可选的依赖关系。通过包管理器安装：

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev
# RHEL / Fedora
sudo dnf install libcurl-devel
# macOS
brew install curl
```

或禁用：`-DNEOGRAPH_USE_LIBCURL=OFF`。没有 libcurl，`SchemaProvider`的`prefer_libcurl=True`模式 （HTTP/2) 不可用 — 默认ConnPool（HTTP/1.1) 仍然有效。

### Pybind 绑定无法与未定义的引用链接

您可能会重新运行`make`拉取新代码后无需重新运行 CMake。构建目录的编译目标文件引用旧标头中的符号。任何一个`make clean && make`或者删除并重新配置构建目录。

### `OPENAI_API_KEY not set`启动时A2A来自脚本的服务器

`cppdotenv::auto_load_dotenv()`读`.env`在调用它的二进制文件中，但是从启动器脚本分叉的子进程**不**继承启动器尚未导出的任何内容。如果您的脚本执行以下操作：

```bash
./member_server 8101 ...   # forks before any env is set up
```

……每个孩子都看到一个空荡荡的环境，拒绝开始。来源`.env`首先，在启动器中，变量被导出到执行 fork 的 shell 中：

```bash
set -a; . ./.env; set +a            # marks every assignment as exported
./member_server 8101 ... &
```

食谱的`scripts/run_session.sh`显示完整模式并回退到同级`.env`。

### 多角色/多进程A2A：在哪里共享 OpenAI 提供商

使用`OpenAIProvider::create_shared(cfg)`（返回`shared_ptr<Provider>`）而不是`create(cfg)`（返回`unique_ptr`）。共享的形式可以捕获到`NodeFactory`lambda 可以在每个图节点上重用A2A要求 -`create()`的`unique_ptr`会迫使你手动`release()`并重新包装。

---

## C++ 消费者 —`httplib.h`宏观一致性（承载、问题#16）

如果您构建一个 **链接到的 C++ 应用程序NeoGraph** AND还`#include <httplib.h>`在您自己的翻译单元中（例如运行您自己的`httplib::Server` SSE端点），每个 TU 包括`<httplib.h>` MUST `#define CPPHTTPLIB_OPENSSL_SUPPORT`**包含之前**。即使在一个 TU 中缺少宏也会默默地产生一个SEGV里面`getaddrinfo`第一次`SchemaProvider::complete_stream`击中LLM端点。

### 为什么会发生这种情况

`cpp-httplib`仅包含标题。班级`httplib::ClientImpl`**有条件地更大**时`CPPHTTPLIB_OPENSSL_SUPPORT`被定义（它获得SSL- 相关成员；布局移动约 8 个字节）。因为图书馆的功能都`inline`，链接器为每个内联函数保留一个实例化并丢弃重复项。如果二进制文件中的两个 TU 针对不同的编译`ClientImpl`布局（因为一个定义了宏，另一个没有），链接器选择一个定义； *其他* TU 的编译端访问错误偏移处的成员 — 经典ODR违反。腐败发生在邻近领域（例如`proxy_host_`最终从实际的偏移量中读取`path_`的尾巴），以及`httplib::ClientImpl::create_client_socket`使用通配符分支到“使用代理”路径`proxy_host_.c_str()` → `getaddrinfo` → `internal_strlen` → SEGV。

### 症状

在阿桑的领导下：

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

没有 ASan：通过 gdb 实现相同的堆栈，带有一个*可以*看起来像文本的野指针值（它是错误偏移槽中的任何字节 - 在 ASan 下，它通常是`0xBE`检疫毒物；如果没有 ASan，它可能是未初始化的堆栈内容，恰好解码为JSON / UTF-8 碎片并且“看起来”像是真正的罪魁祸首造成的内存损坏 - 这是误导性的症状）。

### 使固定

在每个 TU 中，包括`<httplib.h>`：

```cpp
// your main.cpp / sse_handler.cpp / wherever
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
```

或者在 CMake 中全局（首选 - 保证整个目标的一致性）：

```cmake
target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
```

即使您自己的 httplib 使用只需要，该宏也是无害的`Server`（不是`SSLClient`) — 它仅**添加**成员；没有什么需要你实际做的SSL在你身边。

### 如何在没有 ASan 的情况下进行审计

```bash
grep -rn 'include.*httplib\.h\|CPPHTTPLIB_OPENSSL_SUPPORT' src/
```

如果有包含网站`<httplib.h>`前面**没有**`#define CPPHTTPLIB_OPENSSL_SUPPORT`（在同一个 TU 中，或通过编译标志定义），这几乎肯定是错误。

### 为什么NeoGraph无法为您解决此问题

NeoGraph自己的 .cpp 文件都一致地定义了宏。仅当下游 TU 也拉入 httplib.h *没有*宏时，才会发生违规。在编译时检测到这一点需要 (a)NeoGraph暴露`httplib::ClientImpl`在公共标头中（我们故意不这样做——httplib 留在里面`SchemaProvider.cpp`), 或 (b) 链接时间`static_assert`跨翻译单元的结构大小，C++ 不支持。记录陷阱是我们能做的最好的事情；这部分是文档。问题#16关闭。

---

## 构建拓扑的工具/编辑器JSON从发动机漂移

### 症状

你编写（或使用）了一个生成器，GUI，或发出的可视化块编辑器NeoGraph拓扑结构JSON。它提供了节点类型、归约器或条件，然后引擎会拒绝`compile()`和`Unknown node type:` / `Unknown reducer:` / `Unknown condition:`——或者你默默画下的一根树枝永远不会着火。

### 为什么会发生这种情况

该工具的调色板是手工维护的，落后于NeoGraph版本实际链接。分支案例是经典的顶级案例`conditional_edges`回归（在 v0.1.0–v0.1.7 中默默删除，在 v0.1.8 中修复）——发出该块的工具必须验证它是否能够在加载程序→编译往返过程中幸存下来。

### 使固定

不要手动维护调色板。引擎会发出一个机器可读的模式，它所接受的模式正是它所接受的——将工具固定到它上面：

- C++：`neograph::graph::NodeFactory::instance().export_schema()`。
- CLI：`./example_export_schema > schema.json`
（`examples/52_export_schema.cpp`）。
- Python：`neograph_engine.export_schema()`→ 字典。

该文件载有`neograph_version`;让该工具将其与其缓存的架构进行比较，并在不匹配时发出警告。`node_types`反映注册的内容`NodeFactory`在调用时，因此*在*导出之前注册您的自定义节点类型/归约器/条件，就像之前一样`compile()`。 （背景：问题#56.)

---

## 严格的拓扑验证

### 症状

`compile()`投掷`strict topology validation failed (schema_version 1)`列出键，例如`$: unknown or unconsumed key 'conditionnal_edges'`, `nodes.X.barrier: 'wait_for' is missing or empty`， 或者`translation validation failed: compiled graph does not round-trip`。

### 为什么会发生这种情况

你的拓扑声明`"schema_version": 1`，它选择严格编译：编译器拥有的每个对象的每个键都必须由解析器“消耗”。无人使用的密钥几乎总是拼写错误（`conditionnal_edges`, `max_retry`, `promt`）或引擎将**默默地丢弃**的构造 - v0.1.0–v0.1.7 背后的故障模式`conditional_edges`回归。往返（翻译验证）错误意味着编译后的图重新发出为JSON不再与您的输入匹配：编译器丢失或重新连接了某些内容，并且该消息准确列出了内容。

### 使固定

- 修复列出的键 - 每个错误都带有其JSON小路。
- 注释和编辑器元数据属于注释命名空间：
键开头为`_`或者`x-`（例如。`_comment`, `x-studio-pos`) 始终被允许且从未被验证。
- 屏障需要非空`wait_for`大批;内联条件
边缘路线通过`routes`，所以一个`to`就死了——将目标移动到`routes`或丢弃它。
- *使用*已声明的配置模式注册的自定义节点类型
(3-精氨酸`register_type`）在封闭世界中进行检查；添加`"additionalProperties": true`到模式以选择退出类型。
- 要退回到历史宽松的解析，请删除
`schema_version`— 然后再次忽略未知的键，并且往返不匹配仅在 stderr 上发出警告。新文件应保持严格。

### 兼容性时间线

- 所有 `0.x` 版本都让缺失 `schema_version` 或版本为 0 的文档继续使用宽松兼容
  路径；`0.x` 更新不会悄悄把它们重新解释为严格文档。
- 新定义、内置图工厂和持续维护的示例声明当前版本
  （`TOPOLOGY_SCHEMA_VERSION`，当前为 `1`）。
- 计划中的 `1.0.0` 边界会用迁移诊断拒绝缺失版本或版本为 0 的输入，而不是
  静默改变其路由或解析语义。
- C++ 输入使用 `GraphCompiler::upgrade_to_latest()`，Python 输入使用
  `ng.upgrade_topology()`。被旧版忽略的数据会保存在避免名称冲突的
  `x-upgraded-*` 注释中。严格 Core JSON 是保留的互操作工件；JavaScript
  源应通过 QuickJS `define()` 重新编译。

---

## 报告错误

如果您的症状不是以上：

1. 跑步`pip install --upgrade neograph-engine`首先——许多问题是
补丁级修复。
2. 捕获最小重现器：
   - 图定义
   - 使用的节点类型
   - 确切的`engine.run(...)`称呼
   - 这`result.execution_trace`以及（如果流式传输）您看到的事件
3. 请记下您的平台、Python 版本，以及`neograph_engine.__version__`。
4. 在以下位置打开问题<https://github.com/fox1245/NeoGraph/issues>。

如果错误仅针对特定的LLM端点，还请包括线级形状（`example_responses_envelope`对于 OpenAI 响应；`tcpdump`/`wireshark`对于生的HTTP跟踪（如果相关）。
