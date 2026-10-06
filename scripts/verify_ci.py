#!/usr/bin/env python3
"""Run the existing CI gates locally; provision dependencies/services separately.

Every invocation requires a fresh --work-dir and an explicit --jobs count (a
number, or "auto" for the machine's CPU count).
CMake's existing options remain authoritative. No dependency installs, cleanup
or host policy changes are performed here. The install gate stages its own
prefix; the wheel gate delegates the existing declared bootstrap to cibuildwheel.
"""
from __future__ import annotations

import argparse
import atexit
import importlib.util
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PROFILES = (
    "native-linux", "native-posix", "native-windows", "asan", "tsan",
    "msvc-asan", "grpc", "benchmark", "quickjs-performance", "fuzz",
    "install", "sdist", "wheel", "runtime-archive",
)
ASAN_EXAMPLES = (
    "example_custom_graph", "example_send_command", "example_intent_routing",
    "example_state_management", "example_all_features", "example_subgraph",
    "example_checkpoint_hitl", "example_classifier_fanout",
    "example_async_concurrent_runs", "example_parallel_fanout", "example_plan_executor",
)
TSAN_EXAMPLES = (
    "example_classifier_fanout", "example_async_concurrent_runs",
    "example_parallel_fanout", "example_send_command", "example_plan_executor",
)


TIMINGS = []
SLOW_COMMAND_SECONDS = 20


def run(command, *, env=None, cwd=ROOT, capture=False, timeout=None):
    command = [str(arg) for arg in command]
    printable = subprocess.list2cmdline(command)
    print("+ " + printable, flush=True)
    started = time.monotonic()
    try:
        return subprocess.run(command, cwd=cwd, env=env, check=True, text=True,
                              stdout=subprocess.PIPE if capture else None,
                              stderr=subprocess.PIPE if capture else None, timeout=timeout)
    finally:
        elapsed = time.monotonic() - started
        TIMINGS.append((printable, elapsed))
        if elapsed >= SLOW_COMMAND_SECONDS:
            print(f"  [{elapsed:.0f}s] {printable[:80]}", flush=True)


def write_timing_summary(profile):
    """List the slow commands in the GitHub step summary so a lane's time is attributable."""
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    slow = [(text, seconds) for text, seconds in TIMINGS if seconds >= SLOW_COMMAND_SECONDS]
    if not path or not slow:
        return
    lines = [f"### verify_ci {profile}: commands over {SLOW_COMMAND_SECONDS}s", "",
             "| seconds | command |", "|---:|---|"]
    lines += [f"| {seconds:.0f} | `{text[:110].replace('|', '/')}` |" for text, seconds in slow]
    try:
        with open(path, "a", encoding="utf-8") as summary:
            summary.write("\n".join(lines) + "\n")
    except OSError:
        pass


def jobs_count(value):
    if value == "auto":
        return os.cpu_count() or 1
    return int(value)


def require_tools(*names):
    for name in names:
        if not shutil.which(name):
            raise RuntimeError(f"Required tool not on PATH: {name}; provision it explicitly")


def require_python(*modules):
    for module in modules:
        if importlib.util.find_spec(module) is None:
            raise RuntimeError(f"Required Python dependency missing from {sys.executable}: {module}")


def require_platform(*systems):
    if platform.system() not in systems:
        raise RuntimeError(f"This profile requires {'/'.join(systems)}, not {platform.system()}")


def require_postgres():
    url = os.environ.get("NEOGRAPH_TEST_POSTGRES_URL")
    if not url:
        raise RuntimeError("NEOGRAPH_TEST_POSTGRES_URL is required; PG tests must not silently skip")
    require_tools("psql")
    env = os.environ.copy()
    env["PGCONNECT_TIMEOUT"] = "10"
    # URI expansion happens for the explicit dbname parameter, before libpq
    # supplies environment defaults; PGDATABASE must not hold a whole URI.
    print("+ psql -X -w --dbname <test-database> -v ON_ERROR_STOP=1 -Atc SELECT 1", flush=True)
    try:
        result = subprocess.run(["psql", "-X", "-w", "--dbname", url, "-v", "ON_ERROR_STOP=1",
                                 "-Atc", "SELECT 1"], env=env, cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=20)
    except subprocess.TimeoutExpired:
        raise RuntimeError("Postgres prerequisite SELECT 1 timed out") from None
    if result.returncode:
        # libpq diagnostics can echo credential fragments from the URL (for
        # example an invalid percent-encoded password token), so none is forwarded.
        raise RuntimeError(f"Postgres prerequisite failed (psql exit {result.returncode}); "
                           "verify the service, database and NEOGRAPH_TEST_POSTGRES_URL privately")
    if result.stdout.strip() != "1":
        raise RuntimeError("Postgres prerequisite SELECT 1 did not return 1")


