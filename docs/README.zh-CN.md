<!-- neograph-i18n: source=docs/README.md locale=zh-CN source_sha256=21d29bec5271896709470458f9bf7b867b830a0685740d7846d48feba6c96c96 -->
# NeoGraph 文档

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本页列出本目录中的文档以及仓库其他位置的主要 README，并说明每份文档适合谁阅读。初次接触的读者请按顺序阅读第一张表。要查某个类或函数，请去参考文档。最后两张表是设计记录和测量资料，用来解释代码为什么是现在的样子，不是使用指南。

## 从这里开始

| 文档 | 为什么读 |
|---|---|
| [README](../README.md) | NeoGraph 是什么、无需 API 密钥即可运行的 Python 和 C++ 快速入门、构建选项。 |
| [Concepts](concepts.md) | 通道、归约器、节点、边、`Send`、`Command`、检查点、中断和流式输出，按搭建图的先后顺序讲解。 |
| [C++ 示例](../examples/README.md)和 [Python 示例](../bindings/python/examples/README.md) | 可直接运行的程序。大多数不需要 API 密钥，需要的会在各自的索引中标明。 |
| [故障排除](troubleshooting.md) | 构建、安装、运行时和 Python 的错误，以及每个错误的原因和解决办法。 |

## 指南

| 文档 | 想做这件事时阅读 |
|---|---|
| [Python 绑定](python-binding.md) | 在 Python 中使用 NeoGraph：图、带类型的 provider 请求、Program、Hook、检查点、协议客户端。 |
| [Program 指南](program-guide.md) | 编写受限的 JavaScript 程序：并行运行图、处理失败的节点、等待人工批准后恢复，并保持在预算之内。 |
| [Program 图功能](program-graphs.md) | 查阅 Program 的 `define()` 图能表达什么：元数据、`any_of` 汇合、按节点重试、失败边与失败退出、单次尝试超时、健全性检查。 |
| [诊断](diagnostics.md) | 查阅编译器或运行时的诊断代码（`P_JS_*`、`P_CORE_*`、`P_RUNTIME_*`）及其解决办法。 |
| [MCP](mcp.md) | 把图公开为 MCP 工具，或在图中调用 MCP 服务器上的工具。 |
| [A2A](a2a.md) | 把图公开为 Agent2Agent 智能体，或调用远程 A2A 智能体。 |
| [ACP](acp.md) | 把图作为 Agent Client Protocol 智能体运行，供编辑器通过 stdio 启动。 |
| [Harness MCP](HARNESS_MCP.md) | 使用 Harness：6 个 MCP 工具，用于编译并运行有界的多工作者工作流，支持重放和评审。 |
| [异步指南](ASYNC_GUIDE.md)和[并发](concurrency.md) | 选择执行器、让多个会话共享一个引擎、编写异步 provider 和工具、取消运行。 |
| [运行时上下文与严格介入](STRICT_RUNTIME_INTERPOSITION.md) | 让必需的上下文、Hook 和 provider 派发回执成为强制要求，不再交给模型自行决定。 |
| [迁移指南](migration-v0.4-to-v1.0.md) | 把基于旧节点 API（`execute` 及其变体）的代码迁移到 `run(NodeInput)`，把基于 `CompletionParams` 和 `complete` 的代码迁移到带类型的 provider 请求。 |
| [Valgrind 与 sanitizer](VALGRIND.md) | 运行 NeoGraph 的内存检查和 sanitizer 检查。 |

## 参考

| 文档 | 想做这件事时阅读 |
|---|---|
| [C++ 参考](reference-en.md) | 查类型、函数或失败代码。MCP、gRPC、A2A、ACP 的协议失败映射表也在这里。 |
| [生成的 API 参考](doxygen-mainpage.md) | 从 Doxygen 主页开始浏览 `include/neograph/` 中的公开头文件。 |
| [ABI 政策](ABI_POLICY.md) | 了解升级 NeoGraph 时什么情况下需要重新构建自己的代码。 |
| [Schema](../schemas) | 校验 NeoGraph 交换的 JSON：Core 定义、Hook、上下文工件和回执。 |
| [变更日志](../CHANGELOG.md)、[安全政策](../SECURITY.md)和[路线图](../ROADMAP_v1.md) | 查看有哪些变更、报告漏洞，或阅读早期周期保留下来的设计提案。 |

## 设计记录

这些文档描述 Program 层和 QuickJS 编写接口的设计。扩展运行时或需要确切契约时再读，仅仅使用 NeoGraph 不需要读。

| 文档 | 主题 |
|---|---|
| [v1 架构](V1_ARCHITECTURE.md)和 [v1 迁移计划](V1_MIGRATION_PLAN.md) | Core 与 Program 的架构，以及实施顺序。仅有英文。 |
| [QuickJS 控制架构](QUICKJS_CONTROL_ARCHITECTURE.md)、[迁移](QUICKJS_CONTROL_MIGRATION.md)、[执行配置](QUICKJS_EXECUTION_PROFILES.md)和[公开编写边界](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md) | 内嵌的 JavaScript 编写接口及其能做的事。仅有英文。 |
| [QuickJS 迁移完成](QUICKJS_MIGRATION_COMPLETION.md) | 完成该迁移的历史运行手册，属于规划记录。 |
| [Program 能力上下文](PROGRAM_CAPABILITY_CONTEXT.md) | 如何让节点只看到其已批准配置所声明的能力。 |
| [递归 Harness](PROGRAM_RECURSIVE_HARNESSES.md) | 父、子、孙三层智能体树，每个智能体有自己的 Program 版本和预算。 |
| 子程序合成：[契约](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)、[持久化](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)、[恢复](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | Program 如何请求宿主批准生成的子程序源码，以及这一状态如何在重启后保留。 |
| [自演化智能体控制器](SELF_EVOLVING_AGENT_CONTROLLER.md) | 在宿主批准下修改自身图的控制器。仅有英文。 |
| [异步第三阶段设计](ASYNC_STAGE3_DESIGN.md) | 基于 asio 的异步执行器的设计。 |
| [DSL 能力评估](DSL_CAPABILITY_EVAL.md)和[模型合成 Program 的证据](model-program-synthesis-evidence.md) | 测试模型能否写出有效的 Program，附日期和结果。 |

## 测量资料

| 文档 | 主题 |
|---|---|
| [性能详解](performance-deep-dive.md)和[基准测试](../benchmarks/README.md) | 引擎开销与并发的测量值，以及每次运行的配置。这些数值不是保证。 |
| [Program 成本测量](PROGRAM_COST_MEASUREMENT.md)和 [2026-09-07 基线](PROGRAM_COST_BASELINE_2026-09-07.md) | 如何测量 Program 的记账成本和存储成本。 |
| [命令头](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)、[规范 JSON](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md)和 [SHA-256](PROGRAM_SHA256_OPTIMIZATION.md) 优化 | 为降低这些成本改了什么，以及前后的数值。 |
