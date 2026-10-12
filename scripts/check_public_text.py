#!/usr/bin/env python3
"""Keep internal trivia out of the public text of this repository.

NeoGraph is a public repository, so everything tracked in it is read by
strangers: comments, strings, examples, documents. Some wording only makes
sense to the people who worked on an earlier private version, and some details
(a home directory, an address, a token) must never be published at all. This
script finds them mechanically so a review does not have to.

Modes (standard library only; Python 3.8+; Linux, macOS and Windows):

  --tree          scan every tracked file (the whole checkout)
  --diff [BASE]   scan only lines added since BASE (default origin/master):
                  committed, staged, unstaged and untracked additions

Output is one line per finding, "path:line: rule: excerpt", and the exit status
is 1 when anything was found, 0 when clean, 2 for a usage or environment error.
The excerpt of a secret-shaped finding is masked.

Rules (name: what it finds)

  predecessor-name          the name of a predecessor project, spelling variants
  private-repo-link         a link to a private repository, or to any repository
                            under the project's GitHub handle that is not one of
                            the public repositories listed in PUBLIC_REPOS
  internal-process-wording  wording that refers to an internal decision-making
                            process or internal tooling
  home-path                 a personal home directory such as /home/<name>/
  external-ip               a globally routable IPv4 or IPv6 literal
  secret-shaped             a string shaped like an API key, token or private key
  private-project-name      the name of a private project

Not flagged on purpose: e-mail addresses, hardware model names, private-network,
loopback, link-local and documentation addresses, version numbers, and the
project's GitHub handle inside a link to a public repository.

Out of scope: deps/, THIRD_PARTY_LICENSES*, node_modules/, lock files, the
*.schema.json and *.proto copies under bindings/python/tests/interop/*/spec/,
and binary files.

Justified exceptions live in scripts/public_text_allowlist.txt, one per line:
"path-glob<TAB>rule<TAB>reason". The reason is required. A fixture that needs a
fake key or a sample address is allowlisted for that path; the rule itself is
never loosened to let it through.

--scope code limits a scan to files that are not documents (Markdown, docs/,
README*, CHANGELOG*). tests/CMakeLists.txt uses it until the documents are
clean; see NEOGRAPH_PUBLIC_TEXT_GUARD_INCLUDE_DOCS there.
"""

from __future__ import annotations

import argparse
import fnmatch
import ipaddress
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ALLOWLIST = Path(__file__).resolve().with_name("public_text_allowlist.txt")
DEFAULT_BASE = "origin/master"

# Repositories under the project's GitHub handle that readers can open.
PUBLIC_REPOS = frozenset(
    name.lower() for name in ("NeoGraph", "SchemaProvider", "neoclaw", "cppdotenv", "NeoProtocol")
)

MAX_FILE_BYTES = 8 * 1024 * 1024
EXCERPT_WIDTH = 140

KIND_CODE = "code"
KIND_DATA = "data"
KIND_DOC = "doc"
ALL_KINDS = frozenset((KIND_CODE, KIND_DATA, KIND_DOC))

# ---------------------------------------------------------------------------
# File selection
# ---------------------------------------------------------------------------

_SKIP_DIR_PREFIXES = ("deps/", "node_modules/")
_SKIP_ANYWHERE = ("/node_modules/",)
_SKIP_BASENAMES = frozenset(
    (
        "package-lock.json",
        "pnpm-lock.yaml",
        "yarn.lock",
        "Cargo.lock",
        "poetry.lock",
        "uv.lock",
        "Pipfile.lock",
        "composer.lock",
        "Gemfile.lock",
        "conan.lock",
        "pdm.lock",
    )
)
_INTEROP_SPEC = re.compile(r"^bindings/python/tests/interop/[^/]+/spec/")
_BINARY_SUFFIXES = frozenset(
    (
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".bmp", ".mp4", ".mov", ".webm",
        ".mp3", ".wav", ".ogg", ".pdf", ".zip", ".gz", ".tgz", ".bz2", ".xz", ".zst", ".7z",
        ".tar", ".wasm", ".so", ".dll", ".dylib", ".a", ".lib", ".o", ".obj", ".exe", ".pyc",
        ".pyd", ".whl", ".ttf", ".otf", ".woff", ".woff2", ".db", ".sqlite", ".sqlite3",
    )
)
_DATA_SUFFIXES = frozenset(
    (".json", ".jsonl", ".yml", ".yaml", ".toml", ".csv", ".tsv", ".ini", ".cfg", ".conf", ".txt",
     ".example", ".in", ".neograph", ".langgraph", ".env")
)
_DATA_BASENAMES = frozenset((".gitignore", ".gitattributes", ".dockerignore", ".gitmodules"))
_DOC_SUFFIXES = frozenset((".md", ".markdown", ".rst"))


