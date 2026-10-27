<!-- neograph-i18n: source=docs/PROGRAM_RECURSIVE_HARNESSES.md locale=zh-CN source_sha256=5d1cc97f588ff33d80a58bcc32293e4f8f8a566cc239fd48c9be9f17ce504d14 -->
# 递归智能体：各自独立的 Harness generation

**Languages:** [English](PROGRAM_RECURSIVE_HARNESSES.md) | [한국어](PROGRAM_RECURSIVE_HARNESSES.ko.md) | [日本語](PROGRAM_RECURSIVE_HARNESSES.ja.md) | [简体中文](PROGRAM_RECURSIVE_HARNESSES.zh-CN.md)

参考场景把一个逻辑会话作为一棵父/子/孙的树，运行在同一个 owner 之下。
每个智能体都有自己的 ProgramVersion、Core 拓扑、状态、预算和谱系。
子智能体可以通过主编排器所用的同一个、由宿主持有的合成边界，
为它自己的子智能体提出 JavaScript。

参考测试执行四个互不相同的 Core 计划：主 Harness 中有 1 个节点，原来的子智能体有 2 个，
替换后的子智能体有 3 个，孙智能体有 4 个。子智能体是在它的孙智能体仍然存活的时候被替换的。
主编排器通过它已有的 await，收到替换后的子智能体的结果。
SQLite 和 PostgreSQL 的进程退出测试，会在嵌套替换提交之后重新打开整棵树，
并在不重新运行已完成工作的前提下运行完毕。

## 标识与存储

`ProgramRunRecord::logical_run_id()` 和 `ProgramHandle::logical_run_id()` 给出稳定的智能体标识。
每次替换仍然会创建一个不同的物理运行和不可变的 ProgramVersion。
由谱系来选定当前生效的 generation。要拿到该 generation 的句柄，
用 `reconnect(owner, logical_run_id)`。

运行记录 schema 4 在 `logical_run_id` 与物理运行不同时携带它。子关系仍然指向它们原来的子智能体标识；
它们保留第一次准入的链接和调用。当某次替换给出终态结果时，关系会在记录该结果实际的运行/版本标识的同时，
记录 `terminal_generation`。转换后端会对照已提交的子谱系、generation 和终态运行记录，来检查这一点。

schema 2/3 记录仍然可读，不含新字段的普通记录保持 schema 3 的表示。

## 带后代的替换

对一个运行中的智能体族做替换，需要用到拥有它的运行时，以及该智能体生成器中
一个被持有的、已完成的顶层检查点。这个检查点可以属于某个嵌套的智能体。目标必须已经准入。

同一个事务发布新的 generation，并复制已有的子关系和已提交的后代预算。
父智能体的标识和绝对的子深度不能改变。目标必须保留各子智能体的能力/副作用授权和保证下限。
嵌套的替换必须保持父智能体的结果契约。被保留的子智能体要求绑定名称唯一；
已中断的子智能体，必须先有明确的处置，才能替换它的父智能体。

旧的执行直接退役，不会发出逻辑智能体的终态 Hook，也不会取消已转移的子智能体。
逻辑上的子并发和配额清理转移给后继者；针对具体尝试的清理仍然会执行。
已经在等待该子智能体的父智能体，会跟随它后继者的结果；通过该子智能体句柄发起的取消，
沿着同一条链传递。根 generation 的句柄保持它原有的 generation 语义；
要拿到当前生效的根，请使用返回的替换句柄，或重新连接逻辑标识。

只有出现在不可变的 generation 创建发布内容中的子智能体，才是继承来的绑定。
已有的子智能体，可以通过完全匹配的绑定和输入重新接上：

```javascript
// Original child Harness:
const worker = yield ng.spawn("grandchild", {}, "child:spawn");
yield ng.checkpoint(
  {child: worker.child_run_id, replacement: "child-v2.json"},
  "child:swap"
);
```

```javascript
// Replacement Harness: this resolves the inherited child invocation.
const result = yield ng.await(
  ng.spawn("grandchild", {}, "replacement:join"),
  30000,
  "replacement:await"
);
return {generation: 2, result};
```

替换不会重新创建那个子智能体，不会改写它的调用，也不会获得另一份子智能体授权。
旧 generation 中尚未挂接的合成绑定，不会变成新的权限。全新的绑定仍然走正常的合成和准入路径。
源码提案必须与独立审阅过的宿主模板实例一致；参考宿主不会仅仅因为某个智能体把源码写进了检查点，
就批准它。

## 进程丢失之后恢复预先准入的子智能体

能够恢复已准入的子绑定的宿主，可以显式允许尚未完成的 JavaScript `spawn`/`await` 命令，
重新连接到已发布的子智能体：

```cpp
config.recover_existing_child_commands = true; // default: false
```

