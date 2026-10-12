<!-- neograph-i18n: source=benchmarks/stress/README.md locale=ja source_sha256=effd44b68d1d5fb1c6111603ec0a039b12092b0e8528783981ce920c1dc4d94d -->
# NeoGraphの持続的な並行実行のストレスベンチマーク

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このランナーは、3ノードのカウンターグラフを、一定の実時間のあいだ繰り返し実行します。測定できるのは、ローカルでのエンジンの入れ替わり（churn）の負荷です。プロバイダーの呼び出し、永続化、本番運用への適合性は測定しません。

## 測定内容と終了ステータス

`bench_sustained_concurrent` のデフォルトは、`--concurrency 1000`、`--duration-s 60`、`--sample-s 5`、`--warmup-s 5`、`--rss-tolerance-pct 25` です。目標とする実行数と同じ数のスレッドを持つ呼び出し側プールを作り、処理が終わるたびに、代わりの実行をキューに入れます。サンプルが報告するのは、レイテンシーの平均と最大で、P99は含みません。計測は呼び出し側のワーカーの中で始まり、そのキューでの待ち時間は含みません。`ok_total` は、例外を投げずに戻った呼び出しの数を数えたものです。返されたグラフのステートを検証した結果ではありません。

終了コード1は、最後の現在のRSSが、記録したウォーム時のベースラインを許容範囲を超えて上回ったことを示します。終了コードが0でも、リークがないことや、実行時のエラーがないことは証明できません。`err_total` は別に確認してください。最後のRSSは、プールを停止してjoinしたあとに読み取ります。そのため、スレッドの終了処理がドリフトに影響します。ウォーム時のベースラインは、最初のサンプルが `warmup-s` に達している場合にだけ、そのサンプルで取得されます。ウォームアップは、最初のサンプルの間隔以下にしてください。ベースラインがないと、ドリフトは0と報告され、メモリのゲートの判定には使えません。

Windowsではプロセスのワーキングセットのカウンターを、Linuxでは `/proc/self/status` を使います。その他のプラットフォームでは、これらのカウンターがないため、0と報告されることがあります。ピークRSSは最大値の記録で、減ることはありません。増加を調べるときは、現在のRSSと、ベースラインが有効かどうかを確認してください。

## ビルドと実行

外部のSchemaProvider SDKをインストールし、そのプレフィックスを設定します。LLMとオプションのNeoGraphのlibcurlバックエンドを無効にしても、Coreには `SchemaProvider::runtime` が必要です。プレフィックスの代わりに、`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` で、SDKのソースのチェックアウトを明示的に指定することもできます。そのビルドには、C++20、Python、libcurl 7.88以上、OpenSSL Cryptoが必要です。依存関係とプラットフォームの制約については、[ビルドガイド](../../README.md)を参照してください。このレシピは、ネットワークからの取得と、使わないNeoGraphの統合を無効にします。
NeoGraph `0.13.1` では、alpha SDK `0.3.0`（インターフェースのリビジョンと共有世代は6）を使い、それに合ったヘッダーとライブラリーで再ビルドしてください。

```bash
# Set SCHEMAPROVIDER_PREFIX to the installed SDK prefix.
cmake -B build-stress -S . \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_PROGRAM=OFF -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-stress --parallel --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
  --concurrency 1000 --duration-s 60 --sample-s 5 \
  --warmup-s 5 --rss-tolerance-pct 25
```

## 保存してある過去の観測結果

日付不明の、以前のRyzen 7 5800Xでの実行です。条件はconcurrency=100、duration=15 sで、実行数は15.3 M回（約1.0 M回/s）、レイテンシーの平均は約55 µs、ウォーム時のRSSは9.3 MB、最後のRSSは7.4 MB（約−20%、終了コード0）です。日付もSDKのリビジョンも記録されていません。過去の観測として残しているもので、スループットの保証ではありません。下の出力の抜粋は、この観測のものです。省略記号はJSONではありません。

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

## アロケーションの圧迫を試す実験

`prlimit` は、Linuxの仮想アドレス空間を制限します。呼び出し側のスロットごとにスレッドを持つため、グラフを実行する前に、スタックとプールの構築だけで、この制限を使い切ることがあります。実行ごとのcatchが記録するのは、`engine->run` の例外です。プールの作成はこのcatchの外にあります。アロケーションが圧迫された状態で、プロセスが正常に終了できるかどうかは、測定して確かめる合格基準です。このスクリプトが保証するものではありません。

```bash
# Linux: cap virtual address space, not resident memory.
prlimit --as=$((256*1024*1024)) \
  ./build-stress/bench_sustained_concurrent \
  --concurrency 200 --duration-s 30
```

## 追加の実験

24時間の実行や、cgroupのメモリ上限を使う実験では、環境と結果を別に記録する必要があります。定常状態の区間ごとに現在のRSSを比べ、`err_total` と終了シグナルを記録してください。cgroupによる常駐メモリの制限と、`prlimit` によるアドレス空間の制限は、区別して扱ってください。このページでは、これらの実行結果は報告していません。
