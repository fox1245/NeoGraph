<!-- neograph-i18n: source=docs/troubleshooting.md locale=zh-CN source_sha256=619cc967ee0702270da38d4f92f783baacddd5dee11306e5545600ae7c24fb51 -->
# 故障排查

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## 确认安装的是哪个构建产物

先记录 `neograph_engine.__version__`、Python 版本、操作系统和架构、安装方式，以及最先出现的那个错误。请到该版本实际提供的 wheel 文件里确认，不要默认源码检出和旧 wheel 暴露的是同一套 API。类型化切换（typed cutover）移除了旧的 provider 名称；用旧 wheel 做 import，测不出新的绑定。

NeoGraph 发布的平台元数据包含 Linux、macOS 和 Windows。仅有兼容的 wheel 标签或编译器，不能证明运行时已经集成好。WASM 上的 provider 运行时支持尚未确立。

遇到缺少 GLIBC 符号的错误时，把 wheel 的 manylinux 标签与宿主机的 glibc 版本对照。`manylinux_2_34` 的产物要求 glibc 2.34 或更新。Windows 上出现 DLL 错误时，确认用的是 x64 Python，并检查缺失的依赖 DLL；原因不一定只是架构不符。不要单独替换 wheel 中捆绑的某个库。

## 源码配置与缺失的依赖

即使设置了 `NEOGRAPH_BUILD_LLM=OFF`，Core 仍然需要 `SchemaProvider::runtime`。配置 NeoGraph 需要 CMake 3.20 及以上。依赖的查找顺序是：优先使用显式指定的 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`，其次是已安装的包，最后是默认的、固定了修订版本的公开归档。离线配置时，先安装匹配的 SDK，通过 `CMAKE_PREFIX_PATH` 设置它的前缀，并设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。从源码构建 SDK 需要 C++20、Python、独立版 Asio、yyjson、libcurl 7.88 及以上版本和 OpenSSL Crypto；NeoGraph 会把仓库中自带的 Asio/yyjson 提供给下载或源码形式的集成。

SDK 自身没有声明 OpenSSL Crypto 的最低版本。NeoGraph 完整的 HTTPS 配置以及 wheel/sdist 路径，会通过异步/MCP 的 HTTP 依赖要求 OpenSSL 3；只安装 Crypto 的头文件，不足以完成这些构建。

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

构建目录不是已安装的包。在运行 `PYTHONPATH=build python -c "import neograph_engine"` 之前，先用 `pip install 'pydantic>=2' certifi` 安装 wheel 的 Python 依赖：没有 pydantic，import 会失败；没有 certifi，TLS 会使用宿主机的信任库。

出现 `Could NOT find CURL` 时，关闭 `NEOGRAPH_USE_LIBCURL` 解决不了问题：这个开关控制的是 NeoGraph 可选的 CurlH2Pool，不是 SDK 必需的传输层。请安装 libcurl 的开发文件（Debian/Ubuntu 上是 `libcurl4-openssl-dev`，Fedora 上是 `libcurl-devel`），并保持工具链和前缀一致。

SQLite 和 PostgreSQL 是 NeoGraph 的可选组件。要么提供它们的开发包，要么显式关闭 `NEOGRAPH_BUILD_SQLITE`/`NEOGRAPH_BUILD_POSTGRES`。这样做不会去掉 SDK 运行时。头文件或 API 变更之后如果出现绑定符号无法解析，请在全新的构建目录里重新配置，并针对匹配的头文件和库重新构建所有使用方。

曾有人报告 GCC 13 在协程上出现编译器内部错误。请升级到合适的编译器，或者调整报错的那个表达式。绝不要在 catch 处理器里 `co_await`：C++ 本身就禁止在那里使用 await 表达式。应该先把错误捕获下来，再到处理器外面 await 恢复逻辑。

## 已移除的 provider API 与带类型的失败

`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、描述符解释器配置、`prefer_libcurl` 和 Responses WebSocket 的选择，都已不存在。请使用 `SchemaProvider(load_provider_descriptor(descriptor_json), ProviderRuntimeOptions(...), SchemaProviderDefaults(...))` 和 `make_provider_request`。具体的构造函数见 [Python 绑定](python-binding.md)，C++ 的迁移见[迁移指南](migration-v0.4-to-v1.0.md)。

