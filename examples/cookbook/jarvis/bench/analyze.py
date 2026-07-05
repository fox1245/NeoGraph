#!/usr/bin/env python3
"""벤치 결과 비교표 — driver.py 가 남긴 *.jsonl 들의 summary 를 모아 출력."""
import json
import sys

summaries = []
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if "summary" in obj:
                summaries.append(obj["summary"])

if not summaries:
    sys.exit("no summaries found")

cols = ["label", "turns_ok", "startup_ms", "rss_max_kb",
        "mean_ms", "p50_ms", "p90_ms", "p99_ms", "min_ms", "max_ms"]
widths = {c: max(len(c), *(len(str(s.get(c, ""))) for s in summaries))
          for c in cols}
print("  ".join(c.ljust(widths[c]) for c in cols))
for s in sorted(summaries, key=lambda x: x["label"]):
    print("  ".join(str(s.get(c, "")).ljust(widths[c]) for c in cols))

# 짝(mock/api × neograph/langgraph) 오버헤드 델타
by = {s["label"]: s for s in summaries}
for suffix in ("mock", "groq"):
    a, b = by.get(f"neograph-{suffix}"), by.get(f"langgraph-{suffix}")
    if a and b:
        print(f"\n[{suffix}] 턴당 delta (langgraph − neograph): "
              f"mean {b['mean_ms'] - a['mean_ms']:+.3f}ms, "
              f"p50 {b['p50_ms'] - a['p50_ms']:+.3f}ms, "
              f"p99 {b['p99_ms'] - a['p99_ms']:+.3f}ms | "
              f"startup {b['startup_ms'] - a['startup_ms']:+.1f}ms | "
              f"rss {(b['rss_max_kb'] - a['rss_max_kb']) / 1024:+.1f}MB")
