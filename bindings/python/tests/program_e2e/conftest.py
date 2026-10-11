from pathlib import Path

import pytest

import neograph_engine as ng

from . import support

_PACKAGE = Path(support.PACKAGE_DIR)


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "known_issue: strict xfail for an open tracked issue; the reason names it")
    config.addinivalue_line(
        "markers", "slow: a scenario that waits on purpose for a second or more")
    config.addinivalue_line(
        "markers", "live: calls a hosted model; runs only with NEOGRAPH_E2E_LIVE=1 and "
                   "OPENROUTER_API_KEY")


def pytest_collection_modifyitems(config, items):
    # This hook sees every collected item, so limit the skip to this package.
    if getattr(ng, "_HAVE_PROGRAM", False):
        return
    skip = pytest.mark.skip(reason="neograph_engine was built without Program/QuickJS control")
    for item in items:
        if _PACKAGE in Path(str(item.fspath)).parents:
            item.add_marker(skip)


@pytest.fixture(autouse=True)
def _fresh_journal():
    support.JOURNAL.clear()
    yield
    support.JOURNAL.clear()


@pytest.fixture
def host():
    return support.make_host()
