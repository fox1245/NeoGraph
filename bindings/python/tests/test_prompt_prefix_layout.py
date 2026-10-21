"""The stable-first assembly layout against the legacy one, per family encoding (#329).

test_prompt_prefix_wire.py pins what the legacy layout does. These tests run the same scripted conversation
through the stable-first layout, where the epoch records the order of its artifacts and the layout puts the
host instructions, the Stable and the Slow artifacts in front of the history and the Volatile ones behind it:

  * where both layouts mean the same prompt, the bodies are byte for byte those of the legacy layout, for
    every call of every family (stable-only, append, replace-tail, tool-regroup, compaction);
  * a placement the layout cannot honour is refused with a typed error, not moved: a changing block in front
    of the history, one at the default placement, a System block behind the history;
  * the receipt names the two boundaries, read from the encoded body after hoisting and regrouping, and a
    digest of the request up to the end of the append-only part;
  * the share of a body that repeats the one before it, in bytes and approximate tokens, stays in a band
    over the 18 turns, and the order of equal siblings does not depend on their text.
"""

import json

import neograph_engine as ng
import pytest

import prefix_trace as pt

PRESET_IDS = list(pt.PRESETS)
HOISTING = {"anthropic.messages", "google.generate"}
SAME_AS_LEGACY = ["stable-only", "append", "replace-tail", "tool-regroup", "compaction"]
REFUSED = {"volatile-front": ng.ContextStabilityRefusal.VolatileOutsideTail,
           "before-latest-user": ng.ContextStabilityRefusal.VolatileOutsideTail,
           "system-hoist": ng.ContextStabilityRefusal.StableOutsideHistoryHead}


def cases(*layouts):
    return [(preset, layout) for layout in layouts for preset in PRESET_IDS
            if not (layout == "tool-regroup" and preset in pt.NO_PORTABLE_TOOL_CALL)]


def parametrize_cases(*layouts):
    items = cases(*layouts)
    return pytest.mark.parametrize("preset,layout", items, ids=[f"{preset}-{layout}" for preset, layout in items])


_traces = {}


def run(preset, layout, assembly):
    """The conversation, kept: the receipts and the epochs are read by several tests."""
    key = (preset, layout, assembly)
    if key not in _traces:
        conversation = pt.Conversation(preset, layout, assembly=assembly)
        conversation.run()
        _traces[key] = conversation
    return _traces[key]


@parametrize_cases(*SAME_AS_LEGACY)
def test_where_the_layouts_mean_the_same_prompt_the_bodies_are_the_legacy_bodies(preset, layout):
    stable_first = run(preset, layout, "stable_first").bodies
    legacy = run(preset, layout, "legacy").bodies
    assert len(stable_first) == len(legacy)
    for new, old in zip(stable_first, legacy):
        assert new.body == old.body, f"{preset} {layout} call {new.turn}"


@pytest.mark.parametrize("preset", PRESET_IDS)
@pytest.mark.parametrize("layout", sorted(REFUSED))
def test_a_placement_the_layout_cannot_honour_is_refused_and_not_moved(preset, layout):
    with pytest.raises(ng.ContextStabilityError) as refused:
        pt.Conversation(preset, layout, turns=3, assembly="stable_first").run()
    assert refused.value.code == REFUSED[layout]
    assert isinstance(refused.value, ValueError)


@parametrize_cases(*SAME_AS_LEGACY, "stable-system")
def test_the_receipt_follows_the_order_and_the_classes_of_the_epoch(preset, layout):
    conversation = run(preset, layout, "stable_first")
    for call, (epoch, artifacts) in zip(conversation.bodies, conversation.epochs):
        receipt = call.receipt
        assert receipt.assembled_artifact_ids == epoch.ordered_artifact_ids
        assert sorted(receipt.assembled_artifact_ids) == epoch.artifact_ids
        assert epoch.assembly_layout == ng.AssemblyLayout.StableFirst
        assert receipt.class_rule_version == ng.STABILITY_DERIVATION_RULE_VERSION
        declared = {artifact.id for artifact in artifacts if artifact.declared_stability_class is not None}
        assert {artifact_id for artifact_id, source in receipt.class_sources.items()
                if source == ng.ClassSource.Declared} == declared
        classes = [receipt.artifact_classes[artifact_id] for artifact_id in receipt.assembled_artifact_ids]
        assert classes == sorted(classes, key=list(ng.StabilityClass.__members__.values()).index)
        assert receipt.encoder_family == pt.PRESETS[preset][0]
        stored = ng.ContextAssemblyReceipt.parse(receipt.serialize_canonical(), epoch, artifacts)
        assert (stored.id, stored.stable_boundary, stored.prefix_boundary, stored.prefix_digest) == (
            receipt.id, receipt.stable_boundary, receipt.prefix_boundary, receipt.prefix_digest)


