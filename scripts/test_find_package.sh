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
#   exit 0 — a consumer can find_package(NeoGraph), link the exported targets,
#            compile the public Core/A2A/ACP headers, and run
#   exit 1 — it cannot, at whichever of the four stages failed
#
# Shared mode also verifies the platform loader metadata and versioned links.
# Usage: scripts/test_find_package.sh [--keep] [--shared] [--core-only|--program] [--quickjs]
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
quickjs=OFF
platform=$(uname -s)
for arg in "$@"; do
    case "$arg" in
        --keep) keep=--keep ;;
        --shared) shared=ON ;;
        --core-only) component_mode=core ;;
        --program) component_mode=program ;;
        --quickjs) quickjs=ON ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done
if [[ "$quickjs" == "ON" && "$component_mode" != "program" ]]; then
    echo "--quickjs requires --program" >&2
    exit 2
fi

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
    component_cmake_args+=(-DNEOGRAPH_BUILD_QUICKJS_CONTROL="$quickjs")
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
native_abi_consumer_exe="$work/consumer/native_abi_consumer"
dual_quickjs_consumer_exe="$work/consumer/dual_quickjs_consumer"
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
        native_abi_consumer_exe="$work/consumer/Release/native_abi_consumer.exe"
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

check_no_exported_quickjs_symbols() {
    local library symbols leaked macho=0
    case "$platform" in
        Linux)
            library="$prefix/lib/libneograph_program.so.$version"
            [[ -f "$library" ]] || fail "missing shared Program library: $library"
            symbols=$(nm -D --defined-only "$library") \
                || fail "cannot inspect ELF Program exports: $library"
            ;;
        Darwin)
            macho=1
            library="$prefix/lib/libneograph_program.$version.dylib"
            [[ -f "$library" ]] || fail "missing shared Program library: $library"
            symbols=$(nm -g -U "$library") \
                || fail "cannot inspect Mach-O Program exports: $library"
            ;;
        *)
            fail "QuickJS shared-symbol check is unsupported on $platform"
            ;;
    esac

    # ELF spells C symbols directly while Mach-O prefixes them with one
    # underscore. Reject both the upstream names and NeoGraph's private prefix:
    # the latter must remain hidden from a shared library even though it is
    # intentionally global inside a static archive.
    leaked=$(printf '%s\n' "$symbols" |
        awk -v macho="$macho" '{
            symbol = $NF
            if (macho) sub(/^_/, "", symbol)
            if (symbol ~ /^neograph_qjs_/ ||
                symbol ~ /^(JS_|__JS_|js_(free|malloc|realloc|strdup|strndup|string_codePointRange|atod|dtoa)|lre_|__dbuf_|dbuf_|cr_|unicode_|i32toa$|i64toa|u32toa$|u64toa|has_suffix$|pstrcat$|pstrcpy$|rqsort$|strstart$)/) {
                print symbol
            }
        }' | LC_ALL=C sort -u)
    if [[ -n "$leaked" ]]; then
        printf '   exported QuickJS implementation symbols:\n%s\n' "$leaked" >&2
        fail "shared neograph_program exports private QuickJS C symbols"
    fi
    echo "   no private QuickJS implementation symbols are exported"
}

check_static_quickjs_symbol_namespace() {
    local library symbols leaked prefixed macho=0
    library="$prefix/lib/libneograph_program.a"
    [[ -f "$library" ]] || fail "missing static Program archive: $library"
    case "$platform" in
        Linux)
            symbols=$(nm -A -g --defined-only "$library") \
                || fail "cannot inspect static ELF Program archive members: $library"
            ;;
        Darwin)
            macho=1
            symbols=$(nm -A -g -U "$library") \
                || fail "cannot inspect static Mach-O Program archive members: $library"
            ;;
        *)
            fail "QuickJS static-symbol check is unsupported on $platform"
            ;;
    esac
    # Inspect every global defined by the five wrapped engine archive members,
    # never nm -D: static consumers resolve these globals regardless of
    # ELF/Mach-O visibility. Matching members rather than a symbol-name
    # allowlist makes a pinned-source update fail closed even if upstream adds
    # a new support function with an unrelated name.
    leaked=$(printf '%s\n' "$symbols" |
        awk -v macho="$macho" '{
            member = $1
            if (member ~ /(^|[(:])(quickjs|cutils|dtoa|libregexp|libunicode)[.]c[.]o([):]|$)/) {
                symbol = $NF
                if (macho) sub(/^_/, "", symbol)
                if (symbol !~ /^neograph_qjs_/) print symbol
            }
        }' | LC_ALL=C sort -u)
    if [[ -n "$leaked" ]]; then
        printf '   unprefixed static QuickJS implementation symbols:\n%s\n' "$leaked" >&2
        fail "static neograph_program exposes unprefixed QuickJS C symbols"
    fi
    prefixed=$(printf '%s\n' "$symbols" |
        awk -v macho="$macho" '{
            member = $1
            if (member ~ /(^|[(:])quickjs[.]c[.]o([):]|$)/) {
                symbol = $NF
                if (macho) sub(/^_/, "", symbol)
                if (symbol == "neograph_qjs_JS_NewRuntime") print symbol
            }
        }')
    [[ -n "$prefixed" ]] \
        || fail "static neograph_program does not contain the prefixed pinned QuickJS engine"
    echo "   static QuickJS globals are confined to neograph_qjs_*"
}

