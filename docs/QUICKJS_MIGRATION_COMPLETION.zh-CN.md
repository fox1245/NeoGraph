<!-- neograph-i18n: source=docs/QUICKJS_MIGRATION_COMPLETION.md locale=zh-CN source_sha256=6dec683c8c617650e19e61abb2a66a03ff7f0fbe738d65f37a7b12e18b95466a -->
# QuickJS 迁移完成运行手册

**Languages:** [English](QUICKJS_MIGRATION_COMPLETION.md) | [한국어](QUICKJS_MIGRATION_COMPLETION.ko.md) | [日本語](QUICKJS_MIGRATION_COMPLETION.ja.md) | [简体中文](QUICKJS_MIGRATION_COMPLETION.zh-CN.md)

**状态：** 2026-08-11 提出的、历史上的发布完成流程。
其中的起始状态表和有序的闸门，是一份规划记录，并不表示当前的候选版本已通过这些闸门。

**日期：** 2026-08-11

**依据：** [QuickJS 控制架构](QUICKJS_CONTROL_ARCHITECTURE.md)、
[QuickJS 控制运行时迁移计划](QUICKJS_CONTROL_MIGRATION.md)，以及
owner 本地的可执行运行时契约。

**配套的可执行文件：** owner 本地的迁移完成契约。

---

## 1. 目的与固定的范围

这份运行手册，把剩下的 Q0 和 Q7 发布闸门，变成一套有序的、失败时关闭的流程。它**不**改变架构：

- JavaScript 仍是唯一由用户编写的图/控制语言；
- `GraphEngine` 仍是唯一执行 Core 和应用节点的组件；
- `ProgramRuntime` 仍是命令准入(admission)、持久副作用、重放、取消和子 Program 调度的唯一负责方；
- 严格的 Core JSON 仍然是规范数据，而不是面向公众的源码语言；以及
- 直接用 C++ 构造，仍是受信的嵌入 API，不能拿来作为公开的 Program JSON 操作语言得以保留兼容的借口。

这套流程不会启动 issue #35（`trusted_direct`）或持久的 Promise 调度器。
两者都不是完成受限持久 profile 的前提，也都不能与移除旧版本捆绑在一起。

## 2. 历史上已验证的起点