def is_out_of_scope(path: str) -> bool:
    """Third-party and generated files that this repository does not author."""
    if path.startswith(_SKIP_DIR_PREFIXES) or any(part in path for part in _SKIP_ANYWHERE):
        return True
    base = path.rsplit("/", 1)[-1]
    if base.startswith("THIRD_PARTY_LICENSES"):
        return True
    if base in _SKIP_BASENAMES or base.endswith(".lock"):
        return True
    if _INTEROP_SPEC.match(path) and (path.endswith(".schema.json") or path.endswith(".proto")):
        return True
    return Path(base).suffix.lower() in _BINARY_SUFFIXES


def file_kind(path: str) -> str:
    """Return "doc" for documents, "data" for configuration and fixtures, else "code"."""
    base = path.rsplit("/", 1)[-1]
    lower = base.lower()
    if (
        path.startswith("docs/")
        or Path(lower).suffix in _DOC_SUFFIXES
        or lower.startswith("readme")
        or lower.startswith("changelog")
    ):
        return KIND_DOC
    if lower != "cmakelists.txt" and (
        Path(lower).suffix in _DATA_SUFFIXES or lower in _DATA_BASENAMES
    ):
        return KIND_DATA
    return KIND_CODE


# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------

Span = Tuple[int, int]
Finder = Callable[[str], Optional[Span]]
Accept = Callable[["re.Match[str]", str], bool]


@dataclass(frozen=True)
class Rule:
    """A named check. find() returns the span of the first hit in a line, or None."""

    name: str
    summary: str
    kinds: frozenset
    find: Finder
    masked: bool = False


def regex_finder(pattern: "re.Pattern[str]", accept: Optional[Accept] = None) -> Finder:
    def find(line: str) -> Optional[Span]:
        for match in pattern.finditer(line):
            if accept is None or accept(match, line):
                return match.span()
        return None

    return find


def _accept_repo_link(match: "re.Match[str]", _line: str) -> bool:
    name = match.group("repo")
    if name.lower().endswith(".git"):
        name = name[:-4]
    name = name.strip(".-")
    return bool(name) and name.lower() not in PUBLIC_REPOS


_PLACEHOLDER_HOME_NAMES = frozenset(
    (
        "user", "users", "username", "me", "you", "example", "examples", "name", "foo", "bar",
        "baz", "x", "xxx", "all", "default", "public", "shared", "runner", "ubuntu", "vsts",
        "root", "yourname", "your-name", "your_name", "yourusername", "your-username",
        "your_username", "youruser", "myuser", "myname", "someuser", "somebody", "nobody",
    )
)


def _accept_home_path(match: "re.Match[str]", _line: str) -> bool:
    name = match.group("name").lower()
    if name in _PLACEHOLDER_HOME_NAMES:
        return False
    return not re.fullmatch(r"(?:your|my|some|any)[-_]?\w*", name)


