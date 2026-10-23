// The Program API of the JavaScript module against a local OpenAI-compatible server, offline and without a key: compile and
// admit a Program from its source, start a run, watch its events, pause it at an interrupt, resume and fork it, cancel it,
// read a result from the stores, and the failures that must stay failures. The runtime awaits its stores and the model
// calls on the one thread of this event loop; nothing here blocks it.
//
//   node wasm/test/program_test.mjs <directory with neograph.mjs, stores.mjs, neograph_module.mjs and neograph_module.wasm>
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { getEventListeners } from 'node:events';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';
import { promisify } from 'node:util';
import { startFakeOpenAI, replyTo, KOREAN } from './fake_openai_server.mjs';

const directory = path.resolve(process.argv[2] ?? '.');
const { load, NeoGraphError } = await import(pathToFileURL(path.join(directory, 'neograph.mjs')).href);

const KEY = 'test-key-0123456789';

// Two model calls with a pause before the second.
const SOURCE = `
export function define() {
  return ng.graph("two-turns")
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "llm_call"})
    .node("second", {type: "llm_call"})
    .entry("first").edge("first", "second").exit("second")
    .interruptBefore("second");
}
export function* main(input) {
  const reply = yield ng.callCore("two-turns", input, "run");
  return {messages: reply.messages};
}
`;

const server = await startFakeOpenAI();
const provider = (model = 'fixture-model', limits = { input_limit: 1000, output_limit: 200 }) => ({
  baseUrl: server.url, path: '/v1/chat/completions', apiKey: KEY, model,
  ...(limits && { catalogue: { version: 1, models: [{ id: model, ...limits }] } }),
});
const ask = (content) => ({ messages: [{ role: 'user', content }] });
// The assistant's messages of a result. A run that pauses reports the graph's state, one that ends what main() returned.
const replies = (result) => (result.output.channels?.messages.value ?? result.output.messages)
  .filter((m) => m.role === 'assistant').map((m) => m.content);

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

// A Programs object with the two-turns Program admitted, released when `body` ends.
async function withProgram(body, model = 'fixture-model') {
  const programs = neograph.programs({ provider: provider(model) });
  try {
    return await body(programs, await programs.admit(SOURCE));
  } finally {
    programs.release();
  }
}

await check('a source is compiled and admitted', () => withProgram(async (programs, version) => {
  assert.match(version.id, /^sha256:[0-9a-f]{64}$/);
  assert.match(version.bundleId, /^sha256:[0-9a-f]{64}$/);
  assert.equal((await programs.admit(SOURCE)).id, version.id, 'the same source is the same version');
  assert.equal((await programs.admit({ javascript: SOURCE, name: 'named.js' })).name, 'named.js');
}));

