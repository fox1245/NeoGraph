"""Live-LLM E2E test for v0.3.1 multi-Send cancel propagation.

Sister test to test_async_cancel_live_llm.py — that one validates the
single-node path (v0.3.0). This one validates the multi-Send fan-out
path (v0.3.1): a dispatcher emits N Sends, each Send target makes a
real OpenAI call, then mid-flight we cancel the asyncio task. The
v0.3.1 fix forwards the parent's cancel_token onto each isolated
send_state — without it, all N workers would stream their full
responses to completion (the cost-leak the in-process test
MultiSendFanOutSeesParentToken caught with a synthetic worker).

This is the test that proves the fix at the socket layer with real
HTTPS + real billing. Skipped unless NEOGRAPH_LIVE_LLM=1 +
OPENAI_API_KEY set.
"""

from __future__ import annotations

import asyncio
import os
import threading
import time

import pytest

import neograph_engine as ng


def _have_live_llm() -> bool:
    return bool(os.getenv("OPENAI_API_KEY")) and os.getenv(
        "NEOGRAPH_LIVE_LLM", "") == "1"


pytestmark = pytest.mark.skipif(
    not _have_live_llm(),
    reason="set NEOGRAPH_LIVE_LLM=1 + OPENAI_API_KEY to run live tests")


# Each worker bumps started_at[index] when it enters complete() and
# finished_at[index] when complete() returns. Lock-protected because
# the engine fans them out across worker pool threads.
WIDTH = 3


def _build_engine():
    from neograph_engine.llm import OpenAIProvider

    started:  list[float] = [0.0] * WIDTH
    finished: list[float] = [0.0] * WIDTH
    lock = threading.Lock()

    class WorkerLLMNode(ng.GraphNode):
        """Receives a Send with input {"i": <branch index>}, calls
        OpenAI for ~3-6 s, records timing markers per branch."""

        def __init__(self, name, ctx):
            super().__init__()
            self._name = name
            self._ctx = ctx

        def get_name(self):
            return self._name

        def execute(self, state):
            i = state.get("i")
            if not isinstance(i, int):
                i = 0
            with lock:
                started[i] = time.time()

            params = ng.CompletionParams()
            params.model = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
            params.temperature = 0.7
            params.max_tokens = 400
            params.messages = [
                ng.ChatMessage(
                    role="user",
                    content=(
                        f"Write a 300-word essay about historical event #{i}. "
                        "Make it detailed and specific."
                    ),
                ),
            ]
            result = self._ctx.provider.complete(params)
            with lock:
                finished[i] = time.time()
            content = result.message.content if result.message else ""
            return [ng.ChannelWrite(f"reply_{i}", content)]

    class DispatcherNode(ng.GraphNode):
        """Emits WIDTH Sends to the worker, each with its branch index."""

        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def execute_full(self, state):
            sends = [ng.Send("worker", {"i": i}) for i in range(WIDTH)]
            return ng.NodeResult(sends=sends)

    provider = OpenAIProvider(
        api_key=os.environ["OPENAI_API_KEY"],
        base_url=os.getenv("OPENAI_API_BASE", "https://api.openai.com"),
        default_model=os.getenv("OPENAI_MODEL", "gpt-4o-mini"),
    )
    ctx = ng.NodeContext(provider=provider)

    ng.NodeFactory.register_type(
        "_live_fanout_dispatcher",
        lambda name, c, _ctx: DispatcherNode(name),
    )
    ng.NodeFactory.register_type(
        "_live_fanout_worker",
        lambda name, c, _ctx: WorkerLLMNode(name, _ctx),
    )

    channels = {f"reply_{i}": {"reducer": "overwrite"} for i in range(WIDTH)}
    channels["i"] = {"reducer": "overwrite"}

    definition = {
        "name": "live_llm_fanout_cancel",
        "channels": channels,
        "nodes": {
            "dispatcher": {"type": "_live_fanout_dispatcher"},
            "worker":     {"type": "_live_fanout_worker"},
        },
        "edges": [
            {"from": ng.START_NODE, "to": "dispatcher"},
            # worker has no outgoing edge — Send target → implicit __end__.
        ],
    }
    engine = ng.GraphEngine.compile(definition, ctx)
    return engine, started, finished


def test_live_send_fanout_cancel_aborts_all_branches():
    """Cancel an in-flight multi-Send fan-out; assert every branch
    either got torn down (finished_at == 0) or completed within ~3 s
    of the cancel (in-flight bytes already on the wire)."""
    engine, started, finished = _build_engine()

    async def go():
        loop = asyncio.get_running_loop()
        captured: list[dict] = []
        loop.set_exception_handler(lambda lp, ctx: captured.append(ctx))

        cfg = ng.RunConfig(thread_id="live-fanout-cancel", input={})
        fut = engine.run_async(cfg)

        # Wait until ALL WIDTH workers have entered complete().
        deadline = time.time() + 8
        while min(started) == 0.0 and time.time() < deadline:
            await asyncio.sleep(0.05)
        assert min(started) > 0, (
            f"not every worker entered complete() within 8 s — started={started}")

        # Hold so OpenAI has begun streaming on every branch.
        await asyncio.sleep(0.7)

        cancel_t = time.time()
        fut.cancel()
        try:
            await fut
        except asyncio.CancelledError:
            pass

        # Wait up to 20 s after cancel for any branch to either be
        # torn down or finish naturally.
        worker_deadline = cancel_t + 20
        while max(finished) == 0.0 and time.time() < worker_deadline:
            await asyncio.sleep(0.2)
        # Give stragglers another grace period.
        await asyncio.sleep(2.0)

        return {
            "captured":   captured,
            "started":    list(started),
            "finished":   list(finished),
            "cancel_at":  cancel_t,
        }

    out = asyncio.run(go())

    invalid_state = [
        c for c in out["captured"]
        if "InvalidStateError" in repr(c.get("exception"))
        or "invalid state" in str(c.get("message", "")).lower()
    ]
    assert not invalid_state, (
        f"InvalidStateError leaked through: {invalid_state}")

    cancel_t = out["cancel_at"]
    print(f"\n[live-fanout-cancel] cancel_at={cancel_t:.2f}")
    for i, (s, f) in enumerate(zip(out["started"], out["finished"])):
        delta = (f - cancel_t) if f > 0 else None
        print(f"  branch {i}: started=+{s - cancel_t:+.2f}s  "
              f"finished={'aborted' if f == 0 else f'+{delta:+.2f}s after cancel'}")

    # Critical assertion: every branch was either torn down (finished == 0)
    # or completed within 3 s of the cancel (in-flight bytes already on
    # the wire). Pre-v0.3.1 this would show 3-7 s of post-cancel
    # streaming on each branch — the cost-leak that MultiSendFanOutSeesParentToken
    # catches in-process. Live verification proves the asio
    # cancellation_signal really reaches each isolated Send worker's
    # HTTP socket.
    leaks = []
    for i, f in enumerate(out["finished"]):
        if f > 0:
            leak = f - cancel_t
            if leak >= 3.0:
                leaks.append((i, leak))
    assert not leaks, (
        f"cost-leak regression on Send fan-out: {leaks} — branches "
        f"streamed for >3 s AFTER cancel. The v0.3.1 multi-Send fix "
        f"(forwarding run_cancel_token onto isolated send_state) is "
        f"not reaching the socket layer.")
