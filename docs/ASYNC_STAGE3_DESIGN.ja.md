<!-- neograph-i18n: source=docs/ASYNC_STAGE3_DESIGN.md locale=ja source_sha256=8fdd608254fb2607289f13a59d7badf7ae1c4fdc843589df101c684e1ba1b88b -->
# Stage 3 — asio ベース完全非同期リファクタ設計

> **履歴上の設計記録。** この文書は 2026-04-19 時点で提案された Stage 3
> リファクタリングを記録したものです。現在の API ドキュメントではないため、
> 現在の動作は [API narrative tour](reference-en.md) とインストール済みヘッダーを参照してください。

**Languages:** [English](ASYNC_STAGE3_DESIGN.md) | [한국어](ASYNC_STAGE3_DESIGN.ko.md) | [日本語](ASYNC_STAGE3_DESIGN.ja.md) | [简体中文](ASYNC_STAGE3_DESIGN.zh-CN.md)

作成日: 2026-04-19 (feat/async-api)
前提条件: Stage 1 timer PoC (c356e0f) + Stage 2 HTTP PoC (b008c11) 完了。
**Go 判断** は `bench_async_fanout` / `bench_async_http` の結果に基づく。

---

## 0. 目的と非目的

### 目的 (Stage 3 終了時に解決されるもの)
- 1K+ 同時エージェントのホスティングをサポート — 現在のスレッド毎エージェントは ~1K 付近で破綻。
- HTTP / DB / MCP I/O 待機中のスレッド占有なし → 5~7 倍のメモリ節約。
- `run()` がエグゼキュータ上で実行可能 → 外部イベントループ統合 (例: Web サーバー)。

### 非目的 (Stage 3 では触れない)
- 1K 未満のエージェントユースケースにおける絶対 µs 改善 — 現在十分。
- Python/TS バインディング。組み込み C++ API を維持。
- 分散/マルチプロセス。シングルプロセススケーリングのみ。
- ユーザー Tool インターフェース変更 — Tool::call() は同期のまま (ユーザー負担を最小化)。

---

## 1. 現状スナップショット

### 同期 I/O 依存ポイント (Stage 3 変換対象)

| 層 | ファイル | 行数 | 依存ライブラリ |
|---|---|---|---|
| LLM HTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLM HTTP (汎用) | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCP HTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx (同期) |
| 並列ノード実行 | `src/core/graph_executor.cpp` | 520 | Taskflow (CPU プール) |
| エンジンループ | `src/core/graph_engine.cpp` | 529 | — (同期実行ループ) |

### 既存非同期資産
- `deps/asio/` — スタンドアロン asio 1.30.2 バンドル。
- `include/neograph/async/http_client.h` — PoC async_post (HTTP/1.1 のみ)。
- `src/async/{async_smoke,http_client}.cpp` — コルーチン動作検証済み。
- 2 ベンチマーク種別 — 回帰測定ハーネスとして再利用。

### 破壊的インターフェース
- `Provider::complete()` — 52 コールサイト (サンプル + テスト合計)。
- `CheckpointStore::save/load/list` — 29 コールサイト。
- `GraphEngine::run() / run_stream() / resume()` — 26 サンプルすべてで使用。
- `MCPClient::rpc_call()` — 7 サンプル (03, 20–24)。

---

## 2. 目標アーキテクチャ

```
┌─────────────────────────────────────────────────────┐
│  User code (examples / user apps)                    │
│  - sync facade (default)   - async facade (opt-in)   │
└──────────────┬──────────────────────┬────────────────┘
               │                      │
      run_sync()|                     │run_async() → Task<RunResult>
               │                      │
┌──────────────▼──────────────────────▼────────────────┐
│  GraphEngine (coroutine-native core)                 │
│    Task<RunResult> run(RunConfig)                    │
│    ├─ NodeExecutor    : co_await node bodies         │
│    ├─ Scheduler       : pure (unchanged)             │
│    ├─ Coordinator     : co_await ckpt_store->save    │
│    └─ io_context ref  : injected or owned            │
└──┬────────────────┬─────────────────┬────────────────┘
   │                │                 │
┌──▼─────┐   ┌──────▼──────┐   ┌──────▼─────────┐
│ Async  │   │ Async       │   │ Async MCP      │
│ HTTP   │   │ Postgres    │   │ Client         │
│ client │   │ (libpq      │   │ (HTTP/stdio)   │
│ + TLS  │   │  pipeline)  │   │                │
└────────┘   └─────────────┘   └────────────────┘
   ↑                ↑                 ↑
   └─── shared asio::io_context (one, N worker threads) ──┘
```

