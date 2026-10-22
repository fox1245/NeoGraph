// What the store conformance suites cannot reach (#556, #557): the adapters of wasm/js_stores.h against a JavaScript
// store whose methods return Promises that settle in later tasks of the event loop.
//
// The WebAssembly module store_checks has no main. This script is its host: it starts a scenario, runs the host's tasks
// of the module one at a time, and between rounds returns to Node's event loop, which is where a Promise settles. Nothing
// in the module waits for anything. The scenarios and what each expects are in wasm/store_checks.cpp.
//
//   node wasm/test/js_stores_test.mjs <directory with store_checks.mjs, store_checks.wasm and stores.mjs>
import assert from 'node:assert/strict';
import path from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';

const directory = path.resolve(process.argv[2] ?? '.');
const { default: createModule } = await import(pathToFileURL(path.join(directory, 'store_checks.mjs')).href);
const { installStoreBridge, registerStores, releaseStores, memoryStores } = await import(
  pathToFileURL(path.join(directory, 'stores.mjs')).href);

const module = await createModule({ print: (text) => console.log(text), printErr: (text) => console.log(text) });
installStoreBridge(module);

const MEMBERS = ['transitions', 'programs', 'grants', 'fragments', 'modules'];
const text = (pointer) => module.UTF8ToString(pointer);
const report = () => JSON.parse(text(module._ng_check_report()));
const bridge = () => JSON.parse(text(module._ng_check_bridge()));
const state = () => module._ng_check_state();
const register = (stores) => registerStores(module, stores);

// --- stores objects -------------------------------------------------------------------------------------------------

// The reference store, with every method rewritten by `wrap(member, method, call)`.
function derived(target, wrap) {
  const out = {};
  for (const member of MEMBERS) {
    out[member] = {};
    for (const method of Object.keys(target[member])) {
      out[member][method] = (...args) => wrap(member, method, () => target[member][method](...args));
    }
  }
  return out;
}
// Every method returns a Promise that settles `ms` after the call.
const delayed = (target, ms) => derived(target, (member, method, call) => new Promise((resolve, reject) =>
  setTimeout(() => { try { resolve(call()); } catch (error) { reject(error); } }, ms)));
// Every method returns a Promise that stays pending until the gate opens.
function gated(target) {
  let open;
  const opened = new Promise((resolve) => { open = resolve; });
  const stores = derived(target, (member, method, call) => opened.then(() => call()));
  return { stores, open };
}
// Some methods return their value and some a Promise: every other method, in the order of the table.
const mixed = (target) => {
  let index = 0;
  return derived(target, (member, method, call) => (index++ % 2 === 0 ? call()
    : new Promise((resolve, reject) => setTimeout(() => { try { resolve(call()); } catch (error) { reject(error); } }, 1))));
};
// Every method of the table, with the behaviour `behave(member, method)` returns, which is a function of the arguments.
function made(target, behave) {
  const out = {};
  for (const member of MEMBERS) {
    out[member] = {};
    for (const method of Object.keys(target[member])) out[member][method] = behave(member, method);
  }
  return out;
}
const failure = (message, kind) => Object.assign(new Error(message), kind ? { kind } : {});

// --- the host loop ---------------------------------------------------------------------------------------------------

// Runs the module's host tasks until `done()`; between rounds it lets Node settle Promises. False if it took too long.
async function pump(done, { rounds = 4000, tasks = 400000 } = {}) {
  let ran = 0;
  for (let round = 0; round < rounds; round += 1) {
    while (module._ng_check_step()) {
      ran += 1;
      assert.ok(ran < tasks, 'the host loop did not go quiet');
    }
    if (done()) return true;
    await sleep(1);
  }
  return false;
}
// Starts a scenario with the stores objects registered for it. Returns a function that ends it.
function begin(scenario, plain, other) {
  const handles = [plain, other].map((stores) => (stores ? register(stores) : 0));
  assert.equal(module._ng_check_begin(scenario, handles[0], handles[1]), 0, text(module._ng_check_report()));
  return () => {
    module._ng_check_end();
    for (const handle of handles) if (handle) releaseStores(module, handle);
  };
}

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
const waitingIs = (count) => () => bridge().waiting === count;

