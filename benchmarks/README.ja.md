<!-- neograph-i18n: source=benchmarks/README.md locale=ja source_sha256=d0a5deba1ae20473d11d5dd6385d0966e7d08ab563f333f5730751dfeb994466 -->
# NeoGraph と Python のグラフ/パイプライン フレームワーク — エンジン オーバーヘッド ベンチマーク

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraph の呼び出しごとのオーバーヘッドを主要なオーバーヘッドに対して測定します。
**no を含む同一形状のグラフ上の Python オーケストレーション フレームワーク
I/O、スリープなし、LLM 呼び出しなし**。数字はエンジンの内容を反映しています
それ自体にコストがかかります (ノードのディスパッチ、状態チャネルの書き込み、リデューサーの呼び出し) - ではありません
シミュレートされた作業のレイテンシー。

比較したフレームワーク:

|フレームワーク |バージョン |抽象化 |
|-----------|---------|-------------|
|ネオグラフ | 3.0 (`feat/taskflow-removal`) |状態チャネル グラフ、C++20 コルーチン + ASIO |
|ランググラフ | 1.1.9 |状態チャネル グラフ (Python) |
|干し草の山 | 2.27 |型付きソケットを使用したコンポーネントのパイプライン |
|ピダンティックグラフ | 1.84 |単一次ノードのステート マシン |
| LlamaIndex ワークフロー | 0.14 |イベント駆動型の非同期ワークフロー |
| AutoGen グラフフロー | 0.7.5 |メッセージパッシングマルチエージェントグラフ |

## ワークロード

6 つの実装はすべて、まったく同じ 2 つのグラフを定義し、1 回コンパイルします。
(該当する場合) ホット ループで呼び出します。

| ID |形状 |状態 |
|----|-------|-------|
| `seq` | 3 ノード チェーン `a → b → c` |単一の `counter` チャネル、各ノードは `counter+1` (上書きリデューサー) を書き込みます。 |
| `par` | 5 人のワーカーをファンアウトし、`summarizer` で参加します。 | `results: list` (リデューサーの追加) + `count: int`;各ワーカーはそのインデックスを追加し、サマライザーは `len(results)` を書き込みます。 |

チェックポイントはすべてのフレームワークで無効になっています。

2 つのポートには、フレームワークごとのワークロード形状変換が必要です。

* **Haystack** には追加リデューサーがありません。各ワーカーが独自に出力します。
  型指定されたソケットとサマライザーがリストの長さを合計します。同じ数の
  コンポーネントは実行ごとにディスパッチされます。
* **pydantic-graph** は単一の次のノードのステート マシンであるため、
  ファンアウトします。 `par` ワークロードは 6 ノードのシリアル チェーンとしてエミュレートされます
  (`w1 → w2 → w3 → w4 → w5 → summ`)。結果ではフラグが付けられていますが、
  アップルツーアップル並列ファンアウト測定。
* **AutoGen** はステート チャネルではなく、メッセージ パッシングです。カウンターは
  テキストメッセージコンテンツとしてエンコードされます。サマライザは受信をカウントします
  労働者のメッセージ。同じグラフ形状でも、異なる状態モデル。

## 結果

以下の **参考実行** は、2026 年 4 月 22 日に測定されました。
x86_64 Linux 上の NeoGraph v3.0.0、g++ 13 リリース `-O3 -DNDEBUG`、
CPython 3.12.3。 NeoGraph: `bench_neograph` の 10 ランの中央値。
Python フィールド: フレームワークあたり 3 回の実行中央値。バージョン: neograph v3.0.0、
langgraph 1.1.9、haystack-ai 2.28.0、pydantic-graph 1.85.1、
ラマインデックスコア0.14.21、オートジェンエージェントチャット0.7.5。

当時のマスター (2026-04-29) での再測定が以下に含まれています
リファレンス実行。 `par` 行は、サポートされている両方の実行を報告します。
明示的な体制: 現在の `worker_count=1` のデフォルトと
`set_worker_count_auto()` によって有効化されたエンジン所有のプール。 *注意事項*を参照してください。
表の後に、I/O なしマイクロベンチマークがこれらを維持する必要がある理由を示します。
数字は別です。

