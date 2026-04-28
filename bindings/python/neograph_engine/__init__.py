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

import os as _os


def _ensure_ssl_ca_bundle() -> None:
    """Auto-point the manylinux-bundled OpenSSL at a CA store the host
    actually has.

    The PyPI manylinux_2_34 wheel links the OpenSSL that AlmaLinux 9
    ships in the build container — its compiled-in CA store paths are
    ``/etc/pki/tls/certs/ca-bundle.crt`` and friends. Hosts that don't
    follow the RHEL layout (Ubuntu/Debian/Alpine/macOS) end up with
    libssl unable to verify any peer cert, and TLS handshakes against
    api.openai.com / api.anthropic.com hang silently for the full
    request timeout (60 s default) before the engine surfaces them as
    ``ConnPool::async_post: timeout``. curl works because curl uses
    the system libssl, not the wheel-bundled one.

    We unblock by pointing libssl at the ``certifi`` Mozilla CA bundle
    via ``SSL_CERT_FILE`` whenever the caller hasn't set it themselves.
    Idempotent and explicit — the user can override either by setting
    ``SSL_CERT_FILE`` to a different path before importing or by
    setting ``NEOGRAPH_SKIP_CERT_AUTOFIX=1``.
    """
    if _os.environ.get("NEOGRAPH_SKIP_CERT_AUTOFIX"):
        return
    if _os.environ.get("SSL_CERT_FILE"):
        return  # honour an explicit override
    try:
        import certifi  # type: ignore[import-untyped]
    except ImportError:
        return  # certifi missing — trust the host store, surface the
                # 60 s hang to the user with a clear hint via __doc__.
    bundle = certifi.where()
    if bundle and _os.path.isfile(bundle):
        _os.environ["SSL_CERT_FILE"] = bundle


_ensure_ssl_ca_bundle()


from ._neograph import (
    # Versioning
    __version__,

    # Provider surface
    Provider,
    CompletionParams,
    ChatMessage,
    ToolCall,
    ChatTool,
    ChatCompletion,

    # Graph types
    ChannelWrite,
    Send,
    Command,
    NodeResult,

    # Custom-node surface
    GraphState,
    NodeFactory,
    ReducerRegistry,
    ConditionRegistry,

    # Engine
    GraphEngine,
    RunConfig,
    RunResult,
    CheckpointStore,
    InMemoryCheckpointStore,

    # Streaming
    StreamMode,
    GraphEvent,
    # Streaming helpers (Python module — no C++ side)
    # exposed via `from neograph_engine.streaming import message_stream`
    # but also re-exported here for discoverability.

    # Constants
    START_NODE,
    END_NODE,
)

# PostgresCheckpointStore is only present when the binding was built
# with -DNEOGRAPH_BUILD_POSTGRES=ON. The PyPI wheel ships with it OFF
# (libpq bundling is a separate cibw step). Re-export when available
# so source-build users / advanced installs get the durable store
# under the same `neograph_engine.PostgresCheckpointStore` name.
try:
    from ._neograph import PostgresCheckpointStore  # noqa: F401
    _HAVE_POSTGRES = True
except ImportError:
    _HAVE_POSTGRES = False

# Re-import the C++ Tool / NodeContext bindings under private names so
# we can shadow them with Python-side wrappers below.
from ._neograph import Tool as _CppTool
from ._neograph import NodeContext as _CppNodeContext


class Tool:
    """Base class for Python-defined tools.

    Subclass and override:
      - ``get_name(self) -> str``               (required)
      - ``get_definition(self) -> ChatTool``    (required — sent to LLM)
      - ``execute(self, arguments) -> str``     (required — `arguments` is a dict)

    The ``ChatTool`` returned by ``get_definition()`` describes what
    arguments the LLM may pass — its ``name`` MUST match
    ``get_name()``, otherwise the engine's tool-dispatch lookup misses.
    Pass instances into ``NodeContext(tools=[...])``; the engine takes
    ownership at compile time so a Python reference isn't required to
    keep them alive.

    Example::

        class CalculatorTool(neograph_engine.Tool):
            def get_name(self):
                return "calculator"

            def get_definition(self):
                return neograph_engine.ChatTool(
                    name="calculator",
                    description="Evaluate a math expression",
                    parameters={
                        "type": "object",
                        "properties": {"expression": {"type": "string"}},
                        "required": ["expression"],
                    },
                )

            def execute(self, arguments):
                import ast, operator
                tree = ast.parse(arguments["expression"], mode="eval")
                return str(eval(compile(tree, "", "eval")))
    """

    def get_name(self) -> str:
        raise NotImplementedError(
            "Tool subclasses must override get_name() to return the "
            "tool's name (must match the name in get_definition()).")

    def get_definition(self):
        raise NotImplementedError(
            "Tool subclasses must override get_definition() to return "
            "a ChatTool describing the tool's parameter schema.")

    def execute(self, arguments) -> str:
        raise NotImplementedError(
            "Tool subclasses must override execute(arguments) to "
            "perform the tool's work and return a string result.")


class NodeContext(_CppNodeContext):
    """Engine context — provider, tools, model, instructions.

    Subclasses the C++ ``_CppNodeContext`` binding. The C++ struct
    has a ``vector<Tool*>`` for tools, but we can't populate it from
    Python directly (raw-pointer ownership doesn't translate). Instead,
    the Python-side ``tools`` list lives in a ``_pytools`` dynamic
    attribute, and ``GraphEngine.compile()`` reads it back to wrap each
    Python Tool in a C++ trampoline (``PyToolOwner``) and transfer
    ownership to the engine.
    """

    def __init__(self, provider=None, tools=None,
                 model="", instructions="", extra_config=None):
        super().__init__(
            provider=provider,
            model=model,
            instructions=instructions,
            extra_config=extra_config or {},
        )
        # Stash the Python tool list on the wrapper. compile() reads
        # it via py::hasattr / py::object.attr("_pytools").
        self._pytools = list(tools or [])


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
    "ReducerRegistry",
    "ConditionRegistry",
    "node",
    "GraphEngine",
    "RunConfig",
    "RunResult",
    "CheckpointStore",
    "InMemoryCheckpointStore",
    # PostgresCheckpointStore appended dynamically below when present.
    "StreamMode",
    "GraphEvent",
    "START_NODE",
    "END_NODE",
]

if _HAVE_POSTGRES:
    __all__.append("PostgresCheckpointStore")

# Streaming helpers — pure-Python, no C++ side. Re-exported here so a
# `from neograph_engine import message_stream` import works alongside
# the more explicit `from neograph_engine.streaming import message_stream`.
from .streaming import message_stream  # noqa: E402
__all__.append("message_stream")
