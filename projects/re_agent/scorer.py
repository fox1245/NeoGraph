#!/usr/bin/env python3
"""Score the RE agent's output against ground_truth.json.

Compares semantically (name + summary + tags) using an OpenAI Responses
API call (v1/responses, REST — single-shot judge, not the WS path the
agent itself uses). Outputs a per-function verdict + aggregate score
suitable for CI gating.

Usage:
    ./scorer.py /tmp/agent_out.json
    ./scorer.py /tmp/agent_out.json --gt projects/re_agent/targets/ground_truth.json
    ./scorer.py /tmp/agent_out.json --model gpt-5.4-mini

Exit codes:
    0  — passed (matched_score >= --threshold, default 0.83 == 5/6)
    1  — failed score gate
    2  — usage / IO error

Env: OPENAI_API_KEY required. Loaded from cwd's .env (or any parent) via
the same convention as the C++ examples.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

try:
    import openai  # type: ignore
except ImportError:
    sys.stderr.write("scorer.py requires `openai>=1.0`. Install: pip install openai\n")
    sys.exit(2)


def load_dotenv_walk(start: Path) -> None:
    """Mimic cppdotenv::auto_load_dotenv — walk up from cwd looking for .env."""
    for d in [start, *start.parents]:
        env = d / ".env"
        if env.is_file():
            for line in env.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))
            return


JUDGE_PROMPT = """You are scoring a reverse-engineering agent's output against ground truth.

The agent analyzed a stripped binary and renamed each FUN_xxxxxx function
plus added a one-line summary. Ground truth is the original source.
For EACH ground-truth function, find the single best-matching agent
entry (or none) and score:

  name_match:    full | partial | none
    full    = same meaning even if naming differs (xor_encrypt vs xor_decrypt
              both count as `full` because XOR is symmetric)
    partial = right domain but missing key aspect (parse_args vs
              check_license_length — only the length-check part captured)
    none    = no agent entry plausibly maps to this GT function

  summary_match: full | partial | none
    full    = same operation described, even if wording differs
    partial = related but incomplete or partly wrong
    none    = wrong operation or absent

Output JSON ONLY (no markdown fence, no prose), schema:
{
  "matches": [
    {
      "gt_name": "<ground truth name>",
      "agent_renamed": "<agent's renamed value or null>",
      "agent_original": "<agent's original FUN_xxx or null>",
      "name_match":    "full" | "partial" | "none",
      "summary_match": "full" | "partial" | "none",
      "rationale":     "<one short sentence>"
    },
    ...one per ground truth function...
  ],
  "extra_agent_entries": ["<renamed name>", ...],
  "summary": {
    "total_gt": N,
    "name_full": K, "name_partial": L, "name_none": M,
    "summary_full": K2, "summary_partial": L2, "summary_none": M2,
    "matched_score": <float 0..1, name_full counts 1.0, partial 0.5, none 0>
  }
}
"""


def score(agent_path: Path, gt_path: Path, model: str) -> dict:
    agent = json.loads(agent_path.read_text())
    gt = json.loads(gt_path.read_text())

    payload = (
        f"GROUND TRUTH:\n{json.dumps(gt['functions'], indent=2)}\n\n"
        f"AGENT OUTPUT:\n{json.dumps(agent.get('recovered', []), indent=2)}"
    )

    client = openai.OpenAI()
    resp = client.responses.create(
        model=model,
        input=[
            {"role": "system", "content": JUDGE_PROMPT},
            {"role": "user", "content": payload},
        ],
    )
    text = resp.output_text.strip()
    # Tolerate accidental markdown fences just in case.
    if text.startswith("```"):
        text = text.split("\n", 1)[1].rsplit("```", 1)[0]
    return json.loads(text)


def render(report: dict) -> str:
    lines = ["", "=" * 70, "RE Agent Score Report", "=" * 70]
    s = report["summary"]
    lines.append(
        f"  matched_score: {s['matched_score']:.2f}    "
        f"(name full={s['name_full']}/partial={s['name_partial']}/none={s['name_none']}"
        f", total_gt={s['total_gt']})"
    )
    lines.append("")
    lines.append(f"  {'GT name':<22} {'agent name':<28} {'name':<8} {'summary':<8}")
    lines.append("  " + "-" * 66)
    for m in report["matches"]:
        lines.append(
            f"  {m['gt_name']:<22} {(m.get('agent_renamed') or '<none>'):<28} "
            f"{m['name_match']:<8} {m['summary_match']:<8}"
        )
        if m.get("rationale"):
            lines.append(f"     ↳ {m['rationale']}")
    extras = report.get("extra_agent_entries", [])
    if extras:
        lines.append("")
        lines.append(f"  Extra agent entries (no GT match — likely false positives):")
        for e in extras:
            lines.append(f"    - {e}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("agent_out", type=Path, help="Path to agent's stdout JSON")
    ap.add_argument("--gt", type=Path,
                    default=Path(__file__).parent / "targets" / "ground_truth.json")
    ap.add_argument("--model", default="gpt-5.4-mini")
    ap.add_argument("--threshold", type=float, default=0.83,
                    help="Pass gate on matched_score (default 0.83 == 5/6)")
    ap.add_argument("--report-json", type=Path, default=None,
                    help="Also write the full judge JSON here")
    args = ap.parse_args()

    if not args.agent_out.is_file():
        sys.stderr.write(f"Agent output not found: {args.agent_out}\n")
        return 2
    if not args.gt.is_file():
        sys.stderr.write(f"Ground truth not found: {args.gt}\n")
        return 2

    load_dotenv_walk(Path.cwd())
    if not os.environ.get("OPENAI_API_KEY"):
        sys.stderr.write("OPENAI_API_KEY not set (env or .env file)\n")
        return 2

    report = score(args.agent_out, args.gt, args.model)
    print(render(report))
    if args.report_json:
        args.report_json.write_text(json.dumps(report, indent=2))

    score_v = report["summary"]["matched_score"]
    if score_v < args.threshold:
        sys.stderr.write(
            f"FAIL: matched_score {score_v:.2f} < threshold {args.threshold:.2f}\n"
        )
        return 1
    sys.stderr.write(
        f"PASS: matched_score {score_v:.2f} >= threshold {args.threshold:.2f}\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
