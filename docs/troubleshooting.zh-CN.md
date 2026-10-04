<!-- neograph-i18n: source=docs/troubleshooting.md locale=zh-CN source_sha256=7f1eeda183c21890031bb9b18196a7377f140553c17f4362974f8d7341588208 -->
# 故障排除

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## 确认安装的 artifact

记录 `neograph_engine.__version__`、Python version、OS/architecture、安装方式和第一个 error。检查该 release 的 wheel 文件，不假设 checkout 与旧 wheel 提供相同 API。typed 切换删除 legacy provider 名；旧 wheel import 不测试新 binding。

NeoGraph 公开 platform metadata 包括 Linux/macOS/Windows。当前 SchemaProvider runtime/archive 验证范围为 Linux/POSIX；macOS/Windows port 须单独验证后才能声称新 wheel 可用。匹配 wheel tag/compiler 不证明 runtime 集成；WASM provider runtime 支持未确立。

缺失 GLIBC symbol 时比较 wheel manylinux tag 与 host glibc。`manylinux_2_34` artifact 需要 glibc 2.34 以上。Windows DLL error 检查 x64 Python 与缺失 dependency DLL，architecture 不是唯一可能原因。不要单独替换 bundled library。

## source 配置与缺失依赖

Core 即使 `NEOGRAPH_BUILD_LLM=OFF` 也需要 `SchemaProvider::runtime`。NeoGraph 配置要求 CMake 3.20+。依次优先显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、installed package、默认 revision-pinned 公开 archive fallback。离线配置时安装匹配 SDK，在 `CMAKE_PREFIX_PATH` 提供 prefix 并设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。SDK source build 需要 C++20、Python、standalone Asio、yyjson、libcurl 7.88+、OpenSSL Crypto；fetched/source 集成使用 NeoGraph 已提交的 Asio/yyjson。

SDK 单独不声明 OpenSSL Crypto 最小 version。NeoGraph 完整 HTTPS 配置与 wheel/sdist 路径通过 async/MCP HTTP 依赖要求 OpenSSL 3；这些 build 仅安装 Crypto header 不够。

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

`Could NOT find CURL` 不能靠禁用 `NEOGRAPH_USE_LIBCURL` 修复；该 flag 控制 NeoGraph 可选 CurlH2Pool，不控制 SDK 必需 transport。安装 libcurl 开发文件（Debian/Ubuntu 的 `libcurl4-openssl-dev`、Fedora 的 `libcurl-devel`），使用一致 toolchain/prefix。

SQLite/PostgreSQL 是 NeoGraph 可选 component。提供开发 package 或显式禁用 `NEOGRAPH_BUILD_SQLITE`/`NEOGRAPH_BUILD_POSTGRES`；SDK runtime 仍保留。header/API 变化后 binding symbol 未解析时，在 fresh build directory 重新配置 CMake，用匹配 header/library rebuild 所有 consumer。

GCC 13 coroutine ICE 在 Stage 3 报告过。升级到适用 compiler 或重构对应表达式；旧 workaround 不验证整个 SDK/platform build。不要在 catch handler 中 `co_await`，C++ 本身禁止。保存 error，在 handler 外 await recovery。

## 删除的 provider API 与 typed failure

`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor-interpreter 配置、`prefer_libcurl`、Responses WebSocket 选择已删除。用 `SchemaProvider(load_provider_descriptor(descriptor_json), ProviderRuntimeOptions(...), SchemaProviderDefaults(...))` 和 `make_provider_request`。准确 constructor 见 [Python binding](python-binding.md)，C++ 见[迁移](migration-v0.4-to-v1.0.md)。

`prepare` 返回 single-use `PreparedProviderRequest`，`dispatch` 消费它；`invoke` 合并两步。保存 dispatch receipt 后不要重建 admitted request，不要重试 consumed handle。failure outcome 是带真实 error/partial 证据的 data；检查 `outcome.failure`，不要当作成功 completion 读取。

observer exception 在 `ProviderObserverError` 保留 typed outcome/cause。budget settlement 和 terminal receipt persistence error 也保留真实 outcome，但该证据不授权再次 send。delivery unknown 需要 reconciliation，不是 blind retry。

## streaming 与 Python async 边界

显式设置 `ProviderMode.Stream`，observer 不选择 streaming。token display 只筛选非空 content-text delta；usage、reasoning、tool、raw-wire、envelope 不是 token。C++ event view 在 callback 期间借用；Python ProviderEvent 拥有复制的借用 data。避免慢 observer 消耗 host delivery capacity。

```python
import asyncio
import neograph_engine as ng

text = ng.Text()
text.value = "Hello"
request = ng.make_provider_request(
    provider, model, [ng.ProviderMessage(ng.ProviderRole.User, [text])],
    mode=ng.ProviderMode.Stream,
)
request.on_event = observe
outcome = await asyncio.to_thread(provider.invoke, request)
if outcome.failure is not None:
    handle_failure(outcome.failure)
else:
    handle_completion(outcome.completion)
