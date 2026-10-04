<!-- neograph-i18n: source=docs/PROGRAM_COMMAND_HEAD_OPTIMIZATION.md locale=ja source_sha256=52b5dae9fe5c514792e6ec85508f8e548faef3d737711c484cd04294fa3542ef -->
# Command publication head の最適化 — 2026-09-07

**Languages:** [English](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) | [한국어](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ko.md) | [日本語](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_COMMAND_HEAD_OPTIMIZATION.zh-CN.md)

この変更は JavaScript コマンドの公開前の読み取りを減らす。run/journal を別々に読み込み、command history 全体を実体化する代わりに、run、journal、最新 command を一緒に読む。永続公開、CAS、reservation、checkpoint、recovery の semantics は変更しない。

## 実装

- `ProgramTransitionStore::load_command_publication_head` は、一貫した run/journal の組と最新 command append を返す。memory は一つの不変 snapshot を使い、SQLite と PostgreSQL は一つの statement と既存の `(owner_scope, run_id, sequence)` primary key を使う。
- database projection は未使用の migration と last-publication bytes を省く。最新 command の sequence と coordinate を正規値と照合し、run/owner/bundle/journal の結び付きを検証する。
- 通常の append と settlement は最新 command を使う。過去の coordinate への retry は引き続き完全な history を使う。replay と store 側の append/reservation 検証は、従来どおり history を調べる。history scan を完全に除去するものではない。
- 既存 C++ store wrapper は、既存 virtual read を使い、二回目の run read で囲む fallback を得る。同時変更は安全側に失敗する。wrapper は独自の filtering と authority semantics を維持して新しい read を実装できる。
- schema migration、新しい index、cache lifetime、durability setting、graph generation、compiler identity、model configuration の変更はない。
- 公開 C++ virtual interface に method を一つ追加した。Program library と C++ consumer を一緒に再ビルドすること。既存 wrapper の source compatibility は検証対象であるが、prebuilt C++ consumer の ABI compatibility は前提にしない。

## 正しさ

WSL GCC Debug の Program suite は **680 テストに成功**し、**該当しない memory-store process-restart ケース二件を skip**した。追加した七テストと拡張した backend history テストは、不変 snapshot、独立 reader/writer connection、fallback wrapper、分断された fallback read、選択 tail の破損、上限付き tail read を対象とする。過去の破損は引き続き full-history read で拒否する。

suite 全体は、command recovery、lineage CAS、再帰 child replacement、補充不能な budget、SQLite/PostgreSQL の実際の process-loss 境界も実行した。この変更に対して Windows や sanitizer の再実行は主張しない。

## ペア Release 比較

baseline と同じ Ryzen 7 5800X / WSL2 Ubuntu 24.04 / GCC 13.3 Release 構成を使った。比較前に両 binary を凍結した。ケースごとに独立した process pair 五組を使い、各プロセスで warmup 三回と測定 invocation 12 回を実行した。ケース順と AB/BA 順を交互にする。PostgreSQL は native WSL Docker とプロセスごとの新しい database、SQLite は新しい WSL-ext4 file を使う。ペア測定中に build や他の test job は実行していない。

変更前 binary の hash は元の baseline と一致する。以下の変更前/変更後測定は、この記録を作成した同じ作業ターンで一緒に測定したものであり、過去の wall-clock 時間との比較に代わる。JIT や LLM は関与しない。

| ケース | 変更前中央値 (ms) | 変更後中央値 (ms) | ペア削減率の中央値 | ペアの変更後/変更前の範囲 |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0076 | 0.0077 | -2.71% | 1.0007–1.0418 |
| memory-cpp-0 | 2.0373 | 2.0332 | -0.58% | 0.9857–1.0336 |
| memory-javascript-0-commands-1 | 3.3096 | 3.3046 | +0.59% | 0.9780–1.0432 |
| memory-javascript-65536-commands-1 | 31.0390 | 30.7436 | +0.89% | 0.9843–1.0272 |
| memory-javascript-0-commands-16 | 32.0270 | 32.2163 | -0.13% | 0.9808–1.0261 |
| sqlite-javascript-0-commands-1 | 19.5180 | 18.9032 | +3.20% | 0.9645–1.0497 |
| sqlite-javascript-65536-commands-1 | 130.1083 | 122.0217 | +6.02% | 0.9213–0.9490 |
| sqlite-javascript-0-commands-16 | 326.5041 | 256.5506 | +20.94% | 0.7812–0.8014 |
| postgres-javascript-0-commands-1 | 81.8919 | 78.3417 | +4.34% | 0.9037–1.0263 |
| postgres-javascript-65536-commands-1 | 257.4166 | 240.3124 | +6.76% | 0.9198–0.9608 |
| postgres-javascript-0-commands-16 | 1035.1816 | 922.3063 | +10.90% | 0.8689–0.9304 |
| postgres-cpp-0 | 49.0719 | 48.2794 | +1.48% | 0.9555–1.0062 |

削減率は paired process median から計算するため、二つの独立した集約中央値の比率と一致する必要はない。ペアのばらつきの範囲内の小差からは結論を出せない。direct Core と C++ Program 行は負の対照であり、最適化した command-publication read は使わない。小 payload の memory ケースでは、明確な高速化は確認できない。

小さな direct-Core の兆候を受け、一つの広い対照検査を行った。七つの paired process で、各プロセスに warmup 100 回、測定 direct invocation 10,000 回を使った。変更後/変更前比の中央値は 1.0098、ペア範囲は 0.9825–1.0123 だった。生結果は `direct-control/` に保持する。Core source や library code は変更していない。この検査は有意な Core 性能変化を確認するものではない。

## Database API の回数

別の計装付き比較では、invocation 一回と四回を比較し、その差を二度測った。各 run 当たりの回数差は、二回の反復で全て同じだった。この計装の時間は主要比較に使わない。SQLite step call は row iteration を含み、libpq は同期呼び出しだけを対象とする。

| Backend / Program run 当たり Core call | 変更前 SQL API call | 変更後 SQL API call | 変更前 commit | 変更後 commit |
| --- | --- | --- | --- | --- |
| sqlite / 1 | 135 | 130 | 9 | 9 |
| sqlite / 16 | 2715 | 2155 | 114 | 114 |
| postgres / 1 | 108 | 104 | 4 | 4 |
| postgres / 16 | 1053 | 989 | 34 | 34 |

PostgreSQL commit 列は同期 libpq の部分集合だけであり、async checkpoint 経路の全 transaction 数ではない。両 binary は同じ persistence interface を実行し、測定範囲で同じ commit 数を維持する。

## 再現と証拠

変更前 revision（`8fce5e14`）と変更後 revision から、同一の Release option で `bench_program_cost` をビルドする。両 binary を保持する。使い捨て Postgres container を稼働させ、`NEOGRAPH_COST_POSTGRES_URL` を設定した状態で実行する。

```sh
python3 scripts/compare_program_costs.py \
  --before /tmp/before/bench_program_cost \
  --after /tmp/after/bench_program_cost \
  --output /tmp/new-command-head-comparison \
  --postgres-container neograph-n2-postgres
```

詳しい方法: [Program cost 測定](PROGRAM_COST_MEASUREMENT.md)。生の paired sample、回数、source/binary hash、verification log は `artifacts/program-command-head-20260907/` にローカル保持する。大きな SQLite file は WSL に置く。runner は出力 directory を上書きせず、既存の application database も再利用しない。

次に残る対象は、publication と replay 中の full-history validation/materialization、および canonical record 構築である。この変更は、それらのコストが除去されたことや、JIT が役立つことを示すものではない。
