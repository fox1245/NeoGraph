<!-- neograph-i18n: source=tests/live/README.md locale=zh-CN source_sha256=4834040858097bba00645bd6041377aa2c8b1cb59124098518aea7961d848922 -->
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
  - Anthropic Messages 的控制项：thinking 模式和 effort、tool choice、temperature 规则、提示缓存，以及经 OpenRouter 的相同控制项；
  - 类型化的失败：无效密钥、被取消的流、已过期的 deadline。
- **C++**（`tests/live_e2e_test.cpp`，目标 `neograph_live_e2e_tests`）：
  - 使用 Claude 5.5 服务器工具、生成图像和 Interactions 的提供商适配器；
  - 针对随附目录中每个模型的类型化客户端 `DecisionsClient`；
  - 使用随附配置和 `gpt-image-2` 的 `ImagesClient`；
  - 使用随附配置的 `VeoClient`。
- **已知缺口**是该版本线上必须失败的用例（strict xfail）。`tests/live/manifest.json` 列出了它们以及修复它们的 issue。修复合入后，用例开始通过，清单必须随之修改。

每次调用都使用便宜的模型和较小的上限。完整运行的费用是几美分，大部分来自 Veo 视频。

## 运行

先构建要检查的提交的 Python 扩展和 C++ 测试，然后运行：

```bash
python scripts/live_e2e.py run --env-file path/to/keys.env \
    --python path/to/python --pythonpath path/to/build \
    --cxx path/to/build/tests/neograph_live_e2e_tests
```

- **密钥。** env 文件中包含 `OPENAI_API_KEY`、`GEMINI_API_KEY`、`ANTHROPIC_API_KEY` 和 `OPENROUTER_API_KEY`。脚本只把它们传给测试进程，从不打印或保存；失败消息中出现的密钥值会被替换。
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
