# Security Policy

**Languages:** [English](SECURITY.md) | [한국어](SECURITY.ko.md) | [日本語](SECURITY.ja.md) | [简体中文](SECURITY.zh-CN.md)

## Supported versions

NeoGraph is pre-1.0. Security fixes land on `master` first and ship in the
next release of the current minor line. Older minor lines are not patched;
upgrade to the latest release.

| Version | Supported |
| ------- | --------- |
| `master` | Yes, fixes land here first |
| 0.14.x (latest release) | Yes |
| < 0.14 | No |

## Reporting a vulnerability

Please do not open a public issue for a security problem. Report it privately
through GitHub: open the repository's **Security** tab and choose **Report a
vulnerability** (<https://github.com/fox1245/NeoGraph/security/advisories/new>).

Include, where you can:

- the affected version or commit;
- the component: core engine, Program runtime, Python binding, MCP, A2A, ACP
  or gRPC server or client, checkpoint stores, or the SchemaProvider
  integration;
- a minimal reproduction and the impact you observed.

Never include real API keys, credentials or personal data in a report.

What to expect: an acknowledgement within 7 days and an initial assessment
within 14 days. The fix schedule depends on severity. Fixes are disclosed
through a GitHub Security Advisory, with a CVE when one applies, after a
patched release is available.

## Scope

In scope: the NeoGraph sources in this repository, the published wheels, and
vendored third-party code as NeoGraph ships it (for example QuickJS and
cpp-httplib) when the issue is reachable through NeoGraph.

Out of scope: issues in third-party dependencies that NeoGraph does not expose
(please report those upstream), the promotional video tooling under
`docs/promo`, and the examples, which illustrate usage rather than hardened
deployments.

## Security model in brief

- Program sources and graph definitions behave like code. Admit them only from
  trusted authors or through an admission policy; run budgets bound their cost.
- Tool results and model output are untrusted data. Gate effectful tools with
  the ToolGate and Hooks rather than relying on the model to refuse injected
  instructions.
- Pass API keys through the environment or host callbacks, never inside Program
  or graph JSON. NeoGraph does not write keys to logs, events or wire errors.
- Outbound HTTPS from NeoGraph and its SchemaProvider transport requires TLS 1.2
  or newer.