```

Provider invoke/dispatch 是释放 GIL 的同步 Python method；使用 `asyncio.to_thread`，它们不是 native asyncio awaitable。callback 调用、复制、销毁获取 GIL。cancel 经 graph CancelToken 向 SDK 传递 stop；timeout 或取消 waiter 不证明 remote effect 未发生。

continuation 保留完整 `ProviderMessage`，含 tool/refusal/reasoning part。`ChatMessage`/`ToolCall` 是 graph 便利值，不是完整 provider history 的 alias。缺失 usage 为 `None`，不是 zero；known-zero counter 是有证据的 UsageCount。portable projection 不能导入 native replay/financial authority；native continuation 只用 admitted NativeArchive custody 保存。

## TLS 与 local endpoint

通过 `ProviderRuntimeOptions.ca_file` 指定 trust bundle。Python import 保留已有 `SSL_CERT_FILE`，否则在 certifi 可用时选择它；runtime option 将选定 CA file 传给 SDK libcurl。`NEOGRAPH_SKIP_CERT_AUTOFIX=1` 不改 host 设置。不要禁用 certificate verification 来隐藏 endpoint/trust-store error。

v0.1.0–v0.1.6 wheel CA path 导致的 ConnPool timeout 是历史问题；v0.1.7 加入 CA 自动选择。当前 provider 用 SDK libcurl 而非旧路径。local HTTP 是 validated descriptor 的 admitted data，不是 raw URL override；遵循[例 31](../examples/31_local_transformer.cpp)。Responses WebSocket close=1000 配方仅属于删除的旧 transport。

## graph 定义与注册

unknown reducer/condition/node-type 表示 compile 使用的 registry 没有该名称。内置 reducer 为 `overwrite`/`append`；compile 前注册 custom reducer/condition/node factory。`has_tool_calls`/`route_channel` 是内置 condition；Python callback 在 GIL 下运行。

```python
import neograph_engine as ng

ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
ng.ConditionRegistry.register_condition("is_long",
    lambda state: "long" if len(state.get("messages") or []) > 10 else "short")
```

write 的 channel 名必须准确存在。condition 返回 route label；open condition 可用显式 `default`，否则 unmatched label 抛错，closed condition 拒绝声明范围外 label。确认 `__start__` edge 和 loop escape。`RunConfig.max_steps` 默认 `50`；检查 step-limit status，不将截断当作正常完成。

`schema_version: 1` 的 strict parsing 拒绝 unknown/unconsumed key 和 round-trip loss。metadata 用 `_`/`x-` annotation，barrier 用非空 `wait_for`，conditional edge 用 `routes`。custom 注册后用 `ng.export_schema()` 导出 live schema，不另行维护 editor palette。absent/zero version 在 `0.x` 保持 lenient；`ng.upgrade_topology()` 把忽略 data 保留为无碰撞 annotation。

## fan-out 与管理

默认 worker_count `1` 不创建 engine pool。I/O branch 在 suspend 时仍可重叠；单 caller 线程的 CPU body 串行。需要 pool 时在并发执行前设置 `set_worker_count(N)`/`set_worker_count_auto()`。除非 native operation 释放 GIL，Python CPU callback 仍串行。

同 engine 的 run/resume 期间 admin state/history/update/fork 以 `std::logic_error` 失败。先 cancel/drain 并等待完成，不隐藏异常。同 thread_id 并发执行的 checkpoint 交错未规定；共享 store 的不同 engine 需要 host 协调。参见[并发](concurrency.md)。

## checkpoint 与 PostgreSQL failure

缺失 PostgresCheckpointStore export 可能是 wheel/source build 禁用了 component。检查该 artifact 配置，不照搬旧 wheel 功能表。构建可选 target 时安装匹配 libpq。URI password 特殊字符须 percent-encode，或用 libpq key=value 形式；不公开真实 credential。

async connect/reconnect 在全部 host/IP 使用一个 deadline。显式正数 `connect_timeout=N` 以秒计，`1` 向上取两秒。absent/zero/negative 以及仅经 PGCONNECT_TIMEOUT/service file 提供的值使用 async 默认 30 秒。不同于 sync libpq 的 per-host timeout。

允许时 store 创建 table；缺少 CREATE 权限须应用 [PostgreSQL header](../include/neograph/graph/postgres_checkpoint.h) 的 schema。没有 pending-write capability 意味着整个 super-step replay，不保证外部效果 exactly-once。async-only backend 用 AsyncCheckpointStore 加 `adapt_async_checkpoint_store`，不使用旧相互 sync/async crossover。

## tracing adapter 与历史修复

Tracer adapter 须拥有记录 data，不保留 session close 销毁的 Span wrapper raw pointer。参见 [C++ tracing 例](../examples/49_openinference.cpp)。Python contextvars 不自动跨越 C++ callback 边界；显式传入/attach parent context，在 engine compile 前安装 wrapper。

历史修复包括 v0.1.8 接受 top-level conditional_edges、删除 node 双执行 fallback、切换前 httplib macro layout 不一致（issue #16）。这些不恢复删除的 provider signature。跨 translation unit 实例化 header-only httplib 的 consumer 仍需一致定义 CPPHTTPLIB_OPENSSL_SUPPORT；当前 typed-provider HTTP 为 SDK libcurl。

opaque convenience vector property 可能不是 Python list。用 iteration/`list(value)` 检查；ChatMessage.image_urls 等复制 sequence 须 build-then-assign。不要假设修改返回 copy 会改变 C++ request；新 typed request/outcome 与旧 CompletionParams 示例分离。

## 安全报告 bug

提供 version/platform、最小 topology/call、execution_trace/status、去敏后的 typed failure/stop/usage 证据。说明 installed wheel 还是 rebuilt checkout。不包含 credential、private prompt、encoded native request body、未脱敏 packet capture。提交至 <https://github.com/fox1245/NeoGraph/issues>。
