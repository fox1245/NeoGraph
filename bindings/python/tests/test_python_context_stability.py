"""The stability class, anchor mode, epoch order and claim, and the prefix fields of the receipt (#329).

Nothing here changes what the assembler writes. The tests pin the stored values: a value stored before
the change keeps its bytes and identity, a value that sets the new fields has its own identity, and the
claim of an epoch to extend its predecessor is checked.
"""

import json

import neograph_engine as ng
import pytest

import prefix_trace as pt

KIND = ng.ContextArtifactKind

# Canonical bytes and identities written by the code before the stability class existed.
SKILL_V1_ID = "sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a"
SKILL_V1 = '{"content":{"name":"secure-edit","text":"Verify before editing."},"content_digest":"sha256:c3779c8fbb58152f2e24b9e8ec5fe4d67e7a585220894b01cd1d37c1ad534d36","covers_from_sequence":0,"covers_through_sequence":0,"format":"neograph-context-artifact","id":"sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a","kind":"required_skill","media_type":"text/markdown","placement":"before_latest_user","priority":100,"producer_id":"skill-resolver.v1","required":true,"source_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","source_feed_id":"","storage_schema_version":1}'
SUMMARY_V1_ID = "sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0"
SUMMARY_V1 = '{"content":{"text":"Summary of earlier turns."},"content_digest":"sha256:3d56bba71b7b71a84187238fce1ff29e21d15ffe3984023b587cedf193b54a09","covers_from_sequence":1,"covers_through_sequence":6,"format":"neograph-context-artifact","id":"sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0","kind":"derived_context","media_type":"text/plain","placement":"before_history","priority":5,"producer_id":"summarizer.v1","required":false,"source_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","source_feed_id":"hist_test","storage_schema_version":1}'
EPOCH_V1_ID = "sha256:2c8465c9c6f9b883e64db20d05fc343b69b3d9cc10c936102a2cf2c336beef85"
EPOCH_V1 = '{"artifact_ids":["sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a","sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0"],"feed_id":"hist_test","format":"neograph-context-epoch","guarantee_profile":"recorded","id":"sha256:2c8465c9c6f9b883e64db20d05fc343b69b3d9cc10c936102a2cf2c336beef85","raw_from_sequence":1,"raw_through_sequence":42,"raw_window_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","run_id":"run_test","sequence":1,"storage_schema_version":1}'
RECEIPT_V1_ID = "sha256:4d2d8c0f8b669f4d2043a753fd7cba98b8311902a19e2c51a74f704493bfd120"
RECEIPT_V1 = '{"artifact_ids":["sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a","sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0"],"context_epoch_id":"sha256:2c8465c9c6f9b883e64db20d05fc343b69b3d9cc10c936102a2cf2c336beef85","estimated_input_tokens":900,"format":"neograph-context-assembly-receipt","id":"sha256:4d2d8c0f8b669f4d2043a753fd7cba98b8311902a19e2c51a74f704493bfd120","mandatory_input_tokens":200,"message_window_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","normalized_request_digest":"sha256:2222222222222222222222222222222222222222222222222222222222222222","raw_from_sequence":1,"raw_through_sequence":42,"required_skill_artifact_ids":["sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a"],"storage_schema_version":1}'


def _sha(char):
    return "sha256:" + char * 64


def _sources(artifacts):
    """What a receipt records for the classes of artifacts: declared or derived, under the rule in force."""
    return {artifact.id: ng.ClassSource.Declared if artifact.declared_stability_class is not None
            else ng.ClassSource.Derived for artifact in artifacts}


def _skill_data():
    data = ng.ContextArtifactData()
    data.kind = KIND.RequiredSkill
    data.producer_id = "skill-resolver.v1"
    data.source_digest = _sha("a")
    data.media_type = "text/markdown"
    data.placement = ng.ContextPlacement.BeforeLatestUser
    data.priority = 100
    data.required = True
    data.content = {"name": "secure-edit", "text": "Verify before editing."}
    return data


def _summary_data():
    data = ng.ContextArtifactData()
    data.kind = KIND.DerivedContext
    data.producer_id = "summarizer.v1"
    data.source_digest = _sha("b")
    data.source_feed_id = "hist_test"
    data.covers_from_sequence = 1
    data.covers_through_sequence = 6
    data.media_type = "text/plain"
    data.placement = ng.ContextPlacement.BeforeHistory
    data.priority = 5
    data.content = {"text": "Summary of earlier turns."}
    return data


