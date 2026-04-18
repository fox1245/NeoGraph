#!/usr/bin/env python3
"""Engine-overhead benchmark for AutoGen GraphFlow.

Mirrors bench_neograph.cpp / bench_langgraph.py:

  * seq  — 3-agent chain, each increments a counter encoded in the
           text message content (AutoGen passes text messages between
           agents, not typed state channels).
  * par  — dispatch → 5 workers → summarizer. Each worker emits its
           index; summarizer counts worker messages it received.

AutoGen is message-passing / conversational, not state-channel / reducer
based. We encode the counter as message content to force-fit the
LangGraph-style workload, which is the closest apples-to-apples we can
get. Columns match the other benches: workload, iters, total_ms,
per_iter_us.

The GraphFlow instance is built once and reused; flow.reset() is called
between runs (this is the idiomatic pattern).
"""

from __future__ import annotations

import asyncio
import sys
import time

from autogen_agentchat.agents import BaseChatAgent
from autogen_agentchat.base import Response
from autogen_agentchat.messages import TextMessage
from autogen_agentchat.teams import DiGraphBuilder, GraphFlow


# ── Agents ─────────────────────────────────────────────────────────────

class IncAgent(BaseChatAgent):
    @property
    def produced_message_types(self):
        return (TextMessage,)

    async def on_messages(self, messages, cancellation_token):
        last = None
        for m in reversed(messages):
            if isinstance(m, TextMessage):
                last = m.content
                break
        n = int(last) + 1 if last and last.lstrip("-").isdigit() else 1
        return Response(chat_message=TextMessage(source=self.name, content=str(n)))

    async def on_reset(self, cancellation_token):
        pass


class Dispatch(BaseChatAgent):
    @property
    def produced_message_types(self):
        return (TextMessage,)

    async def on_messages(self, messages, cancellation_token):
        return Response(chat_message=TextMessage(source=self.name, content="go"))

    async def on_reset(self, cancellation_token):
        pass


class WorkerAgent(BaseChatAgent):
    def __init__(self, name: str, idx: int):
        super().__init__(name=name, description=f"worker{idx}")
        self.idx = idx

    @property
    def produced_message_types(self):
        return (TextMessage,)

    async def on_messages(self, messages, cancellation_token):
        return Response(
            chat_message=TextMessage(source=self.name, content=str(self.idx))
        )

    async def on_reset(self, cancellation_token):
        pass


class SummAgent(BaseChatAgent):
    @property
    def produced_message_types(self):
        return (TextMessage,)

    async def on_messages(self, messages, cancellation_token):
        count = sum(
            1
            for m in messages
            if isinstance(m, TextMessage) and m.source.startswith("w")
        )
        return Response(chat_message=TextMessage(source=self.name, content=str(count)))

    async def on_reset(self, cancellation_token):
        pass


# ── Builders ───────────────────────────────────────────────────────────

def build_seq() -> GraphFlow:
    a = IncAgent(name="a", description="inc")
    b = IncAgent(name="b", description="inc")
    c = IncAgent(name="c", description="inc")
    builder = DiGraphBuilder()
    builder.add_node(a).add_node(b).add_node(c)
    builder.add_edge(a, b).add_edge(b, c)
    builder.set_entry_point(a)
    return GraphFlow(participants=[a, b, c], graph=builder.build())


def build_par() -> GraphFlow:
    d = Dispatch(name="d", description="disp")
    workers = [WorkerAgent(name=f"w{i}", idx=i) for i in range(1, 6)]
    s = SummAgent(name="s", description="summ")
    builder = DiGraphBuilder()
    builder.add_node(d).add_node(s)
    for w in workers:
        builder.add_node(w)
        builder.add_edge(d, w)
        builder.add_edge(w, s)
    builder.set_entry_point(d)
    return GraphFlow(participants=[d, *workers, s], graph=builder.build())


# ── Bench harness ──────────────────────────────────────────────────────

async def bench(flow: GraphFlow, task: str, iters: int) -> tuple[int, float]:
    t0 = time.perf_counter()
    for _ in range(iters):
        await flow.reset()
        await flow.run(task=task)
    total_s = time.perf_counter() - t0
    return int(total_s * 1000), (total_s * 1_000_000) / iters


async def main() -> None:
    seq_iters = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    par_iters = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    # Warm-up
    warm = build_seq()
    for _ in range(10):
        await warm.reset()
        await warm.run(task="0")

    seq_flow = build_seq()
    seq_total, seq_per = await bench(seq_flow, "0", seq_iters)
    print(f"seq\t{seq_iters}\t{seq_total}\t{seq_per:.2f}")

    par_flow = build_par()
    par_total, par_per = await bench(par_flow, "start", par_iters)
    print(f"par\t{par_iters}\t{par_total}\t{par_per:.2f}")


if __name__ == "__main__":
    asyncio.run(main())
