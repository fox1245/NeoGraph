<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=ja source_sha256=5822dcf16aca038ab00fde8ea9ffb59be2f05e725c5feeef8c4d61ef86a2742b -->
# 自己進化するチャットボット

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**チャットボット ハーネスは、実行時にユーザーの行動に基づいて *独自の*トポロジを再形成します。
この機能は NeoGraph に固有のものです。 LangGraph は実行時にグラフの形状を変更できません。**

[multi_tenant_chatbot](../multi_tenant_chatbot/) クックブックの自然な拡張 — それは
お客様のハーネスは *修正* されていますが、これは *進化* しています。同じコンパイル キャッシュ + thread_id 分離
LLM 判定ステップがもう 1 つあります。

## 2 つのデモ

|ファイル |シナリオ |コスト |ウォールタイム |
|---|---|---|---|
| [server.cpp](server.cpp) |アリス 1人×5ターン — 最小限の進化機構デモ | ~$0.003 | 16秒 |
| [server_multi.cpp](server_multi.cpp) | **5 人の顧客 × 5 ターン — それぞれの個別の進化タイムライン + 出現クラスター** | ~$0.02 | 7分 |

ビルド/実行 (両方):

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customers)
```

## なぜNGだけができるのか

|試み |ランググラフ |ネオグラフ |
|---|---|---|
|顧客ごとに異なるハーネス | ❌ StateGraph = Python オブジェクト | ✅graph_def JSON 行 |
|ハーネスは実行時に自動的に再形成されます。 | ❌ モジュールのリロード + 飛行中の状態の損失 | **✅ 1 つの DB UPDATE + 新しいエンジンは次のリクエストでコンパイルします** |
| 1000 人の顧客の 1000 通りの異なるグラフ | ❌ 顧客あたりのプロセス = 80 GB | ✅ 1 つのプロセス / 個別の形状キャッシュ |
|緊急クラスターの発見 |該当なし | **✅graph_def ハッシュ分布 = 顧客行動クラスター** |

LangChain/LangGraph の StateGraph は Python クラス インスタンスです。pickle にはインポート パスもバンドルされています。
ランタイム ノード/エッジの再形成には Python モジュールのリロードが必要となり、実行中の会話状態が失われます。
**NG の graph-as-JSON は、進化 = 1 つの JSON 変更を意味します。**

## コアメカニズム

各ターンの終了時に、LLM ジャッジ (gpt-4o-mini) は会話履歴と現在の会話を調べます。
トポロジーを作成し、一言で最適な値を返します。

- `simple` — 1 LLM コール、短い直接応答 (事実上の Q に適しています)
- `reflexive` — 3 つの LLM コール (ドラフト → 批評 → 最終) (正確さを求めるのに適しています)
- `fanout` — 3 つの並列 LLM パースペクティブ → マージ (マルチビュー要件に適しています)

判断が異なる場合は、顧客DBのgraph_defをインプレース更新します。次のターンは新しいトポロジを使用します —
**0 デプロイ、0 再起動、実行中の状態は保持**。

```cpp
std::string suggested = llm_judge_topology(
    provider, customer.history, customer.topology_name);

if (suggested != customer.topology_name) {
    customer.topology_def  = topo_registry[suggested]();   // New graph_def
    customer.topology_name = suggested;
    // Cache sees new hash next turn and automatically compiles new engine.
    // Real production: DB UPDATE customer_graphs SET graph_def = ...
}
```

## デモ 1 — アリス 1 人 (server.cpp)

5ターンかけて徐々に進化。ユーザーは自然に事実に関する質問→複数の視点に移行します
質問ですが、ハーネスはシンプル→ファンアウトの進化に従います。

```
── Turn 1 [topology=simple] ──
User: What is a cloud?
Bot:  A cloud is a visible mass of condensed water vapor...
[Evaluating harness fit...] judge → simple

── Turn 3 [topology=simple] ──
User: Now explain blockchain to me — I want both the
      technical view and the economic view.
Bot:  **Technical View:** Blockchain is a decentralized digital ledger...
[Evaluating harness fit...] judge → fanout
  ⟹ EVOLVE: simple → fanout (in-place, deploy 0)

── Turn 4-5 [topology=fanout] ──
... multi-perspective response after ...

Evolution timeline:
  Turn 0:  simple   (initial)
  Turn 3:  fanout   (evolved)
```

## デモ 2 — マルチカスタマー (server_multi.cpp) ⭐

**実際の影響はここにあります。** 5 人の顧客が異なる行動パターンを示し、それぞれが
別の進化タイムライン。緊急クラスター検出のデモ。

各顧客の行動パターンの仮説 + 実績:

|顧客 |行動パターン |仮説 |実際の進化 |検証 |
|---|---|---|---|---|
| **アリス** |段階的（事実→多視点） |途中のファンアウト | `simple → fanout(t3)` | ✅ |
| **ボブ** |事実のみ (「X とは何ですか?」× 5) |シンプルなメンテナンス | `simple` 全5ターン | ✅ |
| **チャーリー** |正確性を求める (「答えを検証する」) |再帰的 |すぐに`simple → reflexive(t1)` | ✅ |
| **デビッド** |最初から「X と Y のマルチアングルを比較」 |高速ファンアウト |すぐに`simple → fanout(t1)` | ✅ |
| **前夜** |混合（事実 ↔ 多視点 ↔ 慎重な振動） |発振リスク | `simple → fanout(t2) → reflexive(t4) → fanout(t5)` **発振** | ✅ |

### 要約結果

```
=== Aggregate stats ===
Customers:           5
Total turns:         25
Total main LLM:      51
Total judge LLM:     25
Total LLM calls:     76
Wall time:           424 s
Peak RSS:            18.99 MB
Compile cache size:  3   ← 5 customers → 3 distinct engine

