<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=ja source_sha256=14c932ce835be59435fe30b831344894d899490a1478a3bd34f442e9113414da -->
# 自己進化型チャットボット

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**チャットボットハーネスは、ユーザーの行動に基づいてランタイムに*自身の*トポロジーを再形成する。この能力はNeoGraphに固有であり、LangGraphはランタイムにグラフを再形成できない。**

[multi_tenant_chatbot](../multi_tenant_chatbot/)クックブックの自然な拡張 — あちらはカスタマーハーネスが*固定*されているが、こちらは*進化*する。同じコンパイルキャッシュ+thread_id分離に、LLM判定ステップが1つ追加されている。

## 2つのデモ

| ファイル | シナリオ | コスト | ウォールタイム |
|---|---|---|---|
| [server.cpp](server.cpp) | Alice 1人×5ターン — 最小の進化メカニズムデモ | ~$0.003 | 16秒 |
| [server_multi.cpp](server_multi.cpp) | **5人のカスタマー×5ターン—各々が独立した進化タイムラインと創発的クラスター** | ~$0.02 | ビルド／実行（両方）： |

ビルド/実行 (両方):

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customers)
```

## これがNGだけに可能な理由

| 試行 | LangGraph | NeoGraph |
|---|---|---|
| カスタマーごとに異なるハーネス | ❌ StateGraph = Pythonオブジェクト | ✅ graph_def JSON 行 |
| Harness はランタイム時に自身を再形成する | ❌ モジュールリロード + 進行中の状態損失 | **✅ 1回のDB UPDATE + 次のリクエスト時の新しいエンジンコンパイル** |
| 1000人の顧客の1000の異なるグラフ | ❌ 顧客ごとのプロセス = 80 GB | ✅ 単一プロセス / 異なるシェイプキャッシュ |
| 創発的なクラスタ発見 | N/A | **✅ graph_def ハッシュ分散 = 顧客行動クラスタ** |

LangChain/LangGraph の StateGraph は Python クラスインスタンス — pickle はインポートパスも同梱するため、ランタイム時のノード/エッジ再形成には Python モジュールリロードが必要となり、会話中の状態が失われる。**NGのグラフ-as-JSON は、進化 = 1つのJSON変更を意味する。**

## Core メカニズム

各ターンの終わりに、OpenRouter経由で固定された DeepSeek モデルが LLM 判定役として機能する: 会話履歴 + 現在のトポロジーを参照し、最適なものを一言で返す:

- `simple` — LLM 呼び出し1回、短く直接的な回答(事実に基づくQ&A に適している)
- `reflexive` — LLM呼び出し3回（draft→critique→final）(精度重視の用途に適合)
- `fanout` — 3つの並行 LLM の視点 → 統合 (多視点の要件に適している)

判定が異なる場合、顧客DBのgraph_defをインプレースで更新する。次ターンは新トポロジーを使用 — **デプロイ0、再起動0、インフライト状態は保持**。

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

## デモ1 — Alice 1人 (server.cpp)

5ターンにわたる段階的進化。ユーザーは事実質問→多視点質問へ自然に移行し、ハーネスはシンプル→fan-out進化に追従する。

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

## デモ2 — マルチカスタマー (server_multi.cpp) ⭐

**真の影響はここにある。** 5人の顧客が異なる行動パターンを示し、それぞれ個別の進化タイムラインを持つ。創発的クラスタ検出のデモ。

各顧客の行動パターン仮説と実結果:

| 顧客 | 行動パターン | 仮説 | 実際の進化 | 検証 |
|---|---|---|---|---|
| **alice** | 段階的 (事実→複数視点) | 途中でfan-out | `simple → fanout(t3)` | ✅ |
| **bob** | 事実のみ（"Xとは何か？" × 5） | シンプルを維持 | `simple` 全5ターン | ✅ |
| **charlie** | 精度追求型（「回答を検証せよ」） | 再帰的 | `simple → reflexive(t1)` 直ちに | ✅ |
| **david** | 開始時「X vs Yを多角的に比較」方式 | 高速fan-out | `simple → fanout(t1)` 直ちに | ✅ |
| **eve** | 混合型（事実確認↔多視点↔注意深い揺れ動き） | オシレーションリスク | `simple → fanout(t2) → reflexive(t4) → fanout(t5)` **oscillation** | ✅ |

### サマリー結果

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

### 主要な観察

1. **行動パタン仮説4/5が正確に検証済み** — 人間の予測した進化経路とLLM判定者の実際の進化判断が完全に一致。すなわち、**LLM判定者はユーザーの意図変化を確実に検出する**。

2. **Eveの振動が実際に観測された ⚠️** — 発話 [factual → multi → factual → careful → factual] により、トポロジーが [simple → fanout → fanout(維持) → reflexive → fanout] と振動する。**アンチフラッピングガードが必要**であり、データで検証する（クールダウンまryehはヒステリシス強化が必要）。

3. **創発的なクラスター発見** — 5人の顧客の多様な発話パターンが自然に**3つのトポロジークラスター**へと分類される。コンパイルキャッシュサイズ = 3 = 個別クラスター数。

**これは本当に興味深い創発的な特性である** — NGのグラフ・アズ・データは、自然に顧客行動のクラスター発見メカニズムとなる。graph_defの分布 = 顧客行動の本質的なクラスター形状。

4. **メモリ効率性** — 5名の顧客で3基のエンジン。キャッシュ共有により2名分の顧客エンジンメモリを節約。**1000名にスケールアップしても、異なる形状が約10個に収束すれば、エンジンメモリはほぼ一定のまま → 実際の1000名以上のマルチテナントが1つのプロセスに収まる。**

5. **逐次シミュレーションはウォールタイムでわずか7分** — 本番環境では、各顧客は独立しているため並列処理が可能。5名の顧客を並列処理 = 約1.5分 + コンパイルキャッシュは並行アクセス安全 (`std::shared_mutex`) なのでレースは発生しない。

## 本番シナリオ — 実際の実装

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

各リクエストの処理フロー:

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

## 将来の拡張

- **アンチ振動ガード** — Eveケースを処理する。過去にNターン以内に進化していればロックアウト、またはヒステリシス（現在のトポロジーが次の候補よりもN%低くない場合は変更しない）。
- **LLM生成のgraph_def** — 現在は3つの定義済みトポロジーから選択している。より意欲的には、LLMがゼロからでgraph_def JSONを生成できる。この[`the-beast/`](../the-beast/)クックブックは、モデル作成のトポロジーにと加えてコンパイル/検証ゲートを実証している。
- **顧客の並列処理** — 逐次デモは7分、顧客ごとの並列処理では約1.5分。`asio::thread_pool` ＋ コンパイルキャッシュを直接使用する。
- **A/Bテストフレームワーク** — 同じ顧客に2つのトポロジーを同時に操作し、応答満足度による勝者を決定する。graph_idごとのスティッキースプリット。
- **CheckpointStore統合** — 本番対応のためのPostgres + 前述のSQLスキーマ。
- **適応的進化レート** — 顧客の履歴安定性に基づいて評価間隔を調整（安定 = 10回毎、不安定 = 毎回）。

## Core メッセージ

> **自己進化とマルチテナントの組み合わせがNGの真髄である。**「自律的に構築されるAIエージェント」というビジョンが、NGのグラフ・データ・パラダイムで**実際に実装可能**である。
> 自身を構築する」というビジョンが、NGのグラフ・アズ・データパラダイムで実際に実装可能である。
> LLMが自身のハーネスを出力 → DB UPDATE → 即時適用 — 閉じたパス
> LangGraphのStateGraph-as-Pythonモデルに対し、NGはこの市場における **唯一のプレイヤー**です。
>
> *"5顧客 × 5ターン = 19 MB / 3 distinct engine / emergent cluster
> discovery / oscillation診断の出発点。真の自己改善型マルチテナントエージェント
> infrastructure。"*
