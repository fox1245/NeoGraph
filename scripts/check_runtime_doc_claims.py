#!/usr/bin/env python3
"""Reject stale runtime claims while preserving labeled historical tests."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ACTIVE_ROOTS = (ROOT / "include", ROOT / "src")
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc"}
FALSE_BARRIER_CLAIMS = (
    "per-run and in-memory only",
    "checkpoint/resume round trip drops",
)


def source_files(root: Path):
    for path in root.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def main() -> int:
    errors: list[str] = []

    for active_root in ACTIVE_ROOTS:
        for path in source_files(active_root):
            for line_number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), start=1
            ):
                if "Taskflow" in line:
                    errors.append(
                        f"{path.relative_to(ROOT)}:{line_number}: "
                        "active runtime comment still mentions Taskflow"
                    )
                lowered = line.lower()
                if any(claim in lowered for claim in FALSE_BARRIER_CLAIMS):
                    errors.append(
                        f"{path.relative_to(ROOT)}:{line_number}: "
                        "active barrier persistence claim is stale"
                    )

    tests_root = ROOT / "tests"
    for path in source_files(tests_root):
        lines = path.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            if "Taskflow" not in line:
                continue
            context = " ".join(lines[max(0, index - 1) : index + 2]).lower()
            if "histor" not in context and "former" not in context:
                errors.append(
                    f"{path.relative_to(ROOT)}:{index + 1}: "
                    "test Taskflow reference is not labeled historical"
                )

    if errors:
        print("runtime documentation claim errors:")
        for error in errors:
            print(f"  {error}")
        return 1

    print("runtime documentation claims: active source clean; historical tests labeled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
