<!-- neograph-i18n: source=docs/python-binding.md locale=zh-CN source_sha256=aa37c77f9c0d5edbcefc44cab95fa0bbd0eaf928751367baa9e4091c7cbbf592 -->
# Python 绑定

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)


NeoGraph随附为`pip`-可安装的Python包，因此相同的C++引擎可以驱动LangGraph来自 Jupyter 笔记本、Gradio 应用程序或 FastAPI 服务的风格工作流程：
```bash
pip install neograph-engine
```

## 五秒演示（无API密钥）

证明安装有效的最短的事情 - 一个装饰器定义的节点，运行它，读取输出：
```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {"name":     {"reducer": "overwrite"},
                 "messages": {"reducer": "append"}},
    "nodes":    {"greet": {"type": "greet"}},
    "edges":    [{"from": ng.START_NODE, "to": "greet"},
                 {"from": "greet",       "to": ng.END_NODE}],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))

print(result.output["channels"]["messages"]["value"])
# [{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

## ReAct代理有真实的LLM
```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider

class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", description="multiply by 2",
        parameters={"type":"object","properties":{"x":{"type":"number"}}})
    def execute(self, args):  return str(args["x"] * 2)

ctx = ng.NodeContext(
    provider=OpenAIProvider(api_key="sk-..."),
    tools=[CalcTool()],
    instructions="Use `calc` for arithmetic.",
)

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "react",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}, "dispatch": {"type": "tool_dispatch"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"}, {"from": "dispatch", "to": "llm"}],
    "conditional_edges": [{"from": "llm", "condition": "has_tool_calls",
                           "routes": {"true": "dispatch", "false": ng.END_NODE}}],
}
engine = ng.GraphEngine.compile(definition, ctx)
result = engine.run(ng.RunConfig(thread_id="t1",
    input={"messages": [{"role": "user", "content": "What is 21 * 2?"}]},
    max_steps=10))
```

## 读取输出

`engine.run(...)`返回一个`RunResult`与这些字段：

|场地|类型|意义|
|---|---|---|
| `output` | `dict` |最终状态——`{"channels": {...}, "global_version": int}`。使用`output["channels"][name]["value"]`读取频道。|
| `max_steps_exhausted` | `bool` | `True`仅当步骤上限停止执行而可运行的工作仍然存在时。|
| `interrupted` | `bool` | `True`如果运行暂停在`interrupt_before` / `interrupt_after` / `NodeInterrupt`。|
| `interrupt_node` | `str` |触发中断的节点名称（当`interrupted`）。|
| `interrupt_value` | `dict` | `{"reason": str, "type": "NodeInterrupt", "value": ...}`对于动态中断（`"value"`仅当节点附加有效负载时才存在），或`{"message": ...}`对于静态的`interrupt_before` / `interrupt_after`。|
| `checkpoint_id` | `str` |运行期间保存的最新检查点 ID，仅供参考；`resume_async()`按`thread_id`恢复，而不是按检查点 ID 恢复。|
| `execution_trace` | `list[str]` |节点名称按执行顺序排列——对于调试路由很有用。|

`RunConfig` 对应 LangGraph 的 `RunnableConfig` 概念。

要异步恢复中断的运行，请传入线程 ID，并可选地传入人工回复：

```python
result = await engine.resume_async(thread_id="t1", resume_value=answer)
```

|场地|默认|意义|
|---|---|---|
| `thread_id` |必需的|对话/会话标识符——保持检查点流分离。|
| `input` | `{}` |初始通道值 - 键必须与图表的匹配`channels`定义。|
| `max_steps` |50|超级台阶吊顶；ReAct循环通常需要 10 个以上。|
| `stream_mode` | `StreamMode.OFF` |位掩码：`EVENTS \| TOKENS \| DEBUG \| VALUES \| UPDATES \| ALL`. Only consulted by `运行流` / `run_stream_async`。|
| `resume_if_exists` | `False` |什么时候`True`并且配置了检查点存储，运行加载最新的检查点`thread_id`（如果有）并适用`input`通过通道归约器在顶部 - 多轮聊天，无需手动线程化先前状态`input`。默认保留新启动语义以实现向后兼容；为了HITL从中断的运行中恢复，使用`engine.resume_async()`反而。|
| `cancel_token` | `None` |选修的`CancelToken`合作取消。之前分配一个`engine.run()`，然后调用`token.cancel()`来自另一个Python线程。引擎停在下一个超步边界处；长时间工作的节点应该进行轮询`input.ctx.cancel_token`。|
```python
token = ng.CancelToken()
config = ng.RunConfig(thread_id="job-42", input={"query": "..."})
config.cancel_token = token

