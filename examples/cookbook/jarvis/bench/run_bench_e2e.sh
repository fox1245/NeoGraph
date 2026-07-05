#!/usr/bin/env bash
# E2E 벤치 — 실제 MCP 도구 왕복 포함 전 경로 (direct/parallel/chat/회상) × Groq.
#
# 구성: 공유 데모 MCP 서버 컨테이너(jarvis-mcp-demo) + 벤치 컨테이너 2종이
# 같은 docker 네트워크에서 순차 실행. 동일 제약(--cpus=2 --memory=2g).
#
# 사용:  GROQ_API_KEY=... bash bench/run_bench_e2e.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
JARVIS="$ROOT/examples/cookbook/jarvis"
OUT="${BENCH_OUT:-/tmp/jarvis-bench-out}"
mkdir -p "$OUT"
NET=jarvis-bench-net
LIMITS=(--cpus=2 --memory=2g)

[[ -n "${GROQ_API_KEY:-}" ]] || { echo "GROQ_API_KEY 필요"; exit 1; }

echo "[e2e] 이미지 빌드..."
docker build -q -f "$JARVIS/bench/Dockerfile.neograph"  -t jarvis-bench-neograph  "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.langgraph" -t jarvis-bench-langgraph "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.mcp"       -t jarvis-bench-mcp       "$ROOT"

docker network create "$NET" 2>/dev/null || true
docker rm -f jarvis-mcp-demo 2>/dev/null || true
docker run -d --rm --name jarvis-mcp-demo --network "$NET" jarvis-bench-mcp
trap 'docker rm -f jarvis-mcp-demo >/dev/null 2>&1 || true' EXIT

echo "[e2e] MCP 서버 대기..."
for i in $(seq 1 30); do
    if docker run --rm --network "$NET" jarvis-bench-mcp python3 -c \
        "import socket; socket.create_connection(('jarvis-mcp-demo', 8888), 2)" \
        2>/dev/null; then
        break
    fi
    sleep 1
    [[ $i -eq 30 ]] && { echo "MCP 서버 기동 실패"; exit 1; }
done

GROQ_ENV=(-e OPENAI_API_KEY="$GROQ_API_KEY"
          -e OPENAI_BASE_URL=https://api.groq.com/openai
          -e JARVIS_ROUTER_MODEL="${BENCH_ROUTER_MODEL:-llama-3.1-8b-instant}"
          -e JARVIS_SYNTH_MODEL="${BENCH_SYNTH_MODEL:-llama-3.3-70b-versatile}"
          -e JARVIS_DEBUG=1
          -e JARVIS_MEMORY_FILE=/tmp/mem.json)

echo "[e2e] NeoGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${GROQ_ENV[@]}" \
    -v "$JARVIS/bench":/src/examples/cookbook/jarvis/bench:ro \
    -v "$JARVIS/config-bench-e2e":/src/examples/cookbook/jarvis/config-bench-e2e:ro \
    jarvis-bench-neograph python3 bench/driver.py \
    --cmd "exec bash scripts/run_jarvis.sh config-bench-e2e" \
    --turns bench/turns_e2e.txt --out /out/neograph_e2e.jsonl \
    --label neograph-e2e --delay 2

echo "[e2e] LangGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${GROQ_ENV[@]}" \
    -e BENCH_MODE=api -e MCP_URL=http://jarvis-mcp-demo:8888 \
    -e JARVIS_MCP_CATALOG=/app/config-bench-e2e/mcp_catalog.json \
    -v "$JARVIS/bench":/app/bench:ro \
    -v "$JARVIS/config-bench-e2e":/app/config-bench-e2e:ro \
    jarvis-bench-langgraph python3 bench/driver.py \
    --cmd "exec python3 bench/langgraph_twin.py" \
    --turns bench/turns_e2e.txt --out /out/langgraph_e2e.jsonl \
    --label langgraph-e2e --delay 2

echo
python3 "$JARVIS/bench/analyze.py" "$OUT"/neograph_e2e.jsonl "$OUT"/langgraph_e2e.jsonl
