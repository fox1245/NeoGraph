"""Scenario 8: a Program whose graph calls a model through OpenRouter.

Default (offline): the identical code path runs against a loopback Chat
Completions peer started by the test, so the wiring (descriptor, ZDR routing,
output cap, typed outcome, usage) is verified with no key and no paid call.

Live (opt-in): NEOGRAPH_E2E_LIVE=1 and OPENROUTER_API_KEY=... run the same
Programs against https://openrouter.ai. The key is read from the environment
only.

Two shapes are covered:

- a Python node that owns one provider call (the host keeps the provider;
  the Program's budgets do not see the call), with
  ~deepseek/deepseek-v4-flash-latest (override with OPENROUTER_MODEL);
- the built-in llm_call node bound to the provider through
  LocalProgramHost(node_context=..., binding_label=...) (#372): the call is
  priced against the run's monetary budget and settled from OpenRouter's
  reported cost. It needs a model whose catalog entry has a route ceiling,
  deepseek/deepseek-v4-flash-0731 by default (override with
  OPENROUTER_PRICED_MODEL); a router alias cannot be bounded and is refused.
"""

import os

import pytest

import neograph_engine as ng

from . import support
from .support import (ChatPeer, admit, budget, ceiling, define_graph, describe, digest,
                      make_bound_host, make_host, openrouter_provider, priced_budget,
                      route_ceiling_hold)

LIVE = os.environ.get("NEOGRAPH_E2E_LIVE") == "1" and bool(os.environ.get("OPENROUTER_API_KEY"))
MODEL = os.environ.get("OPENROUTER_MODEL", "~deepseek/deepseek-v4-flash-latest")
PRICED_MODEL = os.environ.get("OPENROUTER_PRICED_MODEL", support.PRICED_MODEL)
MAX_OUTPUT_TOKENS = support.OUTPUT_CAP

needs_llm = pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                               reason="neograph_engine was built without the LLM target")
live = pytest.mark.skipif(not LIVE, reason="live: set NEOGRAPH_E2E_LIVE=1 and OPENROUTER_API_KEY")


class ModelAnswer(ng.GraphNode):
    """Python node that owns one typed provider call and records its usage."""

    def __init__(self, name, provider, model, routing):
        super().__init__()
        self._name = name
        self._provider = provider
        self._model = model
        self._routing = routing

    def get_name(self):
        return self._name

    def run(self, input):
        controls = ng.ProviderControls()
        controls.max_output_tokens = MAX_OUTPUT_TOKENS
        controls.provider = self._routing
        messages = [
            ng.ProviderMessage(role=ng.ProviderRole.System,
                               parts=[ng.Text("Answer in one short sentence.")]),
            ng.ProviderMessage(role=ng.ProviderRole.User,
                               parts=[ng.Text(input.state.get("question") or "")]),
        ]
        request = ng.make_provider_request(self._provider, self._model, messages, controls=controls)
        request.cancel_token = input.ctx.cancel_token
        outcome = self._provider.invoke(request)
        if outcome.failure is not None:
            raise RuntimeError(f"provider failed: {outcome.failure.error.safe_message}")
        completion = outcome.completion
        usage = completion.usage

        def count(value):
            return None if value is None else value.value

        return [
            ng.ChannelWrite("reply", outcome.text),
            ng.ChannelWrite("usage", {"input": count(usage.input_total),
                                      "output": count(usage.output_total)}),
        ]


CHAT_PROGRAM = r"""
export function define() {
  return ng.graph("chat")
    .channel("question", {reducer: "overwrite", initial: ""})
    .channel("reply", {reducer: "overwrite", initial: ""})
    .channel("usage", {reducer: "overwrite", initial: null})
    .node("answer", {type: "e2e.model_answer"})
    .entry("answer")
    .exit("answer");
}

export function* main(input) {
  const turn = yield ng.callCore("chat", {question: input.question}, "chat:answer");
  return {text: turn.reply, usage: turn.usage};
}
"""

BOUND_CHAT_PROGRAM = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.node("llm", {type: "llm_call"});
    g.entry("llm");
    g.exit("llm");
