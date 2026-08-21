#!/usr/bin/env python3
"""Local topology retrieval for NeoGraph.

This cookbook demonstrates the retrieval half of topology evolution without
requiring NeoCode. It keeps an admitted-topology descriptor registry locally,
persists dense BGE-M3 vectors in Faiss, obtains lexical candidates with BM25,
fuses both ranks with RRF, and optionally calls Voyage rerank-2.5 through the
same OpenRouter credential.

The returned candidate is never executable authority. A NeoGraph host must
resolve its exact ProgramVersion and run compatibility/admission before it may
reuse, P1-migrate, hand off, or synthesize a successor.

Dependencies:
    python -m pip install faiss-cpu numpy

Usage:
    # Uses OPENROUTER_API_KEY from the environment or a parent .env file.
    python topology_retrieval.py --query "preserve a running graph frontier while updating implementation"

    # No network; deterministic stand-in vectors/relevance scores.
    python topology_retrieval.py --mock --query "find a graph migration topology"
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pickle
import re
import sys
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

try:
    import faiss  # type: ignore
    import numpy as np
except ImportError as error:
    print("Install the example dependencies: python -m pip install faiss-cpu numpy", file=sys.stderr)
    raise SystemExit(2) from error


OPENROUTER_BASE_URL = "https://openrouter.ai/api/v1"
DENSE_MODEL = "baai/bge-m3"
RERANK_MODEL = "voyageai/rerank-2.5"
MAX_LENGTH_TOKENS = 8192
EMBEDDING_DIMENSIONS = 1024
RRF_K = 60


@dataclass(frozen=True)
class TopologyDescriptor:
    vector_id: int
    topology_version_id: str
    title: str
    summary: str
    capability_tags: tuple[str, ...]
    migration_class: str
    source_kind: str
    descriptor_sha256: str = ""

    def retrieval_document(self) -> str:
        return "\n".join(
            (
                f"title: {self.title}",
                f"summary: {self.summary}",
                f"capabilities: {' '.join(self.capability_tags)}",
                f"migration_class: {self.migration_class}",
                f"source_kind: {self.source_kind}",
            )
        )

    def sealed(self) -> "TopologyDescriptor":
        digest = hashlib.sha256(self.retrieval_document().encode("utf-8")).hexdigest()
        return TopologyDescriptor(
            self.vector_id,
            self.topology_version_id,
            self.title,
            self.summary,
            self.capability_tags,
            self.migration_class,
            self.source_kind,
            f"sha256:{digest}",
        )


CATALOG = tuple(
    descriptor.sealed()
    for descriptor in (
        TopologyDescriptor(
            1001,
            "sha256:1111111111111111111111111111111111111111111111111111111111111111",
            "Shape-preserving implementation evolution",
            "A control-free single-root Core topology. It preserves channels, reducers, node "
            "names, frontier, barriers, capability bindings, and contracts. A host may use "
            "GraphSemanticMigrationAdapter to continue from a durable super-step on an immutable "
            "successor generation.",
            ("checkpoint", "frontier", "p1-migration", "semantic-adapter"),
            "p1_migrate",
            "cpp_or_declaration_only_javascript",
        ),
        TopologyDescriptor(
            1002,
            "sha256:2222222222222222222222222222222222222222222222222222222222222222",
            "Explicit checkpoint handoff replacement",
            "A QuickJS Program reaches ng.checkpoint and serializes handoff state. The admitted "
            "successor starts fresh with handoff and previous_run_id, preserving lineage budgets "
            "without migrating a JavaScript heap or closure.",
            ("checkpoint", "handoff", "program-replace", "quickjs"),
            "handoff_replace",
            "javascript",
        ),
        TopologyDescriptor(
            1003,
            "sha256:3333333333333333333333333333333333333333333333333333333333333333",
            "Parallel evidence review graph",
            "Fan-out researcher and verifier nodes, join evidence through a reducer, then emit a "
            "review decision. Suited to new runs and does not claim compatibility with an arbitrary "
            "in-flight frontier.",
            ("parallel", "evidence", "review", "new-run"),
            "new_run_only",
            "cpp_or_javascript",
        ),
        TopologyDescriptor(
            1004,
            "sha256:4444444444444444444444444444444444444444444444444444444444444444",
            "Bounded child task graph",
            "A parent Program expands a reviewed finite task DAG through admitted templates, "
            "attenuated child budgets, durable child bindings, and bounded joins.",
            ("subagent", "task-graph", "bounded", "child-budget"),
            "new_run_only",
            "javascript",
        ),
    )
)


def load_dotenv() -> None:
    """Load the first .env found from the current directory upward."""
    for directory in (Path.cwd(), *Path.cwd().parents):
        path = directory / ".env"
        if not path.is_file():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line or line.lstrip().startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))
        return


def truncate_for_bge_m3(text: str) -> str:
    """Conservative character cap for the declared BGE-M3 8192-token budget.

    The corpus descriptors are intentionally short. Production code should use
    the pinned BGE tokenizer for exact token accounting before the API call.
    """
    return text[: MAX_LENGTH_TOKENS * 3]


def normalized(vector: list[float]) -> list[float]:
    length = math.sqrt(sum(value * value for value in vector))
    if length == 0:
        raise ValueError("embedding must not be zero")
    return [value / length for value in vector]


def mock_embedding(text: str) -> list[float]:
    """Deterministic stand-in used only by --mock; never label it BGE-M3."""
    values = [0.0] * EMBEDDING_DIMENSIONS
    for token in tokenize(text):
        digest = hashlib.blake2b(token.encode("utf-8"), digest_size=16).digest()
        slot = int.from_bytes(digest[:4], "big") % EMBEDDING_DIMENSIONS
        sign = 1.0 if digest[4] & 1 else -1.0
        values[slot] += sign
    return normalized(values)


def openrouter_json(path: str, api_key: str, body: dict[str, Any]) -> dict[str, Any]:
    request = urllib.request.Request(
        f"{OPENROUTER_BASE_URL}{path}",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "HTTP-Referer": "https://github.com/fox1245/NeoGraph",
            "X-Title": "NeoGraph topology retrieval cookbook",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"OpenRouter {path} failed ({error.code}): {detail}") from error


def bge_m3_embeddings(texts: list[str], api_key: str, mock: bool) -> list[list[float]]:
    if mock:
        return [mock_embedding(text) for text in texts]
    response = openrouter_json(
        "/embeddings",
        api_key,
        {
            "model": DENSE_MODEL,
            "input": [truncate_for_bge_m3(text) for text in texts],
            "encoding_format": "float",
        },
    )
    vectors = [normalized(item["embedding"]) for item in response["data"]]
    if len(vectors) != len(texts) or any(len(vector) != EMBEDDING_DIMENSIONS for vector in vectors):
        raise RuntimeError("OpenRouter BGE-M3 returned an unexpected embedding shape")
    return vectors


def tokenize(text: str) -> list[str]:
    return re.findall(r"[\w.-]+", text.lower(), flags=re.UNICODE)


class Bm25Index:
    def __init__(self, documents: list[str]):
        self._documents = [tokenize(document) for document in documents]
        self._document_frequency: dict[str, int] = {}
        self._average_length = sum(map(len, self._documents)) / max(1, len(self._documents))
        for document in self._documents:
            for token in set(document):
                self._document_frequency[token] = self._document_frequency.get(token, 0) + 1

    def search(self, query: str, top_k: int) -> list[tuple[int, float]]:
        query_tokens = tokenize(query)
        results: list[tuple[int, float]] = []
        for index, document in enumerate(self._documents):
            term_frequency: dict[str, int] = {}
            for token in document:
                term_frequency[token] = term_frequency.get(token, 0) + 1
            score = 0.0
            for token in query_tokens:
                frequency = term_frequency.get(token, 0)
                if frequency == 0:
                    continue
                document_frequency = self._document_frequency.get(token, 0)
                inverse_frequency = math.log(
                    (len(self._documents) - document_frequency + 0.5) /
                    (document_frequency + 0.5) + 1.0
                )
                score += inverse_frequency * (frequency * 2.0) / (
                    frequency + 1.2 * (1.0 - 0.75 + 0.75 * len(document) / self._average_length)
                )
            results.append((index, score))
        return sorted(results, key=lambda value: value[1], reverse=True)[:top_k]


class PersistentFaissIndex:
    def __init__(self, directory: Path):
        self._directory = directory
        self._index_path = directory / "topologies.faiss"
        self._manifest_path = directory / "topologies.manifest.pkl"

    def load_or_build(self, descriptors: tuple[TopologyDescriptor, ...], api_key: str, mock: bool):
        fingerprint = hashlib.sha256(
            "\n".join(item.descriptor_sha256 for item in descriptors).encode("utf-8")
        ).hexdigest()
        if self._index_path.exists() and self._manifest_path.exists():
            # This manifest is local user-owned data. Never load an untrusted
            # pickle downloaded from another machine or repository.
            with self._manifest_path.open("rb") as stream:
                manifest = pickle.load(stream)
            if (manifest.get("catalog_fingerprint") == fingerprint and
                    manifest.get("dense_model") == ("mock" if mock else DENSE_MODEL) and
                    manifest.get("max_length_tokens") == MAX_LENGTH_TOKENS and
                    manifest.get("index_sha256") ==
                    hashlib.sha256(self._index_path.read_bytes()).hexdigest()):
                return faiss.read_index(str(self._index_path)), manifest

        self._directory.mkdir(parents=True, exist_ok=True)
        vectors = bge_m3_embeddings([item.retrieval_document() for item in descriptors], api_key, mock)
        index = faiss.IndexIDMap2(faiss.IndexHNSWFlat(EMBEDDING_DIMENSIONS, 32, faiss.METRIC_INNER_PRODUCT))
        index.add_with_ids(
            np.asarray(vectors, dtype="float32"),
            np.asarray([item.vector_id for item in descriptors], dtype="int64"),
        )
        faiss.write_index(index, str(self._index_path))
        manifest = {
            "schema_version": 1,
            "catalog_fingerprint": fingerprint,
            "dense_model": "mock" if mock else DENSE_MODEL,
            "max_length_tokens": MAX_LENGTH_TOKENS,
            "dimensions": EMBEDDING_DIMENSIONS,
            "metric": "inner_product",
            "index_type": "IndexIDMap2(IndexHNSWFlat)",
            "descriptors": [asdict(item) for item in descriptors],
            "index_sha256": hashlib.sha256(self._index_path.read_bytes()).hexdigest(),
        }
        with self._manifest_path.open("wb") as stream:
            pickle.dump(manifest, stream, protocol=pickle.HIGHEST_PROTOCOL)
        return index, manifest

    @staticmethod
    def search(index, query_vector: list[float], top_k: int) -> list[tuple[int, float]]:
        scores, ids = index.search(np.asarray([query_vector], dtype="float32"), top_k)
        return [(int(vector_id), float(score)) for vector_id, score in zip(ids[0], scores[0]) if vector_id >= 0]


def reciprocal_rank_fusion(
    dense: list[tuple[int, float]], bm25: list[tuple[int, float]], top_k: int
) -> list[tuple[int, float]]:
    scores: dict[int, float] = {}
    for ranked in (dense, bm25):
        for rank, (candidate_id, _) in enumerate(ranked, start=1):
            scores[candidate_id] = scores.get(candidate_id, 0.0) + 1.0 / (RRF_K + rank)
    return sorted(scores.items(), key=lambda value: value[1], reverse=True)[:top_k]


def rerank(
    query: str,
    candidates: list[TopologyDescriptor],
    api_key: str,
    mock: bool,
    top_k: int,
) -> tuple[list[tuple[TopologyDescriptor, float]], str | None]:
    if mock:
        return [(candidate, float(len(candidates) - index)) for index, candidate in enumerate(candidates[:top_k])], None
    response = openrouter_json(
        "/rerank",
        api_key,
        {
            "model": RERANK_MODEL,
            "query": query,
            "documents": [candidate.retrieval_document() for candidate in candidates],
            "top_n": top_k,
        },
    )
    return [
        (candidates[item["index"]], float(item["relevance_score"]))
        for item in response["results"]
    ], response.get("id")


def main() -> int:
    parser = argparse.ArgumentParser(description="NeoGraph local topology retrieval cookbook")
    parser.add_argument("--query", required=True, help="Task or developer instruction to retrieve for")
    parser.add_argument("--index-dir", type=Path, default=Path(".neograph-topology-index"))
    parser.add_argument("--mock", action="store_true", help="Use deterministic offline embeddings/reranking")
    parser.add_argument("--rebuild", action="store_true", help="Discard local Faiss/manifest artifacts first")
    args = parser.parse_args()

    load_dotenv()
    api_key = os.getenv("OPENROUTER_API_KEY", "")
    if not args.mock and not api_key:
        parser.error("OPENROUTER_API_KEY is required unless --mock is set")
    if args.rebuild:
        for path in (args.index_dir / "topologies.faiss", args.index_dir / "topologies.manifest.pkl"):
            path.unlink(missing_ok=True)

    index_store = PersistentFaissIndex(args.index_dir)
    dense_index, manifest = index_store.load_or_build(CATALOG, api_key, args.mock)
    query_vector = bge_m3_embeddings([args.query], api_key, args.mock)[0]
    dense = index_store.search(dense_index, query_vector, top_k=40)
    bm25 = Bm25Index([candidate.retrieval_document() for candidate in CATALOG]).search(args.query, top_k=40)
    vector_to_catalog = {candidate.vector_id: candidate for candidate in CATALOG}
    dense_for_rrf = [(vector_id, score) for vector_id, score in dense if vector_id in vector_to_catalog]
    bm25_for_rrf = [(CATALOG[index].vector_id, score) for index, score in bm25]
    fused = reciprocal_rank_fusion(dense_for_rrf, bm25_for_rrf, top_k=20)
    fused_candidates = [vector_to_catalog[vector_id] for vector_id, _ in fused]
    reranked, rerank_response_id = rerank(args.query, fused_candidates, api_key, args.mock, top_k=5)

    if reranked:
        selected, relevance = reranked[0]
        decision = {
            "action": selected.migration_class,
            "topology_version_id": selected.topology_version_id,
            "candidate_descriptor_sha256": selected.descriptor_sha256,
            "planner_required": "resolve exact ProgramVersion, then rerun NeoGraph compatibility/admission",
            "rerank_relevance": relevance,
        }
    else:
        decision = {
            "action": "dsl_synthesis_request",
            "planner_required": "create ProgramSynthesisProposal; proposal has no execution authority",
        }

    print(json.dumps({
        "schema_version": 1,
        "pipeline": ["dense", "bm25", "rrf", "rerank"],
        "dense": {
            "model": manifest["dense_model"],
            "max_length_tokens": manifest["max_length_tokens"],
            "faiss_index": str(index_store._index_path),
            "manifest": str(index_store._manifest_path),
            "index_sha256": manifest["index_sha256"],
            "results": dense_for_rrf,
        },
        "bm25": {"results": bm25_for_rrf},
        "rrf": {"results": fused},
        "rerank": {"model": "mock" if args.mock else RERANK_MODEL, "response_id": rerank_response_id},
        "decision": decision,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
