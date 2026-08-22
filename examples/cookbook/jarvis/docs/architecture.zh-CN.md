<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=zh-CN source_sha256=27b6441685a1293819d0813b44b53142b1fa05c36ec9ba960702d5243eb9bf92 -->
# JARVIS Graph — 逐节点详解

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

`config/jarvis_graph.json` 中每个节点的作用及其位于该位置的原因。最好结合 README.md 中的图示阅读。

## 单轮对话的生命周期

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
- 短回答（直接 + 跳过合成）：T0→T8 ≈ 1.0-1.5s
- 正常（直接 + 合成）：T0→T8 ≈ 2.0-3.0s
- 委派（delegate）：T0→T8 ≈ 3.0-8.0s（取决于专家工作耗时）
- 综合（并行 + 合成）：T0→T8 ≈ 2.5-4.0s

## 逐节点详解

### mic_capture (`voice_in`) — 实时麦克风实现
- 默认为 stdin 模式（文本 / `wav:/path`）。**`use_microphone:true` 或环境变量 `JARVIS_MIC=1`** 激活实时麦克风采集。
- `miniaudio` 采集设备以 16kHz 单声道 f32 流式传输到回调 → 互斥缓冲区
- VAD 工作线程对 512 样本（32ms）窗口运行 `Silero VAD`(ONNX) 推理 → 语音概率
- 若超过 `vad_threshold`(0.5)，则开始录音（200ms 预卷以避免截断），500ms 连续静音结束话语 → PCM 推入话语队列 → run() 获取 voice_in
- 噪声 <250ms 被忽略，若超过 `max_utterance_seconds` 则强制结束
- **设备初始化失败时自动回退到 stdin（WSL2 音频桥缺失等）** — 不会崩溃。若 WSLg/PulseAudio 源可用，可在 WSL2 上实时工作。

### stt（`whisper_stt` 或 `moonshine_stt`）
- 在节点生命周期内复用单个 whisper.cpp 模型（无重载成本 ×）
- `language="auto"` 自动检测从最初 30 秒（或整个话语）的音频
- 输出：`user_text`（单个字符串）、`user_lang`（ISO 代码）
- 如果识别置信度过低，则返回空字符串——在路由器阶段检查，该返回被跳过

**GPU 加速（whisper.cpp ROCm/HIP）**：自带的 whisper.cpp 仅支持 CPU，因此 whisper-large-v3-turbo 在 CPU 上运行约 32 秒（jfk 11s）——不适合实时。AMD GPU（gfx1201=R9700，ROCm≥7.2）上运行 `bash scripts/build_whisper_hip.sh` 构建 GGML_HIP → 替换 whisper_install → **~7s（4.5×）**。run_jarvis.sh 自动加载 ROCm 运行时和 WSL dxg 桥（HSA_ENABLE_DXG_DETECTION）。有 GPU 时，config 可保持大模型以实现实时；没有时，切换到 whisper-small（CPU ~8s）。

**备选方案 `moonshine_stt`**（Moonshine-tiny ONNX）：27M 超轻量级，原始16kHz波形输入（非mel），seq2seq（编码器 + 2模型分离解码器 + KV缓存）。与supertonic TTS共享ONNX Runtime。语言特定风格模型，因此`user_lang`为配置固定（tiny-ko = "ko"）。分词器直接从tokenizer.json中的SentencePiece BPE解码（▁→空格 + ByteFallback + Fuse）。通过配置stt.type进行交换有效，Python optimum参考已验证55个token的字符级一致性。int8（约28MB）需要完整ORT构建（捆绑的缩减构建排除ConvInteger）→ 默认fp32（约183MB）。

### text_or_voice（`channel_merge`）
- 在voice_in（STT传递）和text_in（外部A2A）之间选择活动路径
- 如果两者均为空则空轮次 — 图通过一个周期传递
- 外部A2A调用还必须包含`user_lang`（如果缺失则假定为"en"）

### memory_lookup（`memory_lookup`）
- 从NeoGraph `Store` `jarvis.tony` 命名空间读取
- 将最后N轮 + 偏好 + last_topic组合为单个`memory_context`推送
- 成本 ×，始终在图形开始时运行

