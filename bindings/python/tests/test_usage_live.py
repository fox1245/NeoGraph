"""Live: the token count NeoGraph reports is the one the provider billed (#88).

Gated on GROQ_API_KEY / ANTHROPIC_API_KEY; skipped without them, like the other
live tests in this directory.

The stub tests prove the plumbing carries a number. They cannot prove it is the
provider's number. Ground truth here is the `usage` block of the raw HTTP
response, fetched independently — comparing NeoGraph against NeoGraph would
prove nothing.

`prompt_tokens` is the assertion with teeth: same input, same count, every time.
`completion_tokens` depends on what the model chose to say, so it is only checked
for being non-zero.

The streaming case is the one that earns its keep. OpenAI-compatible APIs omit
usage from a streamed response unless `stream_options: {include_usage: true}` is
set; the providers set it, and if someone ever drops that line every streamed run
starts reporting zero tokens — silently, because a zero looks like a cheap run
rather than a broken counter.
"""

import json
import os
import urllib.request

import pytest

import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider

PROMPT = "Reply with exactly one word: ok"
MESSAGES = [{"role": "user", "content": PROMPT}]

GROQ_MODEL = os.getenv("NEOGRAPH_TEST_GROQ_MODEL", "llama-3.1-8b-instant")
GROQ_BASE = "https://api.groq.com/openai"
ANTHROPIC_MODEL = os.getenv("NEOGRAPH_TEST_ANTHROPIC_MODEL", "claude-haiku-4-5-20251001")

# Groq's edge rejects Python-urllib's default User-Agent with a 403.
_UA = {"User-Agent": "neograph-test/1.0"}


def _post(url, body, headers):
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                 headers={**headers, **_UA,
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def _graph_usage(provider, stream=False):
    definition = {
        "name": "live_usage",
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {"llm": {"type": "llm_call"}},
        "edges": [
            {"from": "__start__", "to": "llm"},
            {"from": "llm", "to": "__end__"},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ng.NodeContext(provider=provider))

    cfg = ng.RunConfig()
    cfg.thread_id = "live-stream" if stream else "live"
    cfg.input = {"messages": MESSAGES}

    result = engine.run_stream(cfg, lambda _e: None) if stream else engine.run(cfg)
    return result.usage


@pytest.mark.parametrize("stream", [False, True], ids=["non_streaming", "streaming"])
def test_groq_usage_matches_the_api(stream):
    key = os.getenv("GROQ_API_KEY")
    if not key:
        pytest.skip("GROQ_API_KEY unset")

    truth = _post(f"{GROQ_BASE}/v1/chat/completions",
                  {"model": GROQ_MODEL, "messages": MESSAGES,
                   "temperature": 0, "max_tokens": 16},
                  {"Authorization": f"Bearer {key}"})["usage"]

    provider = OpenAIProvider(api_key=key, base_url=GROQ_BASE, default_model=GROQ_MODEL)
    usage = _graph_usage(provider, stream=stream)

    assert usage.prompt_tokens == truth["prompt_tokens"]
    assert usage.completion_tokens > 0, "streamed runs report zero when include_usage is lost"
    assert usage.total_tokens == usage.prompt_tokens + usage.completion_tokens


def test_anthropic_usage_matches_the_api():
    """A different wire format entirely: usage.input_tokens / output_tokens.

    SchemaProvider parses it through a separate path from the OpenAI shape, so
    the OpenAI-compatible test above says nothing about this one.
    """
    key = os.getenv("ANTHROPIC_API_KEY")
    if not key:
        pytest.skip("ANTHROPIC_API_KEY unset")

    truth = _post("https://api.anthropic.com/v1/messages",
                  {"model": ANTHROPIC_MODEL, "messages": MESSAGES, "max_tokens": 16},
                  {"x-api-key": key, "anthropic-version": "2023-06-01"})["usage"]

    provider = SchemaProvider("schemas/claude.json", api_key=key,
                              default_model=ANTHROPIC_MODEL)
    usage = _graph_usage(provider)

    assert usage.prompt_tokens == truth["input_tokens"]
    assert usage.completion_tokens > 0
