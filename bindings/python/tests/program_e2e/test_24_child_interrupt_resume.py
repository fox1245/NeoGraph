"""A child Program that pauses on an interrupt, started by a parent's main() with ng.spawn (#658).

The child is answered through its own run id, whether the parent awaits it or not. A parent that awaits
the pausing child ends Interrupted and keeps the relation Dispatched; answering the child continues the
parent, which gets the child's result. Resuming the parent itself is refused with P_CHILD_INTERRUPTED,
which names the child run to resume.
"""

import time

import pytest

import neograph_engine as ng

from . import support as s

# The child asks a question in its Core graph and records the answer.
CHILD = s.with_main(
    s.define_graph("""
        g.node("ask", {type: "e2e.step", ask: "approve?"});
        g.node("done", {type: "e2e.step"});
        g.entry("ask");
        g.edge("ask", "done");
        g.exit("done");
    """, name="writer"),
    """
    const out = yield ng.callCore("writer", input, "child:core");
    return {trace: out.trace, answer: out.answer};
    """,
)

ROOT_GRAPH = s.define_graph("""
    g.node("r", {type: "e2e.step"});
    g.entry("r");
    g.exit("r");
""", name="root")

# The parent awaits its child after a Core call of its own.
AWAIT_AFTER_CORE_CALL = s.with_main(ROOT_GRAPH, """
    const pre = yield ng.callCore("root", {}, "root:pre");
    const res = yield ng.await(ng.spawn("writer", {chapter: 1}, "spawn:w"), 20000, "await:w");
    return {pre: pre.trace, child: res};
""")

# The parent's first command is the await.
AWAIT_FIRST = s.with_main(ROOT_GRAPH, """
    const res = yield ng.await(ng.spawn("writer", {chapter: 1}, "spawn:w"), 20000, "await:w");
    const post = yield ng.callCore("root", {}, "root:post");
    return {child: res, post: post.trace};
""")

# The parent spawns the child without awaiting it, then keeps working in a Core node that waits.
SPAWN_THEN_HOLD = s.with_main(
    s.define_graph("""
        g.node("hold", {type: "e2e.step", delay_ms: 30000});
        g.entry("hold");
        g.exit("hold");
    """, name="root"),
    """
    const spawned = yield ng.spawn("writer", {chapter: 1}, "spawn:w");
    const out = yield ng.callCore("root", {}, "root:hold");
    return {child: spawned.child_run_id, out: out.trace};
    """,
)

# The parent only holds; the host starts the child itself.
HOLD_ONLY = s.with_main(
    s.define_graph("""
        g.node("hold", {type: "e2e.step", delay_ms: 30000});
        g.entry("hold");
        g.exit("hold");
    """, name="root"),
    """
    const out = yield ng.callCore("root", {}, "root:hold");
    return {out: out.trace};
    """,
)

ANSWER = {"approved": True}


def _setup(parent_text):
    host = s.make_host(budget_ceiling=s.ceiling(max_child_depth=2, max_total_children=4))
    child_budget = s.budget(wall_time_ms=15_000, max_concurrency=1, max_core_steps=20,
                            max_program_operations=8)
    child_version = s.admit(host, CHILD, child_budget, "child.js")
    parent_budget = s.budget(wall_time_ms=60_000, max_core_steps=50, max_program_operations=16,
                             max_child_depth=1, max_total_children=2)
    parent_version = s.admit(host, parent_text, parent_budget, "parent.js")
    link = host.bind_child(parent_version, "writer", child_version, child_budget)
    return host, parent_version, parent_budget, child_version, child_budget, link


def _paused_child(host, parent_handle):
    """The one child relation of a running parent, once the child has paused."""
    for _ in range(500):
        record = host.run_record(parent_handle.run_id)
        if record is not None and len(record.children) == 1 and record.children[0].terminal_result is not None:
            return record.children[0]
        time.sleep(0.02)
    raise AssertionError("the child did not pause")


def _end(parent_handle):
    parent_handle.cancel()
    parent_handle.wait()


