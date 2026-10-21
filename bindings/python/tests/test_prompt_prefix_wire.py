"""How much of each assembled request repeats the one before it, per family encoding (#329).

A provider's prompt cache reuses the start of a request that is identical, from its first byte, to an
earlier one. These tests assemble a scripted conversation of 18 turns through RuntimeTurnAssembler for
every family encoding, offline and without a key, keep the encoded body of every call, and compare each
body with the one before it. They pin what the layouts do today:

  stable-only     only the fixed block and the history
  append          the changing block is appended to the RAW feed, so the history only grows
  replace-tail    the changing block is an artifact at AfterHistory
  volatile-front  the changing block is an artifact at BeforeHistory
  before-latest-user  the changing block is an artifact at the default placement
  system-hoist    the changing block is a System artifact at AfterHistory
  tool-regroup    two parallel tool calls and their two results in every turn
  compaction      replace-tail, and at turn 10 the oldest exchanges leave the window for a summary

Each layout is asserted three ways: by where the first changed prompt item is, by the bytes that the new body
shares exactly with the previous one, and by a band for the share of the body that repeats (bytes, and
approximate tokens from a word-and-punctuation splitter). A change that moves bytes in front of the history
fails the first two; a change that lengthens or shortens what repeats fails the band. The table of the
ratios is printed at the end of the session.

A history of portable text has no native replay seal, and two families need one: Responses takes an assistant
message only with a seal, so its trace holds the user's turns alone, and Messages takes a tool call only with
one, so tool-regroup runs on Chat, OpenRouter and Gemini. Gemini shares the regrouping code with Messages.

An equal-sibling order test is marked as an expected failure: artifacts with the same kind, producer,
placement and priority are ordered by their id, a hash of their content, so a sibling whose text changes can
move the others. Issue #329 tracks an order that does not depend on content; when it lands the test passes
and its marker has to go (the marker is strict).
"""

import collections
import functools

import pytest

import prefix_trace as pt

PRESET_IDS = list(pt.PRESETS)
# Families that lift every System message into the system field, in front of the messages.
HOISTING = {"anthropic.messages", "google.generate"}
# A tool call in portable history needs a native replay seal in these families, so no offline trace has one.
NEEDS_NATIVE_TOOL_CALL = pt.NO_PORTABLE_TOOL_CALL


@functools.lru_cache(maxsize=None)
def trace(preset, layout):
    calls = pt.run_trace(preset, layout)
    pairs = pt.compare_all(calls)
    pt.ROWS.append(pt.summary_row(preset, layout, pairs))
    return calls, pairs


def cases(*layouts):
    return [(preset, layout) for layout in layouts for preset in PRESET_IDS
            if not (layout == "tool-regroup" and preset in NEEDS_NATIVE_TOOL_CALL)]


def case_ids(items):
    return [f"{preset}-{layout}" for preset, layout in items]


def parametrize_cases(*layouts):
    items = cases(*layouts)
    return pytest.mark.parametrize("preset,layout", items, ids=case_ids(items))


def describe(preset, layout):
    calls, pairs = trace(preset, layout)
    lines = [f"{preset} {layout}: call, bytes, tokens, bytes shared, first changed item of items before > after"]
    lines += [f"  turn {p.turn:2d}  {100 * p.bytes_ratio:5.1f}%  {100 * p.tokens_ratio:5.1f}%  {p.common_bytes:6d}  "
              f"{p.change_at:3d} of {p.items_before} > {p.items_after}" for p in pairs]
    return "\n".join(lines)


# Share of each body that repeats the previous one, mean over the trace: (low, high) for bytes and for tokens.
REPEATS = {
    "stable-only": ((0.95, 1.00), (0.95, 1.00)),
    "append": ((0.93, 1.00), (0.90, 1.00)),
    "replace-tail": ((0.93, 1.00), (0.90, 1.00)),
    "before-latest-user": ((0.91, 1.00), (0.88, 1.00)),
    "system-hoist": ((0.93, 1.00), (0.90, 1.00)),
    "tool-regroup": ((0.95, 1.00), (0.93, 1.00)),
    "compaction": ((0.91, 1.00), (0.88, 1.00)),
    # The changing block sits in front of the history: only the fixed block repeats.
    "volatile-front": ((0.60, 0.90), (0.55, 0.88)),
}
# A System artifact that the family lifts into the system field behaves like volatile-front.
HOISTED_SYSTEM = REPEATS["volatile-front"]