def _artifact(kind, producer, text="text", declared=None, **fields):
    data = ng.ContextArtifactData()
    data.kind = kind
    data.producer_id = producer
    data.source_digest = _sha("c")
    data.media_type = "text/plain"
    data.required = kind in (KIND.RequiredSkill, KIND.HardConstraint)
    data.content = text
    data.stability_class = declared
    for name, value in fields.items():
        setattr(data, name, value)
    return ng.ContextArtifact.create(data)


def _epoch(artifacts, sequence=1, predecessor=None, raw=(1, 5), order=True, claim=None, reason=None, run="run_claim"):
    data = ng.ContextEpochData()
    data.run_id = run
    data.sequence = sequence
    data.predecessor_id = predecessor
    data.feed_id = "hist_claim"
    data.raw_from_sequence, data.raw_through_sequence = raw
    data.raw_window_digest = _sha("e")
    data.artifact_ids = [artifact.id for artifact in artifacts]
    if order:
        data.ordered_artifact_ids = [artifact.id for artifact in artifacts]
    data.prefix_claim = claim
    data.break_reason = reason
    return ng.ContextEpoch.create(data)


def test_values_stored_before_the_class_existed_keep_their_bytes_and_identity():
    skill = ng.ContextArtifact.parse(SKILL_V1)
    summary = ng.ContextArtifact.parse(SUMMARY_V1)
    assert (skill.id, summary.id) == (SKILL_V1_ID, SUMMARY_V1_ID)
    assert skill.serialize_canonical() == SKILL_V1
    assert ng.ContextArtifact.create(_skill_data()).serialize_canonical() == SKILL_V1
    assert ng.ContextArtifact.create(_summary_data()).serialize_canonical() == SUMMARY_V1
    assert skill.declared_stability_class is None and skill.declared_anchor_mode is None
    assert skill.stability_class == ng.StabilityClass.Stable
    assert summary.stability_class == ng.StabilityClass.Volatile
    assert skill.anchor_mode == ng.AnchorMode.Replace

    epoch = ng.ContextEpoch.parse(EPOCH_V1)
    assert epoch.id == EPOCH_V1_ID and epoch.serialize_canonical() == EPOCH_V1
    assert epoch.ordered_artifact_ids is None and epoch.prefix_claim is None and epoch.break_reason is None

    receipt = ng.ContextAssemblyReceipt.parse(RECEIPT_V1, epoch, [skill, summary])
    assert receipt.id == RECEIPT_V1_ID and receipt.serialize_canonical() == RECEIPT_V1
    for name in ("assembled_artifact_ids", "artifact_classes", "class_sources", "class_rule_version",
                 "encoder_family", "stable_boundary",
                 "prefix_boundary", "prefix_digest", "tools_digest", "break_reason"):
        assert getattr(receipt, name) is None, name

    # A successor cannot extend an epoch that records no order.
    successor = _epoch([skill], sequence=2, predecessor=epoch.id, raw=(1, 50), claim=ng.ContextPrefixClaim.Extends)
    with pytest.raises(ng.ContextStabilityError) as refused:
        ng.verify_context_epoch_claim(successor, [skill], epoch, [skill, summary])
    assert refused.value.code == ng.ContextStabilityRefusal.PredecessorOrderUnknown


@pytest.mark.parametrize("kind, producer, expected", [
    (KIND.RequiredSkill, "skill-resolver.v1", ng.StabilityClass.Stable),
    (KIND.HardConstraint, "host-policy.v1", ng.StabilityClass.Stable),
    (KIND.DerivedContext, ng.COMPACTION_PRODUCER_ID, ng.StabilityClass.Slow),
    (KIND.DerivedContext, "summarizer.v1", ng.StabilityClass.Volatile),
    (KIND.HookOutput, "hook-output.v1", ng.StabilityClass.Volatile),
    (KIND.UntrustedSupplemental, "retrieval.v1", ng.StabilityClass.Volatile),
    (KIND.RawHistory, "raw-copy.v1", ng.StabilityClass.Volatile),
    (KIND.HookOutput, ng.COMPACTION_PRODUCER_ID, ng.StabilityClass.Volatile),
])
def test_the_class_of_an_artifact_that_declares_none_is_derived(kind, producer, expected):
    assert ng.derive_stability_class(kind, producer) == expected
    artifact = _artifact(kind, producer)
    assert artifact.stability_class == expected
    assert artifact.declared_stability_class is None


