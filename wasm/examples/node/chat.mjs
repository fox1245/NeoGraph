// One model call through a NeoGraph graph, from Node, over the host's fetch.
//
//   API_KEY=<key> node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]
//   API_KEY=<key> node wasm/examples/node/chat.mjs https://api.example.com /v1/chat/completions <model> "Say hello" --stream
//
// The base url is the origin of an OpenAI-compatible Chat Completions endpoint, and the path is its route. The module
// is the build's wasm/ directory (neograph.mjs, neograph_module.mjs, neograph_module.wasm); NEOGRAPH_WASM_DIR names
// another one. The key comes from the environment of this process and goes nowhere else.
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const directory = path.resolve(process.env.NEOGRAPH_WASM_DIR ?? 'build-wasm/wasm');
const { load } = await import(pathToFileURL(path.join(directory, 'neograph.mjs')).href);

const [baseUrl, apiPath, model, prompt, flag] = process.argv.slice(2);
if (!prompt) {
  console.error('usage: API_KEY=<key> node chat.mjs <base url> <path> <model> "<prompt>" [--stream]');
  process.exit(2);
}
const stream = flag === '--stream';

// A graph with one llm_call node: the conversation goes in, the model's reply is appended to it.
const graph = {
  channels: { messages: { reducer: 'append' } },
  nodes: { llm: { type: 'llm_call' } },
  edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
};

const neograph = await load();
const result = await neograph.run({
  graph,
  provider: { baseUrl, path: apiPath, apiKey: process.env.API_KEY, model },
  input: { messages: [{ role: 'user', content: prompt }] },
  // With an onEvent the model is called in streaming mode and the reply arrives token by token.
  onEvent: stream ? (event) => { if (event.type === 'llm_token') process.stdout.write(event.data); } : undefined,
});
console.log(stream ? '' : result.text);
console.error(`${result.status}, ${JSON.stringify(result.usage)}`);
