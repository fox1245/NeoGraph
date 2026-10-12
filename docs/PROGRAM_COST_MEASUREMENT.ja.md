<!-- neograph-i18n: source=docs/PROGRAM_COST_MEASUREMENT.md locale=ja source_sha256=95fe571a9d490d4a096a313377958d64fd9c63a35f4ee437d8d8d33876a18799 -->
# Programのコストの測定

**Languages:** [English](PROGRAM_COST_MEASUREMENT.md) | [한국어](PROGRAM_COST_MEASUREMENT.ko.md) | [日本語](PROGRAM_COST_MEASUREMENT.ja.md) | [简体中文](PROGRAM_COST_MEASUREMENT.zh-CN.md)

`bench_program_cost` と `scripts/run_program_costs.py` は、Programのどのコストを最適化するかを決めるための、診断用のベースラインを提供します。ランタイムの意味は変わらず、事前に登録されたQuickJSの性能ゲートの代わりにもなりません。LLM、ネットワーク越しのモデル呼び出し、モデルの認証情報は、必要ありません。

実装どうしを比較するには、`scripts/compare_program_costs.py` が、固定した2つの `bench_program_cost` のバイナリを、新しいプロセスのペアで交互に実行します。`bench_canonical_json CASE BYTES ITERATIONS` は、ASCII、Unicode、混在、エスケープの多い文字列を、スカラーな出力のオラクルと照らして、別に確認します。

## ビルドと実行

最適化したビルドを、ネイティブなファイルシステム上で使います。たとえば、Linuxでは次のようにします。

```sh
cmake -S . -B build-cost -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON
cmake --build build-cost -j4 --target \
  bench_program_cost bench_quickjs_primitives bench_quickjs_control
python3 scripts/run_program_costs.py \
  --build-dir build-cost --output-dir /tmp/neograph-cost-results
```

ランナーには、**新しい**出力ディレクトリが必要です。デフォルトのマトリクスは、メモリとSQLiteを使い、独立したプロセスの繰り返しを7回、ウォームアップの呼び出しを10回、ライフサイクルのケースごとに計測する呼び出しを100回行います。`--iterations` と `--warmup` は、ライフサイクルとdirectのケースを制御します。ジェネレーターのマイクロベンチマークは、コマンド1,000個を使います。ケースは、繰り返しごとに順方向と逆方向を交互にして、直列に実行されます。SQLiteのファイルは、出力ディレクトリに作られ、調査のために残ります。

PostgreSQLを含めるには、まず専用のローカルのテスト用コンテナーを起動し、`NEOGRAPH_COST_POSTGRES_URL` に、そのコンテナーが公開しているlocalhostの接続URLを設定して、`--postgres-container <container-name>` を追加します。ランナーは、公開されたポートを確認します。サンプルごとに一意な名前のデータベースを作り、終わった後でそのデータベースだけを削除します。コンテナーの起動や停止は行いません。URLに書かれたデータベースを使うことも、削除することもありません。使い捨てのサーバーは、`docker exec` を通じて、`postgres` がデータベースを作成・削除することを許可している必要があります。アプリケーションのサーバーを対象にしないでください。認証情報は、メタデータにもコマンドの記録にも書き込まれません。

## 各スコープに含まれるもの

| ケース | 含まれるもの | 含まれないもの |
| --- | --- | --- |
| `direct` | ウォーム状態の `GraphEngine::run`、直列のインクリメントノード3つ、状態チャネル2つ | Programのadmission、ジャーナル、チェックポイントの永続化、JS |
| `lifecycle --mode cpp` | admission済みのC++ビルダーv1のProgram、実際のCore、結果の構築、ジャーナルとチェックポイント | JavaScriptによる制御の実行。コンパイルとadmissionは、コールドの別のフィールドです |
| `lifecycle --mode javascript` | admission済みの `define()` とジェネレーターの `main()`、実際のCore、JSとC++の変換、コマンドジャーナルとチェックポイント | コンパイルとadmissionは、コールドの別のフィールドです |
| `generator` | ジェネレーターのオープン／コンパイル／初期化、最初の `next`、ホストコマンドの往復の繰り返し、最終的な変換、後始末 | 実際のCore、スケジューリング、ジャーナル、データベースI/O。応答は合成したものです |
| `resident` | 最初のコマンドの後で一時停止した、独立した複数のQuickJSランタイム | 完全なProgramのエージェント、カタログ、エンジン、ジャーナルのストレージ |
| 既存のプリミティブとcontrolのケース | `bench_quickjs_primitives` と `bench_quickjs_control` にすでに記載されているスコープ | これらのサンプルは、有効／無効のゲートを比較した実行ではありません |

directの実行、C++の実行、JavaScriptの実行では、同じ3ノードのトポロジー、インクリメントの動作、ペイロードのチャネルを使います。実際の実行は、いずれも、カウンターが `3` で、ペイロードがそのまま変わらずに終わらなければなりません。ペイロードのサイズは、0、4,096、65,536バイトです。JavaScriptの複数コマンドの行は、同じCoreグラフを4回または16回実行し、そのたびにカウンターの入力を0にリセットします。最後は、カウンターが `3` で終わります。

C++ v1がadmissionするProgramのオペレーションは、ちょうど1つです。JavaScriptのジェネレーターは、すべてのコマンド数の行に対応するため、ホストが宣言した64オペレーションの上限を使います。どちらも、残りのリソースの上限は同じで、スケジューラースレッドは1つです。ソースの形式と、コマンドをジャーナルに記録する意味が異なるため、JavaScriptからC++を引いても、インタープリターの時間は取り出せ**ません**。

