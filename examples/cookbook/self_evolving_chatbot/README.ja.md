<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=ja source_sha256=e7bb3e4608b1e5f3af734a49afa0976e6b65090989f96eda60028e0c60afc094 -->
# 自己進化型チャットボット

## 現在の型付き Program chat 契約

`program_chat.cpp` は `evolving-chat/v6`、`chat.step` version `1.5.0`
（manifest digest `chat.step/v6`）、ledger schema `neograph.program-chat-call/v6` を使用します。
既存 browser/HTTP protocol は不変です。Alice/Bob owner scope、reviewed-template generation、
役割別 prompt、effect/capability grant、正確な checkpoint lineage、nonrenewable budget は host が所有します。

既存 `server_multi.cpp` live-provider 経路の timeout は180秒です。
ProgramChat の `--provider-timeout-seconds` 範囲/既定値や共有 provider factory の既定値は
変えず、failure 再送も有効にしません。

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

loopback browser は `http://127.0.0.1:8768`。`alice-demo`/`bob-demo` は demo bearer token で、
production 認証ではありません。live には OpenRouter eligible route、鍵、network が必要で、
CLI default は GLM-5.3-Flash です。archive は host-only、既定は `DB_PATH.native`。
独立鍵と owner-private custody を DB と共に保持します。認証 custody であり暗号化や vendor issuer proof ではありません。
raw native payload、鍵、prompt、ledger artifact を公開 log に出さないでください。
この durable recipe は archive を必要としますが、真正 C++ in-memory checkpoint sidecar は不要です。

各 `turn:role` は一度 prepare し、正確な `Provider::request_digest(prepared)` を計算、
`Provider::conservative_token_upper_bound(prepared)` を予約し pending claim を
永続化してから同じ handle を dispatch します。全呼び出し範囲の上限は承認された
モデルの input/output 上限、実際の output cap、hosted invocation 上限と retry 方針から計算します。
モデルの事実がなければ予約と dispatch の前に拒否します。private loopback fixture も
明示的な対応モデルの方針事実が必要で、mock は上限や provider usage を捏造しません。
予約は provider usage、forecast、invoice ではありません。nullable wide provider count は
`charged_tokens` と別で、known zero は欠落ではありません。consistent final input/output
evidence があり、以前の usage が unknown でなく transport 内部再送もない場合だけ精算します。
それ以外は元の予約を `UnknownHold` として保持します。予約超過 report も charge に含め、
`cost` は unknown です。
`--descriptor-policy PATH`（C++ `Options::descriptor_policy_file`）でホスト所有の SDK
descriptor-policy JSON を指定します。組み込み codec resource snapshot で一度承認して
両 tenant で共有し、指定しない場合は組み込み方針を使います。明示的な fixture 方針は正確な
loopback `openrouter_origins` と `program-chat-mock`（または指定した fixture モデル）の
上限を宣言する必要があります。承認された方針 identity をセッション settings に結び付けるため、
同じパスでも内容が変われば新しいセッションが必要です。モデル提案や checkpoint は方針を
選択できません。`--mock` にもこれらの事実が必要で、予算増加は明示的に指定します。
提供される [demo-policy.json](demo-policy.json) は `program-chat-mock` の input 4,096・output
65,536 token の事実だけを宣言します。offline recipe で明示的に選択してください。live model catalog、
spending grant、usage report や budget reset ではありません。明示的な `--model` は admission を
弱めずに継承された model 環境設定を上書きします。`--max-output-tokens` は正の64-bit値を受け取り、
選択された model limit と既存 session budget に従います。固定8,192 CLI上限はありません。

restart では pending は `UnknownHold` となり自動再送しません。completed replay は同じ prepared
 digest/reservation を要求し、archive から元の不変 Outcome/native role history を復元して settlement/output を検証します。
