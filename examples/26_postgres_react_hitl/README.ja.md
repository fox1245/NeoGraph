<!-- neograph-i18n: source=examples/26_postgres_react_hitl/README.md locale=ja source_sha256=b8c1274535db9f44a2a3c254c0b4de2c4dba30f23d37f69e80c0f31a365e6511 -->
# 例 26 — Postgres を利用した HITL を使用したディープリサーチ

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

2 つの NeoGraph 機能をエンドツーエンドでデモンストレーションします。

1. **`PostgresCheckpointStore`** — 実際の PostgreSQL の永続的なチェックポイント
   チャネルブロブ重複排除を使用します。
2. **NodeInterrupt-driven HITL** — Deep Research グラフは次の時間に一時停止します。
   レポートを作成し、人間がレビューして、次のいずれかを再開します。
   承認 (→ 終了) またはフィードバック付き (→ 別の調査ラウンド)。

デモは意図的に**プロセスが不連続**です: バイナリが終了します
レポートを作成した後なので、`resume` を実行すると、新しいプロセスになります。
PG からすべてをリロードする必要があります。それが要点だ — が証明する
チェックポイントは実際にプロセス境界を越えました。

## シナリオ

以下のウォークスルーは、元のアイデアを反映しています。エージェントに質問します。
最新の Vision Transformer 論文、レポートには URL が引用されていないことに注意してください。
そして引用のために返送してください。

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

エージェント プロセスはここで **終了**します。チェックポイントは PG にあります。フォローアップしてください:

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

承認して終了:

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 approve
--- Final report (approved) ---
... final markdown ...
```

## 本当に持続していることを確認する

PG にジャンプして行を確認します。`neograph_checkpoint_blobs` がどのように表示されるかに注目してください。
重複排除のおかげで、`channels × checkpoints` よりも行数が少なくなります。

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

`final_report` には、生成されたレポートごとに 1 行が含まれます。チャンネル
スーパーステップ間で変化はありませんでした (`user_query`、`research_brief`)
合計は 1 行だけです。

### 実走時の参考数値

上記のマルチモーダル RAG デモの完全な実行、再開、再開サイクル
(2 回の監督ラウンド × 各 2 人の研究員、OpenRouter 経由の固定 DeepSeek)
これらの PG 番号が生成されました。

|メトリック |値 |メモ |
|-----------------------------|------------|-------|
| `neograph_checkpoints` | 15 行 | 6 スーパーステップ + 1 ノード割り込み + 6 スーパーステップ + 1 ノード割り込み + 1 承認 cp |
| `neograph_checkpoint_blobs` | 29 行 |対理論値 15 cps × 9 チャネル = 135 — **78.5% 重複除去** |
| `neograph_checkpoint_writes`| **0 行** | clean — すべてのスーパーステップの保留中のログがコミット時にクリアされました。 |
| `final_report` v13 (ラウンド 1)| 2806 B | arXiv URL なし (モデルは `arXiv:NNNN.NNNNN` 短縮表現を使用) |
| `final_report` v27 (ラウンド 2)| 2752B |ユーザーが要求した後の完全な `https://arxiv.org/abs/...` URL |
| BLOB の合計バイト数 | 41KB |すべての LLM トランスクリプトを含むスレッド全体の状態 |

`final_report` v13 → v27 の差分は、ユーザーが
フィードバックによってエージェントの出力が実際に変更されました: 2 番目のレポート
散文をトリミングし、URL 引用を追加 - 品質の向上
HITL ゲートがフィードバックをフィードバックしたため、スーパーバイザにのみ到達しました
`supervisor_messages`に。

## 設定

1. 資格情報をコピーして入力します。
   ```
   cp .env.example .env
   # OPENROUTER_API_KEY を設定し、CRAWL4AI_API_TOKEN には新しい `openssl rand -hex 32` の値を設定します。
   ```
2. サポート サービスを起動します。
   ```
   docker compose up -d postgres crawl4ai
   ```
3. デモを実行します (上記の「シナリオ」を参照)。最初の `docker compose run`
   `agent` イメージのビルドをトリガーします (暖かいマシンで約 1 分)。

完了したら:
```
docker compose down       # stop services, keep PG volume
docker compose down -v    # drop the PG volume too
```

