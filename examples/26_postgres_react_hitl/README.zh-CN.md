<!-- neograph-i18n: source=examples/26_postgres_react_hitl/README.md locale=zh-CN source_sha256=fb347c51a8ebcb9bd157da6854903e0e2a348f5f3672207040849b70a60e57cb -->
# 示例 26 — 基于 Postgres 的深度研究（带 HITL）

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

端到端演示 NeoGraph 的两项功能：

1. **`PostgresCheckpointStore`** — 在真实 PostgreSQL 中实现持久化检查点，并带有 channel-blob 去重。
2. **基于 NodeInterrupt 的 HITL** — 深度研究图在生成报告后暂停，让人类进行审核，然后恢复执行，要么批准（→ 结束），要么提供反馈（→ 进入另一轮研究）。

此演示跨越进程边界。新的`resume`进程必须恢复PG状态及所有者私有native历史；仅PG不足以恢复typed native续接。

## 场景

以下演练复现了原始思路：向智能体询问最新的 Vision Transformer 论文，注意到报告未引用 URL，并将其发回补充引用。

```
$ docker compose run --rm agent run "현재 최신 ViT 관련 논문 알려줘"
=== Postgres HITL Deep Research ===
Thread:  dr-hitl-a1b2c3d4
...
[start] supervisor
[send] fan-out to 2 researcher(s)
[done] researcher
[done] researcher
[start] supervisor
[cmd]  → final_report
[done] final_report
[start] human_review

--- HUMAN REVIEW REQUESTED ---
Awaiting human review of the report. Resume with 'approve' to finalize, or
pass any other text as feedback to trigger another research round.

--- REPORT ---
# Vision Transformer Recent Papers
... <report body> ...

To approve: ./example_postgres_react_hitl resume dr-hitl-a1b2c3d4 approve
To send feedback: ./example_postgres_react_hitl resume dr-hitl-a1b2c3d4 "give me URL citations"
```

智能体进程在此**退出**——检查点保存在 PostgreSQL 中。现在进行后续操作：

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 "give me URL citations"
=== Resuming thread dr-hitl-a1b2c3d4 ===
Feedback: give me URL citations

[start] human_review
[cmd]  → supervisor
[start] supervisor
[send] fan-out to 2 researcher(s)
[done] researcher
[start] supervisor
[cmd]  → final_report
[done] final_report
[start] human_review

--- HUMAN REVIEW REQUESTED (round 2+) ---
... new report with URLs this time ...
```

批准以完成：

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 approve
--- Final report (approved) ---
... final markdown ...
```

## 验证其确实已持久化

进入 PostgreSQL 查看这些行——注意 `neograph_checkpoint_blobs` 因去重而比 `channels × checkpoints` 的行数更少：

```
$ docker compose exec postgres psql -U postgres -d neograph -c "
    SELECT step, current_node, interrupt_phase
    FROM neograph_checkpoints
    WHERE thread_id = 'dr-hitl-a1b2c3d4'
    ORDER BY step;"

$ docker compose exec postgres psql -U postgres -d neograph -c "
    SELECT channel, COUNT(*) AS versions
    FROM neograph_checkpoint_blobs
    WHERE thread_id = 'dr-hitl-a1b2c3d4'
    GROUP BY channel
    ORDER BY versions DESC;"
```

`final_report` 将为每个生成的报告包含一行；在超级步骤（`user_query`、`research_brief`）之间未变化的通道总共只有一行。

### 一次真实运行中的参考数字

这些是切换前运行的历史数字。上方输出也是历史/示意记录，不是当前typed provider迁移的执行验证。

在上述多模态 RAG 演示上完成一轮 运行-恢复-恢复 的循环（2 轮监督者 × 每轮 2 个研究者，固定使用通过 OpenRouter 访问的 DeepSeek），产生的 PG 数字如下：

| 指标                      | 值      | 备注 |
|-----------------------------|------------|-------|
| `neograph_checkpoints`      | 15 行    | 6 个超级步骤 + 1 个 NodeInterrupt + 6 个超级步骤 + 1 个 NodeInterrupt + 1 个批准检查点 |
| `neograph_checkpoint_blobs` | 29 行    | 对比理论值 15 cps × 9 通道 = 135 — **78.5% 去重** |
| `neograph_checkpoint_writes`| **0 行** | 干净 — 每个超级步骤的待处理日志在提交时均已清除 |
| `final_report` v13（第 1 轮）| 2806 B     | 无arXiv URL（模型使用`arXiv:NNNN.NNNNN`简写） |
| `final_report` v27（第2轮）| 2752 B     | 在用户要求后提供完整的`https://arxiv.org/abs/...` URL |
| 总 blob 字节            | 41 KB      | 完整的线程状态，包括所有 LLM transcripts |

`final_report` v13 → v27的差异是确凿的证据，证明用户反馈确实改变了智能体的输出：第二份报告精简了文字并添加了URL引用——这是主管仅在HITL门将反馈回传到`supervisor_messages`后才达到的质量改进。

## 设置

1. 复制并填写凭据
   ```
   cp .env.example .env
   # set OPENROUTER_API_KEY; set CRAWL4AI_API_TOKEN to a fresh `openssl rand -hex 32` value
   ```
2. 启动支持服务：
   ```
   docker compose up -d postgres crawl4ai
   ```
3. 使用BuildKit和Docker Compose >= 2.17，将`SCHEMAPROVIDER_SOURCE`设为实际SchemaProvider源码路径（默认`../../../SchemaProvider`，相对此目录）。Dockerfile先从named additional context安装`SchemaProvider::runtime`。 请使用 Linux/POSIX build host。SchemaProvider 对 Core-only build 也必需；本指南不声称验证了 native macOS/Windows provider transport。
4. 固定`.env`中的`NEOGRAPH_NATIVE_ARCHIVE_OWNER`，仅初始化一次：
   ```
   docker compose run --rm agent init-archive
   ```
