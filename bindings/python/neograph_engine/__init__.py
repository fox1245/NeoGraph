"""NeoGraph — C++ graph agent engine, Python bindings.

This package re-exports the symbols of the underlying ``_neograph`` C
extension. The C extension is what links against ``libneograph_*.so``;
this file is the stable Python import surface so editor tooling and
``help(neograph_engine)`` see something more useful than ``<module>``.

Distribution name on PyPI: ``neograph-engine``. The bare ``neograph``
name was already taken on PyPI (a Python LangGraph wrapper); the
``-engine`` suffix makes the positioning explicit — this package IS
the C++ runtime, not a Python wrapper around someone else's runtime.

Example:
    >>> import neograph_engine as ng
    >>> from neograph_engine.llm import SchemaProvider
    >>> provider = SchemaProvider(schema_path="openai", api_key="sk-...")
    >>> ctx = ng.NodeContext(provider=provider)
    >>> engine = ng.GraphEngine.compile(my_graph_dict, ctx)
    >>> result = engine.run(ng.RunConfig(thread_id="demo", input={...}))
"""

from ._neograph import (
    # Versioning
    __version__,

    # Provider surface
    Provider,
    Tool,
    CompletionParams,
    ChatMessage,
    ToolCall,
    ChatTool,
    ChatCompletion,

    # Graph types
    NodeContext,
    ChannelWrite,
    Send,
    Command,
    NodeResult,

    # Custom-node surface
    GraphState,
    NodeFactory,

    # Engine
    GraphEngine,
    RunConfig,
    RunResult,

    # Streaming
    StreamMode,
    GraphEvent,

    # Constants
    START_NODE,
    END_NODE,
)


class GraphNode:
    """Base class for Python-defined graph nodes.

    Subclass this and override ``execute()`` (and ``get_name()`` if you
    don't pass the name through ``__init__``). For nodes that need to
    emit ``Command`` (routing override) or ``Send`` (dynamic fan-out),
    override ``execute_full()`` instead and return a :class:`NodeResult`
    or a list mixing ``ChannelWrite`` / ``Command`` / ``Send``.

    Register a factory so the JSON graph definition can reference your
    node by type name::

        class CounterNode(neograph_engine.GraphNode):
            def __init__(self, name):
                super().__init__()
                self._name = name

            def get_name(self):
                return self._name

            def execute(self, state):
                current = state.get("count") or 0
                return [neograph_engine.ChannelWrite("count", current + 1)]

        neograph_engine.NodeFactory.register_type(
            "counter",
            lambda name, config, ctx: CounterNode(name),
        )

    The C++ engine calls ``execute_full_async()`` under the hood, but
    the binding bridges that to your sync ``execute_full()`` /
    ``execute()`` for you — you do NOT need to write coroutines from
    Python. GIL handling is also automatic: every dispatch from an
    engine worker thread acquires the GIL before calling into your
    code, and releases it after returning.

    Streaming: override ``execute_stream(state, callback)`` if your
    node produces fine-grained events. The default reuses ``execute()``
    and ignores the callback.
    """

    def __init__(self):
        # Nothing to do in the base — the C++ wrapper holds the
        # engine-side reference. Subclasses that need init can call
        # ``super().__init__()`` for parity with idiomatic Python
        # subclassing patterns.
        pass

    def get_name(self) -> str:
        raise NotImplementedError(
            "GraphNode subclasses must override get_name() to return "
            "the unique node name within the graph.")

    def execute(self, state):
        raise NotImplementedError(
            "GraphNode subclasses must override execute() to return a "
            "list of ChannelWrite, OR override execute_full() to return "
            "a NodeResult with Command/Send.")


def node(type_name=None):
    """Decorator: register a function as a write-only node type.

    Sugar for the common case where your node is a pure function of
    state: takes a :class:`GraphState`, returns a list of channel
    writes. No subclassing, no ``execute_full()`` to think about.

    ::

        @neograph_engine.node("greet")
        def greet_node(state):
            return [neograph_engine.ChannelWrite("messages",
                [{"role": "assistant", "content": "hi!"}])]

    The decorated function is registered as a node type. Its name in
    the JSON definition is ``type_name`` (or the function's ``__name__``
    if not given). Internally we wrap it in a tiny GraphNode subclass.

    Limitations: decorator nodes can't emit Command / Send. Subclass
    :class:`GraphNode` and override ``execute_full()`` for those.
    """

    def decorator(fn):
        registered_type = type_name or fn.__name__

        class _DecoratorNode(GraphNode):
            def __init__(self, name):
                super().__init__()
                self._name = name

            def get_name(self):
                return self._name

            def execute(self, state):
                return fn(state) or []

        NodeFactory.register_type(
            registered_type,
            lambda name, config, ctx: _DecoratorNode(name),
        )
        return fn

    return decorator

# `from neograph_engine.llm import OpenAIProvider, SchemaProvider` — defined in
# llm.py so consumers can `from neograph_engine.llm import ...` mirroring the
# C++ neograph::llm:: namespace.

__all__ = [
    "__version__",
    "Provider",
    "Tool",
    "CompletionParams",
    "ChatMessage",
    "ToolCall",
    "ChatTool",
    "ChatCompletion",
    "NodeContext",
    "ChannelWrite",
    "Send",
    "Command",
    "NodeResult",
    "GraphState",
    "GraphNode",
    "NodeFactory",
    "node",
    "GraphEngine",
    "RunConfig",
    "RunResult",
    "StreamMode",
    "GraphEvent",
    "START_NODE",
    "END_NODE",
]
