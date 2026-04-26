"""Helpers shared across the example scripts.

Specifically: load .env from the script's directory tree, build a
configured OpenAIProvider / SchemaProvider, and gracefully skip when
no API key is set so CI / read-only checkouts don't crash.

Usage::

    from _common import openai_provider
    provider = openai_provider()  # exits cleanly if no key
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider

try:
    from dotenv import load_dotenv
except ImportError:  # pragma: no cover
    print("python-dotenv is required to run the LLM examples:")
    print("    pip install python-dotenv")
    print("(or `pip install neograph-engine[examples]` once that extra ships)")
    sys.exit(1)


def _load_env() -> None:
    """Load .env from the cwd or any parent — same lookup the C++
    examples use via cppdotenv.
    """
    # Walk parents so running the example from anywhere finds the
    # .env in bindings/python/examples (or repo root).
    here = Path(__file__).resolve()
    for parent in [here.parent, *here.parents]:
        candidate = parent / ".env"
        if candidate.is_file():
            load_dotenv(candidate)
            return
    # No .env found — that's fine, env-vars set by the user still win.
    load_dotenv()


_load_env()


def _require_key() -> str:
    key = os.getenv("OPENAI_API_KEY")
    if not key:
        print("OPENAI_API_KEY not set in environment or .env file.")
        print("Skipping the live LLM call.")
        print("  echo 'OPENAI_API_KEY=sk-...' > .env")
        sys.exit(0)
    return key


def openai_provider(default_model: str = "gpt-4o-mini") -> OpenAIProvider:
    """OpenAI-compatible HTTP provider configured from the env.

    Honours:
      OPENAI_API_KEY   — required.
      OPENAI_API_BASE  — default https://api.openai.com.
      OPENAI_MODEL     — default `default_model`.
    """
    return OpenAIProvider(
        api_key=_require_key(),
        base_url=os.getenv("OPENAI_API_BASE", "https://api.openai.com"),
        default_model=os.getenv("OPENAI_MODEL", default_model),
    )


def schema_provider(
    schema: str = "openai_responses",
    default_model: str = "gpt-4o-mini",
    *,
    use_websocket: bool = False,
) -> SchemaProvider:
    """Schema-driven provider — the right pick for OpenAI Responses
    or vendor-specific shapes (Claude, Gemini).

    Honours `OPENAI_API_BASE` for routing to OpenAI-compatible endpoints
    (Groq, vLLM, llama.cpp server, etc.). The schema's
    `connection.base_url` is used when the env var is empty.
    """
    return SchemaProvider(
        schema_path=schema,
        api_key=_require_key(),
        default_model=os.getenv("OPENAI_MODEL", default_model),
        base_url_override=os.getenv("OPENAI_API_BASE", ""),
        use_websocket=use_websocket,
    )


__all__ = ["ng", "openai_provider", "schema_provider"]