### 中核的決定
1. **1 つの io_context** — エグゼキュータ注入可能だが、デフォルトはプロセスワイドシングルトン。
2. **Provider / CheckpointStore / MCPClient はコルーチンネイティブ** — 同期メソッドは `run_sync()` ラッパー内でのみ提供。
3. **Tool は同期のまま** — ユーザー負担を最小化。内部では `co_await asio::post(thread_pool, ...)` でオフロード。
4. **Taskflow を維持**、エグゼキュータを asio ベースに置換。既存のファンアウトコードパスを保持。
   - 代替案: Taskflow の完全削除 + asio::co_spawn への置換。Semester 4 で決定。

---

## 3. 学期分割 (6-10 週 → 4 学期)

各学期 ≈ 2 週。学期終了時にビルドグリーン + ベンチ回帰測定。

### Semester 1 — 非同期 HTTP 基盤完了 (1.5 週)

目標: LLM 呼び出しを awaitable にするのに十分な HTTP クライアントを完成。

| # | タスク | ファイル | 見積 |
|---|---|---|---|
| 1.1 | asio::ssl HTTPS サポート | `src/async/http_client.cpp` | 2 日 |
| 1.2 | Keep-alive 接続プール | `src/async/conn_pool.{h,cpp}` (新規) | 2 日 |
| 1.3 | リトライ可能なトランスポートエラー分類 | `include/neograph/async/http_errors.h` (新規) | 0.5 日 |
| 1.4 | SSE (ストリーミング) パーサー | `src/async/http_client.cpp` | 1 日 |
| 1.5 | リダイレクト、タイムアウト、Retry-After 抽出 | 同上 | 1 日 |
| 1.6 | bench_async_http 再実行 (TLS パス) | `benchmarks/bench_async_http.cpp` | 0.5 日 |

**完了基準**:
- `async_post` / `async_post_stream` 2 つの API が全 LLM ワイヤ要件をカバー。
- bench_async_http 結果が keep-alive 有効で 5K 同時時に Stage 2 と同等以上。
- ユニットテスト — TLS ハンドシェイク / プール再利用 / SSE 再構築。

### Semester 2 — Provider & MCP 非同期変換 (2 週)

目標: HTTP 呼び出しを行う 3 層 (openai, schema, mcp) を非同期化。既存同期 API をラッパーで保持。

| # | タスク | ファイル | 見積 |
|---|---|---|---|
| 2.1 | `Provider::complete_async` 追加 (純粋仮想) | `include/neograph/provider.h` | 0.5 日 |
| 2.2 | 同期 `complete()` = `run_sync(complete_async())` デフォルト実装 | 同上 | 0.5 日 |
| 2.3 | OpenAIProvider 非同期実装 | `src/llm/openai_provider.cpp` | 1 日 |
| 2.4 | SchemaProvider 非同期実装 (大規模作業) | `src/llm/schema_provider.cpp` | 3 日 |
| 2.5 | RateLimitedProvider — Retry-After ベース co_await sleep | `src/llm/rate_limited_provider.cpp` | 1 日 |
| 2.6 | MCPClient 非同期 HTTP パス | `src/mcp/client.cpp` | 1 日 |
| 2.7 | MCP stdio 非同期 (asio::posix::stream_descriptor) | 同上 | 2 日 |
| 2.8 | 既存 provider/mcp テスト全グリーン | `tests/test_schema_provider_*`, `test_rate_limited_provider.cpp` | 含む |