# --- the boundaries, found again in the body ------------------------------------------------------------

def message_parts(message):
    """The parts of one element of the body's message array."""
    if isinstance(message, dict):
        for key in ("content", "parts"):
            if isinstance(message.get(key), list):
                return message[key]
        return [message]
    return [message]


def body_parts(body):
    """(message index, part index, text) of every part of the body's messages, in order."""
    document = json.loads(body)
    for key in ("messages", "input", "contents"):
        if key in document:
            return [(m, p, pt.content_text(part)) for m, message in enumerate(document[key])
                    for p, part in enumerate(message_parts(message))], len(document[key])
    return [], 0


def first_part_with(body, marker):
    parts, count = body_parts(body)
    for m, p, text in parts:
        if marker in text:
            return (m, p)
    return (count, 0)


def first_part_not_in_the_head(body, head_markers):
    """The first part of the messages that holds none of the markers of the Stable (and Slow) head."""
    parts, count = body_parts(body)
    for m, p, text in parts:
        if not any(marker in text for marker in head_markers):
            return (m, p)
    return (count, 0)


@parametrize_cases("replace-tail", "compaction", "stable-system")
def test_the_boundaries_are_where_the_encoded_body_changes_from_stable_to_volatile(preset, layout):
    """Found again from the text of the body, without the code that wrote them: the append-only part ends at the
    first part that holds the changing readings, and the Stable part ends in front of the first part that does not
    hold the fixed block. A family that lifts System text into the system field has the fixed block there, and
    its first message is already outside the Stable part."""
    for call in run(preset, layout, "stable_first").bodies:
        receipt = call.receipt
        prefix = receipt.prefix_boundary
        assert (prefix.message_index, prefix.part_index) == first_part_with(call.body, "Live readings"), (
            f"{preset} {layout} call {call.turn}")
        stable = receipt.stable_boundary
        if layout == "stable-system" and preset in HOISTING:
            # The standing rule is Slow and shares the system field with the fixed block. The field is one
            # block, so the end of the Stable part is not a place this family can mark.
            assert stable is None, f"{preset} {layout} call {call.turn}"
        else:
            assert (stable.message_index, stable.part_index) == first_part_not_in_the_head(
                call.body, ["Entry 001"]), f"{preset} {layout} call {call.turn}"
            assert (stable.message_index, stable.part_index) <= (prefix.message_index, prefix.part_index)


@parametrize_cases("stable-only", "append", "tool-regroup")
def test_a_request_without_a_volatile_artifact_is_a_prefix_as_a_whole(preset, layout):
    """Nothing is behind the history, so the append-only part is the whole body."""
    for call in run(preset, layout, "stable_first").bodies:
        _, count = body_parts(call.body)
        prefix = call.receipt.prefix_boundary
        assert (prefix.message_index, prefix.part_index) == (count, 0), f"{preset} {layout} call {call.turn}"


@pytest.mark.parametrize("preset", PRESET_IDS)
def test_the_prefix_digest_follows_the_prefix_and_not_the_volatile_text(preset):
    plain = run(preset, "replace-tail", "stable_first").bodies
    salted = pt.Conversation(preset, "replace-tail", assembly="stable_first", volatile_salt="another reading").run()
    assert [call.body for call in salted] != [call.body for call in plain]
    assert [call.receipt.prefix_digest for call in salted] == [call.receipt.prefix_digest for call in plain]
    digests = [call.receipt.prefix_digest for call in plain]
    assert len(set(digests)) == len(digests), "the history grows, so the prefix is another one every call"


@pytest.mark.parametrize("preset", PRESET_IDS)
def test_the_hoisted_system_field_holds_the_stable_text_then_the_slow_text_and_never_a_volatile_one(preset):
    bodies = run(preset, "stable-system", "stable_first").bodies
    systems = {text for call in bodies for role, text in call.items if role == "system"}
    if preset in HOISTING:
        assert len(systems) == 1, "the system field is the same in every call"
        system, = systems
        assert system.index("Entry 001") < system.index("Standing rule, declared Slow")
        assert "Live readings" not in system
    else:
        # The family keeps System messages where they are: the Stable one, then the Slow one, ahead of the history.
        for call in bodies:
            roles = [(role, text) for role, text in call.items]
            first, second = roles[0], roles[1]
            assert "Entry 001" in first[1] and "Standing rule, declared Slow" in second[1]


@pytest.mark.parametrize("preset", PRESET_IDS)
def test_a_call_after_a_compaction_extends_it_and_the_compaction_is_declared_a_break(preset):
    conversation = run(preset, "compaction", "stable_first")
    claims = [(epoch.prefix_claim, epoch.break_reason) for epoch, _ in conversation.epochs]
    assert claims[0] == (None, None)
    for index, (claim, reason) in enumerate(claims[1:], start=2):
        if index == 10:
            assert (claim, reason) == (ng.ContextPrefixClaim.Breaks, ng.ContextBreakReason.Compaction)
        else:
            assert (claim, reason) == (ng.ContextPrefixClaim.Extends, None), f"call {index}"
    # The call that compacts breaks the prefix after the fixed block, which is where the Stable part ends.
    calls = conversation.bodies
    assert calls[9].receipt.break_reason == ng.ContextBreakReason.Compaction
    assert calls[10].receipt.break_reason is None


