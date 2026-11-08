// The JavaScript half of the Program stores of a JavaScript host (wasm/js_stores.h, wasm/README.md).
//
//   import { installStoreBridge, registerStores, releaseStores, memoryStores } from './stores.mjs';
//   installStoreBridge(module);                          // once per module instance
//   const handle = registerStores(module, { transitions, programs });   // the host's stores; pass the handle to the module
//   releaseStores(module, handle);
//
// `memoryStores(module)` is the reference store: objects that implement the contract by calling the in-memory C++
// stores in the module. They return values, not Promises. A host can wrap them (delegate every call, write down the
// arguments of the calls that change something, replay them when it starts) to get a durable store without writing
// the stores' rules again.

const MEMBERS = ['transitions', 'programs', 'grants', 'fragments', 'modules'];
const ALL_MEMBERS = [...MEMBERS, 'checkpoints'];

// The C++ exception classes that an Error can name in `kind`. Anything else is a std::runtime_error.
const KINDS = new Set(['invalid_argument', 'out_of_range', 'domain_error', 'length_error', 'logic_error', 'runtime_error']);

function invalidArgument(message) {
  const err = new Error(message);
  err.kind = 'invalid_argument';
  return err;
}

export function normalizeFence(value) {
  if (typeof value === 'bigint') {
    if (value < 1n) throw invalidArgument('CheckpointWriteFence requires fence >= 1');
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isSafeInteger(value) || value < 1) {
      throw invalidArgument('CheckpointWriteFence requires a safe integer fence >= 1 or BigInt');
    }
    return BigInt(value);
  }
  if (typeof value === 'string') {
    if (value.length === 0) throw invalidArgument('CheckpointWriteFence requires a non-empty fence string');
    for (let i = 0; i < value.length; i += 1) {
      const code = value.charCodeAt(i);
      if (code < 48 || code > 57) throw invalidArgument('CheckpointWriteFence requires a decimal integer fence');
    }
    const parsed = BigInt(value);
    if (parsed < 1n) throw invalidArgument('CheckpointWriteFence requires fence >= 1');
    return parsed;
  }
  throw invalidArgument('CheckpointWriteFence requires a BigInt, safe integer, or decimal string fence');
}

const WRITE_FENCE_ERROR_CODE = 'G_CHECKPOINT_WRITE_FENCE';

function makeFenceRefusedError(ownerScope, runId, writerFence, storedFence) {
  const wf = writerFence.toString();
  const sf = storedFence.toString();
  const err = new Error(
    `Checkpoint write refused for run '${runId}' in scope '${ownerScope}': writer_fence=${wf} < stored_fence=${sf}`);
  err.code = WRITE_FENCE_ERROR_CODE;
  err.kind = WRITE_FENCE_ERROR_CODE;
  err.ownerScope = ownerScope;
  err.runId = runId;
  err.storedFence = sf;
  err.writerFence = wf;
  return err;
}

export class MemoryCheckpointStore {
  #fences = new Map();
  #threads = new Map();
  #byId = new Map();
  #pendingWrites = new Map();
  #budgetEffects = new Map();

  constructor() {
    Object.defineProperty(this, 'supportsWriteFence', { value: true, enumerable: false });
    const methods = [
      'raiseFence', 'loadFence', 'save', 'loadLatest', 'loadById', 'list',
      'deleteThread', 'putWrites', 'getWrites', 'clearWrites',
      'saveWithManagedBudgetEffect', 'seedForkCheckpoint',
    ];
    for (const name of methods) {
      this[name] = this[name].bind(this);
    }
  }

  get supportsWriteFence() {
    return true;
  }