DB、archive/key、session、provider/model/settings、build identity を保持し、変更には新 session が必要です。
SDK retry は off（`max_attempts=1`）。restart/置換は budget を更新しません。
model JSON は提案であって native authority ではありません。
現在の範囲限定実行証拠：実際の ProgramChat PostgreSQL blackbox で6 scenario が
18.989秒で pass し、実際の browser で Alice/Bob 隔離と generation-2 置換を観察しました。
この model-free 証拠は vendor 推論 qualification や別の `multi_tenant_chatbot` server/load
recipe、全 storage 変種、移行済み Python provider binding の実行証拠ではありません。

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`cookbook_program_chatbot` は Alice/Bob の message、owner scope、catalog、engine cache、Program family、call ledger、nonrenewable budget を分けます。各 turn の bounded Harness proposal を評価し、承認済み immutable successor を assistant の durable checkpoint で置換します。orchestrator は同じ論理 assistant を待ちます。review は draft → 独立 Program identity の reviewer child 合成/待機 → 修正です。inspector は Core JSON、DSL、tree、generation、残 budget、理由、topology diff、admission を示します。SQLite/PostgreSQL は artifact、transition、chat、reservation/result、evolution decision を保存します。OpenRouter は typed SDK Chat provider、offline demo は同じ compiler/admission/runtime API を使います。

## ビルドと実行

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

`http://127.0.0.1:8768` で Alice/Bob を選びます。demo の `review`、`검토`、`비교` は review Harness を提案します。最初の回答は現在の Harness、承認後の変更は次 turn の resume で適用します。force checkbox はレビュー済み二 plan を交替するだけで品質改善の主張ではありません。

live OpenRouter の key と利用可能 model を設定します：

```bash
export OPENROUTER_API_KEY='...'
export OPENROUTER_MODEL='z-ai/glm-5.3-flash'
./build-chat/cookbook_program_chatbot --live --session openrouter-demo
# Or read an existing dotenv file without printing its values:
./build-chat/cookbook_program_chatbot --live --env-file /path/to/.env \
  --model z-ai/glm-5.3-flash --session glm-demo
```

既定は `z-ai/glm-5.3-flash`。`--model` は `OPENROUTER_MODEL`、process 環境は dotenv に優先します。`--env-file` がなければ working directory に近い `.env` を探し、`--no-env` は探索を止めます。named chatbot setting だけを読み、file は data として parse し shell source しません。GLM 5.3 Flash chat は output cap 内に visible reply を残すため `reasoning_effort=low` が既定。`--reasoning-effort default` は override を省略し、explicit 値は model/provider の対応が必要です。DSL capability evaluator は別の generation 設定です。

ZDR eligible route が必要です。token price を仮定しません。DSL/DB/HTTP body/source に credential を入れないでください。provider、model、skill、output cap、build の変更は新 `--session` が必要で、再開は既存 budget limit を保持します。`NEOGRAPH_CHAT_BASE_URL` は互換 endpoint を指定でき、plain HTTP は literal loopback と `--allow-loopback-provider` が必要な protocol test 用です。

