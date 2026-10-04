<!-- neograph-i18n: source=docs/PROGRAM_CANONICAL_JSON_OPTIMIZATION.md locale=ja source_sha256=21e00a791dfa14a8b9748d723c10121eb2c8acdbd477978e058c932110f2d41d -->
# AgentX 分析による canonical JSON 最適化 — 2026-09-07

**Languages:** [English](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md) | [한국어](PROGRAM_CANONICAL_JSON_OPTIMIZATION.ko.md) | [日本語](PROGRAM_CANONICAL_JSON_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_CANONICAL_JSON_OPTIMIZATION.zh-CN.md)

測定した 64 KiB Program workload では、文字列 scan/copy をまとめることで、すでに最適化済みの command-head 実装（`ef56ff89`）に対し、latency が **memory store で約 54%、SQLite で約 50%、PostgreSQL で約 26%**減った。改善は native canonical JSON 処理によるものであり、この実験は JIT backend を評価しない。

## 変更を選んだ証拠

owner-local の AgentX radare2 MCP peer（connector version 1.8.6）は、content identity を使って、凍結済み Release ELF そのものを調査した。使ったのは artifact open、symbol lookup、read-only disassembly だけである。function disassembly は当初、analysis metadata の欠如を報告した。read-only service は `analyze` を公開していない。address ベースの `disassemble` は成功し、`nm` から得た ELF symbol address/size で調査範囲を限定した。

その後、Linux software CPU-clock sampler は、新たに作成した benchmark process とその thread だけを対象に動作し、kernel/hypervisor sample を除外した。周期は 1 ms、buffer は CPU ごとの inherited buffer である。これは setup、呼び出し元の verification、teardown を含むプロセス全体の leaf-IP profile であり、wall time の内訳や inclusive call graph ではない。未解決の shared-library symbol は明示的に未解決のままとする。

- 小さい memory input: 4,196 sample、損失ゼロ。
- 64 KiB memory input: 5,155 sample、損失ゼロ。`append_escaped` は sample の 25.68%、UTF-8 sequence validation は 17.58%、escaped-size calculation は 11.68% を占め、合計約 55% だった。
- AgentX disassembly は、旧 loop が `0x66a349` で UTF-8 validation を呼び、返された sequence length だけ進むことを示した。ASCII byte では毎回その長さは一だった。source inspection でも、escape-size と output に別々の bytewise loop があることを確認した。

この証拠により、推測的な VM 変更より先に文字列処理を選んだ。測定した改善は、以下の別の非 profile 比較で決定する。sampling API の参照: [Linux perf_event_open](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)。

## 変更と不変条件

`src/core/canonical_json.cpp` は現在、完全な 8-byte word に ASCII または JSON escape byte があるかを検査し、残りを scalar で処理する。unaligned word は `memcpy` で読み込み、repeated-byte mask は byte order に依存しない。UTF-8 validation は ASCII の連続部分を一度に消費し、既存の非 ASCII validity rule を維持する。文字列出力は、検証後に unescaped span を一操作で append する。

size calculation は raw-size 下限から開始し、escape による増加を加える。newline のような short escape を含め、各 control byte に保守的に六 byte を割り当てる元の規則を維持する。16 MiB limit、無効 UTF-8 の拒否、escape byte、sorted key、number encoding、owned-value behavior、content identity は変わらない。新しい dependency、architecture-specific compiler option、public API、SQL query、persistence setting は導入していない。

変更後 ELF も同じ AgentX peer で調査した。`0x668710` の `unescaped_prefix` は `mov rax, qword [r8 + rcx]` を含み、`rcx` を八ずつ進める。意図した wide-load loop が compiled code にあることを確認した。

## 正しさの検証

- 新しい canonical JSON テスト六件は、word boundary 周辺の全 ASCII byte、Unicode と escape の混在、決定的な 4,096-case Unicode corpus、ASCII prefix 後の無効 UTF-8、短い/unaligned view、保守的な materialized-size 境界を対象とする。
- 同じ六テストは、AddressSanitizer と UndefinedBehaviorSanitizer、leak detection 有効、suppression なしでも成功した。
- 対象を絞った Core テスト 211 件が二回の実行で成功した。最初に 180 件、次に専用 backend を有効にして PostgreSQL checkpoint の全 31 ケースを実行した。
- Program suite 全体: 680 件が成功し、該当しない memory-store process-restart ケース二件を skip した。canonical identity の golden、および SQLite/PostgreSQL の recovery、lineage、replacement、budget 検査を含む。
- すべての microbenchmark 出力は、独立した scalar escape oracle と一致した。すべての Program invocation は、想定 counter と完全な payload を保持した。
- 検証は WSL GCC 13.3 で行った。Windows、ARM、engine 全体の sanitizer 検証は主張しない。

## 文字列のみの benchmark

各ケースで新しい process pair 七組、warmup 20 回、プロセスごとに serialization 500 回を測定し、AB/BA とケース順を交互にした。入力は ASCII、韓国語/emoji、escape を含む混合 text、control/quote/backslash が多いケースを含む。準備と出力全体の検証は時間測定の対象外である。極小の sub-microsecond ケースは timer granularity に近く、精密な regression gate として使うべきではない。

