#!/usr/bin/env bash
# Wrapper script to launch the JARVIS cookbook binary.
#
# Why a wrapper is needed:
#   third_party/whisper_install/lib/libwhisper.so depends at link time on
#   libggml*.so in the same directory, but the default whisper.cpp build leaves
#   its RUNPATH empty so DT_RUNPATH is not applied transitively. The RUNPATH on
#   cookbook_jarvis alone is not enough for libwhisper to locate ggml.
#   Exporting LD_LIBRARY_PATH works around this.
#
# Usage:
#   bash scripts/run_jarvis.sh                          # default config (real-tools)
#   bash scripts/run_jarvis.sh config-demo/mock         # alternate config directory
#   bash scripts/run_jarvis.sh path-to-config           # any config directory
#
# Build directory may be either the NeoGraph root build or a standalone
# Jarvis build against installed neograph and SchemaProvider packages.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # = examples/cookbook/jarvis
NEOGRAPH_ROOT="$(cd "$ROOT/../../.." && pwd)"             # = NeoGraph root
TP="$ROOT/third_party"
BUILD_DIR="${NEOGRAPH_BUILD_DIR:-$NEOGRAPH_ROOT/build-jarvis}"
CONFIG_SUFFIX="${JARVIS_BUILD_CONFIG:+/$JARVIS_BUILD_CONFIG}"
BIN="$BUILD_DIR/examples/cookbook/jarvis$CONFIG_SUFFIX/cookbook_jarvis"
if [[ ! -x "$BIN" && -x "$BUILD_DIR$CONFIG_SUFFIX/cookbook_jarvis" ]]; then
    BIN="$BUILD_DIR$CONFIG_SUFFIX/cookbook_jarvis"
fi

# CFG: take the first argument, resolving relative paths against the cookbook directory.
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

# If whisper.cpp is built with GGML_HIP (ROCm), the ROCm runtime (libamdhip64/rocblas/
# hipblas) and WSL dxg bridge are required for GPU execution. Added automatically when ROCM_PATH exists.
ROCM_LIB=""
for r in /opt/rocm-7.2.1/lib /opt/rocm/lib; do
    [ -f "$r/libamdhip64.so" ] && { ROCM_LIB="$r"; break; }
done
if [[ -n "$ROCM_LIB" ]]; then
    export LD_LIBRARY_PATH="$TP/whisper_install/lib:$ROCM_LIB:/usr/lib/wsl/lib:$TP/onnxruntime/lib:${LD_LIBRARY_PATH:-}"
    export HSA_ENABLE_DXG_DETECTION=1     # detect GPU on WSL2
else
    export LD_LIBRARY_PATH="$TP/onnxruntime/lib:$TP/whisper_install/lib:${LD_LIBRARY_PATH:-}"
fi
# Model paths are relative to cwd (assets/...), so cd into the cookbook directory before exec.
cd "$ROOT"
exec "$BIN" "$CFG"
