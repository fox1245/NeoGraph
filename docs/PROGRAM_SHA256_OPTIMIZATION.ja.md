<!-- neograph-i18n: source=docs/PROGRAM_SHA256_OPTIMIZATION.md locale=ja source_sha256=7e4fa7e81f456ac2c8027d75d34ccf3f85a9cbc18acbd7cef91f1282558b981a -->
# SHA-256 の CPU 高速化 — 2026-09-07

**Languages:** [English](PROGRAM_SHA256_OPTIMIZATION.md) | [한국어](PROGRAM_SHA256_OPTIMIZATION.ko.md) | [日本語](PROGRAM_SHA256_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_SHA256_OPTIMIZATION.zh-CN.md)

実行時判定で使う x86 SHA 命令により、64 KiB の identity hash の測定コストは約 **6.2 倍**高速になった。先行する canonical JSON 最適化（`779be451`）と比べ、大入力ケースのペア測定では、Program latency が **memory store で約 28%、SQLite で約 20%、PostgreSQL で約 8%**改善した。小入力の database ケースでは、有意な改善を確認できなかった。

## 実装と互換性

元の scalar SHA-256 圧縮と padding を `canonical_json.cpp` から内部 `sha256.cpp` module へ移した。独立した非インライン x86-64 関数が、SHA 命令で round と message schedule を処理する。register layout は、出典を明記した public-domain の [SHA-Intrinsics 参照実装](https://github.com/noloader/SHA-Intrinsics/blob/d03795497f3e4576083fc2cd8fe0b924f24d0bb2/sha256-x86.c)に従う。実装は四つの vector を循環させる schedule を使う。元の scalar 実装も利用可能なままである。

自動 dispatch は、CPUID basic leaf の存在、SSSE3、SSE4.1、SHA 対応を検査してから高速化関数を選ぶ。GCC/Clang の命令有効化はその関数に限定し、global architecture flag は使わない。MSVC も、同じ runtime check の下で同じ intrinsic をコンパイルする。他の architecture と `NEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF` の build は portable 経路を選ぶ。追加の library dependency は導入していない。

非公開 backend selector は、プロセス全体の挙動を変えずに portable/hardware を強制する正しさのテストを可能にする。installed SDK には含まれない。hardware を明示的に選び、利用できない場合は例外を投げる。digest state は呼び出しごとにローカルであり、CPU の利用可否と選択した function pointer だけをキャッシュする。

identity framing は変わらない。`preamble || NUL || domain || NUL || decimal(payload byte length) || NUL || payload` である。padding、全 256 digest bit、小文字 hexadecimal、`sha256:` prefix を保持する。既存の input-buffer 構築も維持する。この変更は streaming、hash caching、弱い checksum、新しい storage format、異なる authority/budget 規則を導入しない。

命令 semantics の参照: [Intel SHA extensions](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sha-extensions.html)。実行環境の Ryzen は必要な x86 instruction bit を公開するため、Intel vendor だけに限定する dispatch ではない。

## 検証

- 空入力、`abc`、56-byte vector、百万個の `a` byte を含む、標準 SHA-256 既知解。
- binary payload、padding 境界、unaligned input view、identity framing、埋め込み NUL に対する、独立した Python-hashlib vector 35 件。
- portable、automatic、hardware 出力を比較する決定的な 512-input corpus、および独立 hash の同時実行。
- CPU-feature predicate テストは、leaf の欠如や必須 instruction bit のいずれかの欠如を拒否する。
- Linux ASan/UBSan: 通常 build と高速化無効 build の両方で SHA テスト全六件が成功した。leak detection を有効にし、suppression は使っていない。
- Windows MSVC x64: hardware 利用可能時と高速化をコンパイルから除外した場合の両方で、独立 vector 35 件が成功した。これは native backend の正しさの検証であり、Windows Program suite 全体や性能の主張ではない。
- 対象を絞った Core suite: PostgreSQL checkpoint と identity consumer を含め 217 件が成功した。
- Program suite 全体: 680 件が成功し、memory store には該当しない process-restart ケース二件を skip した。保存 identity の golden、SQLite/PostgreSQL 復旧、replacement、lineage、budget は引き続き対象となっている。

## 独立した identity-hash 比較

入力サイズごとに新しい process pair 七組、warmup 20 回、各プロセスで hash 500 回を測定し、1 MiB では 100 回に減らした。AB/BA とケース順を交互にする。benchmark には変更していない identity framing と hex encoding を含む。返されたすべての digest を Python hashlib で独立に検査する。以下の時間は process median の中央値であり、削減率は paired ratio の中央値を使う。

| Payload bytes | 変更前 (µs) | 変更後 (µs) | ペア削減率 |
| --- | --- | --- | --- |
| 0 | 0.391 | 0.240 | +38.62% |
| 32 | 0.591 | 0.291 | +50.76% |
| 55 | 0.721 | 0.281 | +61.03% |
| 56 | 0.591 | 0.290 | +50.96% |
| 63 | 0.591 | 0.280 | +53.36% |
| 64 | 0.591 | 0.281 | +52.45% |
| 128 | 0.782 | 0.311 | +59.26% |
| 1024 | 4.218 | 0.731 | +82.67% |
| 65536 | 190.305 | 30.727 | +83.79% |
| 1048576 | 3367.703 | 507.140 | +84.14% |

## Program 比較

同じ Ryzen 7 5800X、WSL2 Ubuntu 24.04、repository hardening を含む GCC 13.3 Release `-O3 -DNDEBUG`、Runtime scheduler thread 一つ、native-WSL PostgreSQL 16.15 構成を使った。両 binary は凍結済みである。各ケースで process pair 五組、プロセスごとに warmup 三回と測定呼び出し 12 回を行い、各プロセスに新しい SQLite/PostgreSQL database を用いた。このペア時間測定中に build、profiler、他のテストは実行していない。

| ケース | 変更前 (ms) | 変更後 (ms) | ペア削減率 | ペアの変更後/変更前の範囲 |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0081 | 0.0129 | -3.22% | 0.5786–1.6575 |
| memory-cpp-0 | 2.0560 | 1.9606 | +4.64% | 0.8751–0.9940 |
| memory-javascript-0-commands-1 | 3.2981 | 3.1661 | +3.73% | 0.9093–1.1659 |
| memory-javascript-65536-commands-1 | 15.2129 | 10.9126 | +28.27% | 0.6833–0.7740 |
| memory-javascript-0-commands-16 | 32.9455 | 30.7737 | +3.57% | 0.9186–1.0903 |
| sqlite-javascript-0-commands-1 | 19.3532 | 20.0416 | +3.06% | 0.9324–1.0396 |
| sqlite-javascript-65536-commands-1 | 65.1713 | 52.0023 | +20.30% | 0.7795–0.8390 |
| sqlite-javascript-0-commands-16 | 268.0836 | 263.5490 | +0.58% | 0.9359–1.0201 |
| postgres-javascript-0-commands-1 | 83.2907 | 83.2282 | -0.07% | 0.9840–1.0128 |
| postgres-javascript-65536-commands-1 | 190.9782 | 176.1487 | +8.45% | 0.8923–0.9774 |
| postgres-javascript-0-commands-16 | 992.9859 | 991.9219 | +0.21% | 0.9310–1.0355 |
| postgres-cpp-0 | 53.1536 | 53.1421 | +0.81% | 0.9605–1.0069 |

ペア百分率は、別々に集約した中央値列の比率と一致する必要はない。大入力の改善は五組すべてで一貫していた。比率が 1 の両側に分布する小ケースでは結論を出せない。これは普遍的な高速化でも LLM 応答時間の比較でもない。

微小な direct-Core 行にはノイズがあったため、より広い対照測定として七組、warmup 100 回、測定 direct invocation 10,000 回を使った。変更後/変更前比の中央値は 1.0014、範囲は 0.9838–1.0331 だった。有意な direct-Core の変化は確認できない。

## 機械語と追加の証拠

owner-local の AgentX radare2 read-only peer が、content-addressed な変更後 ELF を調査した。symbol 境界内の disassembly では、`compress_x86_sha` 内の `SHA256RNDS2`、`SHA256MSG1`、`SHA256MSG2`、調査した portable compressor に SHA 命令がないこと、availability check 内の CPUID 命令を確認した。専用関数は baseline 経路から分離されている。

変更後の 64 KiB memory workload に対する software CPU-clock profile は、損失ゼロで 1,418 sample を収集した。hardware compressor は sample の約 9.31%、unescaped-string scanning は 17.91% だった。約 39% は未解決の shared-library symbol に残っており、追加の帰属調査なしにすべて allocation に割り当ててはならない。これはプロセス全体の leaf-IP profile であり、wall time の内訳ではなく、主要な時間測定にも使っていない。

AgentX は owner が直接認可した analysis peer として使った。AgentX model WorkOrder や Analysis Graph provenance の取り込みは主張しない。その service configuration は変更していない。

## 再現と保持した証拠

変更前 revision（`779be451`）と変更後 revision で、同一の Release option を使って `bench_sha256` と `bench_program_cost` をビルドする。新しい identity benchmark source は、旧 Core library に対してコンパイルできる。例:

```sh
bench_sha256 65536 500
```

end-to-end 比較には[ペア Program runner](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) を使う。build 時に高速化を無効にするには `-DNEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF` を設定する。

生結果、独立 vector、source/binary hash、upstream source identity、AgentX RPC 記録、範囲を限定した assembly、CPU sample/map、Windows smoke log、test log は、`artifacts/program-sha256-20260907/` にローカル保持している。database file は WSL に置く。認証情報は含めない。
