<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=zh-CN source_sha256=ed7f628ae6ae172e9e2cbd3d681d4880b503f251e10b745e7be089fbcdc42200 -->
# JARVIS Graph：逐节点详解

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

## 带类型 provider 迁移：源码状态

C++ 的 router、synthesizer 和 specialist 测试夹具，使用带类型的 `ProviderRequest`、`sp::Message` 和 `sp::Event`；provider 返回的是完整、不可变的 `sp::Outcome` 值（`sp::runtime::Result`），而不是旧式的响应字符串。`src/provider_support.h` 实现了本地的 Jarvis/coder/researcher mock 配置：确定性的 router JSON、原样回显用户文本，以及一条明确标明为模拟的 researcher 回复。这些夹具既不需要凭据，也不需要网络 provider，但也不会给出真实的调研结果或专家推理。下文提到的现有配置/profile 路径，这次文档更新既没有修复，也没有创建。

本地语音是可选的，需要选定的 whisper/Moonshine 模型、ONNX Runtime/Supertonic 资源、miniaudio，以及可用的麦克风和扬声器；文本/mock 模式能跑通，并不能证明语音可用。“无需云端”只适用于本地/mock 运行。实时请求需要有授权的 `OPENROUTER_API_KEY`、网络访问和 provider 的容量，并且会把提示词、对话记忆，以及附带的工具/委派结果发送给 OpenRouter。实时使用的模型是固定的；原生请求设置了 ZDR，但这不等于保证数据驻留在特定地域。密钥不要写进日志，也不要提交到版本控制。provider 返回的 token 用量是可为空的计量数据，不是账单金额：要算费用，还需要当前 endpoint/模型的定价和计费用量。

`[jarvis:ttft]` 在第一个非空的 `sp::PartDelta` 出现时输出，条件是它的类型为 `PartKind::Text`、通道为 `DeltaChannel::Content`；用量、推理、响应头等其他事件不会触发它。它测的是第一段合成文本出现的时间，不是 TTS 开始发声的时间。Python REPL 驱动仍然只是协议客户端；pybind 基准测试使用已迁移的带类型绑定，需要另行运行并留下证据。保留下来的 C++ 证据覆盖 CLI 问候、模拟的记忆持久化和正常的 EOF。下文的语音/实时耗时是历史数据，不能证明迁移后的路径合格。

`config/jarvis_graph.json` 里每个节点做什么，以及它为什么放在那个位置。
建议对照 README.md 里的图一起看。

## 一轮对话的生命周期

```
T0  Microphone active, Tony utterance start detected
T1  Utterance end (VAD detects 500ms silence)
T2  STT complete — text + detected language code
T3  Memory lookup complete — last 6 turns + preferences + last topic
T4  Router decision complete — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way branch complete — one of self-response(chat) / direct tool / delegation / parallel done
T6  Response synthesis complete (or skip branch bypassed)
T7  Memory commit complete
T8  TTS first chunk playback starts ← point where Tony "starts hearing" the response
T9  TTS last chunk playback complete, microphone reactivated waiting
```

T0→T8 就是 JARVIS 给人的响应时间。目标分布：
- 短回答（direct + skip_synthesis）：T0→T8 ≈ 1.0-1.5s
- 普通回答（direct + synth）：T0→T8 ≈ 2.0-3.0s
- 委派（delegate）：T0→T8 ≈ 3.0-8.0s（取决于专家的处理时间）
- 综合回答（parallel + synth）：T0→T8 ≈ 2.5-4.0s

## 逐节点详解

### mic_capture（`voice_in`）：已实现实时麦克风采集
- 默认是 stdin 模式（文本或 `wav:/path`）。设置 **`use_microphone:true` 或环境变量
  `JARVIS_MIC=1`** 即可启用实时麦克风采集。
