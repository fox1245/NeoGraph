<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=zh-CN source_sha256=72d9de3ebec44aa6eb7f92a3cabc4a08d177461e8869eaff2b0bd003b353c8d3 -->
# 自进化聊天机器人

## 当前类型化 Program chat 契约

`program_chat.cpp` 使用 `evolving-chat/v6`、`chat.step` version `1.5.0`
（manifest digest `chat.step/v6`）和 ledger schema `neograph.program-chat-call/v6`。
现有 browser/HTTP protocol 不变。Alice/Bob owner scope、reviewed-template generation、
角色 prompt、effect/capability grant、精确 checkpoint lineage 与 nonrenewable budget 均由 host 拥有。

保留的 `server_multi.cpp` live-provider 路径每次调用允许600秒，即使按观测到的最慢速度也能完成
8,192 token 的回答；连接若30秒内没有任何字节则判为失败
（OpenRouter 会立即发送响应头，并在生成期间每隔几秒发送 keepalive 空白）。
不改变 ProgramChat 的 `--provider-timeout-seconds` 范围/默认值或共享 provider factory 默认值，
也不启用 failure 重发。

```bash
# Set SCHEMAPROVIDER_SOURCE to the supplied SDK checkout before configuring.
# From the repository root; configure Program, QuickJS control and SQLite (or PostgreSQL).
cmake -S . -B build-chat -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON -DNEOGRAPH_BUILD_SQLITE=ON \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE"
cmake --build build-chat --target cookbook_program_chatbot
./build-chat/cookbook_program_chatbot --mock --no-env --model program-chat-mock \
  --descriptor-policy examples/cookbook/self_evolving_chatbot/demo-policy.json --db evolving-chat.sqlite \
  --native-archive-dir .chat-native --session demo
```

loopback browser 仍为 `http://127.0.0.1:8768`；`alice-demo`/`bob-demo` 是 demo bearer token，
不是 production 认证。live 需要 OpenRouter eligible route、密钥与网络；CLI default 为 GLM-5.3-Flash。
archive 是 host-only，默认 `DB_PATH.native`；必须将独立密钥与 owner-private custody 和 DB 一起保留。
它是认证 custody，不是加密或 vendor issuer proof。不要公开 raw native payload、密钥、prompt 或 ledger artifact。
这个 durable recipe 需要 archive；真实 C++ in-memory checkpoint sidecar 不需要。

每个 `turn:role` 只 prepare 一次，计算精确 `Provider::request_digest(prepared)`，预留
`Provider::conservative_token_upper_bound(prepared)`，先持久化 pending claim 再 dispatch 同一个 handle。
整个调用窗口的上限由已准入模型的 input/output 限制、实际 output cap、hosted invocation 限制和
retry 策略计算。缺少模型事实时，在预留或 dispatch 前拒绝。private loopback fixture 也必须提供
明确支持的模型策略事实；mock 不会编造限制或 provider usage。
预留不是 provider usage、forecast 或 invoice。nullable wide provider count 与 `charged_tokens` 独立；
known zero 不是缺失。只有 consistent final input/output evidence、先前 usage 非 unknown 且没有
transport 内部重发时才结算；否则保留原始预留为 `UnknownHold`。
高于预留的 report 仍计入 charge；`cost` 保持 unknown。
使用 `--descriptor-policy PATH`（C++ `Options::descriptor_policy_file`）指定主机拥有的 SDK
descriptor-policy JSON。它与内嵌 codec resource snapshot 一起准入一次，再由两个 tenant 共享；
未指定时仍使用内置策略。明确的 fixture 策略必须声明精确 loopback `openrouter_origins` 和
`program-chat-mock`（或明确选择的 fixture 模型）的限制。已准入策略 identity 绑定到 session
settings；即使文件路径不变，内容变更也必须使用新 session。模型提案或 checkpoint 不能选择策略。
`--mock` 同样需要这些事实；预算只能明确提高。
提供的 [demo-policy.json](demo-policy.json) 只声明 `program-chat-mock` 的 input 4,096、output 65,536
token 事实。offline recipe 必须显式选择；它不是 live model catalog、spending grant、usage report
或 budget reset。显式 `--model` 覆盖继承的模型环境设置，不会削弱 admission。
`--max-output-tokens` 接受正64-bit值，仍受所选 model limit 与既有 session budget 限制，
没有固定8,192 CLI上限。

