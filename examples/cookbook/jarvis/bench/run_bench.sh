#!/usr/bin/env bash
# NeoGraph vs LangGraph — 자비스 오케스트레이션 코어 벤치.
#
# 동일 제약(--cpus=2 --memory=2g)의 컨테이너 2종에서 동일 토폴로지를 구동:
#   mock 라운드 : LLM 0ms 스텁 → 순수 프레임워크 오버헤드 (200턴)
#   groq 라운드 : 실제 Groq 초고속 추론 (20턴, GROQ_API_KEY 필요)
#
# 사용:  GROQ_API_KEY=... bash bench/run_bench.sh
# 결과:  $BENCH_OUT(기본 /tmp/jarvis-bench-out)/*.jsonl + 비교표 stdout
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"   # NeoGraph 루트
OUT="${BENCH_OUT:-/tmp/jarvis-bench-out}"
mkdir -p "$OUT"
LIMITS=(--cpus=2 --memory=2g)

echo "[bench] 이미지 빌드..."
docker build -q -f "$ROOT/examples/cookbook/jarvis/bench/Dockerfile.neograph" \
    -t jarvis-bench-neograph "$ROOT"
docker build -q -f "$ROOT/examples/cookbook/jarvis/bench/Dockerfile.langgraph" \
    -t jarvis-bench-langgraph "$ROOT"

run_c() {  # run_c <추가 docker 인자...> <이미지> <driver 인자...>
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

if [[ -n "${GROQ_API_KEY:-}" ]]; then
    echo "[bench] groq 라운드 (실추론, 20턴, 턴간 2s)"
    GROQ_ENV=(-e OPENAI_API_KEY="$GROQ_API_KEY"
              -e OPENAI_BASE_URL=https://api.groq.com/openai
              -e JARVIS_ROUTER_MODEL="${BENCH_ROUTER_MODEL:-llama-3.1-8b-instant}"
              -e JARVIS_SYNTH_MODEL="${BENCH_SYNTH_MODEL:-llama-3.3-70b-versatile}")
    run_c "${GROQ_ENV[@]}" jarvis-bench-neograph python3 bench/driver.py \
        --cmd "exec bash scripts/run_jarvis.sh config-bench" \
        --turns bench/turns_groq.txt --out /out/neograph_groq.jsonl \
        --label neograph-groq --delay 2
    run_c "${GROQ_ENV[@]}" -e BENCH_MODE=api jarvis-bench-langgraph python3 bench/driver.py \
        --cmd "exec python3 bench/langgraph_twin.py" \
        --turns bench/turns_groq.txt --out /out/langgraph_groq.jsonl \
        --label langgraph-groq --delay 2
else
    echo "[bench] GROQ_API_KEY 없음 — groq 라운드 건너뜀"
fi

echo
python3 "$ROOT/examples/cookbook/jarvis/bench/analyze.py" "$OUT"/*.jsonl
