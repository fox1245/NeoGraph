// The Program replacement, checkpoint handoff, root Core graph migration, and child synthesis APIs of the JavaScript
// module against the local OpenAI-compatible fixture server.
//
//   node wasm/test/program_replacement_test.mjs <directory with neograph.mjs, stores.mjs, neograph_module.mjs and neograph_module.wasm>
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import { startFakeOpenAI, replyTo } from './fake_openai_server.mjs';

const directory = path.resolve(process.argv[2] ?? '.');
const { load, NeoGraphError } = await import(pathToFileURL(path.join(directory, 'neograph.mjs')).href);

const KEY = 'test-key-0123456789';

// The host's own authority for a synthesized child: its grant and its semantic decision. Fixture identities name the
// host's reviewed template, validator and contract.
const fixtureIdentity = (name) => 'sha256:' + createHash('sha256').update(name).digest('hex');
const TEMPLATE_ID = fixtureIdentity('test synthesis template');
const VALIDATOR_ID = fixtureIdentity('test semantic validator');
const CONTRACT_ID = fixtureIdentity('test semantic contract');
function hostAuthority(childBudget, allowedCapabilities = []) {
  return {
    grant: {
      templateIdentity: TEMPLATE_ID, validatorIdentity: VALIDATOR_ID, contractIdentity: CONTRACT_ID,
      allowedCapabilities, allowedEffects: [], childBudget,
    },
    semanticDecision: {
      validatorIdentity: VALIDATOR_ID, contractIdentity: CONTRACT_ID, accepted: true, evidence: { reviewedBy: 'test host' },
    },
  };
}
// The host's ceiling for these runs: more dynamic compiles and children than the module's default ceiling allows.
const LIMITS = {
  wallTimeMs: 3600000, modelTokens: 100000000, monetaryMicrounits: 1000000000,
  maxConcurrency: 16, maxProgramOperations: 100000, maxCoreSteps: 100000,
  maxDynamicCompiles: 16, maxChildDepth: 4, maxTotalChildren: 16,
};
// Admission bounds up to `maximum`: the structural floor of a Program (one concurrent attempt, operation and Core step,
// one level and one child where the Program dispatches children), never above the maximum.
const boundsUpTo = (maximum) => ({
  minimum: {
    maxConcurrency: Math.min(1, maximum.maxConcurrency), maxProgramOperations: Math.min(1, maximum.maxProgramOperations),
    maxCoreSteps: Math.min(1, maximum.maxCoreSteps), maxChildDepth: Math.min(1, maximum.maxChildDepth),
    maxTotalChildren: Math.min(1, maximum.maxTotalChildren),
  },
  maximum,
});
const HELPER_BUDGET = {
  wallTimeMs: 20000, modelTokens: 5000, monetaryMicrounits: 1000,
  maxConcurrency: 1, maxProgramOperations: 8, maxCoreSteps: 20,
  maxDynamicCompiles: 0, maxChildDepth: 0, maxTotalChildren: 0,
};

const CHECKPOINT_SOURCE = `
export function define() {
  return ng.graph("step")
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "llm_call"})
    .entry("first").exit("first");
}
export function* main(input) {
  const first = yield ng.callCore("step", input, "first-call");
  yield ng.checkpoint({messages: first.messages, note: "ready-for-handoff"}, "handoff-site");
  const second = yield ng.callCore("step", {messages: first.messages}, "second-call");
  return {messages: second.messages, finishedBy: "original"};
}
`;

const REPLACEMENT_SOURCE = `
export function define() {
  return ng.graph("replacement-step")
    .channel("messages", {reducer: "append", initial: []})
    .node("next", {type: "llm_call"})
    .entry("next").exit("next");
}
export function* main(input) {
  const inherited = input.handoff?.messages ?? [];
  const reply = yield ng.callCore("replacement-step", {messages: inherited}, "replacement-call");
  return {
    messages: reply.messages,
    note: input.handoff?.note ?? null,
    previousRunId: input.previous_run_id ?? null,
    finishedBy: "replacement"
  };
}
`;

