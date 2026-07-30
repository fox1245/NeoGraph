#!/usr/bin/env bash
#
# Can a downstream CMake project actually consume an installed NeoGraph?
#
# Builds the engine, installs it into a throwaway prefix, then configures,
# builds and runs tests/integration/find_package — a project that sees nothing
# but that prefix. This is the only test that can catch a broken install: every
# unit test in the repo links against the build tree, where the headers, the
# vendored deps and the libraries are all reachable by accident.
#
#   exit 0 — a consumer can find_package(NeoGraph), link core/mcp_sqlite, and run
#   exit 1 — it cannot, at whichever of the four stages failed
#
# Shared mode also verifies the platform loader metadata and versioned links.
# Usage: scripts/test_find_package.sh [--keep] [--shared] [--core-only|--program]
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel)
work=$(mktemp -d -t ng-findpkg-XXXXXX)
if command -v nproc >/dev/null 2>&1; then
    jobs=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    jobs=$(sysctl -n hw.ncpu)
else
    jobs=2
fi
keep=
shared=OFF
component_mode=default
platform=$(uname -s)
for arg in "$@"; do
    case "$arg" in
        --keep) keep=--keep ;;
        --shared) shared=ON ;;
        --core-only) component_mode=core ;;
        --program) component_mode=program ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

consumer_project="$repo_root/tests/integration/find_package"
component_cmake_args=(-DNEOGRAPH_BUILD_PROGRAM=OFF)
if [[ "$component_mode" != "default" ]]; then
    component_cmake_args=(
        -DNEOGRAPH_BUILD_ASYNC=OFF
        -DNEOGRAPH_BUILD_LLM=OFF
        -DNEOGRAPH_BUILD_MCP=OFF
        -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF
        -DNEOGRAPH_BUILD_A2A=OFF
        -DNEOGRAPH_BUILD_ACP=OFF
        -DNEOGRAPH_BUILD_UTIL=OFF
        -DNEOGRAPH_BUILD_POSTGRES=OFF
        -DNEOGRAPH_BUILD_SQLITE=OFF
        -DNEOGRAPH_BUILD_GRPC=OFF
        -DNEOGRAPH_BUILD_PYBIND=OFF
        -DNEOGRAPH_BUILD_HARNESS_MCP_BINARY=OFF
        -DNEOGRAPH_USE_LIBCURL=OFF
        -DNEOGRAPH_BUILD_PROGRAM=OFF)
fi
if [[ "$component_mode" == "program" ]]; then
    component_cmake_args[${#component_cmake_args[@]}-1]=-DNEOGRAPH_BUILD_PROGRAM=ON
    consumer_project="$repo_root/tests/integration/find_package_program"
fi

cleanup() { [[ "$keep" == "--keep" ]] || rm -rf "$work"; }
trap cleanup EXIT

prefix="$work/prefix"
echo "── prefix: $prefix"

version=$(sed -nE 's/^version[[:space:]]*=[[:space:]]*"([0-9]+\.[0-9]+\.[0-9]+)"/\1/p' \
    "$repo_root/pyproject.toml" | head -1)
[[ -n "$version" ]] || { echo "failed to read project version" >&2; exit 1; }
major=${version%%.*}

# Keep this non-empty: macOS ships Bash 3.2, where expanding an empty array
# under `set -u` raises "unbound variable".
platform_cmake_args=(-DNEOGRAPH_INSTALL_HEADERS=ON)
consumer_exe="$work/consumer/consumer"
case "$platform" in
    MINGW*|MSYS*|CYGWIN*)
        platform_cmake_args+=(
            -DNEOGRAPH_BUILD_POSTGRES=OFF
            -DNEOGRAPH_BUILD_SQLITE=OFF
            -DNEOGRAPH_USE_LIBCURL=OFF)
        if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
            openssl_root=$(cygpath -m "$OPENSSL_ROOT_DIR")
            platform_cmake_args+=("-DOPENSSL_ROOT_DIR=$openssl_root")
        fi
        consumer_exe="$work/consumer/Release/consumer.exe"
        ;;
esac

step() { echo; echo "── $1"; }
fail() { echo "   FAILED: $1"; exit 1; }

check_shared_metadata() {
    case "$platform" in
        Linux)
            local libraries=("$prefix/lib"/libneograph_*.so."$version")
            ((${#libraries[@]} > 0)) || fail "no full-version ELF libraries installed"
            local real base compat expected dynamic
            for real in "${libraries[@]}"; do
                base=${real%.so."$version"}
                compat="$base.so.$major"
                expected="$(basename "$base").so.$major"
                [[ -f "$real" ]] || fail "missing full-version ELF library: $real"
                [[ -L "$compat" ]] || fail "missing SOVERSION ELF link: $compat"
                [[ -L "$base.so" ]] || fail "missing ELF linker name: $base.so"
                dynamic=$(readelf -d "$real") || fail "readelf failed: $real"
                [[ "$dynamic" == *"(SONAME)"*"[$expected]"* ]] \
                    || fail "ELF SONAME is not $expected: $real"
                [[ "$dynamic" == *"(RUNPATH)"*'[$ORIGIN]'* \
                   || "$dynamic" == *"(RPATH)"*'[$ORIGIN]'* ]] \
                    || fail "ELF sibling RPATH is not \$ORIGIN: $real"
            done
            ;;
        Darwin)
            local libraries=("$prefix/lib"/libneograph_*."$version".dylib)
            ((${#libraries[@]} > 0)) || fail "no full-version Mach-O libraries installed"
            local real base compat expected identity dependencies load_commands
            for real in "${libraries[@]}"; do
                base=${real%."$version".dylib}
                compat="$base.$major.dylib"
                expected="$(basename "$base").$major.dylib"
                [[ -f "$real" ]] || fail "missing full-version Mach-O library: $real"
                [[ -L "$compat" ]] || fail "missing SOVERSION Mach-O link: $compat"
                [[ -L "$base.dylib" ]] || fail "missing Mach-O linker name: $base.dylib"
                identity=$(otool -D "$real") || fail "otool failed: $real"
                [[ "$identity" == *"$expected"* ]] \
                    || fail "Mach-O install name is not $expected: $real"
                dependencies=$(otool -L "$real" | tail -n +2) \
                    || fail "otool dependency read failed: $real"
                if [[ "$dependencies" == *"libneograph_"* ]]; then
                    [[ "$dependencies" == *"@rpath/libneograph_"* ]] \
                        || fail "Mach-O NeoGraph dependency is not @rpath-relative: $real"
                fi
                load_commands=$(otool -l "$real") \
                    || fail "otool load-command read failed: $real"
                [[ "$load_commands" == *"LC_RPATH"*"path @loader_path "* ]] \
                    || fail "Mach-O sibling RPATH is not @loader_path: $real"
            done
            ;;
        MINGW*|MSYS*|CYGWIN*)
            [[ -f "$prefix/bin/neograph_core.dll" ]] \
                || fail "Windows DLL name must remain unsuffixed"
            local dll
            for dll in "$prefix/bin"/neograph_*.dll; do
                [[ "$(basename "$dll")" != *".$major.dll" \
                   && "$(basename "$dll")" != *".$version.dll" ]] \
                    || fail "Windows DLL unexpectedly has a version suffix: $dll"
            done
            ;;
        *)
            fail "unsupported platform for shared-library metadata check: $platform"
            ;;
    esac
}

run_consumer() {
    local library_prefix=$1
    case "$platform" in
        Linux)
            LD_LIBRARY_PATH="$library_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                "$consumer_exe"
            ;;
        Darwin)
            DYLD_LIBRARY_PATH="$library_prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                "$consumer_exe"
            ;;
        MINGW*|MSYS*|CYGWIN*)
            PATH="$library_prefix/bin:$PATH" "$consumer_exe"
            ;;
        *)
            "$consumer_exe"
            ;;
    esac
}

