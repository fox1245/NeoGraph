#!/usr/bin/env bash
# Launch the full JARVIS demo session:
#   1) Start the two specialist sub-agents (coder, researcher) in the background
#   2) Start the MCP demo server (time/weather/calculator)
#   3) Start the main JARVIS process (listening for input)
#   4) Shut down all background processes cleanly on Ctrl-C
#
# Set OPENROUTER_API_KEY in .env for the live LLM demo. Without it, the router and
# synthesizer fall back to MockProvider (still exercising tool-calling and delegation paths).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${NEOGRAPH_BUILD_DIR:-${BUILD_DIR:-$ROOT/build-jarvis}}"  # same default as run_jarvis.sh and the README
CONFIG_SUFFIX="${JARVIS_BUILD_CONFIG:+/$JARVIS_BUILD_CONFIG}"
JARVIS_BIN_DIR="$BUILD_DIR/examples/cookbook/jarvis$CONFIG_SUFFIX"
if [[ ! -x "$JARVIS_BIN_DIR/cookbook_jarvis" && -x "$BUILD_DIR$CONFIG_SUFFIX/cookbook_jarvis" ]]; then
    JARVIS_BIN_DIR="$BUILD_DIR$CONFIG_SUFFIX"
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

# --- 1. Specialists ---
echo "[run_session] coder specialist → 127.0.0.1:8210"
"$JARVIS_BIN_DIR/cookbook_jarvis_specialist_coder" 8210 &
PIDS+=($!)

echo "[run_session] researcher specialist → 127.0.0.1:8211"
"$JARVIS_BIN_DIR/cookbook_jarvis_specialist_researcher" 8211 &
PIDS+=($!)

# --- 2. MCP demo server ---
if [[ -f "$ROOT/examples/mcp_demo_server.py" ]]; then
    echo "[run_session] MCP HTTP demo → 127.0.0.1:8000"
    python3 "$ROOT/examples/mcp_demo_server.py" --http 8000 --tools get_current_time,calculate,get_weather &
    PIDS+=($!)
fi

# Wait briefly for background services to come up
sleep 2

# --- 3. Main JARVIS process ---
echo "[run_session] jarvis 본체 기동"
echo "[run_session] 말해보세요. Ctrl-C 로 종료."
"$JARVIS_BIN_DIR/cookbook_jarvis" "$ROOT/examples/cookbook/jarvis/config"
