<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=zh-CN source_sha256=2b43b221e43adde43479b04affc783981618f19917ad94444136151aaf21275e -->
# QuickJS 源码编写

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## 名称取自宿主

请求和所提供的清单（manifest）给出你可以使用的全部名称：节点类型、归约器、条件、
图（Core）名称、子绑定、导入槽位、输入形状，以及 `main` 必须返回的内容。清单只列出
语法（签名、选项键、限制），不包含应用节点。缺少所需名称时，请说明缺的是哪一个。
仅仅看起来合理的名称就是错误的名称。

图名称也是其中之一。宿主给出名称时，请把这个字符串原样传给 `ng.graph(...)` 和每一个
`ng.callCore(...)`；不要根据任务内容自己编一个。

## 模块

使用全局 `ng` 的 ES 模块。它导出 `define`；任务需要运行时控制时，还导出生成器
`main`。没有 `require`、`module.exports` 和 `export default`，动态 `import()` 会被拒绝。
静态 `import` 只对宿主已封存并在任务中给出名称的模块有效，其他任何 `import` 都会被拒绝。

    // GRAPH、REDUCER、NODE_TYPE 代表宿主提供的名称。
    export function define() {
      const g = ng.graph("GRAPH");
      g.channel("value", {reducer: "REDUCER", initial: 0});
      g.node("step", {type: "NODE_TYPE"});
      g.entry("step");
      g.exit("step");
      return g;
    }

    export function* main(input) {
      const result = yield ng.callCore("GRAPH", {value: input.value}, "run");
      return {value: result.channels.value.value};  // 图运行的输出
    }

不要使用 `async`、Promise、定时器、`eval`、`Date` 或 I/O：`Date` 和 `eval` 会抛出异常，
`setTimeout` 不存在，`Math.random()` 始终返回 0。返回的值和每个命令的输入都必须是
纯 JSON：不能有 `undefined`、函数、`Date` 或 `Map`。

## define()：图

`define()` 是同步函数，返回 `ng.graph(name)` 给出的构建器。`ng.callCore` 之类的命令
在这里不存在。

- 构建器的每个方法都返回同一个构建器，从不返回节点句柄。请严格使用清单中的签名和
  选项键。键采用 snake_case（是 `initial_delay_ms`，不是 `initialDelay`）。未知的键在
  `channel`、`retryPolicy` 和命令选项中会被拒绝；`node` 配置中多出的键会传给节点类型，
  是否接受由宿主的注册表决定。
- 图需要 `entry`；没有它就不会运行任何东西，编译器会拒绝。`exit(node)` 标明运行在哪里
  结束（没有出边的节点同样会结束运行）。每条边、屏障、中断和重试都必须指向已声明的
  节点。`__start__` 和 `__end__` 不能作为节点名，但边或路由的目标可以是 `__end__`。
- `node(name, {type, ...config})`：`type` 必填且必须已注册。
  `channel(name, {reducer, initial})`：指定已注册的归约器。
- `conditionalEdge(from, condition, routes)`：`routes` 把已注册条件可能返回的每个标签，
  且只把这些标签，映射到节点。
- 扇出就是从一个节点引出多条边。必须等待所有分支的汇合还需要
  `barrier(join, [branchNodes])`。对同一个通道的并发写入需要一个能合并它们的归约器。

## main()：运行时命令

`main` 是同步生成器。每个 `yield` 恰好向运行时交出一个由 `ng` 构造函数创建的命令，
并取回该命令的结果。原始数组、普通对象、Promise 以及对命令使用的 `yield*` 都会被拒绝。用分支、
循环、`map` 等普通 JavaScript 构建命令和命令数组；要同时运行多个命令，请只 `yield`
一次 `ng.all(commands, {max_in_flight: N})`。

| 构造函数 | 含义 |
|---|---|
| `ng.callCore(name, input, site)` | 运行图。`name` 是传给 `ng.graph` 的字符串，`input` 把通道名映射到值。图运行的输出通过 `result.channels.NAME.value` 读取。 |
| `ng.all(commands, options, site)` | 每次同时运行 `max_in_flight` 个命令，按顺序返回结果。`ng.parallel` 与之相同。 |
| `ng.race(commands, options, site)` | 最先出结果的获胜；至少需要两个命令。 |
| `ng.quorum(commands, k, site)` | `k` 个成功后即完成；`1 <= k <= commands.length`。 |
| `ng.join(commands, mode, options, site)` | 通用形式。`mode` 为 `"all"`、`"race"` 或 `"quorum"`（配合 `required_successes`）。`failure_policy` 为 `"fail_fast"` 或 `"collect"`。 |
| `ng.spawn(binding, input, site)` | 启动宿主批准的子项。要取得其结果，用 `ng.await(spawnCommand, timeoutMs, site)` 包住它。 |
| `ng.emit(value, site)`, `ng.checkpoint(value, site)` | 把一个 JSON 值记入运行日志。 |
| `ng.cancelScope(scope, reason, site)` | 取消 `"current"`、`"nested"`、`"root"` 或 `"run"`；其他名称会使运行失败。在 `main` 的最上层，它以已取消状态结束运行。 |
| `ng.hostCapability(slot, input, site)` | 调用宿主批准的导入槽位（整数）。 |

- `site` 是可选的最后一个参数，是形如 `"item:" + id` 的稳定标签。请由 id 或索引生成，
  不要由时间或随机数生成。恢复后的运行会重新执行 `main`，所以对相同的结果必须产出
  相同的命令。
- 选项键采用 snake_case。`max_in_flight`、`required_successes` 和 await 超时都是正整数。
- 只要有一个命令失败（哪怕只是 `ng.all` 的一个成员），运行就会结束，`main` 不会再被
  恢复，所以包在 `yield` 外面的 `try`/`catch` 无法恢复它。逻辑重试应根据结果的某个字段
  （例如 `done` 标志）循环。对失败节点的重试属于 `define()` 中的 `retryPolicy`。
- 按任务所述的形状读取结果。如果任务说响应带有 `status` 之类的字段，那么该字段就是
  `result.status`；`channels` 包装只属于图运行的输出，不要把它加到任务另有描述的响应上。
- 只有 `define()` 的模块返回 Core 结果。有 `main` 时，其返回值必须与宿主批准的输出契约
  一致。

## 回复与修复

对于源码评估，回复一个只有一个字符串字段 `source` 的 JSON 对象，整个模块放在该字段中。
请做 JSON 转义（`\n`、`\"`），花括号前后不要放任何内容：不要 Markdown 围栏、说明文字、
补丁或 Program JSON。

    {"source": "export function define() {\n  ...\n}\n"}

对于 Harness MCP，先调用 `neograph_schema`，然后以 `harness.mode: "javascript"` 发送
模块，并附上 `source_id`、`source` 以及所有必需的任务、工作者、预算和策略字段。只有源码
不构成请求。仅当 `neograph_compile` 工具可用时才使用它。

编译器拒绝时会返回诊断，其中有 `code`、`message`、指向编译后定义的 JSON `pointer` 和
`witness`；宿主自己对结果做的检查则可能只返回一条普通的错误信息。请修复拒绝所指出的原因，
保留任务已确定的内容，并在同一封装中返回完整的模块。
不要扩大权限，不要添加无关操作，在宿主的检查接受之前不要声称成功。能够编译的源码
仍可能构建出错误的拓扑。
