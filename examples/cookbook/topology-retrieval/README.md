# Local topology retrieval

This NeoGraph cookbook demonstrates the candidate-selection path that precedes
topology reuse, P1 migration, handoff replacement, or DSL synthesis:

```text
OpenRouter baai/bge-m3 (dense, max length 8192)
  + local BM25
  -> reciprocal-rank fusion
  -> OpenRouter voyageai/rerank-2.5
  -> non-authoritative TopologyCandidateRef
  -> NeoGraph compatibility and admission planner
```

The local dense index is a persistent Faiss
`IndexIDMap2(IndexHNSWFlat)` stored as `topologies.faiss`. A companion trusted
local `topologies.manifest.pkl` records vector-ID mappings, the BGE model slug,
the 8192-token policy, descriptor fingerprints, and the index hash. Do not load
the pickle from an untrusted source.

Install and run:

```powershell
python -m pip install faiss-cpu numpy
python topology_retrieval.py --mock --query "preserve a running graph frontier"
python topology_retrieval.py --query "preserve a running graph frontier"
```

The non-mock command obtains BGE-M3 embeddings and Voyage reranking through
`OPENROUTER_API_KEY`. It never executes a retrieved topology. The returned
candidate must still resolve an exact admitted ProgramVersion and pass NeoGraph
compatibility, authority, budget, and migration checks.

The NeoGraph-side consumer is the C++ `example_topology_retrieval` target. It
accepts the reranked candidate key, resolves the exact admitted
bundle from `ProgramCatalog`, proves the P1 adapter, and performs a durable
generation-2 migration. Build and run it with:

```powershell
cmake --build build --target example_topology_retrieval
./build/example_topology_retrieval
```

Run a real retrieval response through that C++ consumer end-to-end:

```powershell
python run_neo_e2e.py `
  --consumer build/Release/example_topology_retrieval.exe `
  --query "preserve a running graph frontier while updating implementation"
```
