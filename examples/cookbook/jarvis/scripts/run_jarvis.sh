#!/usr/bin/env bash
# 자비스 cookbook 실행 wrapper.
#
# 왜 wrapper 가 필요한가:
#   third_party/whisper_install/lib/libwhisper.so 가 같은 디렉토리의
#   libggml*.so 들을 link-time 의존하지만 RUNPATH 가 비어 있어 (whisper.cpp
#   기본 빌드 옵션) DT_RUNPATH 가 transitive 적용되지 않는다. cookbook_jarvis
#   의 RUNPATH 만으로는 libwhisper 가 ggml 못 찾음.
#   LD_LIBRARY_PATH 한 줄 export 로 우회.
#
# 사용:
#   bash scripts/run_jarvis.sh                          # 기본 config (real-tools)
#   bash scripts/run_jarvis.sh config-demo/mock         # 다른 config 디렉토리
#   bash scripts/run_jarvis.sh /절대/경로/config        # 임의 config 도 OK
#
# 기본 빌드 디렉토리 (NEOGRAPH_BUILD_DIR 환경변수로 변경 가능):
#   ~/Coding/NeoGraph/build-jarvis

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # = examples/cookbook/jarvis
NEOGRAPH_ROOT="$(cd "$ROOT/../../.." && pwd)"             # = NeoGraph 루트
TP="$ROOT/third_party"
BIN="${NEOGRAPH_BUILD_DIR:-$NEOGRAPH_ROOT/build-jarvis}/examples/cookbook/jarvis/cookbook_jarvis"

# CFG: 첫 인자 그대로 받되, 상대경로면 cookbook 디렉토리 기준으로 해석.
CFG_ARG="${1:-config-demo/real-tools}"
case "$CFG_ARG" in
    /*) CFG="$CFG_ARG" ;;
    *)  CFG="$ROOT/$CFG_ARG" ;;
esac

if [[ ! -x "$BIN" ]]; then
    echo "[run_jarvis] 빌드된 cookbook_jarvis 없음: $BIN"
    echo "  cmake -B $NEOGRAPH_ROOT/build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON"
    echo "  cmake --build $NEOGRAPH_ROOT/build-jarvis --target cookbook_jarvis -j4"
    exit 1
fi
if [[ ! -d "$CFG" ]]; then
    echo "[run_jarvis] config 디렉토리 없음: $CFG"
    exit 1
fi

export LD_LIBRARY_PATH="$TP/onnxruntime/lib:$TP/whisper_install/lib:${LD_LIBRARY_PATH:-}"
# 모델 path 가 cwd 상대 (assets/...) 이므로 cookbook 디렉토리로 이동 후 실행.
cd "$ROOT"
exec "$BIN" "$CFG"
