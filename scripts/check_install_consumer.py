"""Installed-prefix/ABI checks shared by the install CI profile.

This is the cross-platform replacement for the former Bash/Git consumer. It
retains every component, symbol, loader and relocation check; all artifacts
remain under the caller's fresh work directory for failure diagnosis.
"""
from __future__ import annotations

import os
from pathlib import Path
import platform
import re

from verify_ci import ROOT, binary, build_targets, configure, definitions, minimal_options, require_tools, run, toolchain_options


QUICKJS_SYMBOL = re.compile(
    r"^(neograph_qjs_|JS_|__JS_|js_(free|malloc|realloc|strdup|strndup|string_codePointRange|atod|dtoa)|"
    r"lre_|__dbuf_|dbuf_|cr_|unicode_|i32toa$|i64toa|u32toa$|u64toa|has_suffix$|pstrcat$|pstrcpy$|rqsort$|strstart$)"
)
ENGINE_MEMBER = re.compile(r"(^|[(:])(quickjs|cutils|dtoa|libregexp|libunicode)[.]c[.]o([):]|$)")
MAIN_MEMBER = re.compile(r"(^|[(:])quickjs[.]c[.]o([):]|$)")


def check_shared_metadata(prefix, version, major):
    system = platform.system()
    lib = prefix / "lib"
    if system == "Linux":
        require_tools("readelf")
        libraries = list(lib.glob(f"libneograph_*.so.{version}"))
        if not libraries:
            raise RuntimeError("No full-version ELF libraries installed")
        for real in libraries:
            base = str(real)[:-len(f".so.{version}")]
            compat = Path(base + f".so.{major}")
            if not real.is_file() or not compat.is_symlink() or not Path(base + ".so").is_symlink():
                raise RuntimeError(f"Missing ELF full-version/SOVERSION/linker name: {real}")
            dynamic = run(["readelf", "-d", real], capture=True).stdout
            expected = Path(base).name + f".so.{major}"
            if not re.search(r"\(SONAME\).*\[" + re.escape(expected) + r"\]", dynamic):
                raise RuntimeError(f"ELF SONAME is not {expected}: {real}")
            if not re.search(r"\((?:RUNPATH|RPATH)\).*\[\$ORIGIN\]", dynamic):
                raise RuntimeError(f"ELF sibling RPATH is not $ORIGIN: {real}")
    elif system == "Darwin":
        require_tools("otool")
        libraries = list(lib.glob(f"libneograph_*.{version}.dylib"))
        if not libraries:
            raise RuntimeError("No full-version Mach-O libraries installed")
        for real in libraries:
            base = str(real)[:-len(f".{version}.dylib")]
            compat = Path(base + f".{major}.dylib")
            if not real.is_file() or not compat.is_symlink() or not Path(base + ".dylib").is_symlink():
                raise RuntimeError(f"Missing Mach-O full-version/SOVERSION/linker name: {real}")
            expected = Path(base).name + f".{major}.dylib"
            if expected not in run(["otool", "-D", real], capture=True).stdout:
                raise RuntimeError(f"Mach-O install name is not {expected}: {real}")
            dependencies = run(["otool", "-L", real], capture=True).stdout.splitlines()[1:]
            for dependency in dependencies:
                if "libneograph_" in dependency and "@rpath/libneograph_" not in dependency:
                    raise RuntimeError(f"Mach-O NeoGraph dependency is not @rpath-relative: {real}")
            commands = run(["otool", "-l", real], capture=True).stdout
            if not re.search(r"LC_RPATH\s+cmdsize\s+\d+\s+path @loader_path ", commands):
                raise RuntimeError(f"Mach-O sibling RPATH is not @loader_path: {real}")
    elif system == "Windows":
        if not (prefix / "bin/neograph_core.dll").is_file():
            raise RuntimeError("Windows DLL name must remain unsuffixed")
        for dll in (prefix / "bin").glob("neograph_*.dll"):
            if dll.name.endswith((f".{major}.dll", f".{version}.dll")):
                raise RuntimeError(f"Windows DLL unexpectedly has a version suffix: {dll}")
    else:
        raise RuntimeError(f"Unsupported platform for shared metadata: {system}")


def check_quickjs_symbols(prefix, version, shared):
    require_tools("nm")
    system = platform.system()
    if system not in ("Linux", "Darwin"):
        raise RuntimeError("QuickJS symbol namespace gate currently requires a native ELF/Mach-O toolchain")
    if shared:
        library = prefix / "lib" / (f"libneograph_program.so.{version}" if system == "Linux"
                                    else f"libneograph_program.{version}.dylib")
        flags = ["-D", "--defined-only"] if system == "Linux" else ["-g", "-U"]
    else:
        library = prefix / "lib/libneograph_program.a"
        flags = ["-A", "-g", "--defined-only"] if system == "Linux" else ["-A", "-g", "-U"]
    if not library.is_file():
        raise RuntimeError(f"Missing Program library: {library}")
    output = run(["nm", *flags, library], capture=True).stdout
    leaked = set()
    prefixed = False
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        symbol = fields[-1]
        if system == "Darwin" and symbol.startswith("_"):
            symbol = symbol[1:]
        if shared:
            if QUICKJS_SYMBOL.match(symbol):
                leaked.add(symbol)
        elif ENGINE_MEMBER.search(fields[0]):
            if not symbol.startswith("neograph_qjs_"):
                leaked.add(symbol)
            if MAIN_MEMBER.search(fields[0]) and symbol == "neograph_qjs_JS_NewRuntime":
                prefixed = True
    if leaked:
        raise RuntimeError("Private/unprefixed QuickJS globals: " + ", ".join(sorted(leaked)))
    if not shared and not prefixed:
        raise RuntimeError("Static Program archive does not contain the prefixed pinned QuickJS engine")
    print("QuickJS shared exports hidden" if shared else "Static QuickJS globals confined to neograph_qjs_*")


