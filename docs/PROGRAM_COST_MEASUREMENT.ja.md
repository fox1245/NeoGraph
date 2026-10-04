<!-- neograph-i18n: source=docs/PROGRAM_COST_MEASUREMENT.md locale=ja source_sha256=f22d74f413d9d1665f1444d3a29a05212e4d0ff9c515be846e5439fabe2177e1 -->
# Program コストの測定

**Languages:** [English](PROGRAM_COST_MEASUREMENT.md) | [한국어](PROGRAM_COST_MEASUREMENT.ko.md) | [日本語](PROGRAM_COST_MEASUREMENT.ja.md) | [简体中文](PROGRAM_COST_MEASUREMENT.zh-CN.md)

`bench_program_cost` と `scripts/run_program_costs.py` は、どの Program コストを最適化するかを決めるための診断 baseline を提供する。runtime semantics は変更せず、事前登録済み QuickJS performance gate も置き換えない。LLM、network model call、model credential は不要である。

実装比較用の `scripts/compare_program_costs.py` は、新しい process pair で二つの凍結済み `bench_program_cost` binary を交互に実行する。最初の比較は [Command publication head 最適化](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) に記載する。続く [canonical JSON 最適化](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md) は、AgentX disassembly と CPU sampling で native 文字列処理の変更を選ぶ。その次の [SHA-256 最適化](PROGRAM_SHA256_OPTIMIZATION.md) は、独立に検証した portable fallback と runtime 判定付き CPU 高速化を追加する。`bench_canonical_json CASE BYTES ITERATIONS` は、ASCII、Unicode、混合、escape の多い文字列を、scalar output oracle に対して別途実行する。

## Build と実行

native filesystem 上の最適化 build を使う。Linux の例:

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

runner は**新しい**出力 directory を要求する。デフォルトのマトリクスは memory と SQLite、独立した process repetition 七回、warmup invocation 十回、lifecycle ケースごとの測定 invocation 100 回を使う。`--iterations` と `--warmup` は lifecycle/direct ケースを制御し、generator microbenchmark は command 1,000 回を使う。ケースは逐次実行し、反復ごとに正順/逆順を交互にする。SQLite file は出力 directory に置き、調査用に残す。

PostgreSQL を含めるには、まず専用のローカル test container を起動し、公開された localhost 接続 URL を `NEOGRAPH_COST_POSTGRES_URL` に設定して、`--postgres-container <container-name>` を追加する。runner は公開 port を検査する。sample ごとに一意な名前の database を作り、終了後にその database だけを削除する。container の開始・停止は行わず、URL に記載した database を使うことも削除することもない。使い捨て server は、`postgres` が `docker exec` を通じて database を作成・削除できなければならない。application server を対象にしてはならない。認証情報は metadata や command record に書き込まない。

## 各測定範囲に含むもの

| ケース | 含むもの | 除外するもの |
| --- | --- | --- |
| `direct` | warm `GraphEngine::run`、逐次 increment node 三つ、state channel 二つ | Program admission、journal、checkpoint persistence、JS |
| `lifecycle --mode cpp` | 承認済み C++ builder v1 Program、実際の Core、result 構築、journal と checkpoint | JavaScript control 実行。compilation/admission は別の cold field |
| `lifecycle --mode javascript` | 承認済み `define()` + generator `main()`、実際の Core、JS/C++ conversion、command journal と checkpoint | compilation/admission は別の cold field |
| `generator` | generator open/compile/init、最初の `next`、繰り返す host-command 往復、terminal conversion、teardown | 実際の Core、scheduling、journal、database I/O。response は合成 |
| `resident` | 最初の command 後に停止した複数の独立 QuickJS runtime | 完全な Program agent、catalog、engine、journal storage |
| 既存の primitive/control ケース | `bench_quickjs_primitives` と `bench_quickjs_control` に記載済みの範囲 | これらの sample は enabled/disabled gate の実行ではない |

direct、C++、JavaScript run には、同じ三ノード topology、increment behavior、payload channel を使う。実際の各 run は counter `3` と完全で不変な payload を持って終了しなければならない。payload size は 0、4,096、65,536 byte である。JavaScript の multi-command 行は同じ Core graph を四回または 16 回実行し、毎回 counter input をゼロに戻す。最終 counter は `3` になる。

C++ v1 は Program operation を正確に一つ承認する。JavaScript generator は、すべての command-count 行に対応するため、ホスト宣言の 64-operation 上限を使う。両者は同じその他の resource ceiling と scheduler thread 一つを使う。source form と command-journaling semantics が異なるため、JavaScript から C++ を差し引いても interpreter time を分離することには**ならない**。

