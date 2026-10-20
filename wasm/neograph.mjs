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

import createModule from './neograph_module.mjs';

/** A failed run or an invalid graph or provider. */
export class NeoGraphError extends Error {
  /**
   * @param {{type?: string, message?: string, kind?: string, reason?: string, http_status?: number}} detail
   *   `type` is "provider" (the model call failed: `kind`, `reason` and `httpStatus` say how), "cancelled", or
   *   "error" (an invalid graph or provider, or a failure in a node).
   */
  constructor(detail) {
    super(detail.message ?? 'NeoGraph failed');
    this.name = 'NeoGraphError';
    this.type = detail.type ?? 'error';
    if (detail.kind !== undefined) this.kind = detail.kind;
    if (detail.reason) this.reason = detail.reason;
    if (detail.http_status) this.httpStatus = detail.http_status;
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
  const { apiKey, model, instructions, maxOutputTokens, temperature } = provider;
  return JSON.stringify({
    descriptor: descriptorOf(provider),
    api_key: apiKey ?? '',
    model: model ?? '',
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
  });

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

  return {
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