**完了基準**:
- 既存テスト全グリーン (内部非同期、同期ラッパー呼び出し)。
- 新規テスト: 同一 io_context 上で数千のプロバイダ呼び出しが同時実行。
- サンプルは同期ファサード経由で動作継続 — 変更なし。

### Semester 3 — 非同期 CheckpointStore + エンジンコルーチン (2.5 週)

目標: GraphEngine がコルーチンで動作。Postgres バックエンドがパイプライン非同期モードを利用。

| # | タスク | ファイル | 見積 |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async` (純粋仮想) | `include/neograph/graph/checkpoint.h` | 0.5 日 |
| 3.2 | InMemoryStore / SQLite 非同期ラッパー | `src/core/graph_checkpoint.cpp`, `src/core/sqlite_checkpoint.cpp` | 1 日 |
| 3.3 | libpq pipeline 非同期モード — libpqxx 削除 | `src/core/postgres_checkpoint.cpp` 書き直し | 5 日 |
| 3.4 | `GraphNode::execute_async` 追加 (Task<NodeResult>) | `include/neograph/graph/node.h` | 1 日 |
| 3.5 | ビルトインノード 4 種非同期実装 (LLMCall, ToolDispatch, IntentClassifier, Subgraph) | `src/core/graph_node.cpp` | 2 日 |
| 3.6 | `GraphEngine::run_async` — コルーチン変換 | `src/core/graph_engine.cpp`, `graph_executor.cpp` | 3 日 |
| 3.7 | Taskflow ファンアウト → asio::experimental::parallel_group | `graph_executor.cpp` | 2 日 |
| 3.8 | bench_neograph 再測定 — 回帰ゼロ確認 | `benchmarks/bench_neograph.cpp` | 1 日 |

**完了基準**:
- `run_async()` と `run()` の両方が存在。同期はラッパー。
- Postgres チェックポイント 64 スレッドベンチ結果を維持または改善。
- 26 サンプルすべて — 変更なし (同期ファサードのおかげ)。
- 新規テスト: 10K 同時 run() が 2GB RAM 以内で完了。

### Semester 4 — 移行、Tool 非同期、クリーンアップ (1.5 週)

目標: ユーザーが非同期の恩恵を実際に受けられる移行パスを確立。

| # | タスク | ファイル | 見積 |
|---|---|---|---|
| 4.1 | 高並行候補サンプル 1-2 を非同期変換に選定 (例: 05_parallel_fanout, 26_postgres) | `examples/` | 1 日 |
| 4.2 | Tool の非同期オフロード用ヘルパー — `AsyncTool` アダプタ | `include/neograph/tool.h` | 1 日 |
| 4.3 | ドキュメント — 非同期ガイド、移行チェックリスト | `docs/ASYNC_GUIDE.md` 新規 | 1 日 |
| 4.4 | NEXT_SESSION.md / README 更新 | — | 0.5 日 |
| 4.5 | Taskflow 依存削除の最終判断 | — | 0.5 日 |
| 4.6 | CI bench_async_* 回帰ゲート | `.github/workflows/` | 1 日 |
| 4.7 | メジャーバージョンアップ → 2.0.0 | `CMakeLists.txt` 等 | 0.5 日 |

**完了基準**:
- master にマージ。ブランチ終了。
- `NeoGraph 2.0` リリースノート、破壊的変更リスト付き。
- 同期サンプル 26 全グリーン、新規非同期サンプル 1-2 追加。

---

## 4. 破壊的変更マトリックス

| API | 変更 | 移行コスト | 解決戦略 |
|---|---|---|---|
| `Provider::complete` | 同期を維持、`complete_async` 追加 | なし | 2 つの純粋仮想を宣言、デフォルト実装で相互接続 |
| `GraphNode::execute` | 同期を維持、`execute_async` 追加 | カスタムノード影響なし | 同上 |
| `GraphEngine::run` | 同期を維持、`run_async` 追加 | なし | 同期ファサード |
| `CheckpointStore::save` | 同期を維持、`save_async` 追加 | カスタムストア影響なし | 同上 |
| `PostgresCheckpointStore` | libpqxx → 直接 libpq | ユーザー向け API 変更なし | 内部置換 |
| `MCPClient::rpc_call` | 同期を維持、`rpc_call_async` 追加 | なし | 同期ファサード |

