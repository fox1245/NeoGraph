#!/usr/bin/env python3
"""Fail when a WebAssembly build is larger than its budget, and report the sizes.

The report is a Markdown table on stdout; it is also appended to $GITHUB_STEP_SUMMARY when that is set, so a
workflow shows it in the job summary. Sizes are bytes: the .wasm as built, the .wasm gzipped at level 9, and
optionally the generated .js loader (reported, not gated). Without a budget a size is reported only.
"""

from __future__ import annotations

import argparse
import gzip
import os
import sys
from pathlib import Path


def kib(size: int) -> str:
    return f"{size / 1024:,.0f} KiB"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--wasm", type=Path, required=True, help="the built .wasm file")
    parser.add_argument("--js", type=Path, help="the generated .js loader (reported only)")
    parser.add_argument("--max-wasm", type=int, help="budget for the .wasm in bytes")
    parser.add_argument("--max-gzip", type=int, help="budget for the gzipped .wasm in bytes")
    parser.add_argument("--label", default="WebAssembly build", help="heading of the report")
    args = parser.parse_args()

    data = args.wasm.read_bytes()
    wasm_size = len(data)
    gzip_size = len(gzip.compress(data, compresslevel=9, mtime=0))

    rows = [
        (args.wasm.name, wasm_size, args.max_wasm),
        (f"{args.wasm.name} (gzip -9)", gzip_size, args.max_gzip),
    ]
    lines = [f"### {args.label}", "", "| File | Size | Budget | Used |", "|---|---:|---:|---:|"]
    over = []
    for name, size, budget in rows:
        if budget is None:
            lines.append(f"| {name} | {size:,} B ({kib(size)}) | not gated | |")
            continue
        lines.append(f"| {name} | {size:,} B ({kib(size)}) | {budget:,} B ({kib(budget)}) | "
                     f"{100 * size / budget:.1f}% |")
        if size > budget:
            over.append(f"{name} is {size:,} B, over its budget of {budget:,} B")
    if args.js:
        js_size = args.js.stat().st_size
        lines.append(f"| {args.js.name} | {js_size:,} B ({kib(js_size)}) | not gated | |")
    report = "\n".join(lines) + "\n"
    print(report)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(report + "\n")

    for message in over:
        print(f"size gate: {message}", file=sys.stderr)
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
