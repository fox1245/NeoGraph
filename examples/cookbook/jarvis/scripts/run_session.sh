#!/usr/bin/env bash
# jarvis 전체 시연 실행:
#   1) 전문가 서브에이전트 두 개 (coder, researcher) 백그라운드 띄움
#   2) MCP 데모 서버 띄움 (time/weather/calculator)
#   3) 자비스 본체 띄움 (음성 대기)
#   4) Ctrl-C 시 전부 깔끔하게 종료
#
# .env 에 OPENAI_API_KEY 가 있어야 풀 시연. 없으면 라우터/합성기가 MockProvider
# 로 동작 (도구 호출 흐름은 그대로 검증 가능, 응답 품질만 떨어짐).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

if [[ ! -x "$BUILD_DIR/cookbook_jarvis" ]]; then
    echo "[run_session] cookbook_jarvis 가 빌드되지 않았습니다."
    echo "  cmake -B $BUILD_DIR -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON"
    echo "  cmake --build $BUILD_DIR --target cookbook_jarvis cookbook_jarvis_coder cookbook_jarvis_researcher -j"
    exit 1
fi

PIDS=()
cleanup() {
    echo
    echo "[run_session] 종료 중..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "[run_session] 끝."
}
trap cleanup EXIT INT TERM

# --- 1. 전문가들 ---
echo "[run_session] coder specialist → 127.0.0.1:8210"
"$BUILD_DIR/cookbook_jarvis_coder" 8210 &
PIDS+=($!)

echo "[run_session] researcher specialist → 127.0.0.1:8211"
"$BUILD_DIR/cookbook_jarvis_researcher" 8211 &
PIDS+=($!)

# --- 2. MCP 데모 서버 ---
if [[ -f "$ROOT/examples/demo_mcp_http_server.py" ]]; then
    echo "[run_session] MCP HTTP demo → 127.0.0.1:8000"
    python3 "$ROOT/examples/demo_mcp_http_server.py" --port 8000 &
    PIDS+=($!)
fi

# 서비스 깨어나길 잠깐 기다림
sleep 2

# --- 3. 자비스 본체 ---
echo "[run_session] jarvis 본체 기동"
echo "[run_session] 말해보세요. Ctrl-C 로 종료."
"$BUILD_DIR/cookbook_jarvis" "$ROOT/examples/cookbook/jarvis/config"
