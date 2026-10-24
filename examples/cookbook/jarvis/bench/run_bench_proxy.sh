#!/usr/bin/env bash
# Boundary-instrumented benchmark — places an nginx proxy in front of OpenRouter
# to log per-call upstream latency and compare the residual (turn round-trip minus upstream).
#
# Jarvis refuses to send API keys over plaintext HTTP (key-bearing endpoints require TLS).
# The proxy therefore listens on TLS (8443), and this script generates a throw-away CA
# per run as the trust anchor for both clients: OPENROUTER_CA_FILE (libcurl CAINFO) for
# Jarvis and SSL_CERT_FILE (httpx) for the LangGraph twin. The CA and keys live in a
# temporary directory and are cleaned up on exit.
#
# Usage:  OPENROUTER_API_KEY=... bash bench/run_bench_proxy.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
JARVIS="$ROOT/examples/cookbook/jarvis"
OUT="${BENCH_OUT:-/tmp/jarvis-bench-out}"
mkdir -p "$OUT/proxylogs"
NET=jarvis-bench-net
LIMITS=(--cpus=2 --memory=2g)
PROXY_HOST=jarvis-openrouter-proxy

[[ -n "${OPENROUTER_API_KEY:-}" ]] || { echo "OPENROUTER_API_KEY 필요"; exit 1; }
command -v openssl >/dev/null || { echo "openssl 필요 (일회용 프록시 CA 생성)"; exit 1; }

CERTS="$(mktemp -d)"
trap 'rm -rf "$CERTS"; docker rm -f jarvis-mcp-demo jarvis-openrouter-proxy >/dev/null 2>&1 || true' EXIT
mkdir "$CERTS/proxy"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=jarvis-bench-ca" \
    -keyout "$CERTS/ca.key" -out "$CERTS/ca.pem" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign"
openssl req -newkey rsa:2048 -nodes -subj "/CN=$PROXY_HOST" \
    -keyout "$CERTS/proxy/server.key" -out "$CERTS/server.csr"
printf 'subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' \
    "$PROXY_HOST" > "$CERTS/server.ext"
openssl x509 -req -days 2 -in "$CERTS/server.csr" -CA "$CERTS/ca.pem" -CAkey "$CERTS/ca.key" \
    -CAcreateserial -extfile "$CERTS/server.ext" -out "$CERTS/proxy/server.pem"

echo "[proxy-bench] 이미지 빌드..."
docker build -q -f "$JARVIS/bench/Dockerfile.neograph"  -t jarvis-bench-neograph  "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.langgraph" -t jarvis-bench-langgraph "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.mcp"       -t jarvis-bench-mcp       "$ROOT"
docker build -q -f "$JARVIS/bench/Dockerfile.proxy"     -t jarvis-bench-proxy     "$ROOT"

docker network create "$NET" 2>/dev/null || true
docker rm -f jarvis-mcp-demo jarvis-openrouter-proxy 2>/dev/null || true
docker run -d --rm --name jarvis-mcp-demo   --network "$NET" jarvis-bench-mcp
docker run -d --rm --name jarvis-openrouter-proxy --network "$NET" \
    -v "$OUT/proxylogs":/logs -v "$CERTS/proxy":/certs:ro jarvis-bench-proxy

# Wait for the proxy by verifying the full trust chain and hostname with the throw-away CA
# so certificate issues fail immediately rather than surfacing as a late TLS error on turn 1.
echo "[proxy-bench] MCP/프록시 대기..."
for i in $(seq 1 30); do
    docker run --rm --network "$NET" -v "$CERTS/ca.pem":/certs/ca.pem:ro jarvis-bench-mcp python3 -c \
        "import socket, ssl; socket.create_connection(('jarvis-mcp-demo',8888),2); ctx = ssl.create_default_context(cafile='/certs/ca.pem'); ctx.wrap_socket(socket.create_connection(('$PROXY_HOST',8443),2), server_hostname='$PROXY_HOST').close()" \
        2>/dev/null && break
    sleep 1
    [[ $i -eq 30 ]] && { echo "기동 실패"; exit 1; }
done

# The benchmark sends OPENROUTER_BASE_URL to both implementations so the
# provider call is routed through the same local TLS proxy. Both select the
# throw-away CA bundle (Jarvis: OPENROUTER_CA_FILE, LangGraph/httpx: SSL_CERT_FILE).
OPENROUTER_ENV=(-e OPENROUTER_API_KEY="$OPENROUTER_API_KEY"
                -e OPENROUTER_BASE_URL="https://$PROXY_HOST:8443/openrouter/v1"
                -e JARVIS_DEBUG=1
                -e JARVIS_MEMORY_FILE=/tmp/mem.json)

# Rotate the proxy log after each round to avoid heuristic round splitting (which is
# fragile under smoke noise or throttle gaps). mv + nginx reopen produces a separate log per framework.
rotate_log() {  # $1 = target filename
    docker exec jarvis-openrouter-proxy sh -c \
        "mv /logs/openrouter.log /logs/$1 && nginx -s reopen"
    sleep 1
}
# Truncate any leftover log before starting (prevents contamination from prior runs)
docker exec jarvis-openrouter-proxy sh -c ': > /logs/openrouter.log && nginx -s reopen'

echo "[proxy-bench] NeoGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${OPENROUTER_ENV[@]}" \
    -e OPENROUTER_CA_FILE=/certs/ca.pem -v "$CERTS/ca.pem":/certs/ca.pem:ro \
    -v "$JARVIS/bench":/src/examples/cookbook/jarvis/bench:ro \
    -v "$JARVIS/config-bench-e2e":/src/examples/cookbook/jarvis/config-bench-e2e:ro \
    jarvis-bench-neograph python3 bench/driver.py \
    --cmd "exec bash scripts/run_jarvis.sh config-bench-e2e" \
    --turns bench/turns_e2e.txt --out /out/neograph_proxy.jsonl \
    --label neograph-proxy --delay 2
rotate_log openrouter_neograph.log

echo "[proxy-bench] LangGraph 라운드..."
docker run --rm "${LIMITS[@]}" --network "$NET" -v "$OUT":/out "${OPENROUTER_ENV[@]}" \
    -e SSL_CERT_FILE=/certs/ca.pem -v "$CERTS/ca.pem":/certs/ca.pem:ro \
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
