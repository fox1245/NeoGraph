<!-- neograph-i18n: source=tests/live/README.md locale=zh-CN source_sha256=1e2a8175a45682b2b8acd37bcef658f4993750cc0e379fd7e983364261e38e83 -->
# 实时端到端测试套件

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

单元测试和集成测试回放录制的或脚本生成的提供商流量，因此无法说明提供商现在是否仍接受 NeoGraph 发送的内容，也无法说明 NeoGraph 是否仍接受提供商今天的应答。所以每个版本都要用真实密钥通过这套测试；如果某个提交没有通过的报告，CI 会拒绝发布该版本。

## 覆盖范围

- **Python**（`bindings/python/tests/live/` 和 `bindings/python/tests/test_live_provider.py`）：
  - 八个家族预设（OpenAI Chat 和 Responses、Anthropic Messages、Gemini Generate 和 Interactions、OpenRouter Chat、Responses 和 Messages），每个都以缓冲和流式两种方式调用，并检查流式增量拼接后等于最终文本；
  - Claude 5.5 模型；
  - 图像输入；
  - 每个预设上的强制工具调用，以及完整的工具往返；
  - 保留 OpenRouter Chat 和 Gemini Generate 生成的图像；
  - 使用 Generate 响应模态和语音设置的 Gemini 语音合成（缓冲和流式）以及 Lyria 片段；
  - OpenAI Responses 的 hosted 网页搜索（缓冲和流式）；
  - Anthropic Messages 的控制项：thinking 模式和 effort、tool choice、temperature 规则、提示缓存，以及经 OpenRouter 的相同控制项；
  - 在图中运行的长 Anthropic 智能体运行（`test_live_anthropic_agent.py`）：相互依赖的工具链、必须记住第一轮的最多六轮对话、经过工具轮次并延续到后续轮次的签名 thinking 和 redacted thinking、并行工具调用、工具错误与重试、由人逐个审阅或批准工具调用的流程、贯穿整个对话的提示缓存、Claude 5.5 adaptive thinking，以及以流式、异步和经 OpenRouter 方式运行的相同循环；
  - 类型化的失败：无效密钥、被取消的流、已过期的 deadline；
  - 通过 `Provider` 调用的 SchemaProvider 端点（#531）：OpenAI 嵌入、以 await 等待的 OpenRouter 嵌入和 OpenAI moderation。
- **C++**（`tests/live_e2e_test.cpp`，目标 `neograph_live_e2e_tests`）：
  - 使用 Claude 5.5 服务器工具、生成图像和 Interactions 的提供商适配器；
  - 通过 `Provider` 调用的 SchemaProvider 端点（#531）：OpenAI 和 OpenRouter 嵌入、OpenAI moderation、Anthropic token 计数、OpenAI 图像、再转写回文本的 OpenAI 语音（语音端点唯一的实时测试）、在请求中引用的 Gemini 文件，以及从提交到取消的 Anthropic 和 OpenAI 批处理；
  - OpenAI 和 Google 上的 SchemaProvider 实时转写会话：输入由 OpenAI Realtime 会话朗读出的语音，每个会话提交一个包含所说内容的用户轮次；
  - 从全部四个提供方的列表刷新的模型目录、只有刷新后的目录才列出的模型的调用，以及任何目录都未列出但仍会发送的模型；
- **已知缺口**是该版本线上必须失败的用例（strict xfail）。`tests/live/manifest.json` 列出了它们以及修复它们的 issue。修复合入后，用例开始通过，清单必须随之修改。

每次调用都使用便宜的模型和较小的上限。完整运行约需八分钟，费用约 30 美分，其中大部分来自 Anthropic 智能体运行。

## 运行

先构建要检查的提交的 Python 扩展和 C++ 测试，然后运行：

```bash
python scripts/live_e2e.py run --env-file path/to/keys.env \
    --python path/to/python --pythonpath path/to/build \
    --cxx path/to/build/tests/neograph_live_e2e_tests
```

- **密钥。** env 文件中包含 `OPENAI_API_KEY`、`GEMINI_API_KEY`、`ANTHROPIC_API_KEY` 和 `OPENROUTER_API_KEY`。脚本只把它们传给测试进程，从不打印或保存；失败消息中出现的密钥值会被替换。
- **模型。** `tests/live/manifest.json` 中的 `models` 部分将每个模型角色（如 `openai.chat`、`anthropic.messages` 或 `anthropic.messages.adaptive`）映射到默认模型 ID，Python 套件通过 `live_support.live_model(role)` 解析它。在环境变量或 env 文件中设置 `NG_LIVE_MODEL_<ROLE>`（将 `.` 和 `-` 替换为 `_` 并转为大写，例如 `NG_LIVE_MODEL_OPENAI_CHAT`）即可在不修改代码的情况下覆盖该角色的模型。
- **干净的工作树。** 报告指向一个提交，所以 `run` 拒绝存在未提交更改的工作树。
- **版本。** `run` 拒绝版本与工作树版本不同的 `neograph_engine`。
- **输出。** 它写入 `tests/live/report.json`，其中包含提交、工作树哈希、NeoGraph 版本、固定的 SchemaProvider 提交，以及每个用例及其结果。
- **部分运行。** `--select` 和 `--cxx-filter` 只运行套件的一部分。部分报告永远不能批准发布。

## 发布关卡

1. 发布拉取请求（`release/*` 分支）提交在其最终提交上运行得到的 `tests/live/report.json`。
2. `Live e2e report` 工作流不使用密钥，运行 `python scripts/live_e2e.py verify`。只有以下条件全部成立时才通过：
   - 报告描述的是这个工作树（除报告以外的所有被跟踪文件）；
   - 报告属于这个版本，并且来自干净的工作树；
   - 清单中的每个用例都得到预期结果：通过，或作为已列出的已知缺口而失败；
   - 没有其他失败的用例。
3. Wheels 工作流在发布到 PyPI 之前，会在发布标签上进行同样的检查。

增删用例后，`python scripts/live_e2e.py manifest --python … --pythonpath … --cxx …` 会根据套件收集到的用例重写清单，并保留已知缺口。提交前请检查差异。

## 在 master 上

每次推送到 master 后，`Live e2e report` 工作流还会用真实密钥运行整个套件。这样，提供方的变化或回归会在合入当天暴露，而不是等到下一次发布。运行会把 `report.json` 作为构建产物上传。它不会提交报告，也不会批准发布。

- **密钥。** 四个密钥是 GitHub 环境 `live-e2e` 的机密，只有 master 可以使用该环境。GitHub 不向任何人显示机密的值，会在日志中遮盖它，也不会把它交给拉取请求的工作流。作业把密钥写入只有运行器用户可读的文件，并在作业结束时删除该文件。
- **一次一个。** master 上的运行不会中途取消。运行期间到达的推送会等待，等待中的推送只有最新的那一个会接着运行。
