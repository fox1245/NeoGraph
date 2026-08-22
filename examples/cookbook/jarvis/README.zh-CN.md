<!-- neograph-i18n: source=examples/cookbook/jarvis/README.md locale=zh-CN source_sha256=e52a150fd89075b66a0022d867def85dca59b234e1fc2e664a953c21f6625b10 -->
# JARVIS — 语音驱动的元编排器

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> 云端零依赖，单台树莓派即可运行。
> 麦克风是Tony，NeoGraph是JARVIS，工具/专家是JARVIS的下属。

本手册**不是**一个“语音TTS示例”。它展示了NeoGraph的多智能体原语——MCP工具、双向A2A、异步并行、Store记忆、ReAct子图——**通过一条语音无缝编织在一起**。

## 为什么这是JARVIS

电影中的JARVIS不仅仅是一个带语音TTS的聊天机器人。JARVIS同时做五件事：

1. 在Tony说完之前捕捉意图——**快速意图分类**
2. 能直接回答就回答，否则委托给下属——**4路路由**
3. 同时收集多条信息——**并行fan-out**
4. 记住昨天的对话——**长期记忆**
5. 可以被其他JARVIS/系统调用——**双向A2A**

因此本手册的核心是**拓扑**，而非语音。语音只是输入/输出外壳；创造“JARVIS感”的是NeoGraph的编排引擎。

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

## 两个目录JSON文件——JARVIS的“我能做什么”

当JARVIS启动时，它会读取两个文件并构建其能力清单。**这意味着你可以无需重新编译代码即可添加/移除能力。**

### `config/mcp_catalog.json` — 工具

JARVIS可以直接调用的函数型工具列表。每个条目对应一个MCP服务器（HTTP或stdio）。

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

启动时，它在每个MCP服务器上调用`get_tools()` → 合并工具定义并将其作为“可用工具”注入路由器的系统提示中。

### `config/agent_registry.json` — 专家（A2A）

JARVIS可以将整个任务委托给这些子智能体。每个子智能体作为独立进程/机器上的A2A端点运行。

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

启动时，它向每个URL请求`AgentCard` → 仅激活那些响应的端点。**关键技巧**：任何遵循A2A标准的外部智能体——无论是别人制作的Python A2A机器人、另一个NeoGraph实例，还是其他——只需将其URL添加到此JSON中，即可成为JARVIS的下属。

## 路由器（意图分类）——JARVIS的大脑