const REPLACEMENT_WIDER_AUTHORITY = `
export function define() {
  return ng.graph("wider-step")
    .channel("messages", {reducer: "append", initial: []})
    .channel("extra", {reducer: "overwrite", initial: ""})
    .node("next", {type: "llm_call"})
    .entry("next").exit("next");
}
export function* main(input) {
  const reply = yield ng.callCore("wider-step", {messages: input.handoff?.messages ?? []}, "wider-call");
  return {messages: reply.messages};
}
`;

const DOCUMENT_ONLY_PROGRAM = {
  program_schema_version: 4,
  input_contract: { schema_version: 1, schema: {} },
  output_contract: { schema_version: 1, schema: {} },
  root: {
    op: 'call_core', name: 'doc-only',
    definition: {
      schema_version: 1, name: 'doc-only', channels: { messages: { reducer: 'append' } }, nodes: { llm: { type: 'llm_call' } },
      edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
    },
  },
  declared_budget_requirements: [
    { resource: 'wall_time_ms', minimum: 1, maximum: 60000 }, { resource: 'model_tokens', minimum: 0, maximum: 10000 },
    { resource: 'monetary_microunits', minimum: 0, maximum: 1000000 }, { resource: 'max_concurrency', minimum: 1, maximum: 4 },
    { resource: 'max_program_operations', minimum: 1, maximum: 32 }, { resource: 'max_core_steps', minimum: 1, maximum: 100 },
    { resource: 'max_dynamic_compiles', minimum: 0, maximum: 0 }, { resource: 'max_child_depth', minimum: 0, maximum: 4 },
    { resource: 'max_total_children', minimum: 0, maximum: 4 },
  ],
};

const PARENT_WITH_RETENTION_V1 = `
export function define() {
  return ng.graph("retain-v1")
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "llm_call"})
    .entry("first").exit("first");
}
export function* main(input) {
  const first = yield ng.callCore("retain-v1", input, "retain-v1:first");
  yield ng.checkpoint({stage: "propose-child"}, "retain-v1:propose");
  const child = yield ng.spawn("retained", {messages: [{role: "user", content: "Retained child"}]}, "retain-v1:spawn");
  yield ng.checkpoint({
    stage: "swap-parent",
    messages: first.messages,
    retainedChildId: child.child_run_id
  }, "retain-v1:swap");
  throw new Error("Retired parent v1 must not continue");
}
`;

const PARENT_WITH_RETENTION_V2 = `
export function define() {
  return ng.graph("retain-v2")
    .channel("messages", {reducer: "append", initial: []})
    .node("second", {type: "llm_call"})
    .entry("second").exit("second");
}
export function* main(input) {
  const childOut = yield ng.await(
    ng.spawn("retained", {messages: [{role: "user", content: "Retained child"}]}, "retain-v2:join"),
    30000,
    "retain-v2:await"
  );
  const second = yield ng.callCore("retain-v2", {messages: input.handoff.messages}, "retain-v2:second");
  return {
    finishedBy: "retain-v2",
    previousRunId: input.previous_run_id,
    retainedChildId: input.handoff.retainedChildId,
    childOut,
    messages: second.messages
  };
}
`;

const RETAINED_CHILD_SOURCE = `
export function define() {
  return ng.graph("retained-child-graph")
    .channel("messages", {reducer: "append", initial: []})
    .node("worker", {type: "llm_call"})
    .entry("worker").exit("worker");
}
export function* main(input) {
  yield ng.checkpoint({stage: "retained-child-ready"}, "retained-child:ready");
  const reply = yield ng.callCore("retained-child-graph", input, "retained-child:work");
  return {messages: reply.messages, childKept: true};
}
`;

const MIGRATE_V1 = `
export function define() {
  return ng.graph("migratable")
    .metadata({release: "v1"})
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "tool_dispatch"})
    .node("second", {type: "llm_call"})
    .entry("first").edge("first", "second").exit("second");
}
`;

const MIGRATE_V2 = `
export function define() {
  return ng.graph("migratable")
    .metadata({release: "v2"})
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "tool_dispatch"})
    .node("second", {type: "llm_call"})
    .entry("first").edge("first", "second").exit("second");
}
`;

const PARENT_SYNTHESIS_SOURCE = `
export function define() {
  return ng.graph("parent-graph")
    .channel("messages", {reducer: "append", initial: []})
    .node("ask", {type: "llm_call"})
    .entry("ask").exit("ask");
}
export function* main(input) {
  yield ng.checkpoint({wantChild: "helper"}, "before-child");
  const childResult = yield ng.await(ng.spawn("helper", input, "spawn-helper"), undefined, "await-helper");
  return {fromChild: childResult};
}
`;

