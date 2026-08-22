<!-- neograph-i18n: source=examples/26_postgres_react_hitl/README.md locale=ja source_sha256=c03d573ec24eaf1dd00c474339be503d57dd52b0c8804806bd3035ff4901192f -->
# 例26 — PostgreSQLバックエンドのDeep Research with HITL

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

2つのNeoGraph機能をエンドツーエンドで実演する：

1. **`PostgresCheckpointStore`** — 実PostgreSQLにおけるチャンネルブロブ重複排除を備えた耐久チェックポイント。
2. **NodeInterrupt駆動のHITL** — Deep Researchグラフはレポートを生成した後に一時停止し、人間による確認を待ち、承認（→終了）またはフィードバック（→もう一度研究ラウンド）のいずれかで再開する。

このデモは意図的に**プロセス非連続**である：バイナリはレポート生成後に終了する。そのため`resume`を行うとき、あなたはすべてをPGから再ロードしなければならない新しいプロセスである。それがまさに要点である — チェックポイントが実際にプロセス境界を越えたことを証明する。

## シナリオ

以下のウォークスルーは元のアイデアを反映しています。エージェントに最新のVision Transformer論文を尋ね、レポートにURLが引用されていないことに気づき、引用を求めて送り返すというものです。

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

エージェントプロセスはここで**終了**する — チェックポイントはPGにある。次にフォローアップする：

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

完了を承認する:

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 approve
--- Final report (approved) ---
... final markdown ...
```

## 実際に永続化されたことを検証する

PGに入って行を確認してください。`neograph_checkpoint_blobs`が`channels × checkpoints`より行数が少ないのは、重複排除によるものです。

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

`final_report`は生成されたレポートごとに1行になります。スーパーステップ間で変化しなかったチャンネル(`user_query`、`research_brief`)は合計でちょうど1行になります。

### 実際の実行からの参照番号

上記のマルチモーダルRAGデモでの完全な実行・再開・再開サイクル(スーパーバイザー2ラウンド×リサーチャー各2名、OpenRouter経由でDeepSeek固定)で、以下のPG数値が生成されました。

| メトリック                      | 値      | 注記 |
|-----------------------------|------------|-------|
| `neograph_checkpoints`      | 15行    | 6スーパーステップ + 1 NodeInterrupt + 6スーパーステップ + 1 NodeInterrupt + 1 approve cp |
| `neograph_checkpoint_blobs` | 29 行    | 理論上の15 cps × 9チャンネル = 135と比較 — **78.5%の重複排除** |
| `neograph_checkpoint_writes`| **0行** | クリーン — 各スーパーステップの保留ログはコミット時にクリアされた |
| `final_report` v13（ラウンド1）| 2806バイト     | arXiv URLなし（モデルは`arXiv:NNNN.NNNNN`の短縮形を使用） |
| `final_report` v27（ラウンド2）| 2752バイト     | ユーザーの要求後の完全な`https://arxiv.org/abs/...` URL |
| 合計blobバイト数            | 41 KB      | スレッド状態全体（すべてのLLMトランスクリプトを含む） |

`final_report` v13 → v27の差分は、ユーザーフィードバックが実際にエージェントの出力を変更したことを証明する決定的な証拠である：2番目のレポートは散文を削減し、URL引用を追加した — スーパーバイザーがこの品質向上に到達できたのは、HITLゲートがフィードバックを`supervisor_messages`にフィードバックした場合のみである。

## セットアップ

1. 資格情報をコピーして入力してください：
   ```
   cp .env.example .env
   # set OPENROUTER_API_KEY; set CRAWL4AI_API_TOKEN to a fresh `openssl rand -hex 32` value
   ```
2. サポートサービスを起動してください：
   ```
   docker compose up -d postgres crawl4ai
   ```
3. デモを実行します（上記の「シナリオ」を参照）。最初の`docker compose run`が`agent`イメージのビルドをトリガーします（ウォームマシンで約1分）。

完了した場合：
```
docker compose down       # stop services, keep PG volume
docker compose down -v    # drop the PG volume too
```

