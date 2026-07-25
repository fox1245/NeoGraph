<!-- neograph-i18n: source=TODO_v0.3.md locale=zh-CN source_sha256=ee4fb3a3df1268f70a1cf98004b3825f972ee0f84ad3de77e0887a39e7bb80c5 -->
# v0.3 后续事项

**Languages:** [English](TODO_v0.3.md) | [한국어](TODO_v0.3.ko.md) | [日本語](TODO_v0.3.ja.md) | [简体中文](TODO_v0.3.zh-CN.md)

源自 FastAPI SSE 聊天演示反馈（2026-05-04）。
v0.3.0 发布了取消传播功能；本文件追踪剩余的心智模型与易用性差距。

## ✅ v0.3.1 已关闭（2026-05-04，第二轮）

1. **同一 `thread_id` 自动恢复检查点**——引入可选参数
   `RunConfig.resume_if_exists`。当设为 True 且配置了检查点存储时，
   `engine.run/run_async/run_stream` 会加载 `thread_id` 的最新检查点，
   然后通过通道 reducer 将 `input` 叠加到之上（因此 APPEND 归约的
   `messages` 会在新一轮中增长）。默认 `False` 保持现有的全新启动行为。
   测试：`tests/test_resume_if_exists.cpp`（6 个）+
   `bindings/python/tests/test_resume_if_exists.py`（6 个）。
2. **为流式专用节点提供更好的错误提示**——
   `GraphNode.execute()`（Python 基类）现在会遍历子类 MRO 查找
   `execute_stream` / `execute_full_stream`，若其中任一定义，
   则 `NotImplementedError` 中包含指向
   `engine.run_stream() / run_stream_async()` 的提示。
   测试：`bindings/python/tests/test_streaming_only_error_hint.py`（4 个）。
3. **Token 发送辅助函数**——
   `from neograph_engine.streaming import emit_token` 将原来的
   4 行 `GraphEvent` 构造仪式简化为
   `emit_token(cb, self._name, token)`。
   测试：`bindings/python/tests/test_emit_token_helper.py`（5 个）。
4. **README "与 LangGraph 的区别" 章节**——在 Python Binding 部分下新增。
   指出：可选的多轮恢复、`update_state(channel_writes)` 形式、
   `get_state` 嵌套字典、Python `Provider.complete` 仅限单次、
   流式专用节点需 `run_stream*`，以及新的 `emit_token` 辅助函数。
   同时将 `resume_if_exists` 添加到 `RunConfig` 表格。
7. **并行 / Send 分支的取消传播**——已验证静态并行通过共享父状态
   （v0.3.0 中已正确）。发现并修复了多 Send 间隙：动态扇出工作器
   从 `serialize/restore` 构建隔离的 `GraphState`，但
   `run_cancel_token_` 位于通道集之外并被丢弃——因此已取消的运行
   在 Send 生成的分支上仍会泄漏成本。新增了
   `GraphState::run_cancel_token_shared()`，且
   `NodeExecutor::run_sends_async` 现在将其转发到每个隔离的
   `send_state`。
   测试：`tests/test_cancel_token_propagation.cpp`（3 个——静态并行、
   多 Send、扇出中途中止）。

## 状态：v0.3.x 反馈已关闭

v0.3.x 反馈批次（FastAPI SSE 聊天演示 + 易用性轮次）中所有影响引擎
的事项已落地。剩余事项 #9（pgvector RAG 示例）纯属示范示例——不存在
引擎差距——归属于未来 Cookbook 轨道，记录为 `ROADMAP_v1.md` 中的候选 5。
v0.3.x 系列已关闭；后续引擎工作目标为 v0.4 / v1.0。

## ✅ v0.3.2 已关闭