const CHILD_SOURCE = `
export function define() {
  return ng.graph("child-graph")
    .channel("messages", {reducer: "append", initial: []})
    .node("worker", {type: "llm_call"})
    .entry("worker").exit("worker");
}
export function* main(input) {
  const reply = yield ng.callCore("child-graph", input, "child-call");
  return {messages: reply.messages, childRan: true};
}
`;

const TREE_ROOT_SOURCE = `
export function define() {
  return ng.graph("tree-root-graph")
    .channel("messages", {reducer: "append", initial: []})
    .node("rootNode", {type: "llm_call"})
    .entry("rootNode").exit("rootNode");
}
export function* main(input) {
  const first = yield ng.callCore("tree-root-graph", input, "root:work");
  yield ng.checkpoint({stage: "root-propose"}, "root:propose");
  const childResult = yield ng.await(
    ng.spawn("child", {messages: [{role: "user", content: "Tree child"}]}, "root:spawn"),
    55000,
    "root:await"
  );
  return {rootMessages: first.messages, childResult};
}
`;

const TREE_CHILD_V1_SOURCE = `
export function define() {
  return ng.graph("tree-child-v1-graph")
    .channel("messages", {reducer: "append", initial: []})
    .node("childNode", {type: "llm_call"})
    .entry("childNode").exit("childNode");
}
export function* main(input) {
  const first = yield ng.callCore("tree-child-v1-graph", input, "child:work");
  yield ng.checkpoint({stage: "child-propose"}, "child:propose");
  const desc = yield ng.spawn("grandchild", {messages: [{role: "user", content: "Tree grandchild"}]}, "child:spawn");
  yield ng.checkpoint({
    stage: "child-swap",
    grandchildRunId: desc.child_run_id,
    messages: first.messages
  }, "child:swap");
  throw new Error("Retired child v1 must not continue");
}
`;

const TREE_CHILD_V2_SOURCE = `
export function define() {
  return ng.graph("tree-child-v2-graph")
    .channel("messages", {reducer: "append", initial: []})
    .node("childV2Node", {type: "llm_call"})
    .entry("childV2Node").exit("childV2Node");
}
export function* main(input) {
  const grandResult = yield ng.await(
    ng.spawn("grandchild", {messages: [{role: "user", content: "Tree grandchild"}]}, "replacement:join"),
    30000,
    "replacement:await"
  );
  const second = yield ng.callCore("tree-child-v2-graph", {messages: input.handoff.messages}, "replacement:work");
  return {
    generation: 2,
    previousChildRunId: input.previous_run_id,
    grandchildRunId: input.handoff.grandchildRunId,
    grandResult,
    messages: second.messages
  };
}
`;

const TREE_GRANDCHILD_SOURCE = `
export function define() {
  return ng.graph("tree-grandchild-graph")
    .channel("messages", {reducer: "append", initial: []})
    .node("grandNode", {type: "llm_call"})
    .entry("grandNode").exit("grandNode");
}
export function* main(input) {
  yield ng.checkpoint({stage: "grandchild-ready"}, "grandchild:ready");
  const reply = yield ng.callCore("tree-grandchild-graph", input, "grandchild:work");
  return {messages: reply.messages, grandchildRan: true};
}
`;

function laterStores(stores) {
  const wrapped = {};
  for (const [member, target] of Object.entries(stores)) {
    if (typeof target !== 'object' || target === null) continue;
    wrapped[member] = {};
    for (const [method, call] of Object.entries(target)) {
      wrapped[member][method] = (...args) => new Promise((resolve, reject) => {
        setTimeout(() => {
          try {
            resolve(call(...args));
          } catch (error) {
            reject(error);
          }
        }, 0);
      });
    }
  }
  return wrapped;
}

const server = await startFakeOpenAI();
const provider = (model = 'fixture-model', limits = { input_limit: 1000, output_limit: 200 }) => ({
  baseUrl: server.url, path: '/v1/chat/completions', apiKey: KEY, model,
  ...(limits && { catalogue: { version: 1, models: [{ id: model, ...limits }] } }),
});
const ask = (content) => ({ messages: [{ role: 'user', content }] });
const replies = (messages) => messages.filter((m) => m.role === 'assistant').map((m) => m.content);

