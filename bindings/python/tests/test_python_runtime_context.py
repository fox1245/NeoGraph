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
    data.message = ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(f"hello-{sequence}")])
    data.predecessor_id = predecessor_id
    return ng.RuntimeHistoryRecord.create(data)


def test_returned_message_mutation_cannot_change_immutable_raw_record():
    record = _record()
    identity = record.id
    canonical = record.serialize_canonical()
    original_message_id = record.message.id
    detached = record.message
    detached.role = ng.ProviderRole.Assistant
    detached.id = "forged-message"
    parts = detached.parts
    parts[0].value = "forged content"
    detached.parts = parts
    assert record.id == identity
    assert record.serialize_canonical() == canonical
    assert record.message.role == ng.ProviderRole.User
    assert record.message.id == original_message_id
    assert record.message.parts[0].value == "hello-1"


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


class _DurableReceipts(ng.DurableProviderDispatchReceiptStore):
    def __init__(self):
        super().__init__()
        self.receipts = {}
        self.outcomes = {}

    def persist(self, receipt):
        existing = self.receipts.get(receipt.dispatch_id)
        if existing is None:
            self.receipts[receipt.dispatch_id] = receipt.serialize_canonical()
            return ng.ProviderDispatchReceiptPutResult.Stored
        if existing == receipt.serialize_canonical():
            return ng.ProviderDispatchReceiptPutResult.AlreadyPresent
        return ng.ProviderDispatchReceiptPutResult.Conflict

    def settle(self, owner_scope, outcome):
        existing = self.outcomes.get((owner_scope, outcome.dispatch_id))
        if existing is None:
            self.outcomes[(owner_scope, outcome.dispatch_id)] = outcome.serialize_canonical()
            return ng.ProviderDispatchOutcomePutResult.Stored
        if existing == outcome.serialize_canonical():
            return ng.ProviderDispatchOutcomePutResult.AlreadyPresent
        return ng.ProviderDispatchOutcomePutResult.Conflict

    def outcome(self, owner_scope, dispatch_id):
        value = self.outcomes.get((owner_scope, dispatch_id))
        return None if value is None else ng.ProviderDispatchOutcomeReceipt.parse(value)


def test_runtime_context_canonical_roundtrip_and_profiles_are_public():
    record = _record()
    parsed = ng.RuntimeHistoryRecord.parse(record.serialize_canonical())
    assert parsed.id == record.id
    assert parsed.serialize_canonical() == record.serialize_canonical()
    assert {ng.RuntimeGuaranteeProfile.Legacy,
            ng.RuntimeGuaranteeProfile.Recorded,
            ng.RuntimeGuaranteeProfile.Strict}
    assert ng.ProviderDispatchState.Succeeded.name == "Succeeded"


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


def test_strict_controller_fails_closed_without_active_epoch_or_durable_receipts(provider_peer):
    provider = provider_peer.provider()
    contexts = ng.InMemoryContextStore()
    controller = ng.RuntimeInterpositionController(
        provider, contexts, ng.InMemoryProviderDispatchReceiptStore(), _sha("a"))
    with pytest.raises(RuntimeError, match="active context epoch"):
        controller.invoke(ng.make_provider_request(provider, "model", []))
    with pytest.raises(ValueError, match="durable dispatch receipt store"):
        controller.activate("owner", _epoch(contexts, ng.RuntimeGuaranteeProfile.Strict))
    assert provider_peer.requests == []


def test_controlled_path_assembles_epoch_and_keeps_dependencies_alive(provider_peer):
    provider = provider_peer.provider()
    contexts = ng.InMemoryContextStore()
    receipts = _DurableReceipts()
    controller = ng.RuntimeInterpositionController(
        provider, contexts, receipts, _sha("b"), max_input_tokens=1024)
    controller.activate("owner", _epoch(contexts, ng.RuntimeGuaranteeProfile.Strict))
    request = ng.make_provider_request(provider, "model", [])
    outcome = controller.invoke(request)
    assert outcome.failure is None
    logical, = provider_peer.logical_requests
    assert [(message["role"], message["text"]) for message in logical] == [
        ("user", "hello-1"),
    ]
    assert len(receipts.receipts) == 1
    assert len(receipts.outcomes) == 1


def test_controller_retains_shared_provider_and_store_dependencies(provider_peer):
    provider = provider_peer.provider()
    contexts = ng.InMemoryContextStore()
    receipts = _DurableReceipts()
    refs = [weakref.ref(value) for value in (provider, contexts, receipts)]
    controller = ng.RuntimeInterpositionController(provider, contexts, receipts, _sha("c"))
    del provider, contexts, receipts
    gc.collect()
    assert all(ref() is not None for ref in refs)
    controller.clear()


