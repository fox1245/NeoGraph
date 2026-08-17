"""Runtime-context and controlled-provider binding contracts."""

import neograph_engine as ng
import pytest
import gc
import weakref


def _sha(char):
    return "sha256:" + char * 64


def _record(feed_id="feed", sequence=1, predecessor_id=None):
    data = ng.RuntimeHistoryRecordData()
    data.feed_id = feed_id
    data.sequence = sequence
    data.message_id = f"message-{sequence}"
    data.trust = ng.RuntimeTrustClass.UntrustedInput
    data.message = ng.ChatMessage("user", f"hello-{sequence}")
    data.predecessor_id = predecessor_id
    return ng.RuntimeHistoryRecord.create(data)


def _epoch(store, profile=ng.RuntimeGuaranteeProfile.Recorded):
    feed = ng.ContextStoreFeed("owner", "feed")
    first = _record()
    assert store.append_history(feed, first, None) == ng.ContextStoreAppendResult.Appended
    raw = store.snapshot_history(feed, 1, 1)
    data = ng.ContextEpochData()
    data.run_id = "run"
    data.sequence = 1
    data.feed_id = "feed"
    data.raw_from_sequence = 1
    data.raw_through_sequence = 1
    data.raw_window_digest = raw.digest
    data.guarantee_profile = profile
    return ng.ContextEpoch.create(data)


class _Provider(ng.Provider):
    def __init__(self):
        super().__init__()
        self.calls = 0
        self.last_messages = []

    def complete(self, params):
        self.calls += 1
        self.last_messages = list(params.messages)
        result = ng.ChatCompletion()
        result.message = ng.ChatMessage("assistant", "ok")
        return result

    def get_name(self):
        return "python-runtime-context-provider"


class _DurableReceipts(ng.DurableProviderDispatchReceiptStore):
    def __init__(self):
        super().__init__()
        self.receipts = {}

    def persist(self, receipt):
        existing = self.receipts.get(receipt.dispatch_id)
        if existing is None:
            self.receipts[receipt.dispatch_id] = receipt.serialize_canonical()
            return ng.ProviderDispatchReceiptPutResult.Stored
        if existing == receipt.serialize_canonical():
            return ng.ProviderDispatchReceiptPutResult.AlreadyPresent
        return ng.ProviderDispatchReceiptPutResult.Conflict


def test_runtime_context_canonical_roundtrip_and_profiles_are_public():
    record = _record()
    parsed = ng.RuntimeHistoryRecord.parse(record.serialize_canonical())
    assert parsed.id == record.id
    assert parsed.serialize_canonical() == record.serialize_canonical()
    assert {ng.RuntimeGuaranteeProfile.Legacy,
            ng.RuntimeGuaranteeProfile.Recorded,
            ng.RuntimeGuaranteeProfile.Strict}


def test_context_store_raw_append_snapshot_and_hydrate_roundtrip():
    store = ng.InMemoryContextStore()
    feed = ng.ContextStoreFeed("owner", "feed")
    first = _record()
    second = _record(sequence=2, predecessor_id=first.id)
    assert store.append_history(feed, first, None) == ng.ContextStoreAppendResult.Appended
    assert store.append_history(feed, second, first.id) == ng.ContextStoreAppendResult.Appended
    raw = store.snapshot_history(feed, 1, 2)
    assert store.hydrate_history(raw) == (
        first.serialize_canonical() + "\n" + second.serialize_canonical())


def test_strict_controller_fails_closed_without_active_epoch_or_durable_receipts():
    provider = _Provider()
    contexts = ng.InMemoryContextStore()
    controller = ng.RuntimeInterpositionController(
        provider, contexts, ng.InMemoryProviderDispatchReceiptStore(), _sha("a"))
    with pytest.raises(RuntimeError, match="active context epoch"):
        controller.invoke(ng.CompletionParams(model="model"))
    with pytest.raises(ValueError, match="durable dispatch receipt store"):
        controller.activate("owner", _epoch(contexts, ng.RuntimeGuaranteeProfile.Strict))
    assert provider.calls == 0


def test_controlled_path_assembles_epoch_and_keeps_dependencies_alive():
    provider = _Provider()
    contexts = ng.InMemoryContextStore()
    receipts = _DurableReceipts()
    controller = ng.RuntimeInterpositionController(
        provider, contexts, receipts, _sha("b"), max_input_tokens=1024)
    controller.activate("owner", _epoch(contexts, ng.RuntimeGuaranteeProfile.Strict))
    params = ng.CompletionParams(model="model")
    params.messages = [ng.ChatMessage("user", "unadmitted legacy prompt")]
    assert controller.invoke(params).message.content == "ok"
    assert provider.calls == 1
    assert [message.content for message in provider.last_messages] == ["hello-1"]
    assert len(receipts.receipts) == 1


def test_controller_retains_shared_provider_and_store_dependencies():
    provider = _Provider()
    contexts = ng.InMemoryContextStore()
    receipts = _DurableReceipts()
    refs = [weakref.ref(value) for value in (provider, contexts, receipts)]
    controller = ng.RuntimeInterpositionController(provider, contexts, receipts, _sha("c"))
    del provider, contexts, receipts
    gc.collect()
    assert all(ref() is not None for ref in refs)
    controller.clear()
