"""A file that ships with the package or the examples must import on the oldest supported Python.

The wheels are built for Python 3.9, where `X | None` in an annotation that is evaluated when the
function is defined raises TypeError. A module avoids that with `from __future__ import annotations`
or `Optional[X]`. This test reads the sources, so it finds the mistake on any interpreter.
"""

import ast
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = sorted([*(ROOT / "examples").rglob("*.py"), *(ROOT / "neograph_engine").rglob("*.py")])

pytestmark = pytest.mark.skipif(not SOURCES, reason="the sources are not part of this checkout")


def _annotations(tree):
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            arguments = node.args
            every = [*arguments.posonlyargs, *arguments.args, *arguments.kwonlyargs]
            every += [item for item in (arguments.vararg, arguments.kwarg) if item]
            for argument in every:
                if argument.annotation is not None:
                    yield argument.annotation
            if node.returns is not None:
                yield node.returns
        elif isinstance(node, ast.AnnAssign):
            yield node.annotation


def _has_future_annotations(tree):
    return any(isinstance(node, ast.ImportFrom) and node.module == "__future__"
               and any(alias.name == "annotations" for alias in node.names) for node in tree.body)


def test_there_are_sources_to_check():
    assert len(SOURCES) >= 20


@pytest.mark.parametrize("path", SOURCES, ids=lambda path: str(path.relative_to(ROOT)))
def test_no_annotation_needs_python_310(path):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    if _has_future_annotations(tree):
        return
    unions = [annotation.lineno for annotation in _annotations(tree)
              if any(isinstance(part, ast.BinOp) and isinstance(part.op, ast.BitOr)
                     for part in ast.walk(annotation))]
    assert not unions, (
        f"{path.name} uses `X | Y` in annotations at line(s) {unions}; Python 3.9 evaluates them "
        "when the function is defined. Add `from __future__ import annotations` or use Optional/Union.")