# The prompt of the third call as labels: F fixed block, V changing block, Q question, A reply, C summary.
ORDER_OF_THIRD_CALL = {
    "stable-only": "system[F] Q A Q A Q",
    "append": "system[F] V Q A V Q A V Q",
    "replace-tail": "system[F] Q A Q A Q V",
    "volatile-front": "system[F] V Q A Q A Q",
    "before-latest-user": "system[F] Q A Q A V Q",
}


def in_band(value, band):
    return band[0] <= value <= band[1]


def expected_band(preset, layout):
    if layout == "system-hoist" and preset in HOISTING:
        return HOISTED_SYSTEM
    return REPEATS[layout]


@parametrize_cases("stable-only", "append", "replace-tail", "volatile-front", "before-latest-user", "system-hoist",
                   "tool-regroup", "compaction")
def test_share_that_repeats_stays_in_its_band(preset, layout):
    _, pairs = trace(preset, layout)
    bytes_band, tokens_band = expected_band(preset, layout)
    by_bytes = pt.mean(p.bytes_ratio for p in pairs)
    by_tokens = pt.mean(p.tokens_ratio for p in pairs)
    assert in_band(by_bytes, bytes_band) and in_band(by_tokens, tokens_band), (
        f"mean {by_bytes:.3f} bytes (band {bytes_band}), {by_tokens:.3f} tokens (band {tokens_band})\n"
        + describe(preset, layout))


@parametrize_cases("stable-only", "append", "tool-regroup")
def test_a_call_that_only_appends_extends_the_previous_body(preset, layout):
    """Nothing in front of the new messages changes: the previous body up to its last message is a prefix."""
    _, pairs = trace(preset, layout)
    for pair in pairs:
        assert pair.change_at == pair.items_before, describe(preset, layout)
        assert pair.common_bytes >= pair.before_last[1], describe(preset, layout)


def replaced_tail(preset, layout):
    """The comparisons in which the previous call's last message gives way to the new messages."""
    _, pairs = trace(preset, layout)
    if layout == "compaction":
        return [pair for pair in pairs if pair.turn != 10]
    return pairs


@parametrize_cases("replace-tail", "compaction")
def test_a_changing_tail_leaves_every_byte_in_front_of_it_shared(preset, layout):
    """The case that OpenAI cached only to the end of the fixed block (82% of the input).

    The previous body ends with the changing block. The new body repeats every byte in front of it, the whole
    history so far, and first differs inside that block (its message gives way to the next reply), so the
    encoding offers the provider a prefix that grows by a turn with every call."""
    for pair in replaced_tail(preset, layout):
        start, end = pair.before_last
        assert pair.change_at == pair.items_before - 1, describe(preset, layout)
        assert start <= pair.common_bytes < end, describe(preset, layout)


@pytest.mark.parametrize("preset", [name for name in PRESET_IDS if name not in HOISTING])
def test_a_system_block_at_the_tail_stays_at_the_tail_where_a_family_keeps_system_in_place(preset):
    _, pairs = trace(preset, "system-hoist")
    for pair in pairs:
        start, end = pair.before_last
        assert pair.change_at == pair.items_before - 1, describe(preset, "system-hoist")
        assert start <= pair.common_bytes < end, describe(preset, "system-hoist")


@pytest.mark.parametrize("preset", sorted(HOISTING))
def test_a_system_block_moves_to_the_front_in_a_family_that_lifts_system_text(preset):
    """The lifted text is the start of the prompt, so a System block that changes rewrites everything after it."""
    calls, pairs = trace(preset, "system-hoist")
    for pair in pairs:
        assert pair.change_at == 0, describe(preset, "system-hoist")
    assert pt.order_of(calls[2]) == "system[FV] Q A Q A Q"


