<!-- neograph-i18n: source=ROADMAP_v1.md locale=zh-CN source_sha256=e5d4df28a5ba92ca1778fdff4fa741fc4c435c9dfba1fddbe8dacd31ca1f3121 -->
# NeoGraph v1.0 — 设计细化路线图

**Languages:** [English](ROADMAP_v1.md) | [한국어](ROADMAP_v1.ko.md) | [日本語](ROADMAP_v1.ja.md) | [简体中文](ROADMAP_v1.zh-CN.md)

本文件追踪以未来 v1.0 主版本升级为目标的**架构性**变更。这些不是增量
补丁；每一个都是公开 API 破坏候选，需要弃用窗口期。作为动态文档维护——
当 v0.3.x 补丁系列暴露结构性痛点时，在此添加候选；当某个候选落地时，
在此移除。

## 为什么有这个文件

v0.3.x 取消传播系列（5 轮：v0.3.0 单节点、v0.3.1 多 Send 指针、
v0.3.1+ 进程内轮询、v0.3.2 Python 钩子、v0.3.2 C++ 作用域+重试+
异常类型）是一个需要 5 个补丁的单一逻辑修复，因为**同一个横切关注点
（取消）必须穿过 ~8 个分发入口点加上 2 种入口语言
（C++/Python）**。每个补丁关闭一个入口路径，而让其他路径敞开。

错误的模式几乎从来不是"架构错了"——而是"正确的模式在 N 个位置中的 M 个
位置被应用了"。v0.3.x 系列通过异常验证了*核心*设计（Pregel BSP 超级步骤、
通道 reducer、Send/Command 动态分发、asio 协程贯穿始终）：错误被捕获而
从未质疑模型。

该系列真正暴露的是当前设计中的三个高认知负载接缝，导致 N 位置实现分布
容易出错。以下每个候选都针对一个接缝。

---

## 候选 1 — 基于标签路由的单一分发入口

### 症状

`GraphNode` 暴露 8 个虚方法：

```
execute            execute_async
execute_full       execute_full_async
execute_stream     execute_stream_async
execute_full_stream execute_full_stream_async
```

这些形成了一个 `(同步/异步) × (写入/完整) × (流式/非流式)` 笛卡尔积。
默认实现相互链接；优先级顺序必须保持一致。每个默认链跳跃都是一个可能
隐藏错误的地方：

- v0.3.1 #2：提示消息未提及流式变体。
- v0.3.2 #10（Python）：`PyGraphNode::execute_full_stream` 跳过了
  `execute_stream` 分支——`run_stream` 对流式专用节点无效。
- v0.3.2 #10（C++）：`GraphNode::execute_full_stream` 默认实现先调用
  `execute_full` → `ExecuteDefaultGuard` 递归 → 抛出异常 → `execute_stream`
  从未到达。
- GCC-13 代码生成变通方法在 `execute_full_stream_async` 中需要，因为
  `catch(T&)` 在 `co_await` 周围会静默失败。

### 细化方案

单一虚函数分发：

```cpp
class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
};

struct NodeInput {
    const GraphState&    state;
    const RunContext&    ctx;          // see Candidate 2
    GraphStreamCallback  stream_cb;    // null if non-stream
    bool                 is_async;     // hint, not a hard contract
};

struct NodeOutput {
    std::vector<ChannelWrite> writes;
    std::optional<Command>    command;
    std::vector<Send>         sends;
};
```

用户重写一个方法。同步/异步区别由引擎处理（引擎在 run_sync 中包装同步
重写，异步重写则直接 await——但这是引擎的关切，而非用户的关切）。
流式区别：`stream_cb` 非空 = 需要流式；用户使用或忽略。Command/Send：
只需填充字段。

迁移：在过渡版本中将 8 个虚函数保留为弃用的薄垫片。新代码重写 `run()`。
跳板（`PyGraphNode`）变为一行代码。

### 成本

- 公开 API 破坏——每个现有的 GraphNode 子类需要重写 `run()`。
- `RunContext`（候选 2）是硬性前置条件，否则 `run()` 无法承载每次运行的
  元数据。
- 引擎内部分发逻辑变得简单，但引擎必须基于运行时提示或约定选择同步或异步。

---

## 候选 2 — 针对每次运行元数据的显式 `RunContext`

### 症状

今天 `RunConfig::cancel_token` 是调用者可以设置的唯一每次运行"元数据"。
引擎通过两种机制暗通：

1. `GraphState::run_cancel_token_` — 一个存在于 GraphState 中但**不在
   通道集内**的成员，因此 `serialize()` 会丢失它。

   - v0.3.1 多 Send 修复：`init_state(send_state) +
     send_state.restore(snapshot)` 重建了每个工作器的状态，但丢失了
     `run_cancel_token_`，因为它不在通道集内。需要显式的
     `send_state.set_run_cancel_token(parent.run_cancel_token_shared())`
     在每个 Send 扇出工作器上。
   - 下一个添加每次运行字段的人（deadline？trace_id？指标句柄？）将再次
     犯同样的错误。

2. `current_cancel_token()` thread_local — 由
   `CurrentCancelTokenScope` 在 execute_full_async 入口设置。

   - v0.3.2 C++ 修复：PyGraphNode 安装了作用域；原生 C++
     `GraphNode::execute_full_async` 默认实现没有，因此多 Send C++ 工作器
     的 `Provider::complete` 看到空的 thread_local 且 run_sync 运行而
     没有取消绑定。7 秒成本泄露。
   - 每个新的分发入口点都需要记住安装作用域。忘记 = 静默功能破坏。

这两种机制都存在，因为没有存放每次运行元数据的一等位置。它们是变通方法。

### 细化方案

显式 `RunContext` 与 `GraphState` 一起传递，贯穿每次分发：

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::optional<Deadline>       deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    int                           step;
    StreamMode                    stream_mode;
    // ... extension point for future cross-cutting concerns
};

class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
    // in.ctx is the RunContext — no thread_local, no
    // serialize-loses-it. Every dispatch path threads it explicitly.
};
```

`Provider::complete(params, ctx)` 也接收上下文。没有 thread_local。
没有 `current_cancel_token()`。Send 扇出工作器按值复制 `ctx`
（便宜——shared_ptr + 几个字符串）。

### 成本

- 公开 API 破坏——每个 Provider、每个 GraphNode、每个 Tool。
- 更宽的签名贯穿各处——到处是 `state, ctx`。
- 但是：关闭了整类"我忘记传递 cancel/trace/deadline"的错误。一个签名，
  一个添加新字段的位置，没有变通方法。

### 这个候选本可避免哪些 v0.3.x 错误

- v0.3.1 多 Send 指针丢失：ctx 只是一个显式字段，不埋在非序列化成员中。
- v0.3.2 C++ thread_local 缺失：完全没有 thread_local。
- 未来的 deadline / trace_id / 指标泄露：相同形态，相同预防性覆盖。

---

## 候选 3 — 分层 / 每消费者 CancelToken

### 症状

`CancelToken` 的设计围绕一个 `cancellation_signal sig_` + 一个
`bind_executor` 槽。asio 的 `cancellation_slot` 是单个处理器的——最后
`bind_cancellation_slot` 胜出。并发消费者（多 Send 扇出工作器每个调用
Provider::complete → 内部 run_sync → bind_cancellation_slot）静默地相互
覆盖绑定；只有最后绑定的 HTTP 被取消。

v0.3.2 在这个单信号设计之上嫁接了一个 `add_cancel_hook` 列表，使每个嵌套
的 run_sync 拥有自己的私有信号，父节点的 `cancel()` 通过迭代钩子触发。
可以工作，但读起来像"在 N 消费者上下文中补偿单消费者原语"。再加上一个
emit-vs-bind 竞态：如果调用 add_cancel_hook 时 cancel 已被设置，同步
触发在 co_spawn 绑定槽之前发布 emit，且 emit 被丢失。v0.3.2 添加了一个
急切的 `is_cancelled()` 短路在 run_sync 入口处以回避这个问题——又一个
补丁上的补丁。

### 细化方案

分层取消：

```cpp
class CancelToken {
public:
    /// Create a child token. Parent.cancel() cascades to child.
    /// Each child has its OWN cancellation_signal — no
    /// single-consumer assumption.
    std::shared_ptr<CancelToken> fork();

    /// Cancel this token (and recursively all children).
    void cancel();

    bool is_cancelled() const noexcept;
    asio::cancellation_slot slot();  // each token has its own
    void bind_executor(asio::any_io_executor ex);
};
```

每个 `run_sync(aw, parent_token)` 执行：
```cpp
auto child = parent_token->fork();
child->bind_executor(io.get_executor());
asio::co_spawn(io, body(),
    asio::bind_cancellation_slot(child->slot(), asio::detached));
