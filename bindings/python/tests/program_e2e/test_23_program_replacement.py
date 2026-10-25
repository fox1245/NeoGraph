"""Program replacement, live graph migration, and child synthesis gateway tests."""

from __future__ import annotations

import hashlib
import threading

import pytest

import neograph_engine as ng

from . import support as s


def fixture_identity(name: str) -> str:
    """A sha256 identity that names a test fixture (a host's reviewed template, validator or contract)."""
    return "sha256:" + hashlib.sha256(name.encode()).hexdigest()


TEMPLATE_ID = fixture_identity("test synthesis template")
VALIDATOR_ID = fixture_identity("test semantic validator")
CONTRACT_ID = fixture_identity("test semantic contract")


# --------------------------------------------------------------------------
# Deterministic gate and privileged node helpers
# --------------------------------------------------------------------------

_GATES_LOCK = threading.Lock()
_GATES: dict[str, tuple[threading.Event, threading.Event]] = {}


def make_gate(tag: str) -> tuple[threading.Event, threading.Event]:
    entered = threading.Event()
    release = threading.Event()
    with _GATES_LOCK:
        _GATES[tag] = (entered, release)
    return entered, release


class GateNode(ng.GraphNode):
    """Deterministic barrier node that signals an Event and waits to be released."""

    def __init__(self, name: str, config: dict | None):
        super().__init__()
        self._name = name
        self._config = dict(config or {})

    def get_name(self) -> str:
        return self._name

    def run(self, input):
        s.JOURNAL.add("started", self._name)
        gate_tag = self._config.get("gate")
        if gate_tag:
            with _GATES_LOCK:
                pair = _GATES.get(gate_tag)
            if pair is not None:
                entered, release = pair
                entered.set()
                token = input.ctx.cancel_token
                while not release.wait(timeout=0.01):
                    if token is not None and token.is_cancelled():
                        s.JOURNAL.add("cancelled", self._name)
                        raise RuntimeError(f"{self._name} cancelled at gate")
        hold_ms = self._config.get("hold_ms", 0)
        if hold_ms:
            threading.Event().wait(float(hold_ms) / 1000.0)
        label = self._config.get("label", self._name)
        s.JOURNAL.add("ran", self._name)
        return [
            ng.ChannelWrite("trace", [label]),
            ng.ChannelWrite("counter", (input.state.get("counter") or 0) + 1),
        ]


ng.NodeFactory.register_type(
    "replace.gate", lambda name, config, ctx: GateNode(name, config)
)
ng.NodeFactory.register_type(
    "replace.privileged", lambda name, config, ctx: GateNode(name, config)
)


def build_replacement_registry() -> ng.ProgramRegistrySnapshot:
    builder = ng.ProgramRegistryBuilder()
    for node_type in (*s.NODE_TYPES, "replace.gate"):
        builder.add_registered_node(
            node_type, "1.0.0", s.digest("node:" + node_type)
        )
    builder.add_registered_node(
        "replace.privileged",
        "1.0.0",
        s.digest("node:replace.privileged"),
        required_capabilities=["cap.privileged"],
        declared_effects=["effect.privileged"],
    )
    for name in s.CONDITIONS:
        builder.add_registered_condition(
            name, "1.0.0", s.digest("condition:" + name)
        )
    for reducer in ("append", "overwrite"):
        builder.add_registered_reducer(
            reducer, "1.0.0", s.digest("reducer:" + reducer)
        )
    return builder.build()


def make_replacement_host(
    owner: str = "tenant:replace",
    *,
    max_dynamic_compiles: int = 4,
    max_child_depth: int = 4,
    max_total_children: int = 8,
    scheduler_threads: int = 4,
) -> ng.LocalProgramHost:
    host_ceiling = s.ceiling(
        wall_time_ms=120_000,
        max_concurrency=8,
        max_program_operations=128,
        max_core_steps=2_000,
        max_dynamic_compiles=max_dynamic_compiles,
        max_child_depth=max_child_depth,
        max_total_children=max_total_children,
    )
    return ng.LocalProgramHost(
        build_replacement_registry(),
        owner,
        host_ceiling,
        scheduler_threads=scheduler_threads,
    )


def admit_flexible(
    host: ng.LocalProgramHost,
    text: str,
    max_budget: ng.ProgramRunBudget,
    source_id: str = "program.js",
    *,
    min_child_depth: int = 0,
    min_total_children: int = 0,
) -> tuple[ng.ProgramBundle, ng.ProgramVersion]:
    min_budget = s.budget(
        wall_time_ms=1,
        model_tokens=0,
        monetary_microunits=0,
        max_concurrency=1,
        max_program_operations=1,
        max_core_steps=1,
        max_dynamic_compiles=0,
        max_child_depth=min_child_depth,
        max_total_children=min_total_children,
    )
    bounds = ng.ProgramBudgetBounds(minimum=min_budget, maximum=max_budget)
    bundle = host.compile(s.source(text, source_id), bounds)
    version = host.admit(bundle)
    return bundle, version


