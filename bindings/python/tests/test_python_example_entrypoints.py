"""Static checks for shipped Python example entrypoints."""

from __future__ import annotations

import ast
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
EXAMPLE_ROOTS = (
    ROOT / "bindings/python/examples",
    ROOT / "examples/cookbook",
)


def _example_files() -> list[Path]:
    return [
        path
        for root in EXAMPLE_ROOTS
        for path in root.rglob("*.py")
        if path.is_file()
    ]


def _has_main_guard(node: ast.If) -> bool:
    test = node.test
    return (
        isinstance(test, ast.Compare)
        and isinstance(test.left, ast.Name)
        and test.left.id == "__name__"
        and len(test.ops) == 1
        and isinstance(test.ops[0], ast.Eq)
        and len(test.comparators) == 1
        and isinstance(test.comparators[0], ast.Constant)
        and test.comparators[0].value == "__main__"
    )


def test_python_examples_parse_as_valid_python():
    offenders = []
    for path in _example_files():
        try:
            ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except SyntaxError as exc:
            offenders.append(f"{path.relative_to(ROOT)}: {exc}")
    assert not offenders, "Python examples with syntax errors:\n" + "\n".join(offenders)


def test_main_guard_calls_only_defined_main_functions():
    offenders = []
    for path in _example_files():
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        defined_main = any(
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == "main"
            for node in tree.body
        )
        for node in tree.body:
            if not isinstance(node, ast.If) or not _has_main_guard(node):
                continue
            calls_main = any(
                isinstance(call, ast.Call)
                and isinstance(call.func, ast.Name)
                and call.func.id == "main"
                for statement in node.body
                for call in ast.walk(statement)
            )
            if calls_main and not defined_main:
                offenders.append(str(path.relative_to(ROOT)))
    assert not offenders, "main guards call an undefined main(): " + ", ".join(offenders)