restart 将 pending 转为 `UnknownHold`，不会自动重新发送。completed replay 要求相同 prepared digest/reservation，
从 archive 恢复原始不可变 Outcome/native role history 并验证 settlement/output。
保留相同 DB、archive/key、session、provider/model/settings 和 build identity；改变设置需要新 session。
SDK retry 关闭（`max_attempts=1`）；restart/替换不会更新 budget。model JSON 是提案而不是 native authority。
当前限定范围的运行证据：实际 ProgramChat PostgreSQL blackbox 的6个 scenario 在
18.989秒内 pass；实际 browser 验证了 Alice/Bob 隔离及 generation-2 替换。
这些 model-free 观察不是 vendor 推理 qualification，也不验证独立的
`multi_tenant_chatbot` server/load recipe、所有 storage 变体或已迁移 Python provider binding。

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`cookbook_program_chatbot` 分离 Alice/Bob 的 message、owner scope、catalog、engine cache、Program family、call ledger 和 nonrenewable budget。每 turn 评估 bounded Harness proposal，将已准入 immutable successor 在 assistant 的 durable checkpoint 替换。orchestrator 等待同一逻辑 assistant。review 路径是 draft → 独立 Program identity 的 reviewer child 合成/等待 → 修订。inspector 显示 Core JSON、DSL、tree、generation、剩余 budget、原因、topology diff 和 admission。SQLite/PostgreSQL 保存 artifact、transition、chat、reservation/result 和 evolution decision。OpenRouter 用 typed SDK Chat provider；offline demo 用相同 compiler/admission/runtime API。

## 构建与运行

```bash
cmake -S . -B build-chat -G Ninja \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE"
cmake --build build-chat --target cookbook_program_chatbot -j 4
./build-chat/cookbook_program_chatbot --mock --no-env --model program-chat-mock \
  --descriptor-policy examples/cookbook/self_evolving_chatbot/demo-policy.json --db evolving-chat.sqlite
```

在 `http://127.0.0.1:8768` 切换 Alice/Bob。demo 中 `review`、`검토`、`비교` 提议 review Harness。首次回答用当前 Harness，批准后变更在下一 turn resume 应用。force checkbox 只交替两个已审核 plan，不证明品质提升。

设置 live OpenRouter key 和可用 model：

```bash
export OPENROUTER_API_KEY='...'
export OPENROUTER_MODEL='z-ai/glm-5.3-flash'
./build-chat/cookbook_program_chatbot --live --session openrouter-demo
# Or read an existing dotenv file without printing its values:
./build-chat/cookbook_program_chatbot --live --env-file /path/to/.env \
  --model z-ai/glm-5.3-flash --session glm-demo
```

默认 `z-ai/glm-5.3-flash`。`--model` 优先于 `OPENROUTER_MODEL`，process environment 优先于 dotenv。未指定 `--env-file` 时查找 working directory 最近的 `.env`，`--no-env` 关闭查找。只读取 named chatbot setting，将文件作为 data parse，不作 shell source。GLM 5.3 Flash chat 默认 `reasoning_effort=low`，在 output cap 留出 visible reply 空间。`--reasoning-effort default` 省略 override，explicit 值须由 model/provider 支持。DSL capability evaluator 的 generation 设置独立。

需要 ZDR eligible route，不假定 token price。不要把 credential 放入 DSL/DB/HTTP body/source。provider、model、skill、output cap、build 改变需要新 `--session`，重开保留已有 budget limit。`NEOGRAPH_CHAT_BASE_URL` 可指定兼容 endpoint，plain HTTP 需 literal loopback 和 `--allow-loopback-provider`，仅用于 protocol test。