_V4 = re.compile(r"(?<![\w.\-])(\d{1,3}(?:\.\d{1,3}){3})(?!\.?\w|-[A-Za-z]|\+)")
_V6 = re.compile(r"(?<![\w:.])([0-9A-Fa-f:]{2,39}(?:%\w+)?)(?![\w:.])")
_VERSION_CONTEXT = re.compile(
    r"(?:\b(?:version|ver|rev|release|build|since|until|tag)\b[\s:=\"'(\[*`]*|==|>=|<=|~=|!=|~>|\^)\s*$",
    re.IGNORECASE,
)
_NOT_ROUTABLE_V4 = tuple(
    ipaddress.ip_network(net)
    for net in (
        "0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8", "169.254.0.0/16",
        "172.16.0.0/12", "192.0.0.0/24", "192.0.2.0/24", "192.88.99.0/24", "192.168.0.0/16",
        "198.18.0.0/15", "198.51.100.0/24", "203.0.113.0/24", "224.0.0.0/4", "240.0.0.0/4",
    )
)
# Global unicast IPv6 is the block whose first three bits are 001; documentation
# prefixes inside it are not routable.
_GLOBAL_V6 = ipaddress.IPv6Network((0x2000 << 112, 3))
_NOT_ROUTABLE_V6 = tuple(ipaddress.ip_network(net) for net in ("2001:db8::/32", "3fff::/20"))


def is_public_address(address: "ipaddress._BaseAddress") -> bool:
    """True for a globally routable unicast address."""
    if address.version == 4:
        return not any(address in net for net in _NOT_ROUTABLE_V4)
    return address in _GLOBAL_V6 and not any(address in net for net in _NOT_ROUTABLE_V6)


def find_public_ip(line: str) -> Optional[Span]:
    for match in _V4.finditer(line):
        if _VERSION_CONTEXT.search(line[: match.start(1)]):
            continue
        try:
            address = ipaddress.IPv4Address(match.group(1))
        except ValueError:
            continue
        if is_public_address(address):
            return match.span(1)
    if line.count(":") >= 2:
        for match in _V6.finditer(line):
            text = match.group(1).split("%", 1)[0]
            # An IPv6 literal has eight groups or a "::"; that keeps clock times
            # and MAC addresses out. Only global unicast space counts, which also keeps
            # C++ scopes made of hex letters (Face::bad) out.
            if not ("::" in text or text.count(":") == 7):
                continue
            try:
                address = ipaddress.IPv6Address(text)
            except ValueError:
                continue
            if is_public_address(address):
                return match.span(1)
    return None


_SECRET = re.compile(
    r"(?<![A-Za-z0-9])sk-(?=[A-Za-z0-9_\-]*[0-9A-Z])[A-Za-z0-9_\-]{20,}"
    r"|(?<![A-Za-z0-9])(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})"
    r"|(?<![A-Za-z0-9])(?:AKIA|ASIA)[0-9A-Z]{16}(?![A-Za-z0-9])"
    r"|(?<![A-Za-z0-9])hf_[A-Za-z0-9]{30,}"
    r"|(?<![A-Za-z0-9])AIza[0-9A-Za-z_\-]{35}"
    r"|xox[baprs]-[0-9A-Za-z\-]{10,}"
    r"|-----BEGIN (?:[A-Z]+ )*PRIVATE KEY(?: BLOCK)?-----"
)

RULES: Tuple[Rule, ...] = (
    Rule(
        "predecessor-name",
        "name of a predecessor project",
        ALL_KINDS,
        regex_finder(re.compile(r"nexa[\s_.\-]?graph", re.IGNORECASE)),
    ),
    Rule(
        "private-repo-link",
        "link to a private repository, or to an unlisted repository under the project's handle",
        ALL_KINDS,
        regex_finder(
            re.compile(
                r"(?:github\.com|raw\.githubusercontent\.com|api\.github\.com/repos)[/:]fox1245/"
                r"(?P<repo>[A-Za-z0-9_.\-]+)",
                re.IGNORECASE,
            ),
            _accept_repo_link,
        ),
    ),
    Rule(
        "internal-process-wording",
        "wording that refers to an internal decision process or internal tooling",
        ALL_KINDS,
        regex_finder(
            re.compile(
                r"(?i:owner-local|(?<![A-Za-z0-9_])the owner['’]s|owner['’]s decision"
                r"|following the owner|(?<![A-Za-z0-9_])owner decision)"
                r"|(?<![A-Za-z0-9])AgentX(?![A-Za-z0-9])|(?<![A-Za-z0-9])WorkOrder"
                r"|Analysis Graph provenance"
            )
        ),
    ),
    Rule(
        "home-path",
        "personal home directory",
        ALL_KINDS,
        regex_finder(
            re.compile(
                r"(?<![A-Za-z0-9_.\-])(?:/home/|/Users/|[A-Za-z]:(?:\\{1,2}|/)Users(?:\\{1,2}|/))"
                r"(?P<name>[^/\\\s'\"`<>:;,()\[\]{}$%*?|~]+)"
                r"(?=[/\\]|$|[\s'\"`,;:)\]])"
            ),
            _accept_home_path,
        ),
    ),
    Rule(
        "external-ip",
        "globally routable IP address literal",
        ALL_KINDS,
        find_public_ip,
    ),
    Rule(
        "secret-shaped",
        "string shaped like an API key, token or private key",
        ALL_KINDS,
        regex_finder(_SECRET),
        masked=True,
    ),
    Rule(
        "private-project-name",
        "name of a private project",
        ALL_KINDS,
        regex_finder(re.compile(r"(?<![A-Za-z0-9])(?:Capstone|CAPSTONE)(?![A-Za-z0-9])")),
    ),
)

