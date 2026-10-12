<!-- neograph-i18n: source=docs/VALGRIND.md locale=ja source_sha256=843b39a1b1a8b8f59fd537d6eeed53a258ad133623776cf2c22396c72872dce0 -->
# メモリとサニタイザーの検査

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 対象範囲と現在の前提条件

不正なメモリアクセスとメモリリークの検出にはMemcheck、アドレスのエラーにはASan、対象範囲内の未定義動作にはUBSan、データ競合にはTSanを使います。1つのツールを通ったからといって、別のツールが調べる性質を証明したことにはなりません。以下のレポートは、日付のある過去の結果であり、新しい型付きプロバイダーの適格性確認ではありません。

現在のCoreのビルドには、LLMノードとPostgreSQLを無効にしても、外部の `SchemaProvider::runtime` が必要です。`SDK_PREFIX` に、対応するインストール済みのSDKを設定してください。または、`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` とその開発用の依存を用意します。SDKの内部をサニタイザーで調べるには、そのSDKも、対応する計装（`SP_SANITIZE=address` または `thread`）でビルドします。記録されている、インターフェース3でのランタイムとarchiveの適格性確認は、Linux/POSIXを対象としていて、インターフェース4ではありません。これらのLinuxのコマンドは、Windows、macOS、WASMの適格性確認にはなりません。

NeoGraphにはCMake 3.20以降が必要です。ソースの選択順は、明示したSDKのソースディレクトリ、インストール済みのパッケージ、デフォルトで取得されるリビジョン固定の公開archiveの順です。インストール済みのSDKをオフラインで使う場合は、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を設定します。ソースから、または取得してビルドする場合は、NeoGraphのAsioとyyjsonを使います。システムのlibcurlとOpenSSLの開発用ファイルは、引き続き必要です。

## 現在のMemcheckのシナリオを実行する

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

再ビルドした成果物で、ネットワークを使わない関連するサンプルと、プロバイダーの寿命やキャンセルのシナリオを実行します。ネットワークとPythonの経路には、範囲を絞った診断と、レビュー済みのサードパーティ向けsuppressionが必要です。これらをメモリ検査の対象から外すことはできません。モックプロバイダーが通っても、SDKのトランスポートや、本物のネイティブリプレイの保管の適格性確認にはなりません。

## 過去のMemcheckの一括検査（2026-04-29）

記録されている一括検査は、masterのコミット `4b02dea`、GCC 13.3のDebugビルド、Valgrind 3.22.0で行いました。APIキーを使わない11個のサンプルで、不正なアクセス、対応しない解放、リークは、いずれも0件でした。

| 4b02dea時点のサンプル | 確保回数／解放回数 | 確保バイト数 | エラー数 |
|---|---:|---:|---:|
| example_all_features | 5097 / 5097 | 1080618 | 0 |
| example_async_concurrent_runs | 683 / 683 | 226919 | 0 |
| example_checkpoint_hitl | 1973 / 1973 | 524478 | 0 |
| example_classifier_fanout | 1696 / 1696 | 419024 | 0 |
| example_custom_graph | 799 / 799 | 231767 | 0 |
| example_intent_routing | 3960 / 3960 | 916910 | 0 |
| example_parallel_fanout | 1330 / 1330 | 364867 | 0 |
| example_plan_executor | 3616 / 3616 | 823613 | 0 |
| example_send_command | 3279 / 3279 | 747161 | 0 |
| example_state_management | 2540 / 2540 | 640311 | 0 |
| example_subgraph | 1568 / 1568 | 419423 | 0 |
| 合計 | 26541 / 26541 | 6395091 | 0 |

`Smoke` / `GraphCompiler` / `GraphState` のサブセットは、31/31のテスト、12551回の確保と解放、1890717バイトで、エラーは0件でした。これらの数値は、そのリビジョンのものです。この結果のMemcheckの対象は、テストスイート全体ではありません。

## 現在のASanとUBSanの手順

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  LSAN_OPTIONS="suppressions=$PWD/tests/lsan_suppressions.txt" \
  ctest --test-dir build-asan -E 'BIG_|valgrind' --output-on-failure