  #checkAndUpsertFence(fence) {
    if (fence === null || typeof fence !== 'object') {
      throw invalidArgument('CheckpointWriteFence must be an object');
    }
    const ownerScope = fence.ownerScope ?? fence.owner_scope ?? '';
    const runId = fence.runId ?? fence.run_id ?? '';
    const rawFence = fence.fence;
    if (typeof ownerScope !== 'string' || typeof runId !== 'string') {
      throw invalidArgument('CheckpointWriteFence requires string owner_scope and run_id');
    }
    const hasFenceValue =
      rawFence !== undefined && rawFence !== null &&
      rawFence !== 0 && rawFence !== 0n && rawFence !== '0' && rawFence !== '';
    if (!hasFenceValue && runId.length === 0 && ownerScope.length === 0) {
      return 0n;
    }
    if (runId.length === 0 || !hasFenceValue) {
      throw invalidArgument('CheckpointWriteFence must either be empty or specify both run_id and positive fence');
    }
    const wf = normalizeFence(rawFence);
    const key = `${ownerScope}\0${runId}`;
    const stored = this.#fences.get(key) ?? 0n;
    if (wf < stored) {
      throw makeFenceRefusedError(ownerScope, runId, wf, stored);
    }
    if (wf > stored) {
      this.#fences.set(key, wf);
    }
    return wf;
  }

  raiseFence(ownerScope, runId, newFence) {
    if (typeof ownerScope !== 'string' || typeof runId !== 'string' || runId.length === 0) {
      throw invalidArgument('CheckpointStore.raiseFence requires a non-empty runId');
    }
    const target = normalizeFence(newFence);
    const key = `${ownerScope}\0${runId}`;
    const stored = this.#fences.get(key) ?? 0n;
    if (target > stored) {
      this.#fences.set(key, target);
      return target;
    }
    return stored;
  }

  loadFence(ownerScope, runId) {
    if (typeof ownerScope !== 'string' || typeof runId !== 'string' || runId.length === 0) {
      return 0n;
    }
    return this.#fences.get(`${ownerScope}\0${runId}`) ?? 0n;
  }

  #saveUnchecked(checkpoint) {
    if (!checkpoint || typeof checkpoint !== 'object') {
      throw invalidArgument('Checkpoint must be an object');
    }
    const threadId = checkpoint.thread_id ?? checkpoint.threadId ?? '';
    const id = checkpoint.id ?? '';
    const copy = structuredClone(checkpoint);
    this.#byId.set(id, copy);
    const list = this.#threads.get(threadId) ?? [];
    const existingIdx = list.findIndex((item) => (item.id ?? '') === id);
    if (existingIdx !== -1) list.splice(existingIdx, 1);
    list.unshift(copy);
    this.#threads.set(threadId, list);
  }

  save(checkpoint, fence) {
    if (fence !== undefined && fence !== null) {
      this.#checkAndUpsertFence(fence);
    }
    this.#saveUnchecked(checkpoint);
  }

  loadLatest(threadId) {
    const list = this.#threads.get(threadId);
    if (!list || list.length === 0) return null;
    return structuredClone(list[0]);
  }

  loadById(id) {
    const found = this.#byId.get(id);
    return found ? structuredClone(found) : null;
  }

  list(threadId, limit = 100) {
    if (limit <= 0) return [];
    const list = this.#threads.get(threadId);
    if (!list || list.length === 0) return [];
    return list.slice(0, limit).map((item) => structuredClone(item));
  }

  deleteThread(threadId, fence) {
    if (fence !== undefined && fence !== null) {
      this.#checkAndUpsertFence(fence);
    }
    const list = this.#threads.get(threadId) ?? [];
    for (const cp of list) {
      this.#byId.delete(cp.id ?? '');
    }
    this.#threads.delete(threadId);
    const prefix = `${threadId}\0`;
    for (const key of [...this.#pendingWrites.keys()]) {
      if (key.startsWith(prefix)) this.#pendingWrites.delete(key);
    }
  }

  putWrites(threadId, parentCheckpointId, write, fence) {
    if (fence !== undefined && fence !== null) {
      this.#checkAndUpsertFence(fence);
    }
    const key = `${threadId}\0${parentCheckpointId}`;
    const list = this.#pendingWrites.get(key) ?? [];
    list.push(structuredClone(write));
    this.#pendingWrites.set(key, list);
  }

  getWrites(threadId, parentCheckpointId) {
    const key = `${threadId}\0${parentCheckpointId}`;
    const list = this.#pendingWrites.get(key);
    if (!list || list.length === 0) return [];
    return list.map((item) => structuredClone(item));
  }

  clearWrites(threadId, parentCheckpointId, fence) {
    if (fence !== undefined && fence !== null) {
      this.#checkAndUpsertFence(fence);
    }
    this.#pendingWrites.delete(`${threadId}\0${parentCheckpointId}`);
  }

  saveWithManagedBudgetEffect(checkpoint, effect, fence) {
    if (fence !== undefined && fence !== null) {
      this.#checkAndUpsertFence(fence);
    }
    this.#saveUnchecked(checkpoint);
    if (effect && typeof effect === 'object') {
      const effectId = effect.effect_id ?? effect.effectId ?? checkpoint?.id ?? '';
      this.#budgetEffects.set(effectId, structuredClone(effect));
    }
  }

  seedForkCheckpoint(req, fence) {
    if (fence !== undefined && fence !== null) {
      this.#checkAndUpsertFence(fence);
    }
    const forkCheckpoint = req?.forkCheckpoint ?? req?.fork_checkpoint ?? req?.checkpoint ?? req;
    this.#saveUnchecked(forkCheckpoint);
  }
}

