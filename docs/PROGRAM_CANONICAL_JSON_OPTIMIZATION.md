# Canonical JSON optimization with AgentX analysis — 2026-09-07

On the measured 64 KiB Program workload, batching string scans/copies reduced latency by about **54% with memory stores, 50% with SQLite, and 26% with PostgreSQL**, relative to the already optimized command-head implementation (`ef56ff89`). The improvement comes from native canonical JSON processing; this experiment does not evaluate a JIT backend.

## Evidence that selected the change

The owner-local AgentX radare2 MCP peer (connector version 1.8.6) inspected the exact frozen Release ELF by content identity. Only artifact opening, symbol lookup and read-only disassembly were used. Function disassembly initially reported missing analysis metadata; the read-only service does not expose `analyze`. Address-based `disassemble` succeeded, and findings were bounded by ELF symbol addresses/sizes from `nm`.

A Linux software CPU-clock sampler then ran only newly created benchmark processes and their threads, excluding kernel/hypervisor samples. It used a 1 ms period and per-CPU inherited buffers. This is a leaf-IP profile of the whole process, including setup, caller-side verification and teardown; it is not a wall-time partition or an inclusive call graph. Unresolved shared-library symbols remain explicitly unresolved.

- Small memory input: 4,196 samples, zero lost.
- 64 KiB memory input: 5,155 samples, zero lost. `append_escaped` accounted for 25.68%, UTF-8 sequence validation 17.58%, and escaped-size calculation 11.68% of samples, approximately 55% combined.
- AgentX disassembly showed the old loop calling UTF-8 validation at `0x66a349` and advancing by its returned sequence length, which was one for every ASCII byte. Source inspection also confirmed separate bytewise escape-size and output loops.

This evidence selected string processing ahead of speculative VM changes. The separate unprofiled comparisons below determine the measured improvement. Sampling API reference: [Linux perf_event_open](https://man7.org/linux/man-pages/man2/perf_event_open.2.html).

## Change and invariants

`src/core/canonical_json.cpp` now checks complete 8-byte words for ASCII or JSON escape bytes, then handles the remainder scalarly. Unaligned words are loaded through `memcpy`; repeated-byte masks do not depend on byte order. UTF-8 validation consumes an ASCII run at once and retains the existing non-ASCII validity rules. String emission appends an unescaped span in one operation after validation.

The size calculation starts with the raw-size lower bound, then adds escape expansion. It retains the original conservative six-byte charge for every control byte, including short escapes such as newline. The 16 MiB limit, invalid UTF-8 rejection, escaping bytes, sorted keys, number encoding, owned-value behavior and content identities remain unchanged. No new dependency, architecture-specific compiler option, public API, SQL query, or persistence setting was introduced.

The after ELF was also inspected through the same AgentX peer. Its `unescaped_prefix` at `0x668710` contains `mov rax, qword [r8 + rcx]` and advances `rcx` by eight, confirming the intended wide-load loop in the compiled code.

## Correctness validation

- Six new canonical JSON tests cover all ASCII bytes around word boundaries, mixed Unicode/escaping, a deterministic 4,096-case Unicode corpus, invalid UTF-8 after ASCII prefixes, short/unaligned views, and the conservative materialized-size boundary.
- The same six tests passed with AddressSanitizer and UndefinedBehaviorSanitizer, including leak detection, without suppressions.
- 211 focused Core tests passed across two runs: 180 initially, then all 31 PostgreSQL checkpoint cases with the dedicated backend enabled.
- Full Program suite: 680 passed; two inapplicable memory-store process-restart cases skipped. This includes golden canonical identities and SQLite/PostgreSQL recovery, lineage, replacement and budget checks.
- Every microbenchmark output matched an independent scalar escape oracle; every Program invocation retained its expected counter and full payload.
- Validation was on WSL GCC 13.3. No Windows, ARM or sanitizer-wide engine qualification is claimed.

## String-only benchmark

Seven fresh process pairs per case, 20 warmups and 500 measured serializations per process, alternating AB/BA and case order. The inputs include ASCII, Korean/emoji, mixed text with escapes, and a control/quote/backslash-heavy case. Preparation and complete output verification are outside the timed call. Tiny sub-microsecond cases approach timer granularity and should not be used as precise regression gates.

| Input | Before median (µs) | After median (µs) | Median paired reduction |
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

## Program runtime comparison

Five fresh process pairs per case, three warmups and 12 measured invocations per process, using `compare_program_costs.py`. Both binaries were frozen; the before binary is the prior command-head optimization. The same Ryzen 7 5800X, WSL ext4, Release `-O3 -DNDEBUG` with repository hardening, one Runtime scheduler thread, and native-WSL PostgreSQL 16.15 configuration were used. Each database case starts with a new database. No builds, profilers or other test jobs ran during these timing pairs.

| Case | Before median (ms) | After median (ms) | Median paired reduction | Paired ratio range |
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

Percentages summarize paired ratios, so they need not equal the ratio of the two independent median columns. Tiny direct-Core timings and small database cases have substantial relative spread; the large-input wins were consistent across all five pairs. This is synthetic engine overhead, not chatbot/model response time or a universal workload speedup.

## What remains

A follow-up 64 KiB CPU profile collected 2,317 samples with zero loss. SHA-256 identity calculation accounted for about 35.17% of samples, unescaped-span scanning 10.96%, and UTF-8 validation 1.90%. The larger hash percentage reflects the reduced total work; it is not evidence that hashing became slower. Shared-library symbols still need better attribution before assigning all unresolved cost to allocation or copying.

## Reproduction and scope

Build `bench_canonical_json` and `bench_program_cost` with identical Release options at `ef56ff89` and the changed revision; the new microbenchmark source can be built against the before library without changing it. Example micro command:

```sh
bench_canonical_json unicode 65536 500
```

Use [the paired Program comparison](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) for end-to-end cases. Raw RPC requests/responses, tool schemas, ELF/source hashes, CPU samples/maps, helper sources, micro/runtime results and test logs are retained locally under `artifacts/program-canonical-agentx-20260907/`. The complete read-only input ELFs remain in the assigned AgentX artifact root under their digest-derived directories; no credentials are included in the evidence.

This used AgentX's analysis peer directly from the owner-authorized host. It did not launch an AgentX model WorkOrder, publish a new ToolBindingSet, or import records into AgentX Analysis Graph. Those workflow/provenance guarantees are not claimed. AgentX service configuration and other application data were not changed.