9. **pgvector RAG 示例 → ROADMAP cookbook 轨道**——确认不存在引擎差距
   （现有 `PostgresCheckpointStore` 基础设施已足够；RAG 节点纯属用户代码）。
   记录为 `ROADMAP_v1.md` 中研究/Cookbook 部分下的候选 5，与 #8 位于同一
   区域。属于未来的 Cookbook 节奏而非引擎版本升级系列。

6. **`get_state` 字典形式的扁平 `StateView` 辅助类**——
   `engine.get_state_view(thread_id)` 返回 Pydantic v2 ``StateView``，
   通道作为顶层属性存在（``view.messages`` 而非
   ``state["channels"]["messages"]["value"]``）。基类通过
   ``extra="allow"`` 允许任意通道名称——适用于任何图，无需用户声明的模型。
   通过带声明字段的子类化 ``StateView`` 实现类型化访问；不匹配会引发
   pydantic ``ValidationError`` 而非静默类型强制转换。
   ``view.raw`` 保留未展平的字典以供需要版本 / 元数据的调用者使用。

   Pydantic v2 已添加为硬依赖（这是现代 LLM Python 的基础——FastAPI、
   LangChain、数据模型库都在使用）。

   测试：``bindings/python/tests/test_state_view.py``（12 个）。

8. **自演化图 v2 → `ROADMAP_v1.md` 中的研究轨道**——
   拓扑感知修改器提示词方向记录为该文件中的研究候选 #4。
   引擎侧改动一旦 LLM 评估显示出哪些自省真正有效，预计很小；
   从 v0.3.x 中推迟，因为对已发布的引擎来说，这不是用户阻塞点。

5. **`update_state` 同时接受 dict 和 `list[ChannelWrite]`**——
   v0.3.1 README 描述（"channel_writes 是一个 ChannelWrite 列表"）实际
   有误：引擎仅接受 JSON 对象，因此传入列表会**静默无操作**（C++
   `is_object()` 检查拒收了它）。Pybind 绑定现在根据输入形式分发：
   - `dict` `{channel: value}` → 现有路径（LangGraph 的
     `values={...}` 形式，关键字参数名不同）。
   - `list[ChannelWrite]` → 归约为 dict（每个通道最后一次写入胜出）；
     鸭式 `.channel`/`.value` 对象同样接受。
   - 其他类型引发 `TypeError`，因此静默无操作的陷阱不会重现。

   README "与 LangGraph 的区别" 部分已修正。
   测试：`bindings/python/tests/test_update_state_shapes.py`（11 个）。

10. **仅有 `execute_stream` 的节点通过 `run_stream` 分发**——
    同时在 Python 绑定层和 C++ 引擎层修复。

    **Python**：`PyGraphNode::execute_full_stream` 现在在回退到
    `execute_full` 之前先查询 `execute_stream`，因此仅重写
    `execute_stream(state, cb)` 的 Python 节点在
    `engine.run_stream()` / `run_stream_async()` 下正常工作。
    v0.3.1 中 `GraphNode.execute()` 的提示消息不再误导。

    **C++**（姊妹修复）：仅有 `execute_stream` 重写的 C++ 子类遇到
    相同问题——默认 `GraphNode::execute_full_stream` 先调用
    `execute_full`，后者通过 `execute` / `execute_async` 默认链传递
    并触发 `ExecuteDefaultGuard` 的递归检查。它抛出的 `runtime_error`
    在 `result.writes = execute_stream(state, cb)` 运行之前逃逸。通过
    引入 `GraphNodeMissingOverride`（继承自 `runtime_error` 以保持向后
    兼容）修复——默认递归守卫抛出此专用类型，而
    `execute_full_stream{,_async}` 两个默认值仅捕获*此*类型并回退到
    `execute_stream{,_async}`。真正用户抛出的错误不受影响。

    优先级顺序（保持不变，两种语言一致）：execute_full_stream
    → execute_stream → execute_full → execute。

    测试：
    `bindings/python/tests/test_execute_stream_dispatch.py`（5 个）、
    `tests/test_execute_stream_only_dispatch.cpp`（2 个）。