# Run engine.run(config) in a worker thread, then request cancellation from
# the caller thread when the request disconnects or the user presses Stop.
token.cancel()
```

## 从 Python 节点暂停人类

`interrupt_before`图形定义中的暂停在您编写图形时选择的节点处。这不能表达人机交互实际存在的情况，因为某个步骤是否危险取决于模型刚刚要求的内容：

>*“代理想要运行`rm -rf build/`。允许？”*

为此，节点本身决定——它提出`NodeInterrupt`，附上需要批准的内容。引擎检查点并让您恢复正常`RunResult`（没有人向你提出任何问题），你的答案将返回到提出问题的节点：
```python
import neograph_engine as ng

class ApprovalNode(ng.GraphNode):
    def run(self, input):
        # The human's answer. None until someone has actually answered — which
        # is how you tell "nobody has looked yet" from "the answer was no".
        verdict = input.ctx.resume_value

        if verdict is None:
            raise ng.NodeInterrupt(
                {"tool": "shell", "cmd": "rm -rf build/"},
                reason="shell command needs approval")

        if not verdict.get("approved"):
            return [ng.ChannelWrite("result", "refused")]
        return [ng.ChannelWrite("result", "done")]

    def get_name(self):
        return "risky"
```
```python
result = engine.run(cfg)

if result.interrupted:
    print(result.interrupt_node)               # "risky"      — which node paused
    print(result.interrupt_value["reason"])    # for a human to read
    print(result.interrupt_value["value"])     # for your code to branch on

    result = engine.resume(cfg.thread_id, {"approved": True})   # the answer
```

`NodeInterrupt(reason)`使用纯字符串也可以，并且省略 `"value"` 键。您提出的任何其他问题都将始终是一个错误：节点中的错误会导致运行失败，而不是看起来像人类的问题。

需要检查点存储——否则无法恢复。

## 跨对话记住用户——存储

检查点记住**单次对话**。存储则记住**用户**，跨所有对话。
```python
store = ng.InMemoryStore()
engine.set_store(store)

class Greet(ng.GraphNode):
    def run(self, input):
        seen = input.ctx.store.get(["users", "u1"], "visits")
        n = (seen.value["n"] if seen else 0) + 1
        input.ctx.store.put(["users", "u1"], "visits", {"n": n})
        return [ng.ChannelWrite("greeting", f"visit #{n}")]
```

命名空间是层次化列表，因此`store.search(["users"])`查找每个用户下的所有内容，并且`store.search(["users", "u1"])`查找一个用户的项目。`get()`回报`None`对于错过——缺席是一个答案，而不是一个错误。

子类`ng.Store`将其放入数据库中。

自定义检查点持久性的工作方式相同：子类`ng.CheckpointStore`并实施`save`, `load_latest`, `load_by_id`, `list`， 和`delete_thread`。可选的`put_writes`, `get_writes`， 和`clear_writes`方法默认为无操作/全超步重放行为。里面的价值观`StoreItem`, `Checkpoint`， 和`PendingWrite`是普通的PythonJSON形状（`dict`, `list`、字符串、数字、布尔值和`None`）。

## 在 429 上退缩 —RateLimitedProvider
```python
from neograph_engine.llm import RateLimitedProvider, OpenAIProvider

provider = RateLimitedProvider(OpenAIProvider(...), max_retries=5)
engine = ng.GraphEngine.compile(definition, ng.NodeContext(provider=provider))
```

没有它你最终会包裹起来`engine.run()`在您自己的重试循环中，该循环会重试**整个图表** - 重新运行已成功的每个节点。这会重试一次HTTP请求失败。

它尊重上游`Retry-After`当有一个时，回落到`default_wait_seconds`如果没有，则限制单次睡眠时间`max_wait_seconds`，并放弃一次`max_total_wait_seconds`睡眠时间已累积（`0`= 无总上限）。

您自己的提供商通过提出正确的例外来选择加入：
```python
class MyProvider(ng.Provider):
    def complete(self, params):
        r = requests.post(...)
        if r.status_code == 429:
            raise ng.RateLimitError(
                "rate limited",
                retry_after_seconds=int(r.headers.get("Retry-After", -1)))
