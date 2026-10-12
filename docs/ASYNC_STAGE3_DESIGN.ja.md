<!-- neograph-i18n: source=docs/ASYNC_STAGE3_DESIGN.md locale=ja source_sha256=1b7adfd905a5df954404b35ec17b04b8e334eaee56e2b5f2c8d042b831c9ffa0 -->
# Stage 3 — asioベースの全面非同期化リファクタリングの設計

> **アーカイブ済みの、置き換えられた設計提案です。** この文書が記録しているのは、2026-04-19に提案されたStage 3のリファクタリングです。現在のAPIの指針ではなく、計画されたすべてのゲートが完了したことの証明でもありません。ここに出てくる `Provider::complete` と `complete_async` のペア、および `OpenAIProvider` は、型付きプロバイダーへの切り替え（typed cutover）で削除されました。現在のプロバイダーは、型付きのリクエストを所有し、1回だけ準備して、準備済みの呼び出しをディスパッチします。[APIの解説ツアー](reference-en.md)、[マイグレーションガイド](migration-v0.4-to-v1.0.md)、インストールされたヘッダーを参照してください。SDKの `Client::complete` は、これとは別の、サポートされているAPIです。
>
> 以下の「現在の状態」、スケジュール、ゲート、バージョン番号、次のアクションは、すべて、その提案の日付時点のものです。Pythonは、Stage 3の範囲外でした。現在の切り替えには、型付きのPythonプロバイダーと、所有権付きの結果が含まれます。また、現在のCoreは、LLMノードを無効にしても、外部のSchemaProviderランタイムを必要とします。実装された契約と、プラットフォームの適格性確認の限界については、[非同期ガイド](ASYNC_GUIDE.md)と[Pythonバインディング](python-binding.md)を参照してください。

**Languages:** [English](ASYNC_STAGE3_DESIGN.md) | [한국어](ASYNC_STAGE3_DESIGN.ko.md) | [日本語](ASYNC_STAGE3_DESIGN.ja.md) | [简体中文](ASYNC_STAGE3_DESIGN.zh-CN.md)

作成日：2026-04-19（feat/async-api）
前提：Stage 1のタイマーPoC（c356e0f）と、Stage 2のHTTP PoC（b008c11）が完了していること。
`bench_async_fanout` / `bench_async_http` の結果に基づく、**Goの判断**。

---

## 0. 目的と、目的でないこと

### 目的（Stage 3が終わったときに解決されること）
- 1K以上のエージェントを、同時にホストできるようにします。現在の、エージェントごとにスレッドを使う方式は、約1Kで限界になります。
- HTTP、DB、MCPのI/O待ちの間、スレッドを占有しません。メモリを5〜7倍節約できます。
- `run()` をexecutor上で実行できるようにします。これにより、外部のイベントループ（たとえばWebサーバー）と統合できます。

### 目的でないこと（Stage 3では扱わない）
- 1K未満のエージェントのユースケースでの、マイクロ秒単位の絶対的な改善。現状で十分です。
- Python／TSのバインディング。組み込み用のC++ APIは維持します。
- 分散やマルチプロセス。扱うのは、単一プロセスでのスケーリングだけです。
- 利用者のToolインターフェースの変更。Tool::call() は同期のままにします（利用者の負担を最小にするため）。

---

## 1. 現状のスナップショット

### 同期I/Oに依存している箇所（Stage 3での変換対象）

| 層 | ファイル | 行数 | 依存ライブラリ |
|---|---|---|---|
| LLMのHTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLMのHTTP（汎用） | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCPのHTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx（同期） |
| ノードの並列実行 | `src/core/graph_executor.cpp` | 520 | Taskflow（CPUプール） |
| エンジンのループ | `src/core/graph_engine.cpp` | 529 | —（同期の実行ループ） |

### 既存の非同期の資産
- `deps/asio/` には、スタンドアロンのasio 1.30.2を同梱しています。
- `include/neograph/async/http_client.h` は、PoCのasync_postです（HTTP/1.1のみ）。
- `src/async/{async_smoke,http_client}.cpp` で、コルーチンの動作を確認済みです。
- 2種類のベンチマークは、リグレッション測定のハーネスとして再利用します。

