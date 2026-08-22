<!-- neograph-i18n: source=docs/python-binding.md locale=zh-CN source_sha256=61dd8227b6a8807710fb014cacdf14a64257a18b35778a30981f34bd1eefb35f -->
# Python 绑定

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` 是同一 C++ 运行时的 pybind11 接口。该 wheel 支持 Core、LLM、Program/QuickJS、MCP 和 SQLite 运行时持久化；可选源码构建仅暴露其编译的组件。

```bash
pip install neograph-engine
```

## Core 图快速入门

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages", [
        {"role": "assistant", "content": f"Hello, {state.get('name')}!"}
    ])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

## Core API 对等性

Python 暴露的是 C++ 执行能力，而非独立的 Python 调度器：

- 同步与 asyncio 的 run/stream/resume；
- 精确检查点 `resume_from`、fork、状态检查与有序状态写入；
- 通过图中断和 `NodeInterrupt` 实现静态与动态 HITL；
- `RunMetadata` 截止时间、trace/run 标识与模型 token 上限；
- 图级与节点级 `RetryPolicy`，包括抖动；
- 执行局部或可显式复用的 `CacheScope`；
- 检查点与长期 Store 后端；
- 自定义节点、reducer、条件、provider 与工具；
- 工具门控、执行策略、强制生命周期 Hook 与严格运行时介入。

### 对等性契约

这里的“对等性”是指Python使用相同的原生执行路径和安全契约，而不是把每个内部C++存储类型或权限类型原样复制到Python中。

| 功能 | 原生C++路径 | Python接口 | 状态 |
|---|---|---|---|
| 编译并执行Core图 | `GraphEngine` | `GraphEngine.compile`、run/stream/async方法 | 相同的调度器和运行时 |
| 运行时标识、截止时间与预算 | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | 每次运行使用相同的值 |
| 重试与节点缓存策略 | `RetryPolicy`, `CacheScope` | 图/节点setter与缓存作用域 | 相同的运行时策略 |
| 检查点、HITL与时间旅行 | 检查点Store与恢复API | 恢复、精确`resume_from`、分叉、状态历史/更新 | 相同的检查点契约 |
| Program编写与本地执行 | 编译器、Catalog与`ProgramRuntime` | `ProgramCompiler`、`LocalProgramHost`、句柄/结果 | 原生、限定所有者作用域的便捷主机 |
| 强制生命周期Hook | 注册表、运行器与`HookRuntime` | 定义及`create_hook_runtime`回调 | 相同的失败关闭生命周期边界 |
| 运行时上下文与严格分发 | 上下文Store、收据与介入 | 对应的不可变值、Store与`StrictRuntimeProfile` | 相同的原生控制器 |
| 带持久化能力的wheel默认配置 | SQLite Core/上下文/分发Store | `_HAVE_SQLITE`导出 | 在PyPI wheel中启用 |

原始`ProgramCatalog`、转换Store、替换/迁移控制器、合成网关、Hook日志和RPC执行器仍属于主机组合API。若只暴露这些权限路径的一部分，就可能绕过必需的`proposal -> compile -> admit -> publish -> migrate/spawn`协议。未来的Python主机控制器必须把该协议及其不可续增的谱系预算绑定为一个限定所有者作用域的整体。`_HAVE_PROGRAM`并不声称原始控制平面管理也已实现对等。

### 运行时重试覆盖

```python
policy = ng.RetryPolicy()
policy.max_retries = 3
policy.initial_delay_ms = 100
policy.backoff_multiplier = 2.0
policy.max_delay_ms = 2_000
policy.jitter_pct = 0.2

engine.set_retry_policy(policy)
engine.set_node_retry_policy("remote_call", policy)
```

图定义的 `"retry_policy"` 仍是声明式默认。运行时 setter 是独立的 C++/Python 配置接口。

### 元数据与精确恢复

```python
config = ng.RunConfig(thread_id="job-42", input={"task": "..."})
config.model_token_budget = 20_000
metadata = ng.RunMetadata(
    timeout_ms=30_000,
    trace_id="trace-42",
    run_id="run-42",
    owner_scope="tenant-a",
)
result = engine.run(config, metadata)

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

在 Python 节点内部，相同的值可通过 `input.ctx.trace_id`、`run_id`、`has_deadline`、`deadline_remaining_ms` 和 `model_token_budget` 获取。

### 缓存作用域

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable` 是一个显式断言，表明该节点独立于租户、提供商、Store、工具、凭据、时间和恢复状态。

## Program 与 QuickJS

Python wheels 构建 `neograph::program` 以及受限的 QuickJS 前端。Python 定义的节点可以参与不可变的 Program 注册表，并通过原生 `ProgramRuntime` 执行。

```python
import neograph_engine as ng

registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node(
        "my_node", "1.0.0", "sha256:" + "1" * 64
    )
    .add_registered_reducer(
        "overwrite", "1.0.0", "sha256:" + "2" * 64
    )
    .build()
)

source = ng.ProgramSource.from_javascript("agent.js", r'''
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "my_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}
export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
''')

ceiling = ng.ProgramRunBudget()
ceiling.wall_time_ms = 10_000
ceiling.model_tokens = 1_000
ceiling.monetary_microunits = 1_000
ceiling.max_concurrency = 2
ceiling.max_program_operations = 32
ceiling.max_core_steps = 20
ceiling.max_dynamic_compiles = 1

