<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=ja source_sha256=5331369d595d05bf67c0748516d7da5980b3dfa70262e2df9c01849f90564b58 -->
# 同時負荷ベンチマーク — NeoGraph と Python のグラフ フレームワーク

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

**バースト** 負荷の下では、N 個のリクエストが同時に送信され、
テストはすべてが完了するまで待機します。これらのエンジンはどのようにスケールされますか?
それぞれのアプローチはどの時点で実行できなくなるのでしょうか?

このベンチは、CPU と
6 つのフレームワークにわたって、SBC クラスのターゲットに一致するメモリ制限。

## 設定

- **ワークロード**: 3 ノードの順次カウンター チェーン (`a → b → c`)、それぞれ
  ノードは単一の状態チャネルをインクリメントします。 I/O、スリープ、LLM はありません。
- **バースト パターン**: t=0 で N 個のタスクが送信されました。ランナーはみんなを待っている
  終わらせる。リクエストごとのレイテンシはプロセス内で取得されます。
- **サンドボックス**: `--cpus` および `--memory` (+ `--memory-swap`) を備えた Docker
  一致したため、スワップの代わりに OOM が起動します)。マトリックス：
  - プロファイル **1 CPU / 512 MB** — 「タイト SBC」ターゲット (プライマリ)
  - プロファイル **2 CPU / 1 GB** — 「快適な SBC」ターゲット
- **同時実行数**: N ∈ {10, 100, 1000, 10000}
- **テスト済みエンジン**:
  - `neograph` (3.0) — `engine->run()` をディスパッチする `hardware_concurrency()` ワーカーを備えた呼び出し側 `asio::thread_pool`。エンジン自体は、デフォルトでシングルスレッド `run_sync` 上のスーパーステップ ループを駆動します。各呼び出しでは独自の io_context が使用されるため、スケジューリングはエンジンではなく呼び出し元プールによって制限されます。
  - `langgraph-asyncio` / `langgraph-mp` — `asyncio.gather` / `multiprocessing.Pool` の LangGraph 1.1.9。
  - `haystack-asyncio` / `haystack-mp` — ヘイスタック 2.27.0。 Pipeline.run() は同期です。 asyncio モードは `asyncio.to_thread` でラップします。
  - `pydantic-asyncio` / `pydantic-mp` — pydantic-graph 1.84.1、非同期ネイティブ。
  - `llamaindex-asyncio` / `llamaindex-mp` — LlamaIndex ワークフロー 0.14.20、実行ごとに 1 つの新しいワークフロー (実行ごとのイベント バス)。
  - `autogen-asyncio` / `autogen-mp` — AutoGen GraphFlow 0.7.5、実行ごとに 1 つの新しいフロー (フロー状態は同時安全ではありません)。

## 結果 — 1 CPU / 512 MB プロファイル (非同期モード)

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

このグラフは、6 つのエンジンすべての非同期モードの結果を追跡しています。国会議員
(マルチプロセッシング) 行は、以下の raw-numbers テーブルにあります — mp
N 個のワーカー プロセス間で GIL をバイパスしますが、プールで飽和します
サイズ、パターンはすべての Python フレームワークで同じです。

### 生の数値 (1 CPU / 512 MB、NeoGraph 2026-04-22 on 3.0、Python フィールド 2026-04-19)

2 CPU / 1 GB プロファイルを含むフルマトリックスが含まれています
[`results.jsonl`](results.jsonl)。 N=10,000 は、
最も鋭い話:

| N |エンジン + モード |壁 | P50 | P99 |ピーク RSS | OK / エラー |
|---|---------------|------|-----|-----|----------|---------|
| **10,000** | **ネオグラフ 3.0** | **52 ミリ秒** | **4 μs** | **7 μs** | **5.5 MB** | 10000 / 0 |
| 10,000 | LangGraph 非同期 | 23.4秒 | 20.2秒 | **23.0秒** | 416.2MB | 10000 / 0 |
| 10,000 | LangGraph mp-プール-7 | 8.0秒 | 737μs | 88.4ミリ秒 | 60.3MB | 10000 / 0 |
| 10,000 | Haystack 非同期 | 3.1秒 | 1.7秒 | 2.9秒 | 130.7MB | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9秒 | 167μs | 84.7ミリ秒 | 68.1MB | 10000 / 0 |
| 10,000 | Python グラフの非同期 | 886ミリ秒 | 71μs | **158 μs** | 42.6MB | 10000 / 0 |
| 10,000 | pydantic グラフ mp プール 7 | 2.8秒 | 253μs | 83.8ミリ秒 | 36.7MB | 10000 / 0 |
| 10,000 | **LlamaIndex 非同期** | **OOM が殺害されました** | — | — | — | — |
| 10,000 | LlamaIndex mp プール 7 | 6.6秒 | — | — | 102.5MB | **0 / 10000** |
| 10,000 | **AutoGen 非同期** | **OOM が殺害されました** | — | — | — | — |
| 10,000 | AutoGen mp プール 7 | 46.8秒 | 4.6ミリ秒 | 97.1ミリ秒 | 49.1MB | 10000 / 0 |

2 つのエンジンは、N=10,000 で 512 MB サンドボックスを正常に終了しません。

* **LlamaIndex 非同期** — OOM が強制終了されました。実行中の各ワークフローには、
  実行ごとのイベントバス + チャネルランタイム。そのうち 10k はオーバーシュートします
  ウォールクロックが完了する前に cgroup を実行します。
* **AutoGen 非同期** — OOM が強制終了されました。 10,000 個の同時 GraphFlow インスタンス
  彼らの参加者の州旅行と同じ上限。
* **LlamaIndex mp-pool** — 10,000 個のワーカーの呼び出しがすべて失敗しました。ワークフロー
  インスタンスはワーカー プロセス フォーク間で pickle-safe ではありません。失敗する
  Nとは関係なく。

両方のプロファイルの完全な生の行列は次のとおりです。
[`results.jsonl`](results.jsonl) (セルごとに 1 つの JSON 行)。

## 解釈

### スループット: NeoGraph のスケール、すべての Python asyncio ランタイムのプラトー

NeoGraph の緑色の曲線は、全期間にわたって 22 ～ 25K req/s の範囲に留まります。
すべての Python 非同期曲線が劣化する間、スイープします。発信者側
`asio::thread_pool` は `engine->run()` 呼び出しをすべてにディスパッチします
利用可能なコア。各呼び出しは独自のシングルスレッドを駆動します
`run_sync` を介したスーパーステップ ループ — cgroup の CPU クォータ境界
所要時間はあるがスレッド数ではないため、短いタスクがきれいにインターリーブされます
呼び出し元プール全体で。

**すべての Python 非同期曲線は停滞または劣化します。** 根本的な原因は次のとおりです。
LangGraph、Haystack、pydantic-graph、LlamaIndex、および
AutoGen: 1 つのプロセスに 1 つのイベント ループがあり、GIL が
すべてのコルーチンが実行する必要がある CPU の作業。 N コルーチン → シリアル化
実行 → スループットは N では拡張されません。

各フレームワークの mp-pool モードは、フレームワーク全体で GIL をバイパスします。
`os.cpu_count()` ワーカー プロセス - ただし、そのプール サイズで飽和します
そしてタスクごとにフォーク + ピクルスのオーバーヘッドを支払います。 ~N=1000 を超えると、プールは
フレームワークに関係なく、飽和してスループットがプラトーになります。

### テール レイテンシ: ユニバーサル GIL 上限

N=10,000 では、NeoGraph の P99 はマイクロ秒単位にとどまります。すべてのパイソン
asyncio P99 は N とともに直線的に上昇します。
GIL キューは、スロットの前に完全な実行が完了するまで待機します。

これは LangGraph 固有の問題ではありません。まったく同じ形状が表示されます
Haystack (`to_thread` でラップされた同期パイプライン)、LlamaIndex 用
(非同期イベント駆動型ワークフロー)、pydantic-graph (非同期ステートマシン)、
AutoGen (非同期マルチエージェント ランタイム)。 Python オーケストレーションがある場合
単一プロセスの背後にあるフレームワークでは、GIL が上限となります。

P99 の SLO 期待値を持つ現実的なサーバーの場合 (たとえば、「1 未満」)
リクエストの 99% で 2 秒目)、すべての非同期エンジンは次の時点で中断されます。
正確なブレークポイントはフレームワークによって異なります - ランタイムが軽い
(pydantic-graph、LangGraph) 後で中断し、より重いもの (LlamaIndex、
AutoGen) ははるかに早く壊れますが、それらはすべて壊れます。