// --- (a) a call that completes through a Promise ----------------------------------------------------------------------

await check('a call that returns a Promise resumes the coroutine in a later turn of the loop, on the loop\'s executor', async () => {
  const memory = memoryStores(module);
  const end = begin(0, memory, delayed(memory, 5));
  try {
    // The seed run over the immediate stores and the first call through the Promise store happen in one stretch.
    assert.ok(await pump(waitingIs(1)), 'the call never reached its Promise');
    assert.equal(state(), 0, 'the coroutine is waiting');
    assert.equal(module._ng_check_idle(), 1, 'a waiting call keeps no task of the host ready and no turn scheduled');
    const stepsWhileWaiting = module._ng_check_steps();
    await sleep(40);   // the Promise settles here
    assert.equal(state(), 0, 'the settlement does not resume the coroutine by itself');
    assert.equal(module._ng_check_idle(), 0, 'the settlement scheduled a turn of the loop');
    assert.equal(bridge().completed, 0);
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 1, JSON.stringify(report()));
    const found = report();
    assert.equal(found.seed_status, 'completed');
    assert.equal(found.in_turn_before, true);
    assert.equal(found.in_turn_after, true, 'the coroutine goes on inside a turn of the loop');
    assert.equal(found.on_loop_executor, true);
    assert.equal(found.record_found, true);
    assert.equal(found.record_matches, true, 'the record that came through the Promise is the run that was stored');
    assert.equal(found.missing_is_null, true);
    assert.equal(found.pending_calls, 1);
    assert.equal(found.completed_calls, 1);
    assert.ok(found.steps_across_call >= 1, `${found.steps_across_call} host tasks between the call and its resumption`);
    assert.ok(module._ng_check_steps() > stepsWhileWaiting);
    assert.equal(bridge().waiting, 0);
    assert.equal(bridge().dropped, 0);
  } finally {
    end();
  }
});

await check('a value that is not a Promise completes the call at once, without a turn of the loop', async () => {
  const memory = memoryStores(module);
  const before = bridge();
  const end = begin(1, memory, null);
  try {
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 1, JSON.stringify(report()));
    assert.equal(module._ng_check_steps(), 1, 'one task of the host ran: the turn that started the coroutine');
    const found = report();
    assert.ok(found.calls >= 100, `${found.calls} calls`);
    // Every call has an answer from the store. The one refusal is the store's own decision, with its own exception class:
    // a version that was never published cannot be activated.
    for (const row of found.rows) {
      if (row.call === 'programs.compareActivate') {
        assert.deepEqual([row.outcome, row.type, row.message],
          ['exception', 'invalid_argument', 'Cannot activate an unpublished Program version']);
      } else {
        assert.equal(row.outcome, 'value', JSON.stringify(row));
      }
    }
    const after = bridge();
    assert.equal(after.pending, before.pending, 'no call returned a Promise');
    assert.equal(after.calls - before.calls, found.calls);
    assert.equal(after.immediate - before.immediate, found.calls);
  } finally {
    end();
  }
});

// --- (b) failures, (c) values that are not the type -----------------------------------------------------------------

const rowsOf = () => Object.fromEntries(report().rows.map((row) => [row.call, row]));