| 入力 | 変更前中央値 (µs) | 変更後中央値 (µs) | ペア削減率の中央値 |
| --- | --- | --- | --- |
| ascii-8 | 0.060 | 0.050 | +16.67% |
| ascii-32 | 0.190 | 0.090 | +50.28% |
| unicode-32 | 0.170 | 0.110 | +35.29% |
| mixed-32 | 0.190 | 0.130 | +31.58% |
| escaped-32 | 0.241 | 0.201 | +12.61% |
| ascii-256 | 1.031 | 0.160 | +84.34% |
| unicode-256 | 0.892 | 0.361 | +59.60% |
| mixed-256 | 1.002 | 0.501 | +50.50% |
| escaped-256 | 1.212 | 0.932 | +22.73% |
| ascii-65536 | 298.398 | 87.008 | +70.91% |
| unicode-65536 | 263.603 | 136.770 | +48.33% |
| mixed-65536 | 295.183 | 169.748 | +42.37% |
| escaped-65536 | 492.026 | 416.920 | +14.27% |

## Program runtime 比較

`compare_program_costs.py` を使い、各ケースで新しい process pair 五組、warmup 三回、各プロセスで invocation 12 回を測定した。両 binary は凍結済みで、変更前 binary は先行 command-head 最適化のものである。同じ Ryzen 7 5800X、WSL ext4、repository hardening を含む Release `-O3 -DNDEBUG`、Runtime scheduler thread 一つ、native-WSL PostgreSQL 16.15 構成を使った。各 database ケースは新しい database から開始する。このペア測定中、build、profiler、他の test job は実行していない。

| ケース | 変更前中央値 (ms) | 変更後中央値 (ms) | ペア削減率の中央値 | ペア比率の範囲 |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0078 | 0.0078 | +0.83% | 0.7911–1.2164 |
| memory-cpp-0 | 2.1190 | 1.9813 | +9.03% | 0.9054–0.9640 |
| memory-javascript-0-commands-1 | 3.4078 | 3.2520 | +4.61% | 0.9378–0.9557 |
| memory-javascript-65536-commands-1 | 32.0098 | 14.9131 | +54.19% | 0.4524–0.4727 |
| memory-javascript-0-commands-16 | 32.9070 | 31.2267 | +5.71% | 0.8915–0.9682 |
| sqlite-javascript-0-commands-1 | 19.9449 | 19.9057 | +0.47% | 0.9158–1.0023 |
| sqlite-javascript-65536-commands-1 | 127.6179 | 64.3529 | +50.19% | 0.4935–0.5101 |
| sqlite-javascript-0-commands-16 | 272.5562 | 259.2726 | +4.87% | 0.9361–0.9640 |
| postgres-javascript-0-commands-1 | 81.3888 | 80.5189 | +1.71% | 0.9621–1.0215 |
| postgres-javascript-65536-commands-1 | 246.5232 | 184.0142 | +25.66% | 0.7035–0.7682 |
| postgres-javascript-0-commands-16 | 973.4043 | 945.9677 | +2.14% | 0.9462–1.0037 |
| postgres-cpp-0 | 51.8192 | 50.9214 | +1.73% | 0.8937–0.9983 |

百分率は paired ratio を集約するため、二つの独立した中央値列の比率と一致する必要はない。微小な direct-Core 時間と小さな database ケースは相対的なばらつきが大きい。大入力の改善は五組すべてで一貫していた。これは synthetic engine overhead であり、chatbot/model 応答時間や全 workload の普遍的な高速化ではない。

## 残る対象

追加の 64 KiB CPU profile は、損失ゼロで 2,317 sample を収集した。SHA-256 identity calculation は sample の約 35.17%、unescaped-span scanning は 10.96%、UTF-8 validation は 1.90% だった。hash 割合の増加は全体の作業量が減ったことを反映し、hashing が遅くなった証拠ではない。未解決のコストすべてを allocation や copying に帰属させる前に、shared-library symbol の帰属を改善する必要がある。

## 再現と対象範囲

`ef56ff89` と変更後 revision で、同一の Release option を使って `bench_canonical_json` と `bench_program_cost` をビルドする。新しい microbenchmark source は、変更前 library を変えずに、それに対してビルドできる。micro command の例:

```sh
bench_canonical_json unicode 65536 500
```

end-to-end ケースには[ペア Program 比較](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)を使う。生の RPC request/response、tool schema、ELF/source hash、CPU sample/map、helper source、micro/runtime result、test log は、`artifacts/program-canonical-agentx-20260907/` にローカル保持する。完全な read-only 入力 ELF は、割り当てた AgentX artifact root 内の digest 由来 directory に残す。証拠に認証情報は含めない。

この作業では、owner が認可したホストから AgentX の analysis peer を直接使った。AgentX model WorkOrder の起動、新しい ToolBindingSet の公開、AgentX Analysis Graph への record import は行っていない。それらの workflow/provenance 保証は主張しない。AgentX service configuration や他の application data は変更していない。
