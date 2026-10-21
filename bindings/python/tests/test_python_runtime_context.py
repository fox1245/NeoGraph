"""Runtime-context and controlled-provider binding contracts."""

import neograph_engine as ng
import pytest
import gc
import sys
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


def test_every_context_placement_is_bound_and_assembles_in_its_documented_place(provider_peer):
    provider = provider_peer.provider()
    contexts = ng.InMemoryContextStore()
    feed = ng.ContextStoreFeed("owner", "feed")
    previous = None
    for sequence, (role, trust, text) in enumerate([
            (ng.ProviderRole.User, ng.RuntimeTrustClass.UntrustedInput, "u1"),
            (ng.ProviderRole.Assistant, ng.RuntimeTrustClass.ModelOutput, "a1"),
            (ng.ProviderRole.User, ng.RuntimeTrustClass.UntrustedInput, "u2"),
            (ng.ProviderRole.Assistant, ng.RuntimeTrustClass.ModelOutput, "a2")], start=1):
        data = ng.RuntimeHistoryRecordData()
        data.feed_id = "feed"
        data.sequence = sequence
        data.message_id = f"message-{sequence}"
        data.trust = trust
        data.message = ng.ProviderMessage(role, [ng.Text(text)])
        data.predecessor_id = previous
        record = ng.RuntimeHistoryRecord.create(data)
        assert contexts.append_history(feed, record, previous) == ng.ContextStoreAppendResult.Appended
        previous = record.id

    artifact_ids = []
    for placement, text in [(ng.ContextPlacement.AfterHistory, "after-history"),
                            (ng.ContextPlacement.BeforeHistory, "before-history"),
                            (ng.ContextPlacement.AfterLatestUser, "after-user"),
                            (ng.ContextPlacement.BeforeLatestUser, "before-user")]:
        data = ng.ContextArtifactData()
        data.kind = ng.ContextArtifactKind.DerivedContext
        data.producer_id = "python-placement"
        data.source_digest = _sha("7")
        data.media_type = "text/plain"
        data.placement = placement
        data.content = text
        artifact = ng.ContextArtifact.create(data)
        assert artifact.placement == placement
        assert contexts.put_artifact("owner", artifact) == ng.ContextArtifactPutResult.Stored
        artifact_ids.append(artifact.id)

    epoch_data = ng.ContextEpochData()
    epoch_data.run_id = "placement-run"
    epoch_data.sequence = 1
    epoch_data.feed_id = "feed"
    epoch_data.raw_from_sequence = 1
    epoch_data.raw_through_sequence = 4
    epoch_data.raw_window_digest = contexts.snapshot_history(feed, 1, 4).digest
    epoch_data.artifact_ids = artifact_ids
    controller = ng.RuntimeInterpositionController(
        provider, contexts, ng.InMemoryProviderDispatchReceiptStore(), _sha("9"))
    controller.activate("owner", ng.ContextEpoch.create(epoch_data))

    assert controller.invoke(ng.make_provider_request(provider, "model", [])).failure is None
    logical, = provider_peer.logical_requests
    assert [(message["role"], message["text"]) for message in logical] == [
        ("user", "before-history"), ("user", "u1"), ("assistant", "a1"), ("user", "before-user"),
        ("user", "u2"), ("user", "after-user"), ("assistant", "a2"), ("user", "after-history")]


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


def _required_artifact(kind, text, digest_char):
    data = ng.ContextArtifactData()
    data.kind = kind
    data.producer_id = "python-host-policy"
    data.source_digest = _sha(digest_char)
    data.media_type = "text/markdown"
    data.required = True
    data.content = {"text": text}
    return ng.ContextArtifact.create(data)


def _strict_profile_with_required_artifacts(tmp_path, provider_peer, max_input_tokens,
                                            skill_text, constraint_text):
    # The documented strict path: durable SQLite stores, one required Skill and
    # one hard constraint, and a single short RAW user message.
    provider = provider_peer.provider()
    contexts = ng.SQLiteContextStore(str(tmp_path / "runtime.sqlite3"))
    receipts = ng.SQLiteProviderDispatchReceiptStore(str(tmp_path / "runtime.sqlite3"))
    skill = _required_artifact(ng.ContextArtifactKind.RequiredSkill, skill_text, "1")
    constraint = _required_artifact(ng.ContextArtifactKind.HardConstraint, constraint_text, "2")
    for artifact in (skill, constraint):
        assert contexts.put_artifact("tenant-a", artifact) == ng.ContextArtifactPutResult.Stored
    feed = ng.ContextStoreFeed("tenant-a", "feed")
    record = _record()
    assert contexts.append_history(feed, record, None) == ng.ContextStoreAppendResult.Appended
    epoch_data = ng.ContextEpochData()
    epoch_data.run_id = "strict-run"
    epoch_data.sequence = 1
    epoch_data.feed_id = "feed"
    epoch_data.raw_from_sequence = 1
    epoch_data.raw_through_sequence = 1
    epoch_data.raw_window_digest = contexts.snapshot_history(feed, 1, 1).digest
    epoch_data.artifact_ids = [skill.id, constraint.id]
    epoch_data.guarantee_profile = ng.RuntimeGuaranteeProfile.Strict
    profile = ng.StrictRuntimeProfile(
        provider,
        contexts,
        receipts,
        ng.create_hook_runtime([], {}),
        _sha("a"),
        max_input_tokens=max_input_tokens,
        required_context_artifact_ids=[constraint.id],
        required_skill_artifact_ids=[skill.id],
    )
    profile.activate("tenant-a", ng.ContextEpoch.create(epoch_data))
    return provider, profile