5. 运行演示。需要有效OpenRouter/Crawl4AI凭据、网络及provider余额；研究/反馈轮次产生模型费用。不承诺固定成本或当前运行成功。

一起保留`pgdata`、`native-history`和`native-keys`。历史和密钥使用独立私有卷父目录。恢复要求相同owner、兼容provider descriptor、原始密钥和archive记录；不要为恢复旧线程重新生成密钥。这是经过认证的所有者私有主机保管，**不是加密或provider issuer证明**。也要保护PG、备份、提示、反馈、报告和`.env`。不要将raw native记录或archive/密钥内容输出到公共日志。CLI会打印查询、反馈和报告，请使用私有终端/日志。

完成后：
```
docker compose down       # keep PG, native-history and native-keys
docker compose down -v    # delete all three; old native resume is lost
```

## 直接运行二进制文件（agent不使用docker-compose）

您也可以在宿主机上构建二进制文件，并将其指向docker-compose管理的Postgres + Crawl4AI：

```
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -B build -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_BUILD_POSTGRES=ON -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build --target example_postgres_react_hitl -j

# Load .env privately in the binary's working directory; keep archive paths stable.
mkdir -m 700 .native-keys
./build/example_postgres_react_hitl init-archive
./build/example_postgres_react_hitl run "...your query..."
./build/example_postgres_react_hitl resume <thread_id> "feedback"
./build/example_postgres_react_hitl status <thread_id>
```

宿主机侧.env中的`POSTGRES_URL=postgresql://postgres:test@localhost:55432/neograph`指向compose发布的端口；`CRAWL4AI_URL`对Crawl4AI同样如此。`CRAWL4AI_API_TOKEN`作为其bearer凭据发送；compose文件拒绝空值，并且仅将Crawl4AI发布在`127.0.0.1`上。

## 针对此栈运行集成测试

compose文件在同一Postgres实例上配置了一个**独立的`neograph_test`数据库**，专门用于PostgresCheckpointStore集成测试（这些测试在SetUp中调用`drop_schema()`，否则会清空demo线程）。使用以下命令运行：

```bash
NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir ../../build -R PostgresCheckpoint --output-on-failure
```

将测试URL指向`/neograph`而不是`/neograph_test`将清空你demo数据库中的所有线程数据——请不要这样做。

## 取证提示——用户反馈在PG中的存放位置

如果直接查询 `messages` 通道（引擎用来将恢复值传递给 `HumanReviewNode`的通道），你只会看到空数组：

```
SELECT version, blob_data FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'messages';
-- 0 | null
-- N | []
-- M | []
```

这是故意的，不是数据丢失：`HumanReviewNode`接收传入的用户消息，并立即将`messages: []`写入其`Command.updates`，以使未来的中断周期从干净的通道开始。到检查点被捕获时，数组已经是空的。

实际的反馈文本位于`supervisor_messages`通道中，前缀为标记`[USER FOLLOW-UP after reviewing...]`：

```
SELECT blob_data::text FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'supervisor_messages'
 ORDER BY version DESC LIMIT 1;
```

`grep` 用于该标记，以恢复线程中所有 HITL 轮次中用户所说的全部内容。

## 实现说明

- HITL门控构建在Deep Research图内部，位于`DeepResearchConfig::enable_human_review`标志之后（默认关闭，因此示例25不受影响）。启用后，一个`HumanReviewNode`位于`final_report`和`__end__`之间。
- 该节点在首次执行时抛出`NodeInterrupt`。引擎捕获该异常，在阶段`NodeInterrupt`保存检查点，然后重新抛出给调用方。恢复时，引擎以用户回复写入`messages`通道的方式重新进入同一节点。
- 该节点区分“批准”（→ Command(__end__)）与反馈（→ Command(supervisor)，反馈追加到 `supervisor_messages` 并重置迭代计数器）。两条路径均干净地结束运行，因此 PG 始终具有一致的最近检查点。
- PG保存portable图状态；native恢复还需要受保护的archive及原始密钥。
- C++使用typed `ProviderRequest`/事件和完整不可变`sp::Outcome`。报告文本是projection，不是native replay权限。本指南记录源码迁移，不是新的构建/测试/live验证。

## 空 budget 恢复与报告质量

supervisor、researcher、compression、final-report 仅在已完成的空 `MaxTokens` outcome 没有
visible text 和有效/无效 client tool call 时，允许最多两次额外 semantic call。cap 加倍但不超过
16,384；research-brief 不属于此 ladder。每次尝试使用新 broker ordinal、原 bank admission、
保留的 outcome 和 usage，以及同一个绝对 deadline。没有显式 deadline 时，通过一次无 effect
preparation 取得已配置 deadline，在 mediated invoke 前释放 handle 并固定该 deadline。
显式 deadline 跳过这一步。

Failure、observer/settlement 错误、已交付 streaming part 不重试。空最终文本在 human review 前
报错；有内容的 `MaxTokens` 报告只在 public 投影标为 `Incomplete`，不修改不可变 outcome，
也不重试部分文本。空 compression 耗尽 ladder 后生成 diagnostic，不伪造成功 provider result。
这些是保留的 interface-4 源码契约，不是新的 live 运行或 durability 证明。

## 为什么没有前端？

“binary exits → restart → continue”流程</br>本身（即）就是前端。Web UI只会通过 HTTP 相同方式传递相同参数，对检查点持久性的展示没有任何额外贡献。请直接检视上方 PG 表中的可视证明。
