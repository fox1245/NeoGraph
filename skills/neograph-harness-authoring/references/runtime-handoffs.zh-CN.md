<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/runtime-handoffs.md locale=zh-CN source_sha256=26d4edee068a5a8108f1a4bc1e21380f319667d5477acf772d58904c8d0ae05c -->
# 运行时的子 Program 合成与替换

**Languages:** [English](runtime-handoffs.md) | [한국어](runtime-handoffs.ko.md) | [日本語](runtime-handoffs.ja.md) | [简体中文](runtime-handoffs.zh-CN.md)

这份参考面向确实提供了这些服务的宿主。skill 不会把 ng.compile、ng.replace
或想象出来的某个 MCP 替换工具变成 API。兼容性的 Harness MCP 接口只负责编译和启动工作流；
实时替换需要这里所述的原生宿主集成。

## 宿主必须提供的信息包

从宿主提供的服务中获取：当前生效的 owner/运行/版本、当前的谱系 head、可用的注册表、
提案 schema（模板参数或完整源码）、输入/输出契约、剩余额度，以及结果和诊断的格式。
如果请求里还没有这些信息，就检查可用的配置。缺少哪份契约，就指出缺少哪份，
不要去猜测绑定，也不要声称某个操作已经运行过。

模型提出源码或参数。权限由宿主提供，原子的状态变更也由宿主执行。
provider 的凭据和受信的授权始终留在宿主手里。

## 创建子 Program

1. 为一个具体的子任务及其所需的输出，提出源码或模板参数。
   只请求父 Program 被允许使用的能力和剩余额度。
2. 在规定的边界处，提交宿主的提案信封。宿主会绑定实际的父 Program 和 head，
   预留资源，编译，验证语义，准入，发布，并绑定子 Program。模型自己提供的授权不算权限。
3. 读取结果。被拒绝时，使用返回的诊断和剩余的修复机会。
   绑定成功时，使用确切的子绑定和输入契约。
4. yield 一个已准入的 spawn 命令；需要结果时，包在 await 里。
   记录子 Program 的结果，或确切的失败原因；不要把排队中的提案当成已执行的子 Program 报告出去。

举个例子：聊天机器人那个经过审阅的 review 模板，会发出一个带 kind="review"、turn 和 source 的检查点。
它的宿主由 turn 推导出名称 "review-" + turn，并在绑定之前，对照经过审阅的模板校验 source。
这些字段名只属于那个示例，不是通用的提案 schema。

## 替换当前的 Harness

1. 构造显式的、可序列化的状态，其中包含后继者所需要的内容：
   对话/任务的进度，以及重新接上已保留工作所需的任何确切输入。
2. yield checkpoint(state, stableSite)。宿主在该边界处保留租约。
3. 宿主持久地记录合成的意图，预留一次动态编译，编译、验证并准入后继者，
   并通过 generation CAS 选定这个确切的版本来做替换。
4. 后继者启动一个新的生成器。在做任何新工作之前，先从 input.handoff 中恢复声明过的交接状态；
   存在交接时，不要从头初始化计数器和历史。宿主同时绑定 previous_run_id。
5. 保持逻辑上的父 Program、子 Program 关系、输出契约和被授予的权限相互兼容。
   剩余的不可续期资源会转移给后继者。

跨越边界的是 JSON 状态，而不是 QuickJS 的堆、生成器栈、指针、promise 或原生句柄。
已发布的图/版本是不可变的。

## 保留的子 Program 与恢复

已保留的子 Program 的绑定和原始输入，标识了现有的工作。在当前的原生运行时下，
用已记录的输入去 spawn 一个继承来的绑定，就会重新接上那个子 Program。
输入按规范 JSON 的内容比较，而不是按原来文本中键的顺序；改变这些内容就是冲突。
新的任务需要宿主准入的新绑定，以及新预留的、已缩减的子预算。
替换不得重复已完成的外部工作，也不得恢复已经花掉的额度。

遇到过期 head 的冲突时，重新加载当前生效的 generation。provider 或编译器的结果不确定时，
需要通过宿主提供的路径对账；不要仅仅因为没有可用的答案就重新分发。
对于逻辑上的同一族 Program，遵循宿主返回的取消契约，
不要假定存在子到父的取消或全局取消。如实报告所返回的执行保证。

## 原生宿主实现的入口点

- RuntimeConfig::checkpoint_handler：把句柄/租约入队，然后尽快返回。
  保留租约会让该 generation 暂停；宿主的工作要放在调度器之外。
- ProgramRuntime::prepare_child_synthesis：把一份提案，和一份单独的、由宿主签发的授权，
  绑定到被持有的父检查点上。这份授权不会补充父 Program 剩余的编译预算或挂钟时间预算。
- ProgramSynthesisGateway：编译、语义验证和 Catalog 的准入。
- ProgramRuntime::reserve_synthesis：通过对期望 head 做 CAS，扣减一次未分配的编译；
  刷新租约的 journal head，同时保留它的检查点标识。
- ProgramRuntime::replace：消耗被持有的检查点和已准入的目标，
  使用显式的交接/前驱输入和来源的剩余额度。
- reconnect/recover_child_synthesis：恢复已记录的 generation 和各阶段，
  或者返回需要对账的要求。

这些名称是 C++ 宿主的 API，不是 JavaScript 命令，也不是模型可以自动使用的工具。
签名和完整的调用方式，以仓库中实际的头文件和服务 schema 为准。
生成器控制保留该 API 的 Unmanaged 执行保证；这个标签并不表示没有宿主一侧的控制。
编译器接受了，并不能证明行为的质量。
