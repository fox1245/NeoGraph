<!-- neograph-i18n: source=examples/cookbook/topology-retrieval/README.md locale=ja source_sha256=e3f2a2a1c72cb3ea14094065c49ab311bd18d49e3ab403433fb77fd9b2d63748 -->
# ローカルトポロジー取得

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このNeoGraphクックブックは、トポロジーの再利用、P1マイグレーション、ハンドオフ置換、またはDSL合成に先行する候補選択パスを示しています:

```text
OpenRouter baai/bge-m3 (dense, max length 8192)
  + local BM25
  -> reciprocal-rank fusion
  -> OpenRouter voyageai/rerank-2.5
  -> non-authoritative TopologyCandidateRef
  -> NeoGraph compatibility and admission planner
```

ローカル密インデックスは永続的なFaiss `IndexIDMap2(IndexHNSWFlat)`として格納され、`topologies.faiss` として保存されています。信頼されるローカルな companion `topologies.manifest.pkl` には、vector-IDマッピング、BGEモデルスラッグ、8192トークンポリシー、記述子フィンガープリント、およびインデックスハッシュが記録されています。信頼できないソースからpickleをロードしないでください。

インストールと実行:

```powershell
python -m pip install faiss-cpu numpy
python topology_retrieval.py --mock --query "preserve a running graph frontier"
python topology_retrieval.py --query "preserve a running graph frontier"
```

非モックコマンドは、`OPENROUTER_API_KEY`を通じてBGE-M3埋め込みとVoyageリランキングを取得する。取得したトポロジーを実行することは決してない。返される候補は、正確に承認されたProgramVersionを解決し、NeoGraph互換性、権限、予算、マイグレーションのチェックに合格しなければならない。

NeoGraph側のコンシューマーは、C++の`example_topology_retrieval`ターゲットである。これは、リランキングされた候補キーを受け入れ、`ProgramCatalog`から正確に承認されたバンドルを解決し、P1アダプタを実証し、生成2マイグレーションを永続的に実行する。以下のコマンドでビルドして実行する:

```powershell
cmake --build build --target example_topology_retrieval
./build/example_topology_retrieval
```

そのC++コンシューマーを使用して、実際の検索レスポンスをエンドツーエンドで実行する:

```powershell
python run_neo_e2e.py `
  --consumer build/Release/example_topology_retrieval.exe `
  --query "preserve a running graph frontier while updating implementation"
```
