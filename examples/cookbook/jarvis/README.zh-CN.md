<!-- neograph-i18n: source=examples/cookbook/jarvis/README.md locale=zh-CN source_sha256=19709bb07c36265ac28bd757009fd525505ef7bcd930e8d1b5ee6b763c4ff454 -->
# JARVIS — 语音驱动的元编排器

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> 零云依赖，运行在单台 Raspberry Pi 上。
> 麦克风是 Tony，NeoGraph 是 JARVIS，工具和专家是 JARVIS 的下属。

本 cookbook **不是**“语音 TTS 示例”。它演示 NeoGraph 的
多代理基本能力 — MCP 工具、双向 A2A、异步并行、Store 记忆、
ReAct 子图 — 如何用一句语音**编织在一起**。

## 为什么这是 JARVIS

电影里的 JARVIS 不只是带语音 TTS 的聊天机器人。JARVIS 同时做五件事：

1. 在 Tony 说完之前抓住意图 — **快速意图分类**
2. 能直接回答就直接回答，否则委托给下属 — **4 路路由**
3. 同时收集多条信息 — **并行扇出**
4. 记得昨天的对话 — **长期记忆**
5. 可以被其他 JARVIS / 系统调用 — **双向 A2A**

所以本 cookbook 的核心是**图的形状**，不是语音。语音只是
输入/输出外壳；真正创造“JARVIS 感”的是 NeoGraph 的编排引擎。

## 完整图

```
                          ┌────────────────────────┐
                          │ Background triggers    │
                          │ (timer / external events)│ ── A2A server for
                          └───────────┬────────────┘     JARVIS calls go here
                                      │
 [Microphone]──[VAD]──[whisper.cpp STT]──[memory_lookup]──[intent_router]
    miniaudio                          ▲                   │
                                       │ Store             │
                                       │ (conversation accumulation)│
                                       │                   │ Router makes 4-way decision
                                       │                   │ (chat goes directly to synthesizer)
                                       │                   │
                           ┌───────────┴───────────────────┴───────────────┐
                           │                                                │
                   [direct_branch]        [delegate_branch]        [parallel_branch]
                        │                       │                       │
               MCP tool single call        Delegate to expert entirely       Send / fan-out
               (time, weather, memo, etc.)    (coder, researcher, ...)     to multiple tools simultaneously
                        │                       │                       │
                        └───────────────────────┼───────────────────────┘
                                                │
                                        [response_synth]
                                        (synthesize natural response with large LLM)
                                                │
                                                ↓
                                   [supertonic TTS] ──→ [Speaker]
                                   (in detected language)     miniaudio
```

## 两个目录 JSON 文件 — JARVIS 的“我能做什么”

JARVIS 启动时会读取两个文件并构建能力列表。
**这意味着你可以在不重新编译代码的情况下增加/移除能力。**

### `config/mcp_catalog.json` — 工具

这是 JARVIS 可以直接调用的函数型工具列表。
每个条目对应一个 MCP server（HTTP 或 stdio）。

```json
{
  "tools": [
    {
      "name": "time_weather",
      "transport": "http",
      "url": "http://127.0.0.1:8000",
      "description": "Short, immediate-answer information like current time, weather, exchange rates",
      "enabled": true
    },
    {
      "name": "personal_memo",
      "transport": "stdio",
      "command": ["python3", "examples/demo_mcp_stdio_server.py"],
      "description": "Tony's personal memo storage/retrieval",
      "enabled": true
    }
  ]
}
```

启动时，它会对每个 MCP server 调用 `get_tools()` → 合并 tool definitions，
并把它们作为“可用工具”注入 router 的 system prompt。

### `config/agent_registry.json` — 专家（A2A）

JARVIS 可以把整项任务委托给这些子代理。每个都作为 A2A endpoint
运行在独立进程/机器中。

```json
{
  "agents": [
    {
      "name": "coder",
      "url": "http://127.0.0.1:8210",
      "expertise": "Code writing, review, debugging",
      "fetch_card_on_start": true
    },
    {
      "name": "researcher",
      "url": "http://127.0.0.1:8211",
      "expertise": "Web search + summarization, academic paper organization",
      "fetch_card_on_start": true
    }
  ]
}
```

启动时，它会从每个 URL 请求 `AgentCard` → 只激活那些有响应的代理。
**关键技巧**：任何遵循 A2A 标准的外部代理 — 不管是别人写的 Python
A2A bot、另一个 NeoGraph 实例等 — 只要把 URL 加到这个 JSON，
就能成为 JARVIS 的下属。

## Router（意图分类）— JARVIS 的大脑

