"""Live end-to-end cases of SchemaProvider's endpoints on Provider (#531): embeddings at OpenAI and, awaited, at
OpenRouter, and OpenAI moderation. Nothing runs unless NEOGRAPH_E2E_LIVE=1 and the provider's key variable are set.
Each call sends a sentence or two; the C++ cases of tests/live_e2e_test.cpp (LiveEndpoints) are their peers.
"""

import asyncio

import pytest

import neograph_engine as ng

from live_support import case, live_model, provider


def _failure_text(outcome):
    if not isinstance(outcome, ng.ProviderFailure):
        return ""
    return f"provider failure kind {outcome.error.kind} http {outcome.error.http_status}: {outcome.error.safe_message}"


def _embedding_request(model):
    return ng.EmbeddingsRequest.from_dict(
        {"model": model, "inputs": [{"text": "A graph of agents."}, {"text": "A list of chores."}]})


def _expect_two_vectors(outcome):
    assert isinstance(outcome, ng.EmbeddingsResult), _failure_text(outcome)
    assert len(outcome.vectors) == 2
    assert len(outcome.vectors[0]) > 100 and len(outcome.vectors[0]) == len(outcome.vectors[1])
    assert outcome.usage.input_total is not None and outcome.usage.input_total.value > 0


@pytest.mark.parametrize("model", [case(live_model("openai.embeddings"), keys=["OPENAI_API_KEY"],
                                        id=live_model("openai.embeddings"))])
def test_openai_embeddings(model):
    _expect_two_vectors(provider("openai.chat", model).complete_endpoint(_embedding_request(model)))


@pytest.mark.parametrize("model", [case(live_model("openrouter.embeddings"), keys=["OPENROUTER_API_KEY"],
                                        id=live_model("openrouter.embeddings"))])
def test_openrouter_embeddings_async(model):
    router = provider("openrouter.chat", model)

    async def embed():
        return await router.complete_endpoint_async(_embedding_request(model))

    _expect_two_vectors(asyncio.run(embed()))


@pytest.mark.parametrize("model", [case(live_model("openai.moderation"), keys=["OPENAI_API_KEY"],
                                        id=live_model("openai.moderation"))])
def test_openai_moderation(model):
    openai = provider("openai.chat", model)
    prepared = openai.prepare_endpoint(
        ng.ModerationRequest.from_dict({"model": model, "inputs": [{"text": "I would like a cup of tea."}]}))
    assert prepared.error is None and prepared.endpoint == "moderation"
    outcome = openai.complete_endpoint(prepared)
    assert isinstance(outcome, ng.ModerationResult), _failure_text(outcome)
    assert len(outcome.verdicts) == 1
    assert not outcome.verdicts[0].flagged and outcome.verdicts[0].categories
