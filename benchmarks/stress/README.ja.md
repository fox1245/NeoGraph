<!-- neograph-i18n: source=benchmarks/stress/README.md locale=ja source_sha256=245bd9f1555c8f45feba5119b20683c54700e0b74468f8857b4707267680b859 -->
# NeoGraph持続同時実行ストレスベンチマーク

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

3ノードのカウンターグラフを一定時間繰り返します。ローカルエンジンの反復を測定し、プロバイダー呼び出し、永続化、運用準備の測定ではありません。

## 測定と終了状態

`bench_sustained_concurrent`の既定値は`--concurrency 1000`、`--duration-s 60`、`--sample-s 5`、`--warmup-s 5`、`--rss-tolerance-pct 25`です。目標実行数と同じ数の呼び出し側スレッドを作り、完了時に次を投入します。平均と最大レイテンシを出力し、P99は出しません。計測はワーカー内で始まりキュー待機を除外します。`ok_total`は例外なく戻った呼び出し数で、戻り状態の検証数ではありません。

終了1は最終の現在RSSがウォーム基準から許容値を超えて増えたことを示します。終了0はリークや実行エラーがない証明ではありません。`err_total`も確認してください。最終RSSはプール停止とjoin後に読むため、スレッド終了が影響します。基準は最初のサンプルが`warmup-s`に達した場合だけ記録します。ウォームアップを最初のサンプル間隔より長くしないでください。基準がない場合のドリフト0はメモリ検査の証拠になりません。

Windowsはworking-setカウンター、Linuxは`/proc/self/status`を使います。他の環境の0は測定不可の場合があります。ピークRSSは減少しないため、増加の調査では現在RSSと基準の有効性を確認してください。

## ビルドと実行

外部SchemaProvider SDKをインストールしprefixを設定します。LLMとNeoGraphの任意libcurlバックエンドを無効にしてもCoreは`SchemaProvider::runtime`を必要とします。prefixの代わりに`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`で明示的なソースを指定できます。ソースにはC++20、Python、libcurl ≥7.88、OpenSSL Cryptoが必要です。依存と環境制約は[ビルド案内](../../README.md)を参照してください。以下はネット取得と未使用のNeoGraph統合を無効にします。
NeoGraph `0.13.0` には alpha SDK `0.1.0`、interface revision/shared generation 4 を使い、一致する header/library で再ビルドしてください。現在の統合検証は未完了です。

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

## 保存した過去の観測

以前のREADMEの日時不明のRyzen 7 5800X記録は同時実行100、15秒で15.3 M回（約1.0 M runs/s）、平均約55 µs、ウォームRSS 9.3 MBから最終7.4 MB（約−20%、終了0）でした。日付とSDKリビジョンは記録されていません。過去の証拠であり、新しいcutover検証や性能保証ではありません。以下はその出力の抜粋で、省略記号はJSONではありません。

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

## 割り当て圧力の実験

`prlimit`はLinuxの仮想アドレス空間を制限します。呼び出しスロットごとにスレッドがあり、スタックとプール生成がグラフ実行前に上限を消費する場合があります。実行ごとのcatchは`engine->run`の例外を記録しますがプール生成はその外です。圧力下の正常終了は測定する合格基準であり、スクリプトの保証ではありません。

```bash
# Linux: cap virtual address space, not resident memory.
prlimit --as=$((256*1024*1024)) \
  ./build-stress/bench_sustained_concurrent \
  --concurrency 200 --duration-s 30
```

## 追加の実験

24時間実行やcgroup上限には環境と結果の別記録が必要です。定常区間の現在RSSを比較し、`err_total`と終了シグナルを記録してください。cgroupの常駐メモリ制限と`prlimit`のアドレス空間制限を区別してください。本ページはその実行結果を報告しません。
