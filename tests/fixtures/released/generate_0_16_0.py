#!/usr/bin/env python3
"""Writes the stores in tests/fixtures/released/0.16.0.

These files are written by the released neograph-engine 0.16.0 wheel, through
its public Python API only, and are committed unchanged: the current code must
keep reading and resuming them (tests/test_released_data_compat.cpp). The
generator itself is kept so that the next release can add its own directory
the same way.

Run it in a fresh virtual environment that holds exactly that release:

    python3 -m venv venv
    venv/bin/pip install neograph-engine==0.16.0
    venv/bin/python tests/fixtures/released/generate_0_16_0.py tests/fixtures/released/0.16.0

Nothing here calls a model. The release cannot write every store from Python:
the hook outbox (SQLiteHookJournal) is not exposed by the wheel, so the
runtime-store file carries that table empty.
"""

import gc
import hashlib
import json
import os
import shutil
import sqlite3
import sys

import neograph_engine as ng

RELEASE = "0.16.0"
THREAD_ID = "released-0-16-0"
OWNER = "released-owner"
FEED = "released-feed"
PROGRAM_OWNER = "tenant:released"
BUILD_ID = "released-fixture/v1"


def sha(label):
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def seal(path):
    """Folds the write-ahead log into the file and compacts it."""
    gc.collect()
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    connection.execute("VACUUM")
    connection.close()
    for suffix in ("-wal", "-shm"):
        if os.path.exists(path + suffix):
            os.remove(path + suffix)


# --------------------------------------------------------------------------
# Graph checkpoints: a thread with an earlier checkpoint and a paused run
# --------------------------------------------------------------------------

GRAPH = {
    "name": "released_fixture",
    "channels": {
        "seed": {"reducer": "overwrite"},
        "note": {"reducer": "overwrite"},
        "answer": {"reducer": "overwrite"},
        "result": {"reducer": "overwrite"},
        "trace": {"reducer": "append"},
    },
    "nodes": {
        "prep": {"type": "released_prep"},
        "approve": {"type": "released_approve"},
        "finish": {"type": "released_finish"},
    },
    "edges": [
        {"from": ng.START_NODE, "to": "prep"},
        {"from": "prep", "to": "approve"},
        {"from": "approve", "to": "finish"},
        {"from": "finish", "to": ng.END_NODE},
    ],
}


class Prep(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("note", "prepared:" + str(input.state.get("seed"))),
                ng.ChannelWrite("trace", ["prep"])]


class Approve(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value
        if answer is None:
            raise ng.NodeInterrupt({"question": "ship it?"}, reason="needs approval")
        return [ng.ChannelWrite("answer", answer), ng.ChannelWrite("trace", ["approve"])]


class Finish(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite("result", "done"), ng.ChannelWrite("trace", ["finish"])]


def write_checkpoints(path):
    ng.NodeFactory.register_type("released_prep", lambda name, config, ctx: Prep(name))
    ng.NodeFactory.register_type("released_approve", lambda name, config, ctx: Approve(name))
    ng.NodeFactory.register_type("released_finish", lambda name, config, ctx: Finish(name))
    store = ng.SqliteCheckpointStore(path)
    engine = ng.GraphEngine.compile(GRAPH, ng.NodeContext(), store)
    config = ng.RunConfig()
    config.thread_id = THREAD_ID
    config.input = {"seed": "fixture"}
    paused = engine.run(config)
    assert paused.interrupted and paused.interrupt_node == "approve", paused
    checkpoints = store.list(THREAD_ID, 100)
    latest = store.load_latest(THREAD_ID)
    details = [
        {
            "id": c.id,
            "parent_id": c.parent_id,
            "interrupt_phase": c.interrupt_phase.name,
            "step": c.step,
            "current_node": c.current_node,
            "next_nodes": list(c.next_nodes),
            "schema_version": c.schema_version,
            "channel_values": c.channel_values,
        }
        for c in checkpoints
    ]
    manifest = {
        "thread_id": THREAD_ID,
        "checkpoints_newest_first": details,
        "graph": GRAPH,
        "interrupt_node": paused.interrupt_node,
        "interrupt_value": paused.interrupt_value,
        "latest_checkpoint_id": latest.id,
        "latest_schema_version": latest.schema_version,
        "checkpoint_ids": [c.id for c in checkpoints],
        "checkpoint_count": len(checkpoints),
    }
    del engine, store
    seal(path)
    return manifest


# --------------------------------------------------------------------------
# Runtime stores: history, artifacts, provider receipts and outcomes
# --------------------------------------------------------------------------


def history_record(sequence, predecessor_id):
    data = ng.RuntimeHistoryRecordData()
    data.feed_id = FEED
    data.sequence = sequence
    data.message_id = f"released-message-{sequence}"
    data.trust = ng.RuntimeTrustClass.UntrustedInput
    data.message = ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(f"hello-{sequence}")])
    data.predecessor_id = predecessor_id
    return ng.RuntimeHistoryRecord.create(data)


