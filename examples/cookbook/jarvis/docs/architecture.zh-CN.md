<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=zh-CN source_sha256=0682c23a88e18a9bc1094429a0ccfbc9d49d84c279355eed29ce28f068f1da42 -->
# JARVIS 图 — 逐节点细节

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

说明 `config/jarvis_graph.json` 中每个节点做什么，以及为什么放在那个位置。建议和 README.md 中的图一起阅读。

## 一个 turn 的生命周期

```
T0  Microphone active, Tony utterance start detected
T1  Utterance end (VAD detects 200ms silence)
T2  STT complete — text + detected language code
T3  Memory lookup complete — last 6 turns + preferences + last topic
T4  Router decision complete — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way branch complete — one of self-response(chat) / direct tool / delegation / parallel done
T6  Response synthesis complete (or skip branch bypassed)
T7  Memory commit complete
T8  TTS first chunk playback starts ← point where Tony "starts hearing" the response
T9  TTS last chunk playback complete, microphone reactivated waiting
```

T0→T8 是 JARVIS 的感知响应时间。目标分布：
- 短回答（direct + skip_synthesis）：T0→T8 ≈ 1.0-1.5s
- 正常（direct + synth）：T0→T8 ≈ 2.0-3.0s
- 委派（delegate）：T0→T8 ≈ 3.0-8.0s（取决于专家工作耗时）
- 综合型（parallel + synth）：T0→T8 ≈ 2.5-4.0s

## 逐节点细节

### mic_capture (`voice_in`) — 已实现实时麦克风
- 默认是 stdin 模式（text / `wav:/path`）。**`use_microphone:true` 或 env `JARVIS_MIC=1`** 会启用实时麦克风采集。
- `miniaudio` capture device 将 16kHz mono f32 流式送入 callback → mutex buffer
- VAD worker thread 在 512-sample（32ms）窗口上运行 `Silero VAD`（ONNX）推理 → 语音概率
- 如果超过 `vad_threshold`（0.5），开始录音（200ms pre-roll 避免截断），500ms 连续静音结束话语 → PCM 推入 utterance queue → run() 获取 voice_in
- 忽略 <250ms 的噪声；如果超过 `max_utterance_seconds` 则强制结束
- **设备初始化失败时自动 fallback 到 stdin（WSL2 audio bridge missing 等）** — 不崩溃。如果 WSLg/PulseAudio source 可用，在 WSL2 上可实时工作。

### stt (`whisper_stt` 或 `moonshine_stt`)
- 节点生命周期内复用单个 whisper.cpp 模型（无 reload cost ×）
- `language="auto"` 从前 30s（或整段 utterance）自动检测
- 输出：`user_text`（单个字符串），`user_lang`（ISO code）
- 如果识别置信度过低，输出空字符串 — turn 在 router 阶段跳过

**GPU 加速（whisper.cpp ROCm/HIP）**：捆绑的 whisper.cpp 是 CPU-only，所以 whisper-large-v3-turbo 在 CPU 上需要 ~32s（jfk 11s）— 不适合实时。AMD GPU（gfx1201=R9700，ROCm≥7.2）运行 `bash scripts/build_whisper_hip.sh` 进行 GGML_HIP 构建 → 替换 whisper_install → **~7s（4.5×）**。run_jarvis.sh 会自动加载 ROCm runtime 和 WSL dxg bridge（HSA_ENABLE_DXG_DETECTION）。有 GPU 时，config 可以保持 large 以实现实时；没有 GPU 时，切换到 whisper-small（CPU ~8s）。

**替代选项 `moonshine_stt`**（Moonshine-tiny ONNX）：27M 超轻量，输入原始 16kHz waveform（不是 mel），seq2seq（encoder + 2-model separated decoder + KV cache）。与 supertonic TTS 共用 ONNX Runtime。语言特定 flavor model，因此 `user_lang` 在 config 中固定（tiny-ko = "ko"）。Tokenizer 直接从 tokenizer.json 的 SentencePiece BPE 解码（▁→space + ByteFallback + Fuse）。通过 config stt.type 交换即可，且 Python optimum reference 已验证 55-token 字符级一致性。int8（~28MB）需要完整 ORT build（捆绑的 reduced build 不包含 ConvInteger）→ 默认 fp32（~183MB）。

### text_or_voice (`channel_merge`)
- 在 voice_in（STT 通过）和 text_in（external A2A）之间选择活动路径
- 如果两者都为空，则为空 turn — graph 通过一个周期
- 外部 A2A 调用也必须包含 `user_lang`（缺失时假设为 "en"）

