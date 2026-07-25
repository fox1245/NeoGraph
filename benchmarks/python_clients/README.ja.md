<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=ja source_sha256=6d8012ba0393efa7a76c1905fcab2a826c43a0cd2d8287940fcb28ec62dc25b4 -->
# Python クライアントのオーバーヘッド — NeoGraph バインディングと標準 SDK の比較

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`neograph_engine` を介した同じワークロード (pybind11 を介した C++ エンジン)
公式の Python SDK を通じて。 C++ エンジン ベンチは次のことを示します
エンジン オーバーヘッドでは 100× ～ 600× が有利です ([`benchmarks/`](../README.md) を参照)。
このフォルダーは、ユーザーが座っているときというより狭い質問に答えます。
Python さん、その勝利のうちバインディング境界を越えたものはありますか?

方法論: インプロセス Python モック サーバーは缶詰を返します。
応答は 1 ミリ秒未満であるため、サーバー側の時間は一定です。の
デルタは純粋にクライアント側であり、JSON ビルド、HTTP、解析を行います。

## シーケンシャルオーバーヘッド (K=1)

`bench_a2a_clients.py`:

|クライアント |中央値 | p95 |スループット |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | **1,137 マイクロ秒** | 1,381μs | **860 要求/秒** |
| `a2a-sdk` 1.0.2 | 2,196μs | 2,746μs | 444 リクエスト/秒 |

→ NeoGraph **1.93×** 高速化。

`bench_openai_clients.py`:

|クライアント |中央値 | p95 |スループット |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` | **1,252 μs** | 1,423μs | **789 要求/秒** |
| `openai` 2.33 | 1,927μs | 2,393μs | 509 リクエスト/秒 |

→ NeoGraph **1.54×** 高速化。

したがって、OpenAI-call-inside-A2A ラウンドトリップを組み合わせた場合、
NeoGraph バインディングを使用すると、
SDK スタック — 各レイヤーが独立しているため、メリットはさらに大きくなります。

## 同時スループット

`bench_concurrent.py`、K = 1/4/16/64 の飛行中リクエスト、合計 500:

|   K | NeoGraph 要求/秒 | a2a-sdk 要求/秒 |スピードアップ |
|----:|---------------:|--------------:|--------:|
|   1 |            881 |           448 |  1.97× |
|   4 |        **1,461** |           446 |  **3.28×** |
|  16 |            403 |           390 |  1.03× |
|  64 |            343 |           275 |  1.25倍 |

K=4 行は最もクリーンな読み取りです: NeoGraph の pybind11 ラッパー リリース
C++ HTTP 交換中に GIL が使用されるため、`ThreadPoolExecutor` はスケーリングされます。
ほぼ直線的に。 `a2a-sdk` は非同期ネイティブであり、1 つのイベント ループを使用します。
実行中のリクエストをさらに追加しても、リクエストごとの処理には役立ちません
Python 内で支払うシリアル化コスト。

(模擬サーバーの `http.server.ThreadingHTTPServer` のため、K=16+ は両方ともドロップします)
同時スレッド数が約 400 r/s で飽和します。これは Python stdlib です。
制限であり、クライアントのプロパティではありません。 NeoGraph A2A C++ サーバー
エンジンごとに 200 の同時実行を汗をかくことなく処理します
ただし、上記のクライアント側の数値は独立しています。)

## これが何を意味するか

- **サブ μs エンジンの勝利はバインディング境界を越えることはできません**。
  **2 ～ 3 倍のクライアントの勝利が得られます。** リクエストごとのオーバーヘッドは GIL リリースです。
  HTTP プラミングと JSON 解析 - すべてネイティブ クライアント
  httpx + pydantic よりも高速に動作します。
- 実際の LLM ワークロード (呼び出しごとに 300 ミリ秒以上) の場合、クライアントのオーバーヘッドは次のようになります。
  騒音の中でも、ただし高速エンドポイントまでは高い RPS で (模擬テスト、
  内部サービス、エージェントのファンアウト、マルチショット ルーティング)
  ウォールタイムを支配します。
- **同時スレッドのスケール。** `send_message` での GIL リリース /
  `complete()` を使用すると、実際の ThreadPoolExecutor を実行できます。
  Python の並列性の壁にぶつかっています。クライアントが 1 人の場合に便利
  N 個のエージェントにファンアウトします。

## 再現する

```bash
pip install neograph-engine==0.2.2 a2a-sdk openai httpx
python bench_a2a_clients.py 500       # sequential A2A
python bench_openai_clients.py 500    # sequential OpenAI
python bench_concurrent.py 500        # concurrent A2A
```

上記の数値は、2026 年 4 月 29 日に x86_64 Ubuntu 24.04 で測定されたものです。
(WSL2)、Python 3.12.3、この中のインプロセスモックサーバーに対して
フォルダ。結果は±5%以内で再現可能です。