这适用于普通的静态绑定，以及通过替换继承来的子智能体，不需要合成网关。
父关系、链接回执、owner、确切的调用、子深度，以及已持久化的子运行，必须相互一致。
静态命令必须根据其已记录的命令坐标，推导出同样的子 ID；继承来的子智能体，
保留 generation 创建发布内容中准入的那个 ID。历史上的 Program 版本和宿主授权必须仍然可用。
已配置的宿主准入解析器，仍然照常为子智能体的这次尝试做准入。

恢复使用专门的重连路径：检查之后才消失的子智能体，不会转而创建新的子智能体。
缺失的运行、被改变的输入或不可用的绑定，会让已有的命令保持待处理状态，等待对账。
命令的预留和后代的核算在恢复之后依然保留。这个开关并不授权重放状态未知的
provider、工具、native 或其他外部副作用。

与这个开关无关，重放一次正在运行的尝试时，会根据其确切的 journal 值和原始的命令预留，
补完尚未完成的、封存的 `ng.checkpoint`。这不执行任何外部分发。
其他状态未知的副作用，仍然会让整个智能体族暂停，等待对账：
被中断的子智能体仍是一个已分发、可恢复的关系，
它的父智能体保留正在进行中的命令预留，而不是取消这个关系，也不会把这笔预留重复入账。

如果在存储仍然可用时，结果发布失败，已分发命令的 outcome 和待处理的副作用，
会同时发布为 `Ambiguous`；取消无法抹去这种不确定性。乐观的命令 head 读取冲突会被重试，
而针对未变化的父 head 的永久拒绝，不会无限期地阻塞子智能体的完成通知。

## 回答被中断的子程序

因中断而暂停的子 Harness（其 Core 图的某个节点抛出 `NodeInterrupt`，或图声明了 `interruptBefore`）本身就是一个可恢复的 run。回答它只有一种方式：用子的 run id 恢复子。即 `ProgramRuntime::resume(owner, child_run_id, ...)`，在 Python 中是 `host.resume(child_run_id, value)`。该 id 在父的持久关系 `ProgramRunRecord::children()`（Python 中为 `host.run_record(parent_run_id).children`）里，暂停的子在那里处于 `Dispatched` 状态，并带有它的 `Interrupted` 结果。这对由宿主用 `start_child` 启动的子、由 `main()` spawn 但不 await 的子，以及由 `main()` await 的子都适用。

`main()` await 子的父（`ng.await(ng.spawn(...))`）以 `Interrupted` 结束，绝不会是 `Failed`：它的结果显示子的问题，关系保持 `Dispatched`，await 在途的预留仍由父保留。只有当 run 停在待处理命令自己的 Core 调用中、且停在该调用写下的检查点时，才会释放该命令的预留；await 子的命令是在等待子的 run，所以保留预留。问题属于子，因此恢复父（`resume`）或对账（`reconcile`）都会被 `P_CHILD_INTERRUPTED` 拒绝；witness 中的 `child_run_ids` 指出应改为恢复哪些子，且不会消耗任何东西。

子在得到回答并结束后，运行时会继续父。新的尝试（attempt）从命令日志重放 `main()`，重新加入已经 dispatch 的子（关系变为 `Completed`），await 就像从未暂停过一样拿到子的结果。无论是否设置 `recover_existing_child_commands`，也无论是在本进程还是重启之后，该尝试都会重新加入子：暂停过的子的结果已经在父的关系里，而重新加入一个已经运行过的子不会 dispatch 任何东西。这个标志仍然是针对 dispatch 未被记录的子的 opt-in。继续时只使用日志为该 run 保存的内容，即剩余预算和保留的预留，绝不会获得新的预算。这会沿树向上重复：孙的回答继续子，子的结束继续根。继续是异步的；需要父结果的调用方应等到父的记录结束，再用 `reconnect` 读取。宿主重启后，如果发现某个父的子已经结束而没有进程继续它，调用 `reconnect(parent_run_id)`（或同时返回子的句柄、且不会再次 dispatch 子的 `recover_children(parent_run_id)`）即可启动继续。继续只会发布一次：子的结束、`reconnect` 和另一个进程都可能到达暂停的父，一次发布决定结果，其余的把这个 run 留给它。无法启动的继续（存储拒绝该发布，或某个限额已用尽）会让父保持 `Interrupted`：持有父句柄的进程里，该句柄带有通知 `P_CHILD_CONTINUATION_FAILED`，对父 `reconnect` 会报告原因。回答后再次暂停的子，同样可以继续恢复。

await 的截止时间（`timeout_ms`）与所有被恢复的 await 一样，是从该 await 首次开始起算的绝对时间：截止后才到达的回答会使父以 `P_AWAIT_TIMEOUT` 结束为 `TimedOut`，所以等待人的 await 应设置足以覆盖等待的超时。子在等待期间，取消父或任一祖先，会持久地取消该子及其后代：本进程执行的后代通过其活动的 control 取消，否则根据其记录取消。由另一个进程恢复且仍在运行的后代会被一条 `Cancelled` 记录结束，该进程会发现自己的发布被拒绝。

