<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=zh-CN source_sha256=6eb929ef5081e8b3789f4156c37b960dd7a91be4c7880451bdb74f562630297f -->
# AI国民议会

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**保留的 interface-3 限定范围运行证据。** 类型化 provider 迁移后，四个真实本地 A2A
成员服务器和 C++ 议长完成了离线会话。合成弃权是 fixture 输出，不是
模型判断或 vendor 推理。这不验证 live provider 调用或Python
议长 binding。

一个作为**全新NeoGraph用户**构建的玩具演示——所有API选择都是通过阅读公开文档（README、GitHub上的示例、Doxygen）做出的，从未打开过NeoGraph的源代码。目的有两点：证明A2A能用于真实的多角色场景，并揭示全新C++开发者在过程中遇到的摩擦。

## 功能

国民议会的四名成员位于不同的端口，每个都是一个A2A端点，背后有不同的角色提示词和固定的DeepSeek模型的相同OpenRouter路由。议长（国民议会议长）是一个单独的程序，通过NeoGraph的`A2AClient`向每个成员并行广播一项法案，解析每个成员的投票结果，并宣布结果。

```
                          ┌──────────────────┐
                          │  Speaker         │
                          │   A2AClient ×4   │
                          └─────────┬────────┘
                fetch_agent_card +    send_message_sync
            ┌──────────┬───────────┴───────────┬──────────┐
            ▼          ▼                       ▼          ▼
       :8101 Progress    :8102 Conservative  :8103 Center  :8104 Green
       Kim Jinbo         Park Bosu           Jung Jungdo   Na Noksaek
       (PersonaNode → OpenRouter DeepSeek, persona-specific system prompt)
```

每个成员都是一个单节点NeoGraph（`__start__ → persona → __end__`），由`a2a::A2AServer`提供服务。该图读取一个`prompt`通道并写入一个`response`通道；A2A服务器的默认`GraphAgentAdapter`通过JSON-RPC暴露这些数据。

当前 client 从 AgentCard 选择兼容 JSON-RPC 0.x/1.0 interface；server 广告两个 dialect，
通过 `A2A-Version` 选择 response encoding。初始 SSE task snapshot 是进度，不是完成回答。
card-selected 请求不进行 dialect fallback，已交付 event 不重发。
此前 offline session 不验证保留的1.0 wire 变更。

## 实时记录（通过OpenRouter的DeepSeek，2026年4月29日）

法案：[`bills/basic_income.txt`](bills/basic_income.txt) — 普遍基本收入，每月500,000韩元，由土地税、碳税和累进税资助。

```
[Speaker of the National Assembly] Bill submission: [National Basic Income Law]

[Progress Kim Jinbo]   Protecting socially vulnerable groups + asset/carbon taxation = alignment        → Support
[Conservative Park Bosu]   200 trillion mandatory spending + market distortion + real estate shock    → Oppose
[Center Jung Jungdo]   Acknowledging intent but excessive amount; suggests phased reduction amendment  → Oppose
[Green Na Noksaek]   Carbon tax + unearned income taxation + equitable distribution                    → Support

[Speaker of the National Assembly] Vote result:  2 in favor  /  2 opposed  /  0 abstention
[Speaker of the National Assembly] Tie vote — the bill is rejected (custom).
```

每个人物的推理确实遵循其政党声明的价值观。这不是框架的功劳——这是固定的模型遵循不同的系统提示词的结果——但议会机制（并行A2A、投票统计、发现）完全是NeoGraph的功能。

## 构建和运行（在NeoGraph树中）

