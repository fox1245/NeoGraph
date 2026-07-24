<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=ja source_sha256=8baffd5ea72da3575627014a32aaaf9257b389214daebaf2c5336633f74ff996 -->
# マルチテナントチャットボットサーバー

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**1 つのプロセスで、N 個の異なるエージェント トポロジを使用して N 人の顧客に同時にサービスを提供します。**
測定値: 1000 の同時実 OpenAI 呼び出し / 6 顧客 / 3 トポロジ /
**ピーク 29 MB / エラー 0**。

> 「100 人の顧客がそれぞれ異なるチャットボットを使用するチャットボット SaaS をどのように実行しますか?
> エージェント ハーネス — ReAct、Plan&Execute、ファンアウト、再帰的…?」
>
> LangGraph の答え: 顧客ごとに 1 つのプロセスを開始します。 100 人の顧客 = 100 のプロセス =
> ~8 GB + スーパーバイザード/k8s。
>
> NeoGraph の答え: **顧客ごとに 1 つのgraph_def JSON 行を DB に配置します。
> コンパイル キャッシュ エントリが 1 つあれば完了です。** プロセスごとに 30 MB 未満に収まります。

このクックブックは、その構造の実用的な最小限の実装です。

## シナリオ

6 人の顧客が 3 つの異なるトポロジを使用しています。

|顧客 |トポロジ |形状 | LLM コール/リクエスト |
|---|---|---|---|
|アリス、ボブ | **シンプル** | `start → respond → end` | 1 |
|デビッド・チャーリー | **反射的** | `start → draft → critique → final → end` | 3 |
|イブ、フランク | **ファンアウト** | `start → [perspective_a, _b, _c] → merge → end` | 3（パラレル） |

各顧客のgraph_defはインラインJSONで定義されますが、実際の本番環境では
Postgres `customer_graphs.graph_def JSONB` 行として直接保存します。

コア コード フロー ([server.cpp](server.cpp:140-176)):

```cpp
class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<size_t, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        size_t key = std::hash<std::string>{}(def.dump());
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        std::unique_lock lk(mu_);
        cache_.emplace(key, engine);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
};

// On request arrival
auto def    = db.fetch_graph(customer_id);   // One JSONB row
auto engine = cache.get_or_compile(def, ctx);
RunConfig cfg;
cfg.thread_id = customer_id + "__" + session_id;   // Session isolation key
cfg.input     = user_message;
auto result   = engine->run(cfg);
```

同じトポロジを共有する顧客は、エンジン インスタンスを共有します。顧客グラフ
変更によりハッシュが変更され、新しいエンジンのコンパイル + キャッシュがトリガーされます。

## ビルド/実行

### モックプロバイダーバージョン (外部依存関係なし)

```bash
cmake --build build --target cookbook_multi_tenant_mock
./build/cookbook_multi_tenant_mock
```

OpenAI キーなしで動作します。 NG エンジンの容量を測定 (同時リクエスト 1000 /
コンパイル キャッシュ ヒット率 / メモリ)。

### ライブ LLM バージョン (実際の OpenAI gpt-4o-mini)

```bash
# .env must contain OPENAI_API_KEY at repo root
cmake --build build --target cookbook_multi_tenant_live
./build/cookbook_multi_tenant_live
```

**コスト ≈ $0.06 / 1000 リクエスト** (2330 LLM コール × gpt-4o-mini レート)。

## 測定

|側面 | 1000 要求をモック |ライブ 100 リクエスト | **ライブ 1000 リクエスト** |
|---|---|---|---|
| OK / エラー | 1000 / 0 | 100 / 0 | **1000 / 0** ⭐ |
|ウォールタイム | 5ミリ秒 | 11.5秒 | 50.2秒 |
|平均レイテンシー | 39μs | 1.58秒 | 1.4秒 |
|最大遅延 | 2.99ミリ秒 | 9.33秒 | 14.4秒 |
|スループット | 200K RPS | 8.67 RPS | **19.9 RPS** |
| **ピーク RSS** | **5.25 MB** | **21.9 MB** | **29.25 MB** |
|コンパイルキャッシュヒット率 | 99.7% | 94% | **99.4%** |
|特徴的なエンジン | 3 | 6 | 6 |

**測定環境**: WSL2 / 32スレッドasioスレッドプール / シングルホスト / リアル
OpenAI API 呼び出し。

キー番号:

- **1000 の同時実行中の LLM コルーチン + 接続メモリのコスト ≈
  29MB**。 100 リクエスト → 1000 リクエストの増加 +7 MB ⇒ 追加接続ごとに最大 8 KB。
  asio コルーチン + httplib SSL 接続プールの組み合わせ。
- **同時 1000 でエラーは 0 件** — NG はレート制限 / ネットワーク ジッター / TLS を適切に吸収します
  再試行なしのハンドシェイクジッター。プロバイダ側のスロットルは次のように強化できます。
  `RateLimitedProvider` ラッパー。
