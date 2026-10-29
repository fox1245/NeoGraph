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

It also refuses a direct std::chrono::steady_clock::now(), system_clock::now() or high_resolution_clock::now() (or a
read through a clock type alias) in src/program, include/neograph/program, src/core/host_timer.h,
src/core/host_loop.cpp, src/core/host_admission.cpp, src/core/hook_runtime.cpp, src/core/hook_outbox.cpp and
src/core/tool_dispatch.cpp (#671): that code reads time through src/core/runtime_clock.h, so a test's simulated clock
drives it. Exceptions go in scripts/runtime_clock_allowlist.txt (or --clock-allowlist), with the same format and rules.
Each allowlist excuses only its own kind of finding.

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
from typing import AbstractSet, Dict, List, NamedTuple, Optional, Sequence, Set, Tuple

ROOT = Path(__file__).resolve().parents[1]
HELPER = "src/core/host_timer.h"
CLOCK_HELPER = "src/core/runtime_clock.h"
CLOCK_ALLOWLIST = "runtime_clock_allowlist.txt"
SCAN_DIRS = ("src", "include")
CLOCK_DIRS = ("src/program", "include/neograph/program")
CLOCK_FILES = (
    "src/core/host_timer.h",
    "src/core/host_loop.cpp",
    "src/core/host_admission.cpp",
    "src/core/hook_runtime.cpp",
    "src/core/hook_outbox.cpp",
    "src/core/tool_dispatch.cpp",
)
HEADER_SUFFIXES = frozenset((".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"))
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
CLOCK_NAMES = frozenset(("steady_clock", "system_clock", "high_resolution_clock"))
RAW_CLOCK = re.compile(r"\b(?:steady_clock|system_clock|high_resolution_clock)\s*::\s*now\b")
_USING_ALIAS = re.compile(
    r"\busing\s+(?P<alias>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?:typename\s+)?"
    r"(?P<target>(?:::\s*)?[A-Za-z_][A-Za-z0-9_]*(?:\s*::\s*[A-Za-z_][A-Za-z0-9_]*)*)\s*;"
)
_TYPEDEF_ALIAS = re.compile(
    r"\btypedef\s+(?:typename\s+)?"
    r"(?P<target>(?:::\s*)?[A-Za-z_][A-Za-z0-9_]*(?:\s*::\s*[A-Za-z_][A-Za-z0-9_]*)*)\s+"
    r"(?P<alias>[A-Za-z_][A-Za-z0-9_]*)\s*;"
)
PATTERNS: Tuple[Tuple[re.Pattern, str], ...] = (
    (RAW_TIMER, "a raw Asio timer"),
    (RAW_EXPIRY, "a raw timer expiry call"),
    (RAW_CANCEL, "a raw cancel_after/cancel_at, which arms a timer of its own"),
)
CLOCK_PATTERNS: Tuple[Tuple[re.Pattern, str], ...] = (
    (RAW_CLOCK, "a direct clock read"),
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


def clock_aliases_in_code(code: str, inherited: AbstractSet[str] = frozenset()) -> Set[str]:
    """Return clock type alias names active in blanked `code`, propagating `inherited` unless rebound locally."""
    alias_decls: List[Tuple[str, str, int]] = []
    for pattern in (_USING_ALIAS, _TYPEDEF_ALIAS):
        for match in pattern.finditer(code):
            alias = match.group("alias")
            target = match.group("target")
            parts = [part.strip() for part in target.split("::") if part.strip()]
            if parts:
                alias_decls.append((alias, parts[-1], match.start()))
    alias_decls.sort(key=lambda item: item[2])
    local_names = {alias for alias, _, _ in alias_decls}
    clock_aliases: Set[str] = {alias for alias in inherited if alias not in local_names}
    changed = True
    while changed:
        changed = False
        for alias, target_leaf, _ in alias_decls:
            if alias not in clock_aliases and (target_leaf in CLOCK_NAMES or target_leaf in clock_aliases):
                clock_aliases.add(alias)
                changed = True
    return clock_aliases


def scan_clock_text(text: str, inherited_aliases: AbstractSet[str] = frozenset()) -> List[Tuple[int, str]]:
    """Return (line, message) for each direct or aliased standard clock ::now call in the source text."""
    code = blank_literals(text)
    starts = [0] + [match.end() for match in re.finditer(r"\n", code)]
    found: List[Tuple[int, str]] = []
    for pattern, what in CLOCK_PATTERNS:
        for match in pattern.finditer(code):
            line = bisect.bisect_right(starts, match.start())
            found.append((line, "%s: '%s'" % (what, match.group(0).rstrip(" \t(").strip())))

    for alias in sorted(clock_aliases_in_code(code, inherited_aliases)):
        if alias in CLOCK_NAMES:
            continue
        alias_now = re.compile(r"\b%s\s*::\s*now\b" % re.escape(alias))
        for match in alias_now.finditer(code):
            line = bisect.bisect_right(starts, match.start())
            found.append((line, "a direct clock read: '%s'" % match.group(0).rstrip(" \t(").strip()))
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


def excuse(
    rel: str, found: List[Tuple[int, str]], entries: List[AllowEntry], used: Dict[int, int], advice: str
) -> List[Finding]:
    """Findings for `found` in `rel`, unless an entry of `entries` excuses the file (it is then counted as used)."""
    excused = [index for index, entry in enumerate(entries) if fnmatch.fnmatchcase(rel, entry.glob)]
    for index in excused:
        used[index] = used.get(index, 0) + 1
    if excused:
        return []
    return [Finding(rel, line, "%s. %s" % (message, advice)) for line, message in found]


def unused(path: Path, entries: List[AllowEntry], used: Dict[int, int], what: str) -> List[Finding]:
    return [
        Finding(path.name, entry.line, "the entry '%s' excuses no %s; remove it" % (entry.glob, what))
        for index, entry in enumerate(entries)
        if index not in used
    ]


def check(
    root: Path,
    allowlist: Path,
    dirs: Sequence[str] = SCAN_DIRS,
    helper: str = HELPER,
    clock_allowlist: Optional[Path] = None,
) -> List[Finding]:
    # Each kind of finding has its own allowlist, so that an entry excuses only the kind its file is for. The clock
    # allowlist sits next to the timer one unless given; a missing one excuses nothing.
    entries = read_allowlist(allowlist)
    if clock_allowlist is not None:
        clock_path = clock_allowlist
        clock_entries = read_allowlist(clock_path)
    else:
        clock_path = allowlist.with_name(CLOCK_ALLOWLIST)
        clock_entries = read_allowlist(clock_path) if clock_path.exists() else []
    findings: List[Finding] = []
    files = source_files(root, dirs)
    if helper not in files:
        raise UsageError("the helper %s is not among the scanned files" % helper)
    helper_code = blank_literals((root / helper).read_text(encoding="utf-8", errors="replace"))
    if not re.search(r"\bnote_deadline\s*\(", helper_code):
        findings.append(Finding(helper, 1, "the helper no longer calls note_deadline, so no timer tells the HostLoop"))

    timer_advice = (
        "Wait on neograph::async::HostTimer (%s), or, for code that never runs on a host loop, list the file in "
        "scripts/%s with the reason." % (helper, allowlist.name)
    )
    clock_advice = (
        "Read time through neograph::async::RuntimeClock::now() / neograph::async::system_now() (%s), or list the "
        "file in scripts/%s with the reason." % (CLOCK_HELPER, clock_path.name)
    )
    used: Dict[int, int] = {}
    clock_used: Dict[int, int] = {}
    texts: Dict[str, str] = {
        rel: (root / rel).read_text(encoding="utf-8", errors="replace") for rel in files
    }
    header_clock_aliases: Set[str] = set()
    for rel, text in texts.items():
        if Path(rel).suffix in HEADER_SUFFIXES:
            header_clock_aliases.update(clock_aliases_in_code(blank_literals(text)))
    for rel in files:
        text = texts[rel]
        timer_found = [] if rel == helper else scan_text(text)
        clock_found = (
            scan_clock_text(text, header_clock_aliases)
            if rel in CLOCK_FILES or any(rel == d or rel.startswith(d + "/") for d in CLOCK_DIRS)
            else []
        )
        if timer_found:
            findings.extend(excuse(rel, timer_found, entries, used, timer_advice))
        if clock_found:
            findings.extend(excuse(rel, clock_found, clock_entries, clock_used, clock_advice))
    findings.extend(unused(allowlist, entries, used, "raw timer use"))
    findings.extend(unused(clock_path, clock_entries, clock_used, "direct clock read"))
    return findings


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Refuse a raw Asio timer in src/ and include/ (#567).")
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--allowlist", type=Path, default=None, help="allowlist file (default: scripts/host_timer_allowlist.txt)")
    parser.add_argument(
        "--clock-allowlist",
        type=Path,
        default=None,
        help="allowlist of direct clock reads (default: %s next to the timer allowlist)" % CLOCK_ALLOWLIST,
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()
    allowlist = args.allowlist if args.allowlist is not None else root / "scripts" / "host_timer_allowlist.txt"
    try:
        findings = check(root, allowlist, clock_allowlist=args.clock_allowlist)
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