### 互換性を壊す範囲
- `Provider::complete()`：52箇所の呼び出し（サンプルとテストの合計）。
- `CheckpointStore::save/load/list`：29箇所の呼び出し。
- `GraphEngine::run() / run_stream() / resume()`：26個のサンプルすべてが使用。
- `MCPClient::rpc_call()`：7個のサンプル（03、20〜24）。

---

## 2. 目標のアーキテクチャ

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

### 中心となる決定
1. **io_contextは1つにします。** executorを注入することもできますが、デフォルトはプロセス全体で共有するシングルトンです。
2. **Provider、CheckpointStore、MCPClientは、コルーチンネイティブにします。** 同期メソッドは、`run_sync()` ラッパーの中でだけ提供します。
3. **Toolは同期のままにします。** 利用者の負担を最小にするためです。内部では、`co_await asio::post(thread_pool, ...)` でオフロードします。
4. **Taskflowは維持**し、executorをasioベースのものに置き換えます。既存のfan-outのコードパスは保ちます。
   - 代替案：Taskflowを完全に削除し、asio::co_spawnに置き換える。第4期に決定します。

---

## 3. 期の分割（6〜10週間 → 4期）

各期は約2週間です。期末に、ビルドが通ることと、ベンチマークのリグレッション測定を確認します。

### 第1期 — 非同期HTTP基盤の完成（1.5週間）

目標：LLM呼び出しをawaitableにできるところまで、HTTPクライアントを完成させます。

| # | タスク | ファイル | 見積もり |
|---|---|---|---|
| 1.1 | asio::sslによるHTTPSのサポート | `src/async/http_client.cpp` | 2日 |
| 1.2 | keep-alive接続のプール | `src/async/conn_pool.{h,cpp}`（新規） | 2日 |
| 1.3 | リトライ可能なトランスポートエラーの分類 | `include/neograph/async/http_errors.h`（新規） | 0.5日 |
| 1.4 | SSE（ストリーミング）のパーサー | `src/async/http_client.cpp` | 1日 |
| 1.5 | リダイレクト、タイムアウト、Retry-Afterの抽出 | 上と同じ | 1日 |
| 1.6 | bench_async_httpの再実行（TLS経路） | `benchmarks/bench_async_http.cpp` | 0.5日 |

**完了基準**：
- `async_post` と `async_post_stream` の2つのAPIで、LLMのワイヤー上の要件をすべてカバーできること。
- keep-aliveを有効にした5K同時接続で、bench_async_httpの結果がStage 2と同等以上であること。
- 単体テストがあること：TLSハンドシェイク、プールの再利用、SSEの再組み立て。

### 第2期 — ProviderとMCPの非同期化（2週間）

目標：HTTP呼び出しを行う3つの層（openai、schema、mcp）を非同期化します。既存の同期APIは、ラッパーで保ちます。

| # | タスク | ファイル | 見積もり |
|---|---|---|---|
| 2.1 | `Provider::complete_async` を追加（純粋仮想関数） | `include/neograph/provider.h` | 0.5日 |
| 2.2 | 同期の `complete()` を、`run_sync(complete_async())` というデフォルト実装にする | 上と同じ | 0.5日 |
| 2.3 | OpenAIProviderの非同期実装 | `src/llm/openai_provider.cpp` | 1日 |
| 2.4 | SchemaProviderの非同期実装（作業量が大きい） | `src/llm/schema_provider.cpp` | 3日 |
| 2.5 | RateLimitedProvider：Retry-Afterに基づくco_awaitによるスリープ | `src/llm/rate_limited_provider.cpp` | 1日 |
| 2.6 | MCPClientの非同期HTTP経路 | `src/mcp/client.cpp` | 1日 |
| 2.7 | MCPのstdioの非同期化（asio::posix::stream_descriptor） | 上と同じ | 2日 |
| 2.8 | 既存のプロバイダーとMCPのテストがすべて通る | `tests/test_schema_provider_*`, `test_rate_limited_provider.cpp` | 上記に含む |

**完了基準**：
- 既存のテストがすべて通ること（内部は非同期、呼び出しは同期のラッパー経由）。
- 新しいテストとして、同じio_context上で、数千のプロバイダー呼び出しが同時に実行されること。
- サンプルは、同期のファサード経由で、変更なしで動作し続けること。

