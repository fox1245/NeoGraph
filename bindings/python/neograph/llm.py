"""LLM provider implementations — OpenAI-compatible + multi-vendor schema-driven.

Mirrors the C++ ``neograph::llm`` namespace. Both providers are concrete
subclasses of :class:`neograph.Provider` and can be passed straight to
:class:`neograph.NodeContext`.
"""

from ._neograph import OpenAIProvider, SchemaProvider

__all__ = ["OpenAIProvider", "SchemaProvider"]