ライフサイクルのケースごとに、選んだバックエンドから、ProgramStore、CheckpointStore、ProgramTransitionStoreを構築します。メモリモードでも、実際のインメモリのジャーナルとチェックポイントの値を保持し、永続化のインターフェースを迂回することはありません。SQLiteは、独立したストア接続を持つ、1つのデータベースファイルを使います。既存のCheckpointStoreはWALと `synchronous=NORMAL` を設定します。Programストアの接続は、SQLiteのsynchronousのデフォルトのままです。PostgreSQLは、既存のストアの実装と、4接続のチェックポイントプールを使います。結果を比較するときは、実際のサーバー設定を記録してください。これらはバックエンドのデフォルトであり、電源断に対する保証が同一であるという主張ではありません。

## 時間とメモリのフィールドの読み方

`samples` には、計測したすべての呼び出しが入ります。`first_run` は、ウォームアップが終わる前に、別に記録されます。`cold` には、ストアの構築、Programのコンパイル、admission、Runtimeの構築が含まれます。これらのフィールドは、フィクスチャのセットアップの一部を含まないため、プロセスの起動時間の完全な内訳ではありません。

直列な1回の呼び出しの中で、`before_core_us`、`core_span_us`、`after_core_us`、`wake_us` は、Programのイベントのタイムスタンプを使って、開始から待機が戻るまでの実時間を分割します。**Coreのイベント区間は、純粋なCoreのCPU時間ではありません。** チェックポイントの処理が含まれ、複数コマンドの行では、Coreの呼び出しの間の時間も含まれます。`start_us` と `post_start_us` は、もう1つの分割を提供します。2つの分割を足し合わせてはいけません。`start()` が戻る前に、Coreの処理が始まることがあるためです。

イベントシンクは、アトミックなタイムスタンプを記録します。そのため、ライフサイクルの結果にはこのオブザーバーの影響が含まれ、direct／generatorのケースにはイベントシンクがありません。これらは診断用の時間であり、オブザーバーのない場合の最小のレイテンシーを示すものではありません。

ランナーは、まずプロセスごとに集計し、次に、それらの独立したプロセスの結果を集計します。`invocation_median.total_us.median` は、プロセスごとの中央値の中央値です。`invocation_p95.total_us.median` は、プロセス内のp95の中央値です。パーセンタイルには、nearest rank法を使います。プロセスのサンプルが7つのとき、プロセスレベルのp95は、その最大値になります。生のサンプル、MAD、極値も参照できます。中央値の小さな差から、統計的な有意性を推測してはいけません。

LinuxのRSSは、プロセスのメモリであり、JSのメモリ確保の会計ではありません。ライフサイクルのRSSの増加には、保持されている実行履歴とアロケーターの挙動が含まれます。常駐するジェネレーターのRSSには、ランタイムの初期化のオーバーヘッドと、アロケーターの粒度が含まれます。増分の占有量を見積もるには、ジェネレーターの数が多い範囲での傾きを使ってください。どちらも、再帰的なエージェント1つあたりの総メモリの測定ではありません。他のプラットフォームでメモリのフィールドが0になっているのは、Linux専用のプローブが使えないという意味です。

ランナーは、バイナリとソースのハッシュ、ソースのコミットと追跡対象の差分のハッシュ、CMakeのオプション、CPUとプロセスのアフィニティを記録します。報告には、生のエビデンスを添えて保管してください。測定の実行中に、バイナリをビルドし直してはいけません。

## Linuxでの、データベースAPIのオプションの診断

`benchmarks/program_store_profile.c` は、Programのランタイムを変更せずに、データベースライブラリの呼び出し回数と時間を計測できます。プロファイラーを使わない主要なマトリクスが**終わった後で**、これを別にビルドします。

```sh
cc -O2 -shared -fPIC -I/usr/include/postgresql \
  benchmarks/program_store_profile.c -o /tmp/program_store_profile.so -ldl -pthread
NEOGRAPH_COST_STORE_PROFILE=/tmp/new-store-profile.json \
LD_PRELOAD=/tmp/program_store_profile.so \
  build-cost/bench_program_cost --case lifecycle --backend sqlite \
  --storage /tmp/new-cost-profile.sqlite --iterations 40 --warmup 0
```

プロファイラーが記録するのは、カテゴリと、回数・時間の集計だけです。SQLの文字列、パラメーター、認証情報は記録しません。計測するのは、`sqlite3_step`、または同期の `PQexec` と `PQexecParams`、そして `sqlite3_exec`（トランザクション制御を含む）です。二重に数えないように、`exec` の内側でのSQLiteの入れ子の step は除外します。SQLiteでは、1つのクエリの行を反復するときに、`step` が複数回呼ばれることがあります。非同期のlibpqの呼び出し、接続の確立、SQLiteのprepareの時間、呼び出し側のシリアライズは、これらのAPIの区間の外にあります。プロセス全体の合計には、コンストラクター、スキーマの作成、admission、ウォームアップが含まれます。プロファイリングはオーバーヘッドを加え、並行するAPIの区間は重なりうるため、これらの値を、CPUの正確な内訳として、ベースラインの実時間から引いてはいけません。固定のセットアップの呼び出し回数と、実行ごとの呼び出し回数を分けるには、呼び出し回数を変えて、新しいデータベースでの実行を比較してください。

## このベースラインの境界

このマトリクスが測定するのは、直列に実行した合成のワークロードと、一時停止したジェネレーターが占める量です。テナント間の公平性、並行スループット、再帰的なspawn、ライブな置き換え、プロセス喪失からの復旧、本番のストレージ、JITバックエンドについては、適格性確認をしていません。ジェネレーターのリプレイの行は、記録した合成のコマンド応答をリプレイするもので、永続的な再接続全体の時間を測るものではありません。JITの可能性を判断するには、ウォームアップ、コンパイル、メモリのコストを含めた、実際の比較が必要です。