`prepare` 返回只能使用一次的 `PreparedProviderRequest`，`dispatch` 会把它消耗掉，`invoke` 则把两步合在一起。已准入的请求一旦持久化了它的分发回执，就不要再重建，也不要重试已被消耗的句柄。失败的 outcome 本身就是数据，带有真实的错误和部分输出的证据；请检查 `outcome.failure`，不要把它当作形似成功的 completion 来读。

observer 抛出的异常，会把带类型的 outcome 和 cause 保留在 `ProviderObserverError` 里。预算结算和终态回执持久化的错误同样保留真实的 outcome；这些证据并不授权再发送一次。送达状态未知的请求需要对账，不能盲目重试。

## 流式输出与 Python 异步的边界

请显式设置 `ProviderMode.Stream`。只挂 observer 不会选择流式。要显示 token，请过滤出非空的内容文本增量；用量、reasoning、工具、raw-wire 和 envelope 事件都不是 token。C++ 的事件视图只在回调期间有效（是借用的）；Python 的 ProviderEvent 持有从借用数据复制出来的副本。避免使用慢速的 observer，它们会占用宿主的交付能力。

```python
import asyncio
import neograph_engine as ng

text = ng.Text()
text.value = "Hello"
request = ng.make_provider_request(
    provider, model, [ng.ProviderMessage(ng.ProviderRole.User, [text])],
    mode=ng.ProviderMode.Stream,
)
request.on_event = observe
outcome = await asyncio.to_thread(provider.invoke, request)
if outcome.failure is not None:
    handle_failure(outcome.failure)
else:
    handle_completion(outcome.completion)
```

Provider 的 invoke/dispatch 是同步的 Python 方法，会释放 GIL。请配合 `asyncio.to_thread` 使用，它们不是原生的 asyncio awaitable。回调的调用、复制和销毁都会获取 GIL。取消使用图的 CancelToken，并把停止请求转发给 SDK；超时或等待方被取消，并不能证明远端的副作用没有发生。

续接对话时，请保留完整的 `ProviderMessage`，包括工具、refusal 和 reasoning 这些 part。`ChatMessage` 和 `ToolCall` 只是方便图使用的值，不是完整 provider 历史的别名。缺失的用量是 `None`，不是 0；已知为零的计数器仍然是带证据的 UsageCount 值。可移植的 outcome 投影无法引入 native replay 或资金方面的权限。只有在已准入的 NativeArchive 托管下，才能持久化 native 的续接状态。

## TLS 与本地 endpoint

要指定明确的信任包，设置 `ProviderRuntimeOptions.ca_file`。Python 导入时会保留已有的 `SSL_CERT_FILE`，没有的话，在 certifi 可用时选用 certifi；运行时选项把选定的 CA 文件交给 SDK 的 libcurl。设置 `NEOGRAPH_SKIP_CERT_AUTOFIX=1` 则不改动宿主自己的选择。不要为了掩盖 endpoint 或信任库的错误而关闭证书校验。

本地 HTTP 的准入应该写在经过校验的描述符里，而不是直接覆盖 URL；请参照[示例 31](../examples/31_local_transformer.cpp)。

## 图定义与注册

出现 unknown reducer、condition 或 node type 的诊断，说明这个名称不在编译所用的注册表里。内置的 reducer 是 `overwrite` 和 `append`；自定义的 reducer、条件和节点工厂必须在编译之前注册。`has_tool_calls` 和 `route_channel` 是内置条件。注册的 Python 回调在 GIL 下运行。