@pytest.fixture(autouse=True)
def _clear_journal():
    s.JOURNAL.clear()
    with _GATES_LOCK:
        _GATES.clear()
    yield


# --------------------------------------------------------------------------
# 1. Replace a handed-off run with an admitted second version:
#    accepted, continues on the new version, remaining budget is carried over,
#    and children of the source are kept.
# --------------------------------------------------------------------------


def test_replace_handed_off_run_continues_and_retains_children_and_budget():
    host = make_replacement_host()

    child_entered, child_release = make_gate("child_hold")
    child_js = s.with_main(
        s.define_graph(
            """
            g.node("child_step", {type: "replace.gate", gate: "child_hold", label: "child:v1"});
            g.entry("child_step");
            g.exit("child_step");
            """,
            name="child_core",
        ),
        """
        const out = yield ng.callCore("child_core", input, "child:call");
        return {child_trace: out.trace};
        """,
    )
    child_budget = s.budget(
        wall_time_ms=20_000,
        max_concurrency=2,
        max_program_operations=16,
        max_core_steps=20,
        max_dynamic_compiles=0,
        max_child_depth=0,
        max_total_children=0,
    )
    _, child_v1 = admit_flexible(host, child_js, child_budget, "child-v1.js")

    parent_v1_js = s.with_main(
        s.define_graph(
            """
            g.node("p1", {type: "replace.gate", label: "parent:v1"});
            g.entry("p1");
            g.exit("p1");
            """,
            name="parent_core",
        ),
        """
        const pre = yield ng.callCore("parent_core", input, "parent:pre");
        const spawned = yield ng.spawn("worker", {seed: 7}, "parent:spawn");
        yield ng.checkpoint(
          {stage: "after_spawn", pre_trace: pre.trace, child_id: spawned.child_run_id},
          "parent:checkpoint"
        );
        return {generation: 1, pre_trace: pre.trace};
        """,
    )
    parent_v2_js = s.with_main(
        s.define_graph(
            """
            g.node("p2", {type: "replace.gate", label: "parent:v2"});
            g.entry("p2");
            g.exit("p2");
            """,
            name="parent_core",
        ),
        """
        const post = yield ng.callCore("parent_core", {}, "parent:post");
        const child_out = yield ng.await(
          ng.spawn("worker", {seed: 7}, "parent:rejoin"),
          20000,
          "parent:await"
        );
        return {
          generation: 2,
          handoff_stage: input.handoff.stage,
          pre_trace: input.handoff.pre_trace,
          post_trace: post.trace,
          child_trace: child_out.child_trace,
        };
        """,
    )

    parent_budget = s.budget(
        wall_time_ms=60_000,
        max_concurrency=4,
        max_program_operations=32,
        max_core_steps=50,
        max_dynamic_compiles=1,
        max_child_depth=1,
        max_total_children=2,
    )
    _, parent_v1 = admit_flexible(host, parent_v1_js, parent_budget, "parent-v1.js")
    _, parent_v2 = admit_flexible(host, parent_v2_js, parent_budget, "parent-v2.js")
    host.bind_child(parent_v1, "worker", child_v1, child_budget)
    host.bind_child(parent_v2, "worker", child_v1, child_budget)

    parent_handle = host.start(parent_v1, {}, parent_budget)
    handoff = parent_handle.next_handoff()
    assert child_entered.wait(timeout=5.0)
    exact_handoff = handoff.wait()
    assert exact_handoff.value["stage"] == "after_spawn"
    assert exact_handoff.value["pre_trace"] == ["parent:v1"]

    source_record_before = host.run_record(parent_handle.run_id)
    source_lineage_before = host.run_lineage(parent_handle.logical_run_id)
    assert source_lineage_before.active_generation == 1
    assert len(source_record_before.children) == 1
    original_child_id = source_record_before.children[0].child_run_id

    authoritative_budget = source_lineage_before.remaining_budget
    replacement_input = {
        "previous_run_id": parent_handle.run_id,
        "handoff": exact_handoff.value,
    }
    replaced_handle = host.replace(
        handoff, parent_v2, replacement_input, authoritative_budget
    )
    assert replaced_handle.logical_run_id == parent_handle.logical_run_id
    assert replaced_handle.run_id != parent_handle.run_id
    assert replaced_handle.program_version_id == parent_v2.id

    lineage_after = host.run_lineage(parent_handle.logical_run_id)
    assert lineage_after.active_generation == 2
    gen2 = host.run_generation(parent_handle.logical_run_id, 2)
    assert gen2.replacement_receipt is not None
    assert gen2.replacement_receipt.source_run_id == parent_handle.run_id
    assert gen2.replacement_receipt.target_run_id == replaced_handle.run_id
    assert gen2.replacement_receipt.target_program_version_id == parent_v2.id

    replaced_record = host.run_record(replaced_handle.run_id)
    assert len(replaced_record.children) == 1
    assert replaced_record.children[0].child_run_id == original_child_id
    for field in (
        "model_tokens",
        "monetary_microunits",
        "max_concurrency",
        "max_program_operations",
        "max_core_steps",
        "max_dynamic_compiles",
        "max_child_depth",
        "max_total_children",
    ):
        assert getattr(replaced_record.invocation_budget, field) == getattr(
            authoritative_budget, field
        )
    assert (
        replaced_record.invocation_budget.wall_time_ms
        <= authoritative_budget.wall_time_ms
    )

    child_release.set()
    result = replaced_handle.wait()
    assert result.status == ng.ProgramTerminalStatus.Completed, s.describe(result)
    assert result.program_version_id == parent_v2.id
    assert result.output == {
        "generation": 2,
        "handoff_stage": "after_spawn",
        "pre_trace": ["parent:v1"],
        "post_trace": ["parent:v2"],
        "child_trace": ["child:v1"],
    }


