<!-- neograph-i18n: source=examples/26_postgres_react_hitl/README.md locale=zh-CN source_sha256=b8c1274535db9f44a2a3c254c0b4de2c4dba30f23d37f69e80c0f31a365e6511 -->
# 示例 26 — 基于 Postgres 的带 HITL 深度研究

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

端到端演示两个 NeoGraph 功能：

1. **`PostgresCheckpointStore`** — 在真实 PostgreSQL 中持久化检查点，
   并对 channel-blob 做去重。
2. **NodeInterrupt-driven HITL** — Deep Research graph 在生成报告后暂停，
   让人类审核，然后要么批准后继续（→ end），要么带反馈继续（→ another research round）。

这个 demo 刻意做成**进程不连续**：binary 在生成报告后退出，
所以当你 `resume` 时，会是一个必须从 PG 重新加载所有内容的新进程。
这正是重点所在 — 证明检查点确实跨过了进程边界。

## 场景

下面的演练复现了最初的想法：向代理询问最新的 Vision Transformer 论文，
发现报告没有引用 URL，然后把它退回去要求补充引用。

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

agent 进程在这里**退出** — 检查点在 PG 中。现在继续：

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

批准以结束：

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 approve
--- Final report (approved) ---
... final markdown ...
```

## 验证它确实已持久化

进入 PG 查看这些行 — 注意，由于去重，`neograph_checkpoint_blobs`
的行数少于 `channels × checkpoints`：

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

每生成一份报告，`final_report` 都会有一行；在 super-step 之间没有变化的 channel
（`user_query`、`research_brief`）总共恰好只有一行。

### 一次真实运行的参考数字

在上面的 multimodal-RAG demo 上完成一次 run-resume-resume 循环
（2 轮 supervisor × 每轮 2 个 researcher，claude-sonnet-4-5）
产生了这些 PG 数字：

| 指标                        | 值         | 说明 |
|-----------------------------|------------|-------|
| `neograph_checkpoints`      | 15 行      | 6 个 super-step + 1 个 NodeInterrupt + 6 个 super-step + 1 个 NodeInterrupt + 1 个 approve 检查点 |
| `neograph_checkpoint_blobs` | 29 行      | 理论值为 15 个检查点 × 9 个 channel = 135 — **78.5% 去重** |
| `neograph_checkpoint_writes`| **0 行**   | 干净 — 每个 super-step 的 pending log 都在提交时清空 |
| `final_report` v13（第 1 轮）| 2806 B     | 没有 arXiv URL（模型使用了 `arXiv:NNNN.NNNNN` 简写） |
| `final_report` v27（第 2 轮）| 2752 B     | 用户要求后补充了完整的 `https://arxiv.org/abs/...` URL |
| blob 总字节数              | 41 KB      | 整个 thread state，包括所有 LLM 转录 |

`final_report` v13 → v27 的 diff 是确凿证据：用户反馈确实改变了代理的输出：
第二份报告删减了正文，并新增 URL 引用 — 这是一次质量提升；
supervisor 之所以能做到，是因为 HITL gate 把反馈送回了
`supervisor_messages`。

## 设置

1. 复制并填入凭据：
   ```
   cp .env.example .env
   # edit ANTHROPIC_API_KEY
   ```
2. 启动支撑服务：
   ```
   docker compose up -d postgres crawl4ai
   ```
3. 运行 demo（见上面的“场景”）。第一次 `docker compose run`
   会触发 `agent` image build（在预热机器上约 1 分钟）。

完成后：
```
docker compose down       # stop services, keep PG volume
docker compose down -v    # drop the PG volume too
```

## 直接运行二进制文件（代理不使用 docker-compose）

你也可以在主机上构建二进制文件，并指向由 docker-compose 管理的
Postgres + Crawl4AI：

```
cmake -B build -DNEOGRAPH_BUILD_POSTGRES=ON -DNEOGRAPH_BUILD_TESTS=OFF
cmake --build build --target example_postgres_react_hitl -j

./build/example_postgres_react_hitl run "...your query..."
./build/example_postgres_react_hitl resume <thread_id> "feedback"
./build/example_postgres_react_hitl status <thread_id>
```

主机侧 .env 中的 `POSTGRES_URL=postgresql://postgres:test@localhost:55432/neograph`
指向 compose 发布的端口；`CRAWL4AI_URL` 也是如此。

## 针对此栈运行集成测试

compose 文件会在同一个 Postgres 实例上专门准备一个**独立的 `neograph_test` 数据库**，
供 PostgresCheckpointStore 集成测试使用（这些测试会在 SetUp 中调用 `drop_schema()`，
否则会清空 demo thread）。用下面的命令运行：

```bash
NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir ../../build -R PostgresCheckpoint --output-on-failure
```

把测试 URL 指向 `/neograph` 而不是 `/neograph_test` 会清掉 demo DB 中的任何 thread 数据 — 不要这样做。

## 取证提示 — 用户反馈在 PG 中的位置

如果你直接查询 `messages` channel（engine 用它把 resume value 交给 `HumanReviewNode`），
你只会看到空数组：

```
SELECT version, blob_data FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'messages';
-- 0 | null
-- N | []
-- M | []
```

这是刻意设计，不是数据丢失：`HumanReviewNode` 会消费传入的 user message，
并立即在它的 `Command.updates` 中把 `messages: []` 写回，
这样未来的 interrupt cycle 会从干净 channel 开始。
到创建检查点时，数组已经是空的。

实际反馈文本位于 `supervisor_messages` channel，
前面带有标记 `[USER FOLLOW-UP after reviewing...]`：

```
SELECT blob_data::text FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'supervisor_messages'
 ORDER BY version DESC LIMIT 1;
```

对这个标记执行 `grep`，即可恢复该 thread 中所有 HITL 轮次里用户说过的全部内容。

## 实现说明

- HITL gate 通过 `DeepResearchConfig::enable_human_review` flag 内置在 Deep Research graph 中
  （默认关闭，所以示例 25 不受影响）。开启后，`HumanReviewNode` 位于
  `final_report` 与 `__end__` 之间。
- 该 node 首次执行时抛出 `NodeInterrupt`。engine 捕获它，在 `NodeInterrupt` phase
  保存一个检查点，然后重新抛给调用方。resume 时，engine 会重新进入同一个
  node，并把用户回复写入 `messages` channel。
- 该 node 区分“approve”（→ Command(__end__)）与反馈
  （→ Command(supervisor)，同时把反馈追加到
  `supervisor_messages` 并重置迭代计数器）。两条路径
  都会干净地结束运行，因此 PG 中总是有一致的最新检查点。
- 场景的三个步骤（initial run、resume with feedback、resume with approve）
  都会跨过进程边界 — 两次调用之间，engine state 完全保存在 PG 中。

## 为什么没有前端？

“二进制文件退出 → 重启 → 继续”的流程本身就是前端。Web UI
只会通过 HTTP marshal 同样的参数，并不会为检查点持久性演示增加任何内容。
请直接查看 PG 表（上文）来获得可视化证据。
