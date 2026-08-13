#!/usr/bin/env bash
# 경계 계측 벤치 — OpenRouter 앞에 nginx 프록시를 세워 콜별 상류 시간을
# 로깅하고, 턴 왕복에서 차감한 "잔차"로 공정 비교.
#
# 사용:  OPENROUTER_API_KEY=... bash bench/run_bench_proxy.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
JARVIS="$ROOT/examples/cookbook/jarvis"
OUT="${BENCH_OUT:-/tmp/jarvis-bench-out}"
mkdir -p "$OUT/proxylogs"
NET=jarvis-bench-net
LIMITS=(--cpus=2 --memory=2g)

[[ -n "${OPENROUTER_API_KEY:-}" ]] || { echo "OPENROUTER_API_KEY 필요"; exit 1; }

echo "[proxy-bench] 이미지 빌드..."
docker build -q -f "$JARVIS/bench/Dockerfile.neograph"  -t jarvis-bench-neograph  "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.langgraph" -t jarvis-bench-langgraph "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.mcp"       -t jarvis-bench-mcp       "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.proxy"     -t jarvis-bench-proxy     "$ROOT"

docker network create "$NET" 2>/dev/null || true
docker rm -f jarvis-mcp-demo jarvis-openrouter-proxy 2>/dev/null || true
docker run -d --rm --name jarvis-mcp-demo   --network "$NET" jarvis-bench-mcp
docker run -d --rm --name jarvis-openrouter-proxy --network "$NET" \
    -v "$OUT/proxylogs":/logs jarvis-bench-proxy
trap 'docker rm -f jarvis-mcp-demo jarvis-openrouter-proxy >/dev/null 2>&1 || true' EXIT

echo "[proxy-bench] MCP/프록시 대기..."
for i in $(seq 1 30); do
    docker run --rm --network "$NET" jarvis-bench-mcp python3 -c \
        "import socket; socket.create_connection(('jarvis-mcp-demo',8888),2); socket.create_connection(('jarvis-openrouter-proxy',8080),2)" \
        2>/dev/null && break
    sleep 1
    [[ $i -eq 30 ]] && { echo "기동 실패"; exit 1; }
done

# The benchmark sends OPENROUTER_BASE_URL to both implementations so the
# provider call is routed through the same local proxy.
OPENROUTER_ENV=(-e OPENROUTER_API_KEY="$OPENROUTER_API_KEY"
                -e OPENROUTER_BASE_URL=http://jarvis-openrouter-proxy:8080/openrouter/v1
                -e JARVIS_DEBUG=1
                -e JARVIS_MEMORY_FILE=/tmp/mem.json)

# 라운드마다 프록시 로그를 분리 — 라운드 분할 추측(스모크 오염·스로틀 갭에
# 취약)을 제거. mv + nginx reopen 으로 프레임워크별 독립 로그.
rotate_log() {  # $1 = 대상 파일명
    docker exec jarvis-openrouter-proxy sh -c \
        "mv /logs/openrouter.log /logs/$1 2>/dev/null; nginx -s reopen" || true
    sleep 1
}
# 시작 전 잔여 로그 비우기 (이전 스모크/런 오염 차단)
docker exec jarvis-openrouter-proxy sh -c ': > /logs/openrouter.log; nginx -s reopen' || true

echo "[proxy-bench] NeoGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${OPENROUTER_ENV[@]}" \
    -v "$JARVIS/bench":/src/examples/cookbook/jarvis/bench:ro \
    -v "$JARVIS/config-bench-e2e":/src/examples/cookbook/jarvis/config-bench-e2e:ro \
    jarvis-bench-neograph python3 bench/driver.py \
    --cmd "exec bash scripts/run_jarvis.sh config-bench-e2e" \
    --turns bench/turns_e2e.txt --out /out/neograph_proxy.jsonl \
    --label neograph-proxy --delay 2
rotate_log openrouter_neograph.log

echo "[proxy-bench] LangGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${OPENROUTER_ENV[@]}" \
    -e BENCH_MODE=api -e MCP_URL=http://jarvis-mcp-demo:8888 \
    -e JARVIS_MCP_CATALOG=/app/config-bench-e2e/mcp_catalog.json \
    -v "$JARVIS/bench":/app/bench:ro \
    -v "$JARVIS/config-bench-e2e":/app/config-bench-e2e:ro \
    jarvis-bench-langgraph python3 bench/driver.py \
    --cmd "exec python3 bench/langgraph_twin.py" \
    --turns bench/turns_e2e.txt --out /out/langgraph_proxy.jsonl \
    --label langgraph-proxy --delay 2
rotate_log openrouter_langgraph.log

echo
echo "=== 완료시간(total) 잔차 ==="
python3 "$JARVIS/bench/analyze_proxy.py" \
    "neograph-proxy:$OUT/neograph_proxy.jsonl:$OUT/proxylogs/openrouter_neograph.log" \
    "langgraph-proxy:$OUT/langgraph_proxy.jsonl:$OUT/proxylogs/openrouter_langgraph.log"
echo
echo "=== TTFT (스트리밍 첫 토큰) ==="
python3 "$JARVIS/bench/analyze_ttft.py" \
    "neograph-proxy:$OUT/neograph_proxy.jsonl:$OUT/proxylogs/openrouter_neograph.log" \
    "langgraph-proxy:$OUT/langgraph_proxy.jsonl:$OUT/proxylogs/openrouter_langgraph.log"
