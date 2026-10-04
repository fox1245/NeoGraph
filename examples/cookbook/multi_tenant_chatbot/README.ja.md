<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=ja source_sha256=5b5b58f71f1129b96f7164783daefd9467d64ba6bc19df7d96ef3a99bfaf8fe6 -->
# マルチテナントチャットボットサーバー

## 現在のソース境界と過去の測定

mock は provider-free です。cache は topology、model、instruction、extra configuration、provider name を結びますが live cache の tenant/provider capability key は使いません。mock の再利用結果は production cross-tenant 分離の証明ではありません。

C++ live は型付き SDK request と完全な不変 Outcome を使用します。engine cache identity は
 tenant/topology と捕捉した provider/model/host instruction に結び付きます。信頼 host の tenant 選択、
quota/store 境界、thread 分離は維持します。portable JSON 要約は native authority ではありません。
以下の数値/実行記録は過去の資料で、新移行の実行または live pass ではありません。
live binary は **1,000 request / 32 worker** 固定で、低費用 small-smoke flag はありません。
単発検証として実行せず、live 鍵/network と別途承認された大きな provider 費用を必要とします。
価格や zero-error を保証しません。鍵/prompt/artifact は非公開で raw native payload を公開しません。
provider retry は明示的な一 layer、既定 off。旧 throttle-provider wrapper は現在の public API にありません。

`server_live_llm.cpp` は型付き provider factory に経路別180秒 timeout を渡します。
全体の既定値は変えず、timeout は不確実/観測済み呼出しの再送権限ではありません。

保存済み interface-3 model-free E2E では専用 mock workload1,000 graph要求・error0・compiled topology3・cache hit997を
観察し、Alice topology置換は既存 fanout engineを再使用しました。これは topology load/cache証拠で、
production認証・quota・semantic response・memory capacity保証ではありません。isolated-host CLIは
異なるpolicy tuple二つを出力しましたが graphを実行しませんでした。


**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**1つのプロセスが、N人の顧客に対してN種類の異なるエージェントトポロジーを同時に提供する。** 測定値：実OpenAI呼び出し1000並行／顧客6／トポロジー3／**ピーク29 MB／エラー0**。

mock workload は捕捉した設定が同等の場合だけ compiled engine を共有します。認証、tenant 選択、store と quota は host の責任です。以下の過去の RSS はそれらの上限ではありません。

このクックブックは、その構造の動作する最小実装である。

## 分離契約 (production 境界)

上の測定は topology 共有の測定で、production SaaS の security/capacity 保証ではありません。認証済み ingress が不変 `TenantScope` を作り、tenant 別 provider/model policy、`GraphRegistry` snapshot、`ToolSet`、`ScopedStore`、`ScopedCheckpointStore`、Harness namespace、`TenantQuota` を提供します。

```cpp
auto backend_store = std::make_shared<InMemoryStore>(); // or a tenant DB
auto backend_checkpoints = std::make_shared<InMemoryCheckpointStore>();
TenantScope scope("tenant-a", "authz-a"); // trusted ingress only
ScopedStore store(scope, backend_store);
ScopedCheckpointStore checkpoints(scope, backend_checkpoints);
TenantQuota quota({.max_concurrency = 32, .max_queue = 64,
                   .max_model_tokens = 2'000'000, .max_artifacts = 1000});
```

公開 thread/checkpoint/run/artifact ID は各 tenant で再利用できます。scoped adapter は private backend namespace に写し、違う tenant の lookup は absence を返します。resume、replay、fork、cancellation、dereference は同じ ingress scope を使います。

`CatalogConfig::materialization_context_identity` は provider/model policy、tool catalog、store、registry snapshot を識別する non-secret host identity で generation cache identity の一部です。credential、authorization token、topology JSON を identity/cache key/journal/log/diagnostic に入れないでください。exact capability receipt が意図的に同等の場合のみ省略します。

mock は topology 再利用を示し、tenant/provider 分離、quota、production memory を証明しません。production host は tenant 別 scoped engine/resource binding を保持し、自分で測定してください。

### Offline isolated-host reference

```bash
cmake --build build --target cookbook_multi_tenant_isolated_host
./build/cookbook_multi_tenant_isolated_host
```

この CLI は異なる topology、provider/model、tool、store、quota policy tuple 二つを出力します。graph は実行しません。synthetic identity を使い network credential は不要です。production ingress/control-plane は deployment host が実装します。

## シナリオ

6人の顧客が3つの異なるトポロジーを使用する：

| 顧客 | トポロジー | 形状 | LLM呼び出し／リクエスト |
|---|---|---|---|
| alice、bob | **シンプル** | `start → respond → end` | 1 |
| charlie、david | **再帰的** | `start → draft → critique → final → end` | 3 |
| eve、frank | fanout | `start → [perspective_a, _b, _c] → merge → end` | 3（並列） |

各顧客のgraph_defはインラインJSONとして定義されているが、実際の本番環境ではPostgres `customer_graphs.graph_def JSONB` 行として直接保存される。

Coreのコードフロー（[server.cpp](server.cpp)）：