def test_a_declared_class_and_anchor_mode_are_in_the_identity_only_when_set():
    plain = ng.ContextArtifact.create(_summary_data())
    assert json.loads(plain.serialize_canonical())["storage_schema_version"] == 1

    data = _summary_data()
    data.stability_class = ng.StabilityClass.Slow
    slow = ng.ContextArtifact.create(data)
    assert slow.id != plain.id
    assert slow.stability_class == slow.declared_stability_class == ng.StabilityClass.Slow
    assert json.loads(slow.serialize_canonical())["stability_class"] == "slow"

    data = _summary_data()
    data.anchor_mode = ng.AnchorMode.KeepInPlace
    anchored = ng.ContextArtifact.create(data)
    assert anchored.id not in (plain.id, slow.id)
    assert anchored.anchor_mode == anchored.declared_anchor_mode == ng.AnchorMode.KeepInPlace
    assert plain.anchor_mode == ng.AnchorMode.Replace and plain.declared_anchor_mode is None
    data.anchor_mode = ng.AnchorMode.Replace
    assert ng.ContextArtifact.create(data).id != plain.id

    for artifact in (slow, anchored):
        again = ng.ContextArtifact.parse(artifact.serialize_canonical())
        assert again.id == artifact.id
        assert again.declared_stability_class == artifact.declared_stability_class
        assert again.declared_anchor_mode == artifact.declared_anchor_mode
    forged = json.loads(plain.serialize_canonical())
    forged["stability_class"] = "stable"
    with pytest.raises(ValueError):
        ng.ContextArtifact.parse(json.dumps(forged))


@pytest.mark.parametrize("kind", [KIND.RequiredSkill, KIND.HardConstraint])
def test_a_volatile_artifact_of_a_system_kind_is_refused_with_a_typed_error(kind):
    with pytest.raises(ng.ContextStabilityError) as refused:
        _artifact(kind, "producer.v1", declared=ng.StabilityClass.Volatile)
    assert refused.value.code == ng.ContextStabilityRefusal.VolatileSystemArtifact
    assert isinstance(refused.value, ValueError)
    assert _artifact(kind, "producer.v1", declared=ng.StabilityClass.Stable).stability_class == ng.StabilityClass.Stable
    assert _artifact(kind, "producer.v1", declared=ng.StabilityClass.Slow).stability_class == ng.StabilityClass.Slow
    assert _artifact(KIND.HookOutput, "producer.v1", declared=ng.StabilityClass.Volatile).stability_class == (
        ng.StabilityClass.Volatile)


def test_an_epoch_records_an_order_and_a_claim():
    a, b, c = _sha("1"), _sha("2"), _sha("3")
    data = ng.ContextEpochData()
    data.run_id = "run_test"
    data.sequence = 2
    data.predecessor_id = _sha("9")
    data.feed_id = "hist_test"
    data.raw_from_sequence, data.raw_through_sequence = 1, 4
    data.raw_window_digest = _sha("d")
    data.artifact_ids = [a, b, c]
    legacy = ng.ContextEpoch.create(data)
    assert legacy.ordered_artifact_ids is None

    data.ordered_artifact_ids = [c, a, b]
    ordered = ng.ContextEpoch.create(data)
    assert ordered.id != legacy.id
    assert ordered.artifact_ids == [a, b, c] and ordered.ordered_artifact_ids == [c, a, b]
    assert ng.ContextEpoch.parse(ordered.serialize_canonical()).ordered_artifact_ids == [c, a, b]
    data.ordered_artifact_ids = [a, b]
    with pytest.raises(ValueError):
        ng.ContextEpoch.create(data)

    data.ordered_artifact_ids = [c, a, b]
    data.prefix_claim = ng.ContextPrefixClaim.Breaks
    with pytest.raises(ValueError):
        ng.ContextEpoch.create(data)  # a break names its reason
    data.break_reason = ng.ContextBreakReason.Compaction
    breaking = ng.ContextEpoch.create(data)
    again = ng.ContextEpoch.parse(breaking.serialize_canonical())
    assert (again.prefix_claim, again.break_reason) == (ng.ContextPrefixClaim.Breaks, ng.ContextBreakReason.Compaction)
    assert {reason.name for reason in ng.ContextBreakReason.__members__.values()} == {
        "Compaction", "StableChange", "LayoutChange", "AnchorDrop", "OrderChange", "Explicit"}

    data.prefix_claim = ng.ContextPrefixClaim.Extends
    data.break_reason = None
    data.predecessor_id = None
    data.sequence = 1
    with pytest.raises(ng.ContextStabilityError) as refused:
        ng.ContextEpoch.create(data)  # the first epoch has nothing to extend
    assert refused.value.code == ng.ContextStabilityRefusal.PredecessorUnavailable


