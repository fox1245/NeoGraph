<!-- neograph-i18n: source=docs/PROGRAM_RECURSIVE_HARNESSES.md locale=zh-CN source_sha256=66abaa1eb78c67c16cf2ef0e6f42ad71632806116dc0b71dd952e8433d4b4970 -->
# 具有独立 Harness generation 的递归 agent

**Languages:** [English](PROGRAM_RECURSIVE_HARNESSES.md) | [한국어](PROGRAM_RECURSIVE_HARNESSES.ko.md) | [日本語](PROGRAM_RECURSIVE_HARNESSES.ja.md) | [简体中文](PROGRAM_RECURSIVE_HARNESSES.zh-CN.md)

经过验证的场景在同一 owner 下，将一个逻辑 session 运行为父/子/孙树。每个 agent 有自己的 ProgramVersion、Core 拓扑、状态、预算和 lineage。子级可通过与主编排器相同的宿主所有合成边界，为自己的子级提出 JavaScript。

参考测试执行四个不同 Core plans：主 Harness 一个节点，原 child 两个，替代 child 三个，grandchild 四个。child 在其 grandchild 仍存活时被替换。主编排器通过已有 await 接收替代 child 的结果。SQLite 和 PostgreSQL 进程退出测试在嵌套替换提交后重新打开完整树，并完成而不重跑已完成工作。

## 身份与存储

`ProgramRunRecord::logical_run_id()` 和 `ProgramHandle::logical_run_id()` 暴露稳定 agent 身份。每次替换仍创建不同物理 run 和不可变 ProgramVersion。lineage 选择活动 generation。用 `reconnect(owner, logical_run_id)` 获取该 generation 的 handle。

新 run-record schema 4 在 `logical_run_id` 不同于物理 run 时携带该字段。child relations 继续指向原 child 身份，保留最初获准链接及调用。替代版本提供终止结果时，relation 同时记录 `terminal_generation` 和结果实际 run/version 身份。transition 后端根据已提交 child lineage、generation 和终止 run record 检查这些内容。

已有 schema 2/3 记录仍可读，不带新字段的普通记录保留 schema 3 表示。不要将旧 Program 二进制与新 C++ 记录布局混用；一起重新构建 Program 使用方。

## 带后代的替换

存活 family 的替换使用所属 runtime，以及该 agent generator 中一个持有且已完成的顶层 checkpoint。该 checkpoint 可以属于嵌套 agent。目标必须已经准入。

同一事务发布新 generation，并复制已有 child relations 和已提交 descendant budget。父身份与绝对子深度不能改变。目标必须保留 children 的能力/效果 grants 和保证下限。嵌套替换必须保留父级结果契约。保留的 children 要求唯一 binding names；中断 child 必须在父级可替换前获得显式处置。

旧执行退役时，不触发逻辑 agent 终止 hook，也不取消已转移 children。逻辑 child 并发及 quota 清理转移给 successor；attempt 专属清理仍运行。已经等待 child 的父级跟随 successor 结果，经该 child handle 的取消也沿相同链传播。root-generation handles 保留原有 generation 语义；使用返回的 replacement handle，或重连逻辑身份来获取活动 root。

只有不可变 generation-creation 发布中已有的 children，才是继承 bindings。可通过精确匹配的 binding 和输入重新 join 已有 children：

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

替换不会重新创建该 child、重写其调用或获取另一个 child grant。旧 generation 未附接的 synthesis bindings 不会成为新权限。新 bindings 仍使用普通合成与准入路径。源 proposal 必须匹配经独立审查的宿主模板实例；参考宿主不会仅因 agent 将源放入 checkpoint 就批准它。

## 进程丢失后恢复预先准入的 children

恢复其获准 child bindings 的宿主，可以显式允许未完成 JavaScript `spawn`/`await` 命令重连到已发布 children：

```cpp
config.recover_existing_child_commands = true; // default: false
```

这适用于普通静态 bindings，以及替换继承的 children，无须 synthesis gateway。父 relation、link receipt、owner、精确 invocation、child depth 和持久 child run 必须一致。静态命令必须从记录的 command coordinate 推导出相同 child ID；继承 children 保留 generation-creation 发布准入的 ID。历史 Program 版本及宿主 grants 必须仍可用。配置的宿主 admission resolver 仍按普通方式准入 child attempt。

恢复使用专用 reconnect 路径：检查后消失的 child 不能回退为创建新 child。缺失 run、改变输入或不可用 binding，会使已有命令保持待处理，等待对账。命令预留和 descendant 记账跨恢复保留。该 flag 不授权重放未知 provider、tool、native 或其他外部效果。单独授权的合成恢复协议不变。