| 方面 | 2026-08-11 记录的状态 | 证据 |
| --- | --- | --- |
| Q1–Q6 的基础运行时与编写方式切换 | 已实现 | `quickjs-control-runtime.sdd.yaml` 的 `completion_state`；JavaScript `define()`/生成器的行为，由 `tests/test_harness_program_cutover.cpp` 覆盖。 |
| Core DSL/elaborator 和 Harness DSL 的编写 | 已移除/被拒绝 | 父规格中的 `authoring_cutover_contract.completed_removals`；Harness 转换器会拒绝旧模式。 |
| 旧存储的排空 | 仅通过了限定范围的“无部署”证明 | `docs/QUICKJS_CONTROL_MIGRATION.md` 记录了证明 `sha256:06f362…fd6217`；它不能代替之后的生产快照。 |
| Linux 上的 QuickJS | 已实现并经过演练 | Linux CI 启用了 `NEOGRAPH_BUILD_QUICKJS_CONTROL`；`neograph_quickjs_tests`、ASan/UBSan 以及预先登记的性能矩阵都已存在。 |
| macOS 上的 QuickJS | 部分完成 | 已安装的共享 Program 使用方启用了 QuickJS，但普通的 macOS 构建/测试任务没有启用它，也没有静态 QuickJS 使用方这一行。 |
| Windows 上的 QuickJS | 未通过资格认证 | `CMakeLists.txt` 在 Windows 上有意拒绝 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`；Windows 上的 Program 使用方，是在禁用 QuickJS 的情况下运行的。 |
| Q7 的源码/运行时移除 | 尚未开始 | `SourceKind::CanonicalJson`、Program 文档 schema、`ProgramPlan`、旧的编译器解析，以及操作树分发器，仍然留在源码里。 |

因此，这次发布**还不具备**执行 Q7 删除的条件。必须先完成平台资格认证；
最终的排空证明，必须在实际的移除边界处重新建立。

上面那张历史表格里关于 Windows 的源码限制，后来已被 NeoGraph 自己维护的 MSVC 构建覆盖层（overlay）所取代，
它固定在 `ProgramSource` 的运行时标识里。源码层面支持，并不等于平台资格认证。
保留下来的规范存储，以及受信 C++ 的带类型操作基础设施仍然存在；
移除面向公众的 JSON 编写方式，并没有删掉所有共享的分发器或已存储的 schema。

## 3. 适用于每一步的规则

1. **不得悄悄缩小范围。** Windows 是 Q0 的要求。只在本地给 `WIN32` 开例外，不算完成。
   要取消这一要求，需要另行批准的架构变更，而不是实现一侧的豁免。
2. **不得在无意中让供应商源码漂移。** 要把 QuickJS 的发布版本、归档摘要、许可证、
   被排除的 `quickjs-libc.c`，以及私有符号命名空间，都明确地写出来。
   平台垫片（shim）或补丁会改变可执行的语义，必须进入运行时标识和来源证据。
3. **切换的围栏之后，不留旧版回退。** JavaScript 的准入失败，
   不得转而选用 JSON、Core DSL 或旧的操作树编译器。
4. **不得仅凭文件名来删除。** 保留下来的某些 Program 类型，是规范存储或受信的 C++ 基础设施。
   删除每个依赖之前，先把它归类为：仅旧版使用、与 JavaScript 共享、或与受信 C++ 共享。
5. **不能用源码检出来证明。** 最终的存储证明，来自冻结的完整清单，
   或确切的“无部署”声明，绝不能来自空的检出或合成的 CTest 夹具。
6. **每个阶段都是失败时关闭的。** 入口/出口条件不满足，就停止整个序列；
   不会把硬性闸门降级成警告，也不会重新开放旧版的编写方式。

## 4. 有序的完成流程

### M0：冻结发布边界，并写好证据索引

**入口：** 已获批准的父契约和这份配套规格都已就位。

1. 选定一个候选提交和切换 ID。
2. 在源码树之外，记录一份不可变的证据索引。它必须绑定：提交、编译器/工具链、
   CMake 缓存/选项、目标平台/架构、QuickJS 的发布版本/归档摘要/构建选项、
   下面的每一条命令，以及每个结果文件的 SHA-256 标识。
3. 在开始测量之前，记录受支持的矩阵：Linux x86_64 和 arm64、macOS，以及 Windows x64；
   凡是父契约要求的地方，每个平台都要覆盖仅 Core 和启用 QuickJS 的 Program。
4. 把已有的“无部署”证明，仅作为历史证据保留。不要用它 2026-08-10 的标识，
   去证明之后的某次删除发布。

**出口：** 候选版本和证据 schema 已不可变，并且后续任何阶段，在证据索引里没有对应条目的情况下，
都不得报告通过的结果。

### M1：完成 QuickJS 的可移植性实现

**入口：** M0 已通过。

1. 让 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` 能在 Windows 上构建，同时不默认启用 QuickJS，
   也不加入 `quickjs-libc.c`、`std`、`os`、动态模块加载器，或公开的 QuickJS 导出符号。
2. 任何有关计时、栈上限、分配器或编译器的兼容性适配，都放在经过审阅的、
   NeoGraph 自己维护的层里。不要悄悄给内置的供应商归档打补丁。
3. 扩展 `JavaScriptRuntimeIdentity`/来源信息，使已准入的 bundle，
   除了上游归档和构建选项，还能标识确切的移植/垫片实现。当这个标识不同时，
   重放必须在分发之前失败。
4. 保持 `neograph_quickjs` 为私有，并给它的静态 C 符号加前缀。验证共享的
   `neograph::program` 没有导出它们，并且静态使用方不会与单独链接的 QuickJS 冲突。
5. 加入一个最小的 C 嵌入冒烟可执行文件。它必须创建运行时/上下文，对确定性的源码求值，
   绑定一个显式的 C 函数，强制执行中断以及内存/栈上限，并证明 `std`/`os` 不存在。