对固定的 DeepSeek 模型（`~deepseek/deepseek-v4-flash-latest`）进行一次调用会返回：

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat` — 不使用工具或委托。Synthesizer 使用自己的知识 + 对话记忆直接回答。
  问候、自我介绍、闲聊、“我之前说了什么？”这类对话回忆。如果 router
  发明了目录中不存在的工具/代理，验证阶段会降级到此 mode。
- `direct` — 单次工具调用。如果结果很简单（`"3:30 PM"`），用 `skip_synthesis=true`
  跳过合成，直接进入 TTS。**很快。**
- `delegate` — 完全委托给 `delegate_to` 指向的 A2A endpoint。
  拿到结果后，只为语音合成一行摘要。
- `parallel` — 多个 `tool_calls`。使用 NeoGraph 的 `make_parallel_group`
  同时执行，归约器把结果组合后交给 Synthesizer。

### 为什么分离 Router 和 Synthesizer

如果所有东西都通过一个大型 LLM + ReAct，每轮会花 1-3 秒，直接毁掉 JARVIS 感。
- Router：小模型，约 200ms，单个 JSON
- Synthesizer：大模型，约 800-1500ms，单条自然语言响应
- 如果工具提供即时答案，跳过 synthesizer → 响应约 500ms 开始

电影中 JARVIS 的快速响应节奏来自这种分离。

## Memory (`Store`)

每轮开始时，`memory_lookup` node 从 NeoGraph `Store` 拉取最近 N 轮 + 用户偏好
（`tony.prefers.language=ko`、`tony.last_topic=...`）。

每轮结束时，JARVIS 会把响应 + Tony 的发言 + 已用工具推入 Store。
下一轮的 router 就可以解析“我前面提到的那个东西”这类指代。`JsonFileStore`
会持久化到文件 — 重启后仍能记得。空轮次（STT 失败 / 噪声）不会提交，
以防记忆污染。`prefs.native_lang` 维护估计出的母语
（语言一致性）。

## 双向 A2A — JARVIS 调用别人，也被别人调用

- **调用别人**：通过来自 `agent_registry.json` 的 `A2AClient` 委托给专家。
- **被别人调用**：JARVIS 自身暴露一个 `A2AServer`（端口 8200）。
  - 外部系统可以通过 `POST /v1/messages` 向 JARVIS 发送文本消息。
  - 移动应用、其他 NeoGraph 实例，甚至另一个 JARVIS 都可以调用它。
  - 文本输入会跳过麦克风/STT 阶段，直接进入 router。

**JARVIS 到 JARVIS 通信演示**：Home JARVIS (8200) ↔ Office JARVIS (8201)。
“从办公室 JARVIS 获取今天的会议纪要” → Home JARVIS 通过 A2A 调用 Office JARVIS
→ 响应通过语音传递给 Tony。

## 后台触发器（主动）

一个独立异步图在后台运行：
- 定时器（每 5 分钟检查日历）
- 外部事件（家庭传感器、收到邮件）
- 外部 A2A 调用

当事件发生时，它会把消息注入 JARVIS 的主图 → JARVIS 会在 Tony 询问前开口。
("Sir, meeting in 10 minutes.")

完全使用 NeoGraph 的 `27_async_concurrent_runs.cpp` 模式。

## 目录结构

```
jarvis/
├── README.md                      ← This document
├── CMakeLists.txt                 External dependencies (whisper/onnxruntime/miniaudio) gated
├── config/                        Default config (graph · catalog · registry · persona)
├── config-demo/                   Execution preset (real-tools / mock)
├── config-bench*/                 Benchmark config
├── src/
│   ├── main.cpp                   Entry point (node registration · graph compilation · main loop)
│   ├── audio/                     miniaudio capture (+Silero VAD) · playback, supertonic TTS
│   ├── stt/                       whisper_node (multi-language · language consistency) + moonshine_node (edge)
│   ├── orchestrator/              Router, MCP catalog loader, A2A dispatcher
│   └── memory/                    Store-based conversation memory (JsonFileStore persistence)
├── specialists/                   coder / researcher (separate A2A servers)
├── bench/                         NeoGraph vs LangGraph benchmark (twin · driver · Docker)
├── assets/download.sh             Download whisper/supertonic/moonshine/silero models
├── scripts/
│   ├── run_jarvis.sh              Execution wrapper (LD_LIBRARY_PATH · ROCm · dxg auto)
│   ├── jarvis_repl.py             Korean readline REPL (text/wav input)
│   ├── build_whisper_hip.sh       Build whisper.cpp ROCm/HIP GPU
│   └── demo_mcp_server.py         Demo MCP server (time/weather/calc)
└── docs/architecture.md          Detailed node-by-node graph explanation
```

## 构建 / 运行

```bash
# 1. Download models (whisper-large-v3-turbo ~1.6GB + supertonic + silero VAD)
#    Lightweight: JARVIS_WHISPER=small bash assets/download.sh  (Raspberry Pi / CPU)
bash examples/cookbook/jarvis/assets/download.sh