```

您提出的任何其他内容仍然是错误。

## 在运行图表之前检查它 -`validate`
```python
report = ng.validate(definition)
if report.has_errors():
    print(report.summary())
    for d in report.errors():
        print(d.code, d.path, d.message)
```

悬垂边缘、无法到达的节点、死亡障碍——您可以阅读的报告，而不是找出何时`compile()`抛出。

值得了解的一个优势：`validate()`首先编译定义，因此节点类型无人将表面注册为**异常**，而不是诊断。在验证之前注册您的节点类型，就像在编译之前一样。

**节点级别的重试不需要类。** Put`"retry_policy": {...}`在图形定义中，引擎尊重它——这在 Python 中一直有效：
```python
definition["retry_policy"] = {"max_retries": 5, "initial_delay_ms": 100}
```

## MCP— 使用远程工具服务器
```python
client = ng.mcp.MCPClient("http://localhost:8931")     # or ["python", "server.py"]
client.initialize()

engine = ng.GraphEngine.compile(
    definition, ng.NodeContext(tools=client.get_tools()))
```

这就是整个整合。`get_tools()`返回服务器的目录作为图表可以调度的工具，您可以在同一目录中将它们与您自己的 Python 工具自由混合`NodeContext`。

`client.call_tool(name, args)`在任何图之外直接调用一个。

**当服务器重叠时它们会重叠。**MCP工具是网络往返，这是并发调度付费的情况，并且`MCPTool`是真正的C++`AsyncTool`。HTTP使用并发请求； stdio 帧写入并关联无序回复JSON-RPCID：

|运输|3 次调用 × 0.4 秒|
|---|---|
| HTTP |**0.41 s** — 每个调用都是它自己的请求|
|标准输入输出|**~0.4 s** 使用并发服务器 — 一个管道，请求 ID 多路复用|

串行处理请求的子进程仍然需要大约 1.2 秒。多路复用消除了客户端瓶颈；它无法创建服务器端并发。

初始化是自动的`get_tools()`和`call_tool()`，以及一个明确的`initialize()`仍然有效且幂等。`get_initialize_result()`公开协商的协议、功能、服务器信息和指令。`get_tool_definitions()`跟随所有分页光标并保留完整MCP元数据。使用`call_tool_result()`或者`MCPTool.execute_result()`当你需要的时候`structured_content`、非文本块、`is_error`， 或者`_meta`; `call_tool()`是源兼容的原始JSON正面。

当对会话的最后一个引用（客户端或其生成的任何工具）被删除时，stdio 子进程将终止。

可运行，离线（它启动自己的MCP服务器）： [`examples/26_mcp_tools.py`](../bindings/python/examples/26_mcp_tools.py)。

## 使工具同时运行

当模型一次性要求使用多种工具时，NeoGraph将他们一起派遣。它们是否实际上“重叠”是工具的选择：
```python
class Fetch(ng.AsyncTool):          # ng.Tool -> serial;  ng.AsyncTool -> overlaps
    def execute(self, arguments):
        return requests.get(arguments["url"]).text
    ...