```

Pythonでは、拡張をimportする前に、対応するASanのランタイムをプリロードしてください。リーク検出をまるごと無効にする回避策ではなく、`tests/lsan_suppressions.txt` を使います。実際のインタープリターの設定は、CIのワークフローで定義されています。除外したものがあれば、結果とともに報告してください。古いPythonのテスト件数を、新しい実行にそのまま写してはいけません。

## 過去のサニタイザーとソークテストのエビデンス

2026-04-29のmaster `6bd9632` で記録されたサニタイザーの一括検査は、11個のモックのサンプルと、322/322のCTestのケースを対象とし、ASan/UBSanのエラーは0件でした。古い、デフォルトの連鎖の再帰を防ぐ修正は、この履歴に属します。現在のGraphNodeの契約では、オーバーライドするのは `run(NodeInput)` の1つだけです。

過去の、Counter/Sendを10000回実行したソークテストでは、`wall=0.68s`、`ops=14728/s`、100回目と9999回目のRSSは4608 KBでした（差は0 KB）。別に行った、1000件を同時に実行するデバッグビルドの検査では、差は128 KBでした。観測したRSSが増えていないことは、実行されなかったコードにリークの経路がないことの証明にはなりません。また、アロケーターによる再利用が、短い期間のメモリの増加を隠すこともあります。

過去の堅牢化の測定結果は、ベースラインがseq 5.1 µs／par 275.2 µs、堅牢化後がseq 5.1 µs／par 275.6 µsでした。この測定で、検出できる差がなかったことが分かるだけで、すべてのワークロードでコストがゼロであることを示すものではありません。

## CIと堅牢化の契約

[拡張CIワークフロー](../.github/workflows/ci-extended.yml)は、`sanitizer-test`、`tsan-test`、`fuzz-canary` を定義しています。これらは、プッシュのたびではなく、毎晩とオンデマンドで実行されます。ASan/LSanの下でC++のテストとサンプル、およびPythonを、TSanの下で並行処理の経路を、libFuzzerでコンパイラーの入力を、それぞれ検査します。ワークフローの定義は、通すべきゲートを示すものです。現在の依存構成がそれを通ったことの証明ではありません。

TSanとASanには、別々のビルドが必要です。Linuxの `setarch x86_64 -R` は、`ADDR_NO_RANDOMIZE` を設定してアドレスのランダム化を無効にします。それでも、権限やカーネルのメモリレイアウトによって、TSanが動かないことがあります。アドレスのサニタイズでは、レースがないことは証明できません。

`NEOGRAPH_ENABLE_HARDENING=ON` にすると、コンパイラーとプラットフォームで適格性確認されたアサーション、スタックプロテクター、FORTIFY、リンカーのフラグが有効になります。Linux x86は、設定された `-fcf-protection` オプションをサポートします。他のプラットフォームには、この主張は当てはまりません。サニタイザーのフラグがある場合と、MSVCの場合、CMakeはこれらのフラグを設定しません。CETのタグ付けは、あらゆる制御フローへの攻撃が失敗することを一般に保証するものではありません。

## 未初期化の読み取りとsuppression

MemorySanitizerには、C++標準ライブラリやランタイムライブラリを含め、計装された依存が必要です。ASan/UBSan/TSanは、未初期化の読み取りを一般的に検出する代わりにはなりません。ASanに、一般的な `detect_uninitialized_reads` オプションはありません。詳しくは[MemorySanitizerのドキュメント](https://clang.llvm.org/docs/MemorySanitizer.html)を参照してください。

[LSanのsuppression](../tests/lsan_suppressions.txt)と[TSanのsuppression](../tests/tsan_suppressions.txt)は、レポートごとに照らして確認してください。サードパーティのパターンを含み、過去のlibpqxxのエントリもあります。現在のPostgreSQLはlibpqを使っています。NeoGraph自身が持つリークやレースを、suppressionしてはいけません。suppressionは、ライブラリのフレームを経由した確保も対象にしうるため、suppressionされたレポートがあっても、所有権がすべて正しいことの証明にはなりません。