```

无需嫁接的 add_cancel_hook 列表。没有 emit-vs-bind 竞态（子令牌全新
创建，信号先绑定，fork() 快照父状态）。多 Send 扇出：3 个兄弟令牌，
父令牌取消所有三个。

借鉴：Go 的 `context.Context` 取消，asio 的 `asio::cancellation_state` /
`make_cancellation_filter`（如果 asio 获得合适的 API）。此模式广为人知。

### 成本

- CancelToken 的公开 API 变更（仅添加——`fork()` 是新的）。旧的
  `add_cancel_hook` 将被弃用。
- 内部：每个 `run_sync(aw, cancel)` 变为 `run_sync(aw, cancel->fork())`。
- 净效果：一个原语取代"单信号 + 钩子列表 + 急切取消短路 + 每消费者竞态说明"。

---

## 横切观察

三个候选互相组合：候选 2 通过分发路径携带候选 3 的令牌；候选 1 的单一
`run()` 自然接收包含取消子令牌的 `RunContext`。

如果只选择落地一个，优先候选 2——它消灭了最大类别的重复性错误（任何需要
穿过每次分发的内容）。

追踪：当 v0.3.x 补丁轮次暴露新的架构接缝，或某个候选落地时（划线删除并
链接到合并提交），更新本文件。

---

## 模式回顾——9 个下游发现（issue #36）

ProjectDatePop 的 `cpp_backend` 在 v0.5 → v0.8 窗口期间的压力测试产生了
9 个 NeoGraph 发现。**这 9 个发现中至少有 7 个追溯到候选 1 + 6 关闭的
同一结构模式**——不是增量修复，而是*通过消除模式可能重现的表面来*关闭。

### 统一模式

> **"X 仅在 Y 时安全"——但 Y 前提条件既未在 docstring 中说明，也未在
> 编译时强制执行，更未在违反时于运行时反馈。默认路径静默地做错事，通常
> 仅在输入笛卡尔积的特定角落生效。**

| # | 隐藏的条件不变式 |
|---|---|
| #4 | `Provider::complete_stream_async` 默认桥接是安全的**仅当**原生同步 `complete_stream` 自身不使用 `run_sync`——被 `SchemaProvider` WS 路径静默违反 |
| #5 | `Provider` 的 4 虚函数笛卡尔积是安全的**仅当**选择的重写面恰好避免桥接嵌套——不变式从 `provider.h` 不可见 |
| #6 | `schema_mutex_` × on_chunk 锁定是安全的**仅当**用户的回调不重新进入 SchemaProvider——修复前未记录 |
| #9 | C++ openinference 等效性被需要，因为 Python 包装器有一个关于回调线程标识的隐藏假设，无法转换 |
| #16 | NeoGraph 的内嵌 cpp-httplib 是正确的**仅当**每个消费者翻译单元定义了 `CPPHTTPLIB_OPENSSL_SUPPORT`——否则静默 ODR 违反 |
| #34 | `extra_fields` 应用**仅当** `params.tools` 非空——无工具调用时静默丢弃推理字段 |
| #35 | `temperature` 被发送**仅当** `params.temperature ≥ 0`——但 schema 无法声明"此 provider 根本不能接受 temperature"，迫使每个调用点取反默认值 |

另外两个发现（#17 文档缺口、#33 每次调用绑定缺口）是缺口报告而非隐藏
不变式陷阱；相同的根本诊断（抽象声明了静态表面但没有暴露动态等价物）适用。

### 为什么候选 1 + 6 关闭的是*整个类别*，而不仅仅是个例

上述每个发现通过针对行为不端的特定重写点的**定向补丁**关闭（PR #10、
PR #11、PR #12、PR #19、PR #20、PR #37、PR #37）。每个补丁留下了允许
该模式的*表面*不变：8 个 GraphNode 虚函数、4 个 Provider 虚函数、
schema build_body 分支树。下一个下游——或下一个供应商 schema，或调整
默认值的下一次重构——将在同一笛卡尔积的某个新角落发现某个新的
"X 仅在 Y 时安全"隐藏在暗处。

候选 1 和 6 将这些笛卡尔积折叠为**各一个虚函数**。它们落地后：

- **候选 1**（GraphNode 8 → 1）：不再有"你重写 8 个虚函数中的哪一个
  决定桥接是否安全"的决策。用户重写 `run(NodeInput)`。同步 vs 异步、
  流式 vs 非流式、写入 vs 完整结果都是函数体形态选择——没有与虚函数
  身份绑定的隐藏不变式。
- **候选 6**（Provider 4 → 1）：新实现使用 `CompletionProvider::do_invoke()`
  作为它们的单一重写点。现有 `Provider` 表面为兼容性保持稳定，而新路径
  有一个明确的请求模式和一个排出模式。

剩余 2 个发现（#9 线程标识、#16 ODR 宏）*不*由候选 1 + 6 修复——
它们是独立的 issue 类别（可观测层等效性、构建系统约定）。#9 已通过
PR #12 + 等效性测试解决。#16 现在是编译时守卫（v0.8.0 `api.h`）。

### 本次回顾中具有持久价值的部分

这 9 个发现**与项目年龄无关**。它们都不需要长期的生产部署或异国供应商——
它们来自单个下游消费者（ProjectDatePop）在约 3 周内编写真实的 agent 流。
没有候选 1 + 6，下一个有类似深度的下游将产生另外 5–10 个相同形态的发现。
有了它们，此类别已关闭。

这是**在 v1.0 周期中优先候选 1 + 6 而非更多表面 v0.x 清理**的结构性论据。
每个新的"X 仅在 Y 时安全"发现通过补丁努力自己买单，但 7 个发现的累积
努力已经超过了候选 1 + 6 的预估成本。

### v0.x 弃用窗口中的缓解措施

在候选 1 + 6 落地之前，在它们今天存在的地方固定不变式：

- `[[deprecated]]` 在旧的 8 个虚函数上 + `docs/migration-v0.4-to-v1.0.md`
  — 已在 v0.4 / v0.8 落地。
- `@warning` 块在每个有"X 仅在 Y 时安全"前提的重写点上（例如
  `Tracer::start_span`、`OpenInferenceTracerSession::close`）。
- 编译时 `#error` 守卫在语言可以表达的跨 TU 不变式上（例如
  `CPPHTTPLIB_OPENSSL_SUPPORT` 宏一致性——已在 v0.8 落地）。
- 友好的运行时错误，在违反时指明不变式（例如 `Unknown reducer: 'foo'.
  Available: ...`——已在 v0.8 落地）。

这些缩小了该模式造成损害的窗口，但未关闭整个类别。候选 1 + 6 做到了。

---

## 状态

| # | 候选 | 状态 | 触发轮次 / issues |
|---|---|---|---|
| 1 | 单一 `run()` 分发 + 标签 | **已在 v0.9.0 落地。** `run(NodeInput)` 是纯虚函数；旧的 8 个虚函数和回退链已消失。 | v0.3.1 #2、v0.3.2 #10（×2 种语言）；模式由 #36（9 个下游发现）加强 |
| 2 | 显式 `RunContext` 参数 | **已在 v0.4–v0.8 落地**（`RunContext::store` 字段在 v0.8 #27 中添加） | v0.3.1 多 Send、v0.3.2 C++ 作用域 |
| 3 | 分层 CancelToken | **已在 v0.4 落地**（`CancelToken::fork()` + 级联） | v0.3.2 钩子、v0.3.2 emit-vs-bind |
| 4 | 自演化图运行时钩子 | 研究 | TODO_v0.3.md #8 |
| 5 | pgvector RAG 示例 | Cookbook | TODO_v0.3.md #9 |
| 6 | Provider 单一分发 | **已落地，未移除。** `CompletionProvider::do_invoke()` 是推荐的一重写路径。现有 `Provider::complete*` 方法继续受支持；弃用警告已被撤回，没有移除计划。 | #4（在 v0.7 关闭）、#5（兼容性策略）、模式由 #36 加强 |

---

# 执行计划

> **状态：** 候选 1 已完成。以下计划记录了迁移如何从 v0.4.0 到破坏性的
> v0.9.0 v1 预备版分阶段进行；它是历史背景，而非剩余工作。

## 面向用户的动机

暂时忘记错误类别框架。从**今天打开 README 的新用户**的视角，表面看起来
碎片化：

  - "如何写一个节点？" — 8 个虚函数（`execute` / `execute_async`
    / `execute_full` / `execute_full_async` / `execute_stream` /
    `execute_stream_async` / `execute_full_stream` /
    `execute_full_stream_async`）。选哪一个？答案是"取决于 Send/Command、
    同步/异步、流式/非流式"——三个正交轴，用户必须从一开始就推理。
  - "如何取消？" — `RunConfig::cancel_token` 存在，但要让取消到达 LLM
    还需要：(a) 引擎安装 thread_local 作用域，(b) Provider::complete
    读取它，(c) run_sync 注册一个钩子，(d) 工作器不重试。这些没有一个
    在一个地方可读。
  - "如何更新状态？" — 在 v0.3.2 中是 `dict | list[ChannelWrite]`。
    在此之前 README 记录了一种形态而绑定对另一种静默无操作。新用户遇到
    "为什么我的写入没有生效？" 然后必须调试。
  - "如何读取状态？" — 嵌套 `state["channels"][name]["value"]` 或
    扁平 `engine.get_state_view(thread_id).<channel>` 或类型化的
    Pydantic 子类。三个有效答案；没有单一标准答案。
  - "如何运行一个图？" — `run`（同步）vs `run_async` vs
    `run_stream` vs `run_stream_async` vs `resume` vs `resume_async`。
    六个入口点，又是多轴矩阵。

**每个单独的添加都是有理由的**（resume_if_exists 是真实的聊天语义，
StateView 是真实的易用性胜利等）。但**累积效果是做一件事有 2–4 种方式
散布在文档、示例和绑定代码中**。v0.3.x 补丁不断堆叠；v0.3.x 取消轮次
（5 轮）使其可见，这种碎片化也是错误藏身之处——当"正确方式"在 N 个位置
中的 M 个位置时，第 N+1 个位置的遗漏就是静默无操作 / 忘记模式的错误。