const neograph = await load();
let failures = 0;
async function check(name, body) {
  try {
    await body();
    console.log(`ok   ${name}`);
  } catch (error) {
    failures += 1;
    console.log(`FAIL ${name}: ${error?.stack ?? error}`);
  }
}

await check('a checkpoint handoff is held for inspection and continues when released', async () => {
  const programs = neograph.programs({ limits: LIMITS, provider: provider() });
  try {
    const version = await programs.admit(CHECKPOINT_SOURCE);
    const run = programs.start(version, { input: ask('Handoff and continue') });
    const held = await run.handoff();
    assert.equal(held.runId, run.id);
    assert.match(held.journalHead, /^sha256:[0-9a-f]{64}$/);
    assert.match(held.coordinateId, /^sha256:[0-9a-f]{64}$/);
    assert.match(held.entryId, /^sha256:[0-9a-f]{64}$/);
    assert.equal(held.value.note, 'ready-for-handoff');
    assert.deepEqual(replies(held.value.messages), [replyTo('Handoff and continue')]);

    assert.equal(await run.releaseHandoff(), true);
    assert.equal(await run.releaseHandoff(), false, 'a released handoff is not released twice');
    const done = await run.result;
    assert.equal(done.status, 'completed');
    assert.equal(done.output.finishedBy, 'original');
    assert.equal(replies(done.output.messages).length, 2);
  } finally {
    programs.release();
  }
});

await check('replace continues on the admitted second version with exact remaining budget and kept children', async () => {
  const rootBudget = {
    wallTimeMs: 60000, modelTokens: 20000, monetaryMicrounits: 10000,
    maxConcurrency: 4, maxProgramOperations: 24, maxCoreSteps: 60,
    maxDynamicCompiles: 2, maxChildDepth: 2, maxTotalChildren: 4,
  };
  const childBudget = {
    wallTimeMs: 30000, modelTokens: 5000, monetaryMicrounits: 2000,
    maxConcurrency: 1, maxProgramOperations: 8, maxCoreSteps: 20,
    maxDynamicCompiles: 0, maxChildDepth: 0, maxTotalChildren: 0,
  };
  const programs = neograph.programs({ limits: LIMITS, provider: provider(), budget: rootBudget });
  try {
    const v1 = await programs.admit({ javascript: PARENT_WITH_RETENTION_V1, budgetBounds: boundsUpTo(rootBudget) });
    const v2 = await programs.admit({ javascript: PARENT_WITH_RETENTION_V2, budgetBounds: boundsUpTo(rootBudget) });
    const original = programs.start(v1, { input: ask('Replace and keep child') });

    const proposeHold = await original.handoff();
    assert.equal(proposeHold.value.stage, 'propose-child');
    await original.synthesizeChild('retained', RETAINED_CHILD_SOURCE, { budget: childBudget, ...hostAuthority(childBudget) });
    assert.equal(await original.releaseHandoff(), true);

    const swapHold = await original.handoff();
    assert.equal(swapHold.value.stage, 'swap-parent');
    const retainedChildId = swapHold.value.retainedChildId;
    const childHold = await programs.handoff(retainedChildId);
    assert.equal(childHold.value.stage, 'retained-child-ready');

    const replacement = original.replace(v2, { runId: 'run-replacement-with-child' });
    assert.equal(replacement.id, 'run-replacement-with-child');
    assert.equal(replacement.versionId, v2.id);

    assert.equal(await programs.releaseHandoff(retainedChildId), true);
    const [retiredResult, replacedResult] = await Promise.all([original.result, replacement.result]);
    assert.equal(retiredResult.status, 'cancelled');
    assert.equal(replacedResult.status, 'completed');
    assert.equal(replacedResult.output.finishedBy, 'retain-v2');
    assert.equal(replacedResult.output.previousRunId, original.id);
    assert.equal(replacedResult.output.retainedChildId, retainedChildId);
    assert.equal(replacedResult.output.childOut.childKept, true);
    assert.deepEqual(replies(replacedResult.output.childOut.messages), [replyTo('Retained child')]);
    assert.equal(replies(replacedResult.output.messages).length, 2);

    assert.equal(
      replacedResult.remainingBudget.maxProgramOperations + replacedResult.usage.programOperations,
      retiredResult.remainingBudget.maxProgramOperations,
    );
    assert.equal(
      replacedResult.remainingBudget.maxCoreSteps + replacedResult.usage.coreSteps,
      retiredResult.remainingBudget.maxCoreSteps,
    );
    assert.equal(
      replacedResult.remainingBudget.modelTokens + replacedResult.usage.modelTokens,
      retiredResult.remainingBudget.modelTokens,
    );
    assert.equal(replacedResult.remainingBudget.maxDynamicCompiles, 1);
  } finally {
    programs.release();
  }
});