@parametrize_cases("volatile-front")
def test_a_changing_block_in_front_of_the_history_is_the_first_change(preset, layout):
    """Right after the fixed block, whatever the history behind it: that is all a provider can reuse."""
    _, pairs = trace(preset, layout)
    for pair in pairs:
        assert pair.change_at == 1, describe(preset, layout)


@parametrize_cases("before-latest-user")
def test_the_default_placement_moves_with_the_latest_user_message(preset, layout):
    """The block sits in front of the newest question, so the previous call's block and question are replaced."""
    _, pairs = trace(preset, layout)
    for pair in pairs:
        assert pair.change_at == pair.items_before - 2, describe(preset, layout)


@pytest.mark.parametrize("preset", PRESET_IDS)
def test_compaction_rewrites_what_follows_the_fixed_block_once(preset):
    """The summary joins right after the fixed block and the oldest exchanges leave: the first change is there.

    The call after it extends the new prefix again; test_a_changing_tail_... covers it and every other call."""
    calls, pairs = trace(preset, "compaction")
    compacting = [pair for pair in pairs if pair.turn == 10]
    assert len(compacting) == 1 and compacting[0].change_at == 1, describe(preset, "compaction")
    assert pt.order_of(calls[9]).startswith("system[F] C Q")
    following = [pair for pair in pairs if pair.turn == 11]
    assert len(following) == 1 and following[0].change_at == following[0].items_before - 1


@parametrize_cases("stable-only", "append", "replace-tail", "volatile-front", "before-latest-user")
def test_assembled_order(preset, layout):
    calls, _ = trace(preset, layout)
    expected = ORDER_OF_THIRD_CALL[layout]
    if preset in pt.NO_ASSISTANT:
        expected = " ".join(item for item in expected.split() if item != "A")
    assert pt.order_of(calls[2]) == expected


@pytest.mark.parametrize("preset", PRESET_IDS)
def test_epoch_identity_is_not_part_of_the_bytes(preset):
    """A new run id, and so a new epoch id for every call, over the same window and artifacts: the same bodies."""
    first = [call.body for call in pt.Conversation(preset, "replace-tail", turns=6).run()]
    again = [call.body for call in pt.Conversation(preset, "replace-tail", turns=6, run_id="another-run").run()]
    assert again == first


@pytest.mark.parametrize("preset,layout", [("openai.responses", "stable-only"), ("anthropic.messages", "tool-regroup")])
def test_a_family_that_needs_a_native_seal_refuses_the_portable_history_of_the_trace(preset, layout):
    """Why these families have a shorter trace: Responses takes no assistant message without a native seal, and
    Messages takes no tool call without one. A family that admits them later should get the full trace."""
    conversation = pt.Conversation(preset, layout, turns=3)
    conversation.replies = True  # Responses: the trace leaves its replies out
    with pytest.raises(RuntimeError, match="native replay ineligible"):
        conversation.run()


@pytest.mark.xfail(strict=True, reason="equal siblings are ordered by artifact id, a content hash; #329 replaces the "
                   "tie-break with an order that does not depend on content")
@pytest.mark.parametrize("kind", [pt.KIND.DerivedContext, pt.KIND.HardConstraint], ids=["user", "system"])
@pytest.mark.parametrize("preset", ["openai.chat", "anthropic.messages"])
def test_a_sibling_whose_text_changes_does_not_move_the_others(preset, kind):
    """Three artifacts share kind, producer, placement and priority; one changes its text on each of 100 calls.

    The other two should keep their place in the prompt, and so should the one that changes. Today it is ordered
    among them by its id, a hash of its text, so it lands in front of, between and behind them from call to call
    and moves them with it. A System artifact is lifted into the system field by Messages, where the order is the
    order of the text."""
    orders = collections.Counter(pt.sibling_order(body) for body in pt.sibling_bodies(preset, kind))
    assert len(orders) == 1, dict(orders)
