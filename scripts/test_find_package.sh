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
# Usage: scripts/test_find_package.sh [--keep] [--shared]
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel)
work=$(mktemp -d -t ng-findpkg-XXXXXX)
keep=
shared=OFF
for arg in "$@"; do
    case "$arg" in
        --keep) keep=--keep ;;
        --shared) shared=ON ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

cleanup() { [[ "$keep" == "--keep" ]] || rm -rf "$work"; }
trap cleanup EXIT

prefix="$work/prefix"
echo "── prefix: $prefix"

step() { echo; echo "── $1"; }
fail() { echo "   FAILED: $1"; exit 1; }

step "1/4 configure engine"
cmake -S "$repo_root" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS="$shared" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DNEOGRAPH_BUILD_TESTS=OFF \
    -DNEOGRAPH_BUILD_EXAMPLES=OFF \
    > "$work/configure.log" 2>&1 || { tail -20 "$work/configure.log"; fail "engine configure"; }

step "2/4 build + install engine"
cmake --build "$work/build" -j"$(nproc)" --target install \
    > "$work/install.log" 2>&1 || { tail -20 "$work/install.log"; fail "engine build/install"; }

echo "   installed libraries:"
find "$prefix" -name 'libneograph_*' -o -name 'neograph_*.lib' 2>/dev/null \
    | sed 's|^|     |' || true
echo "   package config:"
find "$prefix" -name 'NeoGraphConfig.cmake' 2>/dev/null | sed 's|^|     |' || true

step "3/4 configure consumer against the prefix"
cmake -S "$repo_root/tests/integration/find_package" -B "$work/consumer" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix" \
    > "$work/consumer-configure.log" 2>&1 \
    || { tail -20 "$work/consumer-configure.log"; fail "find_package(NeoGraph) — the consumer cannot see the package"; }

step "4/4 build + run consumer"
cmake --build "$work/consumer" -j"$(nproc)" \
    > "$work/consumer-build.log" 2>&1 \
    || { tail -30 "$work/consumer-build.log"; fail "consumer build — headers or link"; }

out=$("$work/consumer/consumer") || fail "consumer run"
echo "   consumer says: $out"

echo
echo "── OK: an installed NeoGraph is consumable via find_package"
