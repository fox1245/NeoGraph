#!/usr/bin/env bash
# whisper.cpp 를 AMD ROCm(HIP) GPU 백엔드로 빌드 → third_party/whisper_install 교체.
#
# 왜: 기본 번들 whisper.cpp 는 CPU 전용이라 whisper-large-v3-turbo 가 CPU 에서
# ~32초(jfk 11초 클립)로 라이브 마이크에 부적합. ROCm 빌드 시 GPU 로 ~7초(4.5×).
#
# 요구: ROCm ≥7.2 (gfx1201=R9700 은 7.2 필요), hipcc/amdclang++, rocBLAS/hipBLAS.
#       WSL2 는 /dev/dxg + librocdxg + HSA_ENABLE_DXG_DETECTION=1 (run_jarvis.sh 가 설정).
# 검증: rocminfo 가 gfx1201 을 봐야 함.
#
# 사용:  bash scripts/build_whisper_hip.sh [gfx타깃]   # 기본 gfx1201
set -euo pipefail
GPU_TARGET="${1:-gfx1201}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/third_party/whisper.cpp"
DEST="$ROOT/third_party/whisper_install"

# ROCm 경로 자동 탐지
ROCM=""
for r in /opt/rocm-7.2.1 /opt/rocm; do
    [ -x "$r/bin/hipcc" ] && { ROCM="$r"; break; }
done
[ -z "$ROCM" ] && { echo "ROCm 미발견 (/opt/rocm*)"; exit 1; }
export PATH="$ROCM/bin:$PATH" ROCM_PATH="$ROCM" HIP_PATH="$ROCM"

[ -d "$SRC" ] || { echo "whisper.cpp 소스 없음: $SRC — download.sh 먼저"; exit 1; }

echo "[whisper-hip] ROCm=$ROCM 타깃=$GPU_TARGET 로 빌드..."
cmake -B "$SRC/build-hip" -S "$SRC" \
    -DGGML_HIP=ON -DGPU_TARGETS="$GPU_TARGET" -DAMDGPU_TARGETS="$GPU_TARGET" \
    -DCMAKE_HIP_COMPILER="$ROCM/lib/llvm/bin/clang++" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
    -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_SERVER=OFF \
    -DCMAKE_PREFIX_PATH="$ROCM"
cmake --build "$SRC/build-hip" -j"$(nproc)"

echo "[whisper-hip] whisper_install 교체 (CPU 백업 → cpu_backup)..."
mkdir -p "$DEST/lib" "$DEST/cpu_backup"
cp -a "$DEST"/lib/*.so* "$DEST/cpu_backup/" 2>/dev/null || true
find "$SRC/build-hip" -name "*.so*" -type f -exec cp -a {} "$DEST/lib/" \;
# soname 링크 재생성
( cd "$DEST/lib"
  for base in libggml-base libggml-cpu libggml-hip libggml libwhisper; do
      full=$(ls ${base}.so.*.* 2>/dev/null | head -1); [ -z "$full" ] && continue
      major=$(echo "$full" | grep -oE 'so\.[0-9]+' | head -1)
      ln -sf "$full" "${base}.so"; ln -sf "$full" "${base}.so.${major#so.}"
  done )
# 헤더도 갱신 (버전 변동 대비)
cp -a "$SRC/include/"*.h "$SRC/ggml/include/"*.h "$DEST/include/" 2>/dev/null || true

echo "[whisper-hip] 완료. libggml-hip 배치: $(ls "$DEST/lib/libggml-hip.so" 2>/dev/null && echo OK)"
echo "  → run_jarvis.sh 가 ROCm 런타임+dxg 를 자동 로드. 실행 시 로그에"
echo "    'found 1 ROCm devices ... gfx1201' + 'using ROCm0 backend' 확인."
