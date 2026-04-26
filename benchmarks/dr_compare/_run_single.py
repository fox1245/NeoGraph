"""Run exactly one query against the named impl. Stdout: '<elapsed>s <chars>'."""
import sys, time, uuid

impl = sys.argv[1]
query = sys.argv[2] if len(sys.argv) > 2 else "사과에 대해서 조사해줘"

if impl == "neograph":
    from dr_neograph import run_query
elif impl == "langgraph":
    from dr_langgraph import run_query
else:
    raise SystemExit(f"unknown impl: {impl}")

tid = f"cap-{uuid.uuid4().hex[:10]}"
t0 = time.perf_counter()
out = run_query(query, tid)
print(f"{time.perf_counter()-t0:.2f}s {len(out)}")