def write_runtime_stores(path):
    contexts = ng.SQLiteContextStore(path)
    feed = ng.ContextStoreFeed(OWNER, FEED)
    first = history_record(1, None)
    second = history_record(2, first.id)
    assert contexts.append_history(feed, first, None) == ng.ContextStoreAppendResult.Appended
    assert contexts.append_history(feed, second, first.id) == ng.ContextStoreAppendResult.Appended

    artifact_data = ng.ContextArtifactData()
    artifact_data.kind = ng.ContextArtifactKind.RequiredSkill
    artifact_data.producer_id = "released-skill-loader"
    artifact_data.source_digest = sha("artifact-source")
    artifact_data.media_type = "text/markdown"
    artifact_data.required = True
    artifact_data.content = {"text": "Always run the verifier."}
    artifact = ng.ContextArtifact.create(artifact_data)
    assert contexts.put_artifact(OWNER, artifact) == ng.ContextArtifactPutResult.Stored

    receipts = ng.SQLiteProviderDispatchReceiptStore(path)

    def receipt(dispatch_id):
        data = ng.ProviderDispatchReceiptData()
        data.dispatch_id = dispatch_id
        data.provider_binding_identity = sha("binding")
        data.assembly_receipt_id = sha("assembly-" + dispatch_id)
        data.normalized_request_digest = sha("request-" + dispatch_id)
        data.model = "released-model"
        return ng.ProviderDispatchReceipt.create(data)

    settled = receipt("released-dispatch-settled")
    pending = receipt("released-dispatch-pending")
    assert receipts.persist(settled) == ng.ProviderDispatchReceiptPutResult.Stored
    assert receipts.persist(pending) == ng.ProviderDispatchReceiptPutResult.Stored
    outcome_data = ng.ProviderDispatchOutcomeReceiptData()
    outcome_data.dispatch_id = "released-dispatch-settled"
    outcome_data.dispatch_receipt_id = settled.id
    outcome_data.state = ng.ProviderDispatchState.Succeeded
    outcome_data.response_digest = sha("response")
    outcome = ng.ProviderDispatchOutcomeReceipt.create(outcome_data)
    assert receipts.settle("", outcome) == ng.ProviderDispatchOutcomePutResult.Stored

    manifest = {
        "owner_id": OWNER,
        "feed_id": FEED,
        "history_record_ids": [first.id, second.id],
        "history_records_canonical": [first.serialize_canonical(), second.serialize_canonical()],
        "history_texts": ["hello-1", "hello-2"],
        "artifact_canonical": artifact.serialize_canonical(),
        "artifact_content": artifact_data.content,
        "settled_receipt_canonical": settled.serialize_canonical(),
        "pending_receipt_canonical": pending.serialize_canonical(),
        "outcome_canonical": outcome.serialize_canonical(),
        "history_message_ids": ["released-message-1", "released-message-2"],
        "artifact_id": artifact.id,
        "settled_dispatch_id": "released-dispatch-settled",
        "settled_receipt_id": settled.id,
        "outcome_id": outcome.id,
        "pending_dispatch_id": "released-dispatch-pending",
        "hook_outbox_rows": 0,
    }
    del contexts, receipts
    seal(path)
    return manifest


# --------------------------------------------------------------------------
# Program stores: a Program run paused on a node that asks
# --------------------------------------------------------------------------


