#!/usr/bin/env bash
# 경계 계측 벤치 — Groq 앞에 nginx 프록시를 세워 콜별 상류 시간을 로깅,
# 턴 왕복에서 차감한 "잔차"(클라이언트측 순수 시간)로 공정 비교.
#
# 사용:  GROQ_API_KEY=... bash bench/run_bench_proxy.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
JARVIS="$ROOT/examples/cookbook/jarvis"
OUT="${BENCH_OUT:-/tmp/jarvis-bench-out}"
mkdir -p "$OUT/proxylogs"
NET=jarvis-bench-net
LIMITS=(--cpus=2 --memory=2g)

[[ -n "${GROQ_API_KEY:-}" ]] || { echo "GROQ_API_KEY 필요"; exit 1; }

echo "[proxy-bench] 이미지 빌드..."
docker build -q -f "$JARVIS/bench/Dockerfile.neograph"  -t jarvis-bench-neograph  "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.langgraph" -t jarvis-bench-langgraph "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.mcp"       -t jarvis-bench-mcp       "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.proxy"     -t jarvis-bench-proxy     "$ROOT"

docker network create "$NET" 2>/dev/null || true
docker rm -f jarvis-mcp-demo jarvis-groq-proxy 2>/dev/null || true
docker run -d --rm --name jarvis-mcp-demo   --network "$NET" jarvis-bench-mcp
docker run -d --rm --name jarvis-groq-proxy --network "$NET" \
    -v "$OUT/proxylogs":/logs jarvis-bench-proxy
trap 'docker rm -f jarvis-mcp-demo jarvis-groq-proxy >/dev/null 2>&1 || true' EXIT

echo "[proxy-bench] MCP/프록시 대기..."
for i in $(seq 1 30); do
    docker run --rm --network "$NET" jarvis-bench-mcp python3 -c \
        "import socket; socket.create_connection(('jarvis-mcp-demo',8888),2); socket.create_connection(('jarvis-groq-proxy',8080),2)" \
        2>/dev/null && break
    sleep 1
    [[ $i -eq 30 ]] && { echo "기동 실패"; exit 1; }
done

# 프록시 경유 — base_url 만 교체, TLS/keepalive 는 프록시가 양쪽에 동일 제공
GROQ_ENV=(-e OPENAI_API_KEY="$GROQ_API_KEY"
          -e OPENAI_BASE_URL=http://jarvis-groq-proxy:8080/openai
          -e JARVIS_ROUTER_MODEL="${BENCH_ROUTER_MODEL:-llama-3.1-8b-instant}"
          -e JARVIS_SYNTH_MODEL="${BENCH_SYNTH_MODEL:-llama-3.3-70b-versatile}"
          -e JARVIS_DEBUG=1
          -e JARVIS_MEMORY_FILE=/tmp/mem.json)

echo "[proxy-bench] NeoGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${GROQ_ENV[@]}" \
    -v "$JARVIS/bench":/src/examples/cookbook/jarvis/bench:ro \
    -v "$JARVIS/config-bench-e2e":/src/examples/cookbook/jarvis/config-bench-e2e:ro \
    jarvis-bench-neograph python3 bench/driver.py \
    --cmd "exec bash scripts/run_jarvis.sh config-bench-e2e" \
    --turns bench/turns_e2e.txt --out /out/neograph_proxy.jsonl \
    --label neograph-proxy --delay 2

echo "[proxy-bench] LangGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${GROQ_ENV[@]}" \
    -e BENCH_MODE=api -e MCP_URL=http://jarvis-mcp-demo:8888 \
    -e JARVIS_MCP_CATALOG=/app/config-bench-e2e/mcp_catalog.json \
    -v "$JARVIS/bench":/app/bench:ro \
    -v "$JARVIS/config-bench-e2e":/app/config-bench-e2e:ro \
    jarvis-bench-langgraph python3 bench/driver.py \
    --cmd "exec python3 bench/langgraph_twin.py" \
    --turns bench/turns_e2e.txt --out /out/langgraph_proxy.jsonl \
    --label langgraph-proxy --delay 2

echo
python3 "$JARVIS/bench/analyze_proxy.py" "$OUT/proxylogs/groq.log" \
    "neograph-proxy:$OUT/neograph_proxy.jsonl" \
    "langgraph-proxy:$OUT/langgraph_proxy.jsonl"
