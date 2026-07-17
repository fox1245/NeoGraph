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

import json as _json
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
    ToolDecision,
    ToolGateContext,

    # Long-term memory across conversations (#97)
    Store,
    InMemoryStore,
    StoreItem,

    # Pre-flight checks on a graph definition (#97)
    validate,
    ValidationReport,
    Diagnostic,

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

    # v0.4 unified-dispatch surface (PR 7) — new node API
    NodeInput,
    RunContext,
    CancelToken,

    # Custom-node surface
    GraphState,
    NodeFactory,
    ReducerRegistry,
    ConditionRegistry,

    # Topology schema export (issue #56) — drift-proof palette source
    # for external tooling (the visual block editor). Reflects whatever
    # is registered in NodeFactory at call time.
    export_schema,

    # Engine
    GraphEngine,
    RunConfig,
    RunResult,
    UsageAccumulator,
    CheckpointPhase,
    Checkpoint,
    PendingWrite,
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

# SqliteCheckpointStore is present when the binding was built with
# -DNEOGRAPH_BUILD_SQLITE=ON (default ON; libsqlite3 is a tiny,
# ubiquitous system library). Durable, single-file, cross-process
# checkpoint store — survives between separate process invocations
# sharing the same DB path, with no DB server to provision.
try:
    from ._neograph import SqliteCheckpointStore  # noqa: F401
    _HAVE_SQLITE = True
except ImportError:
    _HAVE_SQLITE = False

# A2A (Agent-to-Agent protocol) — present when the binding was built
# with neograph::a2a (the default for v0.2.1+). The submodule wraps
# A2AClient + AgentCard + Task/Message/Part/TaskState/Role and is
# reachable as `neograph_engine.a2a.A2AClient(...)`.
try:
    from ._neograph import a2a  # noqa: F401
    _HAVE_A2A = True
except ImportError:
    _HAVE_A2A = False

# MCP (Model Context Protocol) — present when the binding was built with
# neograph::mcp, which the wheel now does (issue #95). Reachable as
# `neograph_engine.mcp.MCPClient(...)`. get_tools() hands back C++ tools that go
# straight into NodeContext(tools=[...]) and keep their concurrency there.
try:
    from ._neograph import mcp  # noqa: F401
    _HAVE_MCP = True
except ImportError:
    _HAVE_MCP = False

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


class AsyncTool(Tool):
    """A tool that may run concurrently with its siblings (issue #96).

    When the model asks for several tools in one turn, NeoGraph dispatches them
    together. Whether they actually *overlap* is up to the tool:

    - Subclass :class:`Tool` and yours will not. It runs to completion before
      the next one starts, exactly as Python tools always have. An existing
      tool that keeps state cannot suddenly find itself racing a copy of
      itself — that guarantee is why concurrency is opt-in rather than default.

    - Subclass ``AsyncTool`` and yours will. Write the same ``execute()``; the
      binding runs it on a worker thread so it can overlap with its siblings.

    ::

        class Fetch(neograph_engine.AsyncTool):
            def get_definition(self):
                d = neograph_engine.ChatTool()
                d.name = "fetch"
                d.description = "Fetch a URL"
                return d

            def execute(self, arguments):
                return requests.get(arguments["url"]).text   # releases the GIL

            def get_name(self):
                return "fetch"

    **What the speedup depends on, plainly.** A Python function holds the GIL
    while it runs. Your tool overlaps with its siblings only while it is *not*
    holding it — that is, while it is blocked on I/O, which is when CPython
    lets go. An HTTP call, a socket read, a database query, ``time.sleep``: all
    release it, and all overlap.

    A tool that burns CPU in Python holds the GIL for its whole body and will
    **not** overlap, no matter how many threads it is handed. Declaring such a
    tool ``AsyncTool`` buys nothing. (If the heavy work happens inside numpy, a
    C extension, or a subprocess, the GIL is released there and it does
    overlap.)

    **Thread safety is now yours.** Two calls to the same AsyncTool can be in
    flight at once — the model is free to ask for the same tool twice in one
    turn. Keep per-call state on the stack, not on ``self``.

    Concurrency is bounded by an internal worker pool (32 threads by default;
    set ``NEOGRAPH_TOOL_THREADS`` to change it). These threads spend their time
    blocked on I/O, so a generous pool costs little.
    """


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
        # Let the property setter keep one replaceable Python reference. Passing
        # provider to the C++ constructor would install a keep_alive edge that
        # cannot be removed when the property is reassigned.
        super().__init__(
            provider=None,
            model=model,
            instructions=instructions,
            extra_config=extra_config or {},
        )
        self.provider = provider
        # Stash the Python tool list on the wrapper. compile() reads
        # it via py::hasattr / py::object.attr("_pytools").
        self._pytools = list(tools or [])


