#!/usr/bin/env python3
"""Run one example offline and check its output against the annotations in its source.

An example declares its smoke test in the leading comment block of its own source
file (`//` for C++) or its module docstring (Python). One annotation per line,
optionally behind `//` or `#`:

    smoke-expect:  text that must appear in stdout+stderr (literal, repeatable)
    smoke-expect-line: a whole output line (compared after stripping) that must appear
    smoke-forbid:  text that must not appear (literal, repeatable)
    smoke-args:    command-line arguments (split like a shell); tokens @SRC@ (repository
                   root), @BIN@ (build directory), @PYTHON@ (interpreter), @TMP@ (scratch dir),
                   @PORT@ / @PORT2@ ... @PORT8@ (distinct free TCP ports on 127.0.0.1, chosen
                   for this run; the same token has the same value everywhere in one run;
                   runs that use a token take a machine-wide lock from choosing the ports
                   until the companions listen, so parallel runners never get the same one)
    smoke-stdin:   one line written to stdin, newline added (repeatable); stdin is then closed
    smoke-env:     NAME=VALUE added to the environment (repeatable)
    smoke-companion: a helper process started first (repeatable), for a client that needs a
                   server. Tokens as in smoke-args, plus @TARGET:<cmake target>@ for a built
                   example binary. It is stopped when the run ends.
    smoke-wait-port: TCP port on 127.0.0.1 to wait for (up to 20 s) after starting companions;
                   a number or a @PORT@ token, repeatable (each port is awaited)
    smoke-exit:    expected exit status (default 0)
    smoke-timeout: seconds before the run is killed (default 60)
    smoke-requires: Python only. Comma-separated modules; the test is skipped when one is missing
    smoke-os:      comma-separated subset of linux,darwin,windows; the test is skipped elsewhere
    smoke-slow:    reason this run is CPU-heavy; it is skipped when a sanitizer's *_OPTIONS
                   variable is set (the nightly sanitizer lanes), where it would not fit 60 s
    smoke-skip:    reason this example is not run offline (needs a key, network, a service,
                   a display ...). Mutually exclusive with smoke-expect.

Every offline example must have at least one smoke-expect that names a real result
(a final state value, an event, a count), not a banner. The run starts with no API
keys and no live-mode switches in the environment, in an empty scratch directory.

Command line (C++): examples/run_example_smoke.py --source FILE --exe BINARY [--build-dir DIR]
Command line (Python): examples/run_example_smoke.py --source FILE.py [--python EXE]
Exit status 0 means every expectation held; 77 means skipped (ctest SKIP_RETURN_CODE).
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]   # the source tree: examples/ lives directly under it
HEADER_LINES = 80
SKIP_STATUS = 77
KEYS = ("expect", "expect-line", "forbid", "args", "stdin", "env", "companion", "wait-port", "exit",
        "timeout", "requires", "os", "slow", "skip")
ANNOTATION = re.compile(r"^smoke-([a-z-]+):(.*)$")
SANITIZER_VARIABLES = ("ASAN_OPTIONS", "TSAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "MSAN_OPTIONS")
CREDENTIAL_VARIABLES = (
    "OPENROUTER_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GEMINI_API_KEY",
    "GOOGLE_API_KEY", "OPENAI_API_BASE", "NG_PROVIDER_DESCRIPTOR", "NG_EXAMPLE_LIVE",
)


@dataclass
class Annotations:
    expect: list[str] = field(default_factory=list)
    expect_line: list[str] = field(default_factory=list)
    forbid: list[str] = field(default_factory=list)
    args: str = ""
    stdin: list[str] = field(default_factory=list)
    env: list[str] = field(default_factory=list)
    companion: list[str] = field(default_factory=list)
    wait_port: list[str] = field(default_factory=list)
    exit: int = 0
    timeout: int = 60
    requires: list[str] = field(default_factory=list)
    os: list[str] = field(default_factory=list)
    slow: str = ""
    skip: str = ""
    errors: list[str] = field(default_factory=list)

    @property
    def declared(self) -> bool:
        return bool(self.expect or self.expect_line or self.skip)


def parse_annotations(path: Path) -> Annotations:
    """Read the smoke-* annotations from the first HEADER_LINES lines of `path`."""
    result = Annotations()
    text = path.read_text(encoding="utf-8", errors="replace")
    for number, raw in enumerate(text.splitlines()[:HEADER_LINES], start=1):
        line = raw.strip()
        for prefix in ("//", "#"):
            if line.startswith(prefix):
                line = line[len(prefix):].lstrip()
                break
        # Python examples keep the annotations inside the module docstring, with no prefix.
        match = ANNOTATION.match(line)
        if not match:
            continue
        key, value = match.group(1), match.group(2).strip()
        if key not in KEYS:
            result.errors.append(f"{path.name}:{number}: unknown annotation 'smoke-{key}'")
            continue
        if key in ("expect", "expect-line", "forbid", "stdin", "env"):
            if not value and key != "stdin":
                result.errors.append(f"{path.name}:{number}: smoke-{key} needs a value")
            getattr(result, key.replace("-", "_")).append(value)
        elif key in ("requires", "os"):
            getattr(result, key).extend(item.strip() for item in value.split(",") if item.strip())
        elif key == "companion":
            result.companion.append(value)
        elif key == "wait-port":
            if re.fullmatch(r"\d+|@PORT[2-8]?@", value):
                result.wait_port.append(value)
            else:
                result.errors.append(f"{path.name}:{number}: smoke-wait-port needs a number or a @PORT@ token")
        elif key in ("exit", "timeout"):
            try:
                setattr(result, key.replace("-", "_"), int(value))
            except ValueError:
                result.errors.append(f"{path.name}:{number}: smoke-{key} needs an integer")
        else:
            setattr(result, key, value)  # slow, skip
    if (result.expect or result.expect_line) and result.skip:
        result.errors.append(f"{path.name}: has both smoke-expect and smoke-skip")
    return result


def run_smoke(source: Path, command: list[str], *, build_dir: Path | None = None,
              python: str | None = None, cwd: Path | None = None) -> int:
    """Run `command` (the program, without annotation arguments) and check `source`'s annotations."""
    notes = parse_annotations(source)
    if notes.errors:
        print("\n".join(notes.errors), file=sys.stderr)
        return 1
    if notes.skip:
        print(f"SKIPPED {source.name}: {notes.skip}")
        return SKIP_STATUS
    if not (notes.expect or notes.expect_line):
        print(f"{source.name}: no smoke-expect or smoke-skip annotation", file=sys.stderr)
        return 1
    system = {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}.get(platform.system(), "")
    if notes.os and system not in notes.os:
        print(f"SKIPPED {source.name}: runs on {', '.join(notes.os)} only")
        return SKIP_STATUS
    if notes.slow and any(name in os.environ for name in SANITIZER_VARIABLES):
        print(f"SKIPPED {source.name}: {notes.slow}")
        return SKIP_STATUS
    missing = [name for name in notes.requires if not _importable(name, python)]
    if missing:
        print(f"SKIPPED {source.name}: missing Python module(s): {', '.join(missing)}")
        return SKIP_STATUS

    scratch = Path(tempfile.mkdtemp(prefix="ng-example-"))
    port_lock = _PortLock()
    try:
        tokens = {"@SRC@": str(ROOT), "@BIN@": str(build_dir or ""),
                  "@PYTHON@": python or sys.executable, "@TMP@": str(scratch)}
        # Only a run that names a @PORT@ token picks ports. Choosing a port, starting the server
        # that binds it and waiting for it happen under one lock shared by every runner on this
        # machine, so two runners (ctest -j) cannot be handed the same number in between.
        annotation_text = [notes.args, *notes.companion, *notes.wait_port, *notes.env, *notes.stdin]
        wanted = [int(match.group(1) or 1) for text in annotation_text
                  for match in re.finditer(r"@PORT([2-8]?)@", text)]
        if wanted:
            port_lock.acquire()
            tokens.update(_free_ports(max(wanted)))

        def substitute(text: str) -> str:
            for token, value in tokens.items():
                text = text.replace(token, value)
            return re.sub(r"@TARGET:([A-Za-z0-9_]+)@",
                          lambda match: str(_find_binary(build_dir, match.group(1))), text)

        argv = [*command, *[substitute(item) for item in shlex.split(notes.args)]]
        env = {key: value for key, value in os.environ.items() if key not in CREDENTIAL_VARIABLES}
        env.update(NEOGRAPH_SUPPRESS_FANOUT_WARNING="1", PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
        for item in notes.env:
            name, _, value = substitute(item).partition("=")
            env[name] = value
        stdin = "".join(substitute(line) + "\n" for line in notes.stdin) if notes.stdin else None
        companions: list[subprocess.Popen] = []
        try:
            for line in notes.companion:
                companions.append(subprocess.Popen(
                    shlex.split(substitute(line), posix=os.name != "nt"), cwd=scratch, env=env,
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT))
            for waited in notes.wait_port:
                port = int(substitute(waited))
                if not _wait_for_port(port, companions):
                    return _fail(source, f"nothing listened on 127.0.0.1:{port} within 20 s",
                                 _stop_companions(companions))
            port_lock.release()
            done = subprocess.run(
                argv, cwd=cwd or scratch, env=env, input=stdin, timeout=notes.timeout,
                stdin=None if stdin is not None else subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                encoding="utf-8", errors="replace")
        except OSError as error:
            _stop_companions(companions)
            return _fail(source, f"could not start a process: {error}", "")
        except subprocess.TimeoutExpired as expired:
            output = expired.stdout.decode("utf-8", "replace") if isinstance(expired.stdout, bytes) \
                else (expired.stdout or "")
            _stop_companions(companions)
            return _fail(source, f"timed out after {notes.timeout} s", output)
        companion_output = _stop_companions(companions)
        output = done.stdout
        if done.returncode != notes.exit:
            return _fail(source, f"exit status {done.returncode}, expected {notes.exit}",
                         output + ("\n--- companion output ---\n" + companion_output if companion_output else ""))
        for needle in notes.expect:
            if needle not in output:
                return _fail(source, f"expected text not found: {needle!r}", output)
        lines = {line.strip() for line in output.splitlines()}
        for wanted in notes.expect_line:
            if wanted not in lines:
                return _fail(source, f"expected output line not found: {wanted!r}", output)
        for needle in notes.forbid:
            if needle in output:
                return _fail(source, f"forbidden text found: {needle!r}", output)
        print(f"PASS {source.name}: {len(notes.expect) + len(notes.expect_line)} expectation(s) held")
        return 0
    finally:
        port_lock.release()
        shutil.rmtree(scratch, ignore_errors=True)


def _find_binary(build_dir: Path | None, name: str) -> Path:
    """A built example by CMake target name: <build>/<name>, <build>/Release/<name>, with .exe on Windows."""
    suffix = ".exe" if os.name == "nt" else ""
    for base in (build_dir or Path("."), (build_dir or Path(".")) / "Release", (build_dir or Path(".")) / "Debug"):
        candidate = base / (name + suffix)
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"built example {name} not found under {build_dir}")