各 lifecycle ケースは、選択した backend から ProgramStore、CheckpointStore、ProgramTransitionStore を構築する。memory mode は実際の in-memory journal/checkpoint value を保持し、persistence interface を迂回しない。SQLite は、store ごとに独立した connection を持つ一つの database file を使う。既存 CheckpointStore は WAL と `synchronous=NORMAL` を設定し、Program store connection は SQLite synchronous のデフォルトを維持する。PostgreSQL は既存 store 実装と四 connection の checkpoint pool を使う。結果比較では実際の server setting を記録すること。これらは backend のデフォルトであり、同じ電源断保証を主張するものではない。

## 時間と memory field の読み方

`samples` はすべての測定 invocation を含み、`first_run` は warmup 完了前に別途記録する。`cold` は store 構築、Program compilation、admission、Runtime 構築を含む。これらの field は fixture setup の一部を除くため、process startup の完全な内訳ではない。

一つの逐次 invocation 内で、`before_core_us`、`core_span_us`、`after_core_us`、`wake_us` は Program event timestamp を使い、開始から wait 復帰までの wall time を分割する。**Core event 区間は純粋な Core CPU time ではない。** checkpoint 作業と、multi-command 行では Core call 間の時間を含む。`start_us` と `post_start_us` は二つ目の分割を提供する。二つの分割を足し合わせてはならない。Core 作業は `start()` が戻る前に開始する場合がある。

event sink は atomic timestamp を記録する。したがって lifecycle 結果はこの observer を含むが、direct/generator ケースには event sink がない。これらは診断時間であり、observer のない最小 latency を主張するものではない。

runner は最初に各 process を集約し、その後に独立した process 結果を集約する。`invocation_median.total_us.median` は process median の中央値である。`invocation_p95.total_us.median` は process 内 p95 の中央値である。percentile は nearest rank を使い、process sample が七つなら、process-level p95 はその最大値となる。生 sample、MAD、極値は残す。中央値の小差から統計的有意性を推定してはならない。

Linux RSS は process memory であり、JS allocation accounting ではない。lifecycle RSS の増加は、保持した run history と allocator behavior を含む。resident-generator RSS は runtime/bootstrap overhead と allocator granularity を含む。増分占有量を推定するには、より大きい generator 数の間の傾きを使う。いずれも recursive agent 一つの全 memory 測定ではない。他 platform の memory field がゼロなのは、Linux 専用 probe を利用できないことを意味する。

runner は binary/source hash、source commit、tracked diff hash、CMake option、CPU、process affinity を記録する。report とともに生証拠を保持し、測定 run 中に binary を再ビルドしてはならない。

## Linux での任意の database API 診断

`benchmarks/program_store_profile.c` は、Program runtime を変えずに database-library call の回数と時間を記録できる。主たる非 profile マトリクスが完了した**後**に、別途ビルドする。

```sh
cc -O2 -shared -fPIC -I/usr/include/postgresql \
  benchmarks/program_store_profile.c -o /tmp/program_store_profile.so -ldl -pthread
NEOGRAPH_COST_STORE_PROFILE=/tmp/new-store-profile.json \
LD_PRELOAD=/tmp/program_store_profile.so \
  build-cost/bench_program_cost --case lifecycle --backend sqlite \
  --storage /tmp/new-cost-profile.sqlite --iterations 40 --warmup 0
```

profiler は分類と集計回数・時間だけを記録し、SQL text、parameter、credential は決して記録しない。`sqlite3_step`、または同期 `PQexec` と `PQexecParams`、および transaction control を含む `sqlite3_exec` を測定する。二重計上を避けるため、`exec` 内のネストした SQLite step は除外する。SQLite row iteration は、一つの query に対して `step` を複数回呼ぶ場合がある。async libpq call、接続確立、SQLite prepare time、呼び出し元の serialization は、これらの API interval 外である。process 全体の total は constructor、schema 作成、admission、warmup を含む。profiling は overhead を追加し、同時 API interval は重なり得るため、正確な CPU 内訳として baseline wall time からこれらの値を差し引いてはならない。固定 setup call 数と run ごとの call を分けるには、invocation 数を変えて、新しい database での run を比較する。

## この baseline の境界

このマトリクスは、逐次の合成実行と停止中 generator の占有量を測定する。tenant fairness、同時 throughput、recursive spawn、live replacement、process-loss recovery、本番 storage、JIT backend は検証しない。generator replay 行は、記録済みの合成 command response を replay し、完全な永続再接続を測定しない。JIT の可能性を評価するには、warmup、compilation、memory cost を含む実際の比較が必要である。
