// The stores of a JavaScript host under the Program API of the module, offline: `programs({ stores })` takes objects
// whose methods return values or Promises, and the runtime awaits them on the host's event loop. The stores here are the
// module's reference store (the in-memory C++ stores behind the contract of wasm/README.md), wrapped to answer in later
// tasks, to fail, to answer garbage and to write what changes to a log that a second object replays.
//
//   node wasm/test/program_stores_test.mjs <directory with neograph.mjs, stores.mjs, neograph_module.mjs and neograph_module.wasm>
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import { startFakeOpenAI, replyTo } from './fake_openai_server.mjs';

const directory = path.resolve(process.argv[2] ?? '.');
const { load, NeoGraphError } = await import(pathToFileURL(path.join(directory, 'neograph.mjs')).href);

const KEY = 'test-key-0123456789';
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
const provider = () => ({
  baseUrl: server.url, path: '/v1/chat/completions', apiKey: KEY, model: 'fixture-model',
  catalogue: { version: 1, models: [{ id: 'fixture-model', input_limit: 1000, output_limit: 200 }] },
});
const ask = (content) => ({ messages: [{ role: 'user', content }] });
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

// The methods of `stores` answering in a later task of the event loop, as a host's database does. `observe(member, method,
// args)` sees every call before it runs; what it returns (a value to throw) makes the call fail.
function later(stores, { observe, delayMs = 0 } = {}) {
  const wrapped = {};
  for (const [member, target] of Object.entries(stores)) {
    wrapped[member] = {};
    for (const [method, call] of Object.entries(target)) {
      wrapped[member][method] = (...args) => new Promise((resolve, reject) => {
        setTimeout(() => {
          try {
            const failure = observe?.(member, method, args);
            if (failure) throw failure;
            resolve(call(...args));
          } catch (error) {
            reject(error);
          }
        }, delayMs);
      });
    }
  }
  return wrapped;
}

// A pause, a resume and a fork of the pause: the lifecycle that every store must serve.
async function lifecycle(programs) {
  const version = await programs.admit(SOURCE);
  const run = programs.start(version, { input: ask('Hello') });
  const paused = await run.result;
  assert.equal(paused.status, 'interrupted');
  const fork = await run.fork('another way').result;
  const done = await run.resume('go on').result;
  return { version, run, paused, fork, done };
}

await check('stores that return values serve a whole lifecycle', async () => {
  const stores = neograph.memoryStores();
  const programs = neograph.programs({ provider: provider(), stores });
  try {
    const { paused, fork, done } = await lifecycle(programs);
    assert.deepEqual(paused.trace, ['first']);
    assert.equal(fork.status, 'completed');
    assert.equal(done.status, 'completed');
    assert.deepEqual(replies(done), [replyTo('Hello'), replyTo('go on')]);
    assert.deepEqual(replies(fork), [replyTo('Hello'), replyTo('another way')]);
    assert.equal((await programs.result(done.runId)).status, 'completed');
  } finally {
    programs.release();
    stores.release();
  }
});

await check('stores that answer in later tasks serve it too, and the host loop keeps running meanwhile', async () => {
  const reference = neograph.memoryStores();
  const calls = new Map();
  const stores = later(reference, {
    delayMs: 1,
    observe: (member, method) => { calls.set(`${member}.${method}`, (calls.get(`${member}.${method}`) ?? 0) + 1); },
  });
  const programs = neograph.programs({ provider: provider(), stores });
  let ticks = 0;
  const timer = setInterval(() => { ticks += 1; }, 2);
  try {
    const { paused, fork, done } = await lifecycle(programs);
    assert.equal(paused.status, 'interrupted');
    assert.equal(fork.status, 'completed');
    assert.equal(done.status, 'completed');
    assert.deepEqual(replies(done), [replyTo('Hello'), replyTo('go on')]);
  } finally {
    clearInterval(timer);
    programs.release();
    reference.release();
  }
  // Every publication of a state went through the object, and the loop was free to run between its calls.
  assert.ok(calls.get('transitions.comparePublish') >= 6, JSON.stringify([...calls]));
  assert.ok(calls.get('programs.publishAdmitted') >= 1 && calls.get('programs.getVersion') >= 1, JSON.stringify([...calls]));
  assert.ok(ticks >= 20, `${ticks} timer ticks while the stores answered`);
});