class _PortLock:
    """An exclusive lock on a file in the temp directory, shared by all runners of this user."""

    def __init__(self) -> None:
        self._handle = None

    def acquire(self) -> None:
        self._handle = open(Path(tempfile.gettempdir()) / "neograph-example-ports.lock", "a+")
        try:
            import fcntl
            fcntl.flock(self._handle, fcntl.LOCK_EX)
        except ImportError:  # Windows
            import msvcrt
            while True:
                try:
                    msvcrt.locking(self._handle.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(0.05)

    def release(self) -> None:
        if self._handle is not None:
            self._handle.close()  # closing the file drops the lock
            self._handle = None


def _free_ports(count: int) -> dict[str, str]:
    """Distinct free TCP ports, as the tokens @PORT@ (= @PORT1@), @PORT2@ ... @PORT<count>@."""
    held = []
    try:
        for _ in range(count):
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.bind(("127.0.0.1", 0))
            held.append(probe)
        ports = [str(probe.getsockname()[1]) for probe in held]
    finally:
        for probe in held:
            probe.close()
    tokens = {f"@PORT{index}@": port for index, port in enumerate(ports, start=1) if index > 1}
    tokens["@PORT@"] = ports[0]
    return tokens


def _wait_for_port(port: int, processes: list, seconds: float = 20.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if any(process.poll() is not None for process in processes):
            return False  # a companion exited instead of listening
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def _stop_companions(processes: list) -> str:
    """Stop helper processes and return what they printed (for failure messages)."""
    collected = []
    for process in processes:
        if process.poll() is None:
            process.terminate()
        try:
            out, _ = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            out, _ = process.communicate()
        collected.append((out or b"").decode("utf-8", "replace"))
    return "\n".join(collected)


def _importable(module: str, python: str | None) -> bool:
    probe = subprocess.run([python or sys.executable, "-c", f"import {module}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return probe.returncode == 0


def _fail(source: Path, reason: str, output: str) -> int:
    tail = output[-3000:]
    print(f"FAIL {source.name}: {reason}\n----- output (last {len(tail)} characters) -----\n{tail}",
          file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--source", required=True, type=Path, help="example source file")
    parser.add_argument("--exe", type=Path, help="built C++ example; omit for a Python script")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--cwd", type=Path, help="run here instead of an empty scratch directory")
    args = parser.parse_args()
    source = args.source.resolve()
    command = [str(args.exe.resolve())] if args.exe else [args.python, str(source)]
    build_dir = args.build_dir.resolve() if args.build_dir else None
    try:
        return run_smoke(source, command, build_dir=build_dir, python=args.python, cwd=args.cwd)
    except FileNotFoundError as error:
        print(f"FAIL {source.name}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