# --------------------------------------------------------------------------
# 2. Refusals with exact codes:
#    P_REPLACEMENT_AUTHORITY, P_REPLACEMENT_BUDGET, P_REPLACEMENT_TARGET,
#    P_REPLACEMENT_INPUT, P_REPLACEMENT_HANDOFF; after each refusal the source
#    run is unchanged (still its first generation).
# --------------------------------------------------------------------------


def test_replace_refusals_preserve_first_generation():
    host = make_replacement_host()
    run_budget = s.budget(
        wall_time_ms=60_000,
        max_concurrency=4,
        max_program_operations=32,
        max_core_steps=50,
    )

    v1_js = s.with_main(
        s.define_graph(
            """
            g.node("a", {type: "replace.gate", label: "v1:a"});
            g.entry("a");
            g.exit("a");
            """,
            name="work",
        ),
        """
        const first = yield ng.callCore("work", input, "v1:call");
        yield ng.checkpoint({trace: first.trace}, "v1:cp");
        return {generation: 1, trace: first.trace};
        """,
    )
    v2_ok_js = s.with_main(
        s.define_graph(
            """
            g.node("a", {type: "replace.gate", label: "v2:a"});
            g.entry("a");
            g.exit("a");
            """,
            name="work",
        ),
        """
        return {generation: 2, trace: input.handoff.trace};
        """,
    )
    v_wider_authority_js = s.with_main(
        s.define_graph(
            """
            g.node("priv", {type: "replace.privileged", label: "priv"});
            g.entry("priv");
            g.exit("priv");
            """,
            name="work",
        ),
        """
        return {generation: 2};
        """,
    )
    v_core_only_js = s.define_graph(
        """
        g.node("a", {type: "replace.gate", label: "core_only"});
        g.entry("a");
        g.exit("a");
        """,
        name="work",
    )

    _, v1 = admit_flexible(host, v1_js, run_budget, "v1.js")
    _, v2_ok = admit_flexible(host, v2_ok_js, run_budget, "v2-ok.js")
    _, v_wider = admit_flexible(host, v_wider_authority_js, run_budget, "v-wider.js")
    _, v_core_only = admit_flexible(host, v_core_only_js, run_budget, "v-core-only.js")

    handle = host.start(v1, {}, run_budget)
    handoff = handle.next_handoff()
    exact = handoff.wait()

    lineage_initial = host.run_lineage(handle.logical_run_id)
    record_initial = host.run_record(handle.run_id)
    assert lineage_initial.active_generation == 1
    valid_budget = lineage_initial.remaining_budget
    valid_input = {
        "previous_run_id": handle.run_id,
        "handoff": exact.value,
    }

    def assert_source_unchanged():
        lineage = host.run_lineage(handle.logical_run_id)
        record = host.run_record(handle.run_id)
        assert lineage.active_generation == 1
        assert lineage.active_run_record_id == lineage_initial.active_run_record_id
        assert record.id == record_initial.id
        assert record.program_version_id == v1.id

    # (a) Wider authority -> P_REPLACEMENT_AUTHORITY
    with pytest.raises(ng.ProgramDiagnosticError) as exc_info:
        host.replace(handoff, v_wider, valid_input, valid_budget)
    assert exc_info.value.code == "P_REPLACEMENT_AUTHORITY"
    assert exc_info.value.diagnostics[0]["code"] == "P_REPLACEMENT_AUTHORITY"
    assert_source_unchanged()

    # (b) Larger budget -> P_REPLACEMENT_BUDGET
    larger_budget = s.budget(
        **{
            **s.budget_dict(valid_budget),
            "max_core_steps": valid_budget.max_core_steps + 10,
        }
    )
    with pytest.raises(ng.ProgramDiagnosticError) as exc_info:
        host.replace(handoff, v2_ok, valid_input, larger_budget)
    assert exc_info.value.code == "P_REPLACEMENT_BUDGET"
    assert exc_info.value.diagnostics[0]["code"] == "P_REPLACEMENT_BUDGET"
    assert_source_unchanged()

    # (c) Wrong target (Core-only bundle without JS control) -> P_REPLACEMENT_TARGET
    with pytest.raises(ng.ProgramDiagnosticError) as exc_info:
        host.replace(handoff, v_core_only, valid_input, valid_budget)
    assert exc_info.value.code == "P_REPLACEMENT_TARGET"
    assert exc_info.value.diagnostics[0]["code"] == "P_REPLACEMENT_TARGET"
    assert_source_unchanged()

    # (d) Mismatched input (a wrong previous_run_id) -> P_REPLACEMENT_INPUT
    with pytest.raises(ng.ProgramDiagnosticError) as exc_info:
        host.replace(
            handoff,
            v2_ok,
            {"previous_run_id": "wrong-run-id", "handoff": exact.value},
            valid_budget,
        )
    assert exc_info.value.code == "P_REPLACEMENT_INPUT"
    assert exc_info.value.diagnostics[0]["code"] == "P_REPLACEMENT_INPUT"
    assert_source_unchanged()

    # (e) Stale handoff (wrong journal head or tampered handoff payload) -> P_REPLACEMENT_HANDOFF
    stale_ref = ng.ExactProgramHandoffReference()
    stale_ref.source_run_id = exact.reference.source_run_id
    stale_ref.source_journal_head = "sha256:" + "0" * 64
    stale_ref.command_coordinate_id = exact.reference.command_coordinate_id
    stale_ref.command_entry_id = exact.reference.command_entry_id
    with pytest.raises(ng.ProgramDiagnosticError) as exc_info:
        host.replace(stale_ref, v2_ok, valid_input, valid_budget)
    assert exc_info.value.code == "P_REPLACEMENT_HANDOFF"
    assert exc_info.value.diagnostics[0]["code"] == "P_REPLACEMENT_HANDOFF"
    assert_source_unchanged()

    # Releasing the held handoff lets the original generation 1 run finish normally.
    handoff.release()
    result = handle.wait()
    assert result.status == ng.ProgramTerminalStatus.Completed, s.describe(result)
    assert result.program_version_id == v1.id
    assert result.output == {"generation": 1, "trace": ["v1:a"]}
    assert host.run_lineage(handle.logical_run_id).active_generation == 1


