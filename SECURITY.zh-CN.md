<!-- neograph-i18n: source=SECURITY.md locale=zh-CN source_sha256=24ae45f92fb362565a8acd8171ea70ddd9ecdd80831b0d281d5c5284d51bc065 -->
# 安全策略

**Languages:** [English](SECURITY.md) | [한국어](SECURITY.ko.md) | [日本語](SECURITY.ja.md) | [简体中文](SECURITY.zh-CN.md)

## 受支持的版本

NeoGraph 尚处于 1.0 之前。安全修复会先合入 `master`，再随当前次版本线的下一个
发布版本交付。更早的次版本线不再提供补丁，请升级到最新发布版本。

| 版本 | 支持 |
| ---- | ---- |
| `master` | 是，修复最先合入这里 |
| 0.14.x（最新发布版本） | 是 |
| < 0.14 | 否 |

## 报告漏洞

请不要为安全问题创建公开 issue，而是通过 GitHub 私下报告：打开仓库的 **Security**
标签页，选择 **Report a vulnerability**
（<https://github.com/fox1245/NeoGraph/security/advisories/new>）。

请尽量提供以下信息：

- 受影响的版本或提交；
- 涉及的组件：核心引擎、Program 运行时、Python 绑定、MCP、A2A、ACP 或 gRPC
  的服务端或客户端、检查点存储，或 SchemaProvider 集成；
- 最小复现步骤，以及观察到的影响。

报告中不要包含真实的 API 密钥、凭据或个人数据。

处理时间：7 天内确认收到，14 天内给出初步评估。修复时间取决于严重程度。修复会在
带补丁的版本发布之后，通过 GitHub Security Advisory 公开；适用时同时申请 CVE。

## 范围

范围内：本仓库中的 NeoGraph 源码、已发布的 wheel，以及 NeoGraph 原样随附的第三方
代码（例如 QuickJS 和 cpp-httplib）中、可以通过 NeoGraph 触及的问题。

范围外：NeoGraph 没有暴露的第三方依赖中的问题（请向上游报告）、`docs/promo`
下的宣传视频工具，以及示例——示例只用来演示用法，并不是经过加固的部署配置。

## 安全模型概要

- Program 源码和图定义的作用等同于代码。只接受来自可信作者的定义，或者经过准入(admission)
  策略放行；运行预算可以限制它们的开销。
- 工具结果和模型输出都是不可信的数据。不要指望模型自己拒绝被注入的指令，而应当用
  ToolGate 和 Hook 来控制有副作用的工具。
- API 密钥通过环境变量或宿主回调传入，不要写进 Program 或图的 JSON。NeoGraph
  不会把密钥写入日志、事件或返回给对端的错误信息。
- NeoGraph 及其 SchemaProvider 传输层的出站 HTTPS 要求 TLS 1.2 或更高版本。
