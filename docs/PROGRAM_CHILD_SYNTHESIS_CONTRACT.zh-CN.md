<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_CONTRACT.md locale=zh-CN source_sha256=e66e1f93b44c7f5cce024d3d7e18a48a5ef87df5b7d356b2266ef0903d5184a9 -->
# 子 Program 的合成：宿主授权契约

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_CONTRACT.zh-CN.md)

本页说明宿主的授权边界。持久化的运行时路径见 [SQLite/PostgreSQL 的持久化与恢复](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)。Program 不能自行提交源码，NeoGraph 也不会用模型生成源码，而是由宿主选定经过审阅的源码。

## 入口与所有权

`ProgramSynthesisGateway::synthesize_child(proposal, grant, parent)` 只在另行选定的
`ProgramChildSynthesisGrant` 之下，才接受提案。宿主把父 Program 的版本、运行记录、
谱系 head 和 generation，加载进 `ProgramChildSynthesisParent`。这些快照和授权，
必须来自宿主自己的存储和经过审阅的模板策略，绝不能来自提出请求的 Program。

这道边界准入的是一份确切的、经过审阅的模板**实例**。它的授权绑定了 `template_identity`
和 `program_synthesis_source_identity(source)`，后者覆盖完整的规范源码信封，
包括 import 标识、封存的模块体、源码坐标，以及运行时和 profile 的标识。
宿主在签发这份授权之前，先渲染并审阅这份实例。模板的渲染及其参数 schema，
不由这个 API 实现。授权还固定了语义验证器和任务契约的标识。
子 Program 的编译，必须使用父 Program 封存的注册表指纹；
相同的源码文本不能选到不同的已注册实现。

已存储授权的哈希只能证明完整性，不能证明它是谁签发的。解析了调用方自己写的授权，
并不等于有权使用它。网关不接受用序列化的授权回执，来代替它的宿主授权和父快照。

## 预留之前的检查

纯函数式的 `authorize_program_child_synthesis` 操作会检查：

- owner、父运行、父 ProgramVersion 和 bundle、父策略指纹、谱系标识、当前生效的 generation，
  以及确切的记录/journal/head 配对；
- 父快照正在运行且未进入终态；
- 确切的、经过审阅的源码标识，规范源码信封的字节，以及封存模块的数量；
- 请求的能力/副作用，要同时对照宿主授权和父策略；import 的模块标识，要对照父策略；
- 九个子预算维度，要对照宿主上限和父 Program 可用的剩余额度，
  且不得借用父 Program 正在进行中的预留；
- 为一次编译、一个子 Program 和一层后代深度留出余量；以及
- 子 Program 的保证下限，不得削弱父 Program 的执行保证。

这些检查不预留资源。它们产生不可变的、只读的 `ProgramChildSynthesisAuthorization` 证据。
其他子 Program 的预留和容量，仍然要在普通的 `ProgramRuntime::start_child` 边界处检查。
并发的预检结果，并不是各自独立的预算授权。

## 预留与编译

子 Program 的入口要求配置 `ProgramSynthesisGatewayConfig::reserve_child`。
它的签名同时接收提案和授权，其中包括用来做 compare-and-swap 的、
确切的 `source_lineage_head_id` 和 `parent_remaining`。
它必须针对那个 head，原子地扣减一次编译；否则就失败，而不是对另一个 generation 计费。
它不能重新加载更新的 head，并悄悄转而对它做预留。
宿主还会检查执行的所有权、取消状态和实时的截止时间，并在结算预留时，扣除已经过去的挂钟时间。
已存储的快照，并不能证明它的挂钟时间额度现在仍然可用。

返回的 `ProgramSynthesisReservation` 必须绑定同样的提案、谱系、来源 head 和起始预算。
起始预算发生变化、head 不对、预留之后的预算不足，或者失去了子 Program/深度的容量，
网关都会在编译器求值之前拒绝。预留对象的构造本身，也独立要求恰好扣减一次编译，
并禁止任何预算的增加。

