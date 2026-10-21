#!/usr/bin/env python3
"""Refuse a raw Asio timer in the engine's sources (#567).

A HostLoop (include/neograph/async/host_loop.h) pumps the engine's io_context from a WebAssembly host's event loop
and cannot see Asio's timer queue. A timer that the loop was not told about works natively and never fires there, so
a run waits forever. Engine code therefore waits on neograph::async::HostTimer (src/core/host_timer.h), which arms the
timer and tells the loop in one call.

This script scans src/ and include/ for what bypasses it and fails when it finds any:

  * an Asio timer type: steady_timer, system_timer, high_resolution_timer, basic_waitable_timer, deadline_timer
    (a header include of one counts);
  * a call of expires_after, expires_at or expires_from_now;
  * asio::cancel_after and asio::cancel_at, which arm a timer of their own.

Comments, string literals and character literals are not searched. The helper header is the one file that may use
them. Code that never runs on a host loop (components that a WebAssembly host does not build) is listed in
scripts/host_timer_allowlist.txt, one entry per line: "path-glob<TAB>reason". The glob is matched against the
repository-relative path with fnmatch rules ("*" also crosses "/"); the reason is required. An entry that excuses
nothing is an error, so the list cannot go stale. The script also checks that the helper still tells the loop.

Output is one line per finding, "path:line: message". The exit status is 0 when clean, 1 when there are findings and
2 for a usage or allowlist error. Standard library only; Python 3.8+.
"""

from __future__ import annotations

import argparse
import bisect
import fnmatch
import re
import sys
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]
HELPER = "src/core/host_timer.h"
SCAN_DIRS = ("src", "include")
SOURCE_SUFFIXES = frozenset(
    (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".c", ".cc", ".cpp", ".cxx", ".mm")
)

# What bypasses the helper. Each pattern is searched in code with comments and literals blanked out.
RAW_TIMER = re.compile(
    r"\b(?:steady_timer|system_timer|high_resolution_timer|basic_waitable_timer|waitable_timer"
    r"|deadline_timer|basic_deadline_timer)\b"
)
RAW_EXPIRY = re.compile(r"\bexpires_(?:after|at|from_now)\s*\(")
RAW_CANCEL = re.compile(r"\bcancel_(?:after|at)\b")
PATTERNS: Tuple[Tuple[re.Pattern, str], ...] = (
    (RAW_TIMER, "a raw Asio timer"),
    (RAW_EXPIRY, "a raw timer expiry call"),
    (RAW_CANCEL, "a raw cancel_after/cancel_at, which arms a timer of its own"),
)

# Comments, string literals and character literals, so that their text is not searched. The order matters: the
# leftmost token wins, so a "//" inside a string is part of the string and a quote inside a comment is part of it.
_BLANK = re.compile(
    r"""
      //(?:[^\n\\]|\\\r?\n|\\.)*                                           # line comment, with continuations
    | /\*.*?\*/                                                            # block comment
    | (?<![A-Za-z0-9_])(?:u8|u|U|L)?R"(?P<delim>[^ ()\\\t\v\f\n]{0,16})\(.*?\)(?P=delim)"   # raw string
    | "(?:\\.|[^"\\\n])*"                                                  # string
    | (?<![A-Za-z0-9_])'(?:\\.|[^'\\\n])*'                                 # character (not a digit separator)
    """,
    re.VERBOSE | re.DOTALL,
)
_NOT_NEWLINE = re.compile(r"[^\n]")


class Finding(NamedTuple):
    path: str
    line: int
    message: str


class AllowEntry(NamedTuple):
    glob: str
    reason: str
    line: int


class UsageError(Exception):
    pass


def blank_literals(text: str) -> str:
    """Return `text` with comments and literals replaced by spaces, newlines kept, so that line numbers hold."""
    return _BLANK.sub(lambda match: _NOT_NEWLINE.sub(" ", match.group(0)), text)


def scan_text(text: str) -> List[Tuple[int, str]]:
    """Return (line, message) for each raw timer use in the source text."""
    code = blank_literals(text)
    starts = [0] + [match.end() for match in re.finditer(r"\n", code)]
    found: List[Tuple[int, str]] = []
    for pattern, what in PATTERNS:
        for match in pattern.finditer(code):
            line = bisect.bisect_right(starts, match.start())
            found.append((line, "%s: '%s'" % (what, match.group(0).rstrip(" \t(").strip())))
    return sorted(found)


def read_allowlist(path: Path) -> List[AllowEntry]:
    entries: List[AllowEntry] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise UsageError("cannot read the allowlist %s: %s" % (path, error))
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        glob, tab, reason = raw.partition("\t")
        glob, reason = glob.strip(), reason.strip()
        if not tab or not glob or not reason:
            raise UsageError("%s:%d: an entry is 'path-glob<TAB>reason', and the reason is required" % (path, number))
        entries.append(AllowEntry(glob, reason, number))
    return entries


def source_files(root: Path, dirs: Sequence[str]) -> List[str]:
    files: List[str] = []
    for name in dirs:
        base = root / name
        if not base.is_dir():
            raise UsageError("%s is not a directory under %s" % (name, root))
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                files.append(path.relative_to(root).as_posix())
    return sorted(files)


def check(root: Path, allowlist: Path, dirs: Sequence[str] = SCAN_DIRS, helper: str = HELPER) -> List[Finding]:
    entries = read_allowlist(allowlist)
    findings: List[Finding] = []
    files = source_files(root, dirs)
    if helper not in files:
        raise UsageError("the helper %s is not among the scanned files" % helper)
    helper_code = blank_literals((root / helper).read_text(encoding="utf-8", errors="replace"))
    if not re.search(r"\bnote_deadline\s*\(", helper_code):
        findings.append(Finding(helper, 1, "the helper no longer calls note_deadline, so no timer tells the HostLoop"))

    used: Dict[int, int] = {}
    for rel in files:
        if rel == helper:
            continue
        found = scan_text((root / rel).read_text(encoding="utf-8", errors="replace"))
        if not found:
            continue
        excused = [index for index, entry in enumerate(entries) if fnmatch.fnmatchcase(rel, entry.glob)]
        if excused:
            for index in excused:
                used[index] = used.get(index, 0) + 1
            continue
        for line, message in found:
            findings.append(
                Finding(
                    rel,
                    line,
                    "%s. Wait on neograph::async::HostTimer (%s), or, for code that never runs on a host loop, "
                    "list the file in scripts/host_timer_allowlist.txt with the reason." % (message, helper),
                )
            )
    for index, entry in enumerate(entries):
        if index not in used:
            findings.append(
                Finding(
                    allowlist.name,
                    entry.line,
                    "the entry '%s' excuses no raw timer use; remove it" % entry.glob,
                )
            )
    return findings


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Refuse a raw Asio timer in src/ and include/ (#567).")
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--allowlist", type=Path, default=None, help="allowlist file (default: scripts/host_timer_allowlist.txt)")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    allowlist = args.allowlist if args.allowlist is not None else root / "scripts" / "host_timer_allowlist.txt"
    try:
        findings = check(root, allowlist)
    except UsageError as error:
        print("check_host_timers: %s" % error, file=sys.stderr)
        return 2
    for finding in findings:
        print("%s:%d: %s" % finding)
    if findings:
        print("check_host_timers: %d finding(s)" % len(findings), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