def test_the_receipt_of_version_2_carries_the_prefix_fields():
    skill = ng.ContextArtifact.create(_skill_data())
    summary = ng.ContextArtifact.create(_summary_data())
    epoch = _epoch([skill, summary], order=False)
    base = ng.ContextAssemblyReceiptData()
    base.context_epoch_id = epoch.id
    base.normalized_request_digest = _sha("2")
    base.message_window_digest = _sha("3")
    base.artifact_ids = epoch.artifact_ids
    base.required_skill_artifact_ids = [skill.id]
    base.raw_from_sequence, base.raw_through_sequence = 1, 5
    base.estimated_input_tokens, base.mandatory_input_tokens = 900, 200
    legacy = ng.ContextAssemblyReceipt.create(base, epoch, [skill, summary])
    assert json.loads(legacy.serialize_canonical())["storage_schema_version"] == 1

    base.assembled_artifact_ids = [summary.id, skill.id]
    base.artifact_classes = {skill.id: ng.StabilityClass.Stable, summary.id: ng.StabilityClass.Volatile}
    base.class_sources = _sources([skill, summary])
    base.class_rule_version = ng.STABILITY_DERIVATION_RULE_VERSION
    base.encoder_family = "openai.chat"
    base.tools_digest = _sha("4")
    receipt = ng.ContextAssemblyReceipt.create(base, epoch, [skill, summary])
    assert receipt.id != legacy.id
    assert json.loads(receipt.serialize_canonical())["storage_schema_version"] == 2
    assert receipt.assembled_artifact_ids == [summary.id, skill.id]
    assert receipt.artifact_classes == {skill.id: ng.StabilityClass.Stable, summary.id: ng.StabilityClass.Volatile}
    assert receipt.class_sources == {skill.id: ng.ClassSource.Derived, summary.id: ng.ClassSource.Derived}
    assert receipt.class_rule_version == ng.STABILITY_DERIVATION_RULE_VERSION
    assert receipt.encoder_family == "openai.chat" and receipt.tools_digest == _sha("4")
    assert receipt.stable_boundary is None and receipt.prefix_boundary is None and receipt.prefix_digest is None
    again = ng.ContextAssemblyReceipt.parse(receipt.serialize_canonical(), epoch, [skill, summary])
    assert again.id == receipt.id

    base.stable_boundary = ng.ContextPrefixBoundary(1, 0)
    base.prefix_boundary = ng.ContextPrefixBoundary(3, 2)
    base.prefix_digest = _sha("5")
    marked = ng.ContextAssemblyReceipt.parse(
        ng.ContextAssemblyReceipt.create(base, epoch, [skill, summary]).serialize_canonical(), epoch,
        [skill, summary])
    assert marked.stable_boundary == ng.ContextPrefixBoundary(1, 0)
    assert (marked.prefix_boundary.message_index, marked.prefix_boundary.part_index) == (3, 2)
    assert marked.prefix_digest == _sha("5")

    base.artifact_classes = {skill.id: ng.StabilityClass.Volatile, summary.id: ng.StabilityClass.Volatile}
    with pytest.raises(ValueError):
        ng.ContextAssemblyReceipt.create(base, epoch, [skill, summary])  # the table is the artifacts' classes