run_budget = ng.ProgramRunBudget()
run_budget.wall_time_ms = 10_000
run_budget.max_concurrency = 2
run_budget.max_program_operations = 32
run_budget.max_core_steps = 20

host = ng.LocalProgramHost(registry, "tenant-a", ceiling)
version = host.compile_admit(source, run_budget)
result = host.run(version, {}, run_budget)
```

`LocalProgramHost` 是一个所有者作用域的内存便捷宿主。它仍然使用 C++ 编译器、Catalog、准入(admission)策略、转换存储和 ProgramRuntime。生成的提案在准入(admission)前还应通过宿主语义验证器；参见 [DSL 能力评估](DSL_CAPABILITY_EVAL.md)。

精确安装的 JavaScript 词汇表以字典形式提供：

```python
manifest = ng.javascript_authoring_capability_manifest()
```

## 强制生命周期 Hooks

Hooks 由宿主生命周期事件触发，而非由模型决定调用工具。

```python
data = ng.HookDefinitionData()
data.phase = ng.HookPhase.CheckpointPublished
data.target_id = "audit"
data.delivery = ng.HookDelivery.BlockingMandatory
data.failure_mode = ng.HookFailureMode.FailClosed
data.effect = ng.ToolEffectClass.ReadOnly

mapper = ng.HookInputMapper()
mapper.kind = ng.HookInputMapperKind.Template
mapper.value_template = {"kind": "checkpoint"}
data.input_mapper = mapper

definition = ng.HookDefinition.create(data)
runtime = ng.create_hook_runtime(
    [definition],
    {"audit": lambda arguments, event_type, event_data: persist(arguments)},
)
engine.set_hook_runtime(runtime)
```

在 `FailClosed` 下的回调失败会阻塞受保护的运行时边界。仅当可接受观测性损失时，`Continue` 才可用。

## 运行时上下文、Skills 与严格分发

该绑定暴露不可变的 RAW 历史记录、上下文工件、epochs、必需的 Skills/约束、转换收据和提供商分发收据。

```python
requirements = ng.RuntimeContextRequirements()
requirements.required_artifact_ids = [skill.id, constraint.id]
requirements.required_skill_artifact_ids = [skill.id]

assembler = ng.RuntimeTurnAssembler(
    context_store,
    max_input_tokens=32_000,
    requirements=requirements,
)
```

`ContextTransformReceipt` 允许任意派生的证据，但要求每个必需工件保持字节一致。

对于完整的严格路径，请使用持久化 SQLite 存储：

```python
contexts = ng.SQLiteContextStore("runtime.sqlite3")
receipts = ng.SQLiteProviderDispatchReceiptStore("runtime.sqlite3")
hooks = ng.create_hook_runtime(definitions, callbacks)

profile = ng.StrictRuntimeProfile(
    provider,
    contexts,
    receipts,
    hooks,
    provider_binding_identity,
    max_input_tokens=32_000,
    required_context_artifact_ids=[constraint.id],
    required_skill_artifact_ids=[skill.id],
)
profile.activate("tenant-a", strict_epoch)
completion = profile.invoke(params)
profile.attach(engine)
```

## HITL 与状态

静态 `interrupt_before`/`interrupt_after`、动态 `NodeInterrupt`、同步 `resume`、asyncio `resume_async` 以及精确 `resume_from` 都需要检查点存储。

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

使用 `get_state_history`、`update_state` 和 `fork` 进行检视和时间旅行。`get_state_view()` 提供基于 Pydantic 的扁平通道访问，而 `get_state()` 保留规范的嵌套表示。

## 异步与取消

`run_async`、`run_stream_async` 和 `resume_async` 返回 `asyncio.Future` 对象。取消 Future 会通过 `CancelToken` 传播到进行中的原生 I/O。流式回调会被编组回调用方的 asyncio 事件循环线程。

Python 定义的提供程序实现同步的 `complete`/`complete_stream`；异步原生的提供程序实现仍然是 C++ 扩展。

## 协议与可观测性

- MCP 客户端工具在构建后可通过 `neograph_engine.mcp` 使用。
- A2A 客户端类型在构建后可通过 `neograph_engine.a2a` 使用。
- `ProtocolHostAdapter` 将官方 Python A2A/ACP 服务器 SDK 与 NeoGraph 会话语义集成。
- `neograph_engine.tracing` 和 `neograph_engine.openinference` 为 Phoenix、Langfuse、Arize 及兼容后端发出供应商中立的 OTel/OpenInference 数据。

## 可选组件

公共包如实标记可选的 C++ 组件：

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A`;
- 缺失的组件是缺失的，而不是在 Python 中模拟的；
- PyPI wheel 启用 Program/QuickJS、LLM、MCP 和 SQLite；源码构建遵循其 CMake 选项。

## 测试与示例

绑定测试套件涵盖 Core 执行、自定义回调、asyncio、取消、Program 编译/运行时、强制 Hook、严格上下文、SQLite 持久化、协议以及 README 示例。

- [Python 示例](../bindings/python/examples/README.md)
- [C++ 示例](../examples/README.md)
- [QuickJS 创作边界](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [严格运行时插桩](STRICT_RUNTIME_INTERPOSITION.md)