await check('a rejected Promise or a thrown exception is a C++ exception with the JavaScript message', async () => {
  const memory = memoryStores(module);
  const stores = made(memory, (member, method) => {
    const key = `${member}.${method}`;
    switch (key) {
      case 'transitions.load': return async () => { throw failure('load was rejected'); };
      case 'transitions.latest': return () => { throw failure('latest threw'); };
      case 'transitions.loadRunPublicationHead': return () => Promise.reject('a plain string, not an Error');
      case 'transitions.loadEvents': return () => Promise.reject(failure('bad arguments', 'invalid_argument'));
      case 'transitions.loadEffectiveRuntimeState': return () => { throw failure('out of range', 'out_of_range'); };
      case 'programs.getVersion': return () => new Promise((resolve, reject) => setTimeout(() => reject(failure('late rejection')), 2));
      case 'programs.compareActivate': return () => { throw failure('a logic error', 'logic_error'); };
      case 'programs.listVersions': return () => Promise.reject(failure('a class nobody knows', 'no_such_class'));
      case 'programs.collectGarbage': return () => Promise.reject(failure('retention failed', 'runtime_error'));
      case 'grants.revoke': return () => Promise.reject(failure('revoke failed'));
      case 'grants.admit': return () => { throw failure('admit threw'); };
      case 'fragments.load': return async () => { throw failure('fragments are down'); };
      case 'modules.get': return () => { throw failure('modules threw'); };
      default: return () => Promise.reject(failure(`${key} failed`));
    }
  });
  const end = begin(2, null, stores);
  try {
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 1, JSON.stringify(report()));
    const rows = rowsOf();
    const expectations = {
      'transitions.load': ['runtime_error', 'load was rejected'],
      'transitions.latest': ['runtime_error', 'latest threw'],
      'transitions.loadRunPublicationHead': ['runtime_error', 'a plain string, not an Error'],
      'transitions.loadEvents': ['invalid_argument', 'bad arguments'],
      'transitions.loadEffectiveRuntimeState': ['out_of_range', 'out of range'],
      'programs.getVersion': ['runtime_error', 'late rejection'],
      'programs.compareActivate': ['logic_error', 'a logic error'],
      'programs.listVersions': ['runtime_error', 'a class nobody knows'],
      'programs.collectGarbage': ['runtime_error', 'retention failed'],
      'grants.revoke': ['runtime_error', 'revoke failed'],
      'grants.admit': ['runtime_error', 'admit threw'],
      'fragments.load': ['runtime_error', 'fragments are down'],
      'modules.get': ['runtime_error', 'modules threw'],
    };
    for (const [call, [type, message]] of Object.entries(expectations)) {
      assert.equal(rows[call]?.outcome, 'exception', `${call}: ${JSON.stringify(rows[call])}`);
      assert.equal(rows[call].type, type, call);
      assert.equal(rows[call].message, message, call);
    }
    // No call returned a value in place of its failure.
    assert.ok(report().rows.every((row) => row.outcome === 'exception'));
    assert.equal(bridge().waiting, 0);
  } finally {
    end();
  }
});

await check('a value that does not parse or has the wrong shape fails the call, and the failure names the method', async () => {
  const memory = memoryStores(module);
  const junk = {
    'transitions.load': 'this is not a record',
    'transitions.latest': 42,
    'transitions.loadRunPublicationHead': { run_record: 1 },
    'transitions.loadEvents': 'not an array',
    'transitions.loadEffectiveRuntimeState': {},
    'programs.getVersion': ['x'],
    'programs.compareActivate': 'bogus',
    'programs.listVersions': {},
    'programs.collectGarbage': { versions_removed: -1, bundles_removed: 0, references: [] },
    'grants.revoke': 'yes',
    'grants.admit': undefined,
    'fragments.load': Promise.resolve(7),
    'modules.get': true,
  };
  const stores = made(memory, (member, method) => {
    const key = `${member}.${method}`;
    return key in junk ? () => junk[key] : () => null;
  });
  const end = begin(3, null, stores);
  try {
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 1, JSON.stringify(report()));
    const rows = rowsOf();
    for (const call of Object.keys(junk)) {
      assert.equal(rows[call]?.outcome, 'exception', `${call}: ${JSON.stringify(rows[call])}`);
      assert.equal(rows[call].type, 'runtime_error', call);
      assert.ok(rows[call].message.includes(`'${call}'`), `${call}: ${rows[call].message}`);
      assert.ok(rows[call].message.includes('malformed value'), `${call}: ${rows[call].message}`);
    }
    // A result that is null where the type is optional is an absent value, not a failure.
    assert.equal(rows['fragments.findPublishedLineage'].outcome, 'value');
  } finally {
    end();
  }
});

// --- (d) a mixed object --------------------------------------------------------------------------------------------------

