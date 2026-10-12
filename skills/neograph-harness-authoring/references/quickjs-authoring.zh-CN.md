<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=zh-CN source_sha256=53dc89ee33f26b7d0f9c6d328453f73aaaf9e53f0b8b4cea360ca1cdc2b800c8 -->
# QuickJS 源码编写

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## 确定宿主的契约

写代码之前，先弄清下面这些绑定。使用已提供的配置，或者检查可用的 schema；
不要用看似合理的猜测去填补缺失的名称。

| 契约 | 需要什么 |
|---|---|
| JS API manifest | 本次构建中，构建器和命令的确切签名 |
| Registry | 节点/reducer/条件/import 的名称、节点配置的 schema 和副作用 |
| Invocation | 输入 JSON 的形态，以及 callCore 可用的 Core 名称 |
| Results | Core 通道的名称，以及生成器必需的最终输出 |
| Children | 已准入的绑定名称、子 Program 的输入/输出，以及被授予的额度 |
| Compiler bridge | 接受的源码信封、诊断，以及剩余的修复机会 |

内置的 JS manifest 列出的是语法，而不是应用的节点。例如，
probe.node 属于能力测试的节点清单；它不是应用的 LLM 节点。固定的能力探针检查的是它自己用例专属的图/命令契约，
而不是任意的生产行为。

## 编译期的图与运行时的 Program

**define()** 是同步的、编译期的函数，返回一个开放的构建器。
用这个构建器创建节点和边。它的方法返回的是构建器，而不是节点句柄。
节点的行为来自宿主注册的 C++ 实现；普通的 JS 回调不是图的节点。

**main(input)** 是可选的、同步的生成器，在稍后运行。纯 JS 可以选择分支、
构建数组和循环、转换 JSON。运行时的副作用，使用由生成器 yield 出来的封存的 ng 命令。
不要在 define() 里调用运行时命令，不要修改已发布的图，不要返回形似图的对象，
也不要使用 async 函数、promise、定时器、require()、环境层面的 I/O、eval 或动态 import。

这个示例只演示管道的连接，假定宿主已经注册了探针节点清单，并要求 input.value 是数字。
它的节点什么都不做，所以它演示的是数据流，而不是 LLM 的动作，也不是某个具名能力用例的答案：

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

在 callCore 里，要严格使用宿主提供的图名称。"main"、"example"、"capability"，
以及节点的名称，都不是通用的别名。

## 数据流与拓扑

正常的 callCore 输入，是从通道名到传入值的映射。每个声明过的通道都会应用它注册的 reducer。
正常的 Core 结果包含序列化后的通道；要读取通道的值，而不是它外面的那层包装。

例如，聊天机器人现有的模板，是这样调用它的 answer 节点的：

~~~javascript
const reply = yield ng.callCore(
  "main", {payload: {phase: "answer", task: task}}, "answer"
);
const answer = reply.channels.result.value;
~~~

这里的 payload/result 是声明过的通道，chat.step 读取 payload.phase。
这些名称和行为来自那个宿主的注册表和模板。把这个片段复制到另一个注册表里，
并不会凭空产生这些绑定。

只有声明、没有 main() 的模块，保留 Core 结果的形态。有 main() 时，
生成器的返回值必须符合另行准入的 Program 输出契约。
能力评测器有时会提供 {accepted: true} 这样的合成命令响应；
请使用该用例所规定的响应契约，不要给合成的响应再加一层 Core 通道的包装。

| 意图 | 构造方式与要点 |
|---|---|
| Linear path | 添加节点、入口/出口，以及每一条连接的边 |
| Conditional routing | conditionalEdge(from, registeredCondition, routes)；把条件的每个标签都映射到一个节点 |
| Static fan-out/fan-in | 添加向外的各个分支和向内的 join 边；join 需要等所有分支时，添加 barrier(joinNode, branchNames) |
| Parallel writes | 选择能处理并发写入的宿主 reducer/通道；仅靠边并不能定义如何合并 |
| Graph interrupts/retry | 使用 manifest 中的 interruptBefore/After 和 retryPolicy 键；它们与 JS 的循环或逻辑上的重试是两回事 |