def test_context_transform_receipt_preserves_required_skill_identity():
    skill_data = ng.ContextArtifactData()
    skill_data.kind = ng.ContextArtifactKind.RequiredSkill
    skill_data.producer_id = "python-skill-loader"
    skill_data.source_digest = _sha("d")
    skill_data.source_feed_id = ""
    skill_data.covers_from_sequence = 0
    skill_data.covers_through_sequence = 0
    skill_data.media_type = "text/markdown"
    skill_data.required = True
    skill_data.content = {"text": "Always run the verifier."}
    skill = ng.ContextArtifact.create(skill_data)

    epoch_data = ng.ContextEpochData()
    epoch_data.run_id = "transform-run"
    epoch_data.sequence = 1
    epoch_data.raw_window_digest = _sha("e")
    epoch_data.artifact_ids = [skill.id]
    epoch_data.guarantee_profile = ng.RuntimeGuaranteeProfile.Strict
    epoch = ng.ContextEpoch.create(epoch_data)

    receipt_data = ng.ContextTransformReceiptData()
    receipt_data.source_context_epoch_id = epoch.id
    receipt_data.transformer_identity = _sha("f")
    receipt_data.input_artifact_ids = [skill.id]
    receipt_data.output_artifact_ids = [skill.id]
    receipt_data.preserved_required_artifact_ids = [skill.id]
    receipt = ng.ContextTransformReceipt.create(
        receipt_data, epoch, [skill], [skill]
    )

    ng.validate_context_transform_receipt(receipt, epoch, [skill], [skill])
    assert receipt.preserved_required_artifact_ids == [skill.id]


def test_strict_runtime_profile_uses_sqlite_durable_stores(tmp_path, provider_peer):
    provider = provider_peer.provider()
    contexts = ng.SQLiteContextStore(str(tmp_path / "strict-context.sqlite3"))
    receipts = ng.SQLiteProviderDispatchReceiptStore(
        str(tmp_path / "strict-dispatch.sqlite3")
    )
    hooks = ng.create_hook_runtime([], {})
    profile = ng.StrictRuntimeProfile(
        provider,
        contexts,
        receipts,
        hooks,
        _sha("a"),
        1024,
    )
    profile.activate(
        "owner", _epoch(contexts, ng.RuntimeGuaranteeProfile.Strict)
    )

    assert profile.active
    request = ng.make_provider_request(provider, "model", [])
    assert profile.invoke(request).failure is None
    assert [(message["role"], message["text"])
            for message in provider_peer.logical_requests[0]] == [("user", "hello-1")]
    profile.clear()
    assert not profile.active


def test_native_raw_record_roundtrip_requires_its_archive_and_owner(provider_peer, tmp_path):
    provider = provider_peer.provider()
    request = ng.make_provider_request(provider, "local-model", [
        ng.ProviderMessage(ng.ProviderRole.User, [ng.Text("native raw history")]),
    ])
    prepared = provider.prepare(request)
    descriptor = prepared.descriptor
    outcome = provider.dispatch(prepared)
    assert outcome.failure is None
    custody = tmp_path / "custody"
    custody.mkdir(mode=0o700)
    records = str(custody / "records")
    key = str(custody / "independent-activation")
    archive = ng.NativeArchive.provision(records, key, "raw-owner", descriptor)
    assert isinstance(archive, ng.NativeArchive)
    data = ng.RuntimeHistoryRecordData()
    data.feed_id = "native-feed"
    data.sequence = 1
    data.message_id = "native-assistant"
    data.trust = ng.RuntimeTrustClass.ModelOutput
    data.message = outcome.messages[0]
    record = ng.RuntimeHistoryRecord.create(data)
    stored = record.serialize_canonical(archive, "raw-owner")
    database = str(tmp_path / "native-context.sqlite3")
    context_store = ng.SQLiteContextStore(database, archive)
    feed = ng.ContextStoreFeed("raw-owner", "native-feed")
    assert context_store.append_history(feed, record, None) == ng.ContextStoreAppendResult.Appended
    del context_store, archive, outcome, record
    gc.collect()
    reopened = ng.NativeArchive.open(records, key, "raw-owner", descriptor)
    assert isinstance(reopened, ng.NativeArchive)
    restored = ng.RuntimeHistoryRecord.parse(stored, reopened, "raw-owner")
    assert restored.message.native is not None
    assert restored.message.parts[0].value == "ok"
    assert restored.serialize_canonical(reopened, "raw-owner") == stored
    context_store = ng.SQLiteContextStore(database, reopened)
    raw_range = context_store.snapshot_history(feed, 1, 1)
    hydrated, = context_store.hydrate_records(raw_range)
    assert hydrated.id == restored.id
    assert hydrated.message.native is not None
    assert hydrated.message.parts[0].value == "ok"
    indexed = context_store.history_record_by_message_id(feed, "native-assistant")
    assert indexed.id == restored.id
    assert indexed.message.native is not None
    assert context_store.history_record_by_message_id(feed, "not-recorded") is None
    missing_custody_store = ng.SQLiteContextStore(database)
    with pytest.raises(ValueError):
        missing_custody_store.history_record_by_message_id(feed, "native-assistant")
    with pytest.raises(ValueError):
        ng.RuntimeHistoryRecord.parse(stored)
    with pytest.raises(ValueError):
        ng.RuntimeHistoryRecord.parse(stored, reopened, "wrong-owner")
    wrong_archive = ng.NativeArchive.open(records, key, "wrong-owner", descriptor)
    assert isinstance(wrong_archive, ng.ProviderError)
    assert wrong_archive.kind == ng.ProviderErrorKind.Permission
    replay = ng.make_provider_request(
        provider, "local-model", list(request.messages) + [restored.message])
    assert provider.invoke(replay).failure is None
