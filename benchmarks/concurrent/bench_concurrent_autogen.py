#!/usr/bin/env python3
"""Concurrent-load bench for AutoGen GraphFlow.

AutoGen GraphFlow keeps per-run message context on the participants;
sharing one flow across concurrent coroutines would interleave state, and
run_stream rejects concurrent use of one team. Each concurrent call therefore
constructs a fresh flow (the construction is inside the timed call, because
the framework requires one team per concurrent run).

Counter is encoded in message content (AutoGen is message-passing, not
state-channel), same as the engine-overhead comparison.

Record schema 2, see burst_common.py.
"""

from __future__ import annotations

import asyncio
import os

from autogen_agentchat.agents import BaseChatAgent
from autogen_agentchat.base import Response
from autogen_agentchat.messages import TextMessage
from autogen_agentchat.teams import DiGraphBuilder, GraphFlow

import burst_common as bc

RSS_AFTER_IMPORT_KB = bc.vm_kb("VmRSS")


class IncAgent(BaseChatAgent):
    @property
    def produced_message_types(self):
        return (TextMessage,)

    async def on_messages(self, messages, cancellation_token):
        bc.inject_failure("autogen")
        last = None
        for m in reversed(messages):
            if isinstance(m, TextMessage):
                last = m.content
                break
        n = int(last) + 1 if last and last.lstrip("-").isdigit() else 1
        return Response(chat_message=TextMessage(source=self.name, content=str(n)))

    async def on_reset(self, cancellation_token):
        pass


def build_seq() -> GraphFlow:
    a = IncAgent(name="a", description="inc")
    b = IncAgent(name="b", description="inc")
    c = IncAgent(name="c", description="inc")
    builder = DiGraphBuilder()
    builder.add_node(a).add_node(b).add_node(c)
    builder.add_edge(a, b).add_edge(b, c)
    builder.set_entry_point(a)
    return GraphFlow(participants=[a, b, c], graph=builder.build())


def check(result) -> None:
    # The last message is the output of agent c: the counter as text.
    bc.check_equal("autogen last message", result.messages[-1].content, str(bc.EXPECTED))


async def run_chain():
    return await build_seq().run(task="0")


# ── async mode ─────────────────────────────────────────────────────────

def run_async(concurrency: int) -> dict:
    async def _warm():
        warm = build_seq()
        for _ in range(10):
            await warm.reset()
            check(await warm.run(task="0"))
    failed = bc.guard_warmup("autogen", "asyncio", concurrency,
                             lambda: asyncio.run(_warm()), RSS_AFTER_IMPORT_KB)
    if failed:
        return failed
    rss_before = bc.vm_kb("VmRSS")

    collector, wall_ns = asyncio.run(bc.burst_async(concurrency, run_chain, check))
    return bc.build_record(
        "autogen", "asyncio", concurrency, collector, wall_ns,
        note="a fresh GraphFlow per call (required: one team cannot run concurrently); construction is in the timed call",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before)


# ── multiprocessing mode ───────────────────────────────────────────────

def _mp_init():
    check(asyncio.run(run_chain()))  # first-call cost off the clock


def _mp_invoke(_: int) -> tuple:
    # A new event loop and a fresh flow per call.
    return bc.timed_call(lambda: asyncio.run(run_chain()), check)


def run_mp(concurrency: int) -> dict:
    pool_size = os.cpu_count() or 1
    rss_before = bc.vm_kb("VmRSS")
    collector, wall_ns, info = bc.burst_mp(concurrency, pool_size, _mp_invoke, _mp_init)
    return bc.build_record(
        "autogen", f"mp-pool-{pool_size}", concurrency, collector, wall_ns,
        note="a fresh GraphFlow and a new event loop per call in each pool worker; pool started before the clock",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before, mp_info=info)


def main() -> None:
    mode, concurrency = bc.parse_args("bench_concurrent_autogen.py")
    bc.finish(run_async(concurrency) if mode == "async" else run_mp(concurrency))


if __name__ == "__main__":
    main()