## 组合运行时命令

- callCore(coreName, input, site) 产生一个命令；yield 它就会执行对 Core 的调用，
  并返回结果。
- all(commands, {max_in_flight: N}, site) 接受封存的命令。用普通的 JS 构建这个列表，
  然后只 yield 一次 all 命令。不要 yield 原始的数组，不要把命令变成 Promise，
  也不要对封存的命令使用 yield*。
- spawn(binding, input, site) 选定一个宿主准入的子绑定。要等待它的结果，
  把这个命令包在 await(spawnCommand, timeoutMs, site) 里。
- checkpoint(state, site) 发布显式的 JSON 状态。它本身不会编译、
  准入或替换任何东西。
- emit 和 cancelScope 具有 manifest 中声明的语义。hostCapability
  需要一个已准入的 import 槽位；它不是通向任意原生 API 的途径。

例如，已知一个已准入的绑定及其确切的原始输入：

~~~javascript
const result = yield ng.await(
  ng.spawn(binding, originalChildInput, "child:spawn"),
  timeoutMs,
  "child:wait"
);
~~~

发出的子 Program ID 本身并不是封存的 await 命令。请使用宿主实际的
join/恢复契约。需要跨越一次替换保留子 Program 时，参见 [runtime-handoffs.md](runtime-handoffs.md)。

source-site 标签是持久坐标的一部分。要从稳定的任务标识或确定的序号推导它们；
避免使用时钟或随机值。已完成的操作在重放时必须保持它们原来的输入。JS 循环、
重试、并行分支或新的子 Program，都不会补充预算。

基于 accepted=false 这类成功结果的循环，是逻辑上的重试。
失败的 Core 命令是 Program 的一种结果；不要以为普通的 JS
try/catch 能让它继续。请使用宿主的失败/恢复/对账契约。

## 输出、编译与修复

使用当前所处场景要求的信封：
- 源码求值：恰好一个 JSON 对象，其中的 source 字符串是完整的模块。
  不要 Markdown 围栏、补丁、ProgramBundle JSON 或说明性文字。
- Harness MCP：先通过 neograph_schema 发现接口。把模块放在
  harness.mode="javascript"、source_id 和 source 之下，同时带上所有必需的
  任务/worker/预算/策略字段。只有 source 并不是完整的 MCP 请求。
- 宿主原生的提案：遵循那个宿主的 schema。这个 skill 没有定义通用的、自由格式的
  编译或替换 RPC。

通过提供的编译器桥提交。在评测模式下，由宿主提交返回的源码并返回诊断；
并不意味着有可供模型调用的工具。
在 MCP 模式下，只有该工具可用时，才使用 neograph_compile。

失败时，找到报告的错误码、路径和 source site，修复被违反的契约。例如：
- 未知的 node/reducer/condition：与注册的绑定和配置保持一致；
- Core 绑定错误：让所有命令中的图名称与已准入的图名称保持一致；
- yield 的不是命令：产生一个封存的命令，或一个已准入的结构化 join；
- 输出/输入错误：纠正通道映射，或 Program 的结果契约；
- 缺少边/屏障：修复降级之后的拓扑，而不只是改源码的措辞。

在现有的修复机会之内，返回完整的替换源码。
不要抹掉诊断，不要放宽授权，不要加入无关的操作；在编译器和任务专属的检查都通过之前，不要声称成功。

仓库检出中的能力桥，可以这样查看和使用：

~~~text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
~~~

构建目标：program_dsl_capability_probe。具名用例需要提供它们各自的契约；
仅仅编译成功，并不表示某个用例通过了，探针成功也不授予生产环境的执行权限。运行脚本
scripts/run_dsl_capability_eval.ts 会执行有界的模型/诊断迭代。
