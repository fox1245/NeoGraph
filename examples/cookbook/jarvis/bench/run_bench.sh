#!/usr/bin/env bash
# NeoGraph vs LangGraph — JARVIS orchestration core benchmark.
#
# Runs the identical topology in two containers under identical limits (--cpus=2 --memory=2g):
#   mock round : 0ms LLM stub → pure framework overhead (200 turns)
#   OpenRouter round : pinned DeepSeek inference (20 turns, requires OPENROUTER_API_KEY)
#
# Usage: OPENROUTER_API_KEY=... bash bench/run_bench.sh
# Output: $BENCH_OUT (default /tmp/jarvis-bench-out)/*.jsonl + comparison table on stdout
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"   # NeoGraph root
OUT="${BENCH_OUT:-/tmp/jarvis-bench-out}"
mkdir -p "$OUT"
LIMITS=(--cpus=2 --memory=2g)

echo "[bench] 이미지 빌드..."
docker build -q -f "$ROOT/examples/cookbook/jarvis/bench/Dockerfile.neograph" \
    -t jarvis-bench-neograph "$ROOT"
docker build -q -f "$ROOT/examples/cookbook/jarvis/bench/Dockerfile.langgraph" \
    -t jarvis-bench-langgraph "$ROOT"

run_c() {  # run_c <extra docker args...> <image> <driver args...>
    docker run --rm "${LIMITS[@]}" -v "$OUT":/out \
        -e JARVIS_MEMORY_FILE=/tmp/mem.json "$@"
}

echo "[bench] mock 라운드 (0ms LLM, 200턴) — 순수 프레임워크 오버헤드"
run_c jarvis-bench-neograph python3 bench/driver.py \
    --cmd "exec bash scripts/run_jarvis.sh config-bench" \
    --turns bench/turns_mock.txt --out /out/neograph_mock.jsonl \
    --label neograph-mock
run_c -e BENCH_MODE=mock jarvis-bench-langgraph python3 bench/driver.py \
    --cmd "exec python3 bench/langgraph_twin.py" \
    --turns bench/turns_mock.txt --out /out/langgraph_mock.jsonl \
    --label langgraph-mock

if [[ -n "${OPENROUTER_API_KEY:-}" ]]; then
    echo "[bench] OpenRouter 라운드 (DeepSeek, 20턴, 턴간 2s)"
    OPENROUTER_ENV=(-e OPENROUTER_API_KEY="$OPENROUTER_API_KEY")
    run_c "${OPENROUTER_ENV[@]}" jarvis-bench-neograph python3 bench/driver.py \
        --cmd "exec bash scripts/run_jarvis.sh config-bench" \
        --turns bench/turns_openrouter.txt --out /out/neograph_openrouter.jsonl \
        --label neograph-openrouter --delay 2
    run_c "${OPENROUTER_ENV[@]}" -e BENCH_MODE=api jarvis-bench-langgraph python3 bench/driver.py \
        --cmd "exec python3 bench/langgraph_twin.py" \
        --turns bench/turns_openrouter.txt --out /out/langgraph_openrouter.jsonl \
        --label langgraph-openrouter --delay 2
else
    echo "[bench] OPENROUTER_API_KEY 없음 — OpenRouter 라운드 건너뜀"
fi

echo
python3 "$ROOT/examples/cookbook/jarvis/bench/analyze.py" "$OUT"/*.jsonl