output limit は [OpenRouter chat API](https://openrouter.ai/docs/api/api-reference/chat/create-a-chat-completion) の `max_completion_tokens` に送ります。既定4,096は provider reasoning token を含み、`--max-output-tokens` は正の64-bit値です。host model limit/session budget は残り、固定8,192 CLI cap はありません。`--provider-timeout-seconds` は1..120秒(既定120)。checkpoint wait は順次 call を含み、reviewer child は別180秒 budget。timeout は予約を `UnknownHold` に保ち自動 retry しません。

WSL native Docker に専用 PostgreSQL DB を作り同じ shell に URL を設定します。example は table を作りますが production deployment/migration manager ではありません。

```bash
export NEOGRAPH_CHAT_POSTGRES_URL='postgresql://USER:PASSWORD@127.0.0.1:PORT/DATABASE'
./build-chat/cookbook_program_chatbot --mock --session postgres-demo
```

変数があれば chat/Program persistence は PostgreSQL、なければ `--db` は SQLite を選びます。各 backend は build 時に無効にできます。

## 合成するもの

model は `{plan, reason, confidence}` を返します。host は `direct`/`review`、bounded reason、[0,1] confidence のみ許可します。0.7未満、invalid JSON、unknown plan は reject、同じ plan は `kept`。evolution は JSON-object response mode を要求しても application が exact field/value を検証します。空/切れた proposal は完成済み回答を失わず reject します。known usage は精算し stop reason は diagnostic に保存、transport uncertainty は自動 redispatch を防ぎます。

レビュー済み JavaScript template を parameterize します。source identity、bounded compilation、host semantic/template validation、Catalog admission、generation CAS が必要です。child は durable synthesis gateway と独立に保存した host grant を通ります。model は grant/credential/arbitrary import/native code/追加 budget を発行できません。reviewed-template synthesis の検証であって任意 model-written JavaScript や回答品質の証明ではありません。QuickJS generator control/model node は `Unmanaged`、inspector は unknown external model effect の strict replay を主張しません。

## Agent authoring skill

host は [SKILL.md](../../../skills/neograph-harness-authoring/SKILL.md) と `references/chat-template-proposals.md` を evolution model の system context に入れます。answer/reviewer は役割別 prompt。skill digest は session、actual prompt/output cap は call identity に結び付きます。以前の build DB は新 session を使います。

skill の別 QuickJS authoring/native runtime-handoff guide を source-generation evaluator が使います。native compiler manifest を model に渡し、返された source を compile、rejected diagnostic を bounded repair に返します。[DSL capability evaluation](../../../docs/DSL_CAPABILITY_EVAL.md) を参照してください。chatbot template path は model-callable compiler tool を公開しません。

## Accounting と recovery

tenant session の既定は12turn/100model call/200,000model token。dispatch 前に admitted whole-window bound を予約します。以前の usage unknown や transport 内 resend がない場合だけ consistent final usage で精算し missing usage は予約を保ちます。model limit がなければ request byte から token を推定せず dispatch 前に reject。reservation 超過 report も charge。monetary cost は unknown で price/monetary ceiling はありません。

Program compile/operation/Core step/child-depth/wall-time budget は restart/replacement で更新しません。idle も wall-time を消費。dynamic successor は held checkpoint で host-only `ProgramRuntime::reserve_synthesis` により先に予約します。

同じ DB/session/provider/model で再開し、最初の turn が family を reconnect して最後の checkpoint を reconcile します。stored result は再利用、pending/uncertain call は自動再送しません。interrupted compilation intent は無料 recompile なしで残ります。replacement 前に保存した admitted successor は exact held checkpoint から publish、committed replacement は lineage で解決します。DB/session あたり一 server process のみ。loopback listener と `alice-demo`/`bob-demo` は production 認証ではなく、explicit cancellation は process-loss restart と違い family を閉じます。

## 検証

```bash
python3 examples/cookbook/self_evolving_chatbot/test_program_chat.py \
  ./build-chat/cookbook_program_chatbot
# Run the same command with NEOGRAPH_CHAT_POSTGRES_URL for PostgreSQL.
```

black-box suite は concurrent tenant 二つ、keep/swap、recursive reviewer、idempotent request、restart、budget exhaustion、authorization、実際の provider adapter の local HTTP fixture を扱います。external OpenRouter 成功は主張しません。`--script scenario.json` は `{tenant, request_id, message, force_swap?}` 配列、`--crash-after-script` は family を cancel せず committed snapshot 後に終了する restart scenario 用です。

## 以前の Core example

`server.cpp`/`server_multi.cpp` と旧 target は次 request の Core graph を選び、新 `cookbook_program_chatbot` の live Program replacement は示しません。保存された移行前の記録は単独 Alice16秒(当時約$0.003)、五顧客/25turn/76model callの424秒・18.99MB・compiled engine三つ(当時約$0.02)です。最終 topology は fanout三/reflexive一/simple一。この記録は現在 route の価格や移行 source の検証ではありません。