RULE_NAMES = tuple(rule.name for rule in RULES)


# ---------------------------------------------------------------------------
# Allowlist
# ---------------------------------------------------------------------------


class AllowlistError(ValueError):
    pass


@dataclass(frozen=True)
class AllowEntry:
    glob: str
    rule: str
    reason: str

    def covers(self, path: str, rule: str) -> bool:
        return self.rule in ("*", rule) and fnmatch.fnmatchcase(path, self.glob)


def parse_allowlist(text: str, source: str = "allowlist") -> List[AllowEntry]:
    entries: List[AllowEntry] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip("\r\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 3 or not all(field.strip() for field in fields[:2]) or not "\t".join(fields[2:]).strip():
            raise AllowlistError(
                f"{source}:{number}: expected path-glob<TAB>rule<TAB>reason, got {line!r}"
            )
        glob, rule, reason = fields[0].strip(), fields[1].strip(), "\t".join(fields[2:]).strip()
        if rule != "*" and rule not in RULE_NAMES:
            raise AllowlistError(f"{source}:{number}: unknown rule {rule!r}")
        entries.append(AllowEntry(glob, rule, reason))
    return entries


def load_allowlist(path: Optional[Path]) -> List[AllowEntry]:
    if path is None or not path.is_file():
        return []
    return parse_allowlist(path.read_text(encoding="utf-8"), str(path))


# ---------------------------------------------------------------------------
# Scanning
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    excerpt: str
    kind: str = KIND_CODE

    def render(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.excerpt}"


def _mask(secret: str) -> str:
    return f"{secret[:3]}***({len(secret)} chars masked)"


def make_excerpt(line: str, span: Span, masked: bool) -> str:
    start, end = span
    if masked:
        hidden = _mask(line[start:end])
        line = line[:start] + hidden + line[end:]
    stripped = line.strip()
    start -= len(line) - len(line.lstrip())
    if len(stripped) <= EXCERPT_WIDTH:
        return stripped
    # Keep the match visible in a long line (minified JSON, generated text).
    lo = max(0, min(start - EXCERPT_WIDTH // 4, len(stripped) - EXCERPT_WIDTH))
    hi = lo + EXCERPT_WIDTH
    return ("..." if lo else "") + stripped[lo:hi] + ("..." if hi < len(stripped) else "")


def scan_line(line: str, kind: str, rules: Sequence[Rule] = RULES) -> List[Tuple[Rule, str]]:
    """Return (rule, excerpt) for each rule that fires on this line (once per rule)."""
    found: List[Tuple[Rule, str]] = []
    for rule in rules:
        if kind not in rule.kinds:
            continue
        span = rule.find(line)
        if span is not None:
            found.append((rule, make_excerpt(line, span, rule.masked)))
    return found


def scan_lines(
    path: str,
    lines: Iterable[Tuple[int, str]],
    allowlist: Sequence[AllowEntry] = (),
    rules: Sequence[Rule] = RULES,
) -> List[Finding]:
    kind = file_kind(path)
    active = [rule for rule in rules if not any(e.covers(path, rule.name) for e in allowlist)]
    findings: List[Finding] = []
    for number, line in lines:
        for rule, excerpt in scan_line(line, kind, active):
            findings.append(Finding(path, number, rule.name, excerpt, kind))
    return findings


def decode(data: bytes) -> Optional[List[str]]:
    """Lines of a text file, or None for a binary one."""
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n").split("\n")


def scan_text(
    path: str,
    text: str,
    allowlist: Sequence[AllowEntry] = (),
    rules: Sequence[Rule] = RULES,
) -> List[Finding]:
    """Scan a whole text; the entry point the tests use."""
    return scan_lines(path, enumerate(text.splitlines(), 1), allowlist, rules)


# ---------------------------------------------------------------------------
# Git plumbing
# ---------------------------------------------------------------------------


class GitError(RuntimeError):
    pass


def git(root: Path, *args: str, check: bool = True) -> bytes:
    try:
        result = subprocess.run(
            ["git", "-c", "core.quotePath=false", *args],
            cwd=str(root),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except FileNotFoundError as exc:
        raise GitError("git is not installed or not on PATH") from exc
    if check and result.returncode != 0:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise GitError(f"git {' '.join(args)} failed: {message}")
    return result.stdout if result.returncode == 0 else b""


def in_git_checkout(root: Path) -> bool:
    try:
        return git(root, "rev-parse", "--is-inside-work-tree", check=False).strip() == b"true"
    except GitError:
        return False


def tracked_files(root: Path) -> List[str]:
    out = git(root, "ls-files", "-z")
    return [p for p in out.decode("utf-8", errors="replace").split("\0") if p]


def walk_files(root: Path) -> List[str]:
    """Fallback for a source tree that is not a git checkout (an unpacked archive)."""
    names: List[str] = []
    skipped = (".git", ".venv", "venv", "__pycache__", "node_modules", "CMakeFiles", "_deps")
    for current, dirs, files in os.walk(root):
        top_level = Path(current) == root
        dirs[:] = [
            d for d in dirs
            if d not in skipped and not (top_level and d.startswith(("build", "cmake-build")))
        ]
        for name in files:
            names.append(Path(current, name).relative_to(root).as_posix())
    return sorted(names)


def scan_tree(
    root: Path,
    allowlist: Sequence[AllowEntry],
    scope: str = "all",
) -> Tuple[List[Finding], int]:
    names = tracked_files(root) if in_git_checkout(root) else walk_files(root)
    findings: List[Finding] = []
    scanned = 0
    for name in names:
        if is_out_of_scope(name):
            continue
        if scope == "code" and file_kind(name) == KIND_DOC:
            continue
        full = root / name
        if full.is_symlink() or not full.is_file():
            continue  # submodules (gitlinks), symlinks, files removed from the working tree
        if full.stat().st_size > MAX_FILE_BYTES:
            continue
        lines = decode(full.read_bytes())
        if lines is None:
            continue
        scanned += 1
        findings.extend(scan_lines(name, enumerate(lines, 1), allowlist))
    return findings, scanned


_HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def parse_added_lines(diff: str) -> Iterator[Tuple[str, int, str]]:
    """Yield (path, line number, text) for every added line of a -U0 diff."""
    path: Optional[str] = None
    in_hunk = False
    number = 0
    for raw in diff.split("\n"):
        if raw.startswith("diff --git "):
            path, in_hunk = None, False
            continue
        if not in_hunk:
            if raw.startswith("+++ "):
                target = raw[4:].rstrip("\t")
                path = None if target == "/dev/null" else (
                    target[2:] if target.startswith("b/") else target
                )
            elif raw.startswith("@@"):
                match = _HUNK.match(raw)
                if match and path is not None:
                    number, in_hunk = int(match.group(1)), True
            continue
        if raw.startswith("@@"):
            match = _HUNK.match(raw)
            if match:
                number = int(match.group(1))
            continue
        if raw.startswith("+"):
            if path is not None:
                yield path, number, raw[1:].rstrip("\r")
            number += 1


def scan_diff(
    root: Path,
    base: str,
    allowlist: Sequence[AllowEntry],
    scope: str = "all",
) -> Tuple[List[Finding], int]:
    if not in_git_checkout(root):
        raise GitError(f"{root} is not a git checkout; --diff needs one")
    git(root, "rev-parse", "--verify", "--quiet", base + "^{commit}")
    merge_base = git(root, "merge-base", base, "HEAD", check=False).decode().strip()
    reference = merge_base or base
    diff = git(
        root, "diff", "--no-color", "--no-ext-diff", "--no-textconv", "--unified=0",
        "--src-prefix=a/", "--dst-prefix=b/", "--diff-filter=AMRC", reference, "--",
    ).decode("utf-8", errors="replace")

    per_path: Dict[str, List[Tuple[int, str]]] = {}
    for path, number, text in parse_added_lines(diff):
        per_path.setdefault(path, []).append((number, text))

    untracked = git(root, "ls-files", "-z", "--others", "--exclude-standard")
    for name in untracked.decode("utf-8", errors="replace").split("\0"):
        full = root / name
        if not name or full.is_symlink() or not full.is_file() or full.stat().st_size > MAX_FILE_BYTES:
            continue
        lines = decode(full.read_bytes())
        if lines is not None:
            per_path[name] = list(enumerate(lines, 1))

    findings: List[Finding] = []
    scanned = 0
    for name in sorted(per_path):
        if is_out_of_scope(name) or (scope == "code" and file_kind(name) == KIND_DOC):
            continue
        scanned += 1
        findings.extend(scan_lines(name, per_path[name], allowlist))
    return findings, scanned


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def summarize(findings: Sequence[Finding]) -> str:
    by_rule: Dict[str, int] = {}
    by_kind: Dict[str, int] = {}
    for finding in findings:
        by_rule[finding.rule] = by_rule.get(finding.rule, 0) + 1
        by_kind[finding.kind] = by_kind.get(finding.kind, 0) + 1
    rules = ", ".join(f"{name}={by_rule[name]}" for name in RULE_NAMES if name in by_rule)
    kinds = ", ".join(f"{name}={by_kind[name]}" for name in sorted(by_kind))
    return f"by rule: {rules}; by file kind: {kinds}"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Find internal trivia in public text.",
        epilog="Exit status: 0 clean, 1 findings, 2 usage or environment error.",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--tree", action="store_true", help="scan every tracked file (default)")
    mode.add_argument(
        "--diff", nargs="?", const=DEFAULT_BASE, metavar="BASE",
        help=f"scan only lines added since BASE (default {DEFAULT_BASE})",
    )
    parser.add_argument(
        "--scope", choices=("all", "code"), default="all",
        help="'code' skips documents (Markdown, docs/, README*, CHANGELOG*)",
    )
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    parser.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST)
    parser.add_argument("--list-rules", action="store_true", help="print the rules and exit")
    args = parser.parse_args(argv)

    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):  # Windows consoles default to a legacy code page
            stream.reconfigure(encoding="utf-8", errors="replace")

    if args.list_rules:
        for rule in RULES:
            print(f"{rule.name}\t{','.join(sorted(rule.kinds))}\t{rule.summary}")
        return 0

    try:
        allowlist = load_allowlist(args.allowlist)
        root = args.root.resolve()
        if args.diff is not None:
            findings, scanned = scan_diff(root, args.diff, allowlist, args.scope)
            label = f"added lines since {args.diff}"
        else:
            findings, scanned = scan_tree(root, allowlist, args.scope)
            label = "tracked files"
    except (AllowlistError, GitError, OSError) as exc:
        print(f"check_public_text: {exc}", file=sys.stderr)
        return 2

    findings.sort(key=lambda f: (f.path, f.line, f.rule))
    for finding in findings:
        print(finding.render())
    scope = "documents skipped" if args.scope == "code" else "documents included"
    if findings:
        print(
            f"check_public_text: {len(findings)} finding(s) in {scanned} file(s) of {label} "
            f"({scope}); {summarize(findings)}",
            file=sys.stderr,
        )
        return 1
    print(f"check_public_text: clean ({scanned} file(s) of {label}, {scope})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
