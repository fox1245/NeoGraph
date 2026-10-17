"""Converting Python values to JSON stops at json::parse's nesting limit.

The conversion recursed once per level without a limit or a cycle check, so
a value that contained itself recursed until the process ran out of stack
(#484).
"""
import pytest

import neograph_engine as ng

# json::max_parse_depth: the levels json::parse admits, and the conversion too.
MAX_DEPTH = 256


def nested_lists(depth):
    value = []
    for _ in range(depth - 1):
        value = [value]
    return value


def nested_dicts(depth):
    value = {}
    for _ in range(depth - 1):
        value = {"a": value}
    return value


def test_converts_a_value_at_the_limit():
    for value in (nested_lists(MAX_DEPTH), nested_dicts(MAX_DEPTH)):
        assert ng.ChannelWrite("c", value).value == value


def test_refuses_one_level_more():
    too_deep = (
        nested_lists(MAX_DEPTH + 1),
        nested_dicts(MAX_DEPTH + 1),
        (nested_lists(MAX_DEPTH),),
    )
    for value in too_deep:
        with pytest.raises(ValueError, match="nested deeper than 256 levels"):
            ng.ChannelWrite("c", value)


def test_a_value_that_contains_itself_raises():
    loop = {}
    loop["self"] = loop
    with pytest.raises(ValueError, match="contains itself"):
        ng.ChannelWrite("c", loop)
    items = []
    items.append(items)
    with pytest.raises(ValueError, match="contains itself"):
        ng.ChannelWrite("c", items)