## バイナリを直接実行する（エージェントにdocker-composeを使用しない）

また、ホスト上でバイナリをビルドし、docker-compose管理のPostgres + Crawl4AIを指定することもできます：

```
cmake -B build -DNEOGRAPH_BUILD_POSTGRES=ON -DNEOGRAPH_BUILD_TESTS=OFF
cmake --build build --target example_postgres_react_hitl -j

./build/example_postgres_react_hitl run "...your query..."
./build/example_postgres_react_hitl resume <thread_id> "feedback"
./build/example_postgres_react_hitl status <thread_id>
```

ホスト側の.envの`POSTGRES_URL=postgresql://postgres:test@localhost:55432/neograph`はcompose公開ポートを指します。`CRAWL4AI_URL`はCrawl4AIに対しても同様です。`CRAWL4AI_API_TOKEN`はそのベアラ認証情報として送信され、composeファイルは空の値を拒否し、Crawl4AIを`127.0.0.1`のみに公開します。

## このスタックに対して統合テストを実行する

composeファイルは、同じPostgresインスタンス上に**別の`neograph_test`データベース**をプロビジョニングし、PostgresCheckpointStore統合テスト専用にします（SetUpで`drop_schema()`を呼び出し、そうしないとデモスレッドが消去されます）。テストは次で実行します：

```bash
NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir ../../build -R PostgresCheckpoint --output-on-failure
```

テストURLを`/neograph`ではなく`/neograph_test`に向けると、デモDBにあるスレッドデータがすべて消去されます。そうしないでください。

## フォレンジックのヒント — ユーザーのフィードバックがPGのどこにあるか

`messages`チャンネルを直接クエリすると(エンジンが再開値を`HumanReviewNode`に渡すために使用するチャンネル)、空の配列のみが表示されます。

```
SELECT version, blob_data FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'messages';
-- 0 | null
-- N | []
-- M | []
```

それは意図的なものであり、データ損失ではない: `HumanReviewNode` は受信したユーザーメッセージを消費し、直ちに `messages: []` を自身の `Command.updates` に書き戻すため、将来の割り込みサイクルはクリーンなチャネルから開始される。チェックポイントが取得される時点で、配列はすでに空である。

実際のフィードバックテキストは `supervisor_messages` チャネルに存在し、マーカー `[USER FOLLOW-UP after reviewing...]` が前置される:

```
SELECT blob_data::text FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'supervisor_messages'
 ORDER BY version DESC LIMIT 1;
```

そのマーカーについて`grep`を実行すると、スレッド内のすべてのHITLラウンドでユーザーが言ったことをすべて回復できます。

## 実装ノート

- HITL ゲートは Deep Research グラフ内の `DeepResearchConfig::enable_human_review` フラグの背後に組み込まれている(デフォルトはオフなので、例25は影響を受けない)。オンにすると、`HumanReviewNode` が `final_report` と `__end__` の間に配置される。
- そのノードは、初回実行時に`NodeInterrupt`をスローします。エンジンはそれをキャッチし、フェーズ`NodeInterrupt`でチェックポイントを保存し、呼び出し元に再スローします。再開時、エンジンはユーザーの返信を`messages`チャネルに書き込んだ状態で、同じノードに再入場します。
- ノードは「承認」(→ Command(__end__)) とフィードバック (→ Command(supervisor) で、フィードバックが`supervisor_messages`に追加され、イテレーションカウンタがリセットされる) を区別します。どちらの経路も実行をきちんと終了するため、PGは常に一貫した最新のcpを保持します。
- このシナリオの3つのステップすべて (初回実行、フィードバックでの再開、承認での再開) がプロセス境界を越えます。エンジン状態は、呼び出し間の全体がPG内に存在します。

## なぜフロントエンドがないのか

「バイナリが終了 → 再起動 → 続行」というフローが、まさにフロントエンドです。Web UIがあっても、同じ引数をHTTP経由でマーシャリングするだけで、チェックポイントの永続性のデモンストレーションに何かを加えることはありません。確認のための視覚的な証拠として、上のPGテーブルを直接チェックしてください。