子 Program 的编译，使用所请求的子 Program 的预算上限，而不是父 Program 更大的剩余预算。
编译必须保持源码标识、所请求的能力/副作用闭包，以及已授权的保证下限。
强制的宿主语义验证器，必须与授权中的验证器和契约标识一致，并在 Catalog 准入之前运行。
最终得到的子 Program 策略，不能带有超出请求范围的能力/副作用权限，
也不能带有超出所提交源码 import 范围的模块权限。语义验证被拒绝，
并不会撤销之前已扣减的编译额度。重试需要一份针对具体 head 的有效预留；
不能用过期的预检证据来补回预算。

只用于子 Program 的网关，可以省略通用的 `reserve` 回调。此时调用它通用的 `synthesize()`
入口会失败。反过来，现有的后继者网关，也不会仅仅因为配置了 `reserve`，
就获得子 Program 合成的权限。两个入口都不会回退到对方。

## JavaScript 的预算与保证边界

生成器 Program 可以通过宿主持有的 `ProgramBudgetBounds` 和 Catalog 策略，
得到非零的动态编译上限。默认的编译仍然授予零次动态编译。调用方不能在 `start` 时提高这个上限。
只有声明的 JavaScript，以及不带 `expand_task_graph` 的普通 C++ 计划，
仍然遵守动态编译次数为零的结构性规则。

动态编译没有增加任何 JavaScript 命令，也没有增加对编译器的环境访问。
`ng.hostCapability` 仍是它原有的受信原生接口；合成没有在那里安装插件，
也没有改动原生 C ABI。

当前的编译器，保守地把生成器控制标为 `Unmanaged`。受限的生成器语言 profile，
不能被说成会自动获得 `Strict` 的执行保证标签。这个 API 保留了这一分类。
经过审阅的、只有声明的子 Program，在它的 Core 闭包是 Strict 时，可以满足 Strict 的授权；
生成器形式的子 Program，不能仅仅因为它的提案提出了要求，就通过这条下限。
要准入保证更弱的子 Program，需要显式的、相互兼容的宿主授权和父 Program 的保证，
而不是在编译过程中悄悄降低下限。

## 诊断

| 错误码 | 边界 |
|---|---|
| `P_CHILD_SYNTHESIS_OWNER` | owner 不匹配 |
| `P_CHILD_SYNTHESIS_GENERATION` | 运行/版本/策略不对，head 过期或不一致，generation 未生效 |
| `P_CHILD_SYNTHESIS_SOURCE` | 未经审阅的实例，或超出源码/模块的大小限制 |
| `P_CHILD_SYNTHESIS_REGISTRY` | 编译出的子 Program 使用了不同的封存注册表 |
| `P_CHILD_SYNTHESIS_SEMANTICS` | 验证器或任务契约的标识发生了变化 |
| `P_CHILD_SYNTHESIS_AUTHORITY` | 请求或子 Program 的准入策略扩大了权限 |
| `P_CHILD_SYNTHESIS_BUDGET` | 执行预算无效，或缺少编译/子 Program/深度/宿主的容量 |
| `P_CHILD_SYNTHESIS_RESERVATION` | 预留没有绑定已授权的 head 和剩余预算 |
| `P_CHILD_SYNTHESIS_GUARANTEE` | 授权或编译出的子 Program 违反了保证下限 |

`ProgramSynthesisValidationError` 仍然携带语义拒绝的证据。
编译错误保留现有的编译器诊断。已存储预算的解码器，在规范标识校验之前，
就会拒绝负数、小数和超出范围的整数；无效的数字不会回绕成一个本来有效的已存储 ID。

## 持久化集成

独立的 `synthesize_child` API 到准入为止。运行时 API 增加了：原子的预留、
持久的阶段结果、以父运行/generation 为作用域的绑定，以及在 SQLite 和 PostgreSQL 上，
通过现有子 Program 生命周期做分发。配置、恢复行为、恢复矩阵及其声明的限制，
参见[持久化契约](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)。
授权值仍然只是证据，没有会被误当作新鲜授权的、公开的已存储值构造函数。