await check('an object whose methods are partly immediate and partly pending runs a Program and reads it back', async () => {
  const memory = memoryStores(module);
  const before = bridge();
  const end = begin(4, null, mixed(memory));
  try {
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 1, JSON.stringify(report()));
    const found = report();
    assert.equal(found.status, 'completed');
    assert.equal(found.version_read_back, true);
    assert.equal(found.run_read_back, true);
    const after = bridge();
    assert.ok(after.pending - before.pending > 0, 'some calls were pending');
    assert.ok(after.immediate - before.immediate > 0, 'some calls were immediate');
    assert.equal(after.waiting, 0);
  } finally {
    end();
  }
});

// --- (e) an outcome that arrives after the coroutine is gone ---------------------------------------------------------------

await check('a Promise that settles after its loop was destroyed is dropped', async () => {
  const memory = memoryStores(module);
  const gate = gated(memory);
  const before = bridge();
  const end = begin(5, null, gate.stores);
  try {
    assert.ok(await pump(waitingIs(1)), 'the call never reached its Promise');
    assert.equal(report().started, true);
  } finally {
    end();   // the loop, and the coroutine that waits, are destroyed here
  }
  assert.equal(bridge().waiting, 0, 'the destroyed coroutine took its wait with it');
  gate.open();
  await sleep(30);
  const after = bridge();
  assert.equal(after.dropped - before.dropped, 1, 'the late outcome was dropped');
  assert.equal(after.completed, before.completed);
  assert.equal(after.waiting, 0);
});

await check('a Promise that settles after its coroutine was cancelled is dropped', async () => {
  const memory = memoryStores(module);
  const gate = gated(memory);
  const before = bridge();
  const end = begin(6, null, gate.stores);
  try {
    assert.ok(await pump(waitingIs(1)), 'the call never reached its Promise');
    module._ng_check_cancel();
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 2, 'the cancelled coroutine ends with an error');
    const found = report();
    assert.equal(found.resumed, undefined);
    assert.equal(found.cancelled.type, 'system_error', JSON.stringify(found));
    assert.equal(bridge().waiting, 0, 'the cancelled coroutine unregistered its wait');
    gate.open();
    await sleep(30);
    await pump(() => false, { rounds: 5 });
    const after = bridge();
    assert.equal(after.dropped - before.dropped, 1, 'the late outcome was dropped');
    assert.equal(after.completed, before.completed);
  } finally {
    end();
  }
});

await check('the module goes on working after outcomes were dropped', async () => {
  const end = begin(1, memoryStores(module), null);
  try {
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 1, JSON.stringify(report()));
  } finally {
    end();
  }
});

// --- (f) objects that cannot be adapted -------------------------------------------------------------------------------------

const noop = () => null;
const adapt = (stores) => {
  const handle = register(stores);
  try {
    return JSON.parse(text(module._ng_check_adapt(handle)));
  } finally {
    releaseStores(module, handle);
  }
};

await check('a missing required method fails adapt_js_stores, naming it', async () => {
  const required = { transitions: ['load', 'latest', 'loadEvents', 'loadEffects', 'comparePublish'],
    programs: ['publishAdmitted', 'getBundle', 'getVersion'], grants: ['admit', 'load', 'revoke'],
    fragments: ['publish', 'load', 'compareUpdate'], modules: ['publish', 'get', 'setLifecycle'] };
  const full = (member) => Object.fromEntries(required[member].map((method) => [method, noop]));
  // All required methods: the member is adapted, and so is an object with no members at all.
  assert.deepEqual(adapt({}), { ok: true, members: [] });
  assert.deepEqual(adapt(Object.fromEntries(MEMBERS.map((member) => [member, full(member)]))).members, MEMBERS);
  // Each required method missing, one at a time.
  for (const member of MEMBERS) {
    for (const method of required[member]) {
      const without = { ...full(member) };
      delete without[method];
      const result = adapt({ [member]: without });
      assert.equal(result.ok, false, `${member} without ${method} was adapted`);
      assert.equal(result.type, 'invalid_argument');
      assert.ok(result.message.includes(method) && result.message.includes(member), result.message);
    }
  }
  // A member that is not an object, a method that is not a function, a handle that names nothing.
  assert.throws(() => register({ transitions: 5 }), TypeError);
  const notCallable = adapt({ grants: { ...full('grants'), revoke: 5 } });
  assert.equal(notCallable.ok, false);
  assert.ok(notCallable.message.includes('revoke') && notCallable.message.includes('function'), notCallable.message);
  const unknown = JSON.parse(text(module._ng_check_adapt(987654)));
  assert.equal(unknown.ok, false);
  assert.equal(unknown.type, 'invalid_argument');
});

