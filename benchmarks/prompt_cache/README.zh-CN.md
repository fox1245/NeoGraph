<!-- neograph-i18n: source=benchmarks/prompt_cache/README.md locale=zh-CN source_sha256=606a79f539c2aa4251858cf8bd9238efa4b6b14a5b4024faf240476f774b4114 -->
# 提示词缓存前缀探针

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

提供方的提示词缓存会复用请求中从第一个字节起与此前请求完全相同的开头部分。NeoGraph 运行时用一组固定的上下文制品和不断增长的 RAW 历史组装每个请求（见 [`docs/STRICT_RUNTIME_INTERPOSITION.md`](../../docs/STRICT_RUNTIME_INTERPOSITION.md) 的 "Artifact placement"）。这个探针测量：每轮都会变化的小块内容放在哪里，会如何改变提供方能从每个请求中复用的比例（issue #329）。

## 它做什么

`prompt_cache_probe.py` 通过 `RuntimeTurnAssembler` 和 `ControlledProvider` 运行 12 到 20 轮的对话：

- 一个很长的固定块：4,500 到 5,000 个 token 的 `RequiredSkill` 制品（`--bible-entries`，90 到 115），高于各提供方的缓存最小长度。
- 每轮增加一个问题和一个回复的 RAW 历史。
- 每轮都变化、由场景决定放置位置的约 400 个 token 的动态块（`--dynamic-lines`）。

每次提供方调用，它都会记录提供方报告的用量（输入、缓存读取、缓存写入和未缓存的 token），以及编码后的请求中与上一个请求相同的部分。这部分既按线路报文的字节数衡量，也按阅读顺序（工具、系统、消息）下提示词的字符数衡量。

场景名是动态块的放置位置，后面可以加 `+compact`，表示在 `--compact-at` 处增加一次压缩：最早的几轮离开 RAW 窗口，它们的摘要作为制品加入。放置位置有 `none`、`default`（`BeforeLatestUser`）、`before-history`、`after-history`、`raw-append`（不作为制品，而是把块追加到 RAW 源）、`system-tail`、`system-head` 和 `tie-break`。摘要由探针根据记录下来的问题和回复自行写成，不会调用 `compact_history`。

## 运行

先构建 Python 扩展，然后运行：

```bash
# No network and no key: only the request-prefix columns are filled.
PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py --offline \
    --preset anthropic.messages --model any-name --turns 14 --compact-at 10 \
    --scenarios none,default,before-history,after-history,raw-append,compaction

# Live: the key is read from the environment variable of the preset.
OPENAI_API_KEY=... PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py \
    --preset openai.chat --model <model> --rates input=...,cache_read=...,output=... \
    --budget-usd 0.05 --turns 16 --scenarios before-history,after-history,compaction

# Where does a provider's cache stop? Live only: four requests per level share the fixed block and that
# many turns of history, and differ after it.
OPENAI_API_KEY=... PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py \
    --preset openai.chat --model <model> --rates input=...,cache_read=...,output=... \
    --budget-usd 0.02 --prefix-ladder 1,2,4 --control reasoning_effort=low
```

- **预设。** `openai.chat`、`anthropic.messages`、`google.generate` 和 `openrouter.chat`。`ProviderControls` 字段用 `--control NAME=VALUE` 指定，family 嵌套用点号分隔；Messages 的请求级缓存控制用 `--cache-control 5m` 发送。探针把回复记录为可移植文本，所以 Gemini 需要 `--control generate.history_mode=GeminiHistoryMode.PortableForeign`。
- **成本。** 实时运行需要 `--rates`（每百万 token 的美元价格），用来限制并报告花费。如果没有任何缓存命中的最坏情况超过 `--budget-usd`，它会拒绝启动；花费超过该值时会停止。像 DeepSeek 这样在后台建立缓存的提供方，轮与轮之间需要几秒的 `--pause-ms`。
- **密钥。** 探针从环境变量读取密钥，从不打印或保存。
- **前缀阶梯。** `--prefix-ladder 1,2,4` 取代场景。在每一级（共享的轮数）上，四个请求共享固定块和相应长度的历史，只有其后的块不同：X、Y、Z，以及作为对照的再次发送的 X。固定块不同的校准请求会给出固定块和一个问题的 token 数。读到共享历史末尾的请求表明缓存沿最长共同前缀命中；只读到固定块的请求表明缓存在固定块与之前某个请求的末尾之间不保留任何内容。花费只有几千分之一美元。
- **输出。** 每个场景一张表和一份汇总；加上 `--out` 会把每次调用的记录保存为 JSON。`--show-order N` 打印第 N 次调用的消息顺序。
