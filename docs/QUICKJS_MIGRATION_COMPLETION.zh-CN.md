<!-- neograph-i18n: source=docs/QUICKJS_MIGRATION_COMPLETION.md locale=zh-CN source_sha256=11b6f9b3c96b82c4bfc8364c832e675413d7d12bc8689c2ea6119295c4a6d278 -->
# QuickJS 迁移完成运行手册

**Languages:** [English](QUICKJS_MIGRATION_COMPLETION.md) | [한국어](QUICKJS_MIGRATION_COMPLETION.ko.md) | [日本語](QUICKJS_MIGRATION_COMPLETION.ja.md) | [简体中文](QUICKJS_MIGRATION_COMPLETION.zh-CN.md)

**状态：** 拟议的发布完成程序。它不证明任何剩余门禁已通过。

**日期：** 2026-08-11

**权威依据：** [QuickJS 控制架构](QUICKJS_CONTROL_ARCHITECTURE.md)、[QuickJS 控制运行时迁移计划](QUICKJS_CONTROL_MIGRATION.md)以及所有者本地的可执行运行时契约。

**可执行配套文件：** 所有者本地的迁移完成契约。

---

## 1. 目的与固定范围

本运行手册将剩余的 Q0 和 Q7 发布关卡转化为有序的、失败时关闭的程序。它**不**改变架构：

- JavaScript 仍然是唯一由用户编写的图/控制语言；
- `GraphEngine` 仍然是唯一的 Core/应用节点执行器；
- `ProgramRuntime` 仍然是命令准入(admission)、持久效果、重放、取消和子调度的唯一所有者；
- 严格的 Core JSON 仍然是规范数据，而非公开的源语言；以及
- 直接 C++ 构造仍然是受信任的嵌入 API，而非公开 Program JSON 操作语言的兼容性借口。

本程序不启动问题 #35 (`trusted_direct`) 或持久 Promise 调度器。两者都不是完成受限持久配置文件的先决条件，且两者都不得与遗留移除捆绑在一起。

## 2. 已验证的起点