await check('a store that rejects fails the call closed, with its message', async () => {
  const reference = neograph.memoryStores();
  const stores = later(reference, {
    observe: (member, method) => (member === 'transitions' && method === 'comparePublish' ? new Error('the disk is full') : undefined),
  });
  const programs = neograph.programs({ provider: provider(), stores });
  try {
    const version = await programs.admit(SOURCE);
    const before = server.requests.length;
    await assert.rejects(programs.start(version, { input: ask('x') }).result,
      (error) => error instanceof NeoGraphError && /the disk is full/.test(error.message));
    assert.equal(server.requests.length, before, 'a run that could not be recorded did not call the model');
  } finally {
    programs.release();
    reference.release();
  }
  // The same for the store that holds the Programs: admission fails and nothing is admitted.
  const refusing = later(neograph.memoryStores(), {
    observe: (member, method) => (member === 'programs' && method === 'publishAdmitted' ? new Error('read-only') : undefined),
  });
  const second = neograph.programs({ provider: provider(), stores: refusing });
  try {
    await assert.rejects(second.admit(SOURCE), (error) => error instanceof NeoGraphError && error.type === 'admission'
      && /read-only/.test(JSON.stringify(error.diagnostics)));
  } finally {
    second.release();
  }
});

await check('a store that throws, or answers a value that is not one, fails the call and names the method', async () => {
  const throwing = neograph.memoryStores();
  const garbage = neograph.memoryStores();
  const programsThrowing = neograph.programs({
    provider: provider(),
    stores: { ...throwing, transitions: { ...throwing.transitions, load() { throw new Error('connection lost'); } } },
  });
  const programsGarbage = neograph.programs({
    provider: provider(),
    stores: { ...garbage, transitions: { ...garbage.transitions, load: () => 'this is not a run record' } },
  });
  try {
    const before = server.requests.length;
    const version = await programsThrowing.admit(SOURCE);
    const thrown = await programsThrowing.start(version, { input: ask('x') }).result;
    assert.equal(thrown.status, 'failed');
    assert.match(thrown.failure.message, /connection lost/);
    const other = await programsGarbage.admit(SOURCE);
    const malformed = await programsGarbage.start(other, { input: ask('x') }).result;
    assert.equal(malformed.status, 'failed');
    assert.match(malformed.failure.message, /transitions\.load/);
    assert.match(malformed.failure.message, /malformed/);
    assert.equal(server.requests.length, before, 'neither run called the model');
  } finally {
    programsThrowing.release();
    programsGarbage.release();
    throwing.release();
    garbage.release();
  }
});

await check('an object without a required method is refused when the Programs object is made', () => {
  const reference = neograph.memoryStores();
  try {
    const { load, ...rest } = reference.transitions;
    assert.throws(() => neograph.programs({ provider: provider(), stores: { ...reference, transitions: rest } }),
      (error) => error instanceof NeoGraphError && /transitions\.load/.test(error.message) && /required/.test(error.message));
    assert.throws(() => neograph.programs({ provider: provider(), stores: { transitions: 42 } }), NeoGraphError);
    assert.throws(() => neograph.programs({ provider: provider(), stores: 'stores' }), NeoGraphError);
  } finally {
    reference.release();
  }
});