### 路由器 (`intent_classifier`)
- 通过OpenRouter固定DeepSeek模型（约200-400毫秒）
- 系统提示 = persona.txt [router] + MCP目录文本 + 智能体注册表文本
- 输出JSON验证：解析失败 → 回退（模式=chat）。工具/智能体名称已对照目录·注册表验证；如果不存在，则降级为chat（防止LLM凭空捏造的`delegate_to:"null"`流入下游）。
- 聊天模式直接进入response_synth，无需工具/委派——用于问候、自我介绍、对话回忆

### direct_branch（`tool_dispatch`）
- 分派`route_decision.tool_calls[0]`一次
- 将结果追加到 `tool_results` 通道
- 如果skip_synthesis=true，则绕过下一个节点直接到TTS

### 并行分支（parallel_branch）（`parallel_tool_fanout`）
- 同时执行所有`route_decision.tool_calls`（`make_parallel_group`）
- 通过`max_concurrent`设置上限（默认4）
- 按顺序将所有结果追加到`tool_results`中 → 供reducer用于合成器

### 委派分支（delegate_branch）（`a2a_delegate`）
- 将用户文本发送到`route_decision.delegate_to`指向的A2A端点
- 如果超出`timeout_seconds`则返回错误响应（JARVIS通过语音说“专家未响应”）
- 首先从响应中提取`[SUMMARY]`行 → 保存到`delegated_reply`

### response_synth (`llm_call`)
- 通过OpenRouter固定DeepSeek模型（约800-1500ms）
- 系统提示 = persona.txt [synth]（+ 语言指令 + 会话边界注释）
- 对话历史（memory_context.recent_turns）**以user/assistant角色轮次的message数组形式传递**——此前，用户消息中的内联JSON造成模型将过往回答按内容原文处理（记忆复读机）。
- 当前轮次用户消息 = user_text + 附加的tool_results / delegated_reply
- 逐字防护：若输出在去除首尾空格后与过往回答完全匹配，则重新生成一次
- 输出 = `final_text`（供TTS朗读的字符串）
- 在 skip_synthesis=true 路径中被绕过（synth_skip 占据此位置）

### synth_skip (`passthrough`)
- 直接复制tool_results中的最后一项（通常为工具原始响应）作为final_text
- 示例：时间工具返回“3:30 PM”→ 直接送入语音
- JARVIS响应速度的秘密武器——省去整个约1秒的大型LLM调用

### 内存提交 (`memory_commit`)
- 将本轮user_text + final_text + 所用工具名称追加到Store回合
- 下一轮内存查找拉取这些内容以供路由器上下文使用
- 可以异步处理（与TTS并行）——目前为串行

### tts (`supertonic_tts`)
- 次属音推理，使用 final_text + user_lang → 44.1kHz PCM
- 启动 miniaudio 扬声器播放 → 首个区块约 100–300ms 后开始
- 如果播放期间检测到 voice_in 激活（barge-in），则通过取消令牌取消
  - 初始骨架不支持打断（barge-in）；将在v2中添加

## Graph 外部 — 后台触发器 / A2A 服务器

JARVIS 主图是一个简单的单次话语、单次响应的循环，但 main.cpp 还启动了另外两个组件以构成完整的 JARVIS 体验：

### 后台触发图
- 分离 `GraphEngine`（或仅用 std::thread）
- 监视定时器/外部事件
- 当事件发生时，将消息注入到 JARVIS 主程序的 `text_in` 通道
- JARVIS 在托尼提问之前发言（“先生，10 分钟后有会议。”）

### A2A 服务器（将 JARVIS 对外暴露）
- 基于`self`部分，见`agent_registry.json`
- 对外暴露同一个引擎，封装上包裹以 `GraphAgentAdapter`（例38模式）
- 外部文本输入跳过 STT 阶段，直接进入 `text_in` → router
- 响应可以以文本形式发送回，也可通过本地TTS同时播放

## 已知限制 / 下一版本

- **不支持插话** — TTS播放期间忽略麦克风输入。在v2中添加取消令牌。
- **不支持多说话人** — 假设只有一个人。说话人分离需要独立节点（例如，pyannote）。
- **长记忆压缩** — 随着对话延长，轮次无限增长。需要#56历史压缩模式。
- **目录热重载** — JSON更改检测是手动的（SIGHUP等）。在v2中通过inotify自动重载。
