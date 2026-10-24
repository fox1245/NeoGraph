// NeoGraph for a JavaScript host (#535): compile a graph from JSON and run it, with model calls sent through the
// host's `fetch`. The graph engine is the WebAssembly module `neograph_module.mjs` + `neograph_module.wasm` (built by
// CMake with NEOGRAPH_BUILD_WASM_JS=ON); this file is the whole JavaScript API around it. It runs in Node 22+, in a
// browser and in a Worker, on the one thread of the host's event loop: nothing blocks, and a run is a Promise.
//
//   import { load } from './neograph.mjs';
//   const neograph = await load();                       // Workers: load({ wasmModule })
//   const result = await neograph.run({
//     graph: { channels: { messages: { reducer: 'append' } },
//              nodes: { llm: { type: 'llm_call' } },
//              edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }] },
//     provider: { baseUrl: 'https://api.example.com', path: '/v1/chat/completions',
//                 apiKey: env.API_KEY, model: 'a-model' },
//     input: { messages: [{ role: 'user', content: 'Hello' }] },
//     onEvent: (event) => console.log(event),            // optional: streams the run
//   });
//   console.log(result.text);
//
// The API key is a run-time value of `provider`; it is not in the module. Model calls use `globalThis.fetch`.
//
// A Program (a control source that runs graphs, with approvals, forks and a record of every run) has its own surface:
//
//   const programs = neograph.programs({ provider: { baseUrl, path, apiKey, model } });
//   const version = await programs.admit({ javascript: SOURCE });     // compile and admit
//   const run = programs.start(version, { input, onEvent });          // a ProgramRun
//   const paused = await run.result;                                  // status 'interrupted', with paused.interrupt
//   const done = await run.resume({ decision: 'approved' }).result;   // or run.fork(...), run.cancel()
//
// Its stores are in memory unless `stores` gives the five stores of the host (see README.md).

import createModule from './neograph_module.mjs';
import { installStoreBridge, registerStores, releaseStores, memoryStores } from './stores.mjs';

/** A failed run or an invalid graph or provider. */
export class NeoGraphError extends Error {
  /**
   * @param {{type?: string, message?: string, kind?: string, reason?: string, http_status?: number}} detail
   *   `type` is "provider" (the model call failed: `kind`, `reason` and `httpStatus` say how), "cancelled", "error" (an
   *   invalid graph or provider, or a failure in a node), or, for a Program, "compile" or "admission" (the source was
   *   refused: `diagnostics` lists why), "program" (a runtime refusal with its `code`) and "refused" (a call that the host
   *   loop cannot wait for). "cancelled" is also a resume or a fork that was cancelled before it was sent.
   */
  constructor(detail) {
    super(detail.message ?? 'NeoGraph failed');
    this.name = 'NeoGraphError';
    this.type = detail.type ?? 'error';
    if (detail.kind !== undefined) this.kind = detail.kind;
    if (detail.reason) this.reason = detail.reason;
    if (detail.http_status) this.httpStatus = detail.http_status;
    // A Program: its diagnostic code, the list of diagnostics of a compile or admission refusal, and the witness.
    if (detail.code) this.code = detail.code;
    if (detail.diagnostics) this.diagnostics = detail.diagnostics;
    if (detail.witness !== undefined) this.witness = detail.witness;
  }
}

// The SDK descriptor of a provider: the family of the wire protocol, the origin and the routes. A caller can pass
// `provider.descriptor` as the SDK defines it; `family`, `baseUrl` and `path` build the common one.
function descriptorOf(provider) {
  if (provider.descriptor) return provider.descriptor;
  if (!provider.baseUrl || !provider.path) {
    throw new NeoGraphError({ message: 'provider needs a descriptor, or a baseUrl and a path' });
  }
  const paths = provider.paths ?? { buffered: provider.path, streaming: provider.path };
  return {
    descriptor_version: 1,
    revision: 1,
    id: provider.id ?? 'neograph-js',
    family: provider.family ?? 'openai.chat',
    connection: { base_url: provider.baseUrl, paths },
  };
}