await check('replace refuses with exact codes and leaves the source run unchanged after each refusal', async () => {
  const programs = neograph.programs({ limits: LIMITS, provider: provider() });
  try {
    const v1 = await programs.admit(CHECKPOINT_SOURCE);
    const v2 = await programs.admit(REPLACEMENT_SOURCE);
    const widerVersion = await programs.admit(REPLACEMENT_WIDER_AUTHORITY);
    const docVersion = await programs.admit({ document: DOCUMENT_ONLY_PROGRAM });

    const original = programs.start(v1, { input: ask('Refusal checks') });
    const held = await original.handoff();
    assert.equal(held.value.note, 'ready-for-handoff');

    const expectRefusal = async (code, invokeReplace) => {
      await assert.rejects(
        invokeReplace().result,
        (error) => {
          assert.ok(error instanceof NeoGraphError, `expected NeoGraphError for ${code}`);
          assert.equal(error.type, 'program');
          assert.equal(error.code, code);
          assert.notEqual(error.witness, undefined);
          return true;
        },
      );
      const status = await programs.result(original.id);
      assert.deepEqual(status, { runId: original.id, status: 'running' });
    };

    await expectRefusal('P_REPLACEMENT_AUTHORITY', () =>
      original.replace(widerVersion, { runId: 'refuse-authority' }));

    await expectRefusal('P_REPLACEMENT_BUDGET', () =>
      original.replace(v2, { runId: 'refuse-budget', budget: { maxProgramOperations: 1 } }));

    await expectRefusal('P_REPLACEMENT_TARGET', () =>
      original.replace(docVersion, { runId: 'refuse-target' }));

    await expectRefusal('P_REPLACEMENT_INPUT', () =>
      original.replace(v2, {
        runId: 'refuse-input',
        input: { handoff: held.value, previous_run_id: 'wrong-predecessor-id' },
      }));

    await expectRefusal('P_REPLACEMENT_HANDOFF', () =>
      original.replace(v2, {
        runId: 'refuse-handoff',
        entryId: 'sha256:' + '0'.repeat(64),
      }));

    const valid = await original.replace(v2, { runId: 'run-valid-after-refusals' }).result;
    assert.equal(valid.status, 'completed');
    assert.equal(valid.output.finishedBy, 'replacement');
    assert.equal(valid.output.previousRunId, original.id);
  } finally {
    programs.release();
  }
});