### 第3期 — 非同期のCheckpointStoreと、エンジンのコルーチン化（2.5週間）

目標：GraphEngineをコルーチンで動かします。Postgresのバックエンドは、パイプラインの非同期モードを使います。

| # | タスク | ファイル | 見積もり |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async`（純粋仮想関数） | `include/neograph/graph/checkpoint.h` | 0.5日 |
| 3.2 | InMemoryStoreとSQLiteの非同期ラッパー | `src/core/graph_checkpoint.cpp`, `src/core/sqlite_checkpoint.cpp` | 1日 |
| 3.3 | libpqのパイプライン非同期モード。libpqxxは廃止する | `src/core/postgres_checkpoint.cpp` の書き直し | 5日 |
| 3.4 | `GraphNode::execute_async` を追加（Task<NodeResult>） | `include/neograph/graph/node.h` | 1日 |
| 3.5 | 組み込みノード4種類の非同期実装（LLMCall、ToolDispatch、IntentClassifier、Subgraph） | `src/core/graph_node.cpp` | 2日 |
| 3.6 | `GraphEngine::run_async`：コルーチン化 | `src/core/graph_engine.cpp`, `graph_executor.cpp` | 3日 |
| 3.7 | Taskflowによるfan-outを、asio::experimental::parallel_group に置き換える | `graph_executor.cpp` | 2日 |
| 3.8 | bench_neographの再測定：リグレッションがないことを確認する | `benchmarks/bench_neograph.cpp` | 1日 |

**完了基準**：
- `run_async()` と `run()` の両方があること。同期版はラッパーです。
- Postgresのチェックポイントの64スレッドのベンチマークの結果が、維持または改善されること。
- 26個のサンプルすべてで、何も変わらないこと（同期のファサードのおかげ）。
- 新しいテストとして、10K個の同時 run() が、2GBのRAMの範囲で完了すること。

### 第4期 — 移行、Toolの非同期化、後始末（1.5週間）

目標：利用者が実際に非同期の利点を得られる移行経路を整えます。

| # | タスク | ファイル | 見積もり |
|---|---|---|---|
| 4.1 | 非同期化する、同時実行数の多いサンプルを1〜2個選ぶ（たとえば05_parallel_fanout、26_postgres） | `examples/` | 1日 |
| 4.2 | Toolを非同期にオフロードするためのヘルパー：`AsyncTool` アダプター | `include/neograph/tool.h` | 1日 |
| 4.3 | ドキュメント：非同期ガイド、移行のチェックリスト | `docs/ASYNC_GUIDE.md`（新規） | 1日 |
| 4.4 | NEXT_SESSION.mdとREADMEの更新 | — | 0.5日 |
| 4.5 | Taskflowへの依存を削除するかどうかの最終判断 | — | 0.5日 |
| 4.6 | CIでのbench_async_*のリグレッションゲート | `.github/workflows/` | 1日 |
| 4.7 | メジャーバージョンを2.0.0に上げる | `CMakeLists.txt` など | 0.5日 |

**完了基準**：
- masterにマージし、ブランチを終了すること。
- 互換性を壊す変更の一覧を含む、`NeoGraph 2.0` のリリースノートがあること。
- 26個の同期のサンプルがすべて通り、新しい非同期のサンプルが1〜2個追加されていること。

---

## 4. 互換性を壊す変更のマトリクス

| API | 変更 | 移行コスト | 解決の方針 |
|---|---|---|---|
| `Provider::complete` | 同期版は維持し、`complete_async` を追加 | なし | 純粋仮想関数を2つ宣言し、デフォルト実装で相互に結び付ける |
| `GraphNode::execute` | 同期版は維持し、`execute_async` を追加 | カスタムノードは影響なし | 同上 |
| `GraphEngine::run` | 同期版は維持し、`run_async` を追加 | なし | 同期のファサード |
| `CheckpointStore::save` | 同期版は維持し、`save_async` を追加 | カスタムストアは影響なし | 同上 |
| `PostgresCheckpointStore` | libpqxxから、libpqの直接利用へ | 利用者から見たAPIの変更なし | 内部で置き換える |
| `MCPClient::rpc_call` | 同期版は維持し、`rpc_call_async` を追加 | なし | 同期のファサード |