function stateOf(module) {
  const state = module.ngStoreState;
  if (!state) throw new Error('installStoreBridge(module) has not been called');
  return state;
}

// Runs `use` with a NUL-terminated copy of `text` in the module's memory.
function withCString(module, text, use) {
  const size = module.lengthBytesUTF8(text) + 1;
  const pointer = module._ng_store_alloc(size);
  module.stringToUTF8(text, pointer, size);
  try {
    return use(pointer);
  } finally {
    module._ng_store_free(pointer);
  }
}

function isThenable(value) {
  return value !== null && (typeof value === 'object' || typeof value === 'function') && typeof value.then === 'function';
}

// What a failed method says: its message, and the C++ exception class it asks for.
function describeFailure(error) {
  let message;
  try {
    message = error instanceof Error ? error.message : String(error);
  } catch {
    message = 'the store failed';
  }
  try {
    if (error?.code === WRITE_FENCE_ERROR_CODE || error?.kind === WRITE_FENCE_ERROR_CODE) {
      return {
        kind: WRITE_FENCE_ERROR_CODE,
        message: JSON.stringify({
          message,
          owner_scope: String(error.ownerScope ?? error.owner_scope ?? ''),
          run_id: String(error.runId ?? error.run_id ?? ''),
          stored_fence: String(error.storedFence ?? error.stored_fence ?? ''),
          writer_fence: String(error.writerFence ?? error.writer_fence ?? ''),
        }),
      };
    }
  } catch {
    // Fall through to standard failure handling.
  }
  let kind = '';
  try {
    if (typeof error?.kind === 'string' && KINDS.has(error.kind)) kind = error.kind;
  } catch {
    // A getter that throws names no class.
  }
  return { kind, message };
}

// The JSON text of a method's result. An absent value is null.
function encodeResult(value) {
  const normalized = value === undefined ? null : value;
  const text = JSON.stringify(normalized, (_k, v) => (typeof v === 'bigint' ? v.toString() : v));
  if (text === undefined) throw new TypeError('the result is not JSON');
  return text;
}

/**
 * Installs the functions that the module calls into (Module.ngStoreHas, Module.ngStoreCall) and the registry of the
 * stores objects. Call it once per module instance, before the first adapt_js_stores.
 */
export function installStoreBridge(module) {
  if (module.ngStoreState) return;
  const state = { stores: new Map(), last: 0 };
  module.ngStoreState = state;
  module.ngStoreReply = '';

  // 1: the object has the member (or method) but it is the wrong kind of thing; 2: a function; 3: supportsWriteFence === true;
  // 0: absent; -1: the member is not an object; -2: no such handle.
  module.ngStoreHas = (handle, member, method) => {
    try {
      const stores = state.stores.get(handle);
      if (stores === undefined) return -2;
      if (member === 'checkpoints' && !Object.prototype.propertyIsEnumerable.call(stores, 'checkpoints')) {
        return 0;
      }
      const target = stores[member];
      if (target === undefined || target === null) return 0;
      if (typeof target !== 'object') return -1;
      if (method === null) return 1;
      if (method === 'supportsWriteFence') {
        return target.supportsWriteFence === true ? 3 : 0;
      }
      const found = target[method];
      if (found === undefined) return 0;
      return typeof found === 'function' ? 2 : 1;
    } catch {
      return -1;
    }
  };

  const settle = (call, value) => {
    let text;
    try {
      text = encodeResult(value);
    } catch (error) {
      fail(call, error);
      return;
    }
    withCString(module, text, (pointer) => module._ng_store_resolve(call, pointer));
  };
  const fail = (call, error) => {
    const { kind, message } = describeFailure(error);
    withCString(module, message, (messagePointer) =>
      withCString(module, kind, (kindPointer) => module._ng_store_reject(call, messagePointer, kindPointer)));
  };

  // 0: the method returned a value (the reply is its JSON text); 1: it failed (the reply is {kind, message}); 2: it
  // returned a Promise, whose outcome goes back through _ng_store_resolve / _ng_store_reject. Nothing is thrown: a
  // JavaScript exception must not cross into the module.
  module.ngStoreCall = (handle, member, method, argumentsText, call) => {
    let result;
    try {
      const target = state.stores.get(handle)?.[member];
      if (!target) throw new Error(`stores.${member} is not available`);
      const found = target[method];
      if (typeof found !== 'function') throw new TypeError(`stores.${member}.${method} is not a function`);
      result = found.apply(target, JSON.parse(argumentsText));
      if (!isThenable(result)) {
        module.ngStoreReply = encodeResult(result);
        return 0;
      }
    } catch (error) {
      module.ngStoreReply = JSON.stringify(describeFailure(error));
      return 1;
    }
    // Always from a later task, even for a thenable that settles at once.
    Promise.resolve(result).then((value) => settle(call, value), (error) => fail(call, error));
    return 2;
  };
}