def test_strict_profile_admits_short_history_with_two_required_artifacts(
        tmp_path, provider_peer):
    # Issue #373: the mandatory estimate used the message projection JSON and
    # the input estimate the smaller wire body, so a short history with two
    # required artifacts was rejected as "mandatory exceeds input".
    provider, profile = _strict_profile_with_required_artifacts(
        tmp_path, provider_peer, 32_000,
        "Always run the verifier.", "Never publish without verification.")

    outcome = profile.invoke(ng.make_provider_request(provider, "model", []))

    assert outcome.failure is None
    logical, = provider_peer.logical_requests
    assert [(message["role"], message["text"]) for message in logical] == [
        ("system", "Always run the verifier."),
        ("system", "Never publish without verification."),
        ("user", "hello-1"),
    ]


def test_strict_profile_still_blocks_required_artifacts_over_the_budget(
        tmp_path, provider_peer):
    # Required content larger than the whole input budget is a genuine
    # overflow: the turn is blocked before anything is sent.
    provider, profile = _strict_profile_with_required_artifacts(
        tmp_path, provider_peer, 64, "verify " * 40, "never publish " * 20)

    with pytest.raises(ng.ContextBudgetBlocked):
        profile.invoke(ng.make_provider_request(provider, "model", []))
    assert provider_peer.requests == []


def test_assembly_receipt_rejects_mandatory_tokens_above_the_input_estimate():
    epoch_data = ng.ContextEpochData()
    epoch_data.run_id = "receipt-run"
    epoch_data.sequence = 1
    epoch_data.raw_window_digest = _sha("3")
    epoch = ng.ContextEpoch.create(epoch_data)
    data = ng.ContextAssemblyReceiptData()
    data.context_epoch_id = epoch.id
    data.normalized_request_digest = _sha("4")
    data.message_window_digest = _sha("5")
    data.estimated_input_tokens = 40
    data.mandatory_input_tokens = 41
    with pytest.raises(ValueError, match=r"\(41\).*\(40\)"):
        ng.ContextAssemblyReceipt.create(data, epoch, [])
    data.mandatory_input_tokens = 40
    assert ng.ContextAssemblyReceipt.create(data, epoch, []).mandatory_input_tokens == 40


@pytest.mark.skipif(
    sys.platform == "win32",
    reason="NativeArchive custody needs an owner-only protected DACL on the parent directory; "
           "Path.mkdir(mode=0o700) only sets POSIX permission bits")
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


def _presented(*roles_and_text):
    return [ng.ProviderMessage(role, [ng.Text(text)]) for role, text in roles_and_text]


def test_strict_profile_is_host_stepped_and_refuses_a_turn_the_caller_advanced(
        tmp_path, provider_peer):
    # Issue #444: a loop inside one run presents assistant or tool messages the
    # epoch cannot carry. The profile refuses that call instead of resending the
    # first turn's context, and names the host-stepped contract.
    provider = provider_peer.provider()
    contexts = ng.SQLiteContextStore(str(tmp_path / "stepped-context.sqlite3"))
    receipts = ng.SQLiteProviderDispatchReceiptStore(str(tmp_path / "stepped-dispatch.sqlite3"))
    profile = ng.StrictRuntimeProfile(
        provider, contexts, receipts, ng.create_hook_runtime([], {}), _sha("a"), 1024)
    epoch = _epoch(contexts, ng.RuntimeGuaranteeProfile.Strict)
    profile.activate("owner", epoch)

    first = _presented((ng.ProviderRole.User, "hello-1"))
    advanced = first + _presented(
        (ng.ProviderRole.Assistant, "an answer"), (ng.ProviderRole.User, "more"))

    assert profile.invoke(ng.make_provider_request(provider, "model", first)).failure is None
    with pytest.raises(RuntimeError, match="host appends RAW records and activates a new "
                                           "epoch for every provider call"):
        profile.invoke(ng.make_provider_request(provider, "model", advanced))
    assert len(provider_peer.requests) == 1

    # A retry of the same turn still works.
    assert profile.invoke(ng.make_provider_request(provider, "model", first)).failure is None
    assert len(provider_peer.requests) == 2

    # Activating the next epoch is the host stepping to the next turn.
    next_data = ng.ContextEpochData()
    next_data.run_id = "run"
    next_data.sequence = 2
    next_data.predecessor_id = epoch.id
    next_data.feed_id = "feed"
    next_data.raw_from_sequence = 1
    next_data.raw_through_sequence = 1
    next_data.raw_window_digest = contexts.snapshot_history(
        ng.ContextStoreFeed("owner", "feed"), 1, 1).digest
    next_data.guarantee_profile = ng.RuntimeGuaranteeProfile.Strict
    profile.activate("owner", ng.ContextEpoch.create(next_data))
    assert profile.invoke(ng.make_provider_request(provider, "model", advanced)).failure is None
    assert len(provider_peer.requests) == 3
