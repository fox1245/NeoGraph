<!-- neograph-i18n: source=docs/PROGRAM_COST_BASELINE_2026-09-07.md locale=ja source_sha256=a0929a5678214c2e42de563f8e5eb1d0137f06092e7ca01d27aa3a727b351339 -->
# Program コストの baseline — 2026-09-07

**Languages:** [English](PROGRAM_COST_BASELINE_2026-09-07.md) | [한국어](PROGRAM_COST_BASELINE_2026-09-07.ko.md) | [日本語](PROGRAM_COST_BASELINE_2026-09-07.ja.md) | [简体中文](PROGRAM_COST_BASELINE_2026-09-07.zh-CN.md)

小 payload の baseline は、Program の管理処理と永続ストア処理を最初の最適化対象として示す。JavaScript の解釈が支配的なコストだとは確認しておらず、JIT の高速化も測定していない。

## 環境と方法

- ベースの source: `319f8748c986a2f345ae6de88abb42d89eef9837` と、`metadata.json` の source/binary hash に記録した測定専用の追加。
- AMD Ryzen 7 5800X、WSL2 Ubuntu 24.04、利用可能な logical CPU は 16。GCC 13.3、Release `-O3 -DNDEBUG`、repository hardening 有効。
- binary と SQLite database は WSL ext4 filesystem 上に置く。PostgreSQL 16.15 は native WSL Docker 内でローカル Docker volume を使い、localhost TCP で接続する。`fsync`、`synchronous_commit`、`full_page_writes` は有効、WAL sync method は `fdatasync`。
- ケースごとに新しいプロセス七つを使い、各 lifecycle/direct process で warmup 十回と測定 invocation 四十回を実行した。Runtime scheduler thread は一つで、同時 test job や model call はない。この desktop/VM は、分離された bare-metal benchmark host ではない。
- 成功した process sample は 308、ケースは 44。実際のすべての invocation で counter と payload 出力を検査した。ケース順は反復ごとに正順/逆順を交互にする。
- 主要値は七つの process median の中央値である。以下の p95 は、七つの process 内 nearest-rank p95 の中央値であり、統計的に独立な pooled p95 ではない。
- 生 metadata、case、sample、summary は `artifacts/program-costs-20260907/` にローカル保持する。SQLite database は WSL 内の `/tmp/neograph-program-costs-20260907/` に残し、database file は commit しない。

[測定の定義と再現](PROGRAM_COST_MEASUREMENT.md)を参照。この診断マトリクスは、既存 QuickJS acceptance gate を置き換えない。

## Core 一回の呼び出し、空 payload

| 経路 | 中央値 (ms) | プロセス内 p95 (ms) | プロセス間中央値の MAD (ms) |
| --- | --- | --- | --- |
| Core 直接、checkpoint なし | 0.0074 | 0.0076 | 0.0000 |
| C++ Program / memory | 2.0031 | 2.2607 | 0.0355 |
| JavaScript Program / memory | 3.3192 | 3.4898 | 0.0196 |
| C++ Program / SQLite | 8.5885 | 13.5360 | 0.1155 |
| JavaScript Program / SQLite | 16.4288 | 20.9731 | 0.0253 |
| C++ Program / PostgreSQL | 49.0355 | 51.2952 | 0.5896 |
| JavaScript Program / PostgreSQL | 80.1248 | 82.8138 | 0.5005 |

direct Core 行は Program lifecycle と checkpoint semantics を除く。Program との比率は全処理範囲の比較であり、同等の永続性を持つ engine の比較や regression の証拠ではない。C++ Program も実際の journal/checkpoint 処理を行う。JS–C++ 差には interpreter だけでなく、追加の command journaling と conversion も含む。

## Payload サイズの影響

