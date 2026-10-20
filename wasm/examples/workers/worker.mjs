// A Cloudflare Workers-style host (#535): a request handler that runs a NeoGraph graph with one llm_call node and
// answers with the model's reply, as JSON or as server-sent events.
//
//   POST /      {"message": "..."}                  -> {"text": "...", "usage": {...}}
//   POST /      {"message": "...", "stream": true}  -> text/event-stream: llm_token events, then a "done" event
//
// The model call goes through the Worker's fetch. TLS is the platform's, so nothing here needs OpenSSL, and the
// module runs on the one thread of the request. The settings come from the Worker's bindings (see wrangler.toml);
// the API key is a secret binding, never a value in the code or the module.
//
// Build the module with NEOGRAPH_BUILD_WASM_JS=ON (wasm/README.md) and copy neograph.mjs, neograph_module.mjs and
// neograph_module.wasm from its wasm/ directory next to this file.
import wasmModule from './neograph_module.wasm';
import { load, NeoGraphError } from './neograph.mjs';

const GRAPH = {
  channels: { messages: { reducer: 'append' } },
  nodes: { llm: { type: 'llm_call' } },
  edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
};

// One instance of the module for the life of the isolate. A Worker compiles no bytes at run time: the bundler hands
// over the WebAssembly.Module it imported.
let neograph;
const instance = () => (neograph ??= load({ wasmModule }));

const providerOf = (env) => ({
  baseUrl: env.BASE_URL,
  path: env.API_PATH,
  apiKey: env.API_KEY,
  model: env.MODEL,
});

const sse = (name, data) => `event: ${name}\ndata: ${JSON.stringify(data)}\n\n`;

export default {
  async fetch(request, env, ctx) {
    if (request.method !== 'POST') return new Response('POST {"message": "..."}\n', { status: 405 });
    let body;
    try {
      body = await request.json();
    } catch {
      return Response.json({ error: 'the body is not JSON' }, { status: 400 });
    }
    if (typeof body?.message !== 'string') return Response.json({ error: 'message is required' }, { status: 400 });

    // A client that goes away takes the model call with it.
    const run = {
      graph: GRAPH,
      provider: providerOf(env),
      input: { messages: [{ role: 'user', content: body.message }] },
      signal: request.signal,
    };
    const engine = await instance();

    if (!body.stream) {
      try {
        const result = await engine.run(run);
        return Response.json({ text: result.text, usage: result.usage });
      } catch (error) {
        if (!(error instanceof NeoGraphError)) throw error;
        return Response.json({ error: error.message, type: error.type, kind: error.kind }, { status: 502 });
      }
    }

    // The reply is sent as the model produces it. The run goes on after the handler returns the Response, so it is
    // handed to waitUntil.
    const { readable, writable } = new TransformStream();
    const writer = writable.getWriter();
    const encoder = new TextEncoder();
    const send = (name, data) => writer.write(encoder.encode(sse(name, data)));
    ctx.waitUntil((async () => {
      try {
        const result = await engine.run({
          ...run,
          onEvent: (event) => { if (event.type === 'llm_token') send('token', event.data).catch(() => {}); },
        });
        await send('done', { text: result.text, usage: result.usage });
      } catch (error) {
        await send('error', { message: error.message, type: error.type, kind: error.kind });
      } finally {
        await writer.close();
      }
    })());
    return new Response(readable, { headers: { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' } });
  },
};