def definitions(**values):
    return [f"-D{key}={value}" for key, value in values.items()]


def minimal_options():
    # Same explicit optional-component boundary used by the old install gate.
    return definitions(**{f"NEOGRAPH_BUILD_{name}": "OFF" for name in (
        "ASYNC", "LLM", "MCP", "MCP_HTTP_SERVER", "A2A", "ACP", "UTIL",
        "POSTGRES", "SQLITE", "GRPC", "PYBIND", "HARNESS_MCP_BINARY", "PROGRAM",
    )}, NEOGRAPH_USE_LIBCURL="OFF")


def toolchain_options():
    return definitions(**{name: os.environ[name] for name in
                          ("CMAKE_TOOLCHAIN_FILE", "VCPKG_INSTALLED_DIR") if os.environ.get(name)})


def configure(build, options, *, generator=None, source=ROOT):
    args = ["cmake", "-S", source, "-B", build]
    if generator:
        args += ["-G", generator]
    # Pin both discovery spellings: old pybind11 and FindPython3 consumers.
    args += definitions(Python_EXECUTABLE=sys.executable, Python3_EXECUTABLE=sys.executable,
                        PYTHON_EXECUTABLE=sys.executable)
    run(args + toolchain_options() + list(options))


def build_targets(build, jobs, *targets, prefix=()):
    # gtest_discover_tests executes freshly linked test binaries during the build, so a
    # sanitizer that needs a fixed address layout must wrap the build, not only ctest.
    args = [*prefix, "cmake", "--build", build, "--config", "Release", "--parallel", jobs]
    if targets:
        args += ["--target", *targets]
    run(args)


def ctest(build, jobs, *options, env=None, prefix=()):
    run([*prefix, "ctest", "--test-dir", build, "-C", "Release", "--no-tests=error",
         "--output-on-failure", "--parallel", jobs, *options], env=env)


def binary(build, name, subdir=""):
    base = build / subdir
    suffix = ".exe" if platform.system() == "Windows" else ""
    for candidate in (base / (name + suffix), base / "Release" / (name + suffix)):
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"Required executable missing: {base / (name + suffix)}")


def python_env(build):
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join((str(build), str(ROOT / "bindings/python")))
    return env


def native(args):
    system = platform.system()
    if args.profile == "native-linux":
        require_platform("Linux")
        require_python("pytest", "pydantic", "certifi", "a2a", "acp", "uvicorn", "httpx")
        require_postgres()
    elif args.profile == "native-posix":
        require_platform("Linux", "Darwin")
    else:
        require_platform("Windows")
    options = definitions(CMAKE_BUILD_TYPE="Release", NEOGRAPH_BUILD_LLM="ON",
                          NEOGRAPH_BUILD_MCP="ON", NEOGRAPH_BUILD_UTIL="ON",
                          NEOGRAPH_BUILD_POSTGRES="OFF" if system == "Windows" else "ON",
                          NEOGRAPH_BUILD_PROGRAM="ON", NEOGRAPH_BUILD_QUICKJS_CONTROL="ON",
                          NEOGRAPH_BUILD_TESTS="ON", NEOGRAPH_BUILD_EXAMPLES="OFF")
    if args.profile == "native-linux":
        options += definitions(NEOGRAPH_BUILD_PYBIND="ON")
    if system == "Windows":
        options += ["-A", "x64", *definitions(NEOGRAPH_BUILD_SQLITE="OFF", NEOGRAPH_USE_LIBCURL="OFF")]
    elif system == "Linux" and args.profile == "native-posix":
        options += definitions(NEOGRAPH_BUILD_SQLITE="ON")
    if args.ccache:
        require_tools("ccache")
        options += definitions(CMAKE_C_COMPILER_LAUNCHER="ccache", CMAKE_CXX_COMPILER_LAUNCHER="ccache")
    build = args.work_dir / "build"
    configure(build, options, generator="Visual Studio 17 2022" if system == "Windows" else None)
    build_targets(build, args.jobs)
    env = os.environ.copy()
    if args.profile == "native-posix":
        # This lane intentionally proves PG compilation/linking without a service.
        env.pop("NEOGRAPH_TEST_POSTGRES_URL", None)
    if system == "Windows":
        ctest(build, 1, "-R", "^QuickJS[.]CEmbeddingSmoke$", env=env)
    if args.profile == "native-linux":
        # Python's ACP PG fixture shares the destructive native test database
        # without CTest's resource lock. Run it only after native ctest finishes.
        ctest(build, args.jobs, "-E", "^pybind_smoke$", env=env)
        env = python_env(build)
        run([sys.executable, "-m", "pytest", "-q", "bindings/python/tests/"], env=env)
        env.pop("NEOGRAPH_TEST_POSTGRES_URL", None)
        run([sys.executable, "-m", "pytest", "-q",
             "bindings/python/tests/test_protocol_sdk_e2e.py::test_acp_streaming_and_durable_session_load"], env=env)
    else:
        ctest(build, args.jobs, env=env)


