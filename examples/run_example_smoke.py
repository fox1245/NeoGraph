#!/usr/bin/env python3
"""Run one example offline and check its output against the annotations in its source.

An example declares its smoke test in the leading comment block of its own source
file (`//` for C++) or its module docstring (Python). One annotation per line,
optionally behind `//` or `#`:

    smoke-expect:  text that must appear in stdout+stderr (literal, repeatable)
    smoke-expect-line: a whole output line (compared after stripping) that must appear
    smoke-forbid:  text that must not appear (literal, repeatable)
    smoke-args:    command-line arguments (split like a shell); tokens @SRC@ (repository
                   root), @BIN@ (build directory), @PYTHON@ (interpreter), @TMP@ (scratch dir)
    smoke-stdin:   one line written to stdin, newline added (repeatable); stdin is then closed
    smoke-env:     NAME=VALUE added to the environment (repeatable)
    smoke-exit:    expected exit status (default 0)
    smoke-timeout: seconds before the run is killed (default 60)
    smoke-requires: Python only. Comma-separated modules; the test is skipped when one is missing
    smoke-os:      comma-separated subset of linux,darwin,windows; the test is skipped elsewhere
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
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]   # the source tree: examples/ lives directly under it
HEADER_LINES = 80
SKIP_STATUS = 77
KEYS = ("expect", "expect-line", "forbid", "args", "stdin", "env", "exit", "timeout", "requires", "os", "skip")
ANNOTATION = re.compile(r"^smoke-([a-z-]+):(.*)$")
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
    exit: int = 0
    timeout: int = 60
    requires: list[str] = field(default_factory=list)
    os: list[str] = field(default_factory=list)
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
        elif key in ("exit", "timeout"):
            try:
                setattr(result, key, int(value))
            except ValueError:
                result.errors.append(f"{path.name}:{number}: smoke-{key} needs an integer")
        else:
            setattr(result, key, value)
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
    missing = [name for name in notes.requires if not _importable(name, python)]
    if missing:
        print(f"SKIPPED {source.name}: missing Python module(s): {', '.join(missing)}")
        return SKIP_STATUS

    scratch = Path(tempfile.mkdtemp(prefix="ng-example-"))
    try:
        tokens = {"@SRC@": str(ROOT), "@BIN@": str(build_dir or ""),
                  "@PYTHON@": python or sys.executable, "@TMP@": str(scratch)}

        def substitute(text: str) -> str:
            for token, value in tokens.items():
                text = text.replace(token, value)
            return text

        argv = [*command, *[substitute(item) for item in shlex.split(notes.args)]]
        env = {key: value for key, value in os.environ.items() if key not in CREDENTIAL_VARIABLES}
        env.update(NEOGRAPH_SUPPRESS_FANOUT_WARNING="1", PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
        for item in notes.env:
            name, _, value = substitute(item).partition("=")
            env[name] = value
        stdin = "".join(substitute(line) + "\n" for line in notes.stdin) if notes.stdin else None
        try:
            done = subprocess.run(
                argv, cwd=cwd or scratch, env=env, input=stdin, timeout=notes.timeout,
                stdin=None if stdin is not None else subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                encoding="utf-8", errors="replace")
        except OSError as error:
            return _fail(source, f"could not start {argv[0]}: {error}", "")
        except subprocess.TimeoutExpired as expired:
            output = expired.stdout.decode("utf-8", "replace") if isinstance(expired.stdout, bytes) \
                else (expired.stdout or "")
            return _fail(source, f"timed out after {notes.timeout} s", output)
        output = done.stdout
        if done.returncode != notes.exit:
            return _fail(source, f"exit status {done.returncode}, expected {notes.exit}", output)
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
        shutil.rmtree(scratch, ignore_errors=True)


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
    return run_smoke(source, command, build_dir=build_dir, python=args.python, cwd=args.cwd)


if __name__ == "__main__":
    sys.exit(main())
