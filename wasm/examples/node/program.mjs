// A Program from Node: two model calls with a pause between them, over the host's fetch.
//
//   API_KEY=<key> node wasm/examples/node/program.mjs <base url> <path> <model> "<prompt>" [--answer <text>]
//
// The Program answers the prompt, stops before its second model call and waits, then continues the conversation with what
// the person answered. The answer comes from `--answer`, or from a line typed at the prompt; an empty answer cancels the run.
// The base url is the origin of an OpenAI-compatible Chat Completions endpoint, and the path is its route. The module is the
// build's wasm/ directory (neograph.mjs, stores.mjs, neograph_module.mjs, neograph_module.wasm); NEOGRAPH_WASM_DIR names another one.
//
// A run's budget bounds the model tokens it may spend, and a call is only sent when its worst case fits what is left. The
// worst case is the model's input limit plus its output limit, which the SDK reads from its model catalogue. A model that
// catalogue does not know needs its limits stated, as `catalogue` does below (NEOGRAPH_INPUT_LIMIT, NEOGRAPH_OUTPUT_LIMIT).
import path from 'node:path';
import readline from 'node:readline/promises';
import { pathToFileURL } from 'node:url';

const directory = path.resolve(process.env.NEOGRAPH_WASM_DIR ?? 'build-wasm/wasm');
const { load } = await import(pathToFileURL(path.join(directory, 'neograph.mjs')).href);

const args = process.argv.slice(2);
const flag = args.indexOf('--answer');
const answer = flag >= 0 ? args.splice(flag, 2)[1] : undefined;
const [baseUrl, apiPath, model, prompt] = args;
if (!prompt) {
  console.error('usage: API_KEY=<key> node program.mjs <base url> <path> <model> "<prompt>" [--answer <text>]');
  process.exit(2);
}

// The control source of the Program. `define()` describes the graph: two model calls, with a pause before the second.
// `main()` runs the graph once and returns the conversation.
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

const neograph = await load();
const programs = neograph.programs({
  provider: {
    baseUrl, path: apiPath, apiKey: process.env.API_KEY, model,
    catalogue: {
      version: 1,
      models: [{ id: model, input_limit: Number(process.env.NEOGRAPH_INPUT_LIMIT ?? 4000),
                 output_limit: Number(process.env.NEOGRAPH_OUTPUT_LIMIT ?? 1000) }],
    },
  },
});

try {
  const version = await programs.admit(SOURCE);
  // The events of a run, in order; a model reply arrives token by token.
  const onEvent = (event) => { if (event.payload?.type === 'llm_token') process.stdout.write(event.payload.token); };
  const run = programs.start(version, { input: { messages: [{ role: 'user', content: prompt }] }, onEvent });
  const paused = await run.result;
  console.log(`\n-- ${paused.status} before ${paused.interrupt?.node}`);
  if (paused.status !== 'interrupted') throw new Error(paused.failure?.message ?? `the run ${paused.status}`);

  let reply = answer;
  if (reply === undefined) {
    const lines = readline.createInterface({ input: process.stdin, output: process.stdout });
    reply = await lines.question('Answer to continue, or nothing to cancel: ');
    lines.close();
  }
  if (!reply) {
    console.log(`cancelled: ${await run.cancel()}`);
  } else {
    const done = await run.resume(reply, { onEvent }).result;
    console.log(`\n-- ${done.status}`);
    console.error(`${done.status}, ${done.usage.modelTokens} model tokens, ${done.remainingBudget.modelTokens} left`);
  }
} finally {
  programs.release();
}