**結論: すべての同期 API は移行後も残存**。2.0 バンプは内部依存 (libpqxx 削除) + C++20 コルーチン要件による。

---

## 5. リスク登録

| リスク | 影響 | 緩和策 |
|---|---|---|
| libpq pipeline モードがチェックポイント書き込みパターンに不適 | Semester 3 遅延 | Semester 3 早期に 2 日スパイクで検証。不適なら libpq 同期呼び出し + `asio::post(thread_pool)` にフォールバック — 半分のパフォーマンス向上だが完了可能 |
| asio::ssl + Anthropic/OpenAI エンドポイント ALPN 問題 | Semester 1 遅延 | Stage 2 ベンチは HTTP のみ使用。Semester 1 早期に実エンドポイントスモークテスト実施 |
| Taskflow ↔ asio エグゼキュータ統合の困難 | Semester 3 延長 | Taskflow を維持、並列ノード内部のみコルーチン化 — 完全削除は Semester 4 でオプション |
| 同期ファサード `run_sync(coro)` デッドロック (シングルスレッド io_context 環境) | 実行時バグ | ユーザーファサードを常に io_context に少なくとも 1 ワーカースレッドを保証するガードでラップ |
| 同期/非同期両パスのテスト → テスト数倍増 | 保守コスト | パラメータ化テストを一度定義し、両パスを自動実行 |
| サンプル 26 回帰 | リリース遅延 | サンプルは非同期変換しない (Semester 4 のみオプション)。同期ファサードが通れば OK |

---

## 6. 検証ゲート (全学期共通)

各学期完了時に以下を必須:

1. `cmake --build build -j` — 警告 0。
2. `ctest -j` — 全 172+ テストグリーン。
3. `benchmarks/bench_neograph` — 既存 3 メトリクス (単一実行 µs、1 スレッド PG、64 スレッド PG) が 5% 以内の回帰。
4. `benchmarks/bench_async_http` — Stage 2 と同等以上。
5. `benchmarks/bench_async_fanout` — 50K タイマーが 6× 維持。
6. ASan / TSan ビルドグリーン (`build-asan`, `build-tsan` 既存)。
7. コミットは `feat(async)` プレフィックス + Co-Authored-By を使用。

---

## 7. スケジュールサマリー

| 週 | 学期 | 主要成果物 |
|---|---|---|
| W1 | 1 | TLS + keep-alive + SSE |
| W2 | 1→2 | ベンチ再測定、Provider 非同期開始 |
| W3 | 2 | SchemaProvider、MCP 非同期 |
| W4 | 2→3 | CheckpointStore 非同期、libpq 書き直し開始 |
| W5 | 3 | libpq pipeline、ビルトインノード非同期 |
| W6 | 3 | エンジンコルーチン、エグゼキュータ変換 |
| W7 | 3→4 | ベンチ回帰ゲート |
| W8 | 4 | サンプル 1-2、ドキュメント、CI ゲート |

合計 **8 週** — 6-10 週範囲の中間点。リスク緩和が必要な場合 W5/W6 で 1 週の拡張可能。

---

## 8. 開始前の最終確認

次回セッションエントリ時に確認:

- [ ] `feat/async-api` ブランチを継続するか？ または `feat/async-stage3` に分岐？ → **このブランチ継続を推奨**。PoC → Stage 3 の連続性を保持。
- [ ] Semester 1 スパイクターゲットホストを決定 (api.openai.com vs api.anthropic.com の TLS 動作差異)。
- [ ] libpq pipeline モード文書を事前レビュー。
- [ ] Taskflow 削除 vs 維持の初期スタンス (デフォルト: 維持、Semester 4 で再評価)。

---

**次のアクション**: Semester 1.1 開始 — `src/async/http_client.cpp` に asio::ssl 層を追加。