### メモリ: asyncio の RSS は、保持されたコルーチン スタックとともに増加します

- **NeoGraph 3.0** は、スイープ全体にわたって 4.2 ～ 5.5 MB の間にとどまります。
  タスクはすぐに返されます。発信者側 `asio::thread_pool` のみ
  およびインフライト `run()` ごとに 1 つの io_context が常駐します。
- **mp プール モード** は、フレームワーク全体で 60 ～ 80 MB 近くにとどまります - ワーカー
  プールサイズが支配的です。タスクは蓄積されません。
  1つずつ発送され、返送されます。
- **asyncio モード** は N とともに直線的に増加します。すべての保留中のコルーチン
  Python スタック フレーム、クロージャ状態、実行ごとのフレームワークを保持します
  州。 10,000 個の実行中のコルーチンでは、合計すると数百個になります。
  重いランタイムの場合は MB。

512 MB のメモリ予算では、一部の asyncio 実行がほぼ限界に達します。
cgroup の上限は N=10,000 です。 256 MB の cgroup が狭いほど重くなります
フレームワークは N=1,000 ～
N=10,000。 NeoGraph には、その予算内でまだ最大 500 MB の余裕があります。

## このベンチが語らないこと

- **大規模なフレームワークが「クラッシュ」することは証明されていません。** ストーリーは次のとおりです。
  プロセスの停止ではなく、使用不可能なレイテンシへの正常な劣化。で
  よりタイトな cgroup 以上の N、OOM キルは終了モードになりますが、
  ここではそれを文書化していないため、フォローアップが必要になります。
- **LLM I/O はモデル化されていません。** 実際のエージェントのワークロードは 100 ～ 1,000 ミリ秒です。
  LLM 呼び出しごとに。その待ち時間は、絶対的な観点から見てエンジンギャップを矮小化します -
  ただし、容量の点では異なります。エンジンが 1,000 req/s しかプッシュできない場合
  ランタイムを通じて、同時 LLM I/O がいくらあっても役に立ちません。
- **永続性については説明しません。** チェックポイントはすべてのサーバーで無効になっていました。
  フレームワーク。有効にすると、比較がストアに移行します
  これは別のベンチマークです。
- **ワークロード形状のバイアス。** カウンタ チェーンは NeoGraph ネイティブです。
  状態のセマンティクス。 Haystack は同期パイプラインを `to_thread` でラップします。
  AutoGen はカウンターをメッセージコンテンツとしてエンコードしますが、pydantic-graph には何もありません
  ファンアウト (このベンチでは使用されていませんが、バースト ワークロードに関連します)
  分岐あり）。各フレームワークは、その仕事をすることを求められているのではなく、
  その最善の仕事。
- **WSL2 上の Docker。** `--cpus` は CPU クォータを強制しますが、表示されません
  コア数、これが NeoGraph の `hardware_concurrency()` がまだ残っている理由です
  ホスト数を返します。ベアメタルの結果は次のようになります。
  方向的には同じですが、NeoGraph 側でよりタイトになります (より少ない
  スレッド、コンテキスト切り替えノイズが減少します)。

## 再現する

```bash
# From the repo root.

# Build images once:
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker build -t lg-concurrent -f benchmarks/concurrent/Dockerfile.langgraph .
docker build -t hs-concurrent -f benchmarks/concurrent/Dockerfile.haystack .
docker build -t pg-concurrent -f benchmarks/concurrent/Dockerfile.pydantic_graph .
docker build -t li-concurrent -f benchmarks/concurrent/Dockerfile.llamaindex .
docker build -t ag-concurrent -f benchmarks/concurrent/Dockerfile.autogen .

# Full matrix (88 cells across 6 engines × 2 modes × 4 concurrencies × 2 profiles,
# excluding neograph which has no mode split):
bash benchmarks/concurrent/run_matrix.sh

# Re-render charts from the results:
node benchmarks/render_concurrent.js
```

単一セルのデバッグ実行:

```bash
docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    ng-concurrent 10000

docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    li-concurrent async 10000
```

各コンテナは次の形式の JSON 行を 1 行出力します。

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