class RateLimitError(RuntimeError):
    """Raise this from a Provider that hit a rate limit (issue #97).

    ``RateLimitedProvider`` catches it, honours ``retry_after_seconds`` if the
    upstream told you one, and retries **the call**. Anything else you raise
    propagates as an error, as it should.

    ::

        class MyProvider(neograph_engine.Provider):
            def complete(self, params):
                response = requests.post(...)
                if response.status_code == 429:
                    raise neograph_engine.RateLimitError(
                        "rate limited",
                        retry_after_seconds=int(response.headers.get("Retry-After", -1)))
                ...

    The binding translates this into the C++ ``RateLimitError`` at the boundary.
    Without that translation the wrapper would sail straight past a Python
    provider's 429 and never retry — a capability that exists and quietly does
    nothing is worse than one that is plainly absent.
    """

    def __init__(self, message="rate limited", retry_after_seconds=-1):
        super().__init__(message)
        #: What the upstream asked you to wait, or -1 if it did not say.
        self.retry_after_seconds = retry_after_seconds


class NodeInterrupt(Exception):
    """Pause the graph from inside a node, resumably (issue #94).

    Raise this when the node has decided — from what it is actually holding,
    which is something no graph definition can know in advance — that a human
    has to look before it goes on::

        class ApprovalNode(neograph_engine.GraphNode):
            def run(self, input):
                answer = input.ctx.resume_value
                if answer is None:
                    raise neograph_engine.NodeInterrupt(
                        {"tool": "shell", "cmd": "rm -rf build/"},
                        reason="shell command needs approval")
                if not answer.get("approved"):
                    return [neograph_engine.ChannelWrite("result", "refused")]
                ...

    The engine catches it, checkpoints, and hands the caller a ``RunResult``
    with ``interrupted=True``, ``interrupt_node`` naming this node, and
    ``interrupt_value`` carrying ``{"reason": ..., "value": ...}``. The caller
    answers with ``engine.resume(thread_id, {"approved": True})``, and the node
    runs again — this time with the answer on ``input.ctx.resume_value``.

    This is the dynamic counterpart to ``interrupt_before`` / ``interrupt_after``
    in the graph definition, which pause at a node picked when the graph was
    written.

    A plain reason works too, and is what the C++ examples have always shown::

        raise neograph_engine.NodeInterrupt("needs human approval")

    Anything raised that is *not* a NodeInterrupt stays an error: a bug in a
    node must fail the run loudly, not masquerade as a question for a human.
    """

    def __init__(self, value=None, reason=None):
        # One positional argument covers both shapes. A string is the reason
        # (the C++ one-argument constructor); anything else is the payload,
        # and the reason falls back to its JSON form so logs and checkpoints
        # still have something human-readable to show.
        if isinstance(value, str) and reason is None:
            self.reason = value
            self.value = None
        else:
            self.value = value
            if reason is not None:
                self.reason = reason
            elif value is None:
                self.reason = ""
            else:
                self.reason = _json.dumps(value, ensure_ascii=False,
                                          separators=(",", ":"))
        super().__init__(self.reason)


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
        # If the subclass only defines a streaming variant, the user
        # almost certainly meant to drive the graph via run_stream() /
        # run_stream_async() instead. Detect that and surface the hint
        # so the error message points at the actual fix rather than at
        # the missing override (TODO_v0.3.md item #2).
        hint = ""
        cls = type(self)
        for stream_method in ("execute_full_stream", "execute_stream"):
            base_attr = getattr(GraphNode, stream_method, None)
            sub_attr  = getattr(cls,        stream_method, None)
            if sub_attr is not None and sub_attr is not base_attr:
                hint = (f" (this node defines {stream_method}() — call "
                        f"engine.run_stream() / run_stream_async() instead "
                        f"of run() / run_async() so the streaming variant "
                        f"is dispatched.)")
                break
        raise NotImplementedError(
            "GraphNode subclasses must override execute() to return a "
            "list of ChannelWrite, OR override execute_full() to return "
            "a NodeResult with Command/Send." + hint)


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

            # v0.4: use the unified run() override. The decorated user
            # function still takes ``state`` directly (the simplest sugar
            # signature); we adapt by passing input.state to it.
            def run(self, input):
                return fn(input.state) or []

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
    "NodeInput",
    "RunContext",
    "CancelToken",
    "GraphState",
    "GraphNode",
    "AsyncTool",
    "NodeInterrupt",
    "RateLimitError",
    "Store",
    "InMemoryStore",
    "StoreItem",
    "validate",
    "ValidationReport",
    "Diagnostic",
    "RateLimitError",
    "ToolDecision",
    "ToolGateContext",
    "NodeFactory",
    "ReducerRegistry",
    "ConditionRegistry",
    "export_schema",
    "node",
    "GraphEngine",
    "RunConfig",
    "UsageAccumulator",
    "RunResult",
    "CheckpointPhase",
    "Checkpoint",
    "PendingWrite",
    "CheckpointStore",
    "InMemoryCheckpointStore",
    # Postgres/SqliteCheckpointStore appended dynamically below when present.
    "StreamMode",
    "GraphEvent",
    "START_NODE",
    "END_NODE",
]