```bash
# from NeoGraph repo root; A2A and LLM are optional build components
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -S . -B build-cookbook \
    -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

# Offline fixture: no .env loading, credentials or provider transport.
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh --mock
# Live: privately configure OPENROUTER_API_KEY in the environment or .env.
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

先安装typed SchemaProvider CMake包及构建依赖。这是集成NeoGraph目标，不提供standalone项目。`NEOGRAPH_BUILD_DIR`选择二进制；未设置时依次查找`build-pybind`、`build`、recipe的`build`。`--mock`生成合成弃权，不是模型判断或使用量。live将法案/角色提示发往OpenRouter，需要有效密钥、网络和余额；四个成员调用产生费用，不保证固定成本。私密保管密钥、`.env`、提示和输出记录，不公开raw native记录。A2A仅传递portable回复。C++使用typed `ProviderRequest`/SDK事件和完整不可变`sp::Outcome`；projection不授予native replay权限。这是源码迁移记录，不是新的执行验证。

## Python演讲者变体（v0.2.1+，跨语言A2A）

`speaker.py` 使用 `neograph_engine.a2a`，不使用 provider subclass API。请使用从本 checkout 构建的 binding；旧公开 wheel 不证明与当前源码兼容。保留的 C++ 运行未执行 Python speaker。

```bash
# Build/install this checkout's Python binding; see docs/python-binding.md.
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

v0.2.1 binding 是历史 release 结果，不是当前验证。当前 Python speaker 与 C++ caller 共用 application policy：实际 terminal/interrupted agent status text 优先，其次首个 artifact text，最后非空的最后 agent history text。以 `ng.a2a.Role.Agent` 识别 agent message，提交的 user bill 不能成为 member 回复。这是 application 答案选择规则，不是通用 A2A 优先级或新的 runtime 通过。

## 摩擦日记——新NeoGraph用户遇到的绊脚石


以下为切换前历史摩擦记录，不代表当前legacy API可用。上方live记录也是历史记录。

### 1. A2A仅限C++——Python绑定并未暴露它（在v0.2.1中已修复）

历史 v0.2.1 添加了 Python A2A client。当前源码仍公开该 client；运行验证须通过当前 Python 运行单独确认。

### 2. 无系统安装 / 轮子中无头文件（已在 README v0.2.1 中修复）

历史README说明了FetchContent。此recipe只有集成目标，没有standalone CMake项目。需要SchemaProvider。

### 3. Provider 所有权 (`unique_ptr`与`shared_ptr`，v0.2.1解决)

之前的 release 解决了 provider 所有权问题。当前 recipe 使用 `examples::make_openrouter_provider` 和 typed outcome。

### 4. `.env`自动加载不会传播到A2A子进程（已在v0.2.1中记录）

`cppdotenv::auto_load_dotenv()`在调用它的二进制文件内部正常工作，但派生子服务器的启动脚本必须先在父shell中`source .env`。现在已在[`docs/troubleshooting.md`](../../../docs/troubleshooting.md)中的“从源代码构建”下进行记录。

### 5. 运行平稳的项目（正面备注）

- `A2AServer::start_async` + 自动移植（`port=0`）毫无痛苦。
- AgentCard 发现（`fetch_agent_card`）直接可用——无需手动 HTTP。
- 来自`send_message_sync`的并发`std::async` futures——无需客户端锁，无共享会话状态。A2A 规范 / NeoGraph 都能开箱即用地干净处理并行客户端请求。
- `parse_vote` 对自由格式韩文文本的正则表达式有效，因为模型在被要求时可靠地遵循`vote: support/oppose/abstain`。角色输出保持在格式内，使其成为一个5行的计数函数。
- 这是历史in-tree构建体验记录。当前前提见上文。

## 文件

```
ai-assembly/
├── member_server.cpp           # one configurable persona server
├── speaker.cpp                 # orchestrator, broadcasts bill, tallies
├── speaker.py                  # Python A2A client variant
├── prompts/
│   ├── jinbo.txt               # Kim Jinbo (Progress)
│   ├── bosu.txt                # Park Bosu (Conservative)
│   ├── jungdo.txt              # Jung Jungdo (Center)
│   └── nokdang.txt             # Na Noksaek (Green)
├── bills/
│   └── basic_income.txt        # sample bill: National Basic Income Law
└── scripts/
    └── run_session.sh          # spin up 4 members + run speaker
```

## 许可证

MIT，与 NeoGraph 相同。
