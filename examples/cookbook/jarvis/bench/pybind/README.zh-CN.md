<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/pybind/README.md locale=zh-CN source_sha256=721dbef65598b467d85737ce2bfa971f2362af4c95c8f724b9310338a1b887e9 -->
# Python 图基准：NeoGraph 与 LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这些脚本测量 Python 图执行和进程启动。两者都不调用模型提供方，也不需要 API 密钥。它们不验证类型化提供方请求、事件或结果绑定。下列命令面向已安装当前软件包的环境；本次更新仅对照源码，未执行命令。

## 复现

使用安装了当前 `neograph-engine` wheel 的 Python 环境。比较需要安装 `langgraph`；可选的较大 import 栈需要安装 `langchain-openai`。从仓库根目录运行：

```bash
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py neograph
python3 examples/cookbook/jarvis/bench/pybind/perturn.py neograph 5000
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph_openai
python3 examples/cookbook/jarvis/bench/pybind/perturn.py langgraph 5000
```

`perturn.py` 预热后运行五节点链。每个 Python 节点将 `v` 通道加一，每次测量运行都从零开始。脚本输出平均值、p50、p90 和每秒运行次数。运行次数须为正数。

`startup_rss.py` 在新进程中输出启动毫秒数和峰值 RSS。NeoGraph 分支导入软件包并引用三个图符号，不编译图。LangGraph 分支导入软件包并编译单节点图。启动测量覆盖的工作不同，因此时间比率不是图编译加速比。RSS 换算假定 Linux 的 `ru_maxrss` 单位为 KiB；macOS 不能原样使用此换算。Windows 没有 Python 的 `resource` 模块。

## 历史测量

仓库此前报告了以下数值。它们不是当前迁移构建的测量值，本次更新也没有重跑。

| 指标 | Python 中的 NeoGraph | LangGraph |
|---|---|---|
| 每次运行五个 Python 节点 | 0.38 ms；约 2620 次/s | 0.93 ms；约 1075 次/s |
| 按上述不同范围测量的启动 | 40 ms | 462 ms；含 `langchain_openai` 时为 2977 ms |
| 峰值 RSS | 36 MB | 61 MB；含 `langchain_openai` 时为 561 MB |

NeoGraph 在 C++ 中执行图调度器和通道归约；Python 节点函数体获取 GIL。这些测量没有单独测定 GIL 边界成本，也不能证明其他工作负载的性能。节点内导入 PyTorch 等软件包也会增加进程内存占用。