架构细化（候选 1-3）将其折叠为：

  - **一种写节点的方式**（`run(NodeInput) -> NodeOutput` + 标签）。
  - **一种传递每次运行元数据的方式**（`RunContext` 参数）。
  - **一种取消的方式**（内部操作用 `token->fork()`，父令牌取消所有）。
  - **一种读状态的方式**（StateView 是标准方式；原始字典是逃生口）。
  - **一种运行的方式**（将 run / run_async 等折叠为一个接受流回调或返回
    迭代器的方法）。

这就是 v1.0 的约定——文档页面再次简短易读。

## 版本化策略

| 版本 | 范围 | 公开 API |
|---|---|---|
| **v0.4.x** | RunContext 作为*新*参数落地，旧方法弃用但仍可工作。CancelToken 获得 `fork()` 的纯新增。新的 `run(NodeInput)` 纯新增落地。 | 两种 API 均可调用。弃用警告。 |
| **v0.5.x** | 示例和 pybind 绑定迁移到新 API。旧 API 保持弃用。 | 两种 API 均可调用。更强的弃用警告 + 文档引导到新的。 |
| **v1.0.0** | 移除旧 API（8 个虚函数、thread_local 作用域、单处理器 CancelToken signal-on-self）。 | 单一标准 API。 |

理由：**不搞 v0.4 → v1.0 跳跃。** 两版弃用窗口让下游消费者（neoclaw、
NeoProtocol Executor、WASM spike、此仓库外的任何东西）一次迁移一个组件。
cibuildwheel 矩阵在窗口期间保持完整——每条发布路径 20 个 wheels 不变，
只是对旧方法的依赖逐渐减少。

如果迁移花费的时间比预期长（例如第三方 C++ GraphNode 子类很常见），
v0.5 变为 v0.5.x 并延长弃用，v0.6 延长窗口。仅当弃用警告已经在一个
版本中安静下来后才丢弃旧 API。

## PR 排序

每行是一个可合并的 PR。它们按顺序落地，全部在 master 上（没有长期特性
分支——项目的提交历史是直线型的，弃用策略意味着每个 PR 可以独立发布到
PyPI 作为 v0.4.0+i、v0.4.0+(i+1) 等）。

| # | PR | 范围 | 落地位于 |
|---|---|---|---|
| 1 ✓ | **`RunContext` 数据通道（内部）** — 落地 `a473f0e` | 在 `engine.h` 中添加 `struct RunContext`。引擎的 `execute_graph_async` 构建并通过它传递。NodeExecutor 将其传递给 `execute_full_async`。Pybind 包装它。**没有公开可见变更** — 旧方法仍然仅接收 `state`；新的 `ctx` 在分发路径中并存。ctest 442/442 + pytest 96/96 通过。Bench 中位数 5.365 µs（基线 5.285 µs，+1.5%）— 在 WSL2 ~3% 噪声范围内，在 5.185 µs 基线的 ±5% 带内。Pybind 包装推迟到 PR 7（绑定迁移），因为 PR 1 零 pybind diff。 | v0.4.0 |
| 2 ✓ | **`GraphNode::run(NodeInput) -> NodeOutput`** — 落地 `607ce66` | GraphNode 上的新虚函数。默认实现委托给旧的 8 个虚函数（优先级顺序保留）。注册为引擎的首选分发入口。现有 C++ 子类仍然编译 + 通过默认回退工作。ctest 442 → 445（3 个新的 NodeRunDispatch 测试）+ pytest 96/96 + 5 个实时 LLM/WS 通过。Bench 中位数 6.122 µs 对 PR1 基线 6.160 µs（Δ -0.6%）经过 A/B 10 轮（主机今天嘈杂，PR1 基线从昨天的 5.285 漂移到 6.160 — 相同代码，WSL2 抖动；A/B 比较消除主机漂移）。**捕获的陷阱**：`run(const NodeInput&)` 在 pybind 异步路径下在 asio 执行器内部 SEGV（协程引用参数释放后使用，v0.2.0 RunConfig 崩溃形态）。修复：按值接收 `NodeInput`。在 node.h 中文档化。 | v0.4.0 |
| 3 ✓ | **CancelToken `fork()` 纯新增** — 落地 `897645c` | 添加 `std::shared_ptr<CancelToken> CancelToken::fork()`。父 `cancel()` 级联到子。`add_cancel_hook` 继续工作（已弃用；`[[deprecated]]` 注释在 PR 4 中落地）。`run_sync(aw, cancel)` 切换到 `cancel->fork()`。单信号 `slot()` API 为引擎的外部 co_spawn 保留。ctest 445 → 452（7 个新的 CancelTokenFork 测试）+ pytest 96/96 + 5 个实时 LLM/WS 通过。Bench A/B 20 轮（双向交错）：Δ 最小值 +1.0%，Δ 中位数 +1.5% — 在 ±5% 带内；bench 路径没有 `cancel_token` 所以不触及 `fork()`，小增量是二进制布局噪音（PR3 bench 二进制比 PR2 小 3.7KB，布局不同）。 | v0.4.0 |
| 4 ✓ | **弃用注释** — 落地 `35a4517` | 在旧的 8 个虚函数 + `add_cancel_hook` 上添加 `[[deprecated]]`（它返回的 Hook 间接弃用）。跳板作用域（`CurrentCancelTokenScope` / `current_cancel_token()`）推迟——这是 PR 7（绑定迁移）用 `ctx.cancel_token` 读取替换的暗通通道，因此现在弃用它会在没有清晰迁移路径的情况下在每个暗通点强制抑制。内部调用点（graph_node.cpp 默认链、默认 `run()` 转发器）被新的 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 宏（api.h — GCC/clang/MSVC 可移植）包裹。用户代码重写弃用虚函数或调用 `add_cancel_hook` 会看到迁移警告；引擎内部保持干净。ctest 452/452 + pytest 96/96 + 5 个实时 LLM/WS 通过。Bench A/B 10 轮：Δ 中位数 +0.3%，最小值 +0.8% — 纯属性变更，布局噪音。`-Werror=deprecated-declarations` 未启用（CI 从未有 `-Werror`；警告在弃用窗口期间保持信息性）。 | v0.4.0 |
| 5 ✓ | **StateView 标准，原始字典弃用** — 落地 `f31aa53` | 在 pybind docstring 中将 `engine.get_state(thread_id) -> dict` 标记为软弃用。新的标准 = `get_state_view(thread_id) -> StateView`（已在 v0.3.2 中）。没有 `DeprecationWarning` 发出，没有 `[[deprecated]]` 注释——原始字典有合法用途（每通道 `version` 访问、快照序列化）。v1.0 将其保留为逃生口，除非软弃用产生大量反馈。零行为变更。ctest 452/452 + pytest 96/96 通过。 | v0.4.0 |
| 6 ✓ | **示例迁移** — 落地 `a2a24ef`（PR 6a，C++）+ `0a76e3a`（PR 6b，Python） | 7 个 C++ + 19 个 Python 示例（共 44 个 GraphNode 子类）切换到统一的 `run(NodeInput)` API。PR 6a 手动迁移；PR 6b 使用 AST 作用域的辅助工具进行安全的批量重写。冒烟运行与 v0.3.2 输出逐位匹配。ctest 452/452 + pytest 96/96 通过。 | v0.4.x（拆分为 6a + 6b） |
| 7 ✓ | **Pybind 绑定迁移** — 落地 `4e186a5` | `PyGraphNodeOwner` 现在重写 `GraphNode::run(NodeInput)` 并通过 `has_user_method` MRO 遍历分发到 Python 用户的 `run` 方法；当不存在时回退到旧链。将 `RunContext` / `NodeInput` / `CancelToken` 绑定到 Python（从包中重新导出）。暗通 `CurrentCancelTokenScope` 保留——旧链仍然为未迁移的用户代码安装它。PR 9 在删除旧 8 虚函数的同时删除它。ctest 452/452 + pytest 96/96 + 5 个实时 LLM/WS 通过；新的 `run(input)` API 端到端实践。 | v0.4.x |
| 8 ✓ | **文档重写** — 落地 `519a00b` | `docs/reference-en.md` §6 GraphNode 折叠为单一 `run()` 虚函数；§7 下新增 RunContext + CancelToken（含 `fork()` 示例）子节。README "与 LangGraph 的区别" 添加了一个"一个节点方法"条目指向 `run(input)`。`@ng.node` 装饰器的内部 `_DecoratorNode` 现在使用 `run()`，因此五秒演示通过新路径运行。concepts.md / troubleshooting.md 清扫推迟到 PR 9（在旧链被删除后它们明显过时）。 | v0.5.0 |
| 9 ✓ | **旧 API 移除** — 内置迁移 `d1070dc`；旧 GraphNode 链 `19819d8`；cancel hook `1d786a5`；thread-local/Python 旧桥接 `9e8e956`；过时的 Python 测试 `4392fbb`。 | v0.9.0 |

## 已完成的 v0.4.0 后计划（历史）

v0.4.0 于 2026-05-05 发布（`4cae42c`，标签 `v0.4.0`）。下述观察
窗口和破坏性移除均已完成；v0.9.0 于 2026-05-14 发布了移除。

### 阶段 A — 弃用窗口（已完成）

时长：数周 ~ 一个小版本周期。不更改引擎代码；此阶段存在是为了让弃用警告
有时间在 v1.0 删除底层代码之前暴露真实的下游破坏。

