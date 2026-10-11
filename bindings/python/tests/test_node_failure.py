"""Node failure classification for Python node bodies (#364).

A Python exception raised by a node is an ordinary failure. When it carries a
string ``code`` attribute that is an author code (``app.compile_error``; the
``NG.`` prefix is reserved) the failure uses that code, and a boolean
``retryable`` attribute of ``False`` stops the retry loop. The caller still
receives the original exception object.
"""

import itertools

import pytest

import neograph_engine as ng


_ids = itertools.count()


def _engine(node_class, max_retries=2, checkpoints=None):
    type_name = f"node_failure_{next(_ids)}"
    ng.NodeFactory.register_type(type_name, lambda name, _config, _ctx: node_class(name))
    definition = {
        "name": "node_failure",
        "channels": {"result": {"reducer": "overwrite"}},
        "nodes": {"work": {"type": type_name}},
        "edges": [
            {"from": ng.START_NODE, "to": "work"},
            {"from": "work", "to": ng.END_NODE},
        ],
        "retry_policy": {"max_retries": max_retries, "initial_delay_ms": 1,
                         "backoff_multiplier": 1.0, "max_delay_ms": 1},
    }
    if checkpoints is None:
        return ng.GraphEngine.compile(definition, ng.NodeContext())
    return ng.GraphEngine.compile(definition, ng.NodeContext(), checkpoints)


def _failing_node(make_error, succeed_on=None):
    calls = []

    class Node(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, _input):
            calls.append(len(calls))
            if succeed_on is not None and len(calls) - 1 == succeed_on:
                return [ng.ChannelWrite("result", "ok")]
            raise make_error()

    return Node, calls


class CompileError(Exception):
    code = "app.compile_error"
    retryable = False


class FlakyCheck(Exception):
    code = "app.flaky_check"
    retryable = True


class HttpLikeError(Exception):
    """A library error whose ``code`` is an HTTP status, not an author code."""

    def __init__(self):
        super().__init__("service unavailable")
        self.code = 503


class VendorCodedError(Exception):
    code = "E42"


class GiveUp(Exception):
    retryable = False


def test_non_retryable_author_code_runs_once_and_keeps_the_exception():
    error = CompileError("the patch does not compile")
    node, calls = _failing_node(lambda: error)
    engine = _engine(node)

    with pytest.raises(CompileError) as info:
        engine.run(ng.RunConfig(thread_id="compile-error", input={}))

    assert len(calls) == 1
    assert info.value is error
    assert info.value.node_name == "work"
    assert info.value.attempts == 1
    error.__traceback__ = None


def test_retryable_author_code_is_retried():
    node, calls = _failing_node(lambda: FlakyCheck("flaky"), succeed_on=2)
    result = _engine(node).run(ng.RunConfig(thread_id="flaky-check", input={}))

    assert len(calls) == 3
    assert result.output["channels"]["result"]["value"] == "ok"


def test_retryable_false_without_a_code_still_stops_the_retry_loop():
    node, calls = _failing_node(lambda: GiveUp("no point retrying"))
    with pytest.raises(GiveUp) as info:
        _engine(node).run(ng.RunConfig(thread_id="give-up", input={}))
    assert len(calls) == 1
    assert info.value.attempts == 1


@pytest.mark.parametrize("error_class", [HttpLikeError, VendorCodedError, RuntimeError])
def test_other_python_errors_are_ordinary_and_retried(error_class):
    node, calls = _failing_node(lambda: error_class())
    with pytest.raises(error_class) as info:
        _engine(node).run(ng.RunConfig(thread_id=f"ordinary-{error_class.__name__}", input={}))
    assert len(calls) == 3
    assert info.value.attempts == 3
