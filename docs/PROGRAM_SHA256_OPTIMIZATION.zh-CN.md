<!-- neograph-i18n: source=docs/PROGRAM_SHA256_OPTIMIZATION.md locale=zh-CN source_sha256=7e4fa7e81f456ac2c8027d75d34ccf3f85a9cbc18acbd7cef91f1282558b981a -->
# SHA-256 CPU 加速 — 2026-09-07

**Languages:** [English](PROGRAM_SHA256_OPTIMIZATION.md) | [한국어](PROGRAM_SHA256_OPTIMIZATION.ko.md) | [日本語](PROGRAM_SHA256_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_SHA256_OPTIMIZATION.zh-CN.md)

运行时条件启用的 x86 SHA 指令将测得的 64 KiB identity-hash 成本降低约 **6.2 倍**。相对于先前规范 JSON 优化 (`779be451`)，大输入案例的配对 Program 延迟改善约 **内存存储 28%、SQLite 20%、PostgreSQL 8%**。小数据库案例没有确立有意义的改善。

## 实现与兼容性

原标量 SHA-256 compression 和 padding 从 `canonical_json.cpp` 移到内部 `sha256.cpp` 模块。单独、不内联的 x86-64 函数用 SHA 指令执行 rounds 和 message schedule。其 register layout 遵循已注明出处的公有领域 [SHA-Intrinsics 参考](https://github.com/noloader/SHA-Intrinsics/blob/d03795497f3e4576083fc2cd8fe0b924f24d0bb2/sha256-x86.c)；实现使用轮转四向量 schedule。原标量实现仍可用。

自动派发在选择加速函数前检查 CPUID basic leaf 可用性，以及 SSSE3、SSE4.1、SHA 支持。GCC/Clang 指令启用局限于该函数，不使用全局 architecture flag。MSVC 在同一运行时检查后编译相同 intrinsics。其他架构和 `NEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF` 构建选择 portable 路径。没有增加库依赖。

私有 backend selector 允许强制 portable/hardware 正确性测试，不改变全进程行为。它不是 installed SDK 的一部分。硬件不可用时显式选择 hardware 会抛异常。digest state 局限于每次调用；只缓存 CPU availability 和所选函数指针。

身份 framing 不变：`preamble || NUL || domain || NUL || decimal(payload byte length) || NUL || payload`。padding、全部 256 digest bits、小写十六进制和 `sha256:` 前缀保留。现有 input-buffer construction 仍在；此变更不引入 streaming、hash caching、较弱 checksums、新存储格式或不同 authority/budget 规则。

指令语义参考：[Intel SHA extensions](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sha-extensions.html)。运行机器的 Ryzen 暴露所需 x86 instruction bits；派发不限定 Intel 厂商。

## 验证

- 标准 SHA-256 已知答案，包括空输入、`abc`、56-byte vector 和一百万个 `a` 字节。
- 35 个独立 Python-hashlib vectors，覆盖二进制 payloads、padding boundaries、unaligned input views、identity framing 和嵌入 NUL。
- 确定性 512-input corpus 比较 portable、automatic、hardware 输出，加上并发独立 hashing。
- CPU-feature predicate tests 拒绝缺失 leaf 或任意必需 instruction bit。
- Linux ASan/UBSan：六个 SHA tests 在普通和禁用 acceleration 构建中均通过，启用 leak detection，无 suppressions。
- Windows MSVC x64：35 个独立 vectors 在硬件可用时通过，编译移除 acceleration 后再次通过。这是原生 backend 正确性覆盖，不是完整 Windows Program suite 或性能声明。
- Focused Core suite：217 通过，包括 PostgreSQL checkpoints 和 identity consumers。
- 完整 Program suite：680 通过；跳过两个不适用内存存储进程重启案例。仍覆盖 golden stored identities、SQLite/PostgreSQL recovery、replacement、lineage 和 budgets。

## 独立 identity-hash 比较

每个输入大小七对新进程；20 次 warmups；每进程测量 500 次 hashes，1 MiB 时减少到 100。AB/BA 及案例顺序交替。benchmark 包括未改变的 identity framing 和 hex encoding。每个返回 digest 用 Python hashlib 独立检查。下列计时为进程中位数的中位数；降幅使用配对比值中位数。

| Payload 字节数 | 优化前 (µs) | 优化后 (µs) | 配对降幅 |
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

## Program 比较

相同 Ryzen 7 5800X、WSL2 Ubuntu 24.04、GCC 13.3 Release `-O3 -DNDEBUG`（含 repository hardening）、一个 Runtime scheduler thread，以及原生 WSL PostgreSQL 16.15 配置。两个二进制冻结。每案例五对进程，每进程三次 warmups、12 次测量调用，每进程新的 SQLite/PostgreSQL 数据库。配对计时期间没有 builds、profilers 或其他 tests。

| 案例 | 优化前 (ms) | 优化后 (ms) | 配对降幅 | 配对 after/before 范围 |
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

配对百分比不必等于独立汇总中位列之比。大输入改善在全部五对中一致。比值分布于一两侧的小案例没有定论；这不是通用加速，也不是 LLM response time 比较。

微小直接 Core 行噪声较大，因此更广对照使用七对进程、100 次 warmups 和 10,000 次直接测量调用。after/before 中位比为 1.0014，范围 0.9838–1.0331。它没有确立有意义的直接 Core 变化。

## 机器码与后续证据

owner 本地 AgentX radare2 只读 peer 检查按内容寻址的 after ELF。符号范围反汇编确认 `compress_x86_sha` 内有 `SHA256RNDS2`、`SHA256MSG1` 和 `SHA256MSG2`，被检查的 portable compressor 中没有 SHA 指令，availability check 中有 CPUID 指令。专用函数与 baseline 路径隔离。

变更后的 64 KiB 内存工作负载 software CPU-clock profile 收集 1,418 个样本，丢失为零。hardware compressor 占约 9.31%，unescaped-string scanning 占 17.91%。约 39% 仍位于未解析 shared-library symbols，在进一步归因前不能全部归为 allocation。这是全进程 leaf-IP profile，不是墙钟时间分区，也未用于主要计时。

AgentX 用作直接 owner 授权 analysis peer。不声称 AgentX model WorkOrder 或 Analysis Graph provenance import。未改变其 service configuration。

## 复现与保留证据

在 before revision (`779be451`) 和变更 revision 用相同 Release 选项构建 `bench_sha256` 与 `bench_program_cost`。新 identity benchmark 源可以针对旧 Core 库编译。例如：

```sh
bench_sha256 65536 500
```

端到端比较使用 [配对 Program runner](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)。构建时禁用 acceleration 可设置 `-DNEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF`。

原始结果、独立 vectors、source/binary hashes、upstream source identity、AgentX RPC records、有界 assembly、CPU samples/maps、Windows smoke logs 和 test logs 本地保留在 `artifacts/program-sha256-20260907/`。数据库文件留在 WSL。不含凭据。
