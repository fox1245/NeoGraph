"""Run every offline Python example in bindings/python/examples and check what it prints.

Each example declares its own expectations in its module docstring (see
examples/run_example_smoke.py for the grammar): `smoke-expect` lines that must appear in
the output, or a `smoke-skip` reason when it needs a key, a service or a display. An
example with neither fails here, so a new example cannot be added without saying how it
is tested. The examples run with no API key against the installed `neograph_engine`.
"""

import importlib.util
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
EXAMPLES = ROOT / "bindings" / "python" / "examples"
RUNNER = ROOT / "examples" / "run_example_smoke.py"

pytestmark = pytest.mark.skipif(
    not (RUNNER.is_file() and EXAMPLES.is_dir()), reason="examples are not part of this checkout")


def _load_runner():
    spec = importlib.util.spec_from_file_location("run_example_smoke", RUNNER)
    module = importlib.util.module_from_spec(spec)
    sys.modules["run_example_smoke"] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def workspace(tmp_path_factory):
    """A copy of the examples directory: 14 writes my_graph.json beside itself and 15 reads it."""
    destination = tmp_path_factory.mktemp("python-examples") / "examples"
    shutil.copytree(EXAMPLES, destination, ignore=shutil.ignore_patterns("__pycache__"))
    return destination


SCRIPTS = sorted(path.name for path in EXAMPLES.glob("[0-9][0-9]_*.py")) if EXAMPLES.is_dir() else []


def test_there_are_examples():
    assert len(SCRIPTS) >= 20


@pytest.mark.parametrize("name", SCRIPTS)
def test_example(name, workspace, capfd):
    runner = _load_runner()
    source = workspace / name
    notes = runner.parse_annotations(source)
    assert not notes.errors, notes.errors
    assert notes.declared, (
        f"{name} has no 'smoke-expect:' or 'smoke-skip:' line in its docstring; "
        "see examples/run_example_smoke.py")
    status = runner.run_smoke(source, [sys.executable, str(source)], python=sys.executable, cwd=workspace)
    captured = capfd.readouterr()
    if status == runner.SKIP_STATUS:
        pytest.skip(captured.out.strip() or f"{name} is not run offline")
    assert status == 0, captured.err or captured.out
