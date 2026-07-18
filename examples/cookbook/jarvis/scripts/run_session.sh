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
JARVIS_BIN_DIR="$BUILD_DIR/examples/cookbook/jarvis"
if [[ -n "${JARVIS_BUILD_CONFIG:-}" ]]; then
    JARVIS_BIN_DIR="$JARVIS_BIN_DIR/$JARVIS_BUILD_CONFIG"
fi

required_bins=(
    cookbook_jarvis
    cookbook_jarvis_specialist_coder
    cookbook_jarvis_specialist_researcher
)
for bin in "${required_bins[@]}"; do
    if [[ ! -x "$JARVIS_BIN_DIR/$bin" ]]; then
        echo "[run_session] 빌드된 실행 파일 없음: $JARVIS_BIN_DIR/$bin"
        echo "  multi-config 빌드는 JARVIS_BUILD_CONFIG=Release 같이 지정하세요."
        echo "  cmake -B $BUILD_DIR -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON"
        echo "  cmake --build $BUILD_DIR --target cookbook_jarvis cookbook_jarvis_specialist_coder cookbook_jarvis_specialist_researcher -j"
        exit 1
    fi
done

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
"$JARVIS_BIN_DIR/cookbook_jarvis_specialist_coder" 8210 &
PIDS+=($!)

echo "[run_session] researcher specialist → 127.0.0.1:8211"
"$JARVIS_BIN_DIR/cookbook_jarvis_specialist_researcher" 8211 &
PIDS+=($!)

# --- 2. MCP 데모 서버 ---
if [[ -f "$ROOT/examples/demo_mcp_server.py" ]]; then
    echo "[run_session] MCP HTTP demo → 127.0.0.1:8000"
    python3 "$ROOT/examples/demo_mcp_server.py" &
    PIDS+=($!)
fi

# 서비스 깨어나길 잠깐 기다림
sleep 2

# --- 3. 자비스 본체 ---
echo "[run_session] jarvis 본체 기동"
echo "[run_session] 말해보세요. Ctrl-C 로 종료."
"$JARVIS_BIN_DIR/cookbook_jarvis" "$ROOT/examples/cookbook/jarvis/config"
