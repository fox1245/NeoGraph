"""NeoGraph — C++ graph agent engine, Python bindings.

This package re-exports the symbols of the underlying ``_neograph`` C
extension. The C extension is what links against ``libneograph_*.so``;
this file is the stable Python import surface so editor tooling and
``help(neograph)`` see something more useful than ``<module>``.

Example:
    >>> import neograph
    >>> from neograph.llm import SchemaProvider
    >>> provider = SchemaProvider(schema_path="openai", api_key="sk-...")
    >>> ctx = neograph.NodeContext(provider=provider)
    >>> engine = neograph.GraphEngine.compile(my_graph_dict, ctx)
    >>> result = engine.run({"thread_id": "demo", "input": {...}})

The first commit of the binding focuses on a sync surface — `engine.run()`,
`engine.run_stream(cb)`, `engine.get_state(thread_id)`. Async (`run_async`,
`run_stream_async`) and Python-native custom nodes ship in commit 2.
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

# `from neograph.llm import OpenAIProvider, SchemaProvider` — defined in
# llm.py so consumers can `from neograph.llm import ...` mirroring the
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
    "GraphEngine",
    "RunConfig",
    "RunResult",
    "StreamMode",
    "GraphEvent",
    "START_NODE",
    "END_NODE",
]
