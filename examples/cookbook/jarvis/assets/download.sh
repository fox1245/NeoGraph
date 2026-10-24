#!/usr/bin/env bash
# examples/cookbook/jarvis/assets/download.sh
#
# Downloads all models needed for the JARVIS demo (~1.9GB default: ~1.6GB whisper-large-v3-turbo,
# ~183MB Moonshine fp32, ~120MB supertonic-3, ~2MB silero-vad; ~864MB total if JARVIS_WHISPER=small).
# Once downloaded, the local models run completely offline.
#
# Model sources and licenses:
#   supertonic-3     MIT     HuggingFace Supertone/supertonic-3
#   whisper.cpp      MIT     ggerganov ggml-converted model
#   silero-vad       MIT     snakers4/silero-vad ONNX export
#
# Fetches each asset into the local directory if not already present.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

echo "[jarvis] assets directory: ${SCRIPT_DIR}"

# ---------- 0. supertonic C++ inference helper (cpp/helper.{h,cpp}) ----------
# CMakeLists.txt compiles third_party/supertonic/helper.cpp directly, so both
# files are required to build. MIT license — authored by Supertone Inc.
HELPER_DIR="${SCRIPT_DIR}/../third_party/supertonic"
mkdir -p "${HELPER_DIR}"
HELPER_URL_BASE="https://raw.githubusercontent.com/supertone-inc/supertonic/main/cpp"
for f in helper.h helper.cpp; do
    if [[ ! -f "${HELPER_DIR}/${f}" ]]; then
        echo "[jarvis] downloading supertonic ${f} ..."
        curl -fsSL -o "${HELPER_DIR}/${f}" "${HELPER_URL_BASE}/${f}"
    fi
done

# ---------- 1. supertonic-3 (TTS model weights) ----------
# Hosted via HuggingFace LFS — requires git-lfs.
if [[ ! -d "supertonic-3" ]]; then
    echo "[jarvis] downloading supertonic-3 (TTS, ~120MB) ..."
    if ! command -v git-lfs >/dev/null; then
        echo "  ! git-lfs not installed. macOS: brew install git-lfs   linux: apt install git-lfs"
        exit 1
    fi
    git lfs install --skip-repo
    git clone --depth 1 https://huggingface.co/Supertone/supertonic-3 supertonic-3
    # supertonic-3/onnx/ and supertonic-3/voice_styles/ are the two main directories.
    ln -sf supertonic-3/onnx          onnx
    ln -sf supertonic-3/voice_styles  voices
else
    echo "[jarvis] supertonic-3 already present — skipping."
fi

# ---------- 2. whisper.cpp STT model ----------
# Default: large-v3-turbo (~1.6GB) — 99-language auto-detection + transcription.
# Lighter alternatives: JARVIS_WHISPER=small (~470MB) or base/tiny (Raspberry Pi).
# stt.model_path in the config must match the filename downloaded here.
WHISPER_VARIANT="${JARVIS_WHISPER:-large-v3-turbo}"
WHISPER_MODEL="whisper-${WHISPER_VARIANT}.bin"
WHISPER_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${WHISPER_VARIANT}.bin"
if [[ ! -f "${WHISPER_MODEL}" ]]; then
    echo "[jarvis] downloading whisper ${WHISPER_VARIANT}..."
    curl -L -o "${WHISPER_MODEL}" "${WHISPER_URL}"
else
    echo "[jarvis] whisper model already present — skipping."
fi

# ---------- 3. silero-vad ONNX ----------
SILERO_MODEL="silero_vad.onnx"
SILERO_URL="https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx"
if [[ ! -f "${SILERO_MODEL}" ]]; then
    echo "[jarvis] downloading silero VAD (~2MB)..."
    curl -L -o "${SILERO_MODEL}" "${SILERO_URL}"
else
    echo "[jarvis] silero VAD already present — skipping."
fi

# ---------- 4. Moonshine-tiny-ko STT (ONNX, alternative to whisper) ----------
# Low-latency edge STT. Downloads fp32 models because the bundled minimal ONNX
# Runtime build omits the int8 ConvInteger/MatMulInteger kernels; with a full ORT
# build, onnx/*_int8.onnx (~28MB) can be used instead via the config encoder/decoder keys.
MOONSHINE_DIR="moonshine-tiny-ko"
if [[ ! -f "${MOONSHINE_DIR}/onnx/encoder_model.onnx" ]]; then
    echo "[jarvis] downloading Moonshine-tiny-ko STT (~183MB fp32)..."
    if command -v hf >/dev/null 2>&1; then
        hf download onnx-community/moonshine-tiny-ko-ONNX \
            --include "onnx/encoder_model.onnx" "onnx/decoder_model.onnx" \
                      "onnx/decoder_with_past_model.onnx" "tokenizer.json" "config.json" \
            --local-dir "${MOONSHINE_DIR}"
    else
        MS_BASE="https://huggingface.co/onnx-community/moonshine-tiny-ko-ONNX/resolve/main"
        mkdir -p "${MOONSHINE_DIR}/onnx"
        for f in onnx/encoder_model.onnx onnx/decoder_model.onnx \
                 onnx/decoder_with_past_model.onnx tokenizer.json config.json; do
            curl -L -o "${MOONSHINE_DIR}/${f}" "${MS_BASE}/${f}"
        done
    fi
else
    echo "[jarvis] Moonshine-tiny-ko already present — skipping."
fi

echo
echo "[jarvis] assets ready:"
ls -lh onnx voices ${WHISPER_MODEL} ${SILERO_MODEL} 2>/dev/null || true
echo
echo "[jarvis] next:"
echo "  cmake -B build -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON"
echo "  cmake --build build --target cookbook_jarvis -j"
echo "  bash examples/cookbook/jarvis/scripts/run_session.sh"