def test_spawned_child_that_pauses_is_answered_through_its_own_run_id():
    host, version, run_budget, *_ = _setup(SPAWN_THEN_HOLD)
    parent = host.start(version, {}, run_budget)
    try:
        relation = _paused_child(host, parent)
        assert relation.state == ng.ProgramChildState.Dispatched
        assert relation.terminal_result.status == ng.ProgramTerminalStatus.Interrupted

        resumed = host.resume(relation.child_run_id, ANSWER).wait()

        assert resumed.status == ng.ProgramTerminalStatus.Completed, s.describe(resumed)
        assert resumed.output["answer"] == ANSWER
        record = host.run_record(parent.run_id)
        assert [child.state for child in record.children] == [ng.ProgramChildState.Completed]
    finally:
        _end(parent)


def test_child_started_by_the_host_that_pauses_is_answered_through_its_own_run_id():
    host, version, run_budget, child_version, child_budget, link = _setup(HOLD_ONLY)
    parent = host.start(version, {}, run_budget)
    try:
        child = host.start_child(parent, link, child_version, {"chapter": 1}, child_budget)
        paused = child.wait()
        assert paused.status == ng.ProgramTerminalStatus.Interrupted, s.describe(paused)

        resumed = host.resume(child.run_id, ANSWER).wait()

        assert resumed.status == ng.ProgramTerminalStatus.Completed, s.describe(resumed)
        assert resumed.output["answer"] == ANSWER
    finally:
        _end(parent)


def test_awaited_child_that_pauses_is_answered_through_its_own_run_id():
    """The child of an awaiting parent resumes through its own run id and completes."""
    host, version, run_budget, *_ = _setup(AWAIT_FIRST)
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    relation = host.run_record(result.run_id).children[0]
    assert relation.state == ng.ProgramChildState.Dispatched

    resumed = host.resume(relation.child_run_id, ANSWER).wait()

    assert resumed.status == ng.ProgramTerminalStatus.Completed, s.describe(resumed)
    assert resumed.output["answer"] == ANSWER