await check('a fork that is cancelled while the stores are still answering ends cancelled and sends nothing', async () => {
  const reference = neograph.memoryStores();
  const programs = neograph.programs({ provider: provider(), stores: later(reference, { delayMs: 10 }) });
  try {
    const version = await programs.admit(SOURCE);
    const run = programs.start(version, { input: ask('fork') });
    assert.equal((await run.result).status, 'interrupted');
    for (const wait of [0, 15, 50]) {
      const before = server.requests.length;
      const fork = run.fork('go');
      await sleep(wait);
      assert.equal(await fork.cancel(), true, `cancel after ${wait} ms`);
      const result = await fork.result;
      assert.equal(result.status, 'cancelled', `fork cancelled after ${wait} ms`);
      assert.equal((await programs.result(fork.id)).status, 'cancelled');
      assert.equal(server.requests.length, before, 'the model was not called');
    }
    assert.equal((await run.resume('still here').result).status, 'completed');
  } finally {
    programs.release();
    reference.release();
  }
});

await check('forks of one pause started in the same tick take their shares one after another while the stores answer later', async () => {
  const reference = neograph.memoryStores();
  const programs = neograph.programs({ provider: provider(), stores: later(reference, { delayMs: 3 }) });
  try {
    const version = await programs.admit(SOURCE);
    const run = programs.start(version, { input: ask('Crowd') });
    const paused = await run.result;
    assert.equal(paused.status, 'interrupted');
    // The lineage read, the debit and the publication of each fork wait for a later task, and the forks are made in one
    // turn: a share read before the earlier forks have published would be half of the same amount for all of them.
    const seen = [[], [], []];
    const forks = seen.map((events, i) => run.fork(`go-${i}`, { runId: `run-crowd-${i}`, onEvent: (event) => events.push(event) }));
    const results = await Promise.all(forks.map((fork) => fork.result));
    assert.deepEqual(results.map((result) => result.status), ['completed', 'completed', 'completed']);
    let left = paused.remainingBudget.modelTokens;
    for (const [i, events] of seen.entries()) {
      const share = events.find((event) => event.kind === 'started').payload.model_tokens;
      assert.equal(share, Math.floor(left / 2), `fork ${i} took half of what the forks before it left`);
      left -= share;
    }
    const resumed = await run.resume('own').result;
    assert.equal(resumed.status, 'completed');
    assert.equal(resumed.remainingBudget.modelTokens, left - resumed.usage.modelTokens);
    // The same request made again is the same fork, with the share it was given; the stores hold the amount.
    const again = await run.fork('go-1', { runId: 'run-crowd-1' }).result;
    assert.equal(again.status, 'completed');
    assert.deepEqual(again.remainingBudget, results[1].remainingBudget);
  } finally {
    programs.release();
    reference.release();
  }
});

await check('a stores object with a misspelt member, or without the lineage readers, is refused', () => {
  const reference = neograph.memoryStores();
  try {
    assert.throws(() => neograph.programs({ provider: provider(), stores: { transitons: reference.transitions } }),
      (error) => error instanceof NeoGraphError && /transitons/.test(error.message));
    for (const method of ['loadJavascriptCommands', 'loadMigrationPlan', 'loadRunLineage', 'loadGeneration']) {
      const { [method]: left, ...incomplete } = reference.transitions;
      assert.throws(() => neograph.programs({ provider: provider(), stores: { transitions: incomplete } }),
        (error) => error instanceof NeoGraphError && error.message.includes(method), method);
    }
    // A refused object leaves nothing registered behind: the same stores serve a Programs object afterwards.
    const programs = neograph.programs({ provider: provider(), stores: reference });
    programs.release();
  } finally {
    reference.release();
  }
});