![Engine-overhead benchmark: per-iteration latency and peak RSS](../docs/images/bench-engine-overhead.png)

### 反復ごとのオーバーヘッド (μs、低いほど良い)

|フレームワーク | `seq` (3 ノード チェーン) | `par` (ファンアウト 5 + 結合) | `seq` 対 NeoGraph | `par` 対 NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
| **NeoGraph v3.0.0** *(参照、2026-04-22)* | **5.0** | **11.8** | 1× | 1× |
| **NeoGraph マスター** *(2026-04-29、デフォルト `worker_count=1`)* | **5.25** | **14.4** | 1× | 1× |
| **NeoGraph マスター** *(2026-04-29、`set_worker_count_auto()`)* | **5.25** | **278** | 1× | 1× |
|ヘイスタック 2.28.0 | 139.85 | 278.48 | 28.0× / 26.6× / 26.6× | 23.6× / 19.3× / 1.0× |
| pydantic-graph 1.87.0 | 227.14 | 280.26¹ | 45.4× / 43.3× / 43.3× | 23.7×¹ / 19.5×¹ / 1.0×¹ |
|ランググラフ 1.1.10 | 642.62 | 2,261.55 | 128.5× / 122.4× / 122.4× | 191.7× / 157.1× / 8.1× |
| LlamaIndex ワークフロー 0.14.21 | 1,564.54 | 4,373.76 | 312.9× / 298.0× / 298.0× | 370.7× / 303.7× / 15.7× |
| AutoGen グラフフロー 0.7.5 | 3,126.86 | 7,281.08 | 625.4× / 595.6× / 595.6× | 617.0× / 505.6× / 26.2× |

右端の 2 つの列は、v3.0.0 リファレンス / マスターの 3 つの比率を示します。
worker=1 デフォルト / 対マスター自動ワーカー モード。

2026 年 4 月 29 日の再測定 (上) は、2026 年 4 月 22 日の基準を 1 行あたり ±10 % 以内で再現しています (同じマシン、同じツールチェーン、同じワークロード)。 README の見出しの主張 (`seq` では 130× LangGraph、600× AutoGen) はマスター HEAD に当てはまります。

¹ pydantic-graph `par` はシリアル 6 ノード エミュレーションです。
ファンアウトをサポートします。並列ワークロードではありません。完全を期すために含まれています。

### `par` 行に関する注記 (`seq` は変更されません)

`par` 14.4 → 278 μs ギャップは、
実際の作業を行わない 5 つのノードのエンジン所有のスレッド プール:

* **現在の `build()` / `compile()` のデフォルト: `worker_count=1`.** いいえ
  エンジン所有のプールがインストールされているため、ファンアウトは呼び出し側でディスパッチされます。
  執行者。これは 14.4 μs の行であり、次の正しい回帰信号です。
  ノードが非スレッドセーフ状態を保持する CPU の小さなノードまたはグラフ。
* **並列ファンアウトは明示的です。** `set_worker_count_auto()` のサイズは
  `hardware_concurrency` にプールします。 `set_worker_count(N)` は固定を選択します
  シーリング。これは 278 μs の行です。コーディネート費用はこちらからご覧いただけます
  各ワーカーは整数を 1 つだけ追加しますが、それ以外は無視できるためです。
  100 ミリ秒の LLM 呼び出しにより、独立したノードのオーバーラップが可能になります。

ベンチマーク ソースを変更せずに両方のレジームを実行します。

```bash
./build/bench_neograph 10000 5000 1
./build/bench_neograph 10000 5000 auto
```

3 番目の引数は `par` エンジンに適用され、`1` (デフォルト) を受け入れます。
`auto`、または任意の正のワーカー数。出力には以下が含まれます
`config\tpar_workers\t...` 行なので、保存された結果は実行モードを保持します。