await check('an optional method that the object lacks keeps the interface\'s default', async () => {
  const memory = memoryStores(module);
  const required = { transitions: ['load', 'latest', 'loadEvents', 'loadEffects', 'comparePublish'],
    programs: ['publishAdmitted', 'getBundle', 'getVersion'], grants: ['admit', 'load', 'revoke'],
    fragments: ['publish', 'load', 'compareUpdate'], modules: ['publish', 'get', 'setLifecycle'] };
  const stores = Object.fromEntries(MEMBERS.map((member) => [member,
    Object.fromEntries(required[member].map((method) => [method, (...args) => memory[member][method](...args)]))]));
  const end = begin(2, null, stores);
  try {
    assert.ok(await pump(() => state() !== 0));
    assert.equal(state(), 1, JSON.stringify(report()));
    const rows = rowsOf();
    // The default of loadRunPublicationHead reads the record and the journal; an absent run is an absent head.
    assert.equal(rows['transitions.loadRunPublicationHead'].outcome, 'value');
    // The defaults that fail closed still do, and as the interface does: a logic error, not a made-up value.
    assert.equal(rows['transitions.loadJavascriptCommands'].outcome, 'exception');
    assert.equal(rows['programs.getActivation'].outcome, 'exception');
    assert.equal(rows['programs.getActivation'].type, 'logic_error');
    assert.equal(rows['programs.listVersions'].type, 'logic_error');
    assert.equal(rows['programs.collectGarbage'].type, 'logic_error');
    assert.equal(rows['programs.compareActivate'].type, 'logic_error');
    // The default of an optional lookup is "nothing found".
    assert.equal(rows['fragments.findPublishedLineage'].outcome, 'value');
  } finally {
    end();
  }
});

// --- (g) a whole Program run -------------------------------------------------------------------------------------------------

await check('a Program completes over adapters whose methods all return Promises', async () => {
  const memory = memoryStores(module);
  const before = bridge();
  const end = begin(8, null, delayed(memory, 1));
  try {
    assert.ok(await pump(() => state() !== 0, { rounds: 20000 }), 'the run did not finish');
    assert.equal(state(), 1, JSON.stringify(report()));
    const found = report();
    assert.equal(found.status, 'completed', JSON.stringify(found));
    assert.equal(found.run_read_back, true);
    assert.equal(found.head_read_back, true);
    assert.equal(found.version_read_back, true);
    assert.equal(found.catalog_read, true);
    const after = bridge();
    assert.ok(after.pending - before.pending >= 10, `${after.pending - before.pending} calls were pending`);
    assert.equal(after.immediate, before.immediate, 'every call went through a Promise');
    assert.equal(after.completed - before.completed, after.pending - before.pending);
    assert.equal(after.waiting, 0);
    assert.equal(after.dropped, before.dropped);
    // The Program is in the reference store the Promises wrapped, not only in the adapters' view of it.
    const stored = memory.transitions.load('store:tenant', found.run_id);
    assert.ok(stored !== null && typeof stored === 'string', 'the run is stored');
  } finally {
    end();
  }
});