只有 `ng.await(ng.spawn(...))`（或单独被 await 的 spawn）会被重新加入。作为 join（`ng.all`、`ng.race`、`ng.quorum`）成员被 await 的子不会：回答之后，父以 `Interrupted` 结束，并把待处理的 `javascript` 命令作为需要对账的效果，这与子绑定不可用时相同。类型化计划（typed plan）中对 `spawn` 的 `await` 以同样方式继续；计划在继续时会重放自己的操作，这些操作会再次计入 Program 操作预算，所以请为此声明足够的余量。

## JSON 制品与核算

JSON 是序列化后、经过校验的 Program bundle。加载另一份 JSON，是选定一个候选的不可变版本；
修改文件并不会改动正在运行的引擎。宿主准入这个版本，并在被持有的检查点处切换智能体的
generation。生成器的堆和栈状态不会被移植：应用状态通过显式的 `handoff` JSON 跨越边界，
而子关系则保留在持久的树里。

示例中的替换 bundle，在会话开始之前就已编译并准入。主智能体和子智能体在执行过程中，
为它们的后代提出 JavaScript，这些编译消耗它们各自的授权。准备一份新生成的自我替换，
仍然是单独的编译和准入操作；`replace` API 本身选定的是一个已经准入的目标。

已委托出去的编译预算，不能再用于父智能体的另一次合成请求。
替换会转移确切的剩余预算和已提交的后代。子智能体的恢复同样保留第一次尝试时的截止时间。
SQLite Catalog 使用有上限的忙等待超时，因此可以像转换存储一样，与并发的智能体发布共用同一个数据库。

## 运行参考实现

构建 Program 和 QuickJS；对应的用例要启用两种持久化后端。
运行 `neograph_program_tests --gtest_filter="*Recursive*"`。
PostgreSQL 的用例使用现有的一次性 `NEOGRAPH_TEST_POSTGRES_URL` 夹具，以及 CTest 的数据库资源锁。

运行成功的拓扑测试时设置 `NEOGRAPH_RECURSIVE_ARTIFACT_DIR`，可以导出
`main.json`、`child-v1.json`、`child-v2.json`、`grandchild.json` 和
`session.json`。它们使用测试用的注册表和编译器标识，是供审阅的夹具，
不是可独立使用的生产配置。

该参考场景覆盖：三层的树、嵌套的检查点替换、运行中后代的保留与取消、
generation 结果的完整性、预算的缩减，以及 SQLite/PostgreSQL 上整个会话的进程恢复。
它不包含由 LLM 生成源码的服务、Python 或传输层的门面、跨 owner 的会话共享策略，
也不包含对 Core 的任意就地修改。原生 Core 迁移保持它现有的边界。
独立的多宿主在线所有权转移，以及每一种上下文/Hook 失败的组合，都不在覆盖范围内。

## 交互式聊天机器人示例

[会自我演进 Harness 的聊天机器人](../examples/cookbook/self_evolving_chatbot/README.md)
在参考实现的基础上，增加了：两个相互隔离的租户、一个 OpenRouter 适配器、一个浏览器检查器、
每轮经过审阅的模板提案、运行时的后继者编译，以及 SQLite 或 PostgreSQL 上的聊天/provider 账本。
它的助手在替换之后可以合成一个评审者，而原来的编排器继续等待同一个逻辑助手。

`RuntimeConfig::checkpoint_handler` 让宿主可以在持久化发布之后，把一个句柄和只能移动的检查点租约入队。
保留租约会让生成器暂停，而不会阻塞调度线程。回调必须尽快返回；
编译和替换要放在宿主自己的 worker 上执行。重新连接时，它也能看到对最近一个已完成检查点的重放。
显式的 `next_handoff` 请求优先。

`ProgramRuntime::reserve_synthesis` 针对期望的谱系 head，扣减一次未分配的动态编译，
并刷新被持有租约的 journal 引用。检查点的标识和序列化的交接值保持不变。
过期的 head、外来的 owner 或运行时、已过期的挂钟时间预算，
或者已分配给后代的编译预算，都会被拒绝。宿主必须在预留之前持久化意图，并记录结果；
含糊不清的确认，不能作为免费重试的依据。聊天机器人在普通的 `ProgramSynthesisGateway` 中使用这个预留，
然后把已准入的目标传给 `replace`。

聊天机器人的模板闸门并不能证明回答的质量，它的模型调用和生成器控制保留 `Unmanaged` 保证。
进程丢失之后仍待处理的 provider 副作用，需要对账，而不是自动重新分发。