# --------------------------------------------------------------------------
# 3. migrate_graph: the happy path and one guard refusal.
# --------------------------------------------------------------------------


def test_migrate_graph_happy_path_and_guard_refusal():
    host = make_replacement_host(max_child_depth=0, max_total_children=0)
    entered_step1, release_step1 = make_gate("mig_step1")

    core_js = s.define_graph(
        """
        g.node("step1", {type: "replace.gate", gate: "mig_step1", hold_ms: 200, label: "step1"});
        g.node("step2", {type: "replace.gate", label: "step2"});
        g.entry("step1");
        g.edge("step1", "step2");
        g.exit("step2");
        """,
        name="mig_core",
    )
    incompatible_core_js = s.define_graph(
        """
        g.node("step1", {type: "replace.gate", label: "step1"});
        g.node("step2", {type: "replace.gate", label: "step2"});
        g.node("step3", {type: "replace.gate", label: "step3"});
        g.entry("step1");
        g.edge("step1", "step2");
        g.edge("step2", "step3");
        g.exit("step3");
        """,
        name="mig_core",
    )

    budget_v1 = s.budget(
        wall_time_ms=60_000,
        max_concurrency=2,
        max_program_operations=16,
        max_core_steps=20,
        max_dynamic_compiles=0,
        max_child_depth=0,
        max_total_children=0,
    )
    budget_v2 = s.budget(
        wall_time_ms=60_000,
        max_concurrency=2,
        max_program_operations=24,
        max_core_steps=20,
        max_dynamic_compiles=0,
        max_child_depth=0,
        max_total_children=0,
    )

    bundle_v1, core_v1 = admit_flexible(host, core_js, budget_v1, "core-v1.js")
    bundle_bad, core_incompatible = admit_flexible(
        host, incompatible_core_js, budget_v2, "core-bad.js"
    )

    # A different budget declaration changes the bundle's runtime contract, so the
    # live run migrates to a new generation of its own admitted version (as the C++
    # ActiveRunGraphForkDropsSourceActivationButRetainsLineage test does).
    plan_ok = ng.MigrationPlan.between(core_v1, bundle_v1, core_v1, bundle_v1)
    assert plan_ok.is_compatible()
    plan_bad = ng.MigrationPlan.between(
        core_v1, bundle_v1, core_incompatible, bundle_bad
    )
    assert not plan_bad.is_compatible()

    host.activate(core_v1, 0)
    _, handle = host.start_active({}, budget_v1)
    assert entered_step1.wait(timeout=5.0)

    # Guard refusal: incompatible target topology is refused with P_GRAPH_MIGRATION_INCOMPATIBLE
    # before interrupting the running graph.
    with pytest.raises(ng.ProgramDiagnosticError) as exc_info:
        host.migrate_graph(handle, core_incompatible)
    assert exc_info.value.code == "P_GRAPH_MIGRATION_INCOMPATIBLE"
    assert host.run_lineage(handle.logical_run_id).active_generation == 1

    # Happy path: release step1 so it enters its 200ms post-gate hold while migrate_graph
    # arms the safe-point request and waits for the super-step boundary between step1 and step2.
    release_step1.set()
    migrated_handle = host.migrate_graph(handle, core_v1)

    assert migrated_handle.logical_run_id == handle.logical_run_id
    assert migrated_handle.run_id != handle.run_id
    assert migrated_handle.program_version_id == core_v1.id

    result = migrated_handle.wait()
    assert result.status == ng.ProgramTerminalStatus.Completed, s.describe(result)
    assert result.program_version_id == core_v1.id
    assert s.trace_channel(result) == ["step1", "step2"]

    lineage = host.run_lineage(handle.logical_run_id)
    assert lineage.active_generation == 2
    gen2 = host.run_generation(handle.logical_run_id, 2)
    assert gen2.graph_migration_receipt is not None
    assert gen2.graph_migration_receipt.source_version.id == core_v1.id
    assert gen2.graph_migration_receipt.target_version.id == core_v1.id