見出しは今でも変わりません。NeoGraph はテーブルのすべての行を次の方法で獲得します。
フレームワークと構成に応じて 8× ～ 600×。 「199倍高速」
`par` の LangGraph よりも」リファレンスは、2026 年 4 月 29 日で 163 倍でした
ワーカー=1 測定;オートワーカーモードはマイクロベンチマークのオーバーヘッドを犠牲にします
ブロッキングノードまたは CPU バウンドノードでの実際の同時実行のために。

### エンドツーエンドのプロセス指標

ウォームアップと両方のワークロードを含むバイナリ/スクリプト ランタイム全体。
`seq` = 10,000 イター、`par` = 5,000 イター。で測定
`/usr/bin/time -f "%e s, %M KB"`。

|フレームワーク |合計経過時間 |ピーク RSS |執行者 |
|-----------|--------------:|---------:|---------:|
| **ネオグラフ 3.0** | **0.11秒** | **4.5MB** |デフォルトではシングルスレッド io_context |
|ピダンティックグラフ | 3.98秒 | 35.1MB |シングルスレッド非同期 (GIL) |
|干し草の山 | 3.85秒 | 80.3MB |シングルスレッド非同期 (GIL) |
|ランググラフ | 18.95秒 | 60.1MB |シングルスレッド非同期 (GIL) |
| LlamaIndex ワークフロー | 39.49秒 | 101.4MB |シングルスレッド非同期 (GIL) |
| AutoGen グラフフロー | 63.29秒 | 52.3MB |シングルスレッド非同期 (GIL) |

NeoGraph 3.0 のデフォルトのスーパーステップ ループは、コルーチンを実行します。
`run_sync` 経由のシングルスレッド io_context。 CPU 並列ファンアウトは
`engine->set_worker_count(N)` 経由でオプトインします。 I/Oバウンドノードの場合
ワークロードでは、単一スレッドが依然として co_await 一時停止によってオーバーラップします。

## Linux ARM64 ベースライン: Neoverse-N1