**出口：** 受支持的三个操作系统，都能在干净的目录里，配置并构建启用 QuickJS 和仅 Core 两种配置，
并记录下新的运行时标识。

### M2：对构建、打包和已安装使用方的矩阵做资格认证

**入口：** M1 已通过。

对下面的每一行，都使用干净的构建目录和安装目录。仅在构建树里成功是不够的。

| 平台 | 必需的启用 QuickJS 的行 | 必需的仅 Core 的行 |
| --- | --- | --- |
| Linux x86_64 | Program 静态 + 共享；运行时/Harness 测试；C 冒烟；已安装的使用方 | 静态 + 共享的已安装使用方；没有 QuickJS 链接/接口/导出符号的证据 |
| Linux arm64 | Program 静态 + 共享；运行时/Harness 测试；C 冒烟；已安装的使用方 | 静态 + 共享的已安装使用方 |
| macOS | Program 静态 + 共享；运行时/Harness 测试；C 冒烟；已安装的使用方 | 静态 + 共享的已安装使用方 |
| Windows x64 | 用 MSVC 构建 Program 静态 + 共享；运行时/Harness 测试；C 冒烟；已安装的使用方 | 静态 + 共享的已安装使用方 |

已安装的 QuickJS 使用方，必须通过已安装的包，演练**成功的** `define()` 和 `function* main()`
的发布/运行，而不仅仅是一次语法拒绝。它还必须保留已有的、独立链接的第二个 QuickJS 的冲突探针。
已安装使用方的驱动脚本是
`python scripts/verify_ci.py install --work-dir <fresh-output> --jobs 2 --program --quickjs`
（共享库再加 `--shared`）。它的私有符号检查，目前仍然只支持 ELF/Mach-O；
当前 Windows 的安装行没有加 `--quickjs`。就这项完成标准而言，
Windows 上的静态命名空间和共享导出隐藏，仍然需要原生的检查证据；
原生运行时/C ABI 测试不能代替它。

**出口：** 所有行都从已安装的前缀运行，并记录包的元数据、加载器/链接闭包、可执行文件的输出，
以及私有符号的检查结果。

### M3：对隔离、重放、ABI 和销毁安全性做资格认证

**入口：** M2 已通过。

1. 在每个启用的平台上，运行 QuickJS 运行时隔离语料库、JavaScript/Harness 的端到端测试、
   确定性的重放/恢复测试、故障注入测试，以及原生 ABI 符合性测试。
2. Linux 必须在启用 QuickJS 的情况下，运行 ASan+UBSan 和 TSan。macOS 必须在启用 QuickJS 的情况下，
   运行它所支持的 ASan/UBSan 等价物。Windows 必须在启用 QuickJS 的情况下，
   运行受支持的 MSVC AddressSanitizer 配置。不要声称运行过一个不可用的 sanitizer；
   要把与工具链相关的限制，记录在证据索引里。
3. 测试语料库要包含：一次长时间的 JavaScript 求值期间发生取消、在待处理的 `callCore` 处被中断、
   分配器销毁、取消之后回调才完成，以及一个相互竞争的第二个 QuickJS 引擎。
4. 验证源码/运行时/profile/原生绑定不一致时，会在分发之前失败，包括进程重启之后。

**出口：** 所有受支持的 sanitizer/运行时行都通过，且没有任何会掩盖 NeoGraph 或 QuickJS
所有权缺陷的抑制规则；每个被拒绝的夹具，都确认零次分发。

### M4：关闭性能、启动、内存分配和二进制体积的闸门

**入口：** M3 已通过。

1. 运行已有的 Linux 阻塞性矩阵：

   ```sh
   python scripts/verify_ci.py quickjs-performance --work-dir <fresh-build-root> --jobs 4
   ```

   使用一个全新的、不存在的、归自己所有的构建根目录，以及源码根目录下一个真实的 Git 检出：
   原样保留的 `run_quickjs_performance.py` 需要提交/脏标记的来源信息，
   所以在无关的检出下面解压出来的归档是不够的。
   配对的禁用/启用构建，以及结果，都保留在那个根目录下；
   把 `<fresh-build-root>/quickjs-performance.json` 保留在私有的证据索引里。

   它已获批准的预先登记仍是阈值的权威；不要因为候选版本没通过，就放宽阈值。