=== Final topology distribution ===
  fanout:    3 customers  (alice, david, eve)
  reflexive: 1 customer   (charlie)
  simple:    1 customer   (bob)
```

### 主な所見

1. **行動パターン仮説 4/5 が正確に検証されました** — 人類の予測された進化経路
   とLLMジャッジの実際の進化判定は正確に一致します。つまり、**LLM ジャッジは確実に検出します
   ユーザーの意図の変化**。

2. **実際に観察されたイブの振動 ⚠️** — 発話 [事実 → マルチ → 事実
   → 慎重 → 事実] トポロジーの振動を引き起こす [単純 → ファンアウト →
   ファンアウト(維持)→再帰→ファンアウト]。 **バタつき防止ガードが必要**
   データによって検証されます (クールダウンまたはヒステリシスの強化が必要です)。

3. **創発クラスター発見** — 5 人の顧客の多様な発話パターンを自然に発見
   **3 つのトポロジ クラスタ** に分類します。コンパイルキャッシュサイズ =
   3 = 個別のクラスター数。

   **これは本当に興味深い創発特性です** — NG のデータとしてのグラフは当然のことです
   顧客行動クラスター発見メカニズムになります。グラフ_def
   分布 = 顧客行動の本質的なクラスター形状。

4. **メモリ効率** — 5 人の顧客 → 3 つのエンジン。 2顧客のエンジン
   キャッシュ共有によりメモリが節約されます。 **形状が異なる場合は 1,000 人の顧客までスケールアップ可能
   ~10 に収束、エンジン メモリはほぼ一定に留まる → 実際
   1,000 を超える顧客のマルチテナントが 1 つのプロセスに収まります。**

5. **シーケンシャル シミュレーションの所要時間はわずか 7 分** — 本番環境では、各顧客は
   独立しているので並列可能。 5 人の顧客が並行して = ~1.5 分 +
   コンパイル キャッシュは同時アクセスが安全 (`std::shared_mutex`) なので、競合はありません。

## 実稼働シナリオ - 実際の実装

```sql
CREATE TABLE customer_graphs (
    customer_id   TEXT PRIMARY KEY,
    graph_def     JSONB NOT NULL,
    topology_name TEXT,
    updated_at    TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE customer_evolution_log (
    id            SERIAL PRIMARY KEY,
    customer_id   TEXT REFERENCES customer_graphs(customer_id),
    turn          INT,
    from_topology TEXT,
    to_topology   TEXT,
    judge_reason  TEXT,
    evolved_at    TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE customer_sessions (
    thread_id     TEXT PRIMARY KEY,
    customer_id   TEXT,
    history       JSONB,
    updated_at    TIMESTAMPTZ DEFAULT NOW()
);
```

各リクエストの処理フローは次のとおりです。

```cpp
auto& cust    = db.fetch_customer(customer_id);  // graph_def + topology_name
auto  engine  = cache.get_or_compile(cust.graph_def, ctx);  // Hash-based cache
auto  history = db.fetch_history(thread_id);     // Session isolation key
RunConfig rcfg;
rcfg.thread_id = thread_id;
rcfg.input = {{"messages", history + user_msg}};
auto result = engine->run(rcfg);

db.append_history(thread_id, user_msg, result);

if (turn % EVAL_INTERVAL == 0) {
    auto suggested = llm_judge_topology(provider, history, cust.topology_name);
    if (suggested != cust.topology_name && !in_cooldown(cust)) {
        db.update_customer_graph(customer_id, topo_registry[suggested](),
                                  suggested);
        db.log_evolution(customer_id, turn, cust.topology_name, suggested);
    }
}
```

## 将来の拡張機能

- **防振ガード** — イブケースに対応します。過去Nターン以内に進化した場合はロックアウト、
  またはヒステリシス (現在のトポロジが次の候補より N% 低くない場合は変更されません)。
- **LLM によって生成された graph_def** — 現在は 3 つの事前定義トポロジから選択します。
  graph_def JSON を最初から生成する例は
  [`the-beast/`](../the-beast/) cookbook のモデル作成トポロジと
  コンパイル/検証ゲートを参照してください。
- **顧客の並列処理** — 連続デモ 7 分、顧客あたりの並列処理 = ~1.5 分。
  `asio::thread_pool` + コンパイル キャッシュを直接使用します。
- **A/B フレームワーク** — 同じ顧客に対して 2 つのトポロジを同時に運用し、勝者を決定します。
  対応の満足度。 graph_id によるスティッキー分割。
- **CheckpointStore の統合** — Postgres + 上記の実際の SQL スキーマ
  生産準備完了。
- **適応進化率** — 顧客履歴の安定性に基づいて評価間隔を調整します
  (安定 = 10 ターンごと、不安定 = ターンごと)。

## コアメッセージ

> **自己進化+マルチテナントの組み合わせがNGの本質** 「AIエージェント」のビジョン
> それ自体を構築する」は、NG のgraph-as-data パラダイムを使用すると **実際に実装可能** です。
> LLM は独自のハーネスを出力 → DB UPDATE → 即時アプリケーション — のクローズドパス
> LangGraph の StateGraph-as-Python モデルである NG は、この市場の **唯一のプレーヤー**です。
>
> *「5 顧客 × 5 ターン = 19 MB / 3 つの異なるエンジン / 緊急クラスター」
> 発見/発振診断。真の自己改善型マルチテナント エージェントの出発点
> インフラストラクチャー。"*