```

经过测量，有 20 个工具，每个工具等待 300 毫秒：

|工具基类|墙钟时间|
|---|---|
| `ng.Tool` |6.0秒|
| `ng.AsyncTool` |**0.30 秒** (19.9×)|

**为什么选择加入。** 同步`Tool`在下一个开始之前运行完成 - 因此，保持状态的现有工具不会突然发现自己正在与自身的副本竞争。并发是你声明的东西，而不是发生在你身上的东西。另一方面：两次调用同一个`AsyncTool`可以立即飞行（模型可能在一回合中要求两次），因此将每次调用状态保留在堆栈上，而不是在`self`。

**明确说明的边界。** Python 函数保存GIL当它运行时。你的工具只有在“不”持有它时才与它的同级工具重叠——即当它在 I/O 上被阻塞时，因为那时 CPython 就会放手。一个HTTP调用、套接字读取、数据库查询、`time.sleep`：全部释放，全部重叠。

会燃烧的工具CPU**在Python中**持有GIL对于它的整个主体并且不会重叠，无论它有多少个线程：

|3CPU-边界`AsyncTool`s|3.1× 1 的时间|
|---|---|

声明这样一个工具`AsyncTool`什么也不买。 （如果繁重的工作发生在 numpy、C 扩展或子进程内部，则GIL在那里发布并且确实重叠。）这是通过测试确定的，因此该声明不能悄悄地漂移。

并发性受内部工作池限制——默认为 32 个线程，或者`NEOGRAPH_TOOL_THREADS`。他们将时间花在 I/O 上，因此慷慨的池成本很低。

可运行，离线：[`examples/25_async_tools.py`](../bindings/python/examples/25_async_tools.py)。

## 门控工具调用——“代理想要运行`rm -rf build/`。允许？”

*模型要求工具 X* 和 *工具 X 运行* 之间有一个钩子，它返回三个判断之一：
```python
def gate(call, gctx):
    if call.name not in DANGEROUS:
        return ng.ToolDecision.allow()

    # None until a human has actually answered — which is how the gate tells
    # "nobody has been asked yet" from "the answer was no", and so avoids
    # asking the same question forever.
    if gctx.resume_value is None:
        return ng.ToolDecision.interrupt(
            f"{call.name} needs approval",
            {"tool": call.name, "arguments": call.arguments})

    if gctx.resume_value.get("approved"):
        return ng.ToolDecision.allow()
    return ng.ToolDecision.deny("the operator refused this command")

engine.set_tool_gate(gate)
```
```python
result = engine.run(cfg)
if result.interrupted:
    print(result.interrupt_value["reason"])   # "shell needs approval"
    print(result.interrupt_value["value"])    # {"tool": ..., "arguments": ...}
    result = engine.resume(cfg.thread_id, {"approved": True})
```

|判决|影响|
|---|---|
| `ToolDecision.allow()` |运行它。|
| `ToolDecision.allow({...})` |使用这些参数运行它——这是环境值（租户、线程、凭证）被注入的地方，而不是每个工具都知道它们。|
| `ToolDecision.deny(reason)` |不要运行它。原因可以追溯到模型作为工具的结果，因此它可以适应而不是在下一轮再次要求相同的工具。|
| `ToolDecision.interrupt(reason, payload)` |不要运行它，并暂停整个运行。有效载荷表面位于`RunResult.interrupt_value["value"]`。|

许可、审计、参数重写和每次调用中断不是四个功能；他们是戴着四顶帽子的原始人。

**该门在任何工具运行之前都会看到每个调用，并且该顺序就是设计。** 假设模型要求`list_files`和`shell`一起，并且仅`shell`需要批准。当跑步暂停时，`list_files`也**没有**运行——尽管大门允许。

这并非疏忽。`resume()`从顶部重新进入节点，因为中断的节点没有记录写入。有`list_files`已经运行，批准将运行它*第二次-将其交换为`git commit`和提示`rm -rf`刚刚双重承诺。如果人类说**不**，则任何已执行的操作都无法撤消。 “被拒绝”并不意味着“什么也没发生”的许可系统不是许可系统。

两个实用注意事项：

- **需要检查点存储。**中断必须是可恢复的；没有
没有什么可以恢复的存储。
- **闸门位于引擎上，而不是`RunConfig`.** `resume()`建立自己的
`RunConfig`因此，一旦人类回答了它提出的提示，每次运行的门就会消失，然后危险的工具就会不受控制地运行。在引擎上设置一次后，每次运行和恢复都会保持不变。

可端到端运行，否API密钥： [`examples/24_tool_approval_gate.py`](../bindings/python/examples/24_tool_approval_gate.py)。

## 内置减速机

通道需要一个归约器——新写入如何与现有值相结合。目前内置两个：

|归约器|行为|典型用途|
|---|---|---|
| `"overwrite"` |新价值取代旧价值。|单值通道：`name`, `current_question`，中间划痕。|
| `"append"` |新列表连接到现有列表。|对话历史记录、中间结果、任何您想要跨节点积累的内容。|

自定义归约器从 Python 注册（自 v0.1.9 起）：
```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)

