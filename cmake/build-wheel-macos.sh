#!/usr/bin/env bash
set -euo pipefail

# LLVM 20.1.8 has an arm64 Sonoma bottle and stable C++20 stop-token support.
# Keep its headers and runtime together; the wheel repair checks every copied
# dylib against macOS 14.0 rather than retagging a newer-runtime dependency.
brew install llvm@20 libpq sqlite openssl@3 curl

: "${CXX:?The macOS build must select llvm@20 clang++}"
: "${CXXFLAGS:?The macOS build must select the matching libc++ headers}"
: "${LDFLAGS:?The macOS build must select the matching libc++ runtime}"
if [[ "${MACOSX_DEPLOYMENT_TARGET:-}" != "14.0" ]]; then
    echo "The macOS release deployment target must remain 14.0" >&2
    exit 1
fi

# Probe the actual selected standard library, not just the compiler version.
# Use the same flags as CMake's wheel/native-archive builds, including arm64.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/stop-token.cpp" <<'CPP'
#include <exception>
#include <stop_token>
#include <thread>

#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION < 200000 || _LIBCPP_VERSION >= 210000
#error "The macOS release build requires LLVM 20 libc++ headers"
#endif
#if !defined(__cpp_lib_jthread) || __cpp_lib_jthread < 201911L
#error "The selected libc++ does not provide stable C++20 stop-token support"
#endif

int main() {
    std::stop_source source;
    std::stop_token token = source.get_token();
    int callbacks = 0;
    std::stop_callback callback(token, [&] { ++callbacks; });
    if (!token.stop_possible() || token.stop_requested()) return 1;
    if (!source.request_stop() || !token.stop_requested() || callbacks != 1) return 2;
    if (source.request_stop() || callbacks != 1) return 3;
    std::jthread thread([](std::stop_token worker_token) {
        if (!worker_token.stop_possible()) std::terminate();
    });
    thread.join();
    return 0;
}
CPP
read -r -a cxx_flags <<< "$CXXFLAGS"
read -r -a link_flags <<< "$LDFLAGS"
"$CXX" --version
"$CXX" -std=c++20 -arch arm64 -mmacosx-version-min=14.0 \
    "${cxx_flags[@]}" "$work/stop-token.cpp" "${link_flags[@]}" \
    -o "$work/stop-token"
# A successful new-header probe against Apple's old dylib is not sufficient.
python3 - "$work/stop-token" <<'PY'
import subprocess
import sys

links = subprocess.check_output(["otool", "-L", sys.argv[1]], text=True)
print(links, end="")
runtimes = [line.strip().split()[0] for line in links.splitlines()[1:]
            if "libc++.1.dylib" in line]
if len(runtimes) != 1 or runtimes[0].startswith(("/usr/lib/", "/System/")):
    raise SystemExit("The stop-token probe must link Homebrew LLVM's libc++ runtime")
PY
"$work/stop-token"
echo "LLVM 20 libc++ stop-token compile, link and execution probe passed (arm64, macOS 14.0 target)"
