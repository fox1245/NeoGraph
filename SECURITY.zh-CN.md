<!-- neograph-i18n: source=SECURITY.md locale=zh-CN source_sha256=1846450e26db18b21a90809734487fbc5fef135a3425887bd70fae57a20beb4e -->
# 安全策略

**Languages:** [English](SECURITY.md) | [한국어](SECURITY.ko.md) | [日本語](SECURITY.ja.md) | [简体中文](SECURITY.zh-CN.md)

## 受支持的版本

NeoGraph 尚处于 1.0 之前。安全修复先合入 `master`，再随当前次版本线的下一个
发布交付。较旧的次版本线不再打补丁，请升级到最新发布。

| 版本 | 支持 |
| ---- | ---- |
| `master` | 是，修复最先合入这里 |
| 0.13.x（最新发布） | 是 |
| < 0.13 | 否 |

## 报告漏洞

请不要为安全问题创建公开 issue。请通过 GitHub 私下报告：在仓库的 **Security**
标签页选择 **Report a vulnerability**
（<https://github.com/fox1245/NeoGraph/security/advisories/new>）。

请尽量包含：

- 受影响的版本或提交；
- 组件：核心引擎、Program 运行时、Python 绑定、MCP·A2A·ACP·gRPC 的服务端或
  客户端、检查点存储，或 SchemaProvider 集成；
- 最小复现步骤以及观察到的影响。

报告中请勿包含真实的 API 密钥、凭据或个人数据。

处理时间：7 天内确认收到，14 天内给出初步评估。修复时间取决于严重程度。修复会在
打了补丁的版本发布后通过 GitHub Security Advisory 公开，适用时也会申请 CVE。

## 范围

范围内：本仓库中的 NeoGraph 源码、已发布的 wheel，以及 NeoGraph 随附的 vendored
第三方代码（例如 QuickJS、cpp-httplib）中可通过 NeoGraph 触及的问题。

范围外：NeoGraph 未暴露的第三方依赖中的问题（请向上游报告）、`docs/promo` 下的
宣传视频工具，以及用于演示用法、并非加固部署配置的示例。

## 安全模型要点

- Program 源码和图定义应当像代码一样对待。只接受来自可信作者的定义，或通过
  admission 策略准入，并用运行预算限制其成本。
- 工具结果和模型输出是不可信数据。不要指望模型拒绝注入的指令，应使用 ToolGate
  和 Hook 控制有副作用的工具。
- 通过环境变量或宿主回调传递 API 密钥，不要放进 Program 或图的 JSON。NeoGraph
  不会把密钥写入日志、事件或线路上的错误。
- NeoGraph 及其 SchemaProvider transport 的出站 HTTPS 要求 TLS 1.2 或更高版本。