| 区域 | 当前状态 | 证据 |
| --- | --- | --- |
| Q1–Q6 基础运行时与编写(authoring)切换 | 已实现 | `quickjs-control-runtime.sdd.yaml` `completion_state`; JavaScript `define()`/生成器行为由 `tests/test_harness_program_cutover.cpp` 覆盖。 |
| Core DSL/阐释器(elaborator)与 Harness DSL 编写 | 已移除/已拒绝 | 父规格中的 `authoring_cutover_contract.completed_removals`;Harness 转换器拒绝遗留模式。 |
| 遗留存储排空(drain) | 仅通过限定的无部署证明 | `docs/QUICKJS_CONTROL_MIGRATION.md` 记录证明 `sha256:06f362…fd6217`;它不是后续生产快照的替代品。 |
| Linux QuickJS | 已实现并已运行验证 | Linux CI 启用 `NEOGRAPH_BUILD_QUICKJS_CONTROL`;存在 `neograph_quickjs_tests`、ASan/UBSan 以及预注册的性能矩阵。 |
| macOS QuickJS | 部分 | 已安装的共享 Program 消费者启用了 QuickJS，但普通的 macOS 构建/测试任务未启用它，且不存在静态 QuickJS 消费者行。 |
| Windows QuickJS | 不合格 | `CMakeLists.txt` 在 Windows 上故意拒绝 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`；Windows 上的 Program 使用者以禁用 QuickJS 的方式运行。 |
| Q7 源码/ProgramRuntime移除 | 未开始 | `SourceKind::CanonicalJson`、Program 文档模式、`ProgramPlan`、旧式编译器解析，以及操作树调度器仍保留在源码中。 |

因此，本次发布**不**适合进行 Q7 删除。平台适配必须先完成；最终清除证明必须在实际删除边界处现时重新建立。

## 3. 适用于每个步骤的规则

1. **无静默范围缩减。** Windows 是 Q0 需求。本地 `WIN32` 异常不构成完成。移除该需求需要单独接受的架构变更，而非实现侧豁免。
2. **绝不因意外导致供应商源码漂移。** 要明确 QuickJS 版本、归档校验指纹、许可证、排除的 `quickjs-libc.c`，以及私有符号命名空间。平台的 shim 或补丁会改变执行语义，且必须进入运行时身份和来源证明。
3. **切换围栏后无遗留回退。** 失败的 JavaScript 准入(admission)不得选择 JSON、Core DSL 或旧操作树编译器。
4. **无基于文件名的删除。** 某些保留的 Program 类型是规范存储或受信任的 C++ 基础设施。在删除每个依赖之前，将其分类为仅遗留、与 JavaScript 共享、或与受信任的 C++ 共享。
5. **无源码检出证明。** 最终存储证明必须来自冻结的完整清单或确切的未部署证明，而非空检出或合成的CTest测试夹具。
6. 每个阶段都是失败时关闭。失败的进入/退出条件会停止序列；它不会将硬性门禁降级为警告，也不会重新打开传统编写。

## 4. 有序完成程序

### M0 — 冻结发布边界并编写证据索引

**入口:** 已接受的父合约及本配套规格说明均存在。

1. 选择一个候选提交及切换 ID。
2. 在源码树之外记录一个不可变的证据索引。该索引必须绑定提交、编译器/工具链、CMake 缓存/选项、目标平台/架构、QuickJS 发布版/归档摘要/构建选项、以下每条命令，以及每个结果文件的 SHA-256 身份。
3. 在测量之前记录支持矩阵：Linux x86_64 与 arm64、macOS、Windows x64；每个平台必须覆盖仅 Core 模式以及启用 QuickJS 的 Program（在父合约要求之处执行）。
4. 仅将现有的“无部署”证明保留为历史证据。不得使用其 2026-08-10 的身份来认证后续的删除发布。

**Exit:** 候选对象与证据模式不可变更；未经证据索引条目，任何下一阶段均不得报告通过结果。

### M1 — 完成 QuickJS 可移植性实现

**Entry:** M0 已通过。

1. 使 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` 在 Windows 上构建，而不默认启用 QuickJS，也不添加 `quickjs-libc.c`、`std`、`os`、动态模块加载器或公共 QuickJS 导出。
2. 将任何时序、栈限制、分配器或编译器兼容性适配放置于经评审的 NeoGraph 所有层中。不得静默修改第三方归档。
3. 扩展 `JavaScriptRuntimeIdentity`/来源，使通过准入的捆绑包标识出确切的移植/垫片实现，以及上游归档和构建选项。身份不一致时，重放必须在分派前失败。
4. 保持 `neograph_quickjs` 私有，并为其静态 C 符号添加前缀。验证共享的 `neograph::program` 不导出它们，且静态消费者不能与单独链接的 QuickJS 冲突。
5. 添加一个最小的C语言嵌入冒烟可执行文件。它必须创建运行时/上下文，评估确定性源码，绑定一个显式C函数，强制中断和内存/栈上限，并证明`std`/`os`不出现。

**退出：** 所有三个支持的操作系统都可以在干净的目录中配置和构建启用QuickJS的配置以及仅Core配置，并记录新的运行时标识。

### M2 — 限定构建、软件包及已安装消费者的矩阵

**入口:** M1已通过.

对于下面的每一行，请使用干净的构建目录和安装目录。仅构建树内成功是不够的。

| 平台 | 需要启用 QuickJS 的行 | 仅 Core 必需的行 |
| --- | --- | --- |
| Linux x86_64 | Program 静态 + 共享；运行时/Harness 测试；C 冒烟测试；已安装消费者 | 静态 + 共享 installed consumer；无可关联 QuickJS/接口/导出证据 |
| Linux arm64 | Program 静态 + 共享；运行时/Harness 测试；C 冒烟测试；已安装消费者 | static / shared installed consumer |
| macOS | Program 静态 + 共享；运行时/Harness 测试；C 冒烟测试；已安装消费者 | static / shared installed consumer |
| Windows x64 | 使用 MSVC 的 Program 静态 + 共享；运行时/回溯 Harness 测试；C 冒烟测试；已安装的 installed consumer | static / shared installed consumer |

已安装的 QuickJS 使用者必须通过已安装的包执行一次**成功的** `define()` 以及 `function* main()` 发布/运行，而不仅仅是语法拒绝。它还必须保留现有的独立链接的第二个 QuickJS 冲突检测探针。扩展 `scripts/test_find_package.sh` （或将其替换为等效的平台感知驱动程序），以便Windows使用原生检查工具验证静态符号命名空间和共享导出隐藏，而不是跳过该检查。

**Exit：** 所有行均从已安装的前缀运行，并记录包元数据、加载器/链接闭包、可执行输出和私有符号检查结果。