def sanitizer(args):
    require_platform("Linux")
    build = args.work_dir / "build"
    asan = args.profile == "asan"
    options = definitions(CMAKE_BUILD_TYPE="Debug", NEOGRAPH_BUILD_TESTS="ON",
                          NEOGRAPH_BUILD_EXAMPLES="ON", NEOGRAPH_BUILD_BENCHMARKS="OFF",
                          NEOGRAPH_BUILD_POSTGRES="ON" if asan else "OFF")
    env = os.environ.copy()
    if asan:
        require_python("pytest", "pydantic", "certifi")
        require_postgres()
        require_tools("gcc")
        suppression = ROOT / "tests/lsan_suppressions.txt"
        if re.search(r"^\s*leak:.*(?:neograph::|NeoGraph)", suppression.read_text(), re.MULTILINE):
            raise RuntimeError("LSan suppressions must never match NeoGraph symbols")
        flags = "-fsanitize=address,undefined -fno-omit-frame-pointer -O1"
        options += definitions(NEOGRAPH_BUILD_COOKBOOK_JARVIS="ON", NEOGRAPH_JARVIS_FORCE_MOCK="ON",
                               NEOGRAPH_BUILD_PROGRAM="ON", NEOGRAPH_BUILD_QUICKJS_CONTROL="ON",
                               NEOGRAPH_BUILD_PYBIND="ON", CMAKE_C_FLAGS=flags, CMAKE_CXX_FLAGS=flags,
                               CMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined")
        env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=0:strict_init_order=1",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1",
                   LSAN_OPTIONS=f"suppressions={suppression}:exitcode=23")
        prefix = ()
    else:
        require_tools("setarch")
        options += definitions(CMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1 -g",
                               CMAKE_EXE_LINKER_FLAGS="-fsanitize=thread")
        env.pop("NEOGRAPH_TEST_POSTGRES_URL", None)
        env["TSAN_OPTIONS"] = ("halt_on_error=0:second_deadlock_stack=1:exitcode=66:"
                               f"suppressions={ROOT / 'tests/tsan_suppressions.txt'}")
        prefix = ("setarch", platform.machine(), "-R")
    configure(build, options)
    build_targets(build, args.jobs, prefix=prefix)
    # pybind_smoke is run separately below with the interpreter preload boundary.
    ctest(build, 1 if asan else min(args.jobs, 2), "-E",
          "BIG_|valgrind|^pybind_smoke$" if asan else "BIG_|valgrind", env=env, prefix=prefix)
    for example in ASAN_EXAMPLES if asan else TSAN_EXAMPLES:
        run([*prefix, binary(build, example)], env=env, timeout=90)
    if asan:
        preload = []
        for library in ("libasan.so", "libstdc++.so"):
            location = run(["gcc", f"-print-file-name={library}"], capture=True).stdout.strip()
            if not Path(location).is_file():
                raise RuntimeError(f"Compiler did not resolve required preload library: {library}")
            preload.append(location)
        env["LD_PRELOAD"] = ":".join(preload)
        env["PYTHONPATH"] = python_env(build)["PYTHONPATH"]
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:abort_on_error=0:print_suppressions=0"
        run([sys.executable, "-m", "pytest", "bindings/python/tests/", "-q"], env=env)