run_consumer() {
    local library_prefix=$1
    case "$platform" in
        Linux)
            LD_LIBRARY_PATH="$library_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                "$consumer_exe" || return 1
            ;;
        Darwin)
            DYLD_LIBRARY_PATH="$library_prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                "$consumer_exe" || return 1
            ;;
        MINGW*|MSYS*|CYGWIN*)
            PATH="$library_prefix/bin:$PATH" "$consumer_exe" || return 1
            ;;
        *)
            "$consumer_exe" || return 1
            ;;
    esac
    if [[ "$component_mode" == "program" ]]; then
        case "$platform" in
            Linux)
                LD_LIBRARY_PATH="$library_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                    "$native_abi_consumer_exe" || return 1
                ;;
            Darwin)
                DYLD_LIBRARY_PATH="$library_prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                    "$native_abi_consumer_exe" || return 1
                ;;
            MINGW*|MSYS*|CYGWIN*)
                PATH="$library_prefix/bin:$PATH" "$native_abi_consumer_exe" || return 1
                ;;
            *)
                "$native_abi_consumer_exe" || return 1
                ;;
        esac
    fi
    if [[ "$quickjs" == "ON" ]]; then
        case "$platform" in
            Linux)
                LD_LIBRARY_PATH="$library_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                    "$dual_quickjs_consumer_exe" || return 1
                ;;
            Darwin)
                DYLD_LIBRARY_PATH="$library_prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                    "$dual_quickjs_consumer_exe" || return 1
                ;;
            *)
                "$dual_quickjs_consumer_exe" || return 1
                ;;
        esac
    fi
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

    cmake -S "$repo_root/tests/integration/find_package_program" \
        -B "$work/program-disabled-required-consumer" \
        -DCMAKE_PREFIX_PATH="$prefix" \
        -DNEOGRAPH_EXPECT_PROGRAM_COMPONENT=OFF \
        -DNEOGRAPH_TEST_REQUIRED_PROGRAM_REJECTION=ON \
        > "$work/program-disabled-required-configure.log" 2>&1 \
        && fail "required disabled Program component lookup unexpectedly succeeded"
    required_rejection_log=$(<"$work/program-disabled-required-configure.log")
    if [[ "$required_rejection_log" != *"NeoGraph_FOUND to FALSE"* ]]; then
        tail -20 "$work/program-disabled-required-configure.log"
        fail "required Program rejection failed for an unrelated reason"
    fi
    echo "   required Program component lookup correctly rejected"
fi

if [[ "$shared" == "ON" ]]; then
    step "shared-library loader metadata"
    check_shared_metadata
fi
if [[ "$shared" == "ON" && "$quickjs" == "ON" ]]; then
    step "private QuickJS symbols"
    check_no_exported_quickjs_symbols
fi
if [[ "$shared" == "OFF" && "$quickjs" == "ON" ]]; then
    step "static QuickJS symbol namespace"
    check_static_quickjs_symbol_namespace
fi

consumer_cmake_args=(-DNEOGRAPH_EXPECTED_ABI_SOVERSION="$major")
if [[ "$component_mode" == "program" ]]; then
    consumer_cmake_args+=(-DNEOGRAPH_EXPECT_QUICKJS_CONTROL="$quickjs")
fi

step "3/4 configure consumer against the prefix"
cmake -S "$consumer_project" -B "$work/consumer" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix" \
    "${consumer_cmake_args[@]}" \
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
