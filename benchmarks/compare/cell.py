#!/usr/bin/env python3
"""Run one cell: one variant of one framework on one workload, in this process.

    cell.py --framework langgraph --variant ainvoke_async --workload seq --iters 2000 --samples 3
    cell.py --framework langgraph --variant ainvoke_async --workload seq --mode cold
    cell.py --framework langgraph --variant ainvoke_async --workload seq --mode inflight --n 64
    cell.py --framework pass --mode idle                 (the interpreter with nothing imported)

The last line on stdout is `record<TAB>{json}` (see common.py for the validity rule). The exit status is 0 for a
valid cell and 3 for an error cell, 2 for a usage error.
"""

import argparse
import importlib
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import common  # noqa: E402

PORTS = {
    "langgraph": "port_langgraph",
    "haystack": "port_haystack",
    "pydantic_graph": "port_pydantic_graph",
    "llamaindex": "port_llamaindex",
    "autogen": "port_autogen",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--framework", required=True, choices=sorted(PORTS) + ["pass"])
    parser.add_argument("--variant")
    parser.add_argument("--workload", choices=common.WORKLOADS)
    parser.add_argument("--mode", default="loop", choices=("loop", "cold", "inflight", "idle"))
    parser.add_argument("--iters", type=int, default=1000)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--n", type=int, default=1)
    args = parser.parse_args()

    base = {
        "schema": common.SCHEMA,
        "tool": "compare.cell",
        "mode": args.mode,
        "framework": args.framework,
        "variant": args.variant,
        "workload": args.workload,
        "python": sys.version.split()[0],
    }
    if args.framework == "pass":
        record = common.run_idle(None, base)
        return common.finish(record)

    port = importlib.import_module(PORTS[args.framework])
    base["versions"] = common.package_versions(port.PACKAGES)
    if args.mode == "idle":
        return common.finish(common.run_idle(port, base))
    if args.variant not in port.VARIANTS:
        print(f"unknown variant {args.variant!r}; have {sorted(port.VARIANTS)}", file=sys.stderr)
        return 2
    if args.workload is None:
        print("--workload is required", file=sys.stderr)
        return 2
    if args.mode == "cold":
        record = common.run_cold(port, args.variant, args.workload, base)
    elif args.mode == "inflight":
        record = common.run_inflight(port, args.variant, args.workload, args.n, base)
    else:
        base.update({"iters": args.iters, "samples": args.samples, "warmup": args.warmup})
        record = common.run_loop(port, args.variant, args.workload, args.iters, args.samples, args.warmup, base)
    return common.finish(record)


if __name__ == "__main__":
    sys.exit(main())