2. 在测量 macOS 和 Windows 之前，为它们的启动、内存分配，
   以及“启用但未使用”/仅 Core 的测量，加入受版本控制的预先登记。
   记录多次冷启动/预热后的分布，而不是单次计时。
3. 为每个平台生成一份机器可读的资格认证报告，其中包含已安装的仅 Core 版本和 Program 版本的二进制体积、
   动态依赖闭包、运行时创建/启动的样本、分配器的高水位，以及确切的构建配置。
   仅 Core 的行，不得有任何 QuickJS 目标文件、链接依赖、导出符号、分配路径，
   或可归因于 QuickJS 的体积增加。
4. 失败的或噪声很大的指标，在按预先登记的方法重新运行之前，都按闸门失败处理。
   绝不能用平均值抹平回归，也不能拿不同的选项集去比较。

**出口：** 每一项阻塞性的性能/体积/启动/分配阈值都通过，并且其带签名的结果标识已写入证据索引。

### M5：重新确立最终的存储排空，并盘点删除的边界

**入口：** M4 已通过；旧版的发布/恢复，已处于所公布的围栏之后。

1. 如果从未存在过任何发布前或生产环境的部署，就为这个切换 ID，取得一份新的、具名的“无部署”声明。
   否则，把旧版的写入放到围栏之后，并对每个持久的 Program 和 Harness 存储，取得一致的、只读的快照。
2. 对于 SQLite，使用真正一致的快照，并拒绝仍然存在的 WAL/SHM/journal 旁路文件。
   对于 PostgreSQL，取一份常规的 custom 格式 `pg_dump`；绝不要挂载 `PGDATA`，也不要审计正在运行的数据库。
3. 枚举每一个存储，以及发现的每一个旧版制品。把每一个归类为 `translated`、`drain_only` 或 `rejected`；
   `drain_only`、活动的/可恢复的运行、状态未知的结果、仍在生效的旧版激活，以及未扫描的引用，
   都是硬性阻塞项。
4. 在删除之前，立即运行实际的最终证明：

   ```sh
   python3 scripts/audit_legacy_drain.py \
     --inventory <inventory.json> \
     --root <frozen-export-root> \
     --output <evidence-root>/legacy-drain-proof.json \
     --require-final
   ```

5. 另外生成一份源码依赖清单。对于每一个看起来像旧版的类型/文件/测试/schema，
   标注为 `legacy_only`、`shared_with_javascript` 或 `shared_with_trusted_cpp`，
   写明它的替代方案或保留的理由，并写明它的回归测试。不要凭名字来推断。

**出口：** 审计工具报告 `final_drain.passed_is_true`；证明和源码依赖清单，都绑定到同一个切换 ID；
旧版写入的围栏，一直保持到发布完成。

### M6：替换共享的依赖，然后删除旧版的编写/运行时代码

**入口：** M5 已通过。

1. 首先，把保留下来的受信 C++ 调用点，从 Program 文档 JSON 和 `ProgramPlan` 操作树分发器上迁走。
   受信的 API 可以在进程内构造规范数据，但不得暴露或持久化第二套由用户编写的控制语言。
2. 在切断对旧版的依赖的同时，保留 JavaScript 命令路径。替代方案必须保持同样的
   `ProgramRuntime` 准入、预算、journal/副作用、取消和重放的不变式。
   在删除旧路径之前，先加上它的行为回归测试。
3. 删除以下内容，不留别名，也不留解析器回退：
   - Program 文档 v1–v4 的 schema，以及它们面向公众的源码入口；
   - 旧版的 `CanonicalJson` 源码解码，前提是没有任何已存储的制品还需要它；
   - 旧版 `ProgramPlan` 操作树的解析、降级和分发；
   - Harness 的 Program-JSON 转换，以及仅旧版使用的示例/测试/文档；以及
   - 兼容性的构建/链接依赖，以及保留下来的运行时选择代码。