| 経路 | 0 bytes (ms) | 4 KiB (ms) | 64 KiB (ms) |
| --- | --- | --- | --- |
| Core 直接 | 0.0074 | 0.0079 | 0.1308 |
| C++ Program / memory | 2.0031 | 2.8567 | 12.2975 |
| JavaScript Program / memory | 3.3192 | 5.2922 | 30.8116 |
| JavaScript Program / SQLite | 16.4288 | 23.9060 | 130.6772 |
| JavaScript Program / PostgreSQL | 80.1248 | 93.5064 | 252.1259 |

payload は実際の channel state を通じて渡し、不変であることを検査する。model output や模擬 network delay ではない。

## Generator と bridge のコスト

| Payload | Open (µs) | 最初の command (µs) | warm command 往復 (µs) | 終端 next (µs) | Close (µs) |
| --- | --- | --- | --- | --- | --- |
| 0 | 330.34 | 45.65 | 26.66 | 5.73 | 35.85 |
| 4096 | 349.72 | 171.95 | 153.32 | 30.99 | 39.99 |
| 65536 | 811.60 | 2038.30 | 2088.19 | 423.18 | 41.01 |

これらの分離した呼び出しは、合成した Core response を使う。warm 往復時間は native JSON serialization/parsing、host command 作成、JS 実行を含む。純粋な bytecode 実行時間ではなく、lifecycle 行から直接差し引けない。大 payload は、無視できないデータ変換コストを示す。

## Cold 準備

| Mode/backend | Store open (ms) | Compile (ms) | Admit (ms) | Runtime 作成 (ms) | 初回 run (ms) |
| --- | --- | --- | --- | --- | --- |
| memory-cpp-0 | 0.0054 | 0.6768 | 1.0492 | 0.1405 | 4.4991 |
| memory-javascript-0 | 0.0054 | 1.2498 | 1.2180 | 0.1420 | 5.9273 |
| sqlite-javascript-0 | 20.0656 | 1.3064 | 2.9385 | 0.1705 | 25.2293 |
| postgres-javascript-0 | 169.2527 | 1.2941 | 5.9809 | 0.1714 | 98.6726 |

compilation/admission はプロセスごとに一度、warm invocation の時間測定外で行う。generator open は、新しく開始する JS Program ごとに再び行う。これらの cold field は fixture setup の一部を省き、live replacement や migration は測定しない。

## Event marker による分割

この表だけは、各セルが全測定 invocation の平均である。そのため、四つの marker interval の合計は、丸めを除いて平均 total と一致する。最初と最後の Core event の間には checkpoint 作業が含まれ、純粋な Core CPU time ではない。

| Backend (JavaScript, 0 bytes) | 最初の Core event より前 (ms) | Core event の区間 (ms) | 最後の Core event より後 (ms) | 終端から wait 復帰まで (ms) |
| --- | --- | --- | --- | --- |
| memory | 1.1931 | 0.4062 | 1.7080 | 0.0340 |
| sqlite | 7.0816 | 1.2735 | 8.7646 | 0.0353 |
| postgres | 29.8013 | 9.1486 | 41.4887 | 0.0357 |

## 一つの generator 内での複数 Core call

| Backend | 1 call/run (ms) | 4 calls/run (ms) | 16 calls/run (ms) |
| --- | --- | --- | --- |
| memory | 3.319 | 9.041 | 31.596 |
| sqlite | 16.429 | 55.527 | 331.089 |
| postgres | 80.125 | 244.105 | 1034.104 |

実際の journal と checkpoint を伴う逐次呼び出しである。call 数で割れば run の開始・終了を償却できるが、dispatch cost を分離することにはならない。

## 停止中 generator の memory と replay

| 停止中 generator 数 | RSS 増加の中央値 (MiB) |
| --- | --- |
| 1 | 1.285 |
| 32 | 6.309 |
| 128 | 21.934 |

| replay した合成記録 command 数 | 新規 open + replay (ms) |
| --- | --- |
| 0 | 0.1276 |
| 10 | 0.5526 |
| 100 | 3.4148 |
| 1000 | 33.4788 |