- `miniaudio` 采集设备以 16kHz 单声道 f32 的格式把音频流送给回调，写入有互斥锁保护的缓冲区
- VAD 工作线程对每个 512 采样（32ms）的窗口运行 `Silero VAD`（ONNX）推理，得到语音概率
- 超过 `vad_threshold`（0.5）就开始录音（预先保留 200ms，避免开头被截掉），
  连续静音 500ms 即结束这句话 → PCM 推入话语队列 → run() 从中取得 voice_in
- 短于 250ms 的噪声会被忽略；超过 `max_utterance_seconds` 则强制结束
- **设备初始化失败时（例如 WSL2 缺少音频桥），自动回退到 stdin**，不会崩溃。
  如果 WSLg/PulseAudio 的音源可用，在 WSL2 上也能实时工作。

### stt（`whisper_stt` 或 `moonshine_stt`）
- 在节点的整个生命周期内复用同一个 whisper.cpp 模型（没有重新加载的开销）
- `language="auto"` 会根据开头 30 秒（或整句话）自动检测语言
- 输出：`user_text`（单个字符串）、`user_lang`（ISO 代码）
- 识别置信度太低时输出空字符串，这一轮到 router 阶段会被跳过

**GPU 加速（whisper.cpp ROCm/HIP）**：自带的 whisper.cpp 只支持 CPU，
whisper-large-v3-turbo 在 CPU 上要跑约 32 秒（jfk 样例 11 秒），不适合实时使用。
如果是 AMD GPU（gfx1201=R9700，ROCm≥7.2），运行 `bash scripts/build_whisper_hip.sh`
构建 GGML_HIP 版本，替换 whisper_install 后，耗时降到 **约 7 秒（快 4.5 倍）**。
run_jarvis.sh 会自动加载 ROCm 运行时和 WSL 的 dxg 桥（HSA_ENABLE_DXG_DETECTION）。
有 GPU 时，config 里可以继续用大模型来保证实时性；
没有 GPU 就换成 whisper-small（CPU 上约 8 秒）。

**备选方案 `moonshine_stt`**（Moonshine-tiny ONNX）：只有 27M，非常轻量，输入是 16kHz 的原始波形（不是 mel），
结构为 seq2seq（编码器 + 拆成 2 个模型的解码器 + KV 缓存）。它与 supertonic TTS 共用
ONNX Runtime。每个模型对应一种语言，因此 `user_lang` 由配置固定
（tiny-ko 对应 "ko"）。分词器直接按 tokenizer.json 中的 SentencePiece BPE 解码
（▁→空格 + ByteFallback + Fuse）。通过配置项 stt.type 切换即可生效，
并且已用 Python optimum 的参考实现验证过，55 个 token 在字符级完全一致。int8（约 28MB）
需要完整版 ORT（自带的精简版不含 ConvInteger），所以默认使用 fp32（约 183MB）。

### text_or_voice（`channel_merge`）
- 在 voice_in（经过 STT 的语音）和 text_in（外部 A2A 输入）之间，选出当前生效的那条路径
- 两者都为空时，这一轮为空，图空转一个周期
- 外部 A2A 调用也必须带上 `user_lang`（缺失时按 "en" 处理）

### memory_lookup（`memory_lookup`）
- 从 NeoGraph `Store` 的 `jarvis.tony` 命名空间读取
- 把最近 N 轮对话、偏好和 last_topic 合并成一个 `memory_context` 推送出去
- 没有额外开销，每次从图的起点开始都会运行

### router（`intent_classifier`）
- 通过 OpenRouter 调用固定的 DeepSeek 模型（约 200-400ms）
- 系统提示词 = persona.txt 的 [router] 部分 + MCP 目录文本 + agent 注册表文本
- 校验输出的 JSON：解析失败就走回退路径（mode=chat）。工具名和 agent 名
  会与目录、注册表核对；如果不存在，就降级为 chat（避免 LLM 编造的
  `delegate_to:"null"` 流入下游）。
- chat 模式直接进入 response_synth，不调用工具，也不委派，适用于问候、
  自我介绍和回顾对话