无论此 flag 如何，运行中 attempt 的 replay 会从精确 journaled 值及原命令预留完成未结束的封存 `ng.checkpoint`，不执行外部派发。其他未知效果仍暂停 family 等待对账：中断 child 保持已派发、可恢复的 relation，其父级保留在途命令预留，而非取消 relation 或将预留额度归还两次。

结果发布失败但存储仍可用时，已派发命令的结果及待处理效果均以 `Ambiguous` 发布；取消不能消除不确定性。乐观 command-head 读取冲突会重试，但针对未改变父 head 的永久拒绝不能无限阻塞 child 完成通知。

`*StaticChild*` 测试覆盖 SQLite 和 PostgreSQL 进程退出、静态及继承递归 children、不变的 child 身份与回执，以及 grandchild 的 32 个 checkpoints 恰好产生 64 个 journal entries。它们还覆盖未 opt-in、缺失 runs/bindings、改变的 input/receipts，以及 run 在恢复准入与派发之间消失。

## JSON 产物与记账

JSON 是序列化且经过验证的 Program bundle。加载不同 JSON 选择一个候选不可变版本；修改文件不会改变运行中的 engine。宿主准入版本，并在持有 checkpoint 时切换 agent generation。generator heap/stack 状态不会移植：应用状态通过显式 `handoff` JSON 跨越，而 child relationships 留在持久树中。

示例 replacement bundle 在 session 开始前编译并准入。主 agent 和 child agent 在执行期间提出 descendant JavaScript，这些编译分别消耗自己的 grants。准备新生成的 self-replacement 仍是独立编译/准入操作；`replace` API 本身选择已准入目标。

委派的 compile budget 不能再用于其他父合成请求。替换转移精确剩余预算及已提交 descendants。child 恢复还保留首 attempt 的 deadline。SQLite Catalog 现在使用有界 busy timeout，与 transition store 支持并发 agent 发布共享数据库的能力一致。

## 运行参考案例

构建 Program 和 QuickJS；为对应案例启用两个持久后端。运行 `neograph_program_tests --gtest_filter="*Recursive*"`。PostgreSQL 案例使用现有一次性 `NEOGRAPH_TEST_POSTGRES_URL` fixture 和 CTest 数据库资源锁。原生 WSL Docker 适用于此设置。

运行成功拓扑测试时设置 `NEOGRAPH_RECURSIVE_ARTIFACT_DIR`，导出 `main.json`、`child-v1.json`、`child-v2.json`、`grandchild.json` 和 `session.json`。它们使用测试 registry/compiler 身份，是审查 fixtures，不是独立生产配置。

此验证覆盖三层树、嵌套 checkpoint 替换、存活 descendant 保留与取消、generation-result 完整性、预算收窄，以及 SQLite/PostgreSQL 完整 session 进程恢复。它不引入 LLM 源生成服务、Python/transport facade、跨 owner session 共享策略或任意原地 Core 修改。原生 Core 迁移保留已有边界。独立多宿主存活所有权转移，以及全部 context/hook 失败组合，需要单独验证。

## 交互式聊天机器人示例

[演化 Harness 聊天机器人](../examples/cookbook/self_evolving_chatbot/README.md) 在参考案例上加入两个隔离 tenants、OpenRouter adapter、浏览器 inspector、逐轮审查模板 proposals、运行时 successor 编译，以及 SQLite 或 PostgreSQL chat/provider ledgers。其 assistant 可在替换后合成 reviewer，而原编排器继续 await 同一逻辑 assistant。

`RuntimeConfig::checkpoint_handler` 允许宿主在持久发布后将 handle 和仅可移动 checkpoint lease 入队。保留 lease 会暂停 generator，不阻塞 scheduler 线程。回调必须及时返回；编译和替换应在宿主 worker 执行。重连时它也观察最新已完成 checkpoint 的 replay。显式 `next_handoff` 请求优先。

`ProgramRuntime::reserve_synthesis` 针对预期 lineage head 扣减一次未分配 dynamic compile，并刷新该持有 lease 的 journal reference。checkpoint 身份和序列化 handoff 值不变。过期 head、外来 owner/runtime、已过期 wall budget，或已分配给 descendants 的 compile budget 均被拒绝。宿主必须在预留前持久化意图并记录结果；不确定确认不能授权免费重试。聊天机器人在普通 `ProgramSynthesisGateway` 中使用此预留，再将获准目标传给 `replace`。

聊天机器人的 template gate 不证明回答质量，其 model calls 和 generator control 保留 `Unmanaged` 保证。进程丢失后的待处理 provider effects 需要对账，不能自动重新派发。
