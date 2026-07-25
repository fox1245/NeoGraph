<!-- neograph-i18n: source=benchmarks/stress/README.md locale=ja source_sha256=87cc091ee71ab14f86ff9642a147a3d80e1452c56020c0073aba63e125258190 -->
# NeoGraph ストレス ハーネス

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

エンジンのオーバーヘッドと
単一ショットの同時ベンチマーク。どこに `benchmarks/bench_neograph`
**通話ごとのコスト**を測定し、`benchmarks/concurrent/...` は
**単一 10k バースト**、このディレクトリは NeoGraph を **長期にわたって**実行します。

## ここには何がありますか

### `bench_sustained_concurrent`

N 個のグラフ実行を実測時間の M 秒間保持します。新しいものを提出します
完了するとすぐに実行されるため、飛行中はターゲットに留まります。サンプル
RSS およびウィンドウごとの遅延 (`--sample-s` 秒ごと)。 RSS の場合は 1 を終了します
暖かい時期の間は`--rss-tolerance-pct`以上上向きにドリフトします。
ベースライン (ウォームアップ後) と最終サンプル。

バーストベンチでは検出できない 3 つの障害モードを検出します。

- **定常状態リーク** — コルーチン / 保留中の書き込み / キャッシュ
  際限なく成長する状態。ドリフトゲートはベストエフォート型です
  (Valgrind / LSan は依然として権威あるツールです。ASan / TSan CI を参照してください)、
  しかし、60 秒を超える RSS の 25% の上昇は、「これを見てください」という強いシグナルです。
- **レイテンシー ドリフト** — ウィンドウごとの平均 / 最大値は、
  プールが温まります。多くの場合、スレッド プールの枯渇またはスケジューラを指します
  t=0 バースト テストでは表示されない背圧。
- **チャーンによるプールの枯渇** — 完了は新規と重複しています
  送信が行われるため、ワーカー プールには混合された飛行中パターンが表示されます。
  バーストのオールドレインではありません。

#### 使用法

```bash
cmake -B build-stress -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_TESTS=OFF \
    -DNEOGRAPH_BUILD_EXAMPLES=OFF
cmake --build build-stress -j$(nproc) --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
    --concurrency        1000 \
    --duration-s         60   \
    --sample-s           5    \
    --warmup-s           5    \
    --rss-tolerance-pct  25
```

スモーク結果 (同時実行数 = 100、継続時間 = 15 秒、Ryzen 7 5800X 上):
- 15.3 M グラフ実行 / 15 秒 ≈ **1.0 M 実行/秒** 継続
- 実行あたりの平均レイテンシー: ~55 μs
- RSS ウォーム: 9.3 MB → 最終: 7.4 MB (ドリフト ‑20 %、出口 0)

#### 出力形状

サンプルごとに 1 行の JSON 行と、最後の要約行が 1 行あります。

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

### `prlimit` の下の `bench_sustained_concurrent` (メモリ キャップ テスト)

NeoGraph ハンドルを証明するために、ハーネスを仮想メモリ キャップで包みます。
割り当てプレッシャーをクリーンに:

```bash
# Cap address space at 256 MB. Allocations beyond this fail with
# std::bad_alloc — NeoGraph's audit-Round-5 typed catch in
# graph_executor (commit ead703e) rethrows bad_alloc instead of
# silently retrying, so the workload should error out instead of
# crashing.
prlimit --as=$((256*1024*1024)) \
    ./build-stress/bench_sustained_concurrent \
        --concurrency 200 --duration-s 30
```

合格基準: プロセスが正常に終了します (戻りコード 0 または 1、SIGABRT ではありません)
/ SIGSEGV)、`err_total` はゼロ以外の可能性があります (これらは bad_alloc
失敗した実行として再スローします)。

## まだここにはいません

- **24 時間浸漬** — 同じハーネス、より長い壁窓。で実行します
  専用ホスト。単調非減少については `peak_rss_kb` をご覧ください
  数時間にわたる傾向。
- **cgroup 限定実行** — `systemd-run --scope -p MemoryMax=512M`
  `prlimit` よりも厳しいリソース上限 (カーネル側)
  割り当て時間のチェックだけでなく強制も行われます)。 WSL2 システム
  サポートは限られています。実際の Linux ホストでテストします。