对固定的DeepSeek模型（`~deepseek/deepseek-v4-flash-latest`）进行一次调用返回：

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat`——无工具或委托。合成器使用其自身知识加对话记忆直接作答。问候、自我介绍、闲聊、“我之前说了什么”式的对话回忆。如果路由器从目录中凭空生成目录中不存在的工具/智能体，校验阶段会降级到此模式。
- `direct` — 单个工具调用。如果结果简单（`"3:30 PM"`），跳过合成，直接使用`skip_synthesis=true`进入TTS。**快速。**
- `delegate` — 完全委托给由`delegate_to`指向的A2A端点。获取结果后，仅为语音合成一行摘要。
- `parallel` — 多个`tool_calls`。使用NeoGraph的`make_parallel_group`同时执行，归约器(reducer)结合结果用于合成器。

### 为何将路由器与合成器分离

通过一个大型LLM配合ReAct运行所有内容，每轮需要1-3秒，会破坏JARVIS的体验感。
- 路由器：小型模型，约200毫秒，单个JSON
- 合成器：大型模型，约800-1500毫秒，单个自然语言响应
- 如果工具提供即时答案，跳过合成器 → 响应约在500ms内开始

电影JARVIS中快速的响应时机正是来自于这种分离。

## 记忆（`Store`）

在每轮开始时， `memory_lookup` 节点从 NeoGraph 中拉取最近 N 轮对话 + 用户偏好（`tony.prefers.language=ko`, `tony.last_topic=...`） `Store`.

在每一轮结束时，JARVIS将响应、Tony的话语以及使用的工具推送到Store。下一轮的路由器可以解析诸如“我之前提到的那个东西”之类的引用。`JsonFileStore`持久化到文件——跨重启保持记忆。空轮（STT失败/噪声）被排除在提交之外，以防止记忆污染。`prefs.native_lang`维护估算的母语（语言一致性）。

## 双向A2A——JARVIS呼叫与被呼叫

- **调用**：通过`A2AClient`从`agent_registry.json`委托给专家。
- **被呼叫**：JARVIS自身暴露一个`A2AServer`（端口8200）。
  - 外部系统可以通过`POST /v1/messages`向JARVIS发送文本消息。
  - 移动应用、其他 NeoGraph 实例，甚至另一个 JARVIS 都可以调用它。
  - 文本输入跳过麦克风/STT阶段，直接进入路由器。

**JARVIS到JARVIS通信演示**：家庭JARVIS（8200）↔ 办公室JARVIS（8201）。“从办公室JARVIS获取今天的会议纪要”→ 家庭JARVIS通过A2A呼叫办公室JARVIS → 响应以语音方式传达给用户。

## 后台触发器（主动式）

一个独立的异步图在后台运行：
- 定时器（每5分钟检查日历一次）
- 外部事件（家庭传感器、邮件接收）
- 外部 A2A 调用

当事件发生时，它会向JARVIS的主图注入一条消息 → JARVIS在托尼提问之前开口。（“先生，10分钟后有会议。”）

完全使用NeoGraph的`27_async_concurrent_runs.cpp`模式。

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

## 构建/运行

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

实时提供程序固定为 OpenRouter，并带有固定的 DeepSeek 模型和 `OPENROUTER_API_KEY`（位于 `.env` 中）。如果没有密钥，它将使用 MockProvider（回显）离线运行。

## 语音栈详情

### 实时麦克风（miniaudio + Silero VAD）
`JARVIS_MIC=1` 或配置 `use_microphone:true`。Capture 捕获 worker 线程在 512-sample 窗口上运行 Silero VAD，检测语音开始/结束（200ms 预滚动，500ms 静默结束）。**背压**：推理期间丢弃捕获的音频，阻止 TTS 回声、过时话语和启动噪声。设备故障（例如 WSL2 麦克风断开）时自动回退到 stdin。调优：`JARVIS_VAD_THRESHOLD`（默认 0.5），观察：`JARVIS_MIC_DEBUG=1`。

### STT — 两个选项（通过配置`stt.type`切换）
- **`whisper_stt`**（默认）：whisper.cpp。`language:"auto"`自动检测99种语言 → **以说话者的语言进行回答和TTS**。**语言一致性**：在store.prefs中保持母语，因此被误识别为外语的短话语不会突然切换（需要一致的误识别才能切换）。
- **`moonshine_stt`**：Moonshine-tiny ONNX（27M，与supertonic共享ORT）。边缘、低延迟、韩语风格。特定语言模型，因此语言是固定的。

### GPU加速（whisper.cpp ROCm/HIP）
捆绑的whisper.cpp仅为CPU — 大型模型在CPU上大约需要32秒（11秒片段）。AMD GPU（gfx1201=R9700，ROCm≥7.2）运行`bash scripts/build_whisper_hip.sh`以进行GGML_HIP构建 → **约7秒（4.5×）**。run_jarvis.sh自动加载ROCm运行时和WSL dxg。

## 基准测试 — NeoGraph vs LangGraph（`bench/`）

在LangGraph（Python孪生`langgraph_twin.py`）中镜像相同的拓扑（mic→stt→merge→memory→router→4-way→synth/skip→commit→tts），在相同约束（`--cpus=2 --memory=2g`）容器中测量。

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 实施状态

**完全可用** — 已在真实硬件上验证实时语音单轮运行（OpenRouter DeepSeek）。麦克风→VAD→STT→路由器→4路→合成→TTS完整链路 +

已知限制 / 下一版本。
- **不支持打断（Barge-in）** — TTS播放期间的语音通过背压被丢弃（将在v2中添加取消令牌）。
- **未应用流式STT** — 在语音段结束后进行批量转录。Moonshine v2遍历编码器逐块流式处理是下一个候选方案。
- **多说话人·长记忆压缩** — 单一说话人假设，24轮限制。
- **后台触发（主动触发）** — 已设计但未实现。

## 许可证 / 外部依赖

| 库 | 许可证 | 角色 |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) | MIT | TTS（99M，ONNX，31种语言） |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | MIT | STT（99种语言自动检测，CPU/ROCm） |
| [Moonshine](https://github.com/moonshine-ai/moonshine) | MIT | Edge STT 选项（27M ONNX） |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / 公共领域 | 麦克风采集 + 扬声器播放 |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT | 语音开始/结束检测（ONNX） |
| ONNX Runtime | MIT | supertonic·moonshine·VAD 推理 |
