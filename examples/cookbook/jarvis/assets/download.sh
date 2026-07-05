#!/usr/bin/env bash
# examples/cookbook/jarvis/assets/download.sh
#
# 자비스 시연에 필요한 모든 모델을 받음.
# 약 250 MB. 인터넷 한 번이면 끝, 이후 완전 오프라인 동작.
#
# 모델 출처와 라이선스:
#   supertonic-3     MIT     HuggingFace Supertone/supertonic-3
#   whisper.cpp      MIT     ggerganov 의 ggml 변환 모델 (small.bin)
#   silero-vad       MIT     snakers4/silero-vad ONNX export
#
# 골격(skeleton) 단계: 아래 URL 들은 실제 다운로드 단계에서 검증/확정 예정.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

echo "[jarvis] assets directory: ${SCRIPT_DIR}"

# ---------- 0. supertonic 의 C++ 추론 헬퍼 (cpp/helper.{h,cpp}) ----------
# CMakeLists.txt 가 third_party/supertonic/helper.cpp 를 직접 컴파일에 포함시키므로
# 이 두 파일이 반드시 있어야 빌드됨. 라이선스 MIT — Supertone Inc. 저작.
HELPER_DIR="${SCRIPT_DIR}/../third_party/supertonic"
mkdir -p "${HELPER_DIR}"
HELPER_URL_BASE="https://raw.githubusercontent.com/supertone-inc/supertonic/main/cpp"
for f in helper.h helper.cpp; do
    if [[ ! -f "${HELPER_DIR}/${f}" ]]; then
        echo "[jarvis] downloading supertonic ${f} ..."
        curl -fsSL -o "${HELPER_DIR}/${f}" "${HELPER_URL_BASE}/${f}"
    fi
done

# ---------- 1. supertonic-3 (TTS 모델 가중치) ----------
# HuggingFace LFS 사용 — git lfs 필수.
if [[ ! -d "supertonic-3" ]]; then
    echo "[jarvis] downloading supertonic-3 (TTS, ~120MB) ..."
    if ! command -v git-lfs >/dev/null; then
        echo "  ! git-lfs not installed. macOS: brew install git-lfs   linux: apt install git-lfs"
        exit 1
    fi
    git lfs install --skip-repo
    git clone --depth 1 https://huggingface.co/Supertone/supertonic-3 supertonic-3
    # supertonic-3/onnx/  와 supertonic-3/voice_styles/ 두 디렉토리가 본체.
    ln -sf supertonic-3/onnx          onnx
    ln -sf supertonic-3/voice_styles  voices
else
    echo "[jarvis] supertonic-3 already present — skipping."
fi

# ---------- 2. whisper.cpp STT 모델 ----------
# 기본: large-v3-turbo (~1.6GB) — 99개 언어 자동감지 + 전사, 화자 언어로 응답.
# 경량 대안: JARVIS_WHISPER=small(~470MB) 또는 base/tiny (라즈베리파이).
# config 의 stt.model_path 가 여기서 받는 파일명과 일치해야 함.
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

# ---------- 4. Moonshine-tiny-ko STT (ONNX, whisper 대체 옵션) ----------
# 엣지/저지연 STT. fp32 를 받는다 — jarvis 번들 ONNX Runtime(축소 빌드)이
# int8 의 ConvInteger/MatMulInteger 커널을 미포함하므로. 풀 ORT 빌드면
# onnx/*_int8.onnx (~28MB) 로 교체 가능(config 의 encoder/decoder 키).
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
