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

// The C++ exception classes that an Error can name in `kind`. Anything else is a std::runtime_error.
const KINDS = new Set(['invalid_argument', 'out_of_range', 'domain_error', 'length_error', 'logic_error', 'runtime_error']);

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
  const text = JSON.stringify(value === undefined ? null : value);
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

  // 1: the object has the member (or method) but it is the wrong kind of thing; 2: a function; 0: absent; -1: the
  // member is not an object; -2: no such handle.
  module.ngStoreHas = (handle, member, method) => {
    try {
      const stores = state.stores.get(handle);
      if (stores === undefined) return -2;
      const target = stores[member];
      if (target === undefined || target === null) return 0;
      if (typeof target !== 'object') return -1;
      if (method === null) return 1;
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
  for (const member of MEMBERS) {
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

function memoryError({ kind, message }) {
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
        withCString(module, JSON.stringify(args), (argsPointer) => {
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
  Object.defineProperty(stores, 'set', { value: set });
  Object.defineProperty(stores, 'release', { value: () => module._ng_memory_release(set) });
  return stores;
}