これは、別のネイティブ ARM64 プラットフォーム ベースラインです。
[#165](https://github.com/fox1245/NeoGraph/issues/165)、回帰ではありません
上記の x86_64 テーブルとの比較。リビジョン `d7a6477` に固定されています
独自の依存関係が設定されているため、後の測定値に起因するものが残ります。

|アイテム |値 |
|---|---|
|日付 | 2026-07-22 |
| OS | Ubuntu 24.04、Linux `6.17.0-1018-oracle` |
|建築 | `aarch64` |
| CPU | 4 vCPU、ARM Neoverse-N1 |
|メモリ | 23 GiB、スワップなし |
|コンパイラ / CMake | GCC 13.3.0 / CMake 3.28.3 |
|パイソン | CPython 3.12.3 |
| NeoGraph リビジョン | `d7a6477` |

ワークロードと反復数は主要ベンチマーク 10,000 と一致します。
各グラフのコンパイル後の `seq` 反復および 5,000 `par` 反復
一度。 NeoGraph は 10 回測定され、各 Python 実装は 3 回測定されました。
回。表には中央値が示されています。チェックポイント、ネットワーク I/O、モデル呼び出し、
そして睡眠は無効になりました。プロセス全体のピーク RSS の由来
`/usr/bin/time -f "%e s, %M KB"`。

|フレームワーク | `seq` (μs/イター) | `par` (μs/イター) | `seq` 対 NeoGraph | `par` 対 NeoGraph |ピーク RSS |
|---|---:|---:|---:|---:|---:|
| **ネオグラフ** | **9.50** | **21.80** | 1× | 1× | **4.35 MB** |
|ヘイスタック 3.0.0 | 153.44 | 329.67 | 16.2× | 15.1× | 73.6MB |
| pydantic-graph 1.87.0 | 342.60 | 405.87¹ | 36.1× | 18.6×¹ | 32.1MB |
|ランググラフ 1.2.9 | 1,037.55 | 3,289.22 | 109.2× | 150.9× | 63.7MB |
|ラマインデックス 0.14.23 | 2,765.04 | 7,824.85 | 291.1× | 358.9× | 96.9MB |
|オートジェネ0.7.5 | 4,166.39 | 9,571.11 | 438.6× | 439.0× | 47.8MB |

NeoGraph はデフォルトの worker=1 を使用したため、`par` 行はトポロジを測定します。
エンジン所有のスレッド プールではなく、リデューサーとシリアル ディスパッチのオーバーヘッド
実行。個別の並列には明示的な `auto` ベンチマーク モードを使用します。
ファンアウト測定。 2 つのモードを 1 つの見出しに組み合わせないでください。

¹ pydantic-graph はこのファンアウト トポロジをモデル化できません。その `par` 行は
上で説明したものと同じシリアル 6 ノード エミュレーションです。バージョン 1.87.0 が固定されました
当時の最新の 2.15.0 API がベンチマークのサポートを終了したためです。
`Graph(...)` コンストラクター。プロセスは CPU 固定または cgroup 制約されていませんでした。

## 数字の意味

1. **実行ごとのエンジンのオーバーヘッドは、Python 全体で約 29 倍から約 642 倍に及びます
   field.** Haystack が最も無駄のない競合他社です (型付きの DAG
   ソケット、最小限のランタイム）;それでも、1 秒あたり 28.8 倍のコストがかかります
   NeoGraph よりも優れています。もう一方の端では、AutoGen は 642 倍の速度で動作します。
   NeoGraph のコストは、実行ごとのマルチエージェント状態のセットアップによるものです。
2. **NeoGraph 3.0 は、両方の軸において 2.0 よりも優れています。** 同期の折りたたみ
   1 つのコルーチン パスへの非同期でもエンジンは退行しませんでした
   オーバーヘッド — 完全なコルーチン機構 (`run_sync` + io_context)
   コールごと) は、2.0 と比較して、リリース ビルドでは 5 μs 未満です
   同期タスクフロー パス上で 20.65 μs をアドバタイズしました。
3. **メモリ使用量は、NeoGraph の方が桁違いに有利です。
   詳細.** 4.8 MB (NeoGraph) に対し、Python フィールド全体では 35 ～ 101 MB。
   SBC クラスのターゲット (Raspberry Pi クラスの RAM) では、これは
   耐荷重指標 - 「快適に動作する」と「快適に動作する」の違い
   「慎重に実行してください」。
4. **並列ファンアウトは 3.0 でオプトインされています。** NeoGraph 2.x が出荷されています
   デフォルトとしてのタスクフローのワークスチールプール。 3.0 では、
   デフォルトとしてのコルーチン パス (シングルスレッド ディスパッチ、安価)、および
   マルチスレッド プールをオプトインとして公開します。
   `engine->set_worker_count(N)` — エージェントの正しいデフォルト
   I/O バウンド (LLM レイテンシーが支配的) なワークロード
   それ以外の場合は、高速化されずにスレッド作成のオーバーヘッドが発生します。

## 注意事項 — このベンチで測定できないもの

* **実際のエージェントのワークロード。** LLM 主導のパイプラインがボトルネックになっている
  プロバイダーの遅延による (通話あたり 100 ミリ秒～10 秒)。エンジンのオーバーヘッドがなくなる
  その規模で。メンタル モデル: NeoGraph 3.0 のコストは 1 コールあたり約 5 μs、
  Haystack ~144 µs、LangGraph ~657 µs、LlamaIndex/AutoGen ~2 ～ 7 ms —
  500 ミリ秒の API ラウンドトリップの隣ではすべて見えなくなります。このベンチは重要です
  非 LLM ノード、高密度エージェント オーケストレーション、および起動負荷の高い場合
  展開。
* **フレームワークに適したワークロード。** AutoGen、LlamaIndex、および
  pydantic-graph はそれぞれパラダイム (マルチエージェント チャット、
  イベント駆動型の長時間実行ワークフロー、ステートマシン制御フロー)
  このベンチは運動しないということ。 NeoGraph で測定します
  ホームグラウンド。
* **チェックポイントのスループット** 各フレームワークでの永続性の有効化
  シリアル化コストが支配的になります。それは別のベンチマークです。
* **コールド スタート。** 各実装には 10 反復のウォームアップ ループが含まれています
  測定前。フルプロセス番号には Python インタープリターが含まれます
  ブート (~200ms) とフレームワークのインポート時間。これは大きく異なります (LlamaIndex)
  AutoGen は実質的なツリーをインポートします)。