上の memory は、bootstrap/allocator の影響を含む、独立した停止中 QuickJS runtime のものである。完全な agent、catalog、Core engine、永続履歴は含まない。replay は ProgramRuntime scheduling と journal I/O を除き、process-loss recovery latency ではない。

## 補足 database API profile

baseline 後の追加 72 run では、全 backend の両 control mode に対し、profiling on/off、invocation 一回と 21 回、三反復を比較した。表では `(21-run process total − 1-run process total) / 20` を使い、その後に反復間の中央値を取る。call-count 差は三反復すべてで同一だった。時間差は、cold process の変動の影響を受ける推定値のままである。

| 経路 | SQLite step calls/run | SQLite exec calls/run | SQLite commits/run | 同期 libpq calls/run |
| --- | --- | --- | --- | --- |
| memory-cpp | 0 | 0 | 0 | 0 |
| memory-javascript | 0 | 0 | 0 | 0 |
| sqlite-cpp | 73 | 14 | 7 | 0 |
| sqlite-javascript | 117 | 18 | 9 | 0 |
| postgres-cpp | 0 | 0 | 0 | 64 |
| postgres-javascript | 0 | 0 | 0 | 108 |

SQLite step は row ごとに繰り返す場合があり、一意な SQL statement 数ではない。`exec` は transaction control を含み、ネストした step は除外する。libpq の回数・時間は同期 `PQexec`/`PQexecParams` だけを対象とし、async checkpoint 経路と接続確立を除く。client CPU は PostgreSQL server を含まない。API wall interval と client CPU は重なり、足し合わせたり、baseline latency の完全な内訳として扱ったりしてはならない。

memory 対照では database call がゼロと記録された。JS SQLite ケースでは、invocation ごとに commit 九回と外部 step call 117 回が発生する。時間測定した database call 外の CPU 作業も引き続き profile 対象である。JS PostgreSQL ケースは invocation ごとに同期 call 108 回を加えるため、JIT 実験の前に往復回数が具体的な対象となる。

補足証拠は `artifacts/program-costs-20260907/profile/` にある。最初の profiler は SQLite `exec` transaction call を省いており、その暫定出力は使わなかった。修正版 profile は file copy が停止してから再実行した。ただし、その後の profile **有効・無効の両方**の run は、memory 対照を含め主 baseline より明らかに遅かった。この環境変動の原因は特定していない。そのため、ここで検証済みとして扱うのは安定した call count だけである。生の API/CPU 時間は保持するが、**主 baseline latency の帰属には使わない**。主 baseline data は copy 問題より前のものであり、七反復すべてとそのばらつきを保持している。

既存の `bench_program` も、新しいプロセス三つで C++ Program 経路に 2.21–2.31 ms を示した（Core: 6.41–6.65 µs）。この fixture は channel 一つで bounds も異なるため、millisecond 規模の全体時間を照合するものであり、主マトリクスの条件を合わせた代替ではない。

## 次の測定と最適化順序

1. Program の開始・終了と publication serialization を profile する。小 payload のコストは in-memory store と C++ control でも無視できず、JS engine の置換だけでは全体を対処できない。
2. 測定した SQLite/libpq call count を使い、同じ journal、owner、authority、budget、atomicity の保証を維持できる箇所の重複 read と往復を調べる。永続 commit を無効にして良い数値を得てはならない。
3. payload sweep を使い、繰り返される canonical JSON conversion と copying を調べる。compiled-source reuse は warm generator 実行と別に benchmark する。
4. cold-start cost と現実的な agent 数での memory を含め、実際の JS CPU 作業を分離してから JIT backend を比較する。

完全な CPU 帰属、複数 tenant の同時 throughput/fairness、recursive spawn、live replacement、実際の JIT 比較は未測定である。この baseline のために本番 runtime の最適化は行っていない。
