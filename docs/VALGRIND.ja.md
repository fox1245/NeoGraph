<!-- neograph-i18n: source=docs/VALGRIND.md locale=ja source_sha256=21a939b18bdcef34c5ead883275d4932d5f9eeec6c25401be1dfd8836025e75f -->
# メモリとサニタイザーの検査

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 対象範囲と前提条件

不正なメモリアクセスとメモリリークの検出にはMemcheck、アドレスのエラーにはASan、対象範囲内の未定義動作にはUBSan、データ競合にはTSanを使います。1つのツールを通ったからといって、別のツールが調べる性質を証明したことにはなりません。

現在のCoreのビルドには、LLMノードとPostgreSQLを無効にしても、外部の `SchemaProvider::runtime` が必要です。`SDK_PREFIX` に、対応するインストール済みのSDKを設定してください。または、`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` とその開発用の依存を用意します。SDKの内部をサニタイザーで調べるには、そのSDKも、対応する計装（`SP_SANITIZE=address` または `thread`）でビルドします。

NeoGraphにはCMake 3.20以降が必要です。ソースの選択順は、明示したSDKのソースディレクトリ、インストール済みのパッケージ、デフォルトで取得されるリビジョン固定の公開archiveの順です。インストール済みのSDKをオフラインで使う場合は、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を設定します。ソースから、または取得してビルドする場合は、NeoGraphのAsioとyyjsonを使います。システムのlibcurlとOpenSSLの開発用ファイルは、引き続き必要です。

## Memcheckのシナリオを実行する

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

再ビルドした成果物で、ネットワークを使わない関連するサンプルと、プロバイダーの寿命やキャンセルのシナリオを実行します。ネットワークとPythonの経路には、範囲を絞った診断と、レビュー済みのサードパーティ向けsuppressionが必要です。これらをメモリ検査の対象から外すことはできません。モックプロバイダーで実行しても、SDKのトランスポートや、本物のネイティブリプレイの保管は検査できません。

## ASanとUBSanの手順

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

Pythonでは、拡張をimportする前に、対応するASanのランタイムをプリロードしてください。リーク検出をまるごと無効にする回避策ではなく、`tests/lsan_suppressions.txt` を使います。実際のインタープリターの設定は、CIのワークフローで定義されています。除外したものがあれば、結果とともに報告してください。

## CIと堅牢化の契約

[拡張CIワークフロー](../.github/workflows/ci-extended.yml)は、`sanitizer-test`、`tsan-test`、`fuzz-canary` を定義しています。これらは、プッシュのたびではなく、毎晩とオンデマンドで実行されます。ASan/LSanの下でC++のテストとサンプル、およびPythonを、TSanの下で並行処理の経路を、libFuzzerでコンパイラーの入力を、それぞれ検査します。

TSanとASanには、別々のビルドが必要です。Linuxの `setarch x86_64 -R` は、`ADDR_NO_RANDOMIZE` を設定してアドレスのランダム化を無効にします。それでも、権限やカーネルのメモリレイアウトによって、TSanが動かないことがあります。アドレスのサニタイズでは、レースがないことは証明できません。

`NEOGRAPH_ENABLE_HARDENING=ON` にすると、コンパイラーとプラットフォームで適格性確認されたアサーション、スタックプロテクター、FORTIFY、リンカーのフラグが有効になります。Linux x86は、設定された `-fcf-protection` オプションをサポートします。他のプラットフォームには、この主張は当てはまりません。サニタイザーのフラグがある場合と、MSVCの場合、CMakeはこれらのフラグを設定しません。CETのタグ付けは、あらゆる制御フローへの攻撃が失敗することを一般に保証するものではありません。

ある測定では、堅牢化のコストは、ベースラインがseq 5.1 µs／par 275.2 µs、堅牢化後がseq 5.1 µs／par 275.6 µsでした。この測定で、検出できる差がなかったことが分かるだけで、すべてのワークロードでコストがゼロであることを示すものではありません。

## 未初期化の読み取りとsuppression

MemorySanitizerには、C++標準ライブラリやランタイムライブラリを含め、計装された依存が必要です。ASan/UBSan/TSanは、未初期化の読み取りを一般的に検出する代わりにはなりません。ASanに、一般的な `detect_uninitialized_reads` オプションはありません。詳しくは[MemorySanitizerのドキュメント](https://clang.llvm.org/docs/MemorySanitizer.html)を参照してください。

[LSanのsuppression](../tests/lsan_suppressions.txt)と[TSanのsuppression](../tests/tsan_suppressions.txt)は、レポートごとに照らして確認してください。サードパーティのパターンを含み、過去のlibpqxxのエントリもあります。現在のPostgreSQLはlibpqを使っています。NeoGraph自身が持つリークやレースを、suppressionしてはいけません。suppressionは、ライブラリのフレームを経由した確保も対象にしうるため、suppressionされたレポートがあっても、所有権がすべて正しいことの証明にはなりません。
