<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=ja source_sha256=c5618ac13a6472e43cc2a1c6dd605d9f9d85842c5b39a3d5f07d5592047cc3da -->
# 同時負荷ベンチマーク：NeoGraphとPythonの過去の結果

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

このページは、2026 年 4 月のカウンターチェーンの比較を、記録されたデータが裏付ける範囲に絞って残したものです。現在の SchemaProvider SDK や Python バインディングでは再実行しておらず、このディレクトリのハーネスはその後に修正されました（[撤回と修正](#withdrawn-and-corrected)を参照）。修正したハーネスでの再測定は未実施です。「NeoGraph 3.0」は記録されたコホートのラベルであり、現在のパッケージのバージョンではありません。

## ワークロードと計測区間

3つのノードが、上書きのカウンターチャネルを順に増やします（`a → b → c`）。モデル呼び出し、スリープ、ネットワークI/O、チェックポイントストアは使いません。DockerのCPU 1個／512 MBとCPU 2個／1 GBのプロファイルで、N ∈ {10, 100, 1000, 10000}件を投入します。スワップの上限は、メモリの上限と同じ値にしています。

NeoGraph は、呼び出し側に `max(hardware_concurrency(), 1)` の大きさの `asio::thread_pool` を置きます。ワークロードは**クローズドな単一のバースト**です。N 件を一度に投入して、すべて処理し終えるまで流し切ります。時間に沿った到着はモデル化していません。`Wall` は最初の投入から最後の完了までの時間です。従来のハーネスはこれをミリ秒の整数で記録していたため、20 ms 未満の NeoGraph の点は、どのチャートや比率にも使いません。従来のデータにあるリクエストごとの列は撤回しました。

当時のPython側は、LangGraph 1.1.9、Haystack 2.27.0、pydantic-graph 1.84.1、LlamaIndex Workflow 0.14.20、AutoGen GraphFlow 0.7.5を、asyncioとmultiprocessingの両方のモードで測定しました。これらは当時のコホートのバージョンです。現在のDockerで依存を解決した結果ではありません。

## 過去の結果：CPU 1個／512 MB

以下の表とチャートは、保管してある [`results.jsonl`](results.jsonl) から `scripts/render_bench_report.py` が生成し、壁時計時間の列と単一プロセスのメモリの列だけを残します。NeoGraph の結果は 2026-04-22 に、Python の結果は 2026-04-19 に報告されたものです。N=10,000 の表はエンジンだけを測定したもので、プロバイダーの通信や推論は含みません。

![スループット：壁時計時間あたりに完了したリクエスト数、クローズドな単一のバースト](../../docs/images/bench-concurrent-throughput.svg)

![単一プロセスのピーク常駐メモリ、クローズドな単一のバースト](../../docs/images/bench-concurrent-rss.svg)

<!-- bench-record:begin concurrent_wall -->
| N | エンジンとモード | クローズドバーストの壁時計時間 | ピーク RSS、単一プロセス (MiB) | 成功 / エラー |
| ---: | :--- | ---: | ---: | ---: |
| 10,000 | NeoGraph threadpool | 52 ms | 5.5 | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 42.6 | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 撤回: プロセスツリーを合算していない | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 撤回: プロセスツリーを合算していない | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 130.7 | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 撤回: プロセスツリーを合算していない | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 416.2 | 10000 / 0 |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 撤回: プロセスツリーを合算していない | 10000 / 0 |
| 10,000 | LlamaIndex asyncio | OOM で強制終了 | — | — |
| 10,000 | AutoGen asyncio | OOM で強制終了 | — | — |
<!-- bench-record:end concurrent_wall -->

マトリクス全体は [`results.jsonl`](results.jsonl) に保管してあります。記録された LlamaIndex と AutoGen の asyncio のセルは OOM による強制終了に分類されており、呼び出しごとのインスタンス 1 万個が同時には 512 MB に収まらなかったという意味です。これらの観察から、各フレームワークに共通の限界が確立されるわけではありません。 LangGraph の asyncio の行は、通常の `def` ノードで `ainvoke` を呼んでおり、LangGraph はそれをスレッドプールで実行します。`async def` ノードなら別のレジームです（[エンジンオーバーヘッドのガイド](../README.md)を参照）。

<a id="withdrawn-and-corrected"></a>
## 撤回と修正

このページから撤回したものと、それぞれの理由です。

* **P50 と P99 の列とレイテンシーのチャート。**NeoGraph、multiprocessing の行、pydantic-graph（実行が中断されない）のワーカー内のサービス時間を、タスクを作った時点から測った他の asyncio の行のシステム滞在時間と並べていたため、その差はフレームワークがどれだけ頻繁に制御を譲るかを測ったもので、どれだけ速いかを測ったものではありません。
* **LlamaIndex の multiprocessing の行。**ハーネスがワークフローのハンドラーを、実行中のイベントループの外で作ったため、すべての呼び出しが失敗しました（1 万件中 0 件が完了）。6.6 秒とメモリの数値は例外処理のものでした。
* **multiprocessing のメモリの数値。**ハーネスは親プロセスだけを数え、ワーカープロセスを数えていませんでした。
* **N=10 と N=100 の NeoGraph のスループットの点。**壁時計時間をミリ秒の整数に切り捨てて記録し、レンダラーがそれを 1 ms に固定していたため、これらの点は測定値ではありません。

このディレクトリのハーネスは現在、すべてのセルに、完了数、失敗数、各結果の検証を記録します。失敗のあるセルは `status` が `error` で、パーセンタイルがなく、終了ステータスが 0 以外になります。また、共通の 1 つの投入時刻から測ったサービス時間とレスポンス時間、マイクロ秒単位の壁時計時間、multiprocessing のプロセスツリーのメモリのピークも記録します。Docker のマトリクスは、これらのハーネスでは再実行していません。再測定は未実施です。 Docker イメージは現在 [`requirements-compare.txt`](../requirements-compare.txt) の最新リリースに固定してあり、以前の LangGraph の固定バージョン 1.1.7 は PyPI で yank されています。

## 結果の解釈の範囲

GILが有効なCPythonは、Pythonのバイトコードを直列に実行します。ただし、スループットには、イベントループのスケジューリング、フレームワークの処理、プロセス間のシリアライズ、ワーカー数も影響します。このベンチマークでは、原因を1つに切り分けてはいません。asyncio全般に共通する上限も示しておらず、free-threaded Pythonの性能も予測できません。

Docker の CPU クォータは、`hardware_concurrency()` に見えるコア数を変えるとは限りません。呼び出し側プールの大きさとホストのトポロジーを記録してください。ピーク RSS は単一プロセスの Linux の `/proc/self/status` から取っており、未対応のプラットフォームで 0 の場合は測定できなかったことを意味します。NeoGraph はプールのスレッドあたり多くても 1 件しか保持しませんが、Python の asyncio の行は N 件すべてを実行中のまま保持するので、メモリの列は同じ条件での比較ではありません。この表からは、256 MB での実験、ベアメタルでの推定、永続化の比較、リモート LLM の処理能力の保証は導けません。

## 現在の依存関係と再現の状況

Coreは、`NEOGRAPH_BUILD_LLM=OFF` と `NEOGRAPH_USE_LIBCURL=OFF` にしても、外部の `SchemaProvider::runtime` をリンクします。インストール済みのSDKは `CMAKE_PREFIX_PATH` で、明示的なチェックアウトは `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` で指定してください。詳しくは[ビルドガイド](../../README.md)を参照してください。SDKをソースからビルドするには、C++20、設定を生成するためのPython、libcurl 7.88以上、OpenSSL Cryptoが必要です。SDKはLinux/POSIXでのみテストしており、過去のDockerの結果は他のプラットフォームには当てはまりません。
NeoGraph `0.16.0` のレシピには、alpha SDK `0.4.0`（インターフェースのリビジョンと共有世代は7）と、それに合ったヘッダーおよびライブラリーが必要です。

メンテナンスしているNeoGraphのDockerイメージは、curlとOpenSSLの開発用パッケージをインストールし、`neograph::core` にリンクする専用のCMakeコンシューマーをビルドします。SDKへの依存関係は、そこから引き継がれます。SDKの取得はルートのCMakeのポリシーに従います。パッケージもソースのチェックアウトも指定しない場合は、ネットワークへのアクセスが必要になることがあります。NeoGraphのオプションのネットワークモジュールはOFFですが、SDKの依存は残ります。マトリクスは、ビルドする前に出力先を空にします。そのため、新しいパスを指定してください。`status=ok` が示すのは、JSONを取り出せたことだけです。`ok`、`err`、終了ステータスも確認してください。

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the tables and charts from the archived results.jsonl, not from the new output.
python3 scripts/render_bench_report.py
```

次のJSONは、フィールドの形を示す例です。追加で測定した結果ではありません。

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