# 2. Build — onnxruntime, whisper.cpp, miniaudio found on system (or mock if missing)
cmake -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON
cmake --build build-jarvis --target cookbook_jarvis -j

# 3a. Run — text/wav input (Korean line-edit REPL recommended)
cd examples/cookbook/jarvis
python3 scripts/jarvis_repl.py                 # Automatically loads OPENROUTER_API_KEY from .env
#   Tony ▸ Hello?                                # Text
#   Tony ▸ wav:/path/to/audio.wav                # Audio file → STT

# 3b. Run — live microphone (miniaudio capture + Silero VAD)
JARVIS_MIC=1 bash scripts/run_jarvis.sh config-demo/real-tools
#   "Online" appears → speak → voice end detection → STT → response → TTS

# (Demo MCP server for tools — separate terminal)
python3 scripts/demo_mcp_server.py 8888        # Time/weather/calc
```

实时 provider 固定为使用 DeepSeek 模型的 OpenRouter，由 `.env` 中的
`OPENROUTER_API_KEY` 启用。没有 key 时使用 MockProvider（echo）离线运行。

## 语音栈详情

### 实时麦克风（miniaudio + Silero VAD）
`JARVIS_MIC=1` 或配置 `use_microphone:true`。采集工作线程在 512-sample window
上运行 Silero VAD，用于检测语音开始/结束（200ms 预滚，500ms 静音结束）。
**背压**：推理期间丢弃采集内容，以阻止 TTS 回声、过期发言
和起始噪声。设备故障（WSL2 麦克风断开等）会自动回退到 stdin。
调优：`JARVIS_VAD_THRESHOLD`（默认 0.5），观察：`JARVIS_MIC_DEBUG=1`。

### STT — 两个选项（通过配置 `stt.type` 切换）
- **`whisper_stt`**（默认）: whisper.cpp。`language:"auto"` 自动检测 99 种语言
  → **用说话者的语言回答并 TTS**。**语言一致性**：在 store.prefs 中维护母语，
  这样短发言被误识别为外语时不会突然切换（需要持续误识别才会切换）。
- **`moonshine_stt`**: Moonshine-tiny ONNX（27M，与 supertonic 共享 ORT）。
  边缘端、低延迟、韩语风格。特定语言模型，所以语言固定。

### GPU 加速（whisper.cpp ROCm/HIP）
捆绑的 whisper.cpp 是仅 CPU 版本 — large 在 CPU 上需要约 32s（11s 音频片段）。AMD GPU
（gfx1201=R9700，ROCm≥7.2）运行 `bash scripts/build_whisper_hip.sh` 做 GGML_HIP
构建 → **约 7s（4.5×）**。run_jarvis.sh 会自动加载 ROCm runtime 和 WSL dxg。

## 基准测试 — NeoGraph vs LangGraph (`bench/`)

在 LangGraph（Python 对照版 `langgraph_twin.py`）中镜像相同拓扑
（麦克风→STT→合并→记忆→router→4 路→合成/跳过→提交→TTS），
并在相同约束（`--cpus=2 --memory=2g`）容器中测量。

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 实现状态

**完全可用** — 已在真实硬件上验证 OpenRouter DeepSeek 的实时语音单轮。
麦克风→VAD→STT→router→4-way→synth→TTS 全链路 +

已知限制 / 下一版本：
- **不支持打断** — TTS 播放期间的发言会通过背压丢弃
  （v2 将加入 cancel token）。
- **未应用流式 STT** — 发言完成后批量转录。Moonshine v2
  ergodic encoder 逐块流式处理是下一个候选。
- **多说话人 · 长记忆压缩** — 单说话人假设，24 轮限制。
- **后台触发器（主动）** — 已设计但未实现。

## 许可证 / 外部依赖

| 库 | 许可证 | 作用 |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) | MIT | TTS（99M，ONNX，31 种语言） |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | MIT | STT（99 种语言自动检测，CPU/ROCm） |
| [Moonshine](https://github.com/moonshine-ai/moonshine) | MIT | 边缘端 STT 选项（27M ONNX） |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / 公有领域 | 麦克风采集 + 扬声器播放 |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT | 语音开始/结束检测（ONNX） |
| ONNX Runtime | MIT | supertonic·moonshine·VAD 推理 |