```python
import neograph_engine as ng

ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
ng.ConditionRegistry.register_condition("is_long",
    lambda state: "long" if len(state.get("messages") or []) > 10 else "short")
```

写入时必须准确写出一个已存在的通道名。条件返回的是路由标签：开放（open）的条件可以显式使用 `default`，没有它时，未匹配的标签会抛出异常；封闭（closed）的条件则会拒绝声明集合之外的标签。务必保证有一条从 `__start__` 出发的边，循环要有出口路由。`RunConfig.max_steps` 默认是 `50`；请检查步数上限的状态，不要把中途截断的运行当成正常完成。

在 `schema_version: 1` 下，严格的拓扑解析会拒绝未知或未使用的键，也拒绝往返之后有信息丢失的定义。元数据用 `_`/`x-` 开头的注解，屏障上要写非空的 `wait_for`，条件边上要写 `routes`。自定义注册完成之后，用 `ng.export_schema()` 导出当前生效的 schema；不要另外维护一份编辑器的节点面板。缺省或为零的旧版本号，在 `0.x` 路径上仍按宽松方式处理；`ng.upgrade_topology()` 会把被忽略的数据保存在不会冲突的注解里。

## fan-out 与管理操作

worker_count 默认是 `1`，因此不会创建引擎自己的线程池。I/O 分支在挂起时仍然可以重叠；但在单个调用线程上，CPU 密集的函数体会串行执行。如果 CPU 计算能从线程池中受益，请在并发执行之前配置 `set_worker_count(N)` 或 `set_worker_count_auto()`。Python 里 CPU 密集的回调仍然受 GIL 限制而串行，除非某个原生操作释放了 GIL。

同一个引擎上，只要有任何 run/resume 正在进行，此时调用管理类的 state/history/update/fork，就会以 `std::logic_error` 失败。请先取消或排空，并等待运行结束；不要把这个异常吞掉。thread id 相同的并发执行，其检查点的交错顺序没有规定；多个引擎共用一个存储时，需要由宿主来协调。参见[并发指南](concurrency.md)。

## Python Program 错误