### direct_branch（`tool_dispatch`）
- 只分发一次 `route_decision.tool_calls[0]`
- 把结果追加到 `tool_results` 通道
- 如果 skip_synthesis=true，就跳过下一个节点，直接交给 TTS

### parallel_branch（`parallel_tool_fanout`）
- 同时执行 `route_decision.tool_calls` 中的所有调用（`make_parallel_group`）
- 并发数上限由 `max_concurrent` 决定（默认 4）
- 所有结果按顺序追加到 `tool_results` → 由 reducer 交给 synthesizer 使用

### delegate_branch（`a2a_delegate`）
- 把 user_text 发送到 `route_decision.delegate_to` 指向的 A2A endpoint
- 超过 `timeout_seconds` 就返回错误响应（JARVIS 会用语音说“专家没有响应”）
- 优先从响应中提取 `[SUMMARY]` 行 → 保存为 `delegated_reply`

### response_synth（`llm_call`）
- 通过 OpenRouter 调用固定的 DeepSeek 模型（约 800-1500ms）
- 系统提示词 = persona.txt 的 [synth] 部分（+ 语言指令 + 会话边界注释）
- 对话历史（memory_context.recent_turns）**以 user/assistant 角色轮次组成的 messages 数组传入**。
  以前是把 JSON 内联在 user 消息里，模型就会把过去的回答当成
  要原样照搬的内容（memory parrot，即复读历史回答）。
- 当前轮的 user 消息 = user_text，再附上 tool_results / delegated_reply
- 防复读检查：如果输出去掉首尾空白后与过去的某个回答完全一致，就重新生成一次
- 输出 = `final_text`（交给 TTS 朗读的字符串）
- skip_synthesis=true 的路径会绕过它（这个位置由 synth_skip 顶替）

### synth_skip（`passthrough`）
- 把 tool_results 的最后一项（通常是工具的原始响应）直接复制为 final_text
- 例如：时间工具返回“3:30 PM”→ 直接朗读
- 这是 JARVIS 响应快的关键：省掉一次约 1 秒的大模型调用

### memory_commit（`memory_commit`）
- 把本轮的 user_text、final_text 和用到的工具名追加到 Store 的 turns 中
- 下一轮 memory_lookup 会取出这些内容，作为 router 的上下文
- 可以异步处理（与 TTS 并行），目前是串行

### tts（`supertonic_tts`）
- supertonic 以 final_text 和 user_lang 为输入，推理得到 44.1kHz 的 PCM
- 启动 miniaudio 扬声器播放，约 100-300ms 后出第一个音频块
- 尚未实现 barge-in（打断）；播放期间，麦克风输入会被丢弃。

## 图之外：后台触发器 / A2A 服务器

主图每个周期处理一句话并给出一次响应。A2A 服务器已经实现；
下面的后台触发器只是一个尚未实现的设计。

### 后台触发器图
- 单独的 `GraphEngine`（或者只用一个 std::thread）
- 监控定时器和外部事件
- 事件发生时，向 JARVIS 主图的 `text_in` 通道注入一条消息
- JARVIS 可以抢在 Tony 开口之前说话（“先生，10 分钟后有会议。”）

### A2A 服务器（对外开放 JARVIS）
- 依据 `agent_registry.json` 中的 `self` 部分
- 用 `GraphAgentAdapter` 包装同一个引擎后对外提供（示例 38 的做法）
- 外部的文本输入跳过 STT 阶段，直接从 `text_in` 进入 router
- 响应可以作为文本发回去，也可以同时用本地 TTS 播放

## 已知限制 / 下一版

- **不支持 barge-in**：TTS 播放期间会忽略麦克风输入。计划在 v2 中加入取消令牌。
- **不支持多说话人**：假定只有一个人说话。区分说话人需要另加节点（例如 pyannote）。
- **长期记忆压缩**：查询时默认取最近 6 轮，提交时保留最新的 24 轮。更早的轮次会被丢弃，不会做摘要。
- **目录热重载**：目录只在启动时加载，修改后需要重启。尚未实现自动重载。