```cpp
class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        // This provider-free style demo still binds all captured configuration.
        const std::string key = json::array({def, ctx.model, ctx.instructions,
            ctx.extra_config, ctx.provider_name}).dump();
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        // Miss — compile (lock 밖에서, 다른 customer 차단 안 함).
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = cache_.emplace(key, engine);
            if (!inserted) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;  // race — 다른 thread 가 먼저 넣음
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
    std::size_t hits()   const { return hits_.load(); }
    std::size_t misses() const { return misses_.load(); }
    std::size_t size()   { std::shared_lock lk(mu_); return cache_.size(); }
};

// On request arrival
auto def    = db.fetch_graph(customer_id);   // One JSONB row
auto engine = cache.get_or_compile(def, ctx);
RunConfig cfg;
cfg.thread_id = customer_id + "__" + session_id;   // Session isolation key
cfg.input     = {{"messages", json::array({{{"role", "user"}, {"content", user_message}}})}};
auto result   = engine->run(cfg);
```

同じトポロジーを共有する顧客はエンジンインスタンスを共有する。顧客のグラフ変更はハッシュを変更し、新しいエンジンのコンパイル＋キャッシュをトリガーする。

## ビルド / 実行

### Mock 版 (provider 呼び出しなし)

native 構成/link には key がなくても SchemaProvider が必要です。先に `CMAKE_PREFIX_PATH` または `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` を指定してください。

```bash
cmake --build build --target cookbook_multi_tenant_mock
./build/cookbook_multi_tenant_mock
```

OpenAIキーなしで動作。NGエンジン容量を測定する（1000件の同時リクエスト／コンパイルキャッシュヒット率／メモリ）。

### ライブLLMバージョン（OpenRouter DeepSeek）

```bash
# .env must contain OPENROUTER_API_KEY at repo root
cmake --build build --target cookbook_multi_tenant_live
./build/cookbook_multi_tenant_live
```

**コストはプロバイダー依存**（固定されたDeepSeekルートを通る2330回の呼び出し）。

## 測定結果

| Aspect | Mock 1000 req | ライブ100リクエスト | **ライブ1000リクエスト** |
|---|---|---|---|
| OK / エラー | 1000 / 0 | 100 / 0 | **1000 / 0** ⭐ |
| ウォールタイム | 5 ms | 11.5 秒 | 50.2 秒 |
| 平均レイテンシ | 39 µs | 1.58 秒 | 1.4 秒 |
| 最大レイテンシ | 2.99 ms | 9.33 秒 | 14.4 秒 |
| スループット | 200K RPS | 8.67 RPS | **19.9 RPS** |
| **Peak RSS** | **5.25 MB** | **21.9 MB** | **29.25 MB** |
| コンパイルキャッシュヒット率 | 99.7% | 94% | **99.4%** |
| 異なるエンジン数 | 3 | 6 | 6 |

**測定環境**: WSL2 / 32スレッドasioスレッドプール / シングルホスト / 実OpenRouter DeepSeek API呼び出し。

主要数値:

- **1000件の同時実行中のLLMコルーチン + 接続メモリコスト ≈ 29 MB**。 100リクエスト → 1000リクエストで+7 MB増加 ⇒ 追加コネクション1件あたり約 8 KB。asioコルーチン + httplib SSLコネクションプールの組み合わせ。
- 過去の1000 request 実行のエラー0件は現在の reliability 保証ではありません。
- **Cache hit rate 99.4%** — この過去の workload の観察で、1,000 production tenant の memory capacity を保証しません。

## LangGraph比較 — 実際の意味

この実行は LangGraph を benchmark していません。LangGraph は顧客ごとに一 process を要求しません。比較には同じ graph、store、provider、isolation policy が必要です。以前の process-per-customer 推定は測定結果ではありません。

## 実践シナリオ — どこまで行けるか

六顧客の過去の load から cloud instance の容量や 10,000/100,000 connection を予測できません。実際の認証、tenant 別 resource、store、provider route と limit で production host を測定してください。SDK transport の現在の検証範囲は Linux/POSIX です。

## ホットスワップのデモンストレーション

`server.cpp` 末尾は、aliceのトポロジーが`simple` → `fanout`へとインプレースで変更され、直後のリクエストが即座に処理されることを示しています。デプロイサイクル0回、再起動0回です。実際の本番環境では、顧客がWeb UIでグラフJSONを編集 → DB保存 → 次のリクエストで新しいトポロジーが使用される、という流れになります。

## 将来の拡張

- **CheckpointStore統合** — 現在はリクエストごとに履歴を入力として渡しています。Postgres CheckpointStoreを使用すれば、thread_idごとに自動で永続化されます。
- **固定プロバイダー** — すべての顧客が同じOpenRouter DeepSeekモデルを使用します。`NodeContext::provider`は顧客固有のコンテキストを保持できます。
- **ストリーミング応答** — `run(input)`と`input.stream_cb` + SSEによるトークンレベルのストリーミング。NGの`run(NodeInput)`パスをストリームコールバックと直接使用します。
- **A/B実験フレームワーク** — graph_defハッシュ + customer_idによるスティッキー分割でトラフィックを分割。コードパターンを直接拡張します。
- **ストリーミング + キャンセル統合** — クライアント切断時に送信LLMソケットを中止。NGの`RunConfig::cancel_token`を直接配線します。

## Core メッセージ

保存された mock 証拠は 1,000 request、三 compiled topology、997 cache hit です。isolated-host CLI は policy reference で serving workload ではありません。上の live timing は過去の記録で新移行の pass ではありません。
