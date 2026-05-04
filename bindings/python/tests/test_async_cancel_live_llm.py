"""Live-LLM E2E test for run_stream_async cancellation.

Verifies the v0.3 full cancel propagation: a frontend AbortController
(or any asyncio task cancel) tears down the in-flight OpenAI HTTPS
request at the socket layer, so the LLM stops billing immediately —
the cost-leak gap reported in v0.2.3 is closed.

Two things this asserts that no mock-only test can:

  1. ``InvalidStateError`` does not leak from
     ``loop.call_soon_threadsafe(future.set_result, …)`` when the
     future has already been cancelled (the safe-resolve helper guard
     introduced alongside this fix).

  2. The Python node's ``execute()`` either finishes within the cancel
     deadline or, more commonly, is torn down before completion via
     asio's cancellation_signal propagating through the engine →
     Provider::complete → run_sync → ConnPool::async_post → socket op.
     We measure the wall-clock between cancel and worker completion;
     pre-v0.3 the gap was ~5–8 s of uncancelled OpenAI streaming.

Skipped in CI: requires OPENAI_API_KEY in env or .env. Set
NEOGRAPH_LIVE_LLM=1 to actually run.
"""

from __future__ import annotations

import asyncio
import os
import time

import pytest

import neograph_engine as ng


def _have_live_llm() -> bool:
    return bool(os.getenv("OPENAI_API_KEY")) and os.getenv(
        "NEOGRAPH_LIVE_LLM", "") == "1"


pytestmark = pytest.mark.skipif(
    not _have_live_llm(),
    reason="set NEOGRAPH_LIVE_LLM=1 + OPENAI_API_KEY to run live tests")


def _build_engine():
    """Single-node graph that calls a real OpenAI completion.

    OpenAIProvider in the binding exposes only the blocking ``complete``
    surface — that's enough to demonstrate the cost-leak shape: the node
    sits in ``complete()`` for ~3-6 seconds while OpenAI generates 400
    tokens. The asyncio task gets cancelled mid-call; the HTTP socket
    is NOT torn down (no cancel propagation), so OpenAI streams the
    full response and bills for it.
    """
    from neograph_engine.llm import OpenAIProvider

    marker: dict = {"started_at": 0.0, "finished_at": 0.0}

    class LLMNode(ng.GraphNode):
        def __init__(self, name, ctx):
            super().__init__()
            self._name = name
            self._ctx = ctx

        def get_name(self):
            return self._name

        def execute(self, state):
            marker["started_at"] = time.time()
            params = ng.CompletionParams()
            params.model = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
            params.temperature = 0.7
            params.max_tokens = 400

            params.messages = [
                ng.ChatMessage(
                    role="user",
                    content=(
                        "Write a 300-word essay about the history of "
                        "the printing press. Be detailed."
                    ),
                ),
            ]
            result = self._ctx.provider.complete(params)
            marker["finished_at"] = time.time()
            content = result.message.content if result.message else ""
            return [ng.ChannelWrite("reply", content)]

    provider = OpenAIProvider(
        api_key=os.environ["OPENAI_API_KEY"],
        base_url=os.getenv("OPENAI_API_BASE", "https://api.openai.com"),
        default_model=os.getenv("OPENAI_MODEL", "gpt-4o-mini"),
    )
    ctx = ng.NodeContext(provider=provider)
    ng.NodeFactory.register_type(
        "_test_live_llm",
        lambda name, c, _ctx: LLMNode(name, _ctx))

    definition = {
        "name": "live_llm_cancel",
        "channels": {"reply": {"reducer": "overwrite"}},
        "nodes": {"llm": {"type": "_test_live_llm"}},
        "edges": [
            {"from": ng.START_NODE, "to": "llm"},
            {"from": "llm", "to": ng.END_NODE},
        ],
    }
    engine = ng.GraphEngine.compile(definition, ctx)
    return engine, marker


def test_live_run_async_cancel_no_invalid_state():
    """Cancel a real OpenAI complete() run; assert no InvalidStateError."""
    engine, marker = _build_engine()

    async def go():
        loop = asyncio.get_running_loop()
        captured: list[dict] = []
        loop.set_exception_handler(lambda lp, ctx: captured.append(ctx))

        cfg = ng.RunConfig(thread_id="live-cancel", input={})
        fut = engine.run_async(cfg)

        # Wait for the node to enter complete() — verifies the HTTP
        # request is in flight when we cancel.
        deadline = time.time() + 5
        while marker["started_at"] == 0.0 and time.time() < deadline:
            await asyncio.sleep(0.05)
        assert marker["started_at"] > 0, "node never entered complete()"

        # Hold briefly so the OpenAI response has begun streaming.
        await asyncio.sleep(0.5)

        cancel_t = time.time()
        fut.cancel()
        try:
            await fut
        except asyncio.CancelledError:
            pass

        # Wait up to 20 s for the worker to either finish (cost-leak)
        # or be torn down (the deeper fix path).
        worker_deadline = cancel_t + 20
        while marker["finished_at"] == 0.0 and time.time() < worker_deadline:
            await asyncio.sleep(0.2)

        return {
            "captured": captured,
            "started_at": marker["started_at"],
            "finished_at": marker["finished_at"],
            "cancel_at": cancel_t,
        }

    out = asyncio.run(go())

    invalid_state = [
        c for c in out["captured"]
        if "InvalidStateError" in repr(c.get("exception"))
        or "invalid state" in str(c.get("message", "")).lower()
    ]
    assert not invalid_state, (
        f"InvalidStateError leaked through despite the helper guard: "
        f"{invalid_state}")

    print(
        f"\n[live-cancel] cancel_at={out['cancel_at']:.2f}  "
        f"finished_at={out['finished_at']:.2f}  "
        f"delta={(out['finished_at'] - out['cancel_at']):.2f}s after cancel")

    # v0.3 cancel propagation guarantee: either the worker was torn
    # down (finished_at == 0 — the typical asio operation_aborted path)
    # OR it raced to completion within ~1 second of cancel (an in-
    # flight HTTP response that already had bytes on the wire). More
    # than ~3 s of post-cancel work means cancel propagation broke.
    if out["finished_at"] > 0:
        leak_seconds = out["finished_at"] - out["cancel_at"]
        assert leak_seconds < 3.0, (
            f"cost-leak regression: OpenAI request ran for "
            f"{leak_seconds:.2f}s AFTER cancel. The v0.3 cancel "
            f"propagation chain (asyncio cancel → CancelToken → asio "
            f"cancellation_signal → ConnPool socket abort) appears "
            f"broken. Pre-v0.3 baseline was ~7s of post-cancel "
            f"streaming.")
