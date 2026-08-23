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
import urllib.parse
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


def _configure_console() -> None:
    """Keep model responses printable on Windows' legacy console codecs."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")


_configure_console()
_load_env()


def _require_key() -> str:
    key = os.getenv("OPENAI_API_KEY")
    if not key:
        print("OPENAI_API_KEY not set in environment or .env file.")
        print("Skipping the live LLM call.")
        print("  echo 'OPENAI_API_KEY=sk-...' > .env")
        sys.exit(0)
    return key


def openai_provider(default_model: str = "gpt-5.6-luna") -> OpenAIProvider:
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
    default_model: str = "gpt-5.6-luna",
    *,
    use_websocket: bool = False,
    prefer_libcurl: bool | None = None,
) -> SchemaProvider:
    """Schema-driven provider — the right pick for OpenAI Responses
    or vendor-specific shapes (Claude, Gemini).

    Honours `OPENAI_API_BASE` for routing to OpenAI-compatible endpoints
    (Groq, vLLM, llama.cpp server, etc.). The schema's
    `connection.base_url` is used when the env var is empty.
    """
    if prefer_libcurl is None:
        prefer_libcurl = os.getenv("NG_PREFER_LIBCURL", "0") == "1"
    return SchemaProvider(
        schema_path=schema,
        api_key=_require_key(),
        default_model=os.getenv("OPENAI_MODEL", default_model),
        base_url_override=os.getenv("OPENAI_API_BASE", ""),
        use_websocket=use_websocket,
        prefer_libcurl=prefer_libcurl,
    )


def responses_transport() -> str:
    """Select ``websocket``, ``http2``, or ``http1`` for Responses demos.

    Official OpenAI defaults to WebSocket. OpenAI-compatible gateways default
    to HTTP/2 because many implement ``POST /v1/responses`` but not the
    WebSocket upgrade. Set ``NG_RESPONSES_TRANSPORT`` to override the choice.
    """
    explicit = os.getenv("NG_RESPONSES_TRANSPORT", "").strip().lower()
    if explicit:
        if explicit not in {"websocket", "http2", "http1"}:
            raise ValueError(
                "NG_RESPONSES_TRANSPORT must be websocket, http2, or http1")
        return explicit

    base_url = os.getenv("OPENAI_API_BASE", "").strip()
    if not base_url:
        return "websocket"
    hostname = (urllib.parse.urlsplit(base_url).hostname or "").lower()
    if hostname == "api.openai.com":
        return "websocket"
    return "http2" if getattr(ng, "_HAVE_LIBCURL", False) else "http1"


def complete_responses(provider, params, transport: str):
    """Run one bounded Responses call over the selected demo transport."""
    if params.timeout_seconds <= 0:
        params.timeout_seconds = int(
            os.getenv("NG_EXAMPLE_TIMEOUT_SECONDS", "180"))
    if params.max_tokens <= 0:
        params.max_tokens = int(os.getenv("NG_EXAMPLE_MAX_TOKENS", "1600"))

    def invoke():
        if transport == "websocket":
            return provider.complete_stream(params, lambda _chunk: None)
        return provider.complete(params)

    result = invoke()
    if result.message.content.strip() or result.message.tool_calls:
        return result

    # Reasoning models can spend the entire first allowance internally and
    # return a successful but empty assistant message. One larger retry makes
    # the interactive examples useful while keeping their first call bounded.
    params.max_tokens = max(
        params.max_tokens,
        int(os.getenv("NG_EXAMPLE_RETRY_MAX_TOKENS", "4096")),
    )
    params.timeout_seconds = max(params.timeout_seconds, 300)
    result = invoke()
    if not result.message.content.strip() and not result.message.tool_calls:
        raise RuntimeError(
            "Responses API returned an empty assistant message after retry")
    return result


__all__ = [
    "ng",
    "complete_responses",
    "openai_provider",
    "responses_transport",
    "schema_provider",
]