### M3 — 限定隔离、重放、ABI 和拆除安全性

**入口：** M2 已通过。

1. 在每个启用的平台上运行 QuickJS 运行时隔离语料库、JavaScript/Harness 端到端测试、确定性重放/恢复测试、故障注入测试和原生 ABI 一致性测试。
2. Linux 必须在启用 QuickJS 的情况下运行 ASan+UBSan 和 TSan。macOS 必须在启用 QuickJS 的情况下运行其支持的 ASan/UBSan 等效方案。Windows 必须在启用 QuickJS 的情况下运行受支持的 MSVC AddressSanitizer 配置。不要声称不可用的 Sanitizer 已运行；在证据索引中记录工具链特定的限制。
3. 在测试语料库中包括长时间 JavaScript 评估期间的取消、在挂起的 `callCore` 处的中断、分配器拆除、取消后的回调完成，以及一个竞争性的第二个 QuickJS 引擎。
4. 验证源/运行时/配置文件/原生绑定不匹配在分派前失败，包括进程重启后。

**退出：** 所有受支持的 sanitizer/运行时行均通过，且没有隐藏 NeoGraph 或 QuickJS 所有权缺陷的抑制；每个被拒绝的夹具均确认零分派。

### M4 — 关闭性能、启动、分配和二进制大小门限

**入口：** M3 已通过。

1. 运行现有的 Linux 阻塞矩阵：

   ```sh
   scripts/build_quickjs_performance_matrix.sh \
     <fresh-build-root> <evidence-root>/quickjs-performance-linux.json
   ```

其已接受的预注册仍然是阈值权威；不要因候选失败而放宽阈值。
2. 在测量 macOS 和 Windows 之前，为其启动、分配以及已启用但未使用/Core-only 测量添加版本控制的预注册。记录重复的冷/热分布，而不是单次计时。
3. 为每个平台生成机器可读的资格报告，其中包含已安装的 Core-only 和 Program 二进制大小、动态依赖闭包、运行时创建/启动样本、分配器高水位标记和精确的构建配置。Core-only 行必须没有可归因于 QuickJS 的 QuickJS 对象、链接依赖、导出符号、分配路径或大小增加。
4. 将失败或嘈杂的指标视为失败的门限，直到在预注册方法下重新运行。绝不要平均掉回归或比较不同的选项集。

**退出：** 每个阻塞性性能/大小/启动/分配阈值均通过，其签名结果标识存在于证据索引中。

### M5 — 重新建立最终存储排空并清点删除边界

**进入：** M4 已通过；遗留发布/恢复已在已宣告的围栏之后。

1. 如果从未存在预发布或生产部署，则为该切换 ID 获取一份全新的具名无部署证明。否则，将遗留写入置于围栏之后，并捕获每个持久化的 Program 和 Harness 存储的一致只读快照。
2. 对于 SQLite，使用真实的持续一致快照并拒绝活动的 WAL/SHM/日志侧车文件。对于 PostgreSQL，捕获常规自定义格式 `pg_dump`；切勿挂载 `PGDATA` 或审计活动数据库。
3. 枚举每个存储和每个已发现的遗留工件。将每个分类为 `translated`、`drain_only` 或 `rejected`；`drain_only`、活动/可恢复运行、未知结果、活动的遗留激活以及未扫描引用均为硬性阻塞项。
4. 在删除前立即运行实际最终证明：

   ```sh
   python3 scripts/audit_legacy_drain.py \
     --inventory <inventory.json> \
     --root <frozen-export-root> \
     --output <evidence-root>/legacy-drain-proof.json \
     --require-final
   ```

5. 另行生成源码依赖清单。对于每个类似遗留的类型/文件/测试/模式，将其标记为 `legacy_only`、`shared_with_javascript` 或 `shared_with_trusted_cpp`，指明其替代项或保留理由，并指明其回归测试。不要根据名称推断。

**退出：** 审计员报告 `final_drain.passed_is_true`；证明和源码依赖清单均绑定到同一割接 ID；遗留写入围栏在发布期间保持活动。

### M6 — 替换共享依赖，然后删除遗留创作/运行时代码

**进入：** M5 已通过。