class _Recording:
    """Wraps the assembler of a scripted conversation and keeps the receipt of every call."""

    def __init__(self, inner):
        self.inner = inner
        self.receipts = []

    def assemble(self, *arguments):
        prepared, receipt = self.inner.assemble(*arguments)
        self.receipts.append(receipt)
        return prepared, receipt


def _receipts(preset, layout, turns):
    conversation = pt.Conversation(preset, layout, turns=turns)
    recording = _Recording(conversation.assembler)
    conversation.assembler = recording
    conversation.run()
    return conversation, recording.receipts


@pytest.mark.parametrize("preset", ["openai.chat", "anthropic.messages", "google.generate"])
def test_the_assembler_records_the_order_the_classes_and_the_encoder_of_every_call(preset):
    conversation, receipts = _receipts(preset, "replace-tail", 3)
    fixed, = conversation.stable
    assert len(receipts) == 3
    for receipt in receipts:
        assert receipt.encoder_family == pt.PRESETS[preset][0]
        assert receipt.assembled_artifact_ids[0] == fixed.id  # the fixed block is at BeforeHistory, in front
        assert sorted(receipt.assembled_artifact_ids) == receipt.artifact_ids
        assert receipt.artifact_classes[fixed.id] == ng.StabilityClass.Stable
        assert [receipt.artifact_classes[i] for i in receipt.assembled_artifact_ids[1:]] == [ng.StabilityClass.Volatile]
        assert receipt.tools_digest is None
        assert receipt.stable_boundary is None and receipt.prefix_boundary is None
        assert receipt.prefix_digest is None and receipt.break_reason is None


@pytest.mark.parametrize("preset", ["openai.chat", "google.generate", "openrouter.chat"])
def test_the_tools_digest_follows_the_tools_of_the_encoded_request(preset):
    _, without_tools = _receipts(preset, "replace-tail", 2)
    _, with_tools = _receipts(preset, "tool-regroup", 2)
    assert {receipt.tools_digest for receipt in without_tools} == {None}
    # The same tools in every call of the conversation, and a digest that names them.
    assert len({receipt.tools_digest for receipt in with_tools}) == 1
    assert with_tools[0].tools_digest is not None


def test_a_claim_to_extend_is_verified_structurally():
    skill = _artifact(KIND.RequiredSkill, "skill-resolver.v1", "Verify before editing.")
    summary = _artifact(KIND.DerivedContext, ng.COMPACTION_PRODUCER_ID, "Summary.",
                        source_feed_id="hist_claim", covers_from_sequence=1, covers_through_sequence=3)
    assert summary.stability_class == ng.StabilityClass.Slow

    def note(text):
        return _artifact(KIND.HookOutput, "hook-output.v1", text)

    def anchor(through):
        return _artifact(KIND.UntrustedSupplemental, "retrieval.v1", f"Retrieved {through}",
                         source_feed_id="hist_claim", covers_from_sequence=through,
                         covers_through_sequence=through, anchor_mode=ng.AnchorMode.KeepInPlace)

    first_set = [skill, summary, note("reading 1"), anchor(4)]
    first = _epoch(first_set)
    extends = ng.ContextPrefixClaim.Extends

    def refusal(next_set, **fields):
        epoch = _epoch(next_set, sequence=2, predecessor=first.id, claim=extends, **fields)
        with pytest.raises(ng.ContextStabilityError) as refused:
            ng.verify_context_epoch_claim(epoch, next_set, first, first_set)
        return refused.value.code

    good_set = [skill, summary, note("reading 2"), anchor(4)]
    good = _epoch(good_set, sequence=2, predecessor=first.id, raw=(1, 6), claim=extends)
    ng.verify_context_epoch_claim(good, good_set, first, first_set)  # only the Volatile material changed

    with pytest.raises(ng.ContextStabilityError) as refused:
        ng.verify_context_epoch_claim(good, good_set)
    assert refused.value.code == ng.ContextStabilityRefusal.PredecessorUnavailable
    codes = ng.ContextStabilityRefusal
    assert refusal(good_set, raw=(2, 6)) == codes.RawWindowMoved
    assert refusal(good_set, raw=(1, 5)) == codes.RawWindowNotGrown
    assert refusal([skill, anchor(4)], raw=(1, 6)) == codes.StablePrefixChanged
    assert refusal([summary, skill, anchor(4)], raw=(1, 6)) == codes.StablePrefixChanged
    assert refusal([skill, summary, note("reading 2")], raw=(1, 6)) == codes.AnchorDropped
    assert refusal([skill, summary, anchor(4), anchor(9)], raw=(1, 6)) == codes.AnchorOutOfRange

    # An epoch that breaks, or claims nothing, is accepted as it stands.
    breaking = _epoch([skill], sequence=2, predecessor=first.id, raw=(4, 9),
                      claim=ng.ContextPrefixClaim.Breaks, reason=ng.ContextBreakReason.Compaction)
    ng.verify_context_epoch_claim(breaking, [skill])


