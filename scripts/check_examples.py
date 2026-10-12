#!/usr/bin/env python3
"""Static checks that keep the examples from rotting. Runs as ctest `examples.index`.

  1. Every example declares how it is tested: `smoke-expect` (runs offline) or `smoke-skip`
     (needs a key, a service or a display) in its leading comment block / docstring, with
     well-formed annotations. See examples/run_example_smoke.py.
  2. Numbers are unique: no two C++ examples (files or directories) share an NN_ prefix, and
     no two Python examples do.
  3. The READMEs index every example, and the "Offline" column agrees with the annotations:
     `yes` means smoke-expect; `key`, `service` or `paired` means smoke-skip. Checked in the English
     README and in every translation of it.

Exit status 0 when everything holds. Each error says what to change.
"""

from __future__ import annotations

import importlib.util
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples"
PYTHON_EXAMPLES = ROOT / "bindings" / "python" / "examples"
LOCALES = ("", ".ko", ".ja", ".zh-CN")
OFFLINE_TOKENS = ("yes", "key", "service", "paired")
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
MAIN = re.compile(r"\bint\s+main\s*\(")


def load_runner():
    spec = importlib.util.spec_from_file_location("run_example_smoke", EXAMPLES / "run_example_smoke.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["run_example_smoke"] = module
    spec.loader.exec_module(module)
    return module


def cpp_sources():
    """Map each C++ example to the source that carries its annotations."""
    found = {}
    for path in sorted(EXAMPLES.iterdir()):
        if re.match(r"\d\d_", path.name):
            if path.is_file() and path.suffix == ".cpp":
                found[path.name] = path
            elif path.is_dir() and (path / "main.cpp").is_file():
                found[path.name + "/"] = path / "main.cpp"
    for path in sorted((EXAMPLES / "cookbook").rglob("*.cpp")):
        relative = path.relative_to(EXAMPLES)
        if "build" in relative.parts:
            continue
        if "jarvis" in relative.parts and relative.as_posix() != "cookbook/jarvis/src/main.cpp":
            continue  # the Jarvis cookbook's other sources are libraries of its own CMake project
        if MAIN.search(path.read_text(encoding="utf-8", errors="replace")):
            found[relative.as_posix()] = path
    return found


def python_sources():
    return {path.name: path for path in sorted(PYTHON_EXAMPLES.glob("[0-9][0-9]_*.py"))}


def check_numbers(errors):
    cpp = defaultdict(list)
    for path in EXAMPLES.iterdir():
        match = re.match(r"(\d\d)_", path.name)
        if match and ((path.is_file() and path.suffix == ".cpp") or path.is_dir()):
            cpp[match.group(1)].append(path.name)
    for number, names in sorted(cpp.items()):
        if len(names) > 1:
            errors.append(f"examples/: number {number} is used by {', '.join(sorted(names))}; "
                          "every example needs its own number")
    py = defaultdict(list)
    for name in python_sources():
        py[name[:2]].append(name)
    for number, names in sorted(py.items()):
        if len(names) > 1:
            errors.append(f"bindings/python/examples/: number {number} is used by {', '.join(names)}")


def check_annotations(errors, runner, sources, label):
    declared = {}
    for name, path in sources.items():
        notes = runner.parse_annotations(path)
        errors.extend(f"{label}{message}" for message in notes.errors)
        if not notes.declared:
            errors.append(f"{label}{name}: add `smoke-expect: <a line it prints>` (offline) or "
                          "`smoke-skip: <why it cannot run offline>` to the leading comment block")
        declared[name] = "yes" if (notes.expect or notes.expect_line) else ("skip" if notes.skip else "none")
    return declared


def table_rows(readme: Path):
    """Yield (line number, link targets, Offline cell) for rows of tables that have an Offline column."""
    column = None
    for number, line in enumerate(readme.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.startswith("|"):
            column = None
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if column is None:
            lowered = [cell.lower() for cell in cells]
            column = lowered.index("offline") if "offline" in lowered else -1
            continue
        if set("".join(cells)) <= set("-: "):
            continue
        if column >= 0 and column < len(cells):
            yield number, LINK.findall(line), cells[column]


def check_readme(errors, readme: Path, declared, base: Path, prefix: str):
    if not readme.is_file():
        errors.append(f"{readme.relative_to(ROOT)}: missing")
        return
    listed = set()
    for number, targets, offline in table_rows(readme):
        for target in targets:
            key = target.split("#")[0]
            key = key.removeprefix("./")
            if prefix and key.startswith(prefix):
                key = key[len(prefix):]
            if key not in declared:
                continue
            listed.add(key)
            where = f"{readme.relative_to(ROOT)}:{number}"
            if offline not in OFFLINE_TOKENS:
                errors.append(f"{where}: Offline column of {key} must be one of {', '.join(OFFLINE_TOKENS)}, got '{offline}'")
            elif declared[key] == "yes" and offline != "yes":
                errors.append(f"{where}: {key} has smoke-expect, so its Offline cell must be 'yes', got '{offline}'")
            elif declared[key] == "skip" and offline == "yes":
                errors.append(f"{where}: {key} has smoke-skip, so its Offline cell must be 'key', 'service' or 'paired', not 'yes'")
    for key in declared:
        if key not in listed and not key.startswith("cookbook/"):
            errors.append(f"{readme.relative_to(ROOT)}: {key} is not in an index table with an Offline column")


def main() -> int:
    runner = load_runner()
    errors: list[str] = []
    check_numbers(errors)
    cpp = cpp_sources()
    cpp_declared = check_annotations(errors, runner, cpp, "examples/")
    python = python_sources()
    python_declared = check_annotations(errors, runner, python, "bindings/python/examples/")
    for locale in LOCALES:
        check_readme(errors, EXAMPLES / f"README{locale}.md", cpp_declared, EXAMPLES, "")
        check_readme(errors, PYTHON_EXAMPLES / f"README{locale}.md", python_declared, PYTHON_EXAMPLES, "")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print(f"\n{len(errors)} problem(s). See examples/run_example_smoke.py for the annotation grammar.",
              file=sys.stderr)
        return 1
    offline_cpp = sum(1 for value in cpp_declared.values() if value == "yes")
    offline_py = sum(1 for value in python_declared.values() if value == "yes")
    print(f"OK: {len(cpp)} C++ examples ({offline_cpp} offline), "
          f"{len(python)} Python examples ({offline_py} offline)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
