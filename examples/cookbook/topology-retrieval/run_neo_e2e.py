#!/usr/bin/env python3
"""Run an OpenRouter retrieval decision through NeoGraph's C++ consumer."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Topology retrieval -> NeoGraph C++ E2E")
    parser.add_argument("--consumer", required=True, type=Path,
                        help="Path to example_topology_retrieval executable")
    parser.add_argument("--query", required=True)
    parser.add_argument("--index-dir", type=Path, default=Path(".neograph-topology-index"))
    parser.add_argument("--mock", action="store_true")
    args = parser.parse_args()

    retriever = Path(__file__).with_name("topology_retrieval.py")
    command = [sys.executable, str(retriever), "--query", args.query,
               "--index-dir", str(args.index_dir)]
    if args.mock:
        command.append("--mock")
    retrieval = subprocess.run(command, check=True, text=True, capture_output=True)
    decision = json.loads(retrieval.stdout)["decision"]
    if decision["action"] == "dsl_synthesis_request":
        print(json.dumps({"retrieval": decision, "consumer": "not-started"}, indent=2))
        return 0

    consumer = subprocess.run(
        [str(args.consumer), "--candidate-key", decision["candidate_key"]],
        check=True, text=True, capture_output=True,
    )
    result = json.loads(consumer.stdout)
    if not result.get("verified"):
        raise RuntimeError("NeoGraph consumer did not verify the retrieved candidate")
    print(json.dumps({"retrieval": decision, "neograph": result}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