# --------------------------------------------------------------------------
# 4. The child synthesis gateway:
#    a proposal compiled, admitted, and published as a child;
#    a proposal that asks for more authority is refused.
# --------------------------------------------------------------------------


def test_child_synthesis_gateway_admits_and_refuses_excess_authority():
    host = make_replacement_host()

    parent_js = s.with_main(
        s.define_graph(
            """
            g.node("p", {type: "replace.gate", label: "parent: synth"});
            g.entry("p");
            g.exit("p");
            """,
            name="parent_core",
        ),
        """
        const pre = yield ng.callCore("parent_core", input, "parent:pre");
        yield ng.checkpoint({ready_for_synthesis: true}, "parent:cp");
        const child_res = yield ng.await(
          ng.spawn("synth_worker", {}, "parent:spawn_synth"),
          20000,
          "parent:await_synth"
        );
        return {parent_trace: pre.trace, synthesized_child: child_res};
        """,
    )
    parent_budget = s.budget(
        wall_time_ms=60_000,
        max_concurrency=4,
        max_program_operations=32,
        max_core_steps=50,
        max_dynamic_compiles=2,
        max_child_depth=1,
        max_total_children=2,
    )
    _, parent_v1 = admit_flexible(host, parent_js, parent_budget, "parent-synth.js")

    parent_handle = host.start(parent_v1, {}, parent_budget)
    handoff = parent_handle.next_handoff()
    exact = handoff.wait()
    assert exact.value == {"ready_for_synthesis": True}

    child_budget = s.budget(
        wall_time_ms=20_000,
        max_concurrency=2,
        max_program_operations=16,
        max_core_steps=20,
        max_dynamic_compiles=0,
        max_child_depth=0,
        max_total_children=0,
    )
    synth_child_js = s.with_main(
        s.define_graph(
            """
            g.node("c1", {type: "replace.gate", label: "synth:c1"});
            g.node("c2", {type: "replace.gate", label: "synth:c2"});
            g.entry("c1");
            g.edge("c1", "c2");
            g.exit("c2");
            """,
            name="synth_core",
        ),
        """
        const out = yield ng.callCore("synth_core", input, "synth:call");
        return {synth_trace: out.trace};
        """,
    )

    parent_lineage = host.run_lineage(parent_handle.logical_run_id)
    assert parent_lineage is not None

    # The host's own semantic validator decides; the binding approves nothing by itself.
    validated = []

    def validate(proposal, bundle, reservation):
        validated.append(proposal.id)
        return ng.ProgramSynthesisSemanticDecision(
            VALIDATOR_ID, CONTRACT_ID, True, {"reviewed": proposal.id}
        )

    # Without the host's semantic validator nothing is synthesized, however attenuated the proposal.
    unvalidated_source = s.source(synth_child_js, "synth-unvalidated.js")
    unvalidated_proposal = ng.ProgramSynthesisProposal(
        host.owner_scope,
        parent_handle.run_id,
        unvalidated_source,
        child_budget,
        lineage_id=parent_lineage.lineage_id,
    )
    unvalidated_grant = ng.ProgramChildSynthesisGrant(
        host.owner_scope,
        parent_handle.run_id,
        parent_v1,
        parent_lineage.id,
        unvalidated_source,
        child_budget,
        TEMPLATE_ID,
        VALIDATOR_ID,
        CONTRACT_ID,
        minimum_execution_guarantee=ng.ExecutionGuarantee.Unmanaged,
    )
    with pytest.raises(ValueError) as unvalidated:
        host.prepare_child_synthesis(
            parent_handle, handoff, unvalidated_proposal, unvalidated_grant, "synth_worker"
        )
    assert "semantic validator" in str(unvalidated.value)
    assert validated == []
    assert host.child_syntheses(parent_handle.run_id) == []

    host.register_synthesis_validator(validate)

    # Proposal 1: requests a capability ("cap.privileged") not in the parent's closure or grant -> refused.
    bad_source = s.source(synth_child_js, "synth-bad.js")
    bad_proposal = ng.ProgramSynthesisProposal(
        host.owner_scope,
        parent_handle.run_id,
        bad_source,
        child_budget,
        lineage_id=parent_lineage.lineage_id,
        requested_capabilities=["cap.privileged"],
    )
    restrictive_grant = ng.ProgramChildSynthesisGrant(
        host.owner_scope,
        parent_handle.run_id,
        parent_v1,
        parent_lineage.id,
        bad_source,
        child_budget,
        TEMPLATE_ID,
        VALIDATOR_ID,
        CONTRACT_ID,
        allowed_capabilities=[],
        allowed_effects=[],
        minimum_execution_guarantee=ng.ExecutionGuarantee.Unmanaged,
    )
    with pytest.raises(ng.ProgramDiagnosticError) as exc_info:
        host.prepare_child_synthesis(
            parent_handle, handoff, bad_proposal, restrictive_grant, "synth_worker"
        )
    assert exc_info.value.code == "P_CHILD_SYNTHESIS_AUTHORITY"
    assert host.child_syntheses(parent_handle.run_id) == []


    # Proposal 2: attenuated authority -> compiled, admitted, linked, and spawned by the parent.
    good_source = s.source(synth_child_js, "synth-good.js")
    good_proposal = ng.ProgramSynthesisProposal(
        host.owner_scope,
        parent_handle.run_id,
        good_source,
        child_budget,
        lineage_id=parent_lineage.lineage_id,
    )
    good_grant = ng.ProgramChildSynthesisGrant(
        host.owner_scope,
        parent_handle.run_id,
        parent_v1,
        parent_lineage.id,
        good_source,
        child_budget,
        TEMPLATE_ID,
        VALIDATOR_ID,
        CONTRACT_ID,
        minimum_execution_guarantee=ng.ExecutionGuarantee.Unmanaged,
    )
    record = host.prepare_child_synthesis(
        parent_handle, handoff, good_proposal, good_grant, "synth_worker"
    )
    assert record.state == ng.ProgramChildSynthesisState.Bound
    assert good_proposal.id in validated
    assert record.binding_name == "synth_worker"
    assert record.bundle is not None
    assert record.version is not None
    assert record.link_receipt is not None
    assert record.link_receipt.child_name == "synth_worker"

    stored_records = host.child_syntheses(parent_handle.run_id)
    assert len(stored_records) == 1
    assert stored_records[0].state == ng.ProgramChildSynthesisState.Bound

    handoff.release()
    result = parent_handle.wait()
    assert result.status == ng.ProgramTerminalStatus.Completed, s.describe(result)
    assert result.output == {
        "parent_trace": ["parent: synth"],
        "synthesized_child": {"synth_trace": ["synth:c1", "synth:c2"]},
    }
    final_records = host.child_syntheses(parent_handle.run_id)
    assert len(final_records) == 1
    assert final_records[0].state == ng.ProgramChildSynthesisState.Spawned