await check('migrateGraph migrates a live root Core graph run and refuses guard violations', async () => {
  const programs = neograph.programs({ limits: LIMITS, provider: provider('slow') });
  try {
    const v1 = await programs.admit(MIGRATE_V1);
    const v2 = await programs.admit(MIGRATE_V2);
    const jsControlled = await programs.admit(CHECKPOINT_SOURCE);
    assert.notEqual(v1.id, v2.id);

    const guardRun = programs.start(v1, { input: ask('Migrate guards') });
    await sleep(30);

    await assert.rejects(
      guardRun.migrateGraph(v1, { runId: guardRun.id }).result,
      (error) =>
        error instanceof NeoGraphError &&
        error.type === 'program' &&
        error.code === 'P_GRAPH_MIGRATION_SAME_RUN' &&
        error.witness !== undefined,
    );

    await assert.rejects(
      guardRun.migrateGraph(jsControlled, { runId: 'run-migrate-incompatible' }).result,
      (error) =>
        error instanceof NeoGraphError &&
        error.type === 'program' &&
        error.code === 'P_GRAPH_MIGRATION_INCOMPATIBLE' &&
        error.witness !== undefined,
    );
    await guardRun.cancel();
    await guardRun.result;

    const original = programs.start(v1, { input: ask('Migrate graph') });
    // The live run moves to a new generation of its own admitted version, as the C++ and Python tests do.
    const migrated = original.migrateGraph(v1, { runId: 'run-migrated-graph' });
    assert.equal(migrated.id, 'run-migrated-graph');
    assert.equal(migrated.versionId, v1.id);

    const [sourceEnd, migratedEnd] = await Promise.all([original.result, migrated.result]);
    // The source run does not finish its own graph: it is retired to the migrated run, which does.
    assert.equal(sourceEnd.status, 'cancelled');
    assert.equal(migratedEnd.status, 'completed');
    assert.equal(migratedEnd.runId, 'run-migrated-graph');
    assert.equal(migratedEnd.versionId, v1.id);
    assert.ok(migratedEnd.trace.includes('second'), `expected second node in trace: ${JSON.stringify(migratedEnd.trace)}`);
    // The migrated run resumes without the run input as a new message, so the one reply answers the original ask.
    const migratedMessages = migratedEnd.output.channels.messages.value;
    assert.deepEqual(migratedMessages.filter((m) => m.role === 'user').map((m) => m.content), ['Migrate graph']);
    assert.deepEqual(replies(migratedMessages), [replyTo('Migrate graph')]);
  } finally {
    programs.release();
  }
});

await check('child synthesis compiles, admits and publishes a child and refuses wider authority', async () => {
  const programs = neograph.programs({ limits: LIMITS,
    provider: provider(),
    budget: { maxDynamicCompiles: 2, maxChildDepth: 2, maxTotalChildren: 2 },
  });
  try {
    const parentVersion = await programs.admit({ javascript: PARENT_SYNTHESIS_SOURCE, budget: {} });
    const parentRun = programs.start(parentVersion, { input: ask('Synthesize child') });
    const held = await parentRun.handoff();
    assert.deepEqual(held.value, { wantChild: 'helper' });

    await assert.rejects(
      parentRun.synthesizeChild('wider-helper', CHILD_SOURCE, {
        budget: HELPER_BUDGET, capabilities: ['extra.capability'], ...hostAuthority(HELPER_BUDGET),
      }),
      (error) =>
        error instanceof NeoGraphError &&
        error.type === 'program' &&
        error.code === 'P_CHILD_SYNTHESIS_AUTHORITY' &&
        error.witness !== undefined,
    );

    // Without the host's grant and decision nothing is synthesized: the request does not authorize itself.
    await assert.rejects(
      parentRun.synthesizeChild('ungranted-helper', CHILD_SOURCE, { budget: HELPER_BUDGET }),
      (error) => error instanceof NeoGraphError && /needs the host's grant/.test(error.message),
    );

    const synthesized = await parentRun.synthesizeChild('helper', CHILD_SOURCE, {
      budget: HELPER_BUDGET, ...hostAuthority(HELPER_BUDGET),
    });
    assert.equal(synthesized.binding, 'helper');
    assert.equal(synthesized.state, 'bound');
    assert.match(synthesized.versionId, /^sha256:[0-9a-f]{64}$/);
    assert.match(synthesized.bundleId, /^sha256:[0-9a-f]{64}$/);

    assert.equal(await parentRun.releaseHandoff(), true);
    const done = await parentRun.result;
    assert.equal(done.status, 'completed');
    assert.equal(done.output.fromChild.childRan, true);
    assert.deepEqual(replies(done.output.fromChild.messages), [replyTo('Synthesize child')]);
  } finally {
    programs.release();
  }
});