await check('a durable store can wrap the reference store: replaying the logged calls restores the state', async () => {
  const CHANGES = new Set(['transitions.comparePublish', 'transitions.comparePublishExecution', 'programs.publishAdmitted',
    'programs.compareActivate', 'programs.collectGarbage', 'grants.admit', 'grants.revoke', 'fragments.publish',
    'fragments.compareUpdate', 'modules.publish', 'modules.setLifecycle']);
  const memory = memoryStores(module);
  const log = [];
  // The wrapper of wasm/README.md: it delegates to the reference store and writes down the calls that change something,
  // with their arguments.
  const logged = {};
  for (const member of MEMBERS) {
    logged[member] = {};
    for (const method of Object.keys(memory[member])) {
      logged[member][method] = (...args) => {
        if (CHANGES.has(`${member}.${method}`)) log.push({ member, method, args });
        return memory[member][method](...args);
      };
    }
  }
  const end = begin(8, null, logged);
  let found;
  try {
    assert.ok(await pump(() => state() !== 0, { rounds: 20000 }));
    assert.equal(state(), 1, JSON.stringify(report()));
    found = report();
    assert.equal(found.status, 'completed');
  } finally {
    end();
  }
  assert.ok(log.length >= 3, `${log.length} calls were logged`);
  // Replay into a fresh reference store: a refused call is refused again, and the state is the same.
  const fresh = memoryStores(module);
  let refused = 0;
  for (const { member, method, args } of log) {
    try { fresh[member][method](...args); } catch { refused += 1; }
  }
  const owner = 'store:tenant';
  assert.equal(fresh.transitions.load(owner, found.run_id), memory.transitions.load(owner, found.run_id));
  assert.equal(fresh.transitions.latest(owner, found.run_id), memory.transitions.latest(owner, found.run_id));
  assert.deepEqual(fresh.transitions.loadEvents(owner, found.run_id, 0), memory.transitions.loadEvents(owner, found.run_id, 0));
  assert.equal(fresh.programs.getVersion(owner, found.version_id), memory.programs.getVersion(owner, found.version_id));
  assert.notEqual(fresh.transitions.load(owner, found.run_id), null);
  assert.equal(refused, 0, 'nothing the run did was refused');
});

await check('a Program over stores that reject every call fails closed, and does not hang', async () => {
  const memory = memoryStores(module);
  const rejecting = made(memory, (member, method) => () => new Promise((resolve, reject) =>
    setTimeout(() => reject(failure('the store is down')), 1)));
  const end = begin(9, memory, rejecting);
  try {
    assert.ok(await pump(() => state() !== 0, { rounds: 20000 }), 'the run did not finish: it hung');
    assert.equal(state(), 1, JSON.stringify(report()));
    const found = report();
    assert.equal(found.admitted, true);
    assert.notEqual(found.status, 'completed', JSON.stringify(found));
    assert.ok(found.status === 'failed' || found.status === 'exception', JSON.stringify(found));
    if (found.status === 'exception') assert.ok(found.message.includes('the store is down'), found.message);
    assert.equal(bridge().waiting, 0);
  } finally {
    end();
  }
});

await check('a Program whose store loses a publication in the middle of the run does not complete, and does not hang', async () => {
  const memory = memoryStores(module);
  let publications = 0;
  const stores = derived(memory, (member, method, call) => {
    // A one-node run publishes twice, through comparePublish or comparePublishExecution: its start and its result. The
    // start is stored; the result is lost.
    if (member === 'transitions' && method.startsWith('comparePublish') && ++publications > 1)
      return new Promise((resolve, reject) => setTimeout(() => reject(failure('the result was lost')), 1));
    return new Promise((resolve, reject) => setTimeout(() => { try { resolve(call()); } catch (error) { reject(error); } }, 1));
  });
  const end = begin(9, memory, stores);
  try {
    assert.ok(await pump(() => state() !== 0, { rounds: 20000 }), 'the run did not finish: it hung');
    assert.equal(state(), 1, JSON.stringify(report()));
    const found = report();
    assert.ok(publications >= 2, `${publications} publications reached the store: ${JSON.stringify(found)}`);
    assert.notEqual(found.status, 'completed', JSON.stringify(found));
    assert.equal(bridge().waiting, 0);
    console.log(`     (the run ended as ${found.status}${found.message ? `: ${found.message}` : ''})`);
  } finally {
    end();
  }
});

if (failures > 0) {
  console.log(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