def run_consumers(consumer_build, prefix, args):
    env = os.environ.copy()
    system = platform.system()
    variable, directory = ("PATH", prefix / "bin") if system == "Windows" else (
        "DYLD_LIBRARY_PATH" if system == "Darwin" else "LD_LIBRARY_PATH", prefix / "lib")
    env[variable] = str(directory) + (os.pathsep + env[variable] if env.get(variable) else "")
    names = ["consumer"]
    if args.program:
        names.append("native_abi_consumer")
    if args.quickjs:
        names.append("dual_quickjs_consumer")
    for name in names:
        run([binary(consumer_build, name)], env=env)


def check_install(args):
    if args.quickjs and platform.system() == "Windows":
        raise RuntimeError("The existing Windows install matrix intentionally disables QuickJS; native-windows covers its C ABI")
    version_match = re.search(r'^version\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
                              (ROOT / "pyproject.toml").read_text(), re.MULTILINE)
    if not version_match:
        raise RuntimeError("Cannot read project version")
    version = version_match[1]
    major = version.split(".")[0]
    prefix = args.work_dir / "prefix"
    build = args.work_dir / "build"
    consumer_build = args.work_dir / "consumer"
    shared = "ON" if args.shared else "OFF"
    options = definitions(CMAKE_BUILD_TYPE="Release", BUILD_SHARED_LIBS=shared,
                          CMAKE_INSTALL_PREFIX=prefix, NEOGRAPH_BUILD_TESTS="OFF",
                          NEOGRAPH_BUILD_EXAMPLES="OFF", NEOGRAPH_INSTALL_HEADERS="ON")
    options += minimal_options() if args.core_only or args.program else definitions(NEOGRAPH_BUILD_PROGRAM="OFF")
    if args.program:
        options += definitions(NEOGRAPH_BUILD_PROGRAM="ON", NEOGRAPH_BUILD_QUICKJS_CONTROL="ON" if args.quickjs else "OFF")
    if platform.system() == "Windows":
        options += definitions(NEOGRAPH_BUILD_POSTGRES="OFF", NEOGRAPH_BUILD_SQLITE="OFF", NEOGRAPH_USE_LIBCURL="OFF")
        if os.environ.get("OPENSSL_ROOT_DIR"):
            options += definitions(OPENSSL_ROOT_DIR=os.environ["OPENSSL_ROOT_DIR"])
    configure(build, options)
    build_targets(build, args.jobs, "install")
    fixture = ROOT / "tests/integration/find_package_program"
    if args.core_only:
        configure(args.work_dir / "program-disabled-consumer",
                  definitions(CMAKE_PREFIX_PATH=prefix, NEOGRAPH_EXPECT_PROGRAM_COMPONENT="OFF"), source=fixture)
        # A rejection is only evidence if it is the package's Program rejection,
        # not an unrelated compiler/dependency/configuration failure.
        command = ["cmake", "-S", fixture, "-B", args.work_dir / "program-disabled-required-consumer",
                   *toolchain_options(), *definitions(CMAKE_PREFIX_PATH=prefix, NEOGRAPH_EXPECT_PROGRAM_COMPONENT="OFF",
                                NEOGRAPH_TEST_REQUIRED_PROGRAM_REJECTION="ON")]
        import subprocess
        result = subprocess.run([str(arg) for arg in command], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end="")
        (args.work_dir / "program-disabled-required-configure.log").write_text(result.stdout)
        if result.returncode == 0 or "NeoGraph_FOUND to FALSE" not in result.stdout:
            raise RuntimeError("Required disabled Program lookup did not fail for the expected reason")
    if args.shared:
        check_shared_metadata(prefix, version, major)
    if args.quickjs:
        check_quickjs_symbols(prefix, version, args.shared)
    consumer = fixture if args.program else ROOT / "tests/integration/find_package"
    options = definitions(CMAKE_BUILD_TYPE="Release", CMAKE_PREFIX_PATH=prefix,
                          NEOGRAPH_EXPECTED_ABI_SOVERSION=major)
    if args.program:
        options += definitions(NEOGRAPH_EXPECT_QUICKJS_CONTROL="ON" if args.quickjs else "OFF")
    configure(consumer_build, options, source=consumer)
    build_targets(consumer_build, args.jobs)
    run_consumers(consumer_build, prefix, args)
    if args.shared:
        relocated = args.work_dir / "relocated-prefix"
        prefix.rename(relocated)
        run_consumers(consumer_build, relocated, args)
    print("OK: installed NeoGraph is consumable via find_package")
