<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=ja source_sha256=6919a37bc310f6105ba5ef408675a5d982489430bc04ac6c565fdb7c66a4e6a1 -->
# Pythonクライアントのオーバーヘッド: 現在のランナーと過去の結果

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このディレクトリはローカルのプロセス内プロトコルサーバーでNeoGraph PythonバインディングとPython SDKを比較します。時間はクライアント処理、サーバーのスケジューリング、HTTP交換を含み、サーバーコストが一定とは証明していません。実モデルは動きません。表はx86_64 Ubuntu 24.04（WSL2）、Python 3.12.3で2026-04-29に測った過去の記録で、新SDK cutoverの測定ではありません。

## 過去の逐次オーバーヘッド: K=1

`bench_a2a_clients.py`のローカル固定A2A応答の記録です。中央値の比率は1.93×でした。

| Client (2026-04-29) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | 1,137 µs | 1,381 µs | 860 req/s |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

`bench_openai_clients.py`のローカル固定Chat応答の記録です。中央値の比率は1.54×でした。古いプロバイダー名は当時の実装を示すためで、現在のimportではありません。

| Client (2026-04-29, legacy provider) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` (removed) | 1,252 µs | 1,423 µs | 789 req/s |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

## 過去の同時スループット

`bench_concurrent.py`のローカルA2Aサーバー、K ∈ {1, 4, 16, 64}、各行500リクエストの記録です。

| K (2026-04-29, A2A) | NeoGraph req/s | a2a-sdk req/s | Ratio |
|----:|---------------:|--------------:|--------:|
| 1 | 881 | 448 | 1.97× |
| 4 | 1,461 | 446 | 3.28× |
| 16 | 403 | 390 | 1.03× |
| 64 | 343 | 275 | 1.25× |

K=16/64の低下は`ThreadingHTTPServer`とクライアントの相互作用を含みます。標準ライブラリの上限や一般的なasyncio制限を分離していません。K=4も線形スケーリングの証明ではなく、一つの設定の記録です。

## 現在のAPIと依存関係

現在のOpenAI比較は承認済みHTTP Chat descriptorと実行時オプションから`SchemaProvider`を作り、`make_provider_request`と`invoke`を使います。本物のtyped所有outcomeを受け、失敗/完了と表示テキストを確認します。`OpenAIProvider`、`CompletionParams`、`complete()`は削除されたAPIです。モデルはリクエストで明示し、controlsは指定しなければtyped factoryの既定値を使います。古いプロバイダー生成子の既定値ではありません。

両クライアントは同じ非公開HTTP loopbackサーバーとChat経路を使い、固定応答のテキストは`ok`です。サーバーは経路、モデル、プロンプトを確認します。リモート認証、独自CA、有料呼び出しは不要です。HTTP/1.0のローカル処理で、TLS/HTTP2検証やnative replayベンチマークではありません。固定token usageはプロバイダー報告fixtureで、実測トークンや予算課金量ではありません。

[Pythonバインディング案内](../../docs/python-binding.md)で現在のソースに合うwheelを入れ、以下の比較SDKを入れてください。Coreのソースにも外部`SchemaProvider::runtime`が必要です。[ビルド案内](../../README.md)に従ってください。Pythonラッパーだけでは古いwheelに新native APIを追加できません。新wheelの検証や新ベンチマーク合格を主張しません。
NeoGraph `0.13.0` の wheel と native consumer は alpha SDK `0.1.0`、interface revision/shared generation 4 と一致する必要があります。現在の統合検証は未完了です。

## 新しい測定の実行

リポジトリルートから実行します。wheel/ソース/SDKリビジョン、PythonビルドとGILモード、比較パッケージ版、環境、サーバープロトコル、ウォームアップ、反復数、失敗を記録してください。既定は500リクエストで事前にウォームアップします。同時ランナーはK=1/4/16/64を走査します。

```bash
# First install a wheel matching this source via docs/python-binding.md.
python -m pip install a2a-sdk openai httpx
python benchmarks/python_clients/bench_a2a_clients.py 500
python benchmarks/python_clients/bench_openai_clients.py 500
python benchmarks/python_clients/bench_concurrent.py 500
```

過去の表と新出力を分けてください。各層の比率を掛けてOpenAI-inside-A2Aの総高速化は算出できません。その組み合わせは測定していません。現在実装の一般的な2–3×保証や±5%再現性を主張しません。
