"""The child-depth ceiling is a host setting of LocalProgramHost (default 8)."""

import pytest

import neograph_engine as ng

from . import support as s


def test_the_default_ceiling_is_eight_and_the_largest_is_exported():
    assert ng.MAX_SUPPORTED_CHILD_DEPTH == 8
    assert ng.MAX_REPRESENTABLE_CHILD_DEPTH > ng.MAX_SUPPORTED_CHILD_DEPTH


@pytest.mark.parametrize("value", [0, "above"])
def test_an_invalid_ceiling_is_refused_when_the_host_is_built(value):
    ceiling = ng.MAX_REPRESENTABLE_CHILD_DEPTH + 1 if value == "above" else value
    with pytest.raises(ValueError):
        s.make_host(max_supported_child_depth=ceiling)


def test_the_default_ceiling_refuses_a_policy_deeper_than_eight():
    with pytest.raises(ValueError):
        s.make_host(budget_ceiling=s.ceiling(max_child_depth=9, max_total_children=9))


def test_a_raised_ceiling_admits_a_policy_of_that_depth_and_no_deeper():
    s.make_host(budget_ceiling=s.ceiling(max_child_depth=12, max_total_children=12),
                max_supported_child_depth=12)
    with pytest.raises(ValueError):
        s.make_host(budget_ceiling=s.ceiling(max_child_depth=13, max_total_children=13),
                    max_supported_child_depth=12)