- **キャッシュ ヒット率 99.4%** — 顧客が増えてもヒット率は維持されます。
  トポロジ数は変わりません。 **1000 の顧客シナリオ メモリも最大 30 MB のままです**。

## LangGraph の比較 — 本当の意味

LangGraph で同じマルチテナント シナリオを試みると、次のボトルネックが発生します。

|側面 |ネオグラフ | LangGraph の見積もり |
|---|---|---|
| 1 つのプロセスで N 顧客 × N トポロジー | **はい** (29 MB / 1000 要求) |いいえ — StateGraph は Python オブジェクトであり、シリアル化/ストレージが扱いにくい (pickle バンドルのインポート パス) |
|顧客固有のトポロジ変更 | 1 つの DB 行 UPDATE |コード PR → CI → デプロイ サイクル |
|バージョン分離 (顧客 A の v1/v2 グラフが共存) | `graph_versions` 行を追加 | Python 名前空間の衝突、ハックが必要 |
|マルチプロセスの強制 |不要 |顧客 = プロセスの共通パターン |
|メモリ (6 人の顧客) | 29MB | 6 × ~80 MB = 480 MB (LG アイドル ベースライン) |
|メモリ (1000 顧客) | ~30 MB (キャッシュは変更されません) | **~80 GB** (顧客ごとのプロセス) |
|運用インフラ | 1 つのバイナリ | gunicorn / 監視 / k8s + プロセス オーケストレーション |

**プロセスあたり 30 MB と 80 GB。** 2700 倍の違いが、実際のマルチテナント チャットボットの本質です
SaaS運用。

## 実践的なシナリオ – どこまで実現できるか

`t2.micro` で可能なシナリオ (1 vCPU / 1 GB RAM、~$0.01/時間):

|シナリオ | NG メモリの見積もり | t2.microでは可能でしょうか？ |
|---|---|---|
| 100 人の同時アクティブ飛行中 LLM + 100 人の顧客 × 3 トポロジ | ～10MB | ✅ 残りはたっぷり ~990 MB |
|同時飛行中 1000 + 顧客 1000 × 10 トポロジ | ~30MB | ✅ 残りはたっぷり ~970 MB |
|同時飛行中 10,000 + 顧客 10,000 × 100 トポロジー | ~85MB | ✅ 残りはたっぷり ~915 MB |
|同時飛行数 100,000 + … | ~800MB | ⚠️ RAM がほぼ使い果たされています |

* 仮定: 接続あたり最大 8 KB + コンパイル キャッシュ エントリあたり最大 10 KB + ベース 5 MB。

もちろん、t2.micro 1 vCPU と OpenAI 層の RPM 制限は *スループット* 上限です。
**重要な点は、顧客の限界費用が ~0 であるということです**。

> t2.micro 上の LangGraph 100 人の顧客に対して 1 GB = 100 プロセス =
> 8 GB 必要 → インスタンス自体が起動できません。 **m5.2xlarge (32 GB、~$0.38/時間) が必要です。**
>
> NG = **単一の t2.micro ($0.01/時間) の同じタスク。 38× インフラストラクチャ
> コストの違い**

## ホットスワップのデモンストレーション

`server.cpp` の終わりは、アリスのトポロジの `simple` → `fanout` へのインプレース変更を示しています。
そしてすぐに次のリクエストを処理します。 0 デプロイ サイクル、0 再起動。実際の生産
顧客は Web UI でグラフ JSON を編集→ DB 保存→次のリクエストで新しいトポロジを使用します。

## 将来の機能強化

- **CheckpointStore の統合** — 現在、リクエストごとに履歴を入力として渡します。
  Postgres CheckpointStore を使用すると、thread_id ごとに自動永続化が行われます。
- **顧客ごとのプロバイダー** — alice=gpt-4o-mini、bob=claude-haiku style
  顧客ごとに異なるモデル/プロバイダー。 NodeContext::provider は顧客ごとに変更されます。
- **ストリーミング応答** — トークンレベルの `input.stream_cb` + SSE を使用した `run(input)`
  ストリーミング。 NG の `run(NodeInput)` パスをストリーム コールバックで直接使用します。
- **A/B 実験フレームワーク** —graph_def ハッシュ + customer_id スティッキー分割によるトラフィック分割。
  コードパターンを直接拡張します。
- **ストリーミング + 統合のキャンセル** — クライアントの切断時に送信 LLM ソケットを中止します。
  NGの`RunConfig::cancel_token`を直接配線してください。

## コアメッセージ

> *「6000 顧客 × 3 トポロジ = 29 MB。1 つの JSON 行編集 = デプロイなしのホットスワップ。
> 実際の OpenAI 同時呼び出し 1000 回でエラーは 0 件。 1台のt2.microで動作可能*

NeoGraph のこの 1 つの行は、パフォーマンスの数値よりも影響力のあるセールス ポイントである可能性があります。
(`5.5 MB L3 fit / 1024 worker idle 31 MB`)。