step "1/4 configure engine"
cmake -S "$repo_root" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS="$shared" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DNEOGRAPH_BUILD_TESTS=OFF \
    -DNEOGRAPH_BUILD_EXAMPLES=OFF \
    "${platform_cmake_args[@]}" \
    "${component_cmake_args[@]}" \
    > "$work/configure.log" 2>&1 || { tail -20 "$work/configure.log"; fail "engine configure"; }

step "2/4 build + install engine"
cmake --build "$work/build" --config Release -j"$jobs" --target install \
    > "$work/install.log" 2>&1 || { tail -20 "$work/install.log"; fail "engine build/install"; }

echo "   installed libraries:"
find "$prefix" -name 'libneograph_*' -o -name 'neograph_*.lib' 2>/dev/null \
    | sed 's|^|     |' || true
echo "   package config:"
find "$prefix" -name 'NeoGraphConfig.cmake' 2>/dev/null | sed 's|^|     |' || true

if [[ "$component_mode" == "core" ]]; then
    step "disabled Program component discovery"
    cmake -S "$repo_root/tests/integration/find_package_program" \
        -B "$work/program-disabled-consumer" \
        -DCMAKE_PREFIX_PATH="$prefix" \
        -DNEOGRAPH_EXPECT_PROGRAM_COMPONENT=OFF \
        > "$work/program-disabled-configure.log" 2>&1 \
        || { tail -20 "$work/program-disabled-configure.log"; fail "disabled Program component discovery"; }
    echo "   Program component correctly unavailable"
fi

if [[ "$shared" == "ON" ]]; then
    step "shared-library loader metadata"
    check_shared_metadata
fi

step "3/4 configure consumer against the prefix"
cmake -S "$consumer_project" -B "$work/consumer" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DNEOGRAPH_EXPECTED_ABI_SOVERSION="$major" \
    > "$work/consumer-configure.log" 2>&1 \
    || { tail -20 "$work/consumer-configure.log"; fail "find_package(NeoGraph) — the consumer cannot see the package"; }

step "4/4 build + run consumer"
cmake --build "$work/consumer" --config Release -j"$jobs" \
    > "$work/consumer-build.log" 2>&1 \
    || { tail -30 "$work/consumer-build.log"; fail "consumer build — headers or link"; }

out=$(run_consumer "$prefix") || fail "consumer run"
echo "   consumer says: $out"

if [[ "$shared" == "ON" ]]; then
    relocated="$work/relocated-prefix"
    mv "$prefix" "$relocated" || fail "relocate install prefix"
    out=$(run_consumer "$relocated") || fail "consumer run after relocating install prefix"
    echo "   relocated consumer says: $out"
fi

echo
echo "── OK: an installed NeoGraph is consumable via find_package"
