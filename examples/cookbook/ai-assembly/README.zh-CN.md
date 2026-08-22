<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=zh-CN source_sha256=4922ec93b98cf57b8a7fc967e471974122e6b7608a53fc5f1b826cb01f3fd9b8 -->
# AI国民议会

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

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
cmake -S . -B build-cookbook \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

echo 'OPENROUTER_API_KEY=sk-or-...' > .env

bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

成员服务器会进行实时OpenRouter调用；需要`OPENROUTER_API_KEY`和网络访问。编译本身是离线的。

## Python演讲者变体（v0.2.1+，跨语言A2A）

相同的演讲者逻辑，约100行Python，针对相同的C++成员服务器——证明A2A协议可以干净地桥接不同语言：

```bash
pip install 'neograph-engine>=0.2.1'
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

Python A2A绑定（`neograph_engine.a2a`）在v0.2.1中发布。服务端（图作为A2A端点）目前仅限定为C++。

## 摩擦日记——新NeoGraph用户遇到的绊脚石


这些是构建过程中发现的粗糙边界。**所有四个问题在v0.2.1中均已修复**——留在这里作为记录。

### 1. A2A仅限C++——Python绑定并未暴露它（在v0.2.1中已修复）

`pip install neograph-engine`可以工作，但pre-v0.2.1的`neograph_engine`没有导出`A2AClient` / `AgentCard`。v0.2.1添加了`neograph_engine.a2a`子模块（客户端 + AgentCard + Task/Message/Part/TaskState/Role）——参见上面的Python演讲者变体。

**服务端绑定仍仅支持 C++**；A2AServer 需要一个 GIL 感知的生命周期合约，这将是 v0.3 的后续工作。

### 2. 无系统安装 / 轮子中无头文件（已在 README v0.2.1 中修复）

README 中现在有一个“Using NeoGraph from your CMake project”部分，展示了 `FetchContent_Declare` 模式。本指南（cookbook）也位于 NeoGraph 树内，因此可以直接 `add_executable` 使用，无需任何外部依赖——独立版本使用 FetchContent。

### 3. `OpenAIProvider::create()` `unique_ptr` 与 `shared_ptr` (FIXED in v0.2.1)

`OpenAIProvider::create_shared(cfg)` 已添加——直接返回`shared_ptr<Provider>`，以便能干净地捕获到`NodeFactory`闭包中。食谱在`member_server.cpp`的第~133行使用它。

### 4. `.env`自动加载不会传播到A2A子进程（已在v0.2.1中记录）

`cppdotenv::auto_load_dotenv()`在调用它的二进制文件内部正常工作，但派生子服务器的启动脚本必须先在父shell中`source .env`。现在已在[`docs/troubleshooting.md`](../../../docs/troubleshooting.md)中的“从源代码构建”下进行记录。

### 5. 运行平稳的项目（正面备注）

- `A2AServer::start_async` + 自动移植（`port=0`）毫无痛苦。
- AgentCard 发现（`fetch_agent_card`）直接可用——无需手动 HTTP。
- 来自`send_message_sync`的并发`std::async` futures——无需客户端锁，无共享会话状态。A2A 规范 / NeoGraph 都能开箱即用地干净处理并行客户端请求。
- `parse_vote` 对自由格式韩文文本的正则表达式有效，因为模型在被要求时可靠地遵循`vote: support/oppose/abstain`。角色输出保持在格式内，使其成为一个5行的计数函数。
- 树内 CMake 构建是自包含的；如上所示，使用`NEOGRAPH_BUILD_A2A=ON`和`NEOGRAPH_BUILD_LLM=ON`进行配置。

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