* **公平性** NeoGraph は CMake `-DCMAKE_BUILD_TYPE=Release` で構築されました
  これは GCC では `-O3 -DNDEBUG` に解決されます。すべての Python フレームワークは、
  現在の pip がインストールされているバージョンの CPython 3.12 をストック —
  運用環境に典型的な展開であり、カスタム チューニングは必要ありません。歴史的メモ:
  この README の 3.0 より前のバージョンでは `-O2` が宣伝されていました。
  スタンドアロンベンチコマンドが使用したもの。 CMake ビルドは常に
  `Release` を `-O3` に解決しました。

## Reproduce

```bash
# Build native Core + v1 Program benchmarks (Release is required for
# representative timings; the default CMake build type is not optimized).
cmake -B build-program-bench -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_ASYNC=ON
cmake --build build-program-bench --target \
    bench_neograph bench_program bench_program_dispatch -j

# Positional arguments are iterations, warmup runs, and measured samples.
# bench_neograph additionally accepts par_workers before warmup/samples.
./build-program-bench/bench_neograph 10000 5000 1 10 5
./build-program-bench/bench_neograph 10000 5000 auto 10 5
./build-program-bench/bench_program 1000 10 5
./build-program-bench/bench_program_dispatch 100000 10 5
```

Each native benchmark prints `config`, `runtime`, `header`, and `result`
records. Report the median of the measured samples after the explicit warmup;
do not compare a single short run. `bench_program` uses in-memory stores and
no provider/network calls. `bench_program_dispatch` measures only immutable
`ProgramPlan` lookup and descriptor traversal, not Core execution.

The Python framework comparison remains optional and requires third-party
packages:

```bash
# Shared Python venv for every Python framework:
python3 -m venv /tmp/bench_venv
/tmp/bench_venv/bin/pip install \
    langgraph \
    haystack-ai \
    pydantic-graph \
    llama-index-core \
    "autogen-agentchat" "autogen-core" "autogen-ext"

# Run each bench (10k seq + 5k par matches the C++ side):
/tmp/bench_venv/bin/python benchmarks/bench_langgraph.py      10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_haystack.py       10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_pydantic_graph.py 10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_llamaindex.py     10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_autogen.py        10000 5000

# Peak RSS + wall time:
/usr/bin/time -f "%e s, %M KB" ./build-program-bench/bench_neograph
```

The service-backed checkpoint, HTTP, and concurrent Docker benchmarks are
separate experiments; they are not required for the deterministic native
Core/Program run above.

Output format is tab-separated `config`, `runtime`, `header`, `result`, or
`metric` records. The native result rows contain median total time and
per-iteration time; Python scripts retain their historical
`workload<TAB>iters<TAB>total_ms<TAB>per_iter_us` rows.

## Environment used for the 2026-04-19 numbers

```
OS:        Linux 6.6.87.2-microsoft-standard-WSL2 (Ubuntu 24.04 userland)
CPU:       host CPU (8 logical cores exposed to WSL)
Compiler:  g++ 13.x, -std=c++20 -O2 -DNDEBUG
Python:    3.12.3 (system)
Versions:  langgraph 1.1.7, haystack-ai 2.27.0, pydantic-graph 1.84.1,
           llama-index-core 0.14.20, autogen-agentchat 0.7.5
```

Numbers will vary on your hardware, but the ratios should be stable to
within ~20%.