class ProgramStep(ng.GraphNode):
    def __init__(self, name, config):
        super().__init__()
        self._name = name
        self._config = dict(config or {})

    def get_name(self):
        return self._name

    def run(self, input):
        writes = [ng.ChannelWrite("trace", [self._name])]
        if self._config.get("ask"):
            answer = input.ctx.resume_value
            if answer is None:
                raise ng.NodeInterrupt({"question": self._config["ask"]}, reason="needs approval")
            writes.append(ng.ChannelWrite("answer", answer))
        return writes


PROGRAM_TYPE = "released.step"
PROGRAM_SOURCE = """\
export function define() {
  const g = ng.graph("work");
  g.channel("trace", {reducer: "append", initial: []});
  g.channel("answer", {reducer: "overwrite", initial: null});
  g.node("draft", {type: "released.step"});
  g.node("confirm", {type: "released.step", ask: "deploy?"});
  g.entry("draft");
  g.edge("draft", "confirm");
  g.exit("confirm");
  return g;
}
"""


def run_budget(**values):
    budget = ng.ProgramRunBudget()
    for key, value in values.items():
        setattr(budget, key, value)
    return budget


def write_program(directory):
    ng.NodeFactory.register_type(PROGRAM_TYPE, lambda name, config, ctx: ProgramStep(name, config))
    builder = ng.ProgramRegistryBuilder()
    node = {"name": PROGRAM_TYPE, "version": "1.0.0", "digest": sha("node:" + PROGRAM_TYPE)}
    builder.add_registered_node(node["name"], node["version"], node["digest"])
    reducers = []
    for reducer in ("append", "overwrite"):
        reducers.append({"name": reducer, "version": "1.0.0", "digest": sha("reducer:" + reducer)})
        builder.add_registered_reducer(reducer, "1.0.0", reducers[-1]["digest"])
    registry = builder.build()
    ceiling = run_budget(wall_time_ms=120_000, model_tokens=1, monetary_microunits=1,
                         max_concurrency=8, max_program_operations=64, max_core_steps=2_000,
                         max_dynamic_compiles=1, max_child_depth=0, max_total_children=0)
    budget = run_budget(wall_time_ms=10_000, model_tokens=0, monetary_microunits=0,
                        max_concurrency=4, max_program_operations=16, max_core_steps=50,
                        max_dynamic_compiles=0, max_child_depth=0, max_total_children=0)
    host = ng.LocalProgramHost(registry, PROGRAM_OWNER, ceiling, compiler_build_id=BUILD_ID,
                               sqlite_directory=directory)
    version = host.compile_admit(ng.ProgramSource.from_javascript("program.js", PROGRAM_SOURCE), budget)
    paused = host.run(version, {}, budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, paused.status
    pending = paused.interrupt.pending_input
    manifest = {
        "owner_scope": PROGRAM_OWNER,
        "compiler_build_id": BUILD_ID,
        "registry": {"node": node, "reducers": reducers},
        "run_id": paused.run_id,
        "core_node": paused.interrupt.core_node,
        "pending_call_id": pending.call_id,
        "checkpoint_id": paused.checkpoint.checkpoint_id,
        "execution_trace": list(paused.execution_trace),
    }
    del host, version, paused, registry
    gc.collect()
    for name in ("program.db", "checkpoints.db"):
        seal(os.path.join(directory, name))
    # The provider-call journal of a host that binds no provider holds nothing.
    os.remove(os.path.join(directory, "provider_calls.db"))
    for suffix in ("-wal", "-shm"):
        leftover = os.path.join(directory, "provider_calls.db" + suffix)
        if os.path.exists(leftover):
            os.remove(leftover)
    return manifest


def main():
    if ng.__version__ != RELEASE:
        raise SystemExit("this generator belongs to neograph-engine " + RELEASE)
    out = os.path.abspath(sys.argv[1])
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    manifest = {
        "release": RELEASE,
        "checkpoints": write_checkpoints(os.path.join(out, "checkpoints.sqlite")),
        "runtime_stores": write_runtime_stores(os.path.join(out, "runtime_stores.sqlite")),
        "program": write_program(os.path.join(out, "program")),
    }
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print("wrote", out)


if __name__ == "__main__":
    main()