def test_the_controller_activates_an_epoch_that_extends_the_active_one(provider_peer):
    provider = provider_peer.provider()
    contexts = ng.InMemoryContextStore()
    controller = ng.RuntimeInterpositionController(
        provider, contexts, ng.InMemoryProviderDispatchReceiptStore(), _sha("a"))
    skill = _artifact(KIND.RequiredSkill, "skill-resolver.v1", "Verify before editing.")
    contexts.put_artifact("owner", skill)
    first = _epoch([skill])
    successor = _epoch([skill], sequence=2, predecessor=first.id, raw=(1, 6), claim=ng.ContextPrefixClaim.Extends)

    with pytest.raises(ng.ContextStabilityError) as refused:
        controller.activate("owner", successor)
    assert refused.value.code == ng.ContextStabilityRefusal.PredecessorUnavailable
    assert not controller.active
    controller.activate("owner", first)
    controller.activate("owner", successor)
    assert controller.active
    controller.clear()
    controller.activate("owner", successor, first)  # the predecessor the host loaded
    assert controller.active


def _head(text, placement=ng.ContextPlacement.BeforeHistory, **fields):
    return _artifact(KIND.RequiredSkill, "skill-resolver.v1", text, placement=placement, **fields)


def _tail(text, producer="hook-output.v1", priority=0):
    return _artifact(KIND.HookOutput, producer, text, placement=ng.ContextPlacement.AfterHistory, priority=priority)


def test_the_order_of_artifacts_is_a_function_of_class_priority_producer_and_key_alone():
    skill = _head("Verify before editing.")
    high = _tail("high", priority=10)
    beta, gamma = _tail("beta", "alpha.v1"), _tail("gamma", "alpha.v1")
    ordered = ng.order_context_artifacts([(gamma, "2"), (high, ""), (beta, "1"), (skill, "")])
    assert ordered == [skill.id, high.id, beta.id, gamma.id]
    for round_ in range(100):
        first, second, live = (_tail(f"{name} {round_ * 7919}", "sibling.v1") for name in ("first", "second", "live"))
        assert ng.order_context_artifacts([(live, "c"), (first, "a"), (second, "b")]) == [first.id, second.id, live.id]
    with pytest.raises(ng.ContextStabilityError) as refused:
        ng.order_context_artifacts([(beta, "1"), (gamma, "1")])
    assert refused.value.code == ng.ContextStabilityRefusal.AmbiguousOrder


def test_the_stable_first_layout_is_declared_on_the_epoch_and_checked_against_its_artifacts():
    skill, note = _head("Verify before editing."), _tail("reading")
    first = _epoch([skill, note], claim=None)
    data = ng.ContextEpochData()
    data.run_id, data.sequence, data.feed_id = "run_claim", 1, "hist_claim"
    data.raw_from_sequence, data.raw_through_sequence, data.raw_window_digest = 1, 5, _sha("e")
    data.artifact_ids = [skill.id, note.id]
    assert first.assembly_layout == ng.AssemblyLayout.Legacy
    data.assembly_layout = ng.AssemblyLayout.StableFirst
    with pytest.raises(ValueError):
        ng.ContextEpoch.create(data)  # the layout reads the order the epoch records
    data.ordered_artifact_ids = [skill.id, note.id]
    stable_first = ng.ContextEpoch.create(data)
    assert stable_first.assembly_layout == ng.AssemblyLayout.StableFirst
    assert ng.ContextEpoch.parse(stable_first.serialize_canonical()).assembly_layout == ng.AssemblyLayout.StableFirst
    ng.validate_context_layout(stable_first, [skill, note])

    for artifacts, code in [
            ([_head("rule", ng.ContextPlacement.BeforeLatestUser), note], ng.ContextStabilityRefusal.StableOutsideHistoryHead),
            ([skill, _artifact(KIND.HookOutput, "hook-output.v1", "moved", placement=ng.ContextPlacement.BeforeHistory)],
             ng.ContextStabilityRefusal.VolatileOutsideTail),
            ([note, skill], ng.ContextStabilityRefusal.TierOrderViolated)]:
        data.artifact_ids = [artifact.id for artifact in artifacts]
        data.ordered_artifact_ids = data.artifact_ids
        epoch = ng.ContextEpoch.create(data)
        with pytest.raises(ng.ContextStabilityError) as refused:
            ng.validate_context_layout(epoch, artifacts)
        assert refused.value.code == code