await check('a durable wrapper keeps what a second object reads back after the first is released', async () => {
  const CHANGES = new Set(['transitions.comparePublish', 'transitions.comparePublishExecution', 'programs.publishAdmitted',
    'programs.compareActivate', 'programs.collectGarbage', 'grants.admit', 'grants.revoke', 'fragments.publish',
    'fragments.compareUpdate', 'modules.publish', 'modules.setLifecycle']);
  const log = [];
  const first = neograph.memoryStores();
  const durable = {};
  for (const [member, target] of Object.entries(first)) {
    durable[member] = {};
    for (const [method, call] of Object.entries(target)) {
      durable[member][method] = async (...args) => {
        const result = call(...args); // the reference store decides
        if (CHANGES.has(`${member}.${method}`)) log.push({ member, method, args });
        await sleep(0);
        return result;
      };
    }
  }
  const one = neograph.programs({ provider: provider(), stores: durable });
  let version;
  let done;
  let waiting;
  try {
    version = await one.admit(SOURCE);
    done = await one.start(version, { input: ask('Kept') }).resume('and this').result;
    assert.equal(done.status, 'completed');
    waiting = await one.start(version, { input: ask('Waiting') }).result;
    assert.equal(waiting.status, 'interrupted');
  } finally {
    one.release();
    first.release();
  }
  assert.ok(log.length > 0);

  // A fresh reference store, brought to the state of the log by replaying the calls that changed something.
  const second = neograph.memoryStores();
  for (const { member, method, args } of log) second[member][method](...args);
  const two = neograph.programs({ provider: provider(), stores: second });
  try {
    const stored = await two.result(done.runId);
    assert.equal(stored.status, 'completed');
    assert.deepEqual(replies(stored), replies(done));
    assert.equal((await two.result(waiting.runId)).status, 'interrupted', 'a run that waited at an interrupt is still waiting');
    // The Program admitted by the first object is in the second object's store: it starts without being admitted again.
    const started = await two.start(version, { input: ask('Again') }).result;
    assert.equal(started.status, 'interrupted');
    assert.equal(started.versionId, version.id);
  } finally {
    two.release();
    second.release();
  }
});

await check('releasing the object cancels a run that is executing and leaves one that waits as it is', async () => {
  const stores = neograph.memoryStores();
  const first = neograph.programs({ provider: provider(), stores });
  const hangProvider = { ...provider(), model: 'hang', catalogue: { version: 1, models: [{ id: 'hang', input_limit: 1000, output_limit: 200 }] } };
  const hanging = neograph.programs({ provider: hangProvider, stores });
  try {
    const version = await first.admit(SOURCE);
    const waiting = await first.start(version, { input: ask('Waiting') }).result;
    assert.equal(waiting.status, 'interrupted');
    const hangVersion = await hanging.admit(SOURCE);
    const aborted = server.aborted;
    const running = hanging.start(hangVersion, { input: ask('Running') });
    await sleep(150);
    hanging.release();
    const ended = await running.result;
    assert.equal(ended.status, 'cancelled');
    for (let i = 0; i < 40 && server.aborted === aborted; i += 1) await sleep(25);
    assert.equal(server.aborted, aborted + 1, 'the connection of the model call was closed');
    // What each run left in the stores, read by a third object.
    const reader = neograph.programs({ provider: provider(), stores });
    try {
      assert.equal((await reader.result(waiting.runId)).status, 'interrupted');
      assert.equal((await reader.result(ended.runId)).status, 'cancelled');
    } finally {
      reader.release();
    }
  } finally {
    first.release();
    hanging.release();
    stores.release();
  }
});

await check('the default stores and the reference store agree on a whole lifecycle', async () => {
  const viaDefault = neograph.programs({ provider: provider() });
  const reference = neograph.memoryStores();
  const viaReference = neograph.programs({ provider: provider(), stores: reference });
  try {
    const a = await lifecycle(viaDefault);
    const b = await lifecycle(viaReference);
    assert.deepEqual([a.paused.status, a.fork.status, a.done.status], [b.paused.status, b.fork.status, b.done.status]);
    assert.deepEqual(a.done.trace, b.done.trace);
    assert.deepEqual(replies(a.done), replies(b.done));
    assert.equal(a.done.usage.modelTokens, b.done.usage.modelTokens);
  } finally {
    viaDefault.release();
    viaReference.release();
    reference.release();
  }
});

await server.close();
if (failures > 0) {
  console.log(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