def msvc_asan(args):
    require_platform("Windows")
    require_tools("cl", "ninja")
    compiler = subprocess.run(["cl"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    version = re.search(r"\b(19\.\d+\.\d+)\b", compiler.stdout)
    if version is None or tuple(map(int, version[1].split("."))) < (19, 50, 0):
        raise RuntimeError("MSVC ASan requires VS 2026 compatible MSVC >=19.50; 19.44 coroutine ASan is unsafe")
    options = minimal_options() + definitions(CMAKE_BUILD_TYPE="Release", NEOGRAPH_BUILD_MCP_CLIENT="OFF",
        NEOGRAPH_BUILD_MCP_SERVER="OFF", NEOGRAPH_BUILD_PROGRAM="ON", NEOGRAPH_BUILD_QUICKJS_CONTROL="ON",
        NEOGRAPH_BUILD_TESTS="ON", NEOGRAPH_BUILD_EXAMPLES="OFF", NEOGRAPH_BUILD_BENCHMARKS="OFF",
        CMAKE_C_FLAGS="/fsanitize=address /Zi", CMAKE_CXX_FLAGS="/fsanitize=address /Zi",
        CMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO")
    build = args.work_dir / "build"
    configure(build, options, generator="Ninja")
    build_targets(build, args.jobs, "neograph_program_tests", "neograph_quickjs_tests", "neograph_quickjs_c_smoke")
    ctest(build, 1, "-R", "^(QuickJS[.]|QuickJsRuntimeTest[.]|ProgramCompilerTest[.]JavaScript|"
          "ProgramRuntimeTest[.]JavaScript|ProgramCatalogTest[.]JavaScript)")


def quickjs_performance(args):
    require_platform("Linux")
    require_tools("git")
    checkout = run(["git", "rev-parse", "--show-toplevel"], capture=True).stdout.strip()
    if Path(checkout).resolve() != ROOT:
        raise RuntimeError("QuickJS's immutable provenance gate requires this source root to be a Git checkout")
    # The old matrix's two fresh configurations and its immutable runner/thresholds.
    options = minimal_options() + definitions(CMAKE_BUILD_TYPE="Release", NEOGRAPH_BUILD_TESTS="OFF",
        NEOGRAPH_BUILD_BENCHMARKS="ON", NEOGRAPH_BUILD_PROGRAM="ON", NEOGRAPH_BUILD_EXAMPLES="OFF",
        NEOGRAPH_BUILD_MCP_CLIENT="OFF", NEOGRAPH_BUILD_MCP_SERVER="OFF")
    disabled = args.work_dir / "quickjs-disabled"
    enabled = args.work_dir / "quickjs-enabled"
    configure(disabled, options + definitions(NEOGRAPH_BUILD_QUICKJS_CONTROL="OFF"))
    build_targets(disabled, args.jobs, "bench_core_quickjs_probe")
    configure(enabled, options + definitions(NEOGRAPH_BUILD_QUICKJS_CONTROL="ON"))
    build_targets(enabled, args.jobs, "bench_core_quickjs_probe", "bench_quickjs_primitives", "bench_quickjs_control")
    run([sys.executable, ROOT / "scripts/run_quickjs_performance.py",
         "--primitive-binary", binary(enabled, "bench_quickjs_primitives"),
         "--control-binary", binary(enabled, "bench_quickjs_control"),
         "--core-enabled-binary", binary(enabled, "bench_core_quickjs_probe"),
         "--core-disabled-binary", binary(disabled, "bench_core_quickjs_probe"),
         "--output", args.work_dir / "quickjs-performance.json"])


def simple_native(args):
    build = args.work_dir / "build"
    if args.profile == "grpc":
        options = definitions(NEOGRAPH_BUILD_TESTS="ON", NEOGRAPH_BUILD_GRPC="ON", NEOGRAPH_BUILD_A2A="OFF",
            NEOGRAPH_BUILD_ACP="OFF", NEOGRAPH_BUILD_POSTGRES="OFF", NEOGRAPH_BUILD_SQLITE="OFF",
            NEOGRAPH_USE_LIBCURL="OFF", NEOGRAPH_BUILD_EXAMPLES="OFF")
        configure(build, options)
        build_targets(build, args.jobs, "neograph_grpc_graph_contract_tests")
        ctest(build, args.jobs, "-R", "^neograph.grpc_graph_contract$")
    elif args.profile == "benchmark":
        require_platform("Linux")
        configure(build, definitions(CMAKE_BUILD_TYPE="Release", NEOGRAPH_BUILD_BENCHMARKS="ON",
                                     NEOGRAPH_BUILD_POSTGRES="OFF", NEOGRAPH_BUILD_TESTS="OFF"))
        build_targets(build, args.jobs, "bench_async_http", "bench_async_fanout", "bench_neograph")
        run([sys.executable, ROOT / "scripts/check_ci_benchmarks.py", build])
    elif args.profile == "fuzz":
        require_platform("Linux")
        require_tools("clang", "clang++")
        configure(build, definitions(CMAKE_BUILD_TYPE="Debug", CMAKE_C_COMPILER="clang", CMAKE_CXX_COMPILER="clang++",
            NEOGRAPH_BUILD_TESTS="ON", NEOGRAPH_BUILD_FUZZ="ON", NEOGRAPH_BUILD_EXAMPLES="OFF",
            NEOGRAPH_BUILD_BENCHMARKS="OFF", NEOGRAPH_BUILD_POSTGRES="OFF"))
        build_targets(build, args.jobs, "fuzz_graph_compile")
        # libFuzzer writes new corpus members: never mutate the source corpus.
        corpus = args.work_dir / "corpus"
        shutil.copytree(ROOT / "tests/fuzz/corpus/graph_compile", corpus)
        run([binary(build, "fuzz_graph_compile", "tests/fuzz"), "-max_total_time=60", "-timeout=10", "-seed=42", corpus],
            cwd=args.work_dir)
    else:
        require_tools("ninja")
        configure(build, minimal_options() + definitions(CMAKE_BUILD_TYPE="Release", BUILD_SHARED_LIBS="ON",
            NEOGRAPH_BUILD_TESTS="OFF", NEOGRAPH_BUILD_EXAMPLES="OFF", SP_BUILD_TESTS="OFF",
            SP_BUILD_CANARY="OFF", SP_BUILD_RUNTIME_TESTS="ON"), generator="Ninja")
        build_targets(build, args.jobs, "sp_native_archive_portability_tests")
        ctest(build / "_deps/neograph_schemaprovider-build", args.jobs, "-R", "^native_archive_portability$")


def packaging(args):
    if args.profile == "sdist":
        require_python("build", "twine", "scikit_build_core", "pybind11", "ninja")
        tag = args.release_tag
        if tag:
            version = re.search(r'^version\s*=\s*"([^"\n]+)"', (ROOT / "pyproject.toml").read_text(), re.MULTILINE)
            if version is None or tag != "v" + version[1]:
                raise RuntimeError("Release tag does not match pyproject.toml project.version")
        destination = args.work_dir / "dist"
        run([sys.executable, "-m", "build", "--sdist", "--no-isolation", "--outdir", destination, ROOT])
        archives = list(destination.glob("*.tar.gz"))
        if len(archives) != 1:
            raise RuntimeError("Expected exactly one source distribution")
        run([sys.executable, "-m", "twine", "check", *archives])
    else:
        require_python("cibuildwheel")
        if not args.arch:
            raise RuntimeError("wheel requires --arch (x86_64/aarch64/arm64/AMD64)")
        env = os.environ.copy()
        env["CIBW_ARCHS"] = args.arch
        if args.python:
            env["CIBW_BUILD"] = f"{args.python}-*"
        env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(args.jobs)
        if platform.system() == "Linux":
            passed = env.get("CIBW_ENVIRONMENT_PASS_LINUX", "").split()
            env["CIBW_ENVIRONMENT_PASS_LINUX"] = " ".join(dict.fromkeys([*passed, "CMAKE_BUILD_PARALLEL_LEVEL"]))
        elif platform.system() == "Darwin":
            # Preserve the existing repair tool and floors without depending on
            # GitHub's source-dist/source directory layout when invoked locally.
            env["CIBW_REPAIR_WHEEL_COMMAND_MACOS"] = (
                f'python "{ROOT / "cmake/repair-macos-wheel.py"}" --require-archs {{delocate_archs}} '
                '--require-target-macos-version 14.0 -w "{dest_dir}" "{wheel}"')
        elif platform.system() == "Windows":
            require_tools("cl", "vcpkg")
            vcpkg_root = Path(env.get("VCPKG_INSTALLATION_ROOT") or env.get("VCPKG_ROOT") or
                              Path(shutil.which("vcpkg")).parent).resolve()
            toolchain = vcpkg_root / "scripts/buildsystems/vcpkg.cmake"
            if not toolchain.is_file():
                raise RuntimeError(f"Required vcpkg toolchain missing: {toolchain}")
            installed = args.work_dir / "vcpkg-installed"
            downloads = args.work_dir / "vcpkg-downloads"
            downloads.mkdir()
            env["VCPKG_INSTALLATION_ROOT"] = vcpkg_root.as_posix()
            env["VCPKG_INSTALLED_DIR"] = installed.as_posix()
            env["NEOGRAPH_WHEEL_VCPKG_MANIFEST_DIR"] = (ROOT / "cmake/windows-wheel").as_posix()
            env["VCPKG_DOWNLOADS"] = downloads.as_posix()
            # vcpkg's openssl.exe looks for its configuration under a fixed
            # C:\Program Files\Common Files\SSL that does not exist, so `openssl req`
            # (the TLS test fixtures) fails unless the shipped config is named explicitly.
            env["OPENSSL_CONF"] = (installed / "x64-windows/tools/openssl/openssl.cnf").as_posix()
            env["PATH"] = os.pathsep.join((str(installed / "x64-windows/tools/openssl"),
                                           str(installed / "x64-windows/bin"), env["PATH"]))
        run([sys.executable, "-m", "cibuildwheel", ROOT, "--output-dir", args.work_dir / "wheelhouse"], env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=PROFILES)
    parser.add_argument("--work-dir", required=True, type=Path, help="Fresh owned output directory; never cleaned automatically")
    parser.add_argument("--jobs", required=True, type=jobs_count, help="parallel build jobs: a number or auto")
    parser.add_argument("--ccache", action="store_true", help="native-linux: use explicitly provisioned ccache")
    parser.add_argument("--shared", action="store_true", help="install: shared-library mode")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--core-only", action="store_true", help="install: Core-only consumer")
    mode.add_argument("--program", action="store_true", help="install: Program C++ and C ABI consumers")
    parser.add_argument("--quickjs", action="store_true", help="install: include private-symbol and second-engine collision checks")
    parser.add_argument("--release-tag", help="sdist: enforce v<project.version>")
    parser.add_argument("--arch", choices=("x86_64", "aarch64", "arm64", "AMD64"), help="wheel: exact cibuildwheel matrix architecture")
    parser.add_argument("--python", help="wheel: build only this CPython tag, for example cp312 (default: every tag in pyproject.toml)")
    args = parser.parse_args()
    atexit.register(write_timing_summary, args.profile)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.quickjs and not args.program:
        parser.error("--quickjs requires --program")
    if any((args.shared, args.core_only, args.program, args.quickjs)) and args.profile != "install":
        parser.error("consumer switches apply only to install")
    if args.ccache and args.profile != "native-linux":
        parser.error("--ccache applies only to native-linux")
    if args.arch and args.profile != "wheel" or args.release_tag and args.profile != "sdist":
        parser.error("--arch applies to wheel; --release-tag applies to sdist")
    if args.python and (args.profile != "wheel" or not re.fullmatch(r"cp3\d{1,2}", args.python)):
        parser.error("--python applies to wheel and must look like cp312")
    args.work_dir = args.work_dir.resolve()
    if args.work_dir == ROOT or ROOT.is_relative_to(args.work_dir):
        parser.error("--work-dir must not be the source root or its ancestor")
    args.work_dir.mkdir(parents=True, exist_ok=False)
    if args.profile not in ("sdist", "wheel"):
        require_tools("cmake", "ctest")
    if args.profile.startswith("native-"):
        native(args)
    elif args.profile in ("asan", "tsan"):
        sanitizer(args)
    elif args.profile == "msvc-asan":
        msvc_asan(args)
    elif args.profile == "quickjs-performance":
        quickjs_performance(args)
    elif args.profile == "install":
        from check_install_consumer import check_install
        check_install(args)
    elif args.profile in ("sdist", "wheel"):
        packaging(args)
    else:
        simple_native(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"CI gate failed: {error}", file=sys.stderr)
        raise SystemExit(1)
