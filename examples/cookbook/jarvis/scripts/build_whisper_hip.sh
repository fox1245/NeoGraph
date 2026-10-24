#!/usr/bin/env bash
# Build whisper.cpp with the AMD ROCm (HIP) GPU backend and replace third_party/whisper_install.
#
# Why: the default bundled whisper.cpp is CPU-only, so whisper-large-v3-turbo on CPU takes
# ~32s (11s jfk clip), too slow for live mic input. With a ROCm build on GPU: ~7s(4.5×).
#
# Requires: ROCm (≥7.2 for the default gfx1201=R9700 target), hipcc/amdclang++, rocBLAS/hipBLAS.
#       On WSL2: /dev/dxg + librocdxg + HSA_ENABLE_DXG_DETECTION=1 (set by run_jarvis.sh).
# Verify: rocminfo must list your target GPU architecture (default: gfx1201; pass another gfx target as $1 for other GPUs).
#
# Usage: bash scripts/build_whisper_hip.sh [gfx_target]   # default: gfx1201 (override for other AMD GPUs)
set -euo pipefail
GPU_TARGET="${1:-gfx1201}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/third_party/whisper.cpp"
DEST="$ROOT/third_party/whisper_install"

# Auto-detect ROCm path
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
# Recreate soname symlinks
( cd "$DEST/lib"
  for base in libggml-base libggml-cpu libggml-hip libggml libwhisper; do
      full=$(ls ${base}.so.*.* 2>/dev/null | head -1); [ -z "$full" ] && continue
      major=$(echo "$full" | grep -oE 'so\.[0-9]+' | head -1)
      ln -sf "$full" "${base}.so"; ln -sf "$full" "${base}.so.${major#so.}"
  done )
# Refresh headers as well (in case of version changes)
cp -a "$SRC/include/"*.h "$SRC/ggml/include/"*.h "$DEST/include/" 2>/dev/null || true

echo "[whisper-hip] 완료. libggml-hip 배치: $(ls "$DEST/lib/libggml-hip.so" 2>/dev/null && echo OK)"
echo "  → run_jarvis.sh 가 ROCm 런타임+dxg 를 자동 로드. 실행 시 로그에"
echo "    'found 1 ROCm devices ... gfx1201' + 'using ROCm0 backend' 확인."