async function verifyHierarchyAndReplacement(programs) {
  const rootBudget = {
    wallTimeMs: 60000, modelTokens: 30000, monetaryMicrounits: 20000,
    maxConcurrency: 4, maxProgramOperations: 32, maxCoreSteps: 100,
    maxDynamicCompiles: 4, maxChildDepth: 3, maxTotalChildren: 12,
  };
  const childBudget = {
    wallTimeMs: 45000, modelTokens: 12000, monetaryMicrounits: 8000,
    maxConcurrency: 2, maxProgramOperations: 16, maxCoreSteps: 50,
    maxDynamicCompiles: 2, maxChildDepth: 2, maxTotalChildren: 4,
  };
  const grandBudget = {
    wallTimeMs: 30000, modelTokens: 5000, monetaryMicrounits: 2000,
    maxConcurrency: 1, maxProgramOperations: 8, maxCoreSteps: 20,
    maxDynamicCompiles: 0, maxChildDepth: 0, maxTotalChildren: 0,
  };

  const rootVersion = await programs.admit({ javascript: TREE_ROOT_SOURCE, budgetBounds: boundsUpTo(rootBudget) });
  const childV2Version = await programs.admit({ javascript: TREE_CHILD_V2_SOURCE, budgetBounds: boundsUpTo(childBudget) });

  const rootRun = programs.start(rootVersion, { input: ask('Tree root'), budget: rootBudget });
  const rootHold = await rootRun.handoff();
  assert.equal(rootHold.value.stage, 'root-propose');

  const childSynth = await rootRun.synthesizeChild('child', TREE_CHILD_V1_SOURCE, {
    budget: childBudget, ...hostAuthority(childBudget),
  });
  assert.equal(childSynth.state, 'bound');
  assert.equal(await rootRun.releaseHandoff(), true);

  const childProposeHold = await programs.handoff();
  assert.equal(childProposeHold.value.stage, 'child-propose');
  const childRunId = childProposeHold.runId;
  assert.notEqual(childRunId, rootRun.id);

  const grandSynth = await programs.synthesizeChild(childRunId, 'grandchild', TREE_GRANDCHILD_SOURCE, {
    budget: grandBudget, ...hostAuthority(grandBudget),
  });
  assert.equal(grandSynth.state, 'bound');
  assert.equal(await programs.releaseHandoff(childRunId), true);

  const childSwapHold = await programs.handoff(childRunId);
  assert.equal(childSwapHold.value.stage, 'child-swap');
  const grandchildRunId = childSwapHold.value.grandchildRunId;

  const grandHold = await programs.handoff(grandchildRunId);
  assert.equal(grandHold.value.stage, 'grandchild-ready');

  const replacedChild = programs.replace(childRunId, childV2Version);
  assert.equal(await programs.releaseHandoff(grandchildRunId), true);

  const [rootDone, childV2Done] = await Promise.all([rootRun.result, replacedChild.result]);
  assert.equal(childV2Done.status, 'completed');
  assert.equal(rootDone.status, 'completed');
  assert.deepEqual(replies(rootDone.output.rootMessages), [replyTo('Tree root')]);
  assert.equal(rootDone.output.childResult.generation, 2);
  assert.equal(rootDone.output.childResult.previousChildRunId, childRunId);
  assert.equal(rootDone.output.childResult.grandchildRunId, grandchildRunId);
  assert.equal(rootDone.output.childResult.grandResult.grandchildRan, true);
  assert.deepEqual(replies(rootDone.output.childResult.grandResult.messages), [replyTo('Tree grandchild')]);
  assert.equal(replies(rootDone.output.childResult.messages).length, 2);
}

await check('a root -> child -> grandchild hierarchy replaces its running child and finishes with the expected result', async () => {
  const programs = neograph.programs({ limits: LIMITS, provider: provider() });
  try {
    await verifyHierarchyAndReplacement(programs);
  } finally {
    programs.release();
  }
});

await check('replacement, child synthesis and hierarchy replacement work through an async JavaScript store object', async () => {
  const backing = neograph.memoryStores();
  const stores = laterStores(backing);
  const programs = neograph.programs({ limits: LIMITS, provider: provider(), stores });
  try {
    const v1 = await programs.admit(CHECKPOINT_SOURCE);
    const v2 = await programs.admit(REPLACEMENT_SOURCE);
    const original = programs.start(v1, { input: ask('Store replace') });
    await original.handoff();
    const replaced = await original.replace(v2, { runId: 'store-replaced-run' }).result;
    assert.equal(replaced.status, 'completed');
    assert.equal(replaced.output.finishedBy, 'replacement');
    assert.equal(replaced.output.previousRunId, original.id);

    await verifyHierarchyAndReplacement(programs);
  } finally {
    programs.release();
    backing.release();
  }
});

await server.close();
if (failures > 0) {
  console.log(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