""", name="chat")


def run_chat_program(provider, routing, question):
    ng.NodeFactory.register_type(
        "e2e.model_answer", lambda name, config, ctx: ModelAnswer(name, provider, MODEL, routing))
    builder = ng.ProgramRegistryBuilder()
    builder.add_registered_node("e2e.model_answer", "1.0.0", digest("node:e2e.model_answer"))
    builder.add_registered_reducer("overwrite", "1.0.0", digest("reducer:overwrite"))
    host = make_host("tenant:llm", registry=builder.build(),
                     budget_ceiling=ceiling(wall_time_ms=180_000), scheduler_threads=2)
    run_budget = budget(wall_time_ms=150_000, max_core_steps=5, max_program_operations=4)
    version = admit(host, CHAT_PROGRAM, run_budget, "chat.js")
    return host.run(version, {"question": question}, run_budget)


def run_bound_chat_program(base_url, api_key, model, question):
    controls = support.chat_controls()
    controls.usage_include = True  # ask OpenRouter for usage.cost
    host = make_bound_host(base_url, model=model, api_key=api_key, controls=controls,
                           budget_ceiling=support.ceiling(wall_time_ms=180_000,
                                                          model_tokens=10_000_000,
                                                          monetary_microunits=10_000_000))
    run_budget = priced_budget(wall_time_ms=150_000, max_core_steps=5)
    version = admit(host, BOUND_CHAT_PROGRAM, run_budget, "chat.js")
    return host.run(version, {"messages": [{"role": "user", "content": question}]}, run_budget)


@pytest.fixture
def chat_peer():
    with ChatPeer() as peer:
        peer.usage = {"prompt_tokens": 21, "completion_tokens": 3, "total_tokens": 24}
        yield peer


@needs_llm
def test_program_model_call_wiring_against_a_loopback_peer(chat_peer):
    provider, routing = openrouter_provider(chat_peer.base_url, "local-smoke")

    result = run_chat_program(provider, routing, "What is the capital of France?")

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output["text"] == "Paris."
    assert result.output["usage"] == {"input": 21, "output": 3}
    assert result.execution_trace == ["answer"]
    [sent] = chat_peer.requests
    assert sent["path"] == "/api/v1/chat/completions"
    assert sent["auth"] == "Bearer local-smoke"
    body = sent["body"]
    assert body["model"] == MODEL
    assert body.get("provider", {}).get("zdr") is True, body
    caps = [body[key] for key in ("max_tokens", "max_completion_tokens", "max_output_tokens") if key in body]
    assert caps == [MAX_OUTPUT_TOKENS], body
    assert [m["role"] for m in body["messages"]] == ["system", "user"]


@needs_llm
def test_bound_llm_call_wiring_against_a_loopback_peer(chat_peer):
    chat_peer.usage = {"prompt_tokens": 21, "completion_tokens": 3, "total_tokens": 24,
                       "cost": 0.001234567}

    result = run_bound_chat_program(chat_peer.base_url, "local-smoke", support.PRICED_MODEL,
                                    "What is the capital of France?")

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    [record] = result.monetary_records
    assert (record.evidence, record.microunits) == ("reported", 1235)
    assert record.reserved_microunits == route_ceiling_hold()
    assert result.usage.monetary_microunits == 1235
    [sent] = chat_peer.requests
    assert sent["auth"] == "Bearer local-smoke"
    assert sent["body"]["model"] == support.PRICED_MODEL
    assert sent["body"]["usage"] == {"include": True}, sent["body"]


@needs_llm
@pytest.mark.live
@live
def test_live_openrouter_model_call_inside_a_program():
    base_url = os.environ.get("OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1")
    provider, routing = openrouter_provider(base_url, os.environ["OPENROUTER_API_KEY"])

    result = run_chat_program(provider, routing, "What is the capital of France? One word.")

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output["text"].strip(), result.output
    usage = result.output["usage"]
    assert usage["input"] and usage["input"] > 0, usage
    assert usage["output"] and 0 < usage["output"] <= MAX_OUTPUT_TOKENS, usage


@needs_llm
@pytest.mark.live
@live
def test_live_openrouter_bound_llm_call_is_priced():
    base_url = os.environ.get("OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1")

    result = run_bound_chat_program(base_url, os.environ["OPENROUTER_API_KEY"], PRICED_MODEL,
                                    "What is the capital of France? One word.")

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert 0 < result.usage.model_tokens
    [record] = result.monetary_records
    assert record.evidence in ("reported", "estimated"), record
    assert record.origin == "openrouter" and record.model == PRICED_MODEL
    assert 0 < record.microunits <= record.reserved_microunits, record
    assert result.usage.monetary_microunits == record.microunits
    assert result.remaining_budget.monetary_microunits == 1_000_000 - record.microunits