def test_child_synthesis_refuses_a_decision_of_another_validator():
    host = make_replacement_host()

    parent_js = s.with_main(
        s.define_graph(
            """
            g.node("p", {type: "replace.gate", label: "parent: synth"});
            g.entry("p");
            g.exit("p");
            """,
            name="parent_core",
        ),
        """
        const pre = yield ng.callCore("parent_core", input, "parent:pre");
        yield ng.checkpoint({ready_for_synthesis: true}, "parent:cp");
        const child_res = yield ng.await(
          ng.spawn("synth_worker", {}, "parent:spawn_synth"),
          20000,
          "parent:await_synth"
        );
        return {parent_trace: pre.trace, synthesized_child: child_res};
        """,
    )
    parent_budget = s.budget(
        wall_time_ms=60_000,
        max_concurrency=4,
        max_program_operations=32,
        max_core_steps=50,
        max_dynamic_compiles=2,
        max_child_depth=1,
        max_total_children=2,
    )
    _, parent_v1 = admit_flexible(host, parent_js, parent_budget, "parent-synth.js")

    parent_handle = host.start(parent_v1, {}, parent_budget)
    handoff = parent_handle.next_handoff()
    exact = handoff.wait()
    assert exact.value == {"ready_for_synthesis": True}

    child_budget = s.budget(
        wall_time_ms=20_000,
        max_concurrency=2,
        max_program_operations=16,
        max_core_steps=20,
        max_dynamic_compiles=0,
        max_child_depth=0,
        max_total_children=0,
    )
    synth_child_js = s.with_main(
        s.define_graph(
            """
            g.node("c1", {type: "replace.gate", label: "synth:c1"});
            g.node("c2", {type: "replace.gate", label: "synth:c2"});
            g.entry("c1");
            g.edge("c1", "c2");
            g.exit("c2");
            """,
            name="synth_core",
        ),
        """
        const out = yield ng.callCore("synth_core", input, "synth:call");
        return {synth_trace: out.trace};
        """,
    )

    parent_lineage = host.run_lineage(parent_handle.logical_run_id)
    assert parent_lineage is not None

    good_calls = []

    def validate(proposal, bundle, reservation):
        good_calls.append(proposal.id)
        return ng.ProgramSynthesisSemanticDecision(VALIDATOR_ID, CONTRACT_ID, True, {})

    host.register_synthesis_validator(validate)

    # A grant that names another validator than the one the host runs is refused.
    other_source = s.source(synth_child_js, "synth-other.js")
    other_validator_grant = ng.ProgramChildSynthesisGrant(
        host.owner_scope,
        parent_handle.run_id,
        parent_v1,
        parent_lineage.id,
        other_source,
        child_budget,
        TEMPLATE_ID,
        fixture_identity("another semantic validator"),
        CONTRACT_ID,
        minimum_execution_guarantee=ng.ExecutionGuarantee.Unmanaged,
    )
    other_proposal = ng.ProgramSynthesisProposal(
        host.owner_scope,
        parent_handle.run_id,
        other_source,
        child_budget,
        lineage_id=parent_lineage.lineage_id,
    )
    refused = host.prepare_child_synthesis(
        parent_handle, handoff, other_proposal, other_validator_grant, "synth_worker"
    )
    assert refused.state == ng.ProgramChildSynthesisState.Failed
    assert refused.error_code == "P_CHILD_SYNTHESIS_SEMANTICS"
    assert refused.version is None

    assert good_calls == [other_proposal.id]
    assert host.child_syntheses(parent_handle.run_id)[0].state == ng.ProgramChildSynthesisState.Failed
    handoff.release()
    parent_handle.cancel()
    parent_handle.wait()