/**
 * Registers a stores object, `{ transitions, programs, grants, fragments, modules }` (each member optional), and
 * returns the handle that names it to the module (`adapt_js_stores(handle)`).
 */
export function registerStores(module, stores) {
  const state = stateOf(module);
  if (stores === null || typeof stores !== 'object') throw new TypeError('the stores must be an object');
  for (const member of ALL_MEMBERS) {
    const target = stores[member];
    if (target !== undefined && target !== null && typeof target !== 'object')
      throw new TypeError(`stores.${member} must be an object`);
  }
  const handle = ++state.last;
  state.stores.set(handle, stores);
  return handle;
}

/**
 * Registers a synthesis validator `validate(proposal, bundle, reservation)` under the store bridge and returns its
 * handle (`adapt_js_synthesis_validator(handle)`).
 */
export function registerSynthesisValidator(module, validate) {
  const state = stateOf(module);
  if (typeof validate !== 'function') throw new TypeError('the synthesis validator must be a function');
  const handle = ++state.last;
  state.stores.set(handle, { synthesis: { validate } });
  return handle;
}

/** Forgets a stores object. A call that is still pending completes; a call made afterwards fails. */
export function releaseStores(module, handle) {
  stateOf(module).stores.delete(handle);
}

function memoryError(detail) {
  const { kind, message } = detail;
  if (kind === WRITE_FENCE_ERROR_CODE || detail.code === WRITE_FENCE_ERROR_CODE) {
    const runId = typeof detail.run_id === 'string'
      ? detail.run_id
      : (typeof detail.runId === 'string' ? detail.runId : '');
    const storedRaw = detail.stored_fence ?? detail.storedFence;
    const writerRaw = detail.writer_fence ?? detail.writerFence;
    let storedFence = 0n;
    let writerFence = 0n;
    try {
      if (storedRaw !== undefined && storedRaw !== null && storedRaw !== '') {
        storedFence = normalizeFence(storedRaw, 'stored_fence');
      }
      if (writerRaw !== undefined && writerRaw !== null && writerRaw !== '') {
        writerFence = normalizeFence(writerRaw, 'writer_fence');
      }
    } catch {
      storedFence = 0n;
      writerFence = 0n;
    }
    if (!runId || storedFence <= 0n || writerFence <= 0n) {
      const protocolError = new Error(
        message ? `Malformed checkpoint write fence refusal: ${message}` : 'Malformed checkpoint write fence refusal'
      );
      protocolError.kind = 'runtime_error';
      return protocolError;
    }
    const error = new Error(message);
    error.code = WRITE_FENCE_ERROR_CODE;
    error.kind = WRITE_FENCE_ERROR_CODE;
    error.ownerScope = String(detail.owner_scope ?? detail.ownerScope ?? '');
    error.runId = runId;
    error.storedFence = storedFence.toString();
    error.writerFence = writerFence.toString();
    return error;
  }
  const error = new Error(message);
  if (KINDS.has(kind)) error.kind = kind;
  return error;
}

/**
 * The reference store: the in-memory C++ stores of the module, as objects that implement the contract. Every method
 * returns its value directly. `options.set` names a set that already exists (as the module's tests do); otherwise a new,
 * empty one is made. `stores.release()` drops the set.
 */
export function memoryStores(module, options = {}) {
  const set = options.set ?? module._ng_memory_create();
  const call = (member, method, args) =>
    withCString(module, member, (memberPointer) =>
      withCString(module, method, (methodPointer) =>
        withCString(module, JSON.stringify(args, (_k, v) => (typeof v === 'bigint' ? v.toString() : v)), (argsPointer) => {
          const status = module._ng_memory_call(set, memberPointer, methodPointer, argsPointer);
          const text = module.UTF8ToString(module._ng_memory_reply());
          if (status !== 0) throw memoryError(JSON.parse(text));
          return JSON.parse(text);
        })));
  const stores = {};
  for (const member of MEMBERS) {
    const methods = withCString(module, member, (pointer) => {
      module._ng_memory_methods(pointer);
      return JSON.parse(module.UTF8ToString(module._ng_memory_reply()));
    });
    const target = {};
    for (const method of methods) target[method] = (...args) => call(member, method, args);
    stores[member] = target;
  }
  Object.defineProperty(stores, 'checkpoints', { value: new MemoryCheckpointStore() });
  Object.defineProperty(stores, 'set', { value: set });
  Object.defineProperty(stores, 'release', { value: () => module._ng_memory_release(set) });
  return stores;
}