if _HAVE_POSTGRES:
    __all__.append("PostgresCheckpointStore")

if _HAVE_SQLITE:
    __all__.append("SqliteCheckpointStore")

# Streaming helpers — pure-Python, no C++ side. Re-exported here so a
# `from neograph_engine import message_stream` import works alongside
# the more explicit `from neograph_engine.streaming import message_stream`.
from .streaming import message_stream  # noqa: E402
__all__.append("message_stream")


# StateView — flat dot-access wrapper around engine.get_state(). Pydantic
# v2 backed; the base class allows arbitrary channel names so it works
# without a user-declared model. Subclass for typed access. (TODO_v0.3.md
# item #6 / v0.3.2.)
from .state_view import StateView  # noqa: E402
__all__.append("StateView")

# Protocol hosting bridge. A2A and ACP transports remain owned by their
# official Python SDKs; this adapter preserves NeoGraph session resume and
# cancellation semantics inside those SDK callbacks.
from .protocol import (  # noqa: E402
    ProtocolHostAdapter,
    ProtocolStreamEvent,
    last_message_text,
    message_input,
)
__all__.extend([
    "ProtocolHostAdapter",
    "ProtocolStreamEvent",
    "last_message_text",
    "message_input",
])


def _engine_get_state_view(self, thread_id, model=None):
    """Flat dot-access wrapper around ``self.get_state(thread_id)``.

    Args:
        thread_id: Thread / session id whose state to read.
        model:     Optional ``StateView`` subclass with declared fields
                   for typed access. Defaults to the base ``StateView``,
                   which allows any channel name via Pydantic
                   ``extra="allow"``.

    Returns:
        ``StateView`` instance (or the user's ``model``), or ``None`` if
        no checkpoint exists for ``thread_id``.

    Example:
        >>> view = engine.get_state_view("t1")
        >>> view.messages          # → list, no nested ['channels']['x']['value']
        >>> view.raw['global_version']  # the unflattened dict for metadata
        >>>
        >>> class ChatState(ng.StateView):
        ...     messages: list[dict] = []
        >>> typed = engine.get_state_view("t1", model=ChatState)
        >>> typed.messages         # → list[dict], pydantic-validated
    """
    state = self.get_state(thread_id)
    if state is None:
        return None
    cls = model if model is not None else StateView
    return cls.from_state(state)


# Pybind classes accept attribute assignment on the *class* object even
# when they're not built with py::dynamic_attr — adding a method is just
# setting a callable on the type. Instance-level dynamic attrs would be
# rejected, but we're not doing that here. This way the natural shape
# `engine.get_state_view(...)` works out of the box without a free
# function or a wrapper subclass.
GraphEngine.get_state_view = _engine_get_state_view  # type: ignore[attr-defined]