1. 首先将受信任的 C++ 调用点迁移离开 Program 文档 JSON 和 `ProgramPlan` 操作树调度器。受信任 API 可在进程内构造规范数据，但不得公开或持久化第二种用户编写的控制语言。
2. 保留 JavaScript 命令路径，同时切断遗留依赖。替代品必须保持相同的 `ProgramRuntime` 准入(admission)、预算、日志/效果、取消和重放不变量。在移除旧路径之前添加其行为回归测试。
3. 深度删除，不带别名或解析器回退。
   - v1858. Program 文档 v-document v1–v4 的 schemas 及公共schema source 路径.
   - 遗留 `CanonicalJson` 源解码一次，除非存储的工件需要它；
   - 遗留 `ProgramPlan` 操作树解析、降级与分发；
   - 集成 Program-JSON 翻译及仅遗留的示例/测试/文档；以及
   - 兼容性构建/链接依赖和保留的运行时选择代码。
4. 仅保留依赖清单标记为权威存储或必需受信任的 C++ 嵌入式基础设施。特别是，不要删除 JavaScript 源代码/捆绑/日志格式或 Harness JavaScript 翻译器 tool merely because they share the `program` 命名空间。
5. 如果既有用，则保留归档审核工具和不可变证明作为发布证据，但它不能保留可部署的遗遗留运行时回退。

**退出：** 没有新的或存储的执行路径可以解析、编译、调度或回退到 Program JSON 操作树；公共创作边界仅暴露 JavaScript 和受信任的 C++ 嵌入。

### M7 — 最终 clean-room 验证和发布记录

进入:: M6 已通过。

1. 在删除后，从全新的目录中重复执行 M2–M4，包括已安装的消费者和 sanitizer 构建。兼容性检查的任何结果均不得复用。
2. 针对最终代码树运行图等价性、存储迁移、确定性重放、崩溃/故障、原生 ABI 以及 JavaScript/Harness 冒烟测试套件。
3. 重跑 M5，如果在快照之后的任何变更可能已准入(admission)、恢复或改变了遗留状态。否则，在发布证据索引中保留已验证的证明身份。
4. 更新 `completion_state.legacy_runtime_removal` 仅在所有 evidence 被接受后。然后在同一发布变更中更新架构、迁移计划、公共边界、变更日志、示例和问题状态。
5. 发布时发布证据索引。回滚会选择先前已准入的JavaScript发布/激活，或恢复兼容的二进制文件；它绝不会重新开启Core DSL或Program JSON的编写。

**退出条件：** Q0 和 Q7 门禁已完成：已存在一种用户编写的语言、一个规范的 Core IR、一个 Program 持久性/效果模型，并且不再有任何旧版实现回退。

## 5. 删除清单：必要的区分

以下领域在M5阶段需要明确分类。这是一个起始检查清单，并非允许盲目移除所有内容的许可。

| 区域 | 为何不能仅凭名称删除 |
| --- | --- |
| `include/neograph/program/plan.h`、`src/program/plan.cpp`、`compiler.cpp` 与 `run_attempt.cpp` 的遗留部分 | 它们实现了 Program JSON 操作树，属于删除候选对象，但调用方必须首先迁移到非遗留的可信 C++ 或 JavaScript 路径。 |
| `SourceKind::CanonicalJson`、`ProgramSource::parse`、bundle/catalog/迁移兼容分支 | 它们负责解码保留的遗留状态。只有在 M5 证明没有状态需要它们之后，它们才能被移除。 |
| `schemas/program-document-v1.schema.json` 至 `v4` | 它们是遗留的编写模式，应随其公共路由一同消失。 |
| `program-source`, `program-bundle`, `program-version`, 命令日志以及严格 Core 格式 | 这些可能是 JavaScript 或受信任 C++ 所需的规范存储，需要逐项做出保留决策。 |
| `authoring.h`、JavaScript 源码/命令代码、Harness 翻译器 | 它们掌握最终的 JavaScript 边界，必须保留。 |
| `tests/integration/find_package_program/main.cpp` | 它目前用于测试携带 Program 文档的 C++ 构建器；在删除前应将其迁移到最终的受信任 C++ 或 JavaScript 路径。 |
| `scripts/audit_legacy_drain.py` | 它是证据工具，而非运行时回退。应审慎地保留或归档它。 |

## 6. 完成记录

只有当其证据已添加到不可变索引、其退出条件已通过且下一阶段的进入条件为真时，一个阶段才算完成。绿色单元测试套件无法替代平台、已安装消费者、性能或最终排空关口。
