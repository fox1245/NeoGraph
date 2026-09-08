# SHA-256 CPU acceleration — 2026-09-07

Runtime-gated x86 SHA instructions reduced the measured 64 KiB identity-hash cost by about **6.2×**. Compared with the preceding canonical JSON optimization (`779be451`), paired Program latency improved by approximately **28% with memory stores, 20% with SQLite, and 8% with PostgreSQL** for the large-input case. Small database cases did not establish a meaningful improvement.

## Implementation and compatibility

The original scalar SHA-256 compression and padding moved from `canonical_json.cpp` to an internal `sha256.cpp` module. A separate, non-inlined x86-64 function performs the rounds and message schedule with SHA instructions. Its register layout follows the attributed public-domain [SHA-Intrinsics reference](https://github.com/noloader/SHA-Intrinsics/blob/d03795497f3e4576083fc2cd8fe0b924f24d0bb2/sha256-x86.c); the implementation uses a rotating four-vector schedule. The original scalar implementation remains available.

Automatic dispatch checks CPUID basic leaf availability, SSSE3, SSE4.1 and SHA support before selecting the accelerated function. GCC/Clang instruction enablement is local to that function, with no global architecture flag. MSVC compiles the same intrinsics behind the same runtime check. Other architectures and builds with `NEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF` select the portable path. No additional library dependency was introduced.

The private backend selector permits forced portable/hardware correctness tests without changing process-wide behavior. It is not part of the installed SDK. Explicit hardware selection throws when unavailable. Digest state is local to each call; only CPU availability and the selected function pointer are cached.

Identity framing is unchanged: `preamble || NUL || domain || NUL || decimal(payload byte length) || NUL || payload`. Padding, all 256 digest bits, lowercase hexadecimal and the `sha256:` prefix are preserved. The existing input-buffer construction remains in place; this change does not introduce streaming, hash caching, weaker checksums, new storage formats, or different authority/budget rules.

Instruction semantics reference: [Intel SHA extensions](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sha-extensions.html). The running Ryzen exposes the required x86 instruction bits; this is not an Intel-vendor-only dispatch.

## Validation

- Standard SHA-256 known answers, including empty input, `abc`, the 56-byte vector and one million `a` bytes.
- 35 independent Python-hashlib vectors over binary payloads, padding boundaries, unaligned input views, identity framing, and embedded NULs.
- A deterministic 512-input corpus comparing portable, automatic and hardware output, plus concurrent independent hashing.
- CPU-feature predicate tests reject a missing leaf or any missing required instruction bit.
- Linux ASan/UBSan: all six SHA tests passed in both normal and acceleration-disabled builds, with leak detection and no suppressions.
- Windows MSVC x64: 35 independent vectors passed with hardware available and again with acceleration compiled out. This is native backend correctness coverage, not a full Windows Program-suite or performance claim.
- Focused Core suite: 217 passed, including PostgreSQL checkpoints and identity consumers.
- Full Program suite: 680 passed; two inapplicable memory-store process-restart cases skipped. Golden stored identities, SQLite/PostgreSQL recovery, replacement, lineage and budgets remain covered.

## Independent identity-hash comparison

Seven fresh process pairs per input size; 20 warmups; 500 measured hashes per process, reduced to 100 for 1 MiB. AB/BA and case order alternate. The benchmark includes the unchanged identity framing and hex encoding. Every returned digest is checked independently with Python hashlib. Timings below are medians of process medians; reduction uses the median paired ratio.

| Payload bytes | Before (µs) | After (µs) | Paired reduction |
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

## Program comparison

Same Ryzen 7 5800X, WSL2 Ubuntu 24.04, GCC 13.3 Release `-O3 -DNDEBUG` with repository hardening, one Runtime scheduler thread and native-WSL PostgreSQL 16.15 configuration. Both binaries were frozen. Five process pairs per case, three warmups and 12 measured invocations per process, with a new SQLite/PostgreSQL database per process. No builds, profilers or other tests ran during these timing pairs.

| Case | Before (ms) | After (ms) | Paired reduction | Paired after/before range |
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

Paired percentages need not equal the ratio of the independently summarized median columns. Large-input improvements were consistent across all five pairs. Small cases with ratios on both sides of one are inconclusive; this is not a universal speedup or an LLM-response-time comparison.

The tiny direct-Core row was noisy, so one wider control used seven pairs with 100 warmups and 10,000 measured direct invocations. Its median after/before ratio was 1.0014, with range 0.9838–1.0331. It does not establish a meaningful direct-Core change.

## Machine-code and follow-up evidence

The owner-local AgentX radare2 read-only peer inspected the content-addressed after ELF. Symbol-bounded disassembly confirms `SHA256RNDS2`, `SHA256MSG1` and `SHA256MSG2` inside `compress_x86_sha`, no SHA instructions in the inspected portable compressor, and CPUID instructions in the availability check. The specialized function is isolated from the baseline path.

A post-change software CPU-clock profile of the 64 KiB memory workload collected 1,418 samples with zero loss. The hardware compressor accounted for approximately 9.31% of samples, while unescaped-string scanning was 17.91%. About 39% remained in unresolved shared-library symbols, which must not all be assigned to allocation without further attribution. This is a whole-process leaf-IP profile, not a wall-time partition, and was not used for headline timings.

AgentX was used as a direct owner-authorized analysis peer. No AgentX model WorkOrder or Analysis Graph provenance import is claimed. Its service configuration was not changed.

## Reproduction and retained evidence

Build `bench_sha256` and `bench_program_cost` with identical Release options at the before revision (`779be451`) and the changed revision. The new identity benchmark source can be compiled against the old Core library. Example:

```sh
bench_sha256 65536 500
```

Use [the paired Program runner](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) for end-to-end comparisons. To disable acceleration when building, set `-DNEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF`.

Raw results, independent vectors, source/binary hashes, upstream source identity, AgentX RPC records, bounded assembly, CPU samples/maps, Windows smoke logs and test logs are retained locally in `artifacts/program-sha256-20260907/`. Database files remain in WSL. Credentials are not included.