// A Program that is already a document, as the compiler writes one: a graph called from the root, and the bounds of the
// budgets it can run under. It needs no control source, so a module built without the QuickJS runtime accepts it too.
const DOCUMENT = {
  program_schema_version: 4,
  input_contract: { schema_version: 1, schema: {} },
  output_contract: { schema_version: 1, schema: {} },
  root: {
    op: 'call_core', name: 'one',
    definition: {
      schema_version: 1, name: 'one', channels: { messages: { reducer: 'append' } }, nodes: { llm: { type: 'llm_call' } },
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

await check('a Program given as a document is admitted and run', () => withProgram(async (programs) => {
  const version = await programs.admit({ document: DOCUMENT, name: 'one.json' });
  assert.equal(version.name, 'one.json');
  const result = await programs.run(version, { input: ask('Hi') });
  assert.equal(result.status, 'completed');
  assert.deepEqual(replies(result), [replyTo('Hi')]);
  await assert.rejects(programs.admit({ document: { ...DOCUMENT, root: { op: 'no_such_op' } } }),
    (error) => error instanceof NeoGraphError && error.type === 'compile' && error.diagnostics.length > 0);
}));

await check('a refused source says why', () => withProgram(async (programs) => {
  await assert.rejects(programs.admit('export function define() { throw new Error("no graph"); }'), (error) => {
    assert.ok(error instanceof NeoGraphError);
    assert.equal(error.type, 'compile');
    assert.ok(error.diagnostics.length > 0 && error.diagnostics[0].code.startsWith('P_'), JSON.stringify(error.diagnostics));
    return true;
  });
  // A node type that this module does not register is refused by the compiler, with the pointer of the node.
  const unknown = SOURCE.replace('"second", {type: "llm_call"}', '"second", {type: "no_such_type"}');
  await assert.rejects(programs.admit(unknown), (error) => error instanceof NeoGraphError && error.diagnostics?.length > 0);
  await assert.rejects(programs.admit({}), NeoGraphError);
}));

await check('a run pauses at the interrupt and a resume completes it', () => withProgram(async (programs, version) => {
  const before = server.requests.length;
  const events = [];
  const run = programs.start(version, { input: ask('Hello'), onEvent: (event) => events.push(event) });
  assert.match(run.id, /^run-/);
  const paused = await run.result;
  assert.equal(paused.status, 'interrupted');
  assert.equal(paused.runId, run.id);
  assert.equal(paused.versionId, version.id);
  assert.equal(paused.interrupt.node, 'second');
  assert.equal(paused.interrupt.kind, 'input');
  assert.ok(paused.interrupt.pendingId && paused.checkpointId);
  assert.deepEqual(paused.trace, ['first']);
  assert.deepEqual(replies(paused), [replyTo('Hello')]);
  assert.equal(server.requests.length, before + 1, 'one model call before the pause');
  assert.equal(server.requests.at(-1).headers.authorization, `Bearer ${KEY}`);

  // The events arrive in order, and the reply arrives as tokens of the model call, a character cut between chunks whole.
  assert.deepEqual(events.map((e) => e.sequence), events.map((_, i) => i + 1));
  assert.equal(events[0].kind, 'started');
  assert.equal(events.at(-1).kind, 'terminal');
  assert.equal(events.at(-1).payload.status, 'interrupted');
  const tokens = events.filter((e) => e.payload?.type === 'llm_token').map((e) => e.payload.token);
  assert.equal(tokens.join(''), replyTo('Hello'));
  assert.ok(events.every((e) => e.run_id === run.id));

  const resumedEvents = [];
  const resumed = run.resume('approved', { onEvent: (event) => resumedEvents.push(event) });
  assert.equal(resumed.id, run.id, 'a resumed run keeps its identity');
  const done = await resumed.result;
  assert.equal(done.status, 'completed');
  assert.deepEqual(done.trace, ['second']);
  assert.equal(replies(done).length, 2);
  assert.match(JSON.stringify(replies(done)), new RegExp(KOREAN));
  assert.equal(server.requests.length, before + 2, 'the first call was not repeated');
  assert.ok(resumedEvents.length > 0 && resumedEvents.at(-1).payload.status === 'completed');
  assert.ok(resumedEvents[0].sequence > events.at(-1).sequence, 'the journal continues where the pause stopped');

  // What the run cost is reported, and the model tokens are a share of the budget it was given.
  assert.equal(done.usage.modelTokens, 8);
  assert.equal(done.remainingBudget.modelTokens, 10000 - 16);
  assert.ok(!JSON.stringify([paused, done, events, resumedEvents]).includes(KEY), 'the key is in no result or event');
}));

await check('a fork answers the same pause another way and the first call is not repeated', () => withProgram(async (programs, version) => {
  const before = server.requests.length;
  const original = programs.start(version, { input: ask('Fork me') });
  const paused = await original.result;
  assert.equal(paused.status, 'interrupted');
  let requestsAtPause = server.requests.length;
  assert.equal(requestsAtPause, before + 1);

  const fork = original.fork('rejected');
  assert.notEqual(fork.id, original.id);
  const forked = await fork.result;
  assert.equal(forked.status, 'completed');
  assert.equal(forked.runId, fork.id);
  assert.equal(server.requests.length, requestsAtPause + 1, 'the fork asked the model once, for the second node only');
  // The fork took half of what the paused run had left, and a second fork of the same pause takes half of the rest.
  assert.ok(forked.remainingBudget.modelTokens <= paused.remainingBudget.modelTokens / 2);
  const again = await original.fork('again', { runId: 'run-named-fork' }).result;
  assert.equal(again.runId, 'run-named-fork');
  assert.equal(again.status, 'completed');
  assert.deepEqual(replies(again), [replyTo('Fork me'), replyTo('again')]);
  requestsAtPause += 1;

  const resumed = await original.resume('approved').result;
  assert.equal(resumed.status, 'completed');
  assert.equal(resumed.runId, original.id);
  assert.equal(server.requests.length, requestsAtPause + 2);
  // The answer to a pause before a model call is the next message of the conversation: each run answered its own.
  assert.deepEqual(replies(forked), [replyTo('Fork me'), replyTo('rejected')]);
  assert.deepEqual(replies(resumed), [replyTo('Fork me'), replyTo('approved')]);
}));

await check('a paused run is cancelled, and the stores keep what happened to it', () => withProgram(async (programs, version) => {
  const run = programs.start(version, { input: ask('Stop here') });
  assert.equal((await run.result).status, 'interrupted');
  assert.equal((await programs.result(run.id)).status, 'interrupted', 'the stores hold the pause');
  assert.equal(await run.cancel(), true);
  const stored = await programs.result(run.id);
  assert.equal(stored.status, 'cancelled');
  assert.equal(stored.runId, run.id);
  assert.equal(await run.cancel(), false, 'a cancelled run is not cancelled again');
  assert.equal(await programs.result('run-unknown'), null);
  await assert.rejects(run.resume('late').result, NeoGraphError);
}));

await check('a finished run is read back from the stores', () => withProgram(async (programs, version) => {
  const result = await programs.run(version, { input: ask('Once'), runId: 'run-fixed-id' });
  assert.equal(result.status, 'interrupted');
  const done = await programs.start(version, { input: ask('Twice') }).resume('go').result;
  const stored = await programs.result(done.runId);
  assert.equal(stored.status, 'completed');
  assert.equal(stored.runId, done.runId);
  assert.deepEqual(replies(stored), replies(done));
  await assert.rejects(programs.start(version, { input: ask('Again'), runId: 'run-fixed-id' }).result, NeoGraphError,
    'a run id is used once');
}));

await check('a model call in flight leaves the host event loop running', () => withProgram(async (programs, version) => {
  let ticks = 0;
  const timer = setInterval(() => { ticks += 1; }, 10);
  try {
    const paused = await programs.start(version, { input: ask('wait') }).result;
    assert.equal(paused.status, 'interrupted');
  } finally {
    clearInterval(timer);
  }
  assert.ok(ticks >= 5, `${ticks} timer ticks while the model call was in flight`);
}, 'slow'));

await check('runs in flight together keep their own answers', () => withProgram(async (programs, version) => {
  const prompts = ['one', 'two', 'three', 'four'];
  const results = await Promise.all(prompts.map((prompt) => programs.start(version, { input: ask(prompt) }).result));
  assert.deepEqual(results.map((r) => r.status), prompts.map(() => 'interrupted'));
  assert.deepEqual(results.map((r) => replies(r)[0]), prompts.map(replyTo));
  assert.equal(new Set(results.map((r) => r.runId)).size, prompts.length);
  const finished = await Promise.all(prompts.map((prompt) => programs.start(version, { input: ask(prompt) }).resume('go').result));
  assert.deepEqual(finished.map((r) => r.status), prompts.map(() => 'completed'));
}));

await check('aborting a run cancels the model call', () => withProgram(async (programs, version) => {
  const before = server.aborted;
  const controller = new AbortController();
  const run = programs.start(version, { input: ask('x'), signal: controller.signal });
  await sleep(150);
  controller.abort();
  const result = await run.result;
  assert.equal(result.status, 'cancelled');
  for (let i = 0; i < 40 && server.aborted === before; i += 1) await sleep(25);
  assert.equal(server.aborted, before + 1, 'the connection was not closed');
  assert.equal((await programs.result(run.id)).status, 'cancelled');
}, 'hang'));

await check('an error thrown by onEvent cancels that run and leaves the module running', () => withProgram(async (programs, version) => {
  const boom = new Error('the listener failed');
  const run = programs.start(version, { input: ask('x'), onEvent: () => { throw boom; } });
  await assert.rejects(run.result, (error) => error === boom);
  assert.equal((await programs.result(run.id))?.status, 'cancelled');
  assert.equal((await programs.start(version, { input: ask('after') }).result).status, 'interrupted');
}));

await check("the provider's refusal is a failed run, not a thrown one", () => withProgram(async (programs, version) => {
  const result = await programs.start(version, { input: ask('x') }).result;
  assert.equal(result.status, 'failed');
  assert.equal(result.failure.node, 'first');
  assert.ok(result.failure.message.length > 0);
  assert.ok(!JSON.stringify(result).includes(KEY));
  assert.equal(result.interrupt, undefined);
}, 'fail-401'));

await check('a model the catalogue gives no limits for is not called', () => {
  const before = server.requests.length;
  const programs = neograph.programs({ provider: provider('fixture-model', null) });
  return programs.admit(SOURCE).then(async (version) => {
    const result = await programs.start(version, { input: ask('x') }).result;
    assert.equal(result.status, 'failed');
    assert.match(result.failure.message, /ceilings/);
    assert.equal(server.requests.length, before, 'the call that could not be bounded was not sent');
  }).finally(() => programs.release());
});

await check('a budget outside what the Program admits is refused at the start', () => withProgram(async (programs, version) => {
  await assert.rejects(programs.start(version, { input: ask('x'), budget: { maxCoreSteps: 100000 } }).result, (error) => {
    assert.ok(error instanceof NeoGraphError);
    assert.equal(error.type, 'program');
    assert.equal(error.code, 'P_START_BUDGET');
    assert.equal(error.witness.resource, 'max_core_steps');
    return true;
  });
  // A smaller budget is accepted, and the model-token limit is what the run is bounded by.
  const small = await programs.start(version, { input: ask('x'), budget: { modelTokens: 6000, maxCoreSteps: 10 } }).result;
  assert.equal(small.status, 'interrupted');
  assert.equal(small.remainingBudget.modelTokens, 6000 - 8);
  assert.equal(small.remainingBudget.maxCoreSteps, 9);
  // One call is reserved at its worst case, the model's input limit plus its output limit (1,200 tokens here). A budget
  // that cannot cover it is exhausted before the call is sent.
  const before = server.requests.length;
  const tight = await programs.start(version, { input: ask('x'), budget: { modelTokens: 1199 } }).result;
  assert.equal(tight.status, 'budget_exhausted');
  assert.equal(server.requests.length, before, 'the call that might not fit was not sent');
  const exact = await programs.start(version, { input: ask('x'), budget: { modelTokens: 1200 } }).result;
  assert.equal(exact.status, 'interrupted');
}));

await check('a Programs object that is released refuses further calls', async () => {
  const programs = neograph.programs({ provider: provider() });
  const version = await programs.admit(SOURCE);
  const run = programs.start(version, { input: ask('x') });
  const paused = await run.result;
  assert.equal(paused.status, 'interrupted');
  programs.release();
  programs.release();
  await assert.rejects(programs.admit(SOURCE), NeoGraphError);
  await assert.rejects(programs.start(version, { input: ask('y') }).result, NeoGraphError);
  await assert.rejects(programs.result(run.id), NeoGraphError);
});

await check('an invalid provider is refused when the object is made', () => {
  assert.throws(() => neograph.programs({}), NeoGraphError);
  assert.throws(() => neograph.programs({ provider: { model: 'm' } }), NeoGraphError);
  assert.throws(() => neograph.programs({ provider: provider(), limits: { maxDynamicCompiles: 0 } }), NeoGraphError);
  assert.throws(() => neograph.programs({ provider: { ...provider(), catalogue: { version: 2, models: [] } } }), NeoGraphError);
});

await check('a fork that is cancelled before it is sent is withdrawn and the paused run stays resumable', () => withProgram(async (programs, version) => {
  const run = programs.start(version, { input: ask('Withdraw') });
  const paused = await run.result;
  assert.equal(paused.status, 'interrupted');
  const before = server.requests.length;
  const fork = run.fork('never');
  assert.equal(await fork.cancel(), true);
  await assert.rejects(fork.result, (error) => error instanceof NeoGraphError && error.type === 'cancelled');
  // An abort in the same tick does the same, and so does a signal that was aborted before the call.
  const controller = new AbortController();
  const aborted = run.fork('never', { signal: controller.signal });
  controller.abort();
  await assert.rejects(aborted.result, (error) => error instanceof NeoGraphError && error.type === 'cancelled');
  await assert.rejects(run.fork('never', { signal: AbortSignal.abort() }).result, (error) => error.type === 'cancelled');
  assert.equal(server.requests.length, before, 'no fork reached the model');
  const done = await run.resume('now').result;
  assert.equal(done.status, 'completed');
  assert.equal(server.requests.length, before + 1);
}));

await check('a signal that is aborted before the start means no run, and its listener goes when the call settles', () => withProgram(async (programs, version) => {
  const before = server.requests.length;
  const run = programs.start(version, { input: ask('x'), signal: AbortSignal.abort() });
  await assert.rejects(run.result, (error) => error instanceof NeoGraphError && error.type === 'cancelled');
  assert.equal(server.requests.length, before);
  assert.equal(await programs.result(run.id), null, 'the run was never started');

  const controller = new AbortController();
  const waiting = programs.start(version, { input: ask('y'), signal: controller.signal });
  assert.equal(getEventListeners(controller.signal, 'abort').length, 1);
  assert.equal((await waiting.result).status, 'interrupted');
  await sleep(0);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0, 'a settled call leaves no listener on the signal');
  controller.abort();   // the run waits at an interrupt: the signal no longer reaches it
  assert.equal((await programs.result(waiting.id)).status, 'interrupted');
}));

await check('an abort after the Programs object is released does not crash the host', async () => {
  const unhandled = [];
  const record = (error) => unhandled.push(error);
  process.on('unhandledRejection', record);
  try {
    const programs = neograph.programs({ provider: provider('hang') });
    const version = await programs.admit(SOURCE);
    const controller = new AbortController();
    const run = programs.start(version, { input: ask('y'), signal: controller.signal });
    await sleep(100);
    programs.release();
    controller.abort();   // the call is still pending here: cancel() rejects because the object is released
    assert.equal((await run.result).status, 'cancelled');
    await sleep(50);
    assert.deepEqual(unhandled, []);
  } finally {
    process.off('unhandledRejection', record);
  }
});

await check('a duplicate start does not take the cancellation of the run it duplicates', () => withProgram(async (programs, version) => {
  const first = programs.start(version, { input: ask('slow') });
  await sleep(30);
  const duplicate = programs.start(version, { input: ask('again'), runId: first.id });
  const cancelled = first.cancel();           // while the duplicate is still starting: the run to cancel is the executing one
  await assert.rejects(duplicate.result, NeoGraphError);
  assert.equal(await cancelled, true);
  assert.equal((await first.result).status, 'cancelled');
}, 'slow'));

await check('cancel() says true only for a call that it cancelled', () => withProgram(async (programs, version) => {
  const run = programs.start(version, { input: ask('x') });
  assert.equal((await run.result).status, 'interrupted');
  const fork = run.fork('never');
  assert.equal(await fork.cancel(), true);
  assert.equal(await fork.cancel(), false, 'a withdrawn call is not withdrawn twice');
  await assert.rejects(fork.result, NeoGraphError);
  const done = await run.resume('go').result;
  assert.equal(done.status, 'completed');
  const late = run.resume('late');                 // the first resume ended the run: nothing is sent
  await assert.rejects(late.result, NeoGraphError);
  assert.equal(await late.cancel(), false);
}));

await check('a call that fails to start does not need its result to be read', () => withProgram(async (programs, version) => {
  const unhandled = [];
  const record = (error) => unhandled.push(error);
  process.on('unhandledRejection', record);
  try {
    const refused = programs.start(version, { input: ask('x'), budget: { maxCoreSteps: 100000 } });   // refused; never read
    const notJson = programs.start(version, { input: { n: 10n } });                                  // not JSON; never read
    await Promise.allSettled([refused.result, notJson.result]);   // both have failed by now, whoever reads them
    await sleep(10);
    assert.deepEqual(unhandled, []);
    assert.equal((await programs.start(version, { input: ask('after') }).result).status, 'interrupted');
  } finally {
    process.off('unhandledRejection', record);
  }
}));

await check('a budget with a misspelt or impossible member is refused', () => withProgram(async (programs, version) => {
  for (const budget of [{ modelTokenz: 1000 }, { modelTokens: -1 }, { modelTokens: 1.5 }, { modelTokens: '5' }, { maxCoreSteps: 1e30 },
    { maxConcurrency: 2 ** 32 }, 7]) {
    await assert.rejects(programs.start(version, { input: ask('x'), budget }).result,
      (error) => error instanceof NeoGraphError, JSON.stringify(budget));
  }
  assert.throws(() => neograph.programs({ provider: provider(), limits: { wallTimeMilliseconds: 1 } }), NeoGraphError);
  assert.throws(() => neograph.programs({ provider: provider(), budget: { modelTokens: 'many' } }), NeoGraphError);
  assert.throws(() => programs.start(undefined), NeoGraphError);
  assert.throws(() => programs.start({}), NeoGraphError);
}));

await check('a control source that never ends is stopped by its time limit', async () => {
  const programs = neograph.programs({ provider: provider() });
  try {
    const started = Date.now();
    await assert.rejects(programs.admit('export function define() { for (;;) {} }'),
      (error) => error instanceof NeoGraphError && error.type === 'compile');
    const took = Date.now() - started;
    assert.ok(took >= 3000 && took < 20000, `${took} ms`);
  } finally {
    programs.release();
  }
});

// A control source that recurses without end, or nests deeper than the engine can parse, is stopped by QuickJS's own stack
// limit, which the module applies to its linear-memory stack: `admit` and the run fail with a diagnostic, nothing reaches
// the host as an uncaught exception, and the module instance goes on working. (Without the limit the engine overflows
// the host's own call stack, which no code in the module can catch.)
const settles = (promise, what, ms = 30000) => Promise.race([
  promise,
  sleep(ms).then(() => { throw new Error(`${what} did not settle within ${ms} ms`); }),
]);
const GRAPH = 'ng.graph("g").channel("messages", {reducer: "append", initial: []}).node("n", {type: "llm_call"}).entry("n").exit("n")';
const inDefine = (body) => `export function define() { ${body} return ${GRAPH}; }`;
const RUNAWAY = {
  'direct recursion': inDefine('function f() { return f() + 1; } f();'),
  'mutual recursion': inDefine('function a() { return b() + 1; } function b() { return a() + 1; } a();'),
  'a sort comparator that recurses': inDefine('function g() { [2, 1].sort((x, y) => { g(); return x - y; }); } g();'),
  'a map callback that recurses': inDefine('function walk() { return [1].map(walk); } walk();'),
  'a getter that recurses': inDefine('const o = { get x() { return o.x + 1; } }; o.x;'),
  'a Proxy trap that recurses': inDefine('const p = new Proxy({}, { get(target, key) { return p[key]; } }); p.x;'),
  'a JSON reviver that recurses': inDefine('JSON.parse("[1]", function reviver() { return JSON.parse("[1]", reviver); });'),
  'a constructor that constructs itself': inDefine('function C() { new C(); } new C();'),
  'a generator that delegates to itself': inDefine('function* g() { yield* g(); } g().next();'),
  'a constructor reached through 5,000 bound functions': inDefine('function f() {} let g = f; for (let i = 0; i < 5000; i++) { Object.defineProperty(g, "name", { value: "f" }); g = g.bind(null); } new g();'),
  'JSON.stringify of an array nested 100,000 deep': inDefine('let a = []; for (let i = 0; i < 100000; i++) a = [a]; JSON.stringify(a);'),
  'Array.prototype.flat of an array nested 100,000 deep': inDefine('let a = []; for (let i = 0; i < 100000; i++) a = [a]; a.flat(Infinity);'),
  'JSON.parse of text nested 100,000 deep': inDefine('JSON.parse("[".repeat(100000));'),
  'parentheses nested 2,000 deep': `${inDefine('')}\nconst x = ${'('.repeat(2000)}1${')'.repeat(2000)};`,
  'array literals nested 2,000 deep': `${inDefine('')}\nconst x = ${'['.repeat(2000)}1${']'.repeat(2000)};`,
  'object literals nested 2,000 deep': `${inDefine('')}\nconst x = ${'{a:'.repeat(2000)}1${'}'.repeat(2000)};`,
  'calls nested 2,000 deep': `${inDefine('')}\nfunction f(x) { return x; }\nconst x = ${'f('.repeat(2000)}1${')'.repeat(2000)};`,
};

await check('a control source that recurses too deep is refused and the module goes on', async () => {
  const stray = [];
  const record = (error) => stray.push(error);
  process.on('uncaughtException', record);
  process.on('unhandledRejection', record);
  const programs = neograph.programs({ provider: provider() });
  try {
    for (const [name, source] of Object.entries(RUNAWAY)) {
      await settles(assert.rejects(programs.admit(source), (error) => {
        assert.ok(error instanceof NeoGraphError, name);
        assert.equal(error.type, 'compile', name);
        assert.ok(error.diagnostics.some((d) => d.code === 'P_JS_RESOURCE_LIMIT'), `${name}: ${JSON.stringify(error.diagnostics)}`);
        return true;
      }), `admit of ${name}`);
    }
    assert.deepEqual(stray.map(String), []);
    // The same Programs object admits and runs a Program afterwards, and so does a new one of the same module instance.
    const version = await settles(programs.admit(SOURCE), 'a later admit');
    assert.equal((await settles(programs.start(version, { input: ask('after') }).result, 'a later start')).status, 'interrupted');
    const other = neograph.programs({ provider: provider() });
    try {
      assert.equal((await settles(other.admit(SOURCE), 'admit on a new Programs object')).id, version.id);
    } finally {
      other.release();
    }
    assert.deepEqual(stray.map(String), []);
  } finally {
    process.off('uncaughtException', record);
    process.off('unhandledRejection', record);
    programs.release();
  }
});

await check('recursion and nesting that stay within the limit are not affected', () => withProgram(async (programs) => {
  const within = [
    inDefine('function f(n) { return n === 0 ? 0 : 1 + f(n - 1); } if (f(100) !== 100) throw new Error("recursion");'),
    inDefine('') + `\nconst x = ${'('.repeat(100)}1${')'.repeat(100)};`,
    inDefine('let a = []; for (let i = 0; i < 50; i++) a = [a]; if (JSON.stringify(a).length !== 102) throw new Error("json");'),
  ];
  for (const source of within) assert.match((await settles(programs.admit(source), 'admit')).id, /^sha256:/);
}));

await check('a main() that recurses without end fails its run and the module goes on', async () => {
  const stray = [];
  const record = (error) => stray.push(error);
  process.on('uncaughtException', record);
  process.on('unhandledRejection', record);
  const programs = neograph.programs({ provider: provider() });
  try {
    const recursing = SOURCE.replace('export function* main(input) {', 'export function* main(input) {\n  function f() { return f() + 1; }\n  f();');
    assert.notEqual(recursing, SOURCE);
    const version = await settles(programs.admit(recursing), 'admit');       // define() is fine; main() is not
    const before = server.requests.length;
    const result = await settles(programs.start(version, { input: ask('x') }).result, 'the run');
    assert.equal(result.status, 'failed');
    assert.equal(result.failure.code, 'P_JS_RESOURCE_LIMIT');
    assert.equal(server.requests.length, before, 'no model call was sent');
    const good = await settles(programs.admit(SOURCE), 'a later admit');
    assert.equal((await settles(programs.start(good, { input: ask('after') }).result, 'a later start')).status, 'interrupted');
    assert.deepEqual(stray.map(String), []);
  } finally {
    process.off('uncaughtException', record);
    process.off('unhandledRejection', record);
    programs.release();
  }
});

// The example is the documented way in: run it against the same server.
await check('the Node example runs end to end', async () => {
  const example = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'examples', 'node', 'program.mjs');
  const before = server.requests.length;
  const { stdout, stderr } = await promisify(execFile)(process.execPath,
    [example, server.url, '/v1/chat/completions', 'fixture-model', 'Say hello', '--answer', 'approved'],
    { env: { ...process.env, API_KEY: KEY, NEOGRAPH_WASM_DIR: directory } });
  assert.ok(stdout.includes(replyTo('Say hello')), `the example printed the model's reply:\n${stdout}`);
  assert.match(stdout, /-- interrupted before second/);
  assert.match(stdout, /-- completed/);
  assert.match(stderr, /^completed, 8 model tokens, 9984 left/m);
  assert.equal(server.requests.length, before + 2);
  assert.ok(!stdout.includes(KEY) && !stderr.includes(KEY));
});

await server.close();
if (failures > 0) {
  console.log(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