# --------------------------------------------------------------------------
# 5. A three-level hierarchy (root -> child -> grandchild) whose child is
#    replaced while it runs, ending with the oracle result.
# --------------------------------------------------------------------------


def test_three_level_hierarchy_replaces_running_child_and_returns_oracle_result():
    host = make_replacement_host(
        max_dynamic_compiles=4,
        max_child_depth=3,
        max_total_children=6,
        scheduler_threads=4,
    )
    grandchild_entered, grandchild_release = make_gate("gc_hold")
    child_entered, child_release = make_gate("child_hold")

    # Grandchild: 4 nodes
    grandchild_js = s.with_main(
        s.define_graph(
            """
            g.node("g1", {type: "replace.gate", gate: "gc_hold", label: "gc:1"});
            g.node("g2", {type: "replace.gate", label: "gc:2"});
            g.node("g3", {type: "replace.gate", label: "gc:3"});
            g.node("g4", {type: "replace.gate", label: "gc:4"});
            g.entry("g1");
            g.edge("g1", "g2");
            g.edge("g2", "g3");
            g.edge("g3", "g4");
            g.exit("g4");
            """,
            name="grandchild_core",
        ),
        """
        const out = yield ng.callCore("grandchild_core", input, "gc:call");
        return {grandchild_trace: out.trace, count: out.counter};
        """,
    )
    grandchild_budget = s.budget(
        wall_time_ms=20_000,
        max_concurrency=1,
        max_program_operations=8,
        max_core_steps=16,
        max_dynamic_compiles=0,
        max_child_depth=0,
        max_total_children=0,
    )
    _, grandchild_v1 = admit_flexible(
        host, grandchild_js, grandchild_budget, "grandchild.js"
    )

    # Child v1: spawns grandchild, runs 2 nodes, then checkpoints for replacement while grandchild is alive.
    child_v1_js = s.with_main(
        s.define_graph(
            """
            g.node("c1_1", {type: "replace.gate", gate: "child_hold", label: "child_v1:1"});
            g.node("c1_2", {type: "replace.gate", label: "child_v1:2"});
            g.entry("c1_1");
            g.edge("c1_1", "c1_2");
            g.exit("c1_2");
            """,
            name="child_core_v1",
        ),
        """
        const gc = yield ng.spawn("grandchild", {}, "child_v1:spawn_gc");
        const first = yield ng.callCore("child_core_v1", input, "child_v1:core");
        yield ng.checkpoint(
          {v1_trace: first.trace, grandchild_run_id: gc.child_run_id},
          "child_v1:swap"
        );
        return {child_generation: 1, v1_trace: first.trace};
        """,
    )

    # Child v2 (replacement): 3 nodes, rejoins the inherited grandchild, returns combined result to root.
    child_v2_js = s.with_main(
        s.define_graph(
            """
            g.node("c2_1", {type: "replace.gate", label: "child_v2:1"});
            g.node("c2_2", {type: "replace.gate", label: "child_v2:2"});
            g.node("c2_3", {type: "replace.gate", label: "child_v2:3"});
            g.entry("c2_1");
            g.edge("c2_1", "c2_2");
            g.edge("c2_2", "c2_3");
            g.exit("c2_3");
            """,
            name="child_core_v2",
        ),
        """
        const second = yield ng.callCore("child_core_v2", {}, "child_v2:core");
        const gc_result = yield ng.await(
          ng.spawn("grandchild", {}, "child_v2:rejoin_gc"),
          20000,
          "child_v2:await_gc"
        );
        return {
          child_generation: 2,
          v1_trace: input.handoff.v1_trace,
          v2_trace: second.trace,
          grandchild: gc_result,
        };
        """,
    )
    child_budget = s.budget(
        wall_time_ms=40_000,
        max_concurrency=2,
        max_program_operations=24,
        max_core_steps=32,
        max_dynamic_compiles=0,
        max_child_depth=1,
        max_total_children=2,
    )
    _, child_v1 = admit_flexible(host, child_v1_js, child_budget, "child-v1.js")
    _, child_v2 = admit_flexible(host, child_v2_js, child_budget, "child-v2.js")
    host.bind_child(child_v1, "grandchild", grandchild_v1, grandchild_budget)
    host.bind_child(child_v2, "grandchild", grandchild_v1, grandchild_budget)

    # Root: 1 node, spawns and awaits child.
    root_js = s.with_main(
        s.define_graph(
            """
            g.node("r1", {type: "replace.gate", label: "root:1"});
            g.entry("r1");
            g.exit("r1");
            """,
            name="root_core",
        ),
        """
        const root_out = yield ng.callCore("root_core", input, "root:core");
        const child_out = yield ng.await(
          ng.spawn("child", {}, "root:spawn_child"),
          40000,
          "root:await_child"
        );
        return {
          root_trace: root_out.trace,
          child: child_out,
        };
        """,
    )
    root_budget = s.budget(
        wall_time_ms=80_000,
        max_concurrency=4,
        max_program_operations=48,
        max_core_steps=64,
        max_dynamic_compiles=0,
        max_child_depth=2,
        max_total_children=4,
    )
    _, root_v1 = admit_flexible(host, root_js, root_budget, "root.js")
    host.bind_child(root_v1, "child", child_v1, child_budget)

    root_handle = host.start(root_v1, {}, root_budget)
    assert grandchild_entered.wait(timeout=5.0)
    assert child_entered.wait(timeout=5.0)

    root_record = host.run_record(root_handle.run_id)
    assert len(root_record.children) == 1
    child_handle = host.reconnect(root_record.children[0].child_run_id)
    child_handoff = child_handle.next_handoff()
    child_release.set()
    exact_child_handoff = child_handoff.wait()
    assert exact_child_handoff.value["v1_trace"] == ["child_v1:1", "child_v1:2"]

    child_lineage_before = host.run_lineage(child_handle.logical_run_id)
    assert child_lineage_before.active_generation == 1

    replaced_child_handle = host.replace(
        child_handoff,
        child_v2,
        {
            "previous_run_id": child_handle.run_id,
            "handoff": exact_child_handoff.value,
        },
        child_lineage_before.remaining_budget,
    )
    assert replaced_child_handle.logical_run_id == child_handle.logical_run_id
    assert replaced_child_handle.program_version_id == child_v2.id
    assert host.run_lineage(child_handle.logical_run_id).active_generation == 2

    # Release the grandchild so it completes its 4 nodes, replacement child completes its 3 nodes,
    # and root receives the oracle result.
    grandchild_release.set()

    root_result = root_handle.wait()
    assert root_result.status == ng.ProgramTerminalStatus.Completed, s.describe(
        root_result
    )
    assert root_result.output == {
        "root_trace": ["root:1"],
        "child": {
            "child_generation": 2,
            "v1_trace": ["child_v1:1", "child_v1:2"],
            "v2_trace": ["child_v2:1", "child_v2:2", "child_v2:3"],
            "grandchild": {
                "grandchild_trace": ["gc:1", "gc:2", "gc:3", "gc:4"],
                "count": 4,
            },
        },
    }
