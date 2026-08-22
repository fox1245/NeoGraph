<!-- neograph-i18n: source=examples/cookbook/topology-retrieval/README.md locale=zh-CN source_sha256=e3f2a2a1c72cb3ea14094065c49ab311bd18d49e3ab403433fb77fd9b2d63748 -->
# 本地拓扑检索

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本 NeoGraph 食谱演示了拓扑复用、P1 迁移、接管替换或 DSL 合成之前的候选选择路径：

```text
OpenRouter baai/bge-m3 (dense, max length 8192)
  + local BM25
  -> reciprocal-rank fusion
  -> OpenRouter voyageai/rerank-2.5
  -> non-authoritative TopologyCandidateRef
  -> NeoGraph compatibility and admission planner
```

本地稠密索引是一个持久化的 Faiss `IndexIDMap2(IndexHNSWFlat)`，存储为 `topologies.faiss`。配套的受信本地 `topologies.manifest.pkl` 记录向量-ID 映射、BGE 模型 slug、8192 token 策略、描述符指纹以及索引哈希。请勿从不受信任的来源加载 pickle。

安装并运行：

```powershell
python -m pip install faiss-cpu numpy
python topology_retrieval.py --mock --query "preserve a running graph frontier"
python topology_retrieval.py --query "preserve a running graph frontier"
```

非模拟命令通过 `OPENROUTER_API_KEY` 获取 BGE-M3 Embedding 和 Voyage 重排序结果。它绝不执行检索到的拓扑。返回的候选仍必须解析精确的已准入 ProgramVersion，并通过 NeoGraph 兼容性、权威性、预算和迁移检查。

NeoGraph侧消费者是C++ `example_topology_retrieval`目标。它接受重新排序后的候选项键，从`ProgramCatalog`中解析确切的已准入束，证明P1适配器，并执行持久的第二代迁移。构建并运行：

```powershell
cmake --build build --target example_topology_retrieval
./build/example_topology_retrieval
```

运行真实检索响应，通过该C++消费者端到端执行：

```powershell
python run_neo_e2e.py `
  --consumer build/Release/example_topology_retrieval.exe `
  --query "preserve a running graph frontier while updating implementation"
```