def test_a_change_of_layout_has_to_be_declared_and_a_receipt_records_real_boundaries():
    skill, note = _head("Verify before editing."), _tail("reading")
    legacy = _epoch([skill, note])
    data = ng.ContextEpochData()
    data.run_id, data.sequence, data.predecessor_id = "run_claim", 2, legacy.id
    data.feed_id, data.raw_from_sequence, data.raw_through_sequence = "hist_claim", 1, 6
    data.raw_window_digest = _sha("e")
    data.artifact_ids = data.ordered_artifact_ids = [skill.id, note.id]
    data.assembly_layout = ng.AssemblyLayout.StableFirst
    with pytest.raises(ng.ContextStabilityError) as refused:
        ng.verify_context_epoch_claim(ng.ContextEpoch.create(data), [skill, note], legacy, [skill, note])
    assert refused.value.code == ng.ContextStabilityRefusal.LayoutChangeUndeclared
    data.prefix_claim, data.break_reason = ng.ContextPrefixClaim.Breaks, ng.ContextBreakReason.LayoutChange
    declared = ng.ContextEpoch.create(data)
    ng.verify_context_epoch_claim(declared, [skill, note], legacy, [skill, note])

    receipt_data = ng.ContextAssemblyReceiptData()
    receipt_data.context_epoch_id = declared.id
    receipt_data.normalized_request_digest, receipt_data.message_window_digest = _sha("2"), _sha("3")
    receipt_data.artifact_ids = declared.artifact_ids
    receipt_data.required_skill_artifact_ids = [skill.id]
    receipt_data.raw_from_sequence, receipt_data.raw_through_sequence = 1, 6
    receipt_data.estimated_input_tokens, receipt_data.mandatory_input_tokens = 100, 10
    receipt_data.assembled_artifact_ids = [skill.id, note.id]
    receipt_data.artifact_classes = {skill.id: ng.StabilityClass.Stable, note.id: ng.StabilityClass.Volatile}
    receipt_data.class_sources = _sources([skill, note])
    receipt_data.class_rule_version = ng.STABILITY_DERIVATION_RULE_VERSION
    receipt_data.break_reason = ng.ContextBreakReason.LayoutChange
    receipt_data.stable_boundary = ng.ContextPrefixBoundary(1, 0)
    receipt_data.prefix_boundary = ng.ContextPrefixBoundary(4, 0)
    receipt_data.prefix_digest = _sha("5")
    receipt = ng.ContextAssemblyReceipt.create(receipt_data, declared, [skill, note])
    again = ng.ContextAssemblyReceipt.parse(receipt.serialize_canonical(), declared, [skill, note])
    assert again.id == receipt.id and again.prefix_boundary == ng.ContextPrefixBoundary(4, 0)
    receipt_data.stable_boundary = ng.ContextPrefixBoundary(5, 0)
    with pytest.raises(ValueError):
        ng.ContextAssemblyReceipt.create(receipt_data, declared, [skill, note])  # the stable end lies behind the prefix
    receipt_data.stable_boundary = ng.ContextPrefixBoundary(1, 0)
    receipt_data.prefix_boundary = None
    with pytest.raises(ValueError):
        ng.ContextAssemblyReceipt.create(receipt_data, declared, [skill, note])  # a digest names the prefix it covers


