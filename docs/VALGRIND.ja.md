<!-- neograph-i18n: source=docs/VALGRIND.md locale=ja source_sha256=38c9d7fc8073d2557d9b5533dfb33ec2de6867a55ba394bc26d6a5cf6b2e6215 -->
# メモリ & サニタイザースイープ (Valgrind / ASan / UBSan / soak)

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

サンプルバイナリとテストバイナリが行ったすべての割り当てを解放していることの
グラウンドトゥルース確認。`valgrind --tool=memcheck
--leak-check=full --show-leak-kinds=all` で実行。基準は、API キー不要の
サンプル群全体で **0 リーク、0 エラー** です。

## ローカルで再現

```bash
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DNEOGRAPH_BUILD_TESTS=ON \
      -DNEOGRAPH_BUILD_EXAMPLES=ON \
      -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
      -DNEOGRAPH_BUILD_POSTGRES=OFF ..
cmake --build . -j$(nproc)

# Sweep all no-API-key examples
for ex in example_custom_graph example_parallel_fanout example_send_command \
          example_intent_routing example_state_management example_all_features \
          example_plan_executor example_async_concurrent_runs \
          example_classifier_fanout example_subgraph example_checkpoint_hitl; do
    echo "=== $ex ==="
    valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=42 \
        ./$ex >/dev/null
done
```

## 最終スイープ

master (commit 4b02dea、classifier-fanout サンプル追加後) に対して 2026-04-29 に実行。
Valgrind 3.22.0、GCC 13.3 Debug ビルド。

### サンプル — 11 / 11 クリーン

| サンプル | Allocs | Bytes | Errors |
|---|---:|---:|---:|
| `example_all_features` | 5,097 / 5,097 | 1,080,618 | 0 |
| `example_async_concurrent_runs` | 683 / 683 | 226,919 | 0 |
| `example_checkpoint_hitl` | 1,973 / 1,973 | 524,478 | 0 |
| `example_classifier_fanout` | 1,696 / 1,696 | 419,024 | 0 |
| `example_custom_graph` | 799 / 799 | 231,767 | 0 |
| `example_intent_routing` | 3,960 / 3,960 | 916,910 | 0 |
| `example_parallel_fanout` | 1,330 / 1,330 | 364,867 | 0 |
| `example_plan_executor` | 3,616 / 3,616 | 823,613 | 0 |
| `example_send_command` | 3,279 / 3,279 | 747,161 | 0 |
| `example_state_management` | 2,540 / 2,540 | 640,311 | 0 |
| `example_subgraph` | 1,568 / 1,568 | 419,423 | 0 |
| **累計** | **26,541 / 26,541** | **6,395,091** | **0** |

すべての割り当てが解放され、無効読み取り 0、無効書き込み 0、不一致 free 0、
use-after-free 0。

### テスト — `*Smoke*:GraphCompiler*:GraphState*` クリーン

| スイート | テスト | Allocs | Bytes | Errors |
|---|---:|---:|---:|---:|
| Smoke / GraphCompiler / GraphState (31 テスト) | 31 / 31 パス | 12,551 / 12,551 | 1,890,717 | 0 |

valgrind 下での完全な `neograph_tests` スイートは ~30 分かかります —
上記のサブセットが PR ごとの下限。フルスイープは nightly CI ジョブの一部として
実行可能 (まだ配線されていません)。

## カバーされていないもの

- ネットワークを含むサンプル (`example_react_agent`、`example_mcp_*`、
  `example_*_responses_*`) — TLS/ソケットの相互作用が libssl / libcurl
  からのノイズを生成し、valgrind 抑制でマスクする必要がある。
  代わりにモックプロバイダを使用してエンジンパスでリークチェックを使用。
- Crawl4AI / Postgres サンプル — 外部プロセスまたはライブラリの状態がリーク
  チェックを混乱させる。これらのパスのカバレッジは valgrind ではなく CI の
  ASan で実施。
- Python バインディング (`_neograph.so`) — Python インタプリタには終了時の
  意図的な「リーク」(割り当て済みだが解放されないモジュール状態) が多数あり、
  valgrind のシグナルを圧倒する。`LSAN_OPTIONS=detect_leaks=0` を付けた
  ASan が適切なツール。

## ASan + UBSan + LSan スイープ — 11/11 サンプル + 322 ctest クリーン