### memory_lookup (`memory_lookup`)
- 从 NeoGraph `Store` 的 `jarvis.tony` namespace 读取
- 将最近 N 个 turns + prefs + last_topic 合并为单个 `memory_context` push
- 有固定成本，始终在 graph 开始时运行

### router (`intent_classifier`)
- 小 LLM（gpt-4o-mini，~200-400ms）
- System prompt = persona.txt [router] + MCP catalog text + agent registry text
- 输出 JSON validation：parse failure → fallback（mode=chat）。Tool/agent names 会对 catalog·registry 验证；如果不真实，则降级到 chat（防止 LLM 编造的 `delegate_to:"null"` 流向下游）。
- Chat mode 直接进入 response_synth，不使用工具/委派 — 用于问候、自我介绍、对话回忆

### direct_branch (`tool_dispatch`)
- 对 `route_decision.tool_calls[0]` 调用一次
- 将结果追加到 `tool_results` channel
- 如果 skip_synthesis=true，则绕过下一个节点，直接到 TTS

### parallel_branch (`parallel_tool_fanout`)
- 同时执行所有 `route_decision.tool_calls`（`make_parallel_group`）
- 通过 `max_concurrent` 设置上限（默认 4）
- 按顺序将所有结果追加到 `tool_results` → reducer 供 synthesizer 使用

### delegate_branch (`a2a_delegate`)
- 将 user_text 发送到 `route_decision.delegate_to` 指向的 A2A endpoint
- 如果超过 `timeout_seconds`，返回错误响应（JARVIS 通过语音说 “Expert not responding”）
- 先从响应中提取 `[SUMMARY]` 行 → 保存到 `delegated_reply`

### response_synth (`llm_call`)
- 大 LLM（gpt-4o，~800-1500ms）
- System prompt = persona.txt [synth]（+ 语言指令 + 会话边界注释）
- Conversation history（memory_context.recent_turns）以 user/assistant role turns 的 **messages array** 传入 — 之前把 inline JSON 放进 user message 会导致模型把过去答案当作内容逐字复读（memory parrot）。
- Current turn user message = user_text + 附加 tool_results / delegated_reply
- Verbatim guard：如果 trim 后输出与过去答案完全一致，则重新生成一次
- Output = `final_text`（供 TTS 朗读的字符串）
- 在 skip_synthesis=true 路径中被绕过（synth_skip 占据此位置）

### synth_skip (`passthrough`)
- 将 tool_results 中最后一项（通常是工具的原始响应）直接复制为 final_text
- 示例：Time tool 返回 “3:30 PM” → 直接语音播报
- JARVIS 响应速度的秘密武器 — 省掉完整的 ~1s 大 LLM 调用

### memory_commit (`memory_commit`)
- 将本 turn 的 user_text + final_text + 使用过的 tool names 追加到 Store turns
- 下一 turn 的 memory_lookup 会为 router context 拉取这些内容
- 可以异步处理（与 TTS 并行）— 当前为串行

### tts (`supertonic_tts`)
- supertonic 用 final_text + user_lang 推理 → 44.1kHz PCM
- 启动 miniaudio speaker playback → 约 100-300ms 后播放 first chunk
- 如果播放期间检测到 voice_in activation，用 cancel token 取消（barge-in）
  - 初始骨架不支持 barge-in；将在 v2 添加

## 图外 — 后台触发器 / A2A Server

JARVIS 主图是简单的单话语、单响应循环，但 main.cpp 启动了两个额外组件来补全 JARVIS 体验：

### 后台触发图
- 单独的 `GraphEngine`（或只是 std::thread）
- 监控计时器 / 外部事件
- 事件发生时向 JARVIS main 的 `text_in` channel 注入消息
- JARVIS 在 Tony 提问前主动说话（“Sir, meeting in 10 minutes.”）

### A2A Server（将 JARVIS 暴露给外部）
- 基于 `agent_registry.json` 中的 `self` section
- 暴露同一个用 `GraphAgentAdapter` 包装的 engine（example 38 pattern）
- 外部文本输入跳过 STT 阶段，直接走 `text_in` → router
- 响应可以作为文本发回，也可以同时通过本地 TTS 播放

## 已知限制 / 下一版本

- **不支持 Barge-in** — TTS 播放期间忽略麦克风输入。v2 添加 cancel token。
- **不支持多人说话** — 假设只有一个人。说话人分离需要单独节点（例如 pyannote）。
- **长期记忆压缩** — 对话变长时 turns 会无限增长。需要 #56 history_compaction pattern。
- **Catalog 热重载** — JSON 变更检测是手动的（SIGHUP 等）。v2 通过 inotify 自动 reload。