这些错误来自 `LocalProgramHost` 和 QuickJS Program 编译器。背后的规则见 [Program 与 QuickJS](python-binding.md#program-and-quickjs)。

| 错误 | 原因 | 解决办法 |
|---|---|---|
| 节点中出现 `TypeError: get(): incompatible function arguments` | `GraphState.get` 只接受通道名 | 使用 `state.get("value") or 0` |
| `add_registered_node`、`add_registered_reducer` 或 `add_registered_condition` 抛出 `ng.ProgramRegistryError` | 该节点类型、reducer 或条件没有在本进程中注册 | 先用 `@ng.node`、`ng.NodeFactory.register_type`、`ng.ReducerRegistry.register_reducer` 或 `ng.ConditionRegistry.register_condition` 注册 |
| `P_JS_DEFINE_MISSING` | 模块没有导出 `define()` | 导出 `define()`，并返回 Program 的唯一那张图 |
| `P_JS_GRAPH_ENTRY`，或者节点名为 `__start__`/`__end__` 时的 `P_JS_GRAPH_ARGUMENT` | 图没有 `.entry()`，或者有节点用了保留名称 | 调用 `.entry()`，并给节点改名；`.entry()` 和 `.exit()` 负责连接这两个哨兵节点 |
| `P_CORE_E13` 或 `P_CORE_E14` | 节点在第一条条件边之外，还有一条静态边或第二条条件边；两者都永远不会触发 | 把所有目标都放进第一条条件边的 routes 里 |
| `P_JS_CONTROL_COMMAND`：JavaScript command references a Core outside the admitted root | `ng.callCore` 指定的图不是 `define()` 返回的那张 | 使用传给 `ng.graph(...)` 的名称 |
| `ValueError: Every policy budget ceiling except child budgets must be positive; zero: ...` | 列出的某个上限字段为零 | 改成正数，即使 Program 从不调用模型 |
| `ng.ProgramAdmissionError` 中的 `P_ADMIT_PROVIDER` | 某个 `llm_call` 节点需要的 provider 没有在宿主中绑定，或者 `llm_call`、`intent_classifier` 节点注册时没带 `provider=` | 注册 `add_provider(...)` 和 `add_registered_node("llm_call", ..., provider=...)`，并给 `LocalProgramHost` 传入 `node_context=` 和 `binding_label=` |
| `P_MONEY_BUDGET`（`BudgetExhausted`），或其他 `P_MONEY_*` 错误码（`Failed`），且 provider 没有收到任何请求 | 该次调用的预留额度超出了 `monetary_microunits`（0 表示什么都不授予），或者调用无法确定上限：模型或 origin 未知、路由别名没有路由上限、没有输出上限、不知道输入上限，或者用到了价格目录没有建模的功能 | 调高 `monetary_microunits`，设置 `provider_controls.max_output_tokens`，或者改用价格目录中有条目的模型；参见[定价证据与价格目录](reference-en.md#pricing-evidence-and-the-price-catalog) |
| `P_START_BUDGET`: `Program invocation budget <field>=<n> is outside its admitted bounds` | 运行预算与传给 `compile_admit` 的预算不一致 | 传入相同的预算，或者重新编译并准入 |
| `ng.spawn` 抛出 `P_CHILD_BINDING` | `LocalProgramHost` 没有子绑定解析器 | 在 C++ 宿主里运行子 Program |
| `resume()` 抛出 `P_RUN_NOT_FOUND`：`run '<id>' was not found in this LocalProgramHost` | 这个运行属于另一个 `LocalProgramHost`，或者所属的宿主已销毁 | 通过启动该运行的宿主来恢复 |
| `P_CORE_BARRIER_STALLED` | 屏障的某个成员走了不通向 join 的路由，join 再也无法触发 | 让每个成员都路由到 join 或 `__end__`；`P_CORE_E15` 会在编译时对这类路由发出警告 |
| `P_RUNTIME_TIMEOUT`: `Program wall-time budget (N ms) expired` | 运行用完了 `wall_time_ms`；被打断的操作的文本在 `failure.witness["cause_message"]` 里 | 用更大的 `wall_time_ms` 重新编译并准入 |

`callCore` 失败会让运行带着该失败结束；`main()` 无法用 `try`/`catch` 捕获它。请在 Python 中检查 `result.status` 和 `result.failure`。

## 检查点与 PostgreSQL 故障

找不到 PostgresCheckpointStore 的导出，可能是这个 wheel 或源码构建禁用了该组件；请检查这个产物自己的配置，而不是参考更旧的 wheel 的功能列表。构建这个可选目标时，要安装匹配的 libpq。连接 URI 中的特殊密码字符需要做百分号编码，或者改用 libpq 的 key=value 形式；千万不要公开真实的凭据。

异步的连接和重连，对所有主机/IP 共用同一个截止时间。显式给出的正数 `connect_timeout=N` 以秒为单位，其中 `1` 会被进位到 2。未设置、为零或为负的值，以及只通过 PGCONNECT_TIMEOUT 或 service 文件提供的超时，都使用异步默认的 30 秒。这与同步 libpq 按每台主机计时的超时不同。

权限允许时，存储会自己建表；缺少 CREATE 权限时，需要手工执行 [PostgreSQL 头文件](../include/neograph/graph/postgres_checkpoint.h)中的 schema。没有 pending write 能力，意味着要整个超步重放，而不是外部副作用恰好执行一次。只支持异步的自定义后端，应当使用 AsyncCheckpointStore 加 `adapt_async_checkpoint_store`，而不是以前那种同步与异步互为默认实现的交叉写法。

## Tracing 适配器与其他常见陷阱

Tracer 适配器必须持有自己记录的数据，而不能保存指向 Span 包装器的裸指针，因为会话关闭时这些包装器会被销毁。参见 [C++ tracing 示例](../examples/49_openinference.cpp)。Python 的 contextvars 不会自动穿过 C++ 回调的边界；需要显式传递或挂接父 context，并且要在引擎编译之前安装这些包装器。

如果使用方在多个编译单元里实例化仅有头文件的 httplib，需要让 CPPHTTPLIB_OPENSSL_SUPPORT 的定义保持一致；带类型的 provider 通过 SDK 的 libcurl 发送 HTTP 请求。

不透明的便捷向量属性不一定是 Python 列表。检查时用迭代或 `list(value)`；对于 ChatMessage.image_urls 这类返回副本的序列字段，应当先构造好再整体赋值。不要以为修改返回的副本就能改变 C++ 的请求。

## 工具调用、Hook 与协议错误

图运行中的工具调用、节点或子图被强制 Hook 阻塞时，会直接抛出 `HookBoundaryBlocked`，与 `llm::Agent` 的做法相同，而不是包着它的 `NodeExecutionError`，也不会为此发出 `ERROR` 事件。请捕获 `HookBoundaryBlocked`（Python：`ng.HookBoundaryBlocked`），并读取 `invocation_id` 和 `state`。抛出它的 Python 节点（例如来自嵌套的引擎运行）不会被重试。参见[工具失败契约](reference-en.md#tool-failure-contract)。

引擎的 `HookRuntime` 也会运行它的工具调用所对应的 `BeforeToolExecution` 和 `AfterToolExecution` Hook；Program 宿主的 Hook 运行时则运行其各个 Core 的工具调用对应的这两类 Hook。如果图运行以 `HookBoundaryBlocked`（state 为 `failed`，消息为 `lifecycle hook could not run`）停止，请给这次运行指定 owner 作用域（`RunMetadata::owner_scope`）：没有 owner 作用域的 Hook 边界会失败时关闭，检查点 Hook 和工具 Hook 都是如此。

`state == "timed_out"` 表示 Hook 的截止时间已到，或者它的目标超时。最后一次尝试失败的 Hook 报告 `failed`。

工具结果的状态为 `expired`、错误为 `run deadline elapsed before the tool started` 时，表示在调用开始之前，运行的截止时间已经过了：工具和它的 Hook 都没有运行。请调高 `RunMetadata::deadline`（Python 中是 `timeout_ms`，Program 中是 `wall_time_ms`），而不是重试这次调用。被运行的截止时间或客户端的请求超时终止的 MCP 调用，状态是 `timed_out`；取消运行会立即终止正在进行的 MCP 调用。

对于客户端已取消的 `tools/call`，`MCPServer` 在任何传输方式下都不发送响应；Hook 阻塞用 `-32010` 回应，中断用 `-32011` 回应；其他所有失败都是 `isError` 结果，由 `_meta["io.neograph/failure"].code` 给出错误码，其文本是公开消息，绝不是异常文本（想自己选定文本，可以抛出带 `public_message` 的 `NodeFailureError`）。gRPC 客户端必须读取状态，而不是 `OK` 响应里的 `RunGraphResponse.error`：失败的运行对应的是非 OK 的状态，其 `google.rpc.ErrorInfo` 在 `metadata["code"]` 中带有错误码。参见[协议失败映射](reference-en.md#protocol-failure-mapping)。

不是有效 UTF-8 的工具输出或异常消息，每个格式错误的序列都会换成 U+FFFD 后再送达，不会变成空的工具消息、空的 MCP 或 gRPC 负载，也不会变成 Python 中的 `UnicodeDecodeError`。

## 安全地报告 bug

请提供版本和平台、最小化的拓扑和调用代码、execution_trace 和 status，以及经过脱敏的、带类型的失败/停止/用量证据。并说明失败发生在已安装的 wheel 上，还是重新构建的源码检出上。切勿包含凭据、私密的提示词、编码后的 native 请求体，或未经清理的抓包数据。请到 <https://github.com/fox1245/NeoGraph/issues> 报告。