サニタイザ付きでコンパイル:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
```

`LSAN_OPTIONS=` と `UBSAN_OPTIONS=` を設定してサンプル + テストを実行:

```bash
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
ctest --test-dir build-asan -E "BIG_|valgrind"   # 322/322 pass (2026-04-29)
```

master HEAD (commit 6bd9632、2026-04-29) での最終スイープ:

| 対象 | 結果 |
|---|---|
| 11 mock サンプル (custom_graph、send_command、intent_routing、state_management、all_features、subgraph、checkpoint_hitl、classifier_fanout、async_concurrent_runs、parallel_fanout、plan_executor) | ✓ exit=0、ASan/UBSan エラー 0 |
| `neograph_tests` ctest (サニタイザ下 322 テスト) | ✓ 322/322 パス |

ASan が本日追加された再帰ガードで 1 件の誤検出を発見 —
元の `thread_local int` 深さカウンタがネストされたグラフ間で発火していた
(サブグラフノードの内部エンジンが同じスレッドでディスパッチし、外部カウンタを
非ゼロにしていた)。ノードごとの `const GraphNode*` キーに切り替えて修正し、
同じノードが自身のデフォルトチェーンに再入した場合のみガードが発火するように。

## 長時間ソークストレステスト — 10,000 グラフ実行、RSS Δ = 0 KB

```cpp
// /tmp/stress_runs.cpp — see commit log
for (int i = 0; i < 10000; ++i) {
    engine->run(RunConfig{.thread_id = "t" + std::to_string(i),
                          .input    = {{"count", 0}}});
    if (i == 100)  rss_at_100  = read_rss_kb();
    if (i == 9999) rss_at_1000 = read_rss_kb();
}
```

master HEAD で、実行ごとに深さ 3 まで再帰的に Send ファンアウトを送出する
Counter ノードを使用して実行 — 現実的なストレス形状:

```
10000 runs wall=0.68s  ops=14728/s  RSS@100=4608kB  RSS@9999=4608kB  Δ=0kB
PASS: RSS growth bounded
```

10,000 回の逐次グラフ実行、各実行で数 KB を割り当てて解放、最後の 9,900
イテレーションにわたって **0 KB の常駐セット増加**。Linux glibc アロケータが
解放されたブロックをプールにクリーンに返却 — 実行ごとのリークパスは存在しません。

## CI ゲート (sanitizer-test, tsan-test, fuzz-canary)

`.github/workflows/ci.yml` の 3 つの CI ジョブがすべての PR と master への
push で以下を強制:

### `sanitizer-test` — ASan + UBSan + LSan

| ステップ | 対象 |
|---|---|
| `ctest -E "BIG_\|valgrind"` (under `-fsanitize=address,undefined`) | 外部インターフェースを含む全ユニットテスト (サービスコンテナ経由の Postgres、MCP HTTP/stdio、libssl/libcurl ConnPool) |
| 同フラグ下の 11 mock サンプル | フルエンジンパスオーケストレーションカバレッジ |
| `LD_PRELOAD=libasan.so` + `detect_leaks=1` での `pytest bindings/python/tests/` | リーク検出有効で 46/48 Python テスト (pybind を越えて Python 例外を伝播させる 2 テストを除外 — 既知の ASan `__cxa_throw` インターセプト制限、NeoGraph のバグではない) |

### `tsan-test` — エンジンの並行パスでの競合検出

| ステップ | カバレッジ |
|---|---|
| `setarch x86_64 -R ctest -E "BIG_\|valgrind"` (under `-fsanitize=thread`) | 新しい `ConcurrentStress.TwoHundredOverlappingRunsAllSucceed` (200 同時 `run_async` × 3 方向 Send ファンアウト — ワーカープール、スケジューラ、parallel_group、CheckpointStore の並行パスでのデータ競合を捕捉) を含む全 344 ユニットテスト |
| TSan 下の 5 ファンアウト/非同期サンプル | `example_classifier_fanout` + `parallel_fanout` + `send_command` + `plan_executor` + `async_concurrent_runs` |

`setarch x86_64 -R` ラップは `ADDR_NO_RANDOMIZE` をクリアする (カーネルの
`mmap_rnd_bits` デフォルトが Ubuntu 24.04+ で TSan の `unexpected memory mapping`
FATAL を引き起こす)。フラグは `fork` を通じて継承されるため、すべてのテスト
子プロセスも TSan フレンドリーなアドレスレイアウトになる。

TSan + ASan はリンク時に相互排他的なため、これは `sanitizer-test` とは別のジョブ。

### `fuzz-canary` — `GraphCompiler::compile` への libFuzzer

| ステップ | カバレッジ |
|---|---|
| `fuzz_graph_compile` for 60 s wall (`-max_total_time=60`) | `tests/fuzz/corpus/graph_compile/` 下のシードコーパスを変異させ、バイト列を `neograph::json::parse` → `GraphCompiler::compile` に投入。パーサー UB、未処理例外、ヒープバッファオーバーフロー回帰を捕捉。master HEAD での初回実行はクラッシュなしで 1.94 M イテレーション。 |

Clang の `-fsanitize=fuzzer,address,undefined` でビルドされ、クラッシュ時に
ASan/UBSan 診断が同じトレースで表示される。

## リリースビルドハードニング

Release / RelWithDebInfo / MinSizeRel ビルドはデフォルトで多層防御フラグを有効化
(`NEOGRAPH_ENABLE_HARDENING=ON`):

| フラグ | 捕捉対象 |
|---|---|
| `-D_GLIBCXX_ASSERTIONS` | std::vector OOB、`end()` の逆参照、イテレータ無効化、未初期化 `std::optional` アクセス — 暗黙の UB の代わりに診断付き abort。Debug + Release で有効。 |
| `-fstack-protector-strong` | リターンアドレスを破壊するバッファオーバーフロー — カナリアチェックが `ret` 前に発火。 |
| `-fcf-protection=full` | 間接 call/jump ターゲットに制御フロー整合性タグ付け。ROP スタイル攻撃がコールサイトで失敗。CET-IBT を持つ amd64 で安価。 |
| `-D_FORTIFY_SOURCE=2` | libc 文字列/メモリルーチンのインラインチェック。Release のみ (≥`-O1` が必要)。 |
| `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack` | 読み取り専用再配置、即時バインディング (遅延 PLT 書き込みなし)、実行不可スタック — RELRO ベースライン。 |

master HEAD で `bench_neograph` によるパフォーマンス影響測定:

|  | seq µs | par µs |
|---|---:|---:|
| Baseline Release | 5.1 | 275.2 |
| Hardened Release | 5.1 | 275.6 |

測定ノイズ内で **0 % オーバーヘッド**。これらのフラグは作業をリンカ (再配置、
PLT) と関数ごとの 8 バイトカナリアロード+比較にシフト — NeoGraph のエンジン
パスの µs スケールでは不可視。

サニタイザビルド (ASan/TSan/UBSan) では自動的に無効化され、サニタイザ自身の
チェックと重複しないように。MSVC では無効 (異なるハードニングプリミティブ —
`/GS` などを使用、ここでは対象外)。

## 試行されたが実用的でないサニタイザの組み合わせ

**MemorySanitizer** (未初期化読み取り検出): リンクされるすべての C/C++
ライブラリ — libstdc++、libssl、libcurl、libpqxx を含む — が MSan 計装
されている必要があり、そうでない場合ライブラリへの呼び出しがシグナルを圧倒する
誤検出を生成する。Ubuntu 24.04 の Clang プリビルド `libc++` は MSan バリアントを
出荷しておらず、標準ライブラリ + すべての推移的依存の再ビルドは非現実的。
ASan+UBSan+TSan トリオはヒープ割り当て状態に漏出する未初期化読み取りを既に捕捉する
(一部のパスで `detect_uninitialized_reads=1` セマンティクスで ASan 下でヒープが
割り当て時にポイズニングされるため)。スキップ。

## 抑制

| ファイル | 対象範囲 |
|---|---|
| [`tests/lsan_suppressions.txt`](../tests/lsan_suppressions.txt) | libssl / libcurl / libpq / libpqxx / libstdc++ ABI / glibc TLS / CPython インタプリタ / pybind11 型初期化 / pydantic-core。サードパーティのみ — NeoGraph シンボルの追加は実際のバグであり、代わりにリークを修正。 |
| [`tests/tsan_suppressions.txt`](../tests/tsan_suppressions.txt) | asio リアクタ & ソケットサービス (epoll happens-before)、yyjson SIMD 読み取り、OpenSSL CRYPTO_THREAD_run_once。ライブラリ内部の良性競合。 |

## 並行ストレステスト

`tests/test_concurrent_stress.cpp` が標準 ctest スイートの一部として実行される
(Debug と ASan の両方で):

- **TwoHundredOverlappingRunsAllSucceed** — 200 `engine->run_async()` 呼び出しが
  1 つの io_context で重複し、各呼び出しが 3 方向 Send ファンアウト。
  parallel-group + pending-writes 機構が ASan 下で競合フリーであり、200 実行
  すべてが期待される `{0, 1, 4}` ワーカー出力を生成することを検証。
- **RssBoundedOverHundredsOfConcurrentRuns** — 200 実行の 5 バースト
  (合計 1,000 同時) で RSS Δ ≤ 10 MB しきい値。ASan 下ではスキップ
  (サニタイザのシャドウメモリ増加がシグナルを支配するため)。

Debug ビルド実行で 1,000 同時実行にわたって RSS Δ=128 kB を生成 —
エンジン側のメモリプロファイルは持続的な同時負荷下でフラット。
