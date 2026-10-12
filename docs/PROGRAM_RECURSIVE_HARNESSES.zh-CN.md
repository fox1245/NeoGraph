<!-- neograph-i18n: source=docs/PROGRAM_RECURSIVE_HARNESSES.md locale=zh-CN source_sha256=66abaa1eb78c67c16cf2ef0e6f42ad71632806116dc0b71dd952e8433d4b4970 -->
# 递归智能体：各自独立的 Harness generation

**Languages:** [English](PROGRAM_RECURSIVE_HARNESSES.md) | [한국어](PROGRAM_RECURSIVE_HARNESSES.ko.md) | [日本語](PROGRAM_RECURSIVE_HARNESSES.ja.md) | [简体中文](PROGRAM_RECURSIVE_HARNESSES.zh-CN.md)

经过验证的场景，把一个逻辑会话作为一棵父/子/孙的树，运行在同一个 owner 之下。
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

新的运行记录 schema 4，在 `logical_run_id` 与物理运行不同时携带它。子关系仍然指向它们原来的子智能体标识；
它们保留第一次准入的链接和调用。当某次替换给出终态结果时，关系会在记录该结果实际的运行/版本标识的同时，
记录 `terminal_generation`。转换后端会对照已提交的子谱系、generation 和终态运行记录，来检查这一点。

已有的 schema 2/3 记录仍然可读，不含新字段的普通记录保持 schema 3 的表示。
不要把旧的 Program 二进制与新的 C++ 记录布局混用；要把 Program 的使用方一起重新构建。

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
provider、工具、native 或其他外部副作用。另行授予的合成恢复协议保持不变。

与这个开关无关，重放一次正在运行的尝试时，会根据其确切的 journal 值和原始的命令预留，
补完尚未完成的、封存的 `ng.checkpoint`。这不执行任何外部分发。
其他状态未知的副作用，仍然会让整个智能体族暂停，等待对账：
被中断的子智能体仍是一个已分发、可恢复的关系，
它的父智能体保留正在进行中的命令预留，而不是取消这个关系，也不会把这笔预留重复入账。

如果在存储仍然可用时，结果发布失败，已分发命令的 outcome 和待处理的副作用，
会同时发布为 `Ambiguous`；取消无法抹去这种不确定性。乐观的命令 head 读取冲突会被重试，
而针对未变化的父 head 的永久拒绝，不会无限期地阻塞子智能体的完成通知。

`*StaticChild*` 测试覆盖：SQLite 和 PostgreSQL 的进程退出，静态的和继承来的递归子智能体，
不变的子标识和回执，以及孙智能体的 32 个检查点恰好产生 64 条 journal 记录。
它们还演练了：没有主动开启开关、缺失的运行/绑定、被改动的输入/回执，
以及运行在恢复准入和分发之间消失的情形。

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
SQLite Catalog 现在使用有上限的忙等待超时，这与转换存储可以和并发的智能体发布共用同一个数据库的能力相匹配。

## 运行参考实现

构建 Program 和 QuickJS；对应的用例要启用两种持久化后端。
运行 `neograph_program_tests --gtest_filter="*Recursive*"`。
PostgreSQL 的用例使用现有的一次性 `NEOGRAPH_TEST_POSTGRES_URL` 夹具，以及 CTest 的数据库资源锁。
原生的 WSL Docker 适合用来搭建这套环境。

运行成功的拓扑测试时设置 `NEOGRAPH_RECURSIVE_ARTIFACT_DIR`，可以导出
`main.json`、`child-v1.json`、`child-v2.json`、`grandchild.json` 和
`session.json`。它们使用测试用的注册表和编译器标识，是供审阅的夹具，
不是可独立使用的生产配置。

这次资格认证覆盖：三层的树、嵌套的检查点替换、运行中后代的保留与取消、
generation 结果的完整性、预算的缩减，以及 SQLite/PostgreSQL 上整个会话的进程恢复。
它不引入由 LLM 生成源码的服务、Python 或传输层的门面、跨 owner 的会话共享策略，
也不引入对 Core 的任意就地修改。原生 Core 迁移保持它现有的边界。
独立的多宿主在线所有权转移，以及每一种上下文/Hook 失败的组合，都需要另行做资格认证。

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
