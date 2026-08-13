#!/usr/bin/env bash
# Configure matched Release builds, then run the immutable QuickJS gate.
# The build root must not exist: enabled-unused versus disabled Core is only
# meaningful when both configurations start clean and differ by one option.
set -euo pipefail

if (( $# < 2 )); then
    echo "usage: scripts/build_quickjs_performance_matrix.sh <fresh-build-root> <result.json> [runner options]" >&2
    exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root=$1
result=$2
shift 2

if [[ -e "$build_root" ]]; then
    echo "error: build root already exists: $build_root" >&2
    exit 2
fi
mkdir -p "$build_root"

disabled_build="$build_root/quickjs-disabled"
enabled_build="$build_root/quickjs-enabled"
common_options=(
    -DCMAKE_BUILD_TYPE=Release
    -DNEOGRAPH_BUILD_TESTS=OFF
    -DNEOGRAPH_BUILD_BENCHMARKS=ON
    -DNEOGRAPH_BUILD_PROGRAM=ON
    -DNEOGRAPH_BUILD_ASYNC=OFF
    -DNEOGRAPH_BUILD_LLM=OFF
    -DNEOGRAPH_BUILD_MCP=OFF
    -DNEOGRAPH_BUILD_MCP_CLIENT=OFF
    -DNEOGRAPH_BUILD_MCP_SERVER=OFF
    -DNEOGRAPH_BUILD_A2A=OFF
    -DNEOGRAPH_BUILD_ACP=OFF
    -DNEOGRAPH_BUILD_UTIL=OFF
    -DNEOGRAPH_BUILD_POSTGRES=OFF
    -DNEOGRAPH_BUILD_SQLITE=OFF
    -DNEOGRAPH_BUILD_EXAMPLES=OFF
    -DNEOGRAPH_BUILD_GRPC=OFF
    -DNEOGRAPH_USE_LIBCURL=OFF
)

cmake -S "$repo_root" -B "$disabled_build" \
    "${common_options[@]}" -DNEOGRAPH_BUILD_QUICKJS_CONTROL=OFF
cmake --build "$disabled_build" --target bench_core_quickjs_probe --parallel

cmake -S "$repo_root" -B "$enabled_build" \
    "${common_options[@]}" -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build "$enabled_build" --target \
    bench_core_quickjs_probe bench_quickjs_primitives bench_quickjs_control --parallel

python3 "$repo_root/scripts/run_quickjs_performance.py" \
    --primitive-binary "$enabled_build/bench_quickjs_primitives" \
    --control-binary "$enabled_build/bench_quickjs_control" \
    --core-enabled-binary "$enabled_build/bench_core_quickjs_probe" \
    --core-disabled-binary "$disabled_build/bench_core_quickjs_probe" \
    --output "$result" \
    "$@"
