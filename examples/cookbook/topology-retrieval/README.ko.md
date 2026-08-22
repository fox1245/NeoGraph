<!-- neograph-i18n: source=examples/cookbook/topology-retrieval/README.md locale=ko source_sha256=e3f2a2a1c72cb3ea14094065c49ab311bd18d49e3ab403433fb77fd9b2d63748 -->
# 로컬 토폴로지 검색

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 NeoGraph cookbook은 토폴로지 재사용, P1 마이그레이션, 핸드오프 교체, 또는 DSL 합성에 앞서는 후보 선택 경로를 보여줍니다:

```text
OpenRouter baai/bge-m3 (dense, max length 8192)
  + local BM25
  -> reciprocal-rank fusion
  -> OpenRouter voyageai/rerank-2.5
  -> non-authoritative TopologyCandidateRef
  -> NeoGraph compatibility and admission planner
```

로컬 밀집 인덱스는 `IndexIDMap2(IndexHNSWFlat)`로 저장된 영구적인 Faiss `topologies.faiss`입니다. 함께 제공되는 신뢰할 수 있는 로컬 `topologies.manifest.pkl`는 벡터-ID 매핑, BGE 모델 slug, 8192-token 정책, descriptor 지문, 그리고 인덱스 해시를 기록합니다. 신뢰할 수 없는 소스에서 pickle을 로드하지 마십시오.

설치 및 실행:

```powershell
python -m pip install faiss-cpu numpy
python topology_retrieval.py --mock --query "preserve a running graph frontier"
python topology_retrieval.py --query "preserve a running graph frontier"
```

비모의(non-mock) 명령은 `OPENROUTER_API_KEY`를 통해 BGE-M3 임베딩과 Voyage 재순위화(reranking)를 획득합니다. 검색된 토폴로지를 실행하지 않습니다. 반환된 후보는 여전히 정확히 승인(admission)된 ProgramVersion을 해결하고 NeoGraph 호환성, 권한, 예산, 및 마이그레이션 검사를 통과해야 합니다.

NeoGraph 측 소비자는 C++ `example_topology_retrieval` 대상입니다. 재순위화된 후보 키를 수락하고, `ProgramCatalog`에서 정확히 승인된 번들을 해석하며, P1 어댑터를 증명하고, 영구적인 2세대 마이그레이션을 수행합니다. 다음으로 빌드 및 실행합니다:

```powershell
cmake --build build --target example_topology_retrieval
./build/example_topology_retrieval
```

해당 C++ 소비자를 통해 실제 검색 응답을 처음부터 끝까지 실행합니다:

```powershell
python run_neo_e2e.py `
  --consumer build/Release/example_topology_retrieval.exe `
  --query "preserve a running graph frontier while updating implementation"
```