# Now `"reducer": "sum"` works in your channel definitions.
```

条件路由的相同模式 -`ng.ConditionRegistry.register_condition("name", fn)`在哪里`fn(state) -> str`返回路由键之一。

## 绑定涵盖什么内容

- **引擎接口** —`GraphEngine.compile / run / run_stream / run_async / run_stream_async / resume / resume_async / get_state / get_state_history / update_state / fork`, `RunConfig`, `RunResult`, `set_worker_count`, `set_checkpoint_store`, `set_node_cache_enabled`。
- **自定义 Python 节点** — 子类 `neograph_engine.GraphNode`，通过 `NodeFactory.register_type` 或 `@neograph_engine.node` 装饰器注册。引擎在 GIL 下正确调度，包括来自扇出工作线程的调度。
- **自定义 Python 工具** — 子类`neograph_engine.Tool`, 传入`NodeContext(tools=[...])`。引擎在编译时取得所有权。
- **异步** — 每个`*_async`绑定返回一个`asyncio.Future`绑定到调用线程的运行循环。流回调通过以下方式跳转到循环线程`loop.call_soon_threadsafe`因此回调会在 asyncio 期望的地方运行。
- **检查点** — Python 后端可以继承 `CheckpointStore`，也可以直接使用 `InMemoryCheckpointStore`。使用 `-DNEOGRAPH_BUILD_POSTGRES=ON` 构建绑定后，还可以使用 `PostgresCheckpointStore`；受支持的 wheel 会随扩展一起打包 libpq。
- **OpenAI 回应结束WebSocket** — `SchemaProvider(schema="openai_responses", use_websocket=True)`。

轮子：Linux x86_64 (manylinux_2_34)、Linux aarch64 (manylinux_2_34)、macOS arm64 (14+)、Windows x64 (MSVC），对于 Python 3.9 → 3.13。 **20个轮子
+ 每个版本的 sdist** 通过 cibuildwheel。

看 [`bindings/python/examples/`](../bindings/python/examples/)对于完整的示例索引 - 最小图，ReAct, HITL、意图路由、异步、多代理辩论、JSON图往返，以及带有深入研究子图的 Gradio 聊天（Crawl4AI + Postgres 可选）。

## 与的差异LangGraph（Python 绑定）

音高是“LangGraph对于 C++”，但有一些语义与LangGraphPython——在这里出现，这样你就不会在端口中间碰到它们：

- **多圈`thread_id`已选择加入** —`engine.run(cfg)`与
相同的`thread_id`默认情况下**不**自动加载上一回合的检查点；每次跑步都从新开始`cfg.input`。放`cfg.resume_if_exists = True`为LangGraph-风格“加载最新的，在顶部应用输入”行为。默认为`False`所以已经通过线程状态的调用者`input`他们自己不受影响。请参阅`RunConfig`上表。
- **`update_state`接受字典或列表`ChannelWrite`** —
`update_state(thread_id, channel_writes, as_node='')`采用两种形状之一`channel_writes`：
  - 字典：`{"messages": [...]}`— 直接键入形式，最接近
到LangGraph的`values={...}`（kwarg 名称不同）。
  - 列表：`[ChannelWrite("messages", [...]), ...]`— 对称于
每个节点体发出什么。

列表项按顺序应用，包括重复通道，并保留 `ChannelWrite.Mode.OVERWRITE`。
dict 形式仍对每个键值执行 reducer 写入。其他类型会引发 `TypeError`，而不是静默 no-op
（由 item #5 关闭的 v0.3.2 之前陷阱）。
- **`get_state(thread_id)`返回一个嵌套字典 -`get_state_view`
是扁平帮手** —`state["channels"]["messages"]["value"]`是规范的原始形状（跨版本稳定）。对于符合人体工程学的点访问，请使用`view = engine.get_state_view(thread_id)`并阅读`view.messages`, `view.scratch`等直接。`view.raw`为需要版本/元数据的调用者公开未扁平化的字典。子类`StateView`带有用于类型化访问的声明字段（Pydantic v2）：`class ChatState(ng.StateView): messages: list[dict] = []`然后`engine.get_state_view(thread_id, model=ChatState)`。
- **Python`Provider`子类仅绑定同步`complete`和
`complete_stream`方法** - 异步虚函数不绑定在 Python 用户定义的 Provider 子类上，因此自定义 Python 提供程序通过同步条目提供服务。对于异步本机提供程序集成（HTTP/2 多路复用，与其他协程真正重叠），保留在 C++ 中并派生自`neograph::CompletionProvider`那里。
- **单行令牌发出** — `from neograph_engine.streaming import
发行令牌`, then `emit_token(cb, self._name, 令牌)` inside a streaming node. Replaces the 4-line `GraphEvent` 建筑仪式。
- **可观察性内置提供，而不是作为单独的 SaaS** — 对
`neograph_engine.tracing.otel_tracer`（供应商中立的 OTel 跨度）`neograph_engine.openinference.OpenInferenceProvider` + `openinference_tracer`（LLM-shape属性键），指向一个OTLP任何出口商OpenInference- 感知后端（Phoenix、Arize、Langfuse — 全部）OSS，全部可自托管），然后您就得到了LangSmithUX（每回合聊天气泡，DAG层次结构、提示/响应捕获、每次调用令牌计数和成本），无需供应商 SaaS 合同。
  ```bash
  docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
  pip install neograph-engine opentelemetry-exporter-otlp
  ```
  ```python
  from opentelemetry import trace
  from opentelemetry.sdk.trace import TracerProvider
  from opentelemetry.sdk.trace.export import BatchSpanProcessor
  from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
  from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer

  trace.set_tracer_provider(TracerProvider())
  trace.get_tracer_provider().add_span_processor(
      BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
  tracer = trace.get_tracer("my-app")

  wrapped = OpenInferenceProvider(OpenAIProvider(api_key=...), tracer)
  ctx = ng.NodeContext(provider=wrapped)
  engine = ng.GraphEngine.compile(graph, ctx)
  with openinference_tracer(tracer) as cb:
      engine.run_stream(cfg, cb)
  # → http://localhost:6006 renders the trace as a LangSmith-style chain.
  ```

LangGraph主办的LangSmith是该生态系统中典型的可观测路径；LangFuse/ 凤凰是OSS替代品，但需要集成胶。NeoGraph的`OpenInferenceProvider`*是*集成粘合剂——插入，每个`Provider.complete()`成为一个LLM自动跨度。
- **单节点法** —`def run(self, input)`被介绍于
**v0.4.0** 是从 **v0.9.0** 开始的唯一自定义节点覆盖。读取状态来自`input.state`，来自的实时取消句柄`input.ctx.cancel_token`，流接收器（或`None`） 从`input.stream_cb`。返回一个`list[ChannelWrite]`, `list[Send]`, 一个`Command`，或一个`NodeResult`。看 [`migration-v0.4-to-v1.0.md`](migration-v0.4-to-v1.0.md)搬家时`execute*`节点。
- **两个Python deps，句号** —`pip install neograph-engine`
拉`certifi`和`pydantic>=2.0`这就是整个运行时依赖树。图形引擎、调度程序、检查点存储、HTTP/WebSocket客户，MCP/A2A/ACP传输、OpenAI 兼容提供程序和 Postgres/SQLite 检查点后端都是原生 C++ 内置的。比较LangGraph的传递运行时：`langgraph` → `langchain-core` → `langchain` → `langchain-community`（每个快速移动包），加上每个集成包（`langchain-openai`, `langchain-anthropic`, `langchain-postgres`, `langchain-chroma`，……）。这就是为什么一个工作LangGraph脚本在 6 个月后中断——Pydantic v1→v2 在 2024 年打破了世界，导入路径在每个次要版本中发生变化。NeoGraph的 Python 表面是冻结的 C++ 之上的薄薄的 pybind11 层ABI在语义版本控制下。针对 v0.4.x 编写的自定义节点`execute*`兼容性窗口必须迁移到`run(input)`; v0.9.0 在 v1 准备期间删除了旧节点表面。
- **部署不需要 Docker** - 的直接结果
上面的单深树。生产LangChain有效部署*需要* Docker + 完全固定的`requirements.txt`;如果没有它，传递包在下一次部署时的无声小碰撞可能会在运行时导致服务器停机。NeoGraph的轮子附带了完整的本机运行时，因此：

  - `pip install neograph-engine`在裸机上/VPS/一个
无服务器功能有效 - 主机的其他 Python 包无法访问NeoGraph的C++引擎。
  - 容器镜像可以是 **alpine + musl + ~20 MB** (engine .so +
Python 解释器 + 2 deps)，或 **~1.2 MB** 的静态链接 C++ 二进制文件`libc.so.6`作为唯一的动态部门。
  - 无服务器（Lambda、Cloud Run）上的冷启动是 ms 级的，而不是
秒——没有LangChain导入图形进行行走。
  - 锁文件维护负担几乎为零。`pydantic>=2.0`是
这是唯一可能发生变化的限制，您会在安装时看到它，而不是在生产中的凌晨 3 点。
