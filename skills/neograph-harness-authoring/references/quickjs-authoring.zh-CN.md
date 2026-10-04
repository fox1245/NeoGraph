<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=zh-CN source_sha256=53dc89ee33f26b7d0f9c6d328453f73aaaf9e53f0b8b4cea360ca1cdc2b800c8 -->
# QuickJS 源码编写

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## 确定宿主契约

编写代码之前，先明确以下绑定。使用提供的配置或检查可用模式；不要用看似合理的猜测填补缺失名称。

| 契约 | 需要明确的内容 |
|---|---|
| JS API 清单 | 此构建中准确的构建器和命令签名 |
| 注册表 | 节点、归约器、条件、导入的名称，以及节点配置模式和效果 |
| 调用 | 输入 JSON 的结构，以及 callCore 可调用的 Core 名称 |
| 结果 | Core 通道名称，以及生成器必须给出的终态输出 |
| 子项 | 已准入的绑定名称、子项输入/输出和授予的限制 |
| 编译器桥接 | 接受的源码封装、诊断和剩余修复额度 |

内置 JS 清单列出的是语法，而不是应用节点。例如，probe.node 属于能力测试节点集；它不是应用的 LLM 节点。固定能力探针检查的是对应测试用例的图/命令契约，而不是任意生产行为。

## 编译时图与运行时 Program

**define()** 是同步的编译时函数，返回一个开放的构建器。使用该构建器创建节点和边。它的方法返回构建器，而不是节点句柄。节点行为来自宿主注册的 C++ 实现；普通 JS 回调不是图节点。

**main(input)** 是可选的同步生成器，稍后才运行。纯 JS 可以选择分支、构建数组和循环，以及转换 JSON。运行时效果通过生成器 yield 的密封 ng 命令执行。不要从 define() 调用运行时命令、修改已发布的图、返回形似图的对象，也不要使用异步函数、Promise、定时器、require()、环境 I/O、eval 或动态导入。

以下衔接示例假定宿主已经注册探针节点集，并要求 input.value 为数值。该节点不执行任何操作，因此示例展示的是数据流，而不是 LLM 操作或某个具名能力测试用例的答案：

~~~javascript
export function define() {
  const g = ng.graph("example");
  g.channel("value", {reducer: "probe.overwrite", initial: 0});
  g.node("step", {type: "probe.node"});
  g.entry("step");
  g.exit("step");
  return g;
}

export function* main(input) {
  const result = yield ng.callCore("example", {value: input.value}, "copy:value");
  return {value: result.channels.value.value};
}
~~~

在 callCore 中严格使用宿主提供的图名称。"main"、"example"、"capability" 和节点名称都不是通用别名。

## 数据流与拓扑

普通 callCore 输入是通道名称到传入值的映射。每个声明的通道应用其注册的归约器。普通 Core 结果包含序列化通道；读取通道的 value，而不是其包装对象。

例如，聊天机器人现有模板这样调用回答节点：

~~~javascript
const reply = yield ng.callCore(
  "main", {payload: {phase: "answer", task: task}}, "answer"
);
const answer = reply.channels.result.value;
~~~

这里 payload/result 是已声明的通道，chat.step 读取 payload.phase。这些名称和行为来自该宿主的注册表/模板。把此片段复制到其他注册表中，并不会创建这些绑定。

仅包含声明的模块保留 Core 结果结构。存在 main() 时，生成器返回值必须符合单独准入的 Program 输出契约。能力评估器有时会提供合成命令响应，例如 {accepted: true}；应使用该用例说明的响应契约，不要为合成响应添加 Core 通道包装。

| 意图 | 构造方式与需注意的细节 |
|---|---|
| 线性路径 | 添加节点、入口/出口，以及所有连接边 |
| 条件路由 | conditionalEdge(from, registeredCondition, routes)；将每个条件标签映射到一个节点 |
| 静态扇出/扇入 | 添加两条出向分支和入向汇合边；汇合要求所有分支完成时，添加 barrier(joinNode, branchNames) |
| 并行写入 | 选择能处理并发写入的宿主归约器/通道；边本身不定义合并方式 |
| 图中断/重试 | 使用清单中的 interruptBefore/After 和 retryPolicy 键；它们与 JS 循环或逻辑重试相互独立 |

## 组合运行时命令

- callCore(coreName, input, site) 生成一个命令；yield 该命令会执行 Core 调用并返回结果。
- all(commands, {max_in_flight: N}, site) 接受密封命令。使用普通 JS 构建列表，然后一次 yield 该 all 命令。不要 yield 原始数组、将命令转换为 Promise，或对密封命令使用 yield*。
- spawn(binding, input, site) 选择一个宿主已准入的子项绑定。需要等待结果时，将该命令包在 await(spawnCommand, timeoutMs, site) 中。
- checkpoint(state, site) 发布显式 JSON 状态。它本身不会编译、准入或替换任何内容。
- emit 和 cancelScope 遵循清单声明的语义。hostCapability 要求已准入的导入槽位；它不是访问任意原生 API 的通道。

例如，给定已准入的绑定及其准确的原始输入：

~~~javascript
const result = yield ng.await(
  ng.spawn(binding, originalChildInput, "child:spawn"),
  timeoutMs,
  "child:wait"
);
~~~

输出的子项 ID 本身不是密封的 await 命令。使用宿主实际提供的汇合/恢复契约。替换期间需要保留子项时，参见 [runtime-handoffs.md](runtime-handoffs.md)。

源码位置标签参与持久化坐标。根据稳定任务标识符或确定性索引生成它们；不要使用时钟或随机值。重放时，已经完成的操作必须保留原始输入。JS 循环、重试、并行分支或新子项都不会补充预算。

根据成功返回的结果（例如 accepted=false）进行的循环属于逻辑重试。Core 命令失败是 Program 结果；不要假定普通 JS try/catch 可以恢复它。使用宿主的失败/恢复/对账契约。

## 输出、编译与修复

使用当前接口要求的封装：
- 源码评估：恰好一个 JSON 对象，其 source 字符串包含完整模块。不要使用 Markdown 围栏、补丁、ProgramBundle JSON 或解释性文字。
- Harness MCP：先发现 neograph_schema。将模块放在 harness.mode="javascript"、source_id 和 source 下，同时提供所有必需的任务、工作者、预算和策略字段。仅有源码不是完整的 MCP 请求。
- 宿主原生提案：遵循该宿主的模式。本技能没有定义通用的自由格式编译或替换 RPC。

通过提供的编译器桥接提交。在评估模式下，宿主提交你返回的源码并返回诊断；这不意味着提供了模型可调用的工具。在 MCP 模式下，只有 neograph_compile 可用时才使用它。

失败时，定位报告中的代码、路径或源码位置，并修复违反的契约。例如：
- 未知节点/归约器/条件：匹配注册的绑定和配置；
- Core 绑定错误：使各命令中的名称与已准入的图名称一致；
- yield 的内容不是命令：生成一个密封命令或已准入的结构化汇合；
- 输出/输入错误：修正通道映射或 Program 结果契约；
- 缺少边/屏障：修复降低后的拓扑，而不只是源码措辞。

在现有修复额度内返回完整替换源码。不要抹去诊断、扩大授权、添加无关操作，或在编译器和任务专属检查接受之前宣称成功。

可按以下方式检查并使用当前检出版本中的能力桥接：

~~~text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
~~~

构建目标：program_dsl_capability_probe。具名用例需要各自提供的契约；仅编译成功不代表用例通过，探针成功也不授予生产执行权限。运行器 scripts/run_dsl_capability_eval.ts 执行有界的模型/诊断迭代。