## バイナリを直接実行する (エージェントの docker-compose なし)

ホスト上でバイナリをビルドし、それを指定することもできます。
docker-compose で管理された Postgres + Crawl4AI:

```
cmake -B build -DNEOGRAPH_BUILD_POSTGRES=ON -DNEOGRAPH_BUILD_TESTS=OFF
cmake --build build --target example_postgres_react_hitl -j

./build/example_postgres_react_hitl run "...your query..."
./build/example_postgres_react_hitl resume <thread_id> "feedback"
./build/example_postgres_react_hitl status <thread_id>
```

ホスト側の .env の `POSTGRES_URL=postgresql://postgres:test@localhost:55432/neograph`
は compose 公開ポートを指し、`CRAWL4AI_URL` も Crawl4AI で同様です。
`CRAWL4AI_API_TOKEN` は Bearer 資格情報として送信されます。compose は空の値を
拒否し、Crawl4AI を `127.0.0.1` にのみ公開します。

## このスタックに対して統合テストを実行する

構成ファイルは、**別の `neograph_test` データベース**をプロビジョニングします。
特に同じ Postgres インスタンス
PostgresCheckpointStore 統合テスト (`drop_schema()` を呼び出す)
SetUp で実行し、それ以外の場合はデモ スレッドを消去します)。以下を使用して実行します。

```bash
NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir ../../build -R PostgresCheckpoint --output-on-failure
```

テスト URL を `/neograph_test` ではなく `/neograph` に指定すると、
デモ DB にあるスレッドデータを削除します。絶対にそうしないでください。

## フォレンジックのヒント — ユーザーのフィードバックが PG に保存される場所

`messages` チャネルを直接クエリすると (チャネルはエンジン
再開値を `HumanReviewNode` に渡すために使用されます）のみが表示されます
空の配列:

```
SELECT version, blob_data FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'messages';
-- 0 | null
-- N | []
-- M | []
```

これはデータ損失ではなく、意図的なものです。`HumanReviewNode` は
受信したユーザー メッセージを受信し、すぐに `messages: []` を書き戻します。
`Command.updates` なので、今後の割り込みサイクルはクリーンな状態から開始されます。
チャネル。チェックポイントが取得されるまでに、アレイはすでに
空の。

実際のフィードバック テキストは `supervisor_messages` チャネルにあります。
先頭にマーカー `[USER FOLLOW-UP after reviewing...]` が付きます:

```
SELECT blob_data::text FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'supervisor_messages'
 ORDER BY version DESC LIMIT 1;
```

そのマーカーの `grep` は、ユーザーが発言した内容をすべて復元します
HITL はスレッド内でラウンドします。

## 実装メモ

- HITL ゲートは、Deep Research グラフの背後に組み込まれています。
  `DeepResearchConfig::enable_human_review` フラグ (デフォルトはオフなので、
  例 25 は影響を受けません)。これを装着すると、`HumanReviewNode` が座ります
  `final_report` と `__end__` の間。
- そのノードは最初の実行時に `NodeInterrupt` をスローします。エンジン
  それをキャッチし、フェーズ `NodeInterrupt` でチェックポイントを保存し、
  呼び出し元に再スローします。再開すると、エンジンは同じ状態に戻ります
  ユーザーの応答が `messages` チャネルに書き込まれたノード。
- ノードは「承認」(→ コマンド(__end__)) とフィードバックを区別します。
  (→ コマンド(スーパーバイザー)にフィードバックが追加される
  `supervisor_messages` と反復カウンターのリセット)。両方のパス
  実行をクリーンに終了するため、PG は常に一貫した最新の cp を持ちます。
- シナリオの 3 つのステップすべて (最初の実行、フィードバックを伴う再開、
  承認で再開) プロセス境界を越えて - エンジンの状態
  呼び出しの間は完全に PG 内に存在します。

## なぜフロントエンドがないのでしょうか？

「バイナリの終了→再起動→続行」の流れがフロントエンドです。ウェブUI
HTTP 経由で同じ引数をマーシャリングするだけで、追加はしません
チェックポイントの耐久性を実証するために何でも。 PGを検査する
視覚的に証明するために直接表 (上) を作成します。