output limit 发送为 [OpenRouter chat API](https://openrouter.ai/docs/api/api-reference/chat/create-a-chat-completion) 的 `max_completion_tokens`。默认4,096包含 provider reasoning token，`--max-output-tokens` 接受正64-bit值。仍受 host model limit/session budget 限制，无固定8,192 CLI cap。`--provider-timeout-seconds` 为1..120秒(默认120)，checkpoint wait 覆盖其间顺序 call，reviewer child 有独立180秒 budget。timeout 保留 reservation 为 `UnknownHold`，不自动 retry。

在 WSL native Docker 建专用 PostgreSQL DB，并在同一 shell 设置 URL。example 创建 table，但不是 production deployment/migration manager。

```bash
export NEOGRAPH_CHAT_POSTGRES_URL='postgresql://USER:PASSWORD@127.0.0.1:PORT/DATABASE'
./build-chat/cookbook_program_chatbot --mock --session postgres-demo
```

有此变量时 chat/Program persistence 用 PostgreSQL，否则 `--db` 选择 SQLite。各 backend 可在 build 时关闭。

## 合成内容

model 返回 `{plan, reason, confidence}`。host 只接受 `direct`/`review`、bounded reason 和[0,1] confidence。低于0.7、invalid JSON、unknown plan 被 reject；相同 plan 记为 `kept`。evolution 请求 JSON-object response mode，application 仍检查 exact field/value。空/截断 proposal 不会丢失已完成回答。known usage 结算，stop reason 留在 diagnostic，transport uncertainty 阻止自动 redispatch。

这些参数实例化已审核 JavaScript template，需 source identity、bounded compilation、host semantic/template validation、Catalog admission 和 generation CAS。child 需 durable synthesis gateway 及独立持久化 host grant。model 不能提供 grant/credential/arbitrary import/native code/更大 budget。验证的是 reviewed-template synthesis，不是任意 model-written JavaScript 或回答品质。QuickJS generator control/model node 标记 `Unmanaged`，inspector 不声称 unknown external model effect 的 strict replay。

## Agent authoring skill

host 将 [SKILL.md](../../../skills/neograph-harness-authoring/SKILL.md) 及 `references/chat-template-proposals.md` 加入 evolution model 的 system context。answer/reviewer 使用角色 prompt。skill digest 绑定 session，actual prompt/output cap 绑定 call identity。旧 build DB 请用新 session。

skill 的独立 QuickJS authoring/native runtime-handoff guide 用于 source-generation evaluator：向 model 提供 native compiler manifest，compile 返回 source，再将 rejected diagnostic 用于 bounded repair。见 [DSL capability evaluation](../../../docs/DSL_CAPABILITY_EVAL.md)。chatbot template 路径不公开 model-callable compiler tool。

## Accounting 与 recovery

tenant session 默认12turn/100model call/200,000model token。dispatch 前预留 admitted whole-window bound。只有先前 usage 非 unknown 且无 transport 内 resend 时，才以 consistent final usage 结算；missing usage 保留 reservation。缺少 model limit 时 dispatch 前 reject，不以 request byte 估 token。高于 reservation 的 report 仍 charged。monetary cost 是 unknown，不提供 price/monetary ceiling。

Program compile/operation/Core step/child-depth/wall-time budget 不因 restart/replacement 重置；idle 消耗 wall-time。dynamic successor 在 held checkpoint 以 host-only `ProgramRuntime::reserve_synthesis` 先预留。

以相同 DB/session/provider/model 重开，首 turn reconnect family 并 reconcile 最后 checkpoint。stored result 复用，pending/uncertain call 不自动再发。interrupted compilation intent 保留，不免费 recompile。replacement 前保存的 admitted successor 可从 exact held checkpoint publish，已 committed replacement 通过 lineage 解决。每 DB/session 支持一个 server process；loopback listener 和 `alice-demo`/`bob-demo` 不是 production 认证。explicit cancellation 关闭 family，与 process-loss restart 不同。

## 验证

```bash
python3 examples/cookbook/self_evolving_chatbot/test_program_chat.py \
  ./build-chat/cookbook_program_chatbot
# Run the same command with NEOGRAPH_CHAT_POSTGRES_URL for PostgreSQL.
```

black-box suite 覆盖 concurrent 两 tenant、keep/swap、recursive reviewer、idempotent request、restart、budget exhaustion、authorization 和真实 provider adapter 的 local HTTP fixture，不声称 external OpenRouter 成功。`--script scenario.json` 接受 `{tenant, request_id, message, force_swap?}` 数组；`--crash-after-script` 在 committed snapshot 后退出而不 cancel family，用于 restart scenario。

## 早期 Core example

`server.cpp`/`server_multi.cpp` 和原 target 选择下一 request 的 Core graph，不演示新 `cookbook_program_chatbot` 的 live Program replacement。保留的迁移前测量为单 Alice16秒(当时约$0.003)、五客户/25turn/76model call的424秒·18.99MB·三 compiled engine(当时约$0.02)。最终 topology 是 fanout三/reflexive一/simple一。这不是当前 route 定价或已迁移 source 验证。