def _parent_result(host, parent_run_id, timeout=20.0):
    """The parent after its child was answered. It continues on its own, ancestor by ancestor: wait until its
    record ends, then read the result."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        record = host.run_record(parent_run_id)
        if record.terminal_result is not None and record.terminal_result.status != ng.ProgramTerminalStatus.Interrupted:
            return host.reconnect(parent_run_id).wait()
        time.sleep(0.02)
    raise AssertionError("the parent did not continue on its own")


@pytest.mark.parametrize("program", [AWAIT_FIRST, AWAIT_AFTER_CORE_CALL], ids=["await_first", "await_after_core_call"])
def test_parent_that_awaits_a_pausing_child_ends_interrupted_with_the_relation_dispatched(program):
    host, version, run_budget, *_ = _setup(program)
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    assert result.failure is None
    relation = host.run_record(result.run_id).children[0]
    assert relation.state == ng.ProgramChildState.Dispatched
    assert relation.terminal_result.status == ng.ProgramTerminalStatus.Interrupted


@pytest.mark.parametrize("program", [AWAIT_FIRST, AWAIT_AFTER_CORE_CALL], ids=["await_first", "await_after_core_call"])
def test_answering_the_child_continues_the_parent_awaiting_it(program):
    host, version, run_budget, *_ = _setup(program)
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    child_id = host.run_record(result.run_id).children[0].child_run_id

    answered = host.resume(child_id, ANSWER).wait()
    assert answered.status == ng.ProgramTerminalStatus.Completed, s.describe(answered)

    final = _parent_result(host, result.run_id)
    assert final.status == ng.ProgramTerminalStatus.Completed, s.describe(final)
    assert final.output["child"]["answer"] == ANSWER
    assert [child.state for child in host.run_record(result.run_id).children] == [ng.ProgramChildState.Completed]


def test_a_child_is_answered_while_the_handle_of_its_interrupted_parent_is_held():
    """A held handle of the paused parent does not cancel the child that is resumed (#658)."""
    host, version, run_budget, *_ = _setup(AWAIT_FIRST)
    parent = host.start(version, {}, run_budget)
    paused = parent.wait()
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, s.describe(paused)
    child_id = host.run_record(parent.run_id).children[0].child_run_id

    answered = host.resume(child_id, ANSWER).wait()

    assert answered.status == ng.ProgramTerminalStatus.Completed, s.describe(answered)
    final = _parent_result(host, parent.run_id)
    assert final.status == ng.ProgramTerminalStatus.Completed, s.describe(final)
    assert final.output["child"]["answer"] == ANSWER


def test_an_answer_after_the_await_deadline_ends_the_parent_timed_out():
    host, version, run_budget, *_ = _setup(AWAIT_FIRST.replace("20000", "400"))
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    child_id = host.run_record(result.run_id).children[0].child_run_id
    time.sleep(1.0)

    assert host.resume(child_id, ANSWER).wait().status == ng.ProgramTerminalStatus.Completed

    final = _parent_result(host, result.run_id)
    assert final.status == ng.ProgramTerminalStatus.TimedOut, s.describe(final)
    assert final.failure.code == "P_AWAIT_TIMEOUT"


TWO_QUESTIONS = s.with_main(
    s.define_graph("""
        g.node("first", {type: "e2e.step", ask: "first?"});
        g.node("second", {type: "e2e.step", ask: "second?"});
        g.entry("first");
        g.edge("first", "second");
        g.exit("second");
    """, name="writer"),
    """
    const out = yield ng.callCore("writer", input, "child:core");
    return {trace: out.trace, answer: out.answer};
    """,
)


def test_a_child_that_pauses_again_after_its_answer_stays_resumable_and_then_continues_the_parent():
    host = s.make_host(budget_ceiling=s.ceiling(max_child_depth=2, max_total_children=4))
    child_budget = s.budget(wall_time_ms=15_000, max_concurrency=1, max_core_steps=20, max_program_operations=8)
    parent_budget = s.budget(wall_time_ms=60_000, max_core_steps=50, max_program_operations=16,
                             max_child_depth=1, max_total_children=2)
    child_version = s.admit(host, TWO_QUESTIONS, child_budget, "child.js")
    parent_version = s.admit(host, AWAIT_FIRST, parent_budget, "parent.js")
    host.bind_child(parent_version, "writer", child_version, child_budget)
    result = host.run(parent_version, {}, parent_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    child_id = host.run_record(result.run_id).children[0].child_run_id

    again = host.resume(child_id, ANSWER).wait()
    assert again.status == ng.ProgramTerminalStatus.Interrupted, s.describe(again)
    paused = host.run_record(result.run_id).terminal_result
    assert paused is not None
    assert paused.status == ng.ProgramTerminalStatus.Interrupted
    assert paused.id == result.id
    assert paused.attempt == 1

    assert host.resume(child_id, ANSWER).wait().status == ng.ProgramTerminalStatus.Completed
    final = _parent_result(host, result.run_id)
    assert final.status == ng.ProgramTerminalStatus.Completed, s.describe(final)
    assert final.attempt == 2


def test_the_continued_parent_spends_the_budget_of_its_journal_and_gets_no_new_grant():
    host, version, run_budget, *_ = _setup(AWAIT_AFTER_CORE_CALL)
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    assert result.usage.core_steps == 1
    assert result.remaining_budget.max_core_steps == 0
    # The child's grant is reserved, not taken out of the parent's remainder; the Core call, the spawn and the
    # await are charged to the parent.
    assert result.remaining_budget.max_program_operations == run_budget.max_program_operations - 3
    child_id = host.run_record(result.run_id).children[0].child_run_id
    assert host.resume(child_id, ANSWER).wait().status == ng.ProgramTerminalStatus.Completed

    final = _parent_result(host, result.run_id)

    assert final.status == ng.ProgramTerminalStatus.Completed, s.describe(final)
    # Wall time is charged while the parent runs, not while it is paused, and is never granted again.
    assert final.remaining_budget.wall_time_ms <= run_budget.wall_time_ms
    # The Core call, the spawn and the await were charged once: replaying them for the continuation was not a grant.
    assert final.remaining_budget.max_program_operations == result.remaining_budget.max_program_operations
    assert final.remaining_budget.max_core_steps == run_budget.max_core_steps - result.usage.core_steps
    # The continuation joined the dispatched child and spawned no other.
    assert final.remaining_budget.max_total_children == result.remaining_budget.max_total_children
    assert len(host.run_record(result.run_id).children) == 1


def test_resuming_the_parent_of_a_pausing_child_names_the_child_to_resume():
    host, version, run_budget, *_ = _setup(AWAIT_FIRST)
    result = host.run(version, {}, run_budget)
    child_id = host.run_record(result.run_id).children[0].child_run_id

    with pytest.raises(ng.ProgramDiagnosticError) as raised:
        host.resume(result.run_id, ANSWER, result.interrupt.pending_input.call_id)

    assert raised.value.code == "P_CHILD_INTERRUPTED"
    assert raised.value.witness["child_run_ids"] == [child_id]
    # Nothing was consumed: the child still answers.
    assert host.resume(child_id, ANSWER).wait().status == ng.ProgramTerminalStatus.Completed


def test_a_parent_paused_on_a_child_continues_after_a_restart(tmp_path):
    directory = str(tmp_path / "runs")

    def durable_host():
        host = s.make_host(budget_ceiling=s.ceiling(max_child_depth=2, max_total_children=4),
                           sqlite_directory=directory)
        child_budget = s.budget(wall_time_ms=15_000, max_concurrency=1, max_core_steps=20,
                                max_program_operations=8)
        parent_budget = s.budget(wall_time_ms=60_000, max_core_steps=50, max_program_operations=16,
                                 max_child_depth=1, max_total_children=2)
        child_version = s.admit(host, CHILD, child_budget, "child.js")
        parent_version = s.admit(host, AWAIT_AFTER_CORE_CALL, parent_budget, "parent.js")
        host.bind_child(parent_version, "writer", child_version, child_budget)
        return host, parent_version, parent_budget

    host, version, run_budget = durable_host()
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    child_id = host.run_record(result.run_id).children[0].child_run_id
    del host

    restarted, _version, _budget = durable_host()
    answered = restarted.resume(child_id, ANSWER).wait()
    assert answered.status == ng.ProgramTerminalStatus.Completed, s.describe(answered)
    final = _parent_result(restarted, result.run_id)
    assert final.status == ng.ProgramTerminalStatus.Completed, s.describe(final)
    assert final.output["child"]["answer"] == ANSWER


# Three levels: the root awaits a child, which awaits a grandchild that asks.
MIDDLE = s.with_main(ROOT_GRAPH, """
    const res = yield ng.await(ng.spawn("writer", {chapter: 2}, "spawn:g"), 20000, "await:g");
    return {grandchild: res};
""")
ROOT_OF_THREE = s.with_main(ROOT_GRAPH, """
    const res = yield ng.await(ng.spawn("middle", {chapter: 1}, "spawn:m"), 30000, "await:m");
    return {middle: res};
""")


def _three_levels():
    host = s.make_host(budget_ceiling=s.ceiling(max_child_depth=3, max_total_children=6))
    grandchild_budget = s.budget(wall_time_ms=10_000, max_concurrency=1, max_core_steps=10,
                                 max_program_operations=8)
    middle_budget = s.budget(wall_time_ms=40_000, max_concurrency=1, max_core_steps=30,
                             max_program_operations=16, max_child_depth=1, max_total_children=1)
    root_budget = s.budget(wall_time_ms=90_000, max_core_steps=100, max_program_operations=48,
                           max_child_depth=2, max_total_children=2)
    grandchild_version = s.admit(host, CHILD, grandchild_budget, "grandchild.js")
    middle_version = s.admit(host, MIDDLE, middle_budget, "middle.js")
    root_version = s.admit(host, ROOT_OF_THREE, root_budget, "root.js")
    host.bind_child(middle_version, "writer", grandchild_version, grandchild_budget)
    host.bind_child(root_version, "middle", middle_version, middle_budget)
    return host, root_version, root_budget


def _chain(host, root_run_id):
    middle_id = host.run_record(root_run_id).children[0].child_run_id
    grandchild_id = host.run_record(middle_id).children[0].child_run_id
    return middle_id, grandchild_id


def test_a_grandchild_that_pauses_is_answered_and_both_ancestors_complete():
    host, version, run_budget = _three_levels()
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    middle_id, grandchild_id = _chain(host, result.run_id)
    assert host.run_record(middle_id).terminal_result.status == ng.ProgramTerminalStatus.Interrupted

    answered = host.resume(grandchild_id, ANSWER).wait()
    assert answered.status == ng.ProgramTerminalStatus.Completed, s.describe(answered)

    final = _parent_result(host, result.run_id)
    assert final.status == ng.ProgramTerminalStatus.Completed, s.describe(final)
    assert final.output["middle"]["grandchild"]["answer"] == ANSWER
    assert host.run_record(middle_id).terminal_result.status == ng.ProgramTerminalStatus.Completed


def test_cancelling_the_root_while_the_grandchild_waits_ends_everything_cancelled():
    host, version, run_budget = _three_levels()
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    middle_id, grandchild_id = _chain(host, result.run_id)

    root = host.reconnect(result.run_id)
    assert root.cancel() is True
    assert root.wait().status == ng.ProgramTerminalStatus.Cancelled

    for run_id in (result.run_id, middle_id, grandchild_id):
        record = host.run_record(run_id)
        assert record.terminal_result.status == ng.ProgramTerminalStatus.Cancelled, run_id
    assert host.reconnect(grandchild_id).wait().status == ng.ProgramTerminalStatus.Cancelled


def _chain_of(depth):
    """A root that awaits a child that awaits a child, `depth` levels down to a leaf that waits for an answer."""
    host = s.make_host(budget_ceiling=s.ceiling(wall_time_ms=500_000, max_program_operations=512,
                                                max_core_steps=2_000, max_child_depth=depth + 1,
                                                max_total_children=2 * depth + 2))
    leaf_budget = s.budget(wall_time_ms=10_000, max_concurrency=1, max_core_steps=10, max_program_operations=8)
    below = s.admit(host, CHILD, leaf_budget, "level-0.js")
    below_budget = leaf_budget
    for level in range(1, depth + 1):
        source = s.with_main(ROOT_GRAPH, """
            const res = yield ng.await(ng.spawn("next", {}, "spawn:next"), 200000, "await:next");
            return {below: res};
        """)
        budget = s.budget(wall_time_ms=below_budget.wall_time_ms + 10_000, max_concurrency=2,
                          max_core_steps=below_budget.max_core_steps + 10,
                          max_program_operations=below_budget.max_program_operations + 10,
                          max_child_depth=level, max_total_children=level)
        version = s.admit(host, source, budget, f"level-{level}.js")
        host.bind_child(version, "next", below, below_budget)
        below, below_budget = version, budget
    return host, below, below_budget


def test_cancelling_a_paused_root_ends_every_level_of_a_deep_chain_cancelled():
    host, version, run_budget = _chain_of(4)
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, s.describe(result)
    ids = [result.run_id]
    while host.run_record(ids[-1]).children:
        ids.append(host.run_record(ids[-1]).children[0].child_run_id)
    assert len(ids) == 5

    root = host.reconnect(result.run_id)
    assert root.cancel() is True
    assert root.wait().status == ng.ProgramTerminalStatus.Cancelled

    assert [host.run_record(run_id).terminal_result.status for run_id in ids] == [ng.ProgramTerminalStatus.Cancelled] * 5
