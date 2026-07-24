<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=zh-CN source_sha256=828f35d27b957d55c8c766d3ce714ae4094397f9f2d4f0cabea710750619cb9a -->
# AI 国会

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这是一个玩具演示，构建时刻意把自己当作**刚接触 NeoGraph 的新用户** —
每个 API 选择都只通过阅读公开文档（README、GitHub 上的 examples、Doxygen）完成，
从未打开 NeoGraph 源码。目的有两个：证明 A2A 可用于真实的多 persona 场景，
并暴露全新的 C++ 开发者一路上会遇到的摩擦点。

## 它做什么

四名国会议员监听在不同端口上，
每一个都是由不同 persona prompt 和同一个 OpenAI 模型（`gpt-5.4-mini`）支撑的 A2A endpoint。
Speaker（国会议长）是一个独立程序，通过 NeoGraph 的 `A2AClient`
并行向每位成员广播一份法案，从回复中解析每位成员的投票，
并宣布结果。

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
       (PersonaNode → OpenAI gpt-5.4-mini, persona-specific system prompt)
```

每位成员都是一个单节点 NeoGraph（`__start__ → persona → __end__`），
由 `a2a::A2AServer` 提供服务。graph 读取 `prompt` channel 并写入 `response` channel；
A2A server 默认的 `GraphAgentAdapter` 会通过 JSON-RPC 暴露这些 channel。

## 现场转录（gpt-5.4-mini，2026-04-29）

法案：[`bills/basic_income.txt`](bills/basic_income.txt) — 普遍
基本收入，每月 500,000 韩元，由土地税 + 碳税 + 累进税资助。

```
[Speaker of the National Assembly] Bill submission: [National Basic Income Law]

[Progress Kim Jinbo]   Protecting socially vulnerable groups + asset/carbon taxation = alignment        → Support
[Conservative Park Bosu]   200 trillion mandatory spending + market distortion + real estate shock    → Oppose
[Center Jung Jungdo]   Acknowledging intent but excessive amount; suggests phased reduction amendment  → Oppose
[Green Na Noksaek]   Carbon tax + unearned income taxation + equitable distribution                    → Support

[Speaker of the National Assembly] Vote result:  2 in favor  /  2 opposed  /  0 abstention
[Speaker of the National Assembly] Tie vote — the bill is rejected (custom).
```

每个 persona 的推理都确实贴合其政党声明的价值观。
这不是框架的功劳 — 只是 OpenAI 遵循了不同的 system prompt —
但议会机制（并行 A2A、计票、发现）完全由 NeoGraph 提供。

## 构建并运行（在 NeoGraph 源码树中）

```bash
# from NeoGraph repo root
cmake --build build-pybind --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

echo 'OPENAI_API_KEY=sk-...' > .env

bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

## Python speaker 变体（v0.2.1+，跨语言 A2A）

同一个 speaker 逻辑，用约 100 行 Python 实现，对接同一组
C++ 成员服务器 — 证明 A2A 协议可以干净地跨语言桥接：

```bash
pip install neograph-engine          # >= 0.2.1
# (start the C++ members in another terminal as above)
PYTHONPATH=build-pybind python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

Python A2A binding（`neograph_engine.a2a`）随 v0.2.1 发布。
服务器端（graph-as-A2A-endpoint）目前仍然只支持 C++。

## 摩擦点记录 — NeoGraph 新用户踩到的问题

这些是构建本 cookbook 时发现的粗糙边缘。**四个都已在 v0.2.1 修复** —
保留在这里作为记录。

### 1. A2A 曾经只支持 C++ — Python binding 没有暴露它（已在 v0.2.1 修复）

`pip install neograph-engine` 可以工作，但 v0.2.1 之前的 `neograph_engine`
没有导出 `A2AClient` / `AgentCard`。v0.2.1 新增了
`neograph_engine.a2a` 子模块（client + AgentCard + Task/Message/
Part/TaskState/Role）— 见上面的 Python speaker 变体。

**服务器端 binding 仍然只支持 C++**；A2AServer 需要一个
感知 GIL 的生命周期约定，这是 v0.3 的后续工作。

### 2. 没有系统安装 / wheel 中没有头文件（已在 README v0.2.1 修复）

README 现在有一个“从你的 CMake 项目使用 NeoGraph”小节，
展示 `FetchContent_Declare` 模式。本 cookbook 也位于 NeoGraph 源码树内，
所以它可以直接 `add_executable`，不需要任何外部依赖 — 独立变体使用 FetchContent。

### 3. `OpenAIProvider::create()` 的 `unique_ptr` vs `shared_ptr`（已在 v0.2.1 修复）

新增了 `OpenAIProvider::create_shared(cfg)` — 它直接返回
`shared_ptr<Provider>`，因此能被干净地捕获进
`NodeFactory` 闭包。本 cookbook 在 `member_server.cpp`
约第 133 行使用它。

### 4. `.env` 自动加载不会传播到 A2A 子进程（已在 v0.2.1 记录）

`cppdotenv::auto_load_dotenv()` 在调用它的二进制文件内部有效，
但 fork 子服务器的启动脚本必须先在父 shell 中 `source .env`。
现在已记录在 [`docs/troubleshooting.md`](../../../docs/troubleshooting.md) 的
“从源码构建”下。

### 5. 顺利工作的部分（正面记录）

- `A2AServer::start_async` + auto-port（`port=0`）很省心。
- AgentCard 发现（`fetch_agent_card`）直接可用 — 不需要手写 HTTP。
- 来自 `std::async` futures 的并发 `send_message_sync` — 不需要
  客户端侧锁，也没有共享会话状态。A2A spec /
  NeoGraph 都能开箱即用地干净处理并行客户端请求。
- 对自由格式韩文文本使用 `parse_vote` regex 能工作，是因为模型
  在被要求时可靠遵守 `vote: support/oppose/abstain`。Persona 输出
  保持在格式内，使它成为一个 5 行计票函数。
- 构建很干净 — FetchContent 拉取 v0.2.0，不需要手动依赖
  安装。原版 Ubuntu 上的 OpenSSL/CURL 就足够了。

## 文件

```
ai-national-assembly/
├── CMakeLists.txt              # FetchContent NeoGraph v0.2.0
├── src/
│   ├── member_server.cpp       # one binary, configurable persona
│   └── speaker.cpp             # orchestrator, broadcasts bill, tallies
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