**結論：移行の後も、同期APIはすべて残ります。** 2.0に上げるのは、内部の依存（libpqxxの廃止）と、C++20のコルーチンが必要になるためです。

---

## 5. リスク台帳

| リスク | 影響 | 対策 |
|---|---|---|
| libpqのパイプラインモードが、チェックポイントの書き込みパターンに合わない | 第3期の遅れ | 第3期の早い段階で、2日間のスパイクで確認する。合わない場合は、libpqの同期呼び出しと `asio::post(thread_pool)` にフォールバックする。性能向上は半分になるが、完成はできる |
| asio::sslと、Anthropic／OpenAIのエンドポイントとのALPNの問題 | 第1期の遅れ | Stage 2のベンチマークはHTTPだけを使った。第1期の早い段階で、実際のエンドポイントに対するスモークテストを行う |
| Taskflowとasioのexecutorの統合が難しい | 第3期の延長 | Taskflowは残し、並列ノードの内部だけをコルーチンにする。完全な削除は、第4期の任意の作業 |
| 同期のファサード `run_sync(coro)` のデッドロック（io_contextがシングルスレッドの環境） | 実行時のバグ | 利用者向けのファサードは、io_contextに少なくとも1つのワーカースレッドがあることを保証するガードで、常に包む |
| 同期と非同期の両方の経路をテストすると、テストの数が2倍になる | 保守コスト | パラメーター化したテストを1回定義し、両方の経路で自動的に実行する |
| サンプル26のリグレッション | リリースの遅れ | サンプルは非同期化しない（第4期で任意に行うだけ）。同期のファサードが通れば問題ない |

---

## 6. 検証ゲート（全期に共通）

各期の完了時に、次を満たす必要があります。

1. `cmake --build build -j` で、警告が0件であること。
2. `ctest -j` で、172個以上のテストがすべて通ること。
3. `benchmarks/bench_neograph` の既存の3つの指標（単一実行のµs、1スレッドのPG、64スレッドのPG）の悪化が、5%以内であること。
4. `benchmarks/bench_async_http` が、Stage 2と同等以上であること。
5. `benchmarks/bench_async_fanout` で、50Kタイマーの6倍が維持されること。
6. ASanとTSanのビルドが通ること（`build-asan` と `build-tsan` はすでにあります）。
7. コミットには、`feat(async)` の接頭辞と、Co-Authored-Byを付けること。

---

## 7. スケジュールの要約

| 週 | 期 | 主な成果物 |
|---|---|---|
| W1 | 1 | TLS、keep-alive、SSE |
| W2 | 1→2 | ベンチマークの再測定、Providerの非同期化に着手 |
| W3 | 2 | SchemaProvider、MCPの非同期化 |
| W4 | 2→3 | CheckpointStoreの非同期化、libpqの書き直しに着手 |
| W5 | 3 | libpqのパイプライン、組み込みノードの非同期化 |
| W6 | 3 | エンジンのコルーチン化、executorの切り替え |
| W7 | 3→4 | ベンチマークのリグレッションゲート |
| W8 | 4 | サンプル1〜2個、ドキュメント、CIゲート |

合計は**8週間**で、6〜10週間という範囲の中間です。リスクへの対応が必要な場合は、W5とW6で1週間延ばせます。

---

## 8. 着手前の最終確認

次のセッションの開始時に、次を確認します。

- [ ] `feat/async-api` ブランチで続けるか。それとも `feat/async-stage3` に分岐するか。→ **このブランチで続けることを推奨**。PoCからStage 3への連続性を保つため。
- [ ] 第1期のスパイクの対象ホストを決める（api.openai.comとapi.anthropic.comの、TLSの動作の違い）。
- [ ] libpqのパイプラインモードのドキュメントを、事前に確認する。
- [ ] Taskflowを削除するか維持するかの、最初の方針（デフォルトは維持。第4期に再評価）。

---

**次のアクション**：第1期の1.1に着手します。`src/async/http_client.cpp` に、asio::sslの層を追加します。