function providerJson(provider) {
  const { apiKey, model, instructions, maxOutputTokens, temperature, catalogue } = provider;
  return JSON.stringify({
    descriptor: descriptorOf(provider),
    api_key: apiKey ?? '',
    model: model ?? '',
    ...(catalogue !== undefined && { catalogue }),
    ...(instructions !== undefined && { instructions }),
    ...(maxOutputTokens !== undefined && { max_output_tokens: maxOutputTokens }),
    ...(temperature !== undefined && { temperature }),
  });
}

/**
 * Loads the module. In Node and in a browser it finds `neograph_module.wasm` beside `neograph_module.mjs`. A Worker
 * cannot compile bytes at run time: pass the `WebAssembly.Module` its bundler imports as `wasmModule`.
 *
 * @param {{wasmModule?: WebAssembly.Module, wasmBinary?: ArrayBuffer | Uint8Array, locateFile?: (name: string, dir: string) => string}} [options]
 */
export async function load(options = {}) {
  const pending = new Map(); // run id -> { resolve, reject, onEvent }
  const programCalls = new Map(); // call id -> { resolve, reject, onEvent, failure, stop }
  let lastProgramCall = 0;
  const module = await createModule({
    ...(options.wasmBinary && { wasmBinary: options.wasmBinary }),
    ...(options.locateFile && { locateFile: options.locateFile }),
    ...(options.wasmModule && {
      instantiateWasm(imports, done) {
        WebAssembly.instantiate(options.wasmModule, imports).then((instance) => done(instance, options.wasmModule));
        return {};
      },
    }),
    // Called from inside the engine's turn. A JavaScript exception thrown across it would leave the engine half-way
    // through that turn, so an error from `onEvent` stops the run instead and is the run's rejection.
    ngOnEvent(run, text) {
      const call = pending.get(run);
      if (!call?.onEvent || call.failure) return;
      try {
        call.onEvent(JSON.parse(text));
      } catch (error) {
        call.failure = { error };
        module._ng_cancel(run);
      }
    },
    ngOnDone(run, text) {
      const call = pending.get(run);
      if (!call) return;
      pending.delete(run);
      if (call.failure) return call.reject(call.failure.error);
      const outcome = JSON.parse(text);
      if (outcome.ok) call.resolve(outcome.result);
      else call.reject(new NeoGraphError(outcome.error));
    },
    // The same two, for the calls of a Programs object. An error from `onEvent` cancels the run it watches.
    ngProgramEvent(callId, text) {
      const call = programCalls.get(callId);
      if (!call?.onEvent || call.failure) return;
      try {
        call.onEvent(JSON.parse(text));
      } catch (error) {
        call.failure = { error };
        call.stop?.();
      }
    },
    ngProgramDone(callId, text) {
      const call = programCalls.get(callId);
      if (!call) return;
      programCalls.delete(callId);
      if (call.failure) return call.reject(call.failure.error);
      const outcome = JSON.parse(text);
      if (outcome.ok) call.resolve(outcome.result);
      else call.reject(new NeoGraphError(outcome.error));
    },
  });

  // A module with Program support also has the bridge to the host's stores (wasm/stores.mjs). The registration of a stores
  // object ends when the module has dropped the last store made from it.
  const hasStores = typeof module._ng_store_alloc === 'function';
  if (hasStores) {
    installStoreBridge(module);
    module.ngProgramsStoresFree = (handle) => releaseStores(module, handle);
  }

  // The module reads a string from memory it owns; the string is freed once the call returns.
  const withString = (text, call) => {
    const size = module.lengthBytesUTF8(text) + 1;
    const pointer = module._ng_alloc(size);
    module.stringToUTF8(text, pointer, size);
    try {
      return call(pointer);
    } finally {
      module._ng_free(pointer);
    }
  };
  const lastError = () => module.UTF8ToString(module._ng_error());
  let lastRun = 0;

  /** A compiled graph with its provider. */
  class Graph {
    #handle;
    constructor(handle) {
      this.#handle = handle;
    }

    /**
     * Runs the graph once. Resolves with `{ status, text, output, trace, usage }`: `output` is the final state
     * (`output.channels.<name>.value`), `text` the last assistant message. Rejects with a NeoGraphError.
     *
     * @param {object} input The initial channel writes. `messages` is the conversation, as `{ role, content }`.
     * @param {{onEvent?: (event: {type: string, node: string, data: unknown}) => void, signal?: AbortSignal, timeoutMs?: number}} [run]
     *   `onEvent` streams the run: the model is called in streaming mode and every graph event, with the
     *   `llm_token` events of the reply, arrives in order before the Promise resolves. If it throws, the run is
     *   cancelled and rejects with that error. `signal` aborts the run.
     */
    run(input, run = {}) {
      if (this.#handle === 0) return Promise.reject(new NeoGraphError({ message: 'the graph is released' }));
      const { onEvent, signal, timeoutMs = 0 } = run;
      if (signal?.aborted) return Promise.reject(new NeoGraphError({ type: 'cancelled', message: 'the run was aborted' }));
      const id = ++lastRun;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject, onEvent });
        const started = withString(JSON.stringify(input ?? {}), (text) =>
          module._ng_run(this.#handle, id, text, onEvent ? 1 : 0, timeoutMs));
        if (started !== 0) {
          pending.delete(id);
          reject(new NeoGraphError({ message: lastError() }));
          return;
        }
        signal?.addEventListener('abort', () => module._ng_cancel(id), { once: true });
      });
    }

    /** Frees the graph. A run in flight finishes first. */
    release() {
      if (this.#handle !== 0) module._ng_release(this.#handle);
      this.#handle = 0;
    }
  }

  // ---- Programs ----

  const newRunId = () => {
    const bytes = new Uint8Array(16);
    globalThis.crypto.getRandomValues(bytes);
    return `run-${Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('')}`;
  };

  // Starts one call of a Programs object. The outcome arrives through ngProgramDone, in a later turn of the event loop.
  // `controls.onEvent` watches the run's events; `controls.stop` cancels the run when it throws.
  function invoke(host, start, controls = {}) {
    if (host.released) return Promise.reject(new NeoGraphError({ message: 'the Programs object is released' }));
    const callId = ++lastProgramCall;
    return new Promise((resolve, reject) => {
      programCalls.set(callId, { resolve, reject, onEvent: controls.onEvent, stop: controls.stop });
      let failed = null;
      try {
        if (start(callId) !== 0) failed = new NeoGraphError({ message: lastError() });
      } catch (error) {
        // The call could not even be written (a value that is not JSON, for one).
        failed = error instanceof NeoGraphError ? error : new NeoGraphError({ message: String(error?.message ?? error) });
      }
      if (failed) {
        programCalls.delete(callId);
        reject(failed);
      }
    });
  }

  const sendJson = (host, fn, payload, controls) =>
    invoke(host, (callId) => withString(JSON.stringify(payload), (text) => fn(host.handle, callId, text)), controls);

  const withdrawn = (what) => new NeoGraphError({ type: 'cancelled', message: `the ${what} was cancelled before it was sent` });

  /**
   * One run of a Program, from its start to the point where it ends or waits for an answer. A run that ends in an
   * interrupt is resumed or forked, which gives another ProgramRun.
   */
  class ProgramRun {
    #host;
    #id;
    #versionId;
    #result;
    #sent = false;
    #withdrawn = false;
    #settled = false;

    /**
     * @param {object} host
     * @param {string} id
     * @param {string} versionId
     * @param {(send: (fn: Function, payload: object) => Promise<object>) => Promise<object>} plan makes the call, through
     *   `send`, when its turn comes (a resume or a fork waits for the result of the run it continues)
     * @param {{onEvent?: Function, signal?: AbortSignal, what?: string}} options
     */
    constructor(host, id, versionId, plan, { onEvent, signal, what = 'run' } = {}) {
      this.#host = host;
      this.#id = id;
      this.#versionId = versionId;
      const send = (fn, payload) => {
        if (this.#withdrawn) return Promise.reject(withdrawn(what));
        this.#sent = true;
        return sendJson(host, fn, { ...payload, stream: !!onEvent },
          { onEvent, stop: () => this.cancel().catch(() => {}) });
      };
      if (signal?.aborted) this.#withdrawn = true;
      this.#result = plan(send);
      // A call that fails is reported by `result` to whoever reads it. A host that watches the run through `onEvent` and
      // never reads it must not crash on an unhandled rejection.
      this.#result.catch(() => {});
      const settle = () => { this.#settled = true; };
      this.#result.then(settle, settle);
      if (signal && !signal.aborted) {
        const abort = () => this.cancel().catch(() => {});
        signal.addEventListener('abort', abort, { once: true });
        const settled = () => signal.removeEventListener('abort', abort);
        this.#result.then(settled, settled);
      }
    }

    /** The run's identity. A resumed run keeps it; a fork has its own. */
    get id() {
      return this.#id;
    }

    /** The version of the Program this run executes. */
    get versionId() {
      return this.#versionId;
    }

    /**
     * Resolves when the run ends or waits for an answer: `{ status, output, interrupt?, failure?, usage, ... }`. A run that
     * fails or is cancelled still resolves, with that status; it rejects only when the call itself could not be made.
     */
    get result() {
      return this.#result;
    }

    #interrupted(result) {
      if (result.status !== 'interrupted' || !result.interrupt?.pendingId || !result.checkpointId) {
        throw new NeoGraphError({ message: `the run ${this.#id} is not waiting for an answer` });
      }
      return result;
    }

    /**
     * Answers the interrupt the run is waiting at and continues it. The new ProgramRun has the same id.
     * @param {unknown} value what the interrupted node receives
     * @param {{onEvent?: (event: object) => void, signal?: AbortSignal}} [options]
     */
    resume(value, options = {}) {
      const previous = this.#result;
      const id = this.#id;
      return new ProgramRun(this.#host, id, this.#versionId, (send) => previous.then((paused) => {
        const waiting = this.#interrupted(paused);
        return send(module._ng_programs_resume, { runId: id, pendingId: waiting.interrupt.pendingId, value: value ?? null });
      }), { onEvent: options.onEvent, signal: options.signal, what: 'resume' });
    }

    /**
     * Forks the run at the checkpoint it is waiting at, answering the interrupt in the fork. The paused run is not
     * touched and can still be resumed. The fork takes half of what the paused run has left of its budget, or what
     * `options.budget` asks for, which must leave the paused run some wall time and Core steps. Forks of one pause may be made
     * together: each reads what is left after the forks before it. A fork made again under the same `runId` is that fork.
     * @param {unknown} value what the interrupted node receives in the fork
     * @param {{input?: object, budget?: object, runId?: string, onEvent?: (event: object) => void, signal?: AbortSignal}} [options]
     */
    fork(value, options = {}) {
      const { input, budget, runId = newRunId() } = options;
      const previous = this.#result;
      const source = this.#id;
      const versionId = this.#versionId;
      return new ProgramRun(this.#host, runId, versionId, (send) => previous.then((paused) => {
        const waiting = this.#interrupted(paused);
        return send(module._ng_programs_fork, {
          runId: source, newRunId: runId, version: versionId, checkpointId: waiting.checkpointId,
          pendingId: waiting.interrupt.pendingId, value: value ?? null,
          ...(input !== undefined && { input }), ...(budget !== undefined && { budget }),
        });
      }), { onEvent: options.onEvent, signal: options.signal, what: 'fork' });
    }

    /**
     * Cancels the run. Resolves with whether this call cancelled it (false when it had already ended). A resume or a fork
     * that has not been sent yet (it waits for the run it continues) is withdrawn instead, and its `result` rejects.
     */
    cancel() {
      if (!this.#sent) {
        const first = !this.#withdrawn && !this.#settled;
        this.#withdrawn = true;
        return Promise.resolve(first);
      }
      return invoke(this.#host, (callId) =>
        withString(this.#id, (text) => module._ng_programs_cancel(this.#host.handle, callId, text)));
    }
  }

  /** The Programs of one provider and one set of stores: compile and admit sources, start runs, read their results. */
  class Programs {
    #host;
    constructor(host) {
      this.#host = host;
    }

    /**
     * Compiles a Program from its source and admits it. Resolves with `{ id, bundleId, name }`. Rejects with a
     * NeoGraphError of type "compile" or "admission", whose `diagnostics` say why the source was refused.
     *
     * @param {string | {javascript: string, name?: string} | {document: object, name?: string}} source
     *   a string is the text of a JavaScript control source (`define()` builds the graph, `main(input)` yields commands)
     */
    admit(source) {
      const body = typeof source === 'string' ? { javascript: source } : source;
      return sendJson(this.#host, module._ng_programs_admit, body);
    }

    /**
     * Starts a run of an admitted version. Returns at once; `run.result` resolves when it ends or waits for an answer.
     *
     * @param {string | {id: string}} version
     * @param {{input?: object, budget?: object, runId?: string, onEvent?: (event: object) => void, signal?: AbortSignal}} [options]
     *   `budget` overrides members of the default run budget (`wallTimeMs`, `modelTokens`, `maxProgramOperations`, ...).
     *   `onEvent` receives the run's events in order; if it throws, the run is cancelled and its result rejects with
     *   that error. `signal` cancels the run; one that is aborted already means the run is not started.
     * @returns {ProgramRun}
     */
    start(version, options = {}) {
      const { input, budget, runId = newRunId() } = options;
      const versionId = typeof version === 'string' ? version : version?.id;
      if (typeof versionId !== 'string' || versionId === '') {
        throw new NeoGraphError({ message: 'start() needs a version: what admit() resolved with, or its id' });
      }
      return new ProgramRun(this.#host, runId, versionId, (send) => send(module._ng_programs_start, {
        version: versionId, runId, input: input ?? {}, ...(budget !== undefined && { budget }),
      }), { onEvent: options.onEvent, signal: options.signal });
    }

    /** Starts a run and resolves with its result: `start(version, options).result`. */
    run(version, options) {
      return this.start(version, options).result;
    }

    /**
     * What the stores hold of a run: its latest result (the pause of a run that waits is one), `{ runId, status:
     * 'running' }` while it has none, or `null` when the stores know no such run. It reads the transition store, so it
     * also answers for a run that another Programs object started. The budget in it is the one at the time of the result.
     */
    result(runId) {
      return invoke(this.#host, (callId) =>
        withString(runId, (text) => module._ng_programs_result(this.#host.handle, callId, text)));
    }

    /**
     * Cancels the runs that are executing and frees the object. Calls already made finish first, and a run that waits at an
     * interrupt stays as it is in the stores. A store call that never settles leaves the call that waits for it pending.
     */
    release() {
      if (this.#host.released) return;
      this.#host.released = true;
      module._ng_programs_release(this.#host.handle);
    }
  }

  // The members of the host's stores object, and the methods of the transition store that a run needs besides the ones
  // the C++ interface requires: without them a run, a fork or a resume fails.
  const STORE_MEMBERS = ['transitions', 'programs', 'grants', 'fragments', 'modules'];
  const REQUIRED_READERS = ['loadJavascriptCommands', 'loadMigrationPlan', 'loadRunLineage', 'loadGeneration'];

  return {
    /**
     * Programs over a provider and a set of stores.
     *
     * @param {{provider: object, owner?: string, stores?: object, limits?: object, budget?: object,
     *   javascript?: {stackBytes?: number, defineTimeMs?: number}}} config
     *   `provider` is as for `compile`. `stores` are the host's stores (any of `transitions`, `programs`, `grants`,
     *   `fragments`, `modules`); a store left out is in memory. `limits` is the budget ceiling of the admitted Programs and
     *   `budget` the default budget of a run (`wallTimeMs`, `modelTokens`, `maxConcurrency`, `maxProgramOperations`, ...).
     *   `javascript` sets the limits of the control source: `stackBytes`, the stack that `define()` and `main()` may use
     *   (49152, 48 KiB, by default; a whole number from 16384 up to what the module's own stack holds, which the error of
     *   a value above it states), and `defineTimeMs`, the time `define()` may run when a source is admitted (5000 by
     *   default, up to 3600000). The host's own call stack is what a larger `stackBytes` has to fit in, and the module
     *   accepts values far above what it carries: a source that overflows it ends in a trap that the module cannot
     *   recover from, and on Node's default stack 160 KiB is enough for that. Keep `stackBytes` within an eighth of the
     *   host's call stack, and see README.md before raising it.
     * @returns {Programs}
     */
    programs(config) {
      if (typeof module._ng_programs_create !== 'function') {
        throw new NeoGraphError({ message: 'this build of the module has no Program support' });
      }
      const { provider, stores, owner, limits, budget, javascript } = config ?? {};
      if (!provider) throw new NeoGraphError({ message: 'programs() needs a provider' });
      let text;
      try {
        text = JSON.stringify({
          provider: JSON.parse(providerJson(provider)),
          ...(owner !== undefined && { owner }), ...(limits !== undefined && { limits }),
          ...(budget !== undefined && { budget }), ...(javascript !== undefined && { javascript }),
        });
      } catch (error) {
        throw error instanceof NeoGraphError ? error : new NeoGraphError({ message: String(error?.message ?? error) });
      }
      let storesHandle = 0;
      if (stores) {
        if (!hasStores) throw new NeoGraphError({ message: 'this build of the module has no support for stores of the host' });
        if (typeof stores !== 'object') throw new NeoGraphError({ message: 'stores must be an object' });
        for (const name of Object.keys(stores)) {
          if (!STORE_MEMBERS.includes(name)) {
            throw new NeoGraphError({ message: `stores.${name} is not a store: the members are ${STORE_MEMBERS.join(', ')}` });
          }
        }
        for (const method of stores.transitions ? REQUIRED_READERS : []) {
          if (typeof stores.transitions[method] !== 'function') {
            throw new NeoGraphError({ message: `stores.transitions.${method} is required by Programs` });
          }
        }
        try {
          storesHandle = registerStores(module, stores);
        } catch (error) {
          throw new NeoGraphError({ message: error.message });
        }
      }
      const host = { handle: withString(text, (pointer) => module._ng_programs_create(pointer, storesHandle)), released: false };
      if (host.handle === 0) {
        // The stores were registered for this object alone. When the module got as far as making stores from them it has
        // already ended the registration, and ending it again does nothing.
        if (storesHandle !== 0) releaseStores(module, storesHandle);
        throw new NeoGraphError({ message: lastError() });
      }
      return new Programs(host);
    },

    /**
     * The reference store: stores that implement the contract of `programs({ stores })` over the module's in-memory
     * stores, to use as they are or to wrap in a store that persists (see wasm/README.md). Its methods return values, not
     * Promises. `release()` on the result drops its set; do that when the runs on it have ended (their `result` has
     * settled), because a call on a dropped set fails.
     */
    memoryStores() {
      if (!hasStores) throw new NeoGraphError({ message: 'this build of the module has no support for stores of the host' });
      return memoryStores(module);
    },

    /**
     * Compiles a graph definition (JSON text or an object) for a provider. Throws a NeoGraphError.
     *
     * @param {object | string} graph
     * @param {{baseUrl?: string, path?: string, family?: string, descriptor?: object, apiKey?: string, model: string, instructions?: string, maxOutputTokens?: number, temperature?: number}} provider
     */
    compile(graph, provider) {
      const text = typeof graph === 'string' ? graph : JSON.stringify(graph);
      const handle = withString(text, (graphText) => withString(providerJson(provider), (providerText) =>
        module._ng_compile(graphText, providerText)));
      if (handle === 0) throw new NeoGraphError({ message: lastError() });
      return new Graph(handle);
    },

    /** Compiles, runs once and releases. `{ graph, provider, input, ...run }`; see `compile` and `Graph.run`. */
    async run({ graph, provider, input, ...run }) {
      const compiled = this.compile(graph, provider);
      try {
        return await compiled.run(input, run);
      } finally {
        compiled.release();
      }
    },
  };
}
