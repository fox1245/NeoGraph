<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_CONTRACT.md locale=zh-CN source_sha256=b4350a0091100913423075c5ccded5a70741490d738e4bfaabaa0e42158b2c9e -->
# 子 Program 合成：宿主授权契约

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_CONTRACT.zh-CN.md)

状态：N1 宿主边界和 N2 持久运行时集成已实现。见 [SQLite/PostgreSQL 持久化与恢复](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)。专用 Program 侧提交及模型生成仍是后续工作。

## 入口与所有权

`ProgramSynthesisGateway::synthesize_child(proposal, grant, parent)` 只在单独选择的 `ProgramChildSynthesisGrant` 下接受 proposal。宿主将父版本、运行记录、lineage head 和 generation 加载到 `ProgramChildSynthesisParent`。这些快照及 grant 必须来自宿主自己的存储及经过审查的模板策略，绝不能来自请求方 Program。

第一层边界准入的是经过精确审查的模板**实例**。授权绑定 `template_identity` 和 `program_synthesis_source_identity(source)`；后者覆盖完整规范化源封套，包括导入身份、封存模块体、源坐标及 runtime/profile 身份。宿主在签发授权前渲染并审查该实例。此 API 不实现模板渲染及其参数 schema。授权还固定语义验证器与任务契约身份。子编译必须使用父级封存 registry 指纹；在此次首批交付中，相同源文本不能选择不同的已注册实现。

已存 grant 的 hash 证明完整性，不证明签发者身份。解析调用方自行编写的 grant 不代表有权使用它。gateway 不接受序列化授权回执来替代宿主 grant 和父级快照。

## 预留前检查

纯操作 `authorize_program_child_synthesis` 检查：

- owner、parent run、parent ProgramVersion 与 bundle、parent policy 指纹、lineage 身份、活动 generation，以及精确的 record/journal/head 配对；
- 父级快照是否正在运行且未终止；
- 精确审查的源身份、规范源封套字节和封存模块数量；
- 所请求能力/效果是否同时符合宿主 grant 和父策略，导入模块身份是否符合父策略；
- 全部九个子预算维度是否符合宿主上限及可用父余量，不借用父级在途预留；
- 是否留有一次 compile、一个 child 及一次 descendant-depth 跳转的容量；以及
- 子保证下限是否不会削弱父级执行保证。

这些检查不预留资源，只生成不可变、只读的 `ProgramChildSynthesisAuthorization` 证据。其他子预留及容量仍必须在普通 `ProgramRuntime::start_child` 边界检查。并发预检结果不是独立预算授权。

## 预留与编译

子入口要求 `ProgramSynthesisGatewayConfig::reserve_child`。其签名同时接收 proposal 和 authorization，包括用于比较交换的精确 `source_lineage_head_id` 与 `parent_remaining`。它必须针对该 head 原子扣减一次 compile，或失败而不向其他 generation 扣费。不得重新加载更新的 head 并悄悄针对它预留。宿主还检查执行所有权、取消状态及当前 deadline，并在结算预留时计入已消耗墙钟时间。存储快照不能证明其墙钟时间额度现在仍可用。

返回的 `ProgramSynthesisReservation` 必须绑定同一 proposal、lineage、source head 和起始预算。gateway 在编译器求值前拒绝改变的起始预算、错误 head、预留后不足的预算，或丢失的 child/depth 容量。预留构造独立要求恰好扣减一次 compile，禁止任何预算增加。

子编译使用请求的子预算上限，而非父级较大的剩余预算。编译必须保留源身份、所请求能力/效果闭包及获准保证下限。强制宿主语义验证器必须匹配 grant 的 validator 与 contract 身份，并在 Catalog 准入前运行。生成的子策略不能携带超出请求的能力/效果权限，或提交源导入范围外的模块权限。语义拒绝不撤销之前的 compile 扣减。重试需要有效、针对具体 head 的预留；不得重用过期预检证据来补充预算。

仅供子合成的 gateway 可省略通用 `reserve` 回调，此时调用通用 `synthesize()` 入口会失败。反之，已有 successor gateway 不会仅因配置了 `reserve` 就获得子合成权限。两个入口都不会回退到另一个入口。

## JavaScript 预算与保证边界

Generator Program 现在可通过宿主所有的 `ProgramBudgetBounds` 和 Catalog 策略获得非零动态编译上限。默认编译仍给予零次动态编译。调用方不能在 `start` 时提高该限制。仅声明的 JavaScript 及不带 `expand_task_graph` 的普通 C++ 计划，保留零动态编译的结构规则。

该预算变化不添加 JavaScript 命令或环境隐含编译器访问权。`ng.hostCapability` 保留已有可信原生接口；N1 不在其中安装合成插件，也不更改原生 C ABI。

当前编译器保守地将 generator control 标为 `Unmanaged`。不得将受限 generator 语言 profile 描述为自动带有 `Strict` 执行保证。N1 保留这一分类。经过审查的仅声明子 Program，在其 Core 闭包为 Strict 时可以满足 Strict grant；generator 子 Program 不能仅因 proposal 请求 Strict 就满足该下限。准入较弱子 Program 需要明确兼容的宿主 grant 和父级保证，不能在编译期间悄悄降低下限。

## 诊断

| Code | 边界 |
|---|---|
| `P_CHILD_SYNTHESIS_OWNER` | Owner 不匹配 |
| `P_CHILD_SYNTHESIS_GENERATION` | 错误 run/version/policy、过期或不一致 head、非活动 generation |
| `P_CHILD_SYNTHESIS_SOURCE` | 未审查实例或源/模块大小超限 |
| `P_CHILD_SYNTHESIS_REGISTRY` | 编译的子 Program 使用不同封存 registry |
| `P_CHILD_SYNTHESIS_SEMANTICS` | Validator 或任务契约身份改变 |
| `P_CHILD_SYNTHESIS_AUTHORITY` | 请求或子准入策略扩大权限 |
| `P_CHILD_SYNTHESIS_BUDGET` | 无效执行预算，或缺少 compile/child/depth/host 容量 |
| `P_CHILD_SYNTHESIS_RESERVATION` | 预留未绑定获准 head 与剩余预算 |
| `P_CHILD_SYNTHESIS_GUARANTEE` | Grant 或编译的子 Program 违反保证下限 |

`ProgramSynthesisValidationError` 继续携带语义拒绝证据。编译错误保留既有编译器诊断。存储预算解码器在验证规范身份前拒绝负数、小数及超范围整数；无效数字不能通过整数回绕变成看似有效的存储 ID。

## 持久集成与剩余验证

独立 `synthesize_child` API 止于准入。N2 运行时 API 在 SQLite 和 PostgreSQL 上加入原子预留、持久阶段结果、父 run/generation 范围绑定，并通过已有子生命周期派发。配置、恢复行为、N3 失败矩阵及明确限制见 [持久化契约](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)。授权值仍是证据，不提供可能被误认为新授权的公开存储值构造函数。

此 C++ API 增补要求重新构建 Program 使用方。现有 successor 合成结果格式、JavaScript 命令协议、原生控制 C ABI 和 Core-only 依赖边界不变。