4. 只保留依赖清单中标为规范存储、或必需的受信 C++ 嵌入基础设施的制品。
   特别是，不要仅仅因为 JavaScript 的源码/bundle/journal 格式和 Harness 的 JavaScript 转换器同属
   `program` 命名空间，就把它们删掉。
5. 如果有用，可以把排空审计工具和不可变的证明保留为发布证据，但它不得保留可部署的旧版运行时回退。

**出口：** 无论是新的还是已存储的执行路径，都不能解析、编译、分发 Program JSON 操作树，也不能回退到它；
面向公众的编写边界，只暴露 JavaScript 和受信的 C++ 嵌入。

### M7：最终的洁净环境验证与发布记录

**入口：** M6 已通过。

1. 删除之后，从全新的目录重复 M2–M4，包括已安装的使用方和 sanitizer 构建。
   兼容性检出中的任何结果，都不能复用。
2. 针对最终的代码树，运行图等价性、存储迁移、确定性重放、崩溃/故障、原生 ABI，
   以及 JavaScript/Harness 冒烟测试集。
3. 快照之后，如果有任何变更可能准入、恢复或改动旧版状态，就重新运行 M5。
   否则，把已验证的证明标识，保留在发布的证据索引里。
4. 只有在所有证据都被接受之后，才更新 `completion_state.legacy_runtime_removal`。
   然后在同一次发布变更里，更新架构、迁移计划、公开边界、变更日志、示例和 issue 状态。
5. 随发布一并公布证据索引。回滚的做法，是选用之前已准入的 JavaScript 发布/激活，
   或恢复一个兼容的二进制文件；绝不会重新开放 Core DSL 或 Program JSON 的编写方式。

**出口：** Q0 和 Q7 的闸门全部完成：只剩一种由用户编写的语言、一套规范的 Core IR、
一套 Program 的持久性/副作用模型，没有任何旧版实现的回退。

## 5. 删除清单：必须区分清楚的内容

下面这些现有的区域，在 M5 期间需要明确归类。这只是一份起始的检查清单，
并不是允许盲目地把它们全部删掉。

| 区域 | 为什么不能仅凭名字删除 |
| --- | --- |
| `include/neograph/program/plan.h`、`src/program/plan.cpp`，以及 `compiler.cpp` 和 `run_attempt.cpp` 中属于旧版的部分 | 它们实现的是 Program JSON 操作树，是待删除的候选，但调用方必须先迁移到非旧版的受信 C++ 或 JavaScript 路径。 |
| `SourceKind::CanonicalJson`、`ProgramSource::parse`，以及 bundle/catalog/迁移的兼容分支 | 它们负责解码保留下来的旧版状态。只有在 M5 证明没有任何状态还需要它们之后，才能删除。 |
| `schemas/program-document-v1.schema.json` 到 `v4` | 它们是旧版的编写 schema，应当随着它们面向公众的入口一起消失。 |
| `program-source`、`program-bundle`、`program-version`、command-journal 和严格的 Core 格式 | 这些可能是 JavaScript 或受信 C++ 所需要的规范存储，需要逐个决定是否保留。 |
| `authoring.h`、JavaScript 的源码/命令代码、Harness 转换器 | 它们负责最终的 JavaScript 边界，必须保留。 |
| `tests/integration/find_package_program/main.cpp` | 它目前演练的是一个携带 Program 文档的 C++ 构建器；删除之前，先把它迁移到最终的受信 C++ 或 JavaScript 路径。 |
| `scripts/audit_legacy_drain.py` | 它是证据工具，不是运行时回退。要有意识地选择保留或归档它。 |

## 6. 完成记录

只有当某个阶段的证据已加入不可变的索引、它的出口条件已通过、并且下一阶段的入口条件成立时，
这个阶段才算完成。单元测试全绿，不能代替平台、已安装使用方、性能或最终排空的闸门。