@pytest.mark.parametrize("preset", PRESET_IDS)
def test_the_compaction_call_rewrites_what_follows_the_fixed_block_and_the_next_call_extends_it(preset):
    """The summary is Slow: it joins right after the fixed block, once, and the call after it only appends."""
    calls = run(preset, "compaction", "stable_first").bodies
    pairs = pt.compare_all(calls)
    compacting = [pair for pair in pairs if pair.turn == 10]
    assert len(compacting) == 1 and compacting[0].change_at == 1
    following = [pair for pair in pairs if pair.turn == 11]
    assert len(following) == 1 and following[0].change_at == following[0].items_before - 1
    assert pt.order_of(calls[9]).startswith("system[F] C Q")


# --- the share that repeats ------------------------------------------------------------------------------

# Mean over the 18-turn trace: (low, high) for bytes and for tokens. The bodies are those of the legacy layout
# for these layouts, so the bands are the legacy ones.
REPEATS = {
    "stable-only": ((0.95, 1.00), (0.95, 1.00)),
    "append": ((0.93, 1.00), (0.90, 1.00)),
    "replace-tail": ((0.93, 1.00), (0.90, 1.00)),
    "tool-regroup": ((0.95, 1.00), (0.93, 1.00)),
    "compaction": ((0.91, 1.00), (0.88, 1.00)),
}


@parametrize_cases(*SAME_AS_LEGACY)
def test_the_share_that_repeats_stays_in_its_band(preset, layout):
    pairs = pt.compare_all(run(preset, layout, "stable_first").bodies)
    bytes_band, tokens_band = REPEATS[layout]
    by_bytes = pt.mean(pair.bytes_ratio for pair in pairs)
    by_tokens = pt.mean(pair.tokens_ratio for pair in pairs)
    assert bytes_band[0] <= by_bytes <= bytes_band[1] and tokens_band[0] <= by_tokens <= tokens_band[1], (
        f"{preset} {layout}: {by_bytes:.3f} bytes, {by_tokens:.3f} tokens")


# --- the order of equal siblings -------------------------------------------------------------------------

@pytest.mark.parametrize("kind", [pt.KIND.DerivedContext, pt.KIND.HardConstraint], ids=["user", "system"])
@pytest.mark.parametrize("preset", ["openai.chat", "anthropic.messages"])
def test_a_sibling_whose_text_changes_does_not_move_the_others(preset, kind):
    """The test that the legacy layout fails (see test_prompt_prefix_wire.py): three artifacts share kind,
    producer, placement and priority and one changes its text on each of 100 calls. The epoch records their
    order, made from a key and not from their text, so every call carries them in the same order."""
    orders = {pt.sibling_order(body) for body in pt.sibling_bodies_stable_first(preset, kind)}
    assert orders == {("alpha", "bravo", "live")}, orders


# --- the layout is declared, and a change of it is a break --------------------------------------------------

def test_the_first_stable_first_epoch_after_a_legacy_one_declares_the_break():
    legacy = pt.Conversation("openai.chat", "replace-tail", turns=2)
    legacy.run()
    stable_first = pt.Conversation("openai.chat", "replace-tail", turns=2, assembly="stable_first")
    stable_first.run()
    (before, before_artifacts), (after, after_artifacts) = legacy.epochs[-1], stable_first.epochs[0]
    data = ng.ContextEpochData()
    data.run_id = "trace-run"
    data.sequence = before.sequence + 1
    data.predecessor_id = before.id
    data.feed_id = after.feed_id
    data.raw_from_sequence, data.raw_through_sequence = after.raw_from_sequence, after.raw_through_sequence
    data.raw_window_digest = after.raw_window_digest
    data.artifact_ids = after.artifact_ids
    data.ordered_artifact_ids = after.ordered_artifact_ids
    data.assembly_layout = ng.AssemblyLayout.StableFirst
    undeclared = ng.ContextEpoch.create(data)
    with pytest.raises(ng.ContextStabilityError) as refused:
        ng.verify_context_epoch_claim(undeclared, after_artifacts, before, before_artifacts)
    assert refused.value.code == ng.ContextStabilityRefusal.LayoutChangeUndeclared
    data.prefix_claim = ng.ContextPrefixClaim.Breaks
    data.break_reason = ng.ContextBreakReason.LayoutChange
    ng.verify_context_epoch_claim(ng.ContextEpoch.create(data), after_artifacts, before, before_artifacts)
