<!-- neograph-i18n: source=docs/PROGRAM_CANONICAL_JSON_OPTIMIZATION.md locale=zh-CN source_sha256=21e00a791dfa14a8b9748d723c10121eb2c8acdbd477978e058c932110f2d41d -->
# 结合 AgentX 分析的规范 JSON 优化 — 2026-09-07

**Languages:** [English](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md) | [한국어](PROGRAM_CANONICAL_JSON_OPTIMIZATION.ko.md) | [日本語](PROGRAM_CANONICAL_JSON_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_CANONICAL_JSON_OPTIMIZATION.zh-CN.md)

在测量的 64 KiB Program 工作负载上，相对于已优化的 command-head 实现 (`ef56ff89`)，批量字符串扫描/复制使延迟降低约 **内存存储 54%、SQLite 50%、PostgreSQL 26%**。改善来自原生规范 JSON 处理；此实验不评估 JIT 后端。

## 选择变更的证据

owner 本地 AgentX radare2 MCP peer（connector version 1.8.6）按内容身份检查精确冻结的 Release ELF。只使用产物打开、符号查找和只读反汇编。函数反汇编最初报告缺少分析元数据；只读服务不暴露 `analyze`。基于地址的 `disassemble` 成功，结论范围由 `nm` 提供的 ELF 符号地址/大小限定。

随后 Linux software CPU-clock sampler 只运行新建 benchmark 进程及其线程，排除 kernel/hypervisor 样本。采样周期 1 ms，使用逐 CPU inherited buffers。这是包括 setup、调用方验证和 teardown 的全进程 leaf-IP profile，不是墙钟时间分区或 inclusive call graph。未解析 shared-library symbols 明确保留为未解析。

- 小内存输入：4,196 个样本，丢失为零。
- 64 KiB 内存输入：5,155 个样本，丢失为零。`append_escaped` 占 25.68%，UTF-8 sequence validation 占 17.58%，escaped-size calculation 占 11.68%，合计约 55%。
- AgentX 反汇编显示旧循环在 `0x66a349` 调用 UTF-8 验证，按返回 sequence length 前进；每个 ASCII 字节的长度都是一。源检查也确认独立的逐字节 escape-size 和输出循环。

这些证据使字符串处理优先于推测性 VM 变化。下列单独、未采样的比较确定实际测得改善。采样 API 参考：[Linux perf_event_open](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)。

## 变更与不变量

`src/core/canonical_json.cpp` 现在以完整 8-byte words 检查 ASCII 或 JSON escape bytes，再标量处理余数。非对齐 words 通过 `memcpy` 加载；重复字节 masks 不依赖字节序。UTF-8 validation 一次消费一个 ASCII run，并保留既有非 ASCII 有效性规则。验证后字符串输出一次追加完整未转义 span。

大小计算从 raw-size 下界开始，再加转义扩展。它保留每个控制字节原有的保守六字节计费，包括 newline 等短转义。16 MiB 限制、无效 UTF-8 拒绝、转义字节、排序 keys、数字编码、owned-value 行为和内容身份不变。不增加依赖、架构专属编译选项、公开 API、SQL query 或持久设置。

after ELF 也通过同一 AgentX peer 检查。其位于 `0x668710` 的 `unescaped_prefix` 包含 `mov rax, qword [r8 + rcx]`，并将 `rcx` 前进八，确认编译代码中预期的宽加载循环。

## 正确性验证

- 六个新增规范 JSON 测试覆盖 word boundaries 附近所有 ASCII bytes、混合 Unicode/escaping、确定性 4,096-case Unicode corpus、ASCII prefixes 后无效 UTF-8、短/非对齐 views，以及保守物化大小边界。
- 同六个测试当时在 AddressSanitizer 和 UndefinedBehaviorSanitizer 下通过，含 leak detection，无 suppressions。
- 两轮共 211 个 focused Core tests 通过：最初 180 个，之后启用专用后端的全部 31 个 PostgreSQL checkpoint cases。
- 完整 Program suite：680 通过；跳过两个不适用的内存存储进程重启案例。包括 golden canonical identities 和 SQLite/PostgreSQL recovery、lineage、replacement、budget 检查。
- 每个 microbenchmark 输出均匹配独立标量 escape oracle；每次 Program 调用保留预期 counter 和完整 payload。
- 验证在 WSL GCC 13.3 上进行。不声称 Windows、ARM 或全 engine sanitizer 验证。

## 仅字符串 benchmark

每案例七对新进程，每进程 20 次 warmup 和 500 次测量 serialization，交替 AB/BA 及案例顺序。输入包括 ASCII、韩语/emoji、含转义混合文本，以及大量 control/quote/backslash 的案例。准备及完整输出验证在计时调用之外。微小亚微秒案例接近 timer granularity，不应作为精确回归 gate。

| 输入 | 优化前中位数 (µs) | 优化后中位数 (µs) | 配对降幅中位数 |
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

## Program 运行时比较

每案例五对新进程，每进程三次 warmup 和 12 次测量调用，使用 `compare_program_costs.py`。两个二进制冻结；before 是先前 command-head 优化。使用相同 Ryzen 7 5800X、WSL ext4、带 repository hardening 的 Release `-O3 -DNDEBUG`、一个 Runtime scheduler thread 和原生 WSL PostgreSQL 16.15 配置。每个数据库案例以新数据库开始。配对计时期间没有 builds、profilers 或其他测试任务。

| 案例 | 优化前中位数 (ms) | 优化后中位数 (ms) | 配对降幅中位数 | 配对比值范围 |
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

百分比汇总配对比值，不必等于两个独立中位列之比。微小直接 Core 计时和小数据库案例相对离散较大；大输入改善在全部五对中一致。这是合成 engine 开销，不是 chatbot/model response time，也不是所有工作负载的通用加速。

## 剩余工作

后续 64 KiB CPU profile 收集 2,317 个样本，丢失为零。SHA-256 identity calculation 占约 35.17%，unescaped-span scanning 占 10.96%，UTF-8 validation 占 1.90%。hash 百分比增大反映总工作减少，不证明 hashing 变慢。将全部未解析成本归因于 allocation 或 copying 之前，shared-library symbols 仍需要更好的归因。

## 复现与范围

在 `ef56ff89` 和变更 revision 以相同 Release 选项构建 `bench_canonical_json` 和 `bench_program_cost`；新 microbenchmark 源可针对 before 库构建，无须修改该库。示例 micro 命令：

```sh
bench_canonical_json unicode 65536 500
```

端到端案例使用 [配对 Program 比较](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)。原始 RPC 请求/响应、tool schemas、ELF/source hashes、CPU samples/maps、helper sources、micro/runtime results 和 test logs 本地保留在 `artifacts/program-canonical-agentx-20260907/`。完整只读输入 ELFs 仍在指定 AgentX artifact root 的 digest-derived directories 中；证据不含凭据。

此次从 owner 授权宿主直接使用 AgentX analysis peer。没有启动 AgentX model WorkOrder、发布新 ToolBindingSet 或向 AgentX Analysis Graph 导入记录。不声称这些 workflow/provenance 保证。未更改 AgentX service configuration 或其他应用数据。