def test_a_receipt_reads_the_classes_as_they_were_written_under_an_older_derivation_rule():
    skill = ng.ContextArtifact.create(_skill_data())
    summary = ng.ContextArtifact.create(_summary_data())
    declared_data = _summary_data()
    declared_data.producer_id = "summarizer.v2"
    declared_data.stability_class = ng.StabilityClass.Slow
    declared = ng.ContextArtifact.create(declared_data)
    assert summary.stability_class == ng.StabilityClass.Volatile  # by the rule in force
    artifacts = [skill, summary, declared]
    epoch = _epoch(artifacts, order=False)

    data = ng.ContextAssemblyReceiptData()
    data.context_epoch_id = epoch.id
    data.normalized_request_digest, data.message_window_digest = _sha("2"), _sha("3")
    data.artifact_ids = epoch.artifact_ids
    data.required_skill_artifact_ids = [skill.id]
    data.raw_from_sequence, data.raw_through_sequence = 1, 5
    data.estimated_input_tokens, data.mandatory_input_tokens = 100, 10
    data.assembled_artifact_ids = [skill.id, summary.id, declared.id]
    # The receipt says Slow for the summary, as an older rule would have derived it.
    data.artifact_classes = {skill.id: ng.StabilityClass.Stable, summary.id: ng.StabilityClass.Slow,
                             declared.id: ng.StabilityClass.Slow}
    data.class_sources = _sources(artifacts)
    data.class_rule_version = ng.STABILITY_DERIVATION_RULE_VERSION
    with pytest.raises(ValueError):
        ng.ContextAssemblyReceipt.create(data, epoch, artifacts)  # the rule in force derives it again

    data.class_rule_version = ng.STABILITY_DERIVATION_RULE_VERSION - 1
    written = ng.ContextAssemblyReceipt.create(data, epoch, artifacts)
    stored = ng.ContextAssemblyReceipt.parse(written.serialize_canonical(), epoch, artifacts)
    assert stored.id == written.id
    assert stored.artifact_classes[summary.id] == ng.StabilityClass.Slow
    assert stored.class_sources[summary.id] == ng.ClassSource.Derived
    assert stored.class_sources[declared.id] == ng.ClassSource.Declared
    assert stored.class_rule_version == ng.STABILITY_DERIVATION_RULE_VERSION - 1

    # A declared class is strict whatever the rule version, and so is the source that says it was declared.
    data.artifact_classes = {**data.artifact_classes, declared.id: ng.StabilityClass.Stable}
    with pytest.raises(ValueError):
        ng.ContextAssemblyReceipt.create(data, epoch, artifacts)
    data.artifact_classes = {**data.artifact_classes, declared.id: ng.StabilityClass.Slow}
    data.class_sources = {**data.class_sources, declared.id: ng.ClassSource.Derived}
    with pytest.raises(ValueError):
        ng.ContextAssemblyReceipt.create(data, epoch, artifacts)


def test_an_extension_under_the_stable_first_layout_is_also_a_layout_the_assembler_accepts():
    skill, first_note, second_note = _head("Verify before editing."), _tail("reading 1"), _tail("reading 2")

    def epoch(artifacts, sequence, predecessor, layout, through):
        data = ng.ContextEpochData()
        data.run_id, data.sequence, data.predecessor_id = "run_claim", sequence, predecessor
        data.feed_id, data.raw_from_sequence, data.raw_through_sequence = "hist_claim", 1, through
        data.raw_window_digest = _sha("e")
        data.artifact_ids = data.ordered_artifact_ids = [artifact.id for artifact in artifacts]
        data.assembly_layout = layout
        if predecessor is not None:
            data.prefix_claim = ng.ContextPrefixClaim.Extends
        return ng.ContextEpoch.create(data)

    for layout, accepted in ((ng.AssemblyLayout.Legacy, True), (ng.AssemblyLayout.StableFirst, False)):
        before = epoch([skill, first_note], 1, None, layout, 5)
        after = epoch([second_note, skill], 2, before.id, layout, 6)
        if accepted:
            ng.verify_context_epoch_claim(after, [second_note, skill], before, [skill, first_note])
        else:
            with pytest.raises(ng.ContextStabilityError) as refused:
                ng.verify_context_epoch_claim(after, [second_note, skill], before, [skill, first_note])
            assert refused.value.code == ng.ContextStabilityRefusal.TierOrderViolated
