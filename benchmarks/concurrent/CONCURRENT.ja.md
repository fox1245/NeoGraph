<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=ja source_sha256=dd515b96276b7154af604d545ab94284b6602c7614237e998e02e4ba82ff3b9f -->
# 同時負荷ベンチマーク：NeoGraphとPythonの過去の結果

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

このページには、2026年4月に行ったカウンターチェーンの比較を、そのまま残しています。現在のSchemaProvider SDKやPythonバインディングでは、再実行していません。「NeoGraph 3.0」は当時の記録上のラベルで、現在のパッケージのバージョンではありません。

## ワークロードと計測区間

3つのノードが、上書きのカウンターチャネルを順に増やします（`a → b → c`）。モデル呼び出し、スリープ、ネットワークI/O、チェックポイントストアは使いません。DockerのCPU 1個／512 MBとCPU 2個／1 GBのプロファイルで、N ∈ {10, 100, 1000, 10000}件を投入します。スワップの上限は、メモリの上限と同じ値にしています。

NeoGraphは、呼び出し側に置いた `asio::thread_pool` を使い、そのサイズは `max(hardware_concurrency(), 1)` です。リクエストごとの計測はワーカーの中で始まるので、P50とP99には呼び出し側のキューでの待ち時間が含まれません。`total_wall_ms` には、投入から、バースト全体の処理が終わるまでの時間が含まれます。マイクロ秒単位のP99を、サーバーのエンドツーエンドのSLOとそのまま比べないでください。

当時のPython側は、LangGraph 1.1.9、Haystack 2.27.0、pydantic-graph 1.84.1、LlamaIndex Workflow 0.14.20、AutoGen GraphFlow 0.7.5を、asyncioとmultiprocessingの両方のモードで測定しました。これらは当時のコホートのバージョンです。現在のDockerで依存を解決した結果ではありません。

## 過去の結果：CPU 1個／512 MB

グラフと表には、NeoGraphについて2026-04-22に報告した結果と、Pythonについて2026-04-19に報告した結果を、そのまま残しています。N=10,000の表はエンジン単体の結果で、プロバイダーのトランスポートや推論は含みません。取得できなかった指標は、空欄のままです。

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

| N | エンジン＋モード | 実時間 | P50 | P99 | ピークRSS | 成功 / エラー |
|---|---------------|------|-----|-----|----------|---------|
| 10,000 | NeoGraph 3.0（当時のラベル） | 52 ms | 4 µs | 7 µs | 5.5 MB | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 20.2 s | 23.0 s | 416.2 MB | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 737 µs | 88.4 ms | 60.3 MB | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 1.7 s | 2.9 s | 130.7 MB | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 167 µs | 84.7 ms | 68.1 MB | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 71 µs | 158 µs | 42.6 MB | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 253 µs | 83.8 ms | 36.7 MB | 10000 / 0 |
| 10,000 | LlamaIndex asyncio | OOMで強制終了 | — | — | — | — |
| 10,000 | LlamaIndex mp-pool-7 | 6.6 s | — | — | 102.5 MB | 0 / 10000 |
| 10,000 | AutoGen asyncio | OOMで強制終了 | — | — | — | — |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 4.6 ms | 97.1 ms | 49.1 MB | 10000 / 0 |

全マトリクスの結果は、[`results.jsonl`](results.jsonl)に保存してあります。LlamaIndexとAutoGenのasyncioのセルは、OOMによる強制終了と判定されました。この行のLlamaIndexのmultiprocessingは、呼び出しがすべて失敗しています。これらの観測から、フレームワーク一般の限界や、すべての失敗の原因が分かるわけではありません。

## 結果の解釈の範囲

GILが有効なCPythonは、Pythonのバイトコードを直列に実行します。ただし、スループットには、イベントループのスケジューリング、フレームワークの処理、プロセス間のシリアライズ、ワーカー数も影響します。このベンチマークでは、原因を1つに切り分けてはいません。asyncio全般に共通する上限も示しておらず、free-threaded Pythonの性能も予測できません。

DockerのCPUクォータを設定しても、`hardware_concurrency()` から見えるコア数は変わるとは限りません。呼び出し側のプールのサイズと、ホストの構成を記録してください。ピークRSSは、Linuxの `/proc/self/status` から取得しています。未対応のプラットフォームでの0は、測定できなかったことを意味します。multiprocessingのメモリ使用量は、各ランナーが集計している範囲を確認したうえで読んでください。この表から、256 MBでの実験結果、ベアメタルでの性能、永続化を含めた比較、リモートLLMを使う場合の処理能力を導くことはできません。

## 現在の依存関係と再現の状況

Coreは、`NEOGRAPH_BUILD_LLM=OFF` と `NEOGRAPH_USE_LIBCURL=OFF` にしても、外部の `SchemaProvider::runtime` をリンクします。インストール済みのSDKは `CMAKE_PREFIX_PATH` で、明示的なチェックアウトは `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` で指定してください。詳しくは[ビルドガイド](../../README.md)を参照してください。SDKをソースからビルドするには、C++20、設定を生成するためのPython、libcurl 7.88以上、OpenSSL Cryptoが必要です。SDKはLinux/POSIXでのみテストしており、過去のDockerの結果は他のプラットフォームには当てはまりません。
NeoGraph `0.14.0` のレシピには、alpha SDK `0.3.1`（インターフェースのリビジョンと共有世代は6）と、それに合ったヘッダーおよびライブラリーが必要です。

メンテナンスしているNeoGraphのDockerイメージは、curlとOpenSSLの開発用パッケージをインストールし、`neograph::core` にリンクする専用のCMakeコンシューマーをビルドします。SDKへの依存関係は、そこから引き継がれます。SDKの取得はルートのCMakeのポリシーに従います。パッケージもソースのチェックアウトも指定しない場合は、ネットワークへのアクセスが必要になることがあります。NeoGraphのオプションのネットワークモジュールはOFFですが、SDKの依存は残ります。マトリクスは、ビルドする前に出力先を空にします。そのため、新しいパスを指定してください。`status=ok` が示すのは、JSONを取り出せたことだけです。`ok`、`err`、終了ステータスも確認してください。

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the archived default results.jsonl, not the new output.
node benchmarks/render_concurrent.js
```

次のJSONは、フィールドの形を示す例です。追加で測定した結果ではありません。

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