观察事项：

  1. **弃用可见性** — 用户真的看到旧的 8 虚函数 + `add_cancel_hook` 上的
     `[[deprecated]]` 警告吗？PR 4（`35a4517`）将内部调用点置于
     `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 之下，因此警告应**仅**来自
     用户重写点。Issue tracker / 讨论 / 直接反馈渠道中关于"这警告是什么？"
     的提及。
  2. **旧链回归** — 任何新发现的旧 8 虚函数默认链破坏的情况（静默无操作、
     遗忘的作用域等）。v0.3.x 有 5 轮此类情况；再出现一次是可能的。
  3. **下游消费者破坏** — 第三方 `GraphNode` 的 C++ 子类。此仓库轨道中
     已知的消费者：
     - `neoclaw` — `src/neoclaw_nodes.cpp:94` 仍有
       `std::vector<ChannelWrite> execute(const GraphState&) override`。
       必须在 v1.0 发布前自行迁移到 `run(NodeInput)`，否则 neoclaw
       在 v1.0 wheel 上损坏。
     - `NeoProtocol` Executor 运行时 — 使用 NeoGraph WASM 构建；
       推荐 v0.4.0 绑定测试。
     - WASM spike — 引擎零差异路径是 v0.3.x 基线；v0.4 run() 添加是纯
       新增，所以很可能没问题，但要验证。
  4. **新手陷阱面** — `ee11ed6` 新手清扫关闭了聊天演示会话中的 5 个陷阱。
     流式 / MCP / 异步扇出 / HITL resume 是演示未触及的路径；可能有类似
     的陷阱密度。通过全新的 `cibuildwheel` + 首次用户模拟，或通过单独的
     会话预处理来暴露。
  5. **可选的补丁发布** — 如果阶段 A 暴露了真实错误，发布 v0.4.x 补丁。
     如果在 v1.0 之前确实需要新功能，发布 v0.5.0 小版本（仍在弃用窗口内）。

退出标准：阶段 A 在弃用警告"在一个版本中安静下来"时结束——具体来说，
一个完整的小版本周期（例如 v0.5.0 发布）中零用户可见的与旧路径绑定的破坏。

### 阶段 B — 破坏性移除（在 v0.9.0 中完成）

子 PR 按以下顺序独立落地，因此每个步骤可以单独审查和回退。

| 子 PR | 范围 | 风险 | 涉及文件 |
|---|---|---|---|
| **9b** | 删除 `graph_node.cpp` 旧默认链（带 `ExecuteDefaultGuard` 递归检测的 8 虚函数交叉路由逻辑）；从 `node.h` 中删除 8 个虚函数声明；将 `src/core/deep_research_graph.cpp`（5+ 个子类）和 `src/core/plan_execute_graph.cpp`（3+ 个子类）中的内部节点从 `execute()` / `execute_full()` 重写迁移到 `run(NodeInput)`。 | **高** — 每个内部 GraphNode 子类必须在一个 PR 中迁移。内置节点已在 PR 9a（`d1070dc`）中迁移；这两个图工厂是最后持仓者，因为它们的节点是文件局部的，不在 `nodes/` 中。 | `node.h`、`graph_node.cpp`、`deep_research_graph.cpp`、`plan_execute_graph.cpp` |
| **9c** | 删除 `add_cancel_hook` + `Hook` RAII 类 + `hooks_` 成员 + `hooks_mu_` + `cancel()` 的钩子迭代循环。`cancel.h` 缩小为仅 `fork()` + `cancel()` + `is_cancelled()` + `slot()` + `bind_executor()`。 | **中** — `fork()` 是正式替代品，由 7 个 CancelTokenFork ctest 实践。失败模式是任何仍在调用 `add_cancel_hook` 的用户代码出现链接错误（在编译时捕获，非静默）。 | 仅 `cancel.h`（实现是仅头文件） |
| **9d** | 删除 `CurrentCancelTokenScope`（头文件 + 实现）+ `current_cancel_token()` thread_local 访问器 + `state.run_cancel_token_` 成员 + `set_run_cancel_token` / `run_cancel_token` / `run_cancel_token_shared` 访问器。`cancel.cpp` 变为空（文件可删除）。`RunContext::cancel_token` 现在是唯一路径。 | **中** — 每个内部暗通点必须已经读取 `ctx.cancel_token`（PR 7 绑定已完成；provider 侧读取需要审计）。失败模式：provider 仍然读取 `current_cancel_token()` 返回 null → 取消不传播到 LLM HTTP。 | `cancel.h`、`cancel.cpp`（删除）、`state.h`、`graph_state.cpp`，加上对 `provider/*` 的审计扫描 |
| **9e** | 删除 `PyGraphNodeOwner` 的 6 个旧 GraphNode 重写（`execute(GraphState&)`、`execute_full`、`execute_full_async`、`execute_stream`、`execute_full_stream`、`execute_full_stream_async`）— 仅保留 `run(NodeInput)` + `get_name()` + 析构函数。删除 `tests/test_node_default_dispatch.cpp` + `tests/test_node_async_default.cpp` + 它们的 CMakeLists 条目。 | **低** — 纯减法，没有逻辑可破坏。失败模式：任何仍然只定义 `execute()`（没有 `run()`）的用户 Python 类在分发时命中 NotImplementedError。阶段 A 应该已经暴露了这些情况。 | `bindings/python/src/bind_node.cpp`、`tests/CMakeLists.txt`、两个测试文件 |

9b–e 落地后：

  - **SOVERSION 引入**（不是"升级"——目前在任何 neograph_* lib 上
    没有 `set_target_properties(... VERSION ... SOVERSION ...)` 存在）。
    v1.0.0 是为 `libneograph_core` / `_llm` / `_postgres` /
    `_sqlite` / `_mcp` / `_a2a` / `_acp` 引入 SOVERSION 1 的自然时刻。
    验证 cibuildwheel 矩阵（manylinux soname 后缀、macOS install_name、
    Windows DLL——每个处理 SOVERSION 的方式不同；作为基准测试式验证，
    而非假设"CMake 属性 = 能工作"）。
  - **文档清扫** — `docs/concepts.md` "8 分发入口点"段落折叠为一个；
    `docs/troubleshooting.md` 删除旧链条目；README "与 LangGraph 的
    区别"变为"NeoGraph 的思维方式"（大多数 LG-delta 条目不再适用，
    因为差距已弥合）。
  - **v1.0.0 标签 → PyPI 发布** — 最后一步。回滚成本很高（撤回 PyPI
    发布 + 回退标签），因此在推标签之前验证完整的 ctest + pytest +
    5 个实时 LLM + cibuildwheel 20-wheel 矩阵通过。

### v0.4.0 后对此路线图的小修正

审计捕获了早期草案中的两个小不准确之处：

  - **PyGraphNodeOwner 旧重写数量是 6，而非 7。** 早期说明称"7 个重写
    移除，仅 run() 保留"。`bind_node.cpp:183` 的 `PyGraphNodeOwner` 中
    实际的 GraphNode 派生重写是 6 个（8 个 GraphNode 虚函数减去从未
    被重写的 `execute_async` 和 `execute_stream_async`——默认链处理
    它们）。9e 之后：`run()` + `get_name()` + 析构函数保留，而非仅
    `run()`。
  - **SOVERSION 是"引入"而非"升级"。** `CMakeLists.txt:5` 注释提到
    SOVERSION 但没有实际的 `set_target_properties(... SOVERSION ...)`
    调用存在。v1.0 是第一个设置它的版本。含义：cibuildwheel 矩阵运行
    需要验证当 SOVERSION 后缀出现在 Linux .so / macOS dylib install_name
    上时，wheel 布局不退化。

### 历史反事实："如果我们永远不移除呢？"

如果阶段 B 从未落地（旧链留在 v1.0+ 中），系统**不会崩溃**——每个
当前场景继续工作，所有 452 ctest 通过，弃用警告仅在用户重写点触发。
成本是结构性而非急性的：

  - **错误类别滋生地保持开放。** v0.3.x 的 5 轮取消传播补丁系列发生
    是因为相同的模式不得不穿过 8 个分发入口点 × 2 种语言。留下旧链
    使 M-of-N 遗漏错误对下一个横切关注点（deadline / trace_id / 指标
    句柄 / 可观测追踪）保持可用。
  - **`state.run_cancel_token_` 非通道集成员**在每次多 Send 扇出时丢失，
    除非显式转发。任何添加在这里的新每次运行字段都重复 v0.3.1 指针丢失
    错误。
  - **文档中的两个 API 表面** — 新手不读源码无法区分 `execute` 和 `run`；
    `ee11ed6` 新手清扫的 5 个陷阱正是这种文档缺口形态。
  - **SOVERSION 从未被干净引入。** 发行版打包者（Debian、Homebrew、
    conda-forge）将没有 SOVERSION 的库视为上游管理不善。
  - **警告疲劳。** 永久的弃用警告训练用户忽略它们，因此下一次真正的
    弃用会被淹没。

这些今天都没有破坏 v0.4.0。它们使每个未来的演化更慢且更易出错。
v1.0 承诺的"单一标准方式"是同时回答所有五个问题的答案。

## 每个 PR 的约定

每个 PR 必须：

  - **在合并时不破坏 ctest 442/442 + pytest 96/96**（构建中允许弃用
    警告，不允许错误）。
  - **不退化 bench**（`bench_neograph` seq 路径的中位数 µs/iter，按
    `feedback_wsl2_bench_isolation.md` 测量——全新工作树，taskset+chrt）。
  - **最多触及以下之一**：头文件表面 或 引擎内部 或 绑定 或 示例。
    混合 PR 使审查困难且回退昂贵。
  - **在合并时为此表添加一行** — 划线删除提议行，链接合并提交，注明任何
    范围漂移。

## 性能回顾 — `b59444f` 18 天潜伏平行回归

在 v1.0 周期接近结束时，README 的"引擎开销"吹嘘（par 11.8 µs）被发现
已损坏。测量 + 并行二分结果：单个提交 `b59444f` 是该回归，潜伏了 18 天
（2026-04-26 → 2026-05-13）。

### 发生了什么

- `b59444f` 将 `GraphEngine::compile()` 的默认工作器数量从 `1` 改为
  `std::thread::hardware_concurrency()`。意图：扇出节点无需显式配置即可
  获得真正的并行执行。
- 副作用：1 节点顺序 + 5 节点扇出微基准测试承担了额外的**每次迭代跨线程
  提交成本 ~75 µs/iter**。11.8 µs → 283 µs（24×）。
- 4 月 27 日性能审计（`project_perf_audit_2026-04-27.md`）记录 `fd60aab`
  为"修复"，但那是另一个回归（定时测量模式），且保持了工作器计数默认值
  不变。par 微基准测试本身在"default=hardware_concurrency"模式下测量，
  因此数值上看起来正常，但 README 实际的 11.8 µs 声称是 `b59444f` 之前
  的值。
- 尽管 v1.0 周期的每个 PR 约定要求"不退化 bench"，但当时的 bench 是对
  相同的（已退化的）基线测量的，因此落在 ±5% 带内并静默通过。潜伏了
  18 天。
- 2026-05-13，逐提交并行二分（`git worktree add` 11 个并行工作树，
  taskset+chrt 测量）确认 `b59444f` 是导致 par 11.8 µs → 283 µs 跳跃
  的单个提交。回退（`e5ecb08`）恢复到 11.8 → 12.2 µs。

### 权衡 — 为什么 default=1 是正确的

`asio::thread_pool` 跨线程提交成本约 75 µs/任务。如果单个节点的执行时间
在 ms 级（LLM 调用、HTTP 等），该成本消失在噪声中——但在 NeoGraph 的著名
"引擎开销顺序/并行 µs 级"路径上，它是同一量级且直接显现。

- **CPU 微小 / 顺序节点（微基准测试、验证器链等）** — default=1 压倒性
  地更好。在单个 io_context 线程上顺序执行，没有工作池。
- **真正扇出意图（模拟挂起阻塞、独立进程调用、同步 HTTP）** — 用户必须
  显式调用 `engine->set_worker_count_auto()` 或 `set_worker_count(N)`。
  一行代码。

为了使这一权衡显式化，`e5ecb08` 的提交消息 + 以下扇出示例 5 个站点
（10/14/21/36 + `deep_research_graph` 构建器）添加了显式的
`set_worker_count_auto()` 调用，且迁移文档的迁移 3 部分得到了充实。

### 每个 PR 约定加固（防止下一次回归）

仅检查 `bench_neograph par` 微基准测试是否在基线的 ±5% 内是不充分的——
当基线本身已退化时，它一起下滑。在后续补丁中：

  - bench-regression CI 使用**README 中声明的绝对值**（`seq ≈ 5.0 µs`、
    `par ≈ 12 µs` 等）作为第二道挂钟锚定门禁。捕获基线本身的回归。
  - 或添加一个 GitHub Actions cron 用于 master → master 7 天回归测量
    （夜间浸泡式模式）。
  - 如果每个 PR 的 diff 涉及 `GraphEngine::compile()` 或
    `set_worker_count`，PR 正文必须包含"独立的微基准测试测量结果"
    （通过 CODEOWNERS 钩子自动化）。

这三者都是后续工作。在 v1.0 发布前至少有一个必须落地。

### 我们学到了什么

1. **"默认值变更"可以是一个性能关键约定，即使它没有功能含义。** 如果
   README 的吹嘘数字来自"默认"路径，那么默认变更 = README 变更。
2. **回归测量的基线本身可能退化。** 不要只做 ±带比较；也要设置绝对值锚点。
3. **并行二分（11 个并行工作树 + 结果聚合）在 30 分钟内精确定位了 18
   天潜伏的回归。** 比线性二分快得多——当 master 增长过长时的默认工具。

## v0.3.x 陷阱记忆 — 重构期间需避免

构建/发布管道从 v0.1.x → v0.3.x 积累了雷区。每个雷区都有一个记忆条目——
当你触及相关区域时，此表就是清单：

| 陷阱 | 容易出问题的地方 | 记忆条目 |
|---|---|---|
| `NEOGRAPH_API` 宏需要在每个公共类 + 自由函数上 | 新的引擎子库（postgres / sqlite / mcp / a2a / acp）。Windows DLL 边界。 | `feedback_neograph_api_discipline.md` |
| 跨分支陈旧 .so 污染 | 跨分支使用 `BUILD_SHARED_LIBS=ON` build/ → ABI 不匹配，compile() 中 SEGV | `feedback_cross_branch_stale_so_trap.md` |
| 基准测量上的构建目录污染 | 长期存活的 build/ 目录产生比全新工作树构建更慢的二进制文件（+0.4 µs/iter 虚假信号） | `feedback_bench_build_dir_contamination.md` |
| WSL2 测量抖动 | 普通的"多次重复 + 中位数"不收敛——需要 taskset + chrt FIFO 99 | `feedback_wsl2_bench_isolation.md` |
| 注释中的 Doxygen `/*` 通配符 | `/**` 内部的 `fs/*` / `terminal/*` 打开嵌套注释，抑制后续诊断。使用 `&#42;` HTML 实体。 | `feedback_doxygen_slash_star_trap.md` |
| ASan `__cxa_throw` 拦截器 CHECK | 跨越 pybind 边界的 C++ 异常在 `LD_PRELOAD libasan.so` 下触发拦截器。在 CI 中按关键字排除；cancel/throw 正确性由 TSan + 实时 LLM 测试实践。 | （本次会话 — 在 feedback 中添加注释） |
| TSan eptr 生存期竞态 | NodeInterrupt 的 exception_ptr 跨越 co_await 边界触发 libstdc++ `__exception_ptr::_M_release`。修复：提取原因为 `std::string`，在主线程上抛出新的。 | `feedback_parallel_group_eptr_race.md` |
| MSVC 需要显式的 `<array>` / `<algorithm>` | libstdc++ 传递地引入它们；MSVC v143 不会。使用 `std::array` 等的测试文件静默破坏 Windows CI。 | （本次会话 — 在 feedback 中添加注释） |
| scikit-build-core 0.12.2 Windows single_config | `-G` 标志被检测到，环境变量被忽略 — Windows wheel 丢失了 SQLite=OFF 覆写。使用 `[[tool.scikit-build.overrides]]` + `cmake.define`。 | `feedback_libcurl_unconditional_dep.md` |
| Wheel OpenSSL CA 路径 | manylinux libssl 使用 AlmaLinux 路径，在 Ubuntu 上不存在。`__init__.py` 从 certifi 自动设置 `SSL_CERT_FILE`。 | `feedback_wheel_openssl_ca.md` |
| pyproject.toml 运行时依赖不在 CI 的 PYTHONPATH 流程中自动安装 | `pip install --quiet pytest` 行必须镜像 pyproject.toml 的 `dependencies = [...]`。v0.3.2 对 pydantic 失去了这一点。 | （本次会话 — 在 feedback 中添加注释） |
| `compile()` 默认工作器计数回归 | `b59444f` 将默认值从 `1 → hardware_concurrency` 改变，潜伏的 par 微基准测试 11.8 → 283 µs（24×）。基线本身退化模式。在 `e5ecb08` 中修复。 | "性能回顾"部分（上文） |

如果重构 PR 添加了一个新的子库、新的公共类、新的运行时依赖、跨越 pybind
抛出的新测试模式、新的 wheel 平台——先打开此表。v0.3.x 补丁系列的一半
是在重新发现已在此列表上的项目。

## 文档影响地图

当重构落地时，这些页面需要编辑：

  - **`README.md`** — "Python Binding"部分的 RunConfig 表格、"与
    LangGraph 的区别"差异（大多数条目变得过时，应删除而非编辑）、
    "绑定覆盖内容"表面列表。
  - **`docs/reference-en.md`**（1622 行）— GraphNode / Node /
    Provider / CancelToken / RunConfig 部分。大约 30-40% 重写。
    叙述式导览结构保留；API 表面缩小。
  - **`docs/concepts.md`** — 531 行概念叙述。"8 分发入口点"段落折叠
    为一个。取消传播段落清理。
  - **`docs/troubleshooting.md`** — 大多数 v0.3.x 条目变得过时。
    "静默无操作" / "忘记重写" / "thread_local 缺失"条目可以删除。
  - **`bindings/python/examples/`** — 每个示例（22 Python +
    30 C++）更新。
  - **`Doxyfile`** — 无需变更；PROJECT_NUMBER 从 pyproject.toml 读取，
    因此 v1.0.0 自动传播。
  - **`ROADMAP_v1.md`**（本文件）— 划线删除已落地的候选，添加比预期
    更难/更容易的事后分析。

## v1.0 完成的定义

  1. 对以下每一项有单一标准方式：写节点、取消运行、读状态、更新状态、
     运行图。
  2. README 的"Python Binding"部分新用户在 5 分钟内可读完。
  3. `docs/reference-en.md` GraphNode 部分是一个方法，而非八个。
  4. v0.3.x 弃用警告在最终移除之前已在一个版本中安静下来。
  5. ctest / pytest / 实时 LLM / Valgrind / Doxygen 在 v1.0 标签上
     全部通过。

---

# 研究轨道（负载低于以上 v1.0 细化）

## 候选 4 — 自演化 JSON agent v2（研究）

### 背景

`bindings/python/examples/22_self_evolving_graph.py` 证明闭环闭合：
LLM 修改器提出对运行中的图的 JSON 编辑，引擎重新编译，新图运行。
PoC 工作正常，但 LLM 在提出编辑时难以推理通道数据流——它"看不到"
哪些节点读/写哪些通道，因此其提议经常通过错误的线路路由数据。

### 研究方向

向修改器提示中显式暴露通道拓扑。两种形式需研究：

1. **提示中的拓扑摘要** — 引擎发出每个节点的规格如
   ``"节点 X 读取 {a,b}，写入 {c}"``，源自编译后的通道访问模式。
   修改器提示在 JSON 定义旁边接收到此信息。

2. **每阶段通道提议** — 修改器按*阶段*提议通道（拆分 / 合成等），
   而非作为扁平集合。引擎组合检查每个提议阶段的通道集是否与上游/
   下游阶段一致。

### 为什么它不是一个 v1.0 必选项

- 对已发布的引擎不是用户阻塞点——PoC 的缺口在*提示工程*，
  而非引擎。
- 在证明任何引擎侧表面变更的合理性之前，需要 LLM 评估工具集
  （每种拓扑变体的正确率、成本、编辑周期-收敛）。
- 一旦评估显示哪些自省对 LLM 实际上有用，可能折叠为更广泛的
  "图自省 API" v1.x 特性。

### 成本

- 如果研究验证了它，引擎表面添加（拓扑访问器）是微小的。
- 大部分工作是 LLM 侧的实验，在此仓库的热路径之外。

### 触发轮次

TODO_v0.3.md 项目 #8 — 从 v0.3.x 中推迟为研究，不是用户阻塞点。

## 候选 5 — Cookbook 轨道：pgvector RAG 示例

### 背景

`bindings/python/examples/`（23 个示例）涵盖 ReAct、HITL、意图路由、
多 agent 辩论、深度研究（网页抓取 / 网页搜索）、自演化图等——但**没有
向量检索 / RAG** 示例。确认未合并到 16/17：那些是网页研究，而非基于
嵌入的检索。

RAG 是最常见的 LLM 模式之一；缺失对评估 NeoGraph 的用户来说是一个真实
的可发现性缺口。

### 为什么它是 Cookbook 条目，而非引擎关注点

引擎表面按现状已足够——`PostgresCheckpointStore` 已经带来了一个嵌入 +
pgvector 节点可以复用的连接池 / 配置故事。无需引擎 API 添加；这项
工作纯粹是一个示范示例（~150-200 行）：

  - `EmbeddingNode` — 调用 OpenAI 嵌入或本地模型。
  - `RetrieveNode` — 针对预填充表的 pgvector 相似性查询。
  - `RAGCallNode` — 使用连接起来的检索上下文进行 LLM 调用。
  - 一次性索引设置脚本（与示例主体分离，因此示例不会每次运行都重新
    索引）。

### 为什么从 v0.3.x 推迟

v0.3.x 系列的范围围绕 FastAPI SSE 聊天演示反馈暴露的引擎错误 / 易用性。
RAG 不是引擎错误；它是"常见模式需要一个示范示例。"属于一个独立的
Cookbook 节奏，每个条目是一个真实世界的配方，而非版本升级。

### 触发轮次

TODO_v0.3.md 项目 #9 — 确认为 Cookbook 材料（无引擎缺口），推迟到未来
"示例轨道"清扫。

---

## 候选 6 — Provider 单一分发

### 症状

`Provider` 暴露四个虚方法（比 GraphNode 的八个少一个维度）：

```
complete           complete_async
complete_stream    complete_stream_async
```

`(同步/异步) × (流式/非流式)`——与候选 1 相同形态，相同的"至少重写 N 个
中的一个"约定，相同的陷阱。非流式对是安全的（仅一步桥接深）。流式对在
#10 之前不安全：`complete_stream` 是同步 httplib，默认
`complete_stream_async` 桥接内联包裹它（且 WebSocket Responses 路径在
引擎的 io_context 工作器之上嵌套 `run_sync`），当从
`GraphEngine::run_stream_async` 内部调用时产生间歇性段错误（issue #4，
由 PR #10 的工作线程桥接 + `SchemaProvider` 原生重写修复）。

### 为什么这是清理，而非阻塞

具体的崩溃（#4）已关闭——PR #10 为同步 `complete_stream` 生成专用工作
线程并将 token 分发回等待者的执行器；`SchemaProvider` 重写了 WS 路径以
完全跳过工作线程。`OpenAIProvider` 和 `SchemaProvider` HTTP/SSE 路径都
继承了安全的默认实现，因此 4 虚函数笛卡尔积不再容易崩溃。剩下的是架构
瑕疵：重写面比必要的更宽，桥接的安全性取决于你重写了笛卡尔积的哪个角落
（一个在编译时没有任何东西固定的不变式）。

### 落地方向

一重写路径是纯新增而非替代：

```cpp
class CompletionProvider : public Provider {
public:
    asio::awaitable<ChatCompletion>
    invoke_request(CompletionRequest request);

protected:
    virtual asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) = 0;
};
```

`CompletionRequest` 显式选择收集模式或流式模式，而不从回调存在性推断
传输方式。最终适配器保留所有现有 `Provider` 入口点。这使新实现有一个
重写入口，同时保持现有源码和二进制约定不变。

### 相邻项 — `schema_provider.cpp` 拆分

`schema_provider.cpp` 约 1,800 行，集中了多供应商 schema 映射 +
HTTP/SSE 线路 + 主体构建 + 响应解析。单一分发的重写是将其拆分为
`SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl` 的自然时刻。
在此提及以便不需要单独的 ROADMAP 条目；如果工作在不同的 PR 中进行，
可以拆分。

### 触发轮次

Issue #5 — 在调试 #4 时浮现。具体崩溃在 PR #10 中关闭；架构清理通过
纯新增的 `CompletionProvider` 路径和永久兼容性策略落地。

### 落地日志（v0.9.0 候选周期）

5 个 PR 依次在 master 上落地，2026-05-13 中期：

| PR | 范围 | 落地位于 |
|---|---|---|
| **#40 (PR1)** | 添加新虚函数 `Provider::invoke(params, on_chunk = nullptr)`。默认实现转发到 4 个旧虚函数链（所有现有 Provider 子类行为不变）。6 个新 ctest。 | v0.9.0 |
| **#41+#42 (PR2)** | 引擎内置 LLM 节点（`LLMCallNode`、`IntentClassifierNode`）通过 `provider->invoke(params, on_chunk)` 分发。PR #41 仅在堆叠基上合并，然后通过 PR #42 重新应用到 master。 | v0.9.0 |
| **#43 (PR3)** | 迁移所有引擎内部同步 LLM 调用点——`agent.cpp`（5 个点）、`deep_research_graph.cpp`（6 个点）。为 `Provider::invoke()` 默认实现添加线程局部取消传播等效性（重现旧 `complete()` 的 `current_cancel_token()` 行为）。3 个新 ctest。 | v0.9.0 |
| **#44 (PR4)** | 将全部 4 个旧虚函数标记为 `[[deprecated]]`。将 `plan_execute_graph.cpp` 中的 3 个点迁移到 `invoke()`。将 `OpenInferenceProvider` 和 `RateLimitedProvider`（装饰器）的 4 虚函数重写块包装在 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 中——阻止内部转发器警告；只有面向用户的重写/调用点发出警告。 | v0.9.0 |
| **#45 (PR5)** | C++ 示例迁移（`31_local_transformer.cpp`、`cookbook/ai-assembly/member_server.cpp`）。 | v0.9.0 |

### 纯新增兼容性路径（2026-07 修订）

保留现有 `Provider` 虚函数表并添加单独的 `CompletionProvider`。新实现
接收显式的 `CompletionRequest` 并仅重写 `do_invoke()`。现有四个虚函数和
基于回调的 `invoke()` 通过最终适配器接线到新实现；现有 Provider 子类和
Python 跳板保持原样。

现有虚函数作为稳定 API 继续支持，无移除计划。兼容性和安全性修复继续
适用，但无义务将所有新功能回移植到现有四个虚函数。新实现和新直接调用
应分别使用 `do_invoke()` 和 `invoke_request()`。此策略不更改现有公共
签名、虚函数顺序、对象大小或虚函数表。#127 的原生异步传输和操作所有权
与此 API 策略是分开的。

后续：

  - **6b**：新的 Provider 对 `CompletionProvider` 编写。不要更改现有
    内置的直接继承，因为会影响 ABI。
  - **6c**：完成原生异步传输和请求拥有的取消 / 生命周期。
  - **相邻项**：将 `schema_provider.cpp`（1800 行）拆分为
    `SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl`
    （与上述 6b 同一 PR，或在实现时决定分开）。

---

## 候选 7 — gRPC 传输（可选加入组件）

### 背景

HasMCP 的冷邮件（2026-05-15）不是触发器，但那封邮件给出了一个免费的
行业信号："gRPC 是下一个传输方向。" MCP 社区正在讨论[将 gRPC 添加为
标准传输](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/966)，
Google 正在研究 gRPC 作为原生 MCP 传输。gRPC 几乎与 NeoGraph 的 4 轴叙事
的每个轴对齐——protobuf 二进制序列化（性能 / 轻量级）、HTTP/2 多路复用
（多租户连接成本）、原生双向流式（token / 事件）、以及 schema 强制 +
小线路（嵌入式）。

### 决策（2026-05-15）

- **不自行实现。** 通信协议带有显著的重新发明轮子风险——使用标准
  `grpc++` + `protoc`。
- **仅可选加入。** `NEOGRAPH_BUILD_GRPC` 选项，**默认 OFF**。grpc++
  引入 protobuf + abseil + c-ares + re2 + zlib（数十 MB 传递依赖），
  破坏了"2 依赖 / 仅 libc.so.6 / 1.2 MB 二进制"的轻量级轴。默认 OFF
  是阻止它的唯一方法，并应用 `cmake-option-default-flip-trap`（EDDSkills，
  本次会话新添加）纪律：`find_package(Protobuf/gRPC)` 仅在选项门内；
  不翻转默认值。
- **NeoGraph 原生 API，独立于 MCP 标准。** `proto/neograph.proto` =
  `GraphService { RunGraph（一元）/ RunGraphStream（服务器流）/
  Health }`。payload 是 JSON 字符串（保留图即数据的属性——如果我们用强类型
  proto 消息建模，每个用户图变更都将重新生成 .proto）。一旦 MCP-over-gRPC
  标准确定，在此服务旁添加 MCP 形态的服务（此服务不变）。

### 已落地（v0.9.x 周期，脚手架）

- `NEOGRAPH_BUILD_GRPC=OFF` 选项 + 条件 `find_package` + 致命守卫。
- `proto/neograph.proto`、`src/grpc/graph_service.cpp`（哈希键编译缓存—
  复用 multi_tenant_chatbot cookbook 模式）、
  `include/neograph/grpc/graph_service.h`（`NEOGRAPH_HAVE_GRPC` 守卫）、
  `examples/52_grpc_server.cpp`。

### 已验证 — 首次 grpc++ 装备的构建（2026-05-16）

在安装 apt `libgrpc++-dev protobuf-compiler-grpc`（1.51.1）+ protoc 3.21.12
之后，使用 `-DNEOGRAPH_BUILD_GRPC=ON` 构建，端到端通过：
  - `neograph_grpc` / `example_grpc_server` / `example_grpc_client`
    全部编译和链接成功。
  - C++ 客户端 → 服务器：**Health**（ok/version/default_graph）、
    **RunGraph** 一元（`{"text":"hello from grpc"}` →
    `"HELLO FROM GRPC"`，trace=[upper]）、**RunGraphStream**
    （5 个事件，FINAL payload，status OK）。`RESULT: PASS (failures=0)`。
  - protoc 代码生成路径（原始 `add_custom_command`）工作。**修复了一个错误**：
    在 VERBATIM 模式下 `ARGS --proto_path="${dir}"` 中的引号被字面传递，
    因此 protoc 看见 `"…/proto"`（含引号）作为目录 → "directory does not
    exist"。移除引号（`--proto_path=${dir}`）修复了它。

### WSL Windows-PATH 泄漏陷阱（可复现 — 构建环境警告）

在此环境（WSL2，大量 Windows PATH 泄漏）中启用 grpc++ ON 构建时捕获到
两种污染。它们不会在干净的 Linux 主机 / CI 上出现，但 WSL 开发人员会遇到：

  1. **anaconda re2** — 当 `gRPCConfig.cmake` 执行 `find_package(re2)` 时，
     如果不存在系统 re2 cmake 配置（未安装 apt `libre2-dev`），它从
     PATH 中拾取 `/mnt/c/ProgramData/anaconda3/Library/lib/cmake/re2/
     re2Targets.cmake`（Windows）并在 `set_target_properties` 中出错。
     修复：`-DCMAKE_IGNORE_PREFIX_PATH=/mnt/c;…` + `-DCMAKE_IGNORE_PATH=…
     /anaconda3/Library/lib/cmake;…` → grpc 回退到系统 pkg-config re2
     （"Found RE2 via pkg-config"）。
  2. **ZLIB include** — `FindZLIB` 从系统（`/usr/lib/.../libz.so`）拾取
     库，但从 PATH 中的 `/mnt/c/gtk/include`（Windows zlib.h）拾取
     `ZLIB_INCLUDE_DIR` → `-isystem /mnt/c/gtk/include` 泄漏到每个
     grpc 链接的目标 → `/mnt/c/gtk/include/libintl.h` 将 `printf` 重写
     为 `libintl_printf` 宏 → `std::printf` 编译错误。修复：显式设置
     `-DZLIB_INCLUDE_DIR=/usr/include
     -DZLIB_LIBRARY=/usr/lib/x86_64-linux-gnu/libz.so`。

  → 两者都是 `cmake-option-default-flip-trap` 的表亲（环境泄漏将
  `find_package` 拖到错误的前缀）。添加了 EDDSkills SKILL
  `wsl-windows-path-cmake-find-leak`（2026-05-16）。

### NexaGraph 前身分析 — gRPC-MCP 的真正 ROI 是检查点

NeoGraph 的前身 NexaGraph（`/root/Coding/NexaGraph`）早已实现并运行了
gRPC-MCP。调查发现（Explore，2026-05-16）：

- **实现内容**：`proto/rag_service.proto`（RAGService，11 个一元 RPC —
  vector_search / graph_search / ingest / chat history / image task /
  **graph checkpoint** 5 个 RPC）、`src/nexagraph/grpc_client.cpp`
  完全实现，通过 `GRPC_TARGET` 环境变量在生产环境中从 api_server.cpp
  集成。服务器是双传输（HTTP JSON-RPC + gRPC 50051）。无流式（全部一元）。
- **开销减少声明**（`DOCS/grpc-client-plan.md`）：序列化 1ms→0.01ms、
  嵌入 1536d 15KB→6KB、每次请求新连接 → HTTP/2 多路复用。**无测量——
  仅设计理由。**
- **诚实评估**：对于典型的 MCP 工具调用，LLM 推理（数百毫秒）占主导，
  因此 1ms 序列化节省是噪声。gRPC 的增益*实际存在*的领域是**大型结构化
  payload**——嵌入向量、RAG 导入，尤其是**图检查点**
  （`channel_values_json` + `channel_versions_json` 每步增长很大）。
  小型工具元数据 / 字符串查询 <1%（认知复杂度不值）。换句话说，并非
  "MCP 总体上更快"，而是仅限于"大 payload MCP"。

**关键发现 — 引入 NeoGraph 时重新排序优先级：**

1. **gRPC CheckpointStore = 真正的 ROI（最高优先级候选）。** NexaGraph
   的 `grpc_checkpoint.cpp` 已经**继承自 `neograph::graph::CheckpointStore`**
   — 从那时起它就使用 NeoGraph 的检查点抽象。换句话说，几乎可以直接移植，
   形式为在 NeoGraph 的 `Postgres/Sqlite CheckpointStore` 旁边添加
   `GrpcCheckpointStore`（~150 行）。大 payload + 独立于（MCP #966）
   标准 + 自然适合刚构建的 `neograph::grpc` 组件。检查点是每步的大 JSON
    blob，这是唯一热路径中 gRPC 二进制增益实际上可测量的地方。
2. **MCP-over-gRPC 传输（通用工具调用）= 较低优先级。** LLM 占主导，
   因此增益小 + MCP-over-gRPC 标准尚未确定（#966）。即使标准确定后，
   也仅对 RAG / 嵌入等大 payload 工具有意义。

### GrpcCheckpointStore — 已落地 + 已测量（2026-05-16）

添加到 `neograph::grpc`：`GrpcCheckpointStore`（客户端，继承自
`CheckpointStore` — 与 NexaGraph 相同）+
`CheckpointServiceImpl`+`run_checkpoint_server`（服务器，包装任意
`CheckpointStore` 后端）+ `checkpoint_to/from_json` 辅助函数。
`CheckpointService` proto 中的 5 个 RPC。NeoGraph 丰富字段的往返保留
（next_nodes 向量 / `CheckpointPhase` 枚举 / `barrier_state` 嵌套映射 /
`schema_version`），这是 NexaGraph 的扁平映射无法处理的 — 示例 54 正确性
通过。

**测量结果（example_grpc_checkpoint，1536 维嵌入 + 12 轮，200 迭代，
localhost 环回）— 关闭"看似合理但未证实"。不过诚实地说，有一半被否决：**

| 指标 | 值 |
|---|---|
| JSON（checkpoint_json） | 29,080 B |
| Protobuf 线路（CheckpointBlob） | 29,131 B |
| 名义 JSON-RPC 信封 | 29,155 B |
| protobuf / JSON-RPC payload | **99.9%** |
| InMemory 进程内 | save 27 µs / load 36 µs |
| gRPC 往返 | save 720 µs / load 755 µs |
| gRPC 网络开销 | **+693 µs save / +719 µs load** |

**诚实结论 — NexaGraph 的"序列化 15KB→6KB 二进制压缩"声明在 NeoGraph 的
JSON-in-proto 设计中未得到满足（payload 99.9% 相同）。** 原因：为了图即
数据的鲁棒性，整个检查点被打包为单个 proto 字符串字段 → protobuf 字段级
压缩不适用。NexaGraph 有每个成员一个字段的 proto，因此它能压缩，但每次
检查点格式漂移都需要重新生成 proto（每次添加 `next_nodes` /
`barrier_state` / `schema_version` 时）。换句话说，**我们在此权衡中
有意选择 schema 稳定性而非压缩，因此 payload 增益确实是 0
（已证实：按设计不获益）。**

gRPC 的*实际*增益仅在传输层面——HTTP/2 连接复用（消除了 JSON-RPC /
HTTP1.1 的每次调用连接）。单个环回往返 +700 µs 没有展示这一点
（增量仅在负载下 / 远程 RTT 下出现）。换句话说，**传输增益仍然是负载
测试依赖的——单次测量不能证实。**

→ 优先级重新确认：
  - **GrpcCheckpointStore 的真正价值 = "通过类型化 RPC + HTTP/2 连接复用
    的远程检查点" + "多语言：任何语言服务器都可以实现 `CheckpointService`"**。
    而非 NexaGraph 宣称的 payload 压缩。将其作为 cookbook 发布，但诚实
    的卖点不是"压缩"，而是"类型化远程检查点，agent 进程中零 DB 驱动"。
  - **MCP-over-gRPC 传输（通用工具调用）= 搁置**。如检查点测量所证实，
    JSON-in-proto 设计下 payload 压缩不适用，因此如果工具调用遵循相同
    设计，压缩增益为 0 + LLM 占主导。仅在标准（#966）确定且按字段建模
    合理的大型二进制工具（原始嵌入等）时重新考虑。
  - 剩余验证：在负载下（N 级并发检查点保存），HTTP/2 多路复用是否真的
    对每次调用连接产生增量——bench 作业候选（持续，非单次）。

### ToolCalling：JSON-RPC 与 gRPC 面对面（2026-05-16）

用户请求——不是检查点，而是*工具调用*本身，对真实服务器上的两种传输进行
面对面比较。`proto` 获得 `ToolService.CallTool`，示例 55 在两者上启动
**相同的计算函数：(a) httplib JSON-RPC 2.0 `tools/call`（MCP 形态，
HTTP/1.1 keep-alive）(b) gRPC ToolService（HTTP/2）**，并测量两者。

**诚实事件 — "gRPC 快 70 倍"是测量伪影。** 第一次运行：JSON-RPC p50
43 ms（无论 payload 大小都恒定）。43 ms = 教科书式的 TCP delayed-ACK
定时器签名。原因：`CPPHTTPLIB_TCP_NODELAY` 默认 **false** → Nagle 开启，
gRPC 默认 TCP_NODELAY 开启 → 不公平。将"gRPC 快 70 倍"照单全收地提交将
是一个谎言。在两侧应用 `Server/Client::set_tcp_nodelay(true)` 并重新测量。

**公平条件下的结果（环回，两侧 keep-alive + NODELAY，N=300 p50，2 次复测）：**

| payload | gRPC p50 | JSON-RPC p50 | 比率 |
|---|---|---|---|
| 微小参数（~30 B） | 433 / 448 µs | 436 / 410 µs | **0.99–1.09×（平手）** |
| 1536-float（~12 KB） | 655 / 680 µs | 1079 / 1016 µs | **0.61–0.67×（gRPC ~1.5×）** |
| 参数线路（微小） | 42 B | 118 B | 信封开销 |
| 参数线路（12 KB） | 12025 B | 12100 B | **99%（压缩 0）** |

**事实：**
- **小工具调用（大多数真实工具调用）：JSON-RPC ≈ gRPC 平手。**
  传输切换 ROI ≈ 0。
- **大 payload 工具调用（~12 KB+，嵌入 / RAG 块返回）：gRPC ~1.5×。**
  NexaGraph 提到的领域，但 1.5× 而非 70×。
- Payload 压缩仍然是 0（JSON-in-proto，与检查点测量一致）。
- 环回天花板——在真实网络上，RTT 对两侧同等增加，比率进一步向 1 收敛。
  1.5× 是最佳情况。

**候选 7 最终裁决：**
- gRPC 的 ROI 是 (1) **多语言边车 / 远程类型化 RPC**（语言边界），
  (2) **大 payload 工具 / 检查点上的 ~1.5×**。通用工具调用的大规模
  迁移没有价值（平手 + 标准 #966 尚未确定）。
- MCP-over-gRPC 传输：**搁置，已证实。**"通用 MCP 工具调用变快"
  被测量推翻（平手）。仅在标准确定后，且仅对嵌入密集型工具有意义。
- Nagle 事件 → EDDSkills SKILL 候选 `bench-shock-number-nagle-first`
  （令人震惊的传输基准数字 = 首先怀疑 TCP_NODELAY / Nagle /
  delayed-ACK；`perf-regression-bench-bisect` 的表亲）。
  用户批准后添加。

### 为什么 NeoGraph JSON-RPC 与 gRPC 打平 — yyjson（已证实）

用户见解："JSON-RPC 用 yyjson 解析所以很快；结构上 gRPC 必须赢。"
在示例 55 中添加了剥离传输的纯编解码器微基准测试来验证：

| 12 KB payload，仅编解码器，5000 迭代 | µs |
|---|---|
| yyjson 解析+转储 | **38.9** |
| protobuf 序列化+解析 | **1.75** |
| → yyjson / protobuf | **22.3× 更慢** |

**用户完全正确。** protobuf 在结构上是快 22 倍的编解码器。但在往返中，
差异稀释为 12 KB 时的 1.5×——序列化差距 ~37 µs 仅占完整往返 692–1096 µs
的一小部分（其余是套接字 I/O / 系统调用 / HTTP 帧）。**量化证据表明工具
调用的热路径由套接字 I/O 主导，而非编解码器。**

关键含义——**NeoGraph 的 JSON-RPC 与 gRPC 打平归功于 yyjson，而非 JSON-RPC
协议快。** 使用典型技术栈（Python 的 `json` 比 yyjson 慢约 50×，对于 12 KB
约 2 ms），编解码器主导往返 → 在那里 gRPC 结构上占主导。只有 NeoGraph 使用
yyjson，因此避免了该陷阱。

→ 这是一个隐藏的卖点，也是搁置候选 7 的*最终*理由："其他框架有 JSON
解析瓶颈，因此 gRPC 传输对它们至关重要，但 NeoGraph 的 MCP / JSON-RPC
不是，因为有 yyjson。" 具体而言，对 NeoGraph，MCP-over-gRPC 的 ROI 甚至
更低（编解码器优势已被 yyjson 抵消）。gRPC 仅用于多语言 / 远程边界 +
大 payload 上的 ~1.5× 目的——已证实。

### NexaGraph 二次收获 — 历史压缩 + GrpcRemoteTool（2026-05-16）

在完整 NexaGraph 调查之后，除了已移植的 `GrpcCheckpointStore`，另外移植了
三个*通用、非重复、NeoGraph 中尚未存在*的项目。（特定于 RAG 应用的
stdio / HTTP MCP 在 `proto/rag_mcp_server/backend` 中被排除，因为 NeoGraph
已经拥有它或它是特定于应用的。`DOCS/graph-engine-design.md` 实际上是
NeoGraph 的设计祖先，因此它不是"移植"目标。）

1. **`neograph::history`（新的核心工具，纯新增）** — 从 NexaGraph 的 CAF
   `compress_history` actor 移植核心，剥离了 actor 外壳：
   - `compact_history(messages, Provider&, model, max_tokens=12000,
     recent_keep=6) -> awaitable<CompactedHistory>` — 当 token 估计超出
     预算时，用单个 LLM 调用汇总（系统 1 + 最后 N）之间的部分，用
     system-summary 消息替换它。`co_await provider.invoke()`（不使用已弃用
     的 `complete()`，零异步库依赖——核心内部已使用协程）。
   - `sanitize_tool_calls(messages&)` — 一项 NeoGraph **完全缺失**的防御：
     两遍移除由截断破坏的 OpenAI tool-pairs（有 tool_call 无响应的
     assistant / 有 tool 消息无调用的响应），幂等。`compact_history` 在
     内部将其应用于输出 → 压缩结果永远不产生 400。
   - `estimate_tokens` — 保守的 ~3 chars/tok 估计（混合 KO / EN）。
   - 示例 56 `history_compaction`（离线 MockProvider，无需密钥）—
     sanitize 3→1、compact 29 msgs/975 tok → 6 msgs/208 tok、
     原始不变验证通过。`src/core/history.cpp` 在所有配置下构建到
     `neograph_core` — 496/497 ctest 通过（1 个失败 = 预先存在的
     `pybind_smoke` openinference 模块缺失，不相关）。

2. **`neograph::grpc::GrpcRemoteTool`** — 示例 55 是通过 gRPC *导出*工具
   的一侧（`run_tool_server`），这是它的镜像——将远程
   `ToolService.CallTool` *导入*为普通 `neograph::Tool` 的一侧。移植了
   NexaGraph 的 `GrpcTool` 适配器。pimpl（公共头文件无 grpc++ 依赖，
   与 `GrpcCheckpointStore` 相同姿态）。由于简单 proto 没有 list-tools
   RPC，定义通过构造函数注入。服务器错误 → 重新抛出为 `runtime_error`
   （工具错误，非传输错误——与本地 Tool 相同的约定）。示例 57
   `grpc_remote_tool` — 服务器线程 + `Tool&` 多态调用 + 错误路径通过。
   **gRPC ROI #1（多语言远程类型化 RPC）的消费者侧实现** — 从 agent 的
   视角，进程边界工具在调用点与本地工具无法区分。

### 剩余（仍待处理）

  - 向 CI 添加 `grpc-build` 作业（apt deps + ON 构建 + `example_grpc_client`
    / `server` 冒烟——在干净的 ubuntu runner 上，上述 WSL 陷阱不适用）。
  - `RunGraphStream` 的 `ServerWriter::Write` 在流式节点回调内部被调用——
    当前假定单个超级步骤循环线程。在多工作器扇出图中，如果回调在工作
    线程上调用，则需要 `ServerWriter` 同步（gRPC `ServerWriter` 不是
    线程安全的）。当前示例是单节点，因此这不暴露。
  - TLS / auth：记录用户接线而非 `run_server` 的不安全默认值。
