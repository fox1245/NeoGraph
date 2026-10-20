// The JavaScript module against a local OpenAI-compatible server, offline and without a key: a graph with one llm_call
// node, run through the host's fetch, buffered and streamed.
//
//   node wasm/test/llm_host_test.mjs <directory with neograph.mjs, neograph_module.mjs and neograph_module.wasm>
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import { startFakeOpenAI, replyTo, KOREAN } from './fake_openai_server.mjs';

const directory = path.resolve(process.argv[2] ?? '.');
const { load, NeoGraphError } = await import(pathToFileURL(path.join(directory, 'neograph.mjs')).href);

const KEY = 'test-key-0123456789';
const GRAPH = {
  channels: { messages: { reducer: 'append' } },
  nodes: { llm: { type: 'llm_call' } },
  edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
};

const server = await startFakeOpenAI();
const provider = (model = 'fixture-model') => ({
  baseUrl: server.url, path: '/v1/chat/completions', apiKey: KEY, model,
});
const ask = (content) => ({ messages: [{ role: 'user', content }] });

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

await check('a buffered run returns the final state', async () => {
  const before = server.requests.length;
  const result = await neograph.run({ graph: GRAPH, provider: provider(), input: ask('Hello') });
  assert.equal(result.status, 'completed');
  assert.equal(result.text, replyTo('Hello'));
  assert.deepEqual(result.trace, ['llm']);
  assert.deepEqual(result.usage, { input_tokens: 3, output_tokens: 5, total_tokens: 8 });
  // The final state holds the conversation: the question and the model's answer.
  const messages = result.output.channels.messages.value;
  assert.ok(Array.isArray(messages) && messages.length === 2, JSON.stringify(messages));
  assert.match(JSON.stringify(messages), new RegExp(KOREAN));
  // One request went out through fetch, with the key given at run time and the model given at run time.
  assert.equal(server.requests.length, before + 1);
  const request = server.requests.at(-1);
  assert.equal(request.method, 'POST');
  assert.equal(request.url, '/v1/chat/completions');
  assert.equal(request.headers.authorization, `Bearer ${KEY}`);
  assert.equal(request.body.model, 'fixture-model');
  assert.notEqual(request.body.stream, true);
  assert.deepEqual(request.body.messages.at(-1).content, [{ type: 'text', text: 'Hello' }]);
  // The key is the host's secret: it is not in anything the run returns.
  assert.ok(!JSON.stringify(result).includes(KEY));
});

await check('a streamed run delivers its events in order', async () => {
  const events = [];
  const result = await neograph.run({ graph: GRAPH, provider: provider(), input: ask('Hi there'), onEvent: (e) => events.push(e) });
  assert.equal(server.requests.at(-1).body.stream, true);
  assert.equal(result.status, 'completed');
  assert.equal(result.text, replyTo('Hi there'));
  const types = events.map((e) => e.type);
  assert.equal(types[0], 'node_start');
  assert.equal(types.at(-1), 'node_end');
  assert.ok(events.every((e) => e.node === 'llm'), JSON.stringify(events));
  // The tokens are the model's reply, cut where it was cut, and a character that arrived in two chunks is whole.
  const tokens = events.filter((e) => e.type === 'llm_token').map((e) => e.data);
  assert.ok(tokens.length >= 3, `${tokens.length} tokens`);
  assert.equal(tokens.join(''), replyTo('Hi there'));
  assert.ok(tokens.every((token) => !token.includes('�')));
  assert.ok(!JSON.stringify(events).includes(KEY));
});

await check('a run does not block the host event loop', async () => {
  let ticks = 0;
  const timer = setInterval(() => { ticks += 1; }, 10);
  try {
    const result = await neograph.run({ graph: GRAPH, provider: provider('slow'), input: ask('wait') });
    assert.equal(result.text, replyTo('wait'));
  } finally {
    clearInterval(timer);
  }
  // The server holds the answer for 150 ms; a blocked loop would have counted none.
  assert.ok(ticks >= 5, `${ticks} timer ticks while the model call was in flight`);
});

await check('runs in flight together keep their own answers', async () => {
  const prompts = ['one', 'two', 'three', 'four'];
  const results = await Promise.all(prompts.map((prompt, i) =>
    neograph.run({ graph: GRAPH, provider: provider(i % 2 ? 'slow' : 'fixture-model'), input: ask(prompt), onEvent: i === 2 ? () => {} : undefined })));
  assert.deepEqual(results.map((r) => r.text), prompts.map(replyTo));
});

await check('a compiled graph runs more than once', async () => {
  const graph = neograph.compile(GRAPH, provider());
  try {
    assert.equal((await graph.run(ask('first'))).text, replyTo('first'));
    assert.equal((await graph.run(ask('second'))).text, replyTo('second'));
  } finally {
    graph.release();
  }
  await assert.rejects(graph.run(ask('third')), NeoGraphError);
});

await check("the provider's refusal arrives as a typed error", async () => {
  await assert.rejects(neograph.run({ graph: GRAPH, provider: provider('fail-401'), input: ask('x') }), (error) => {
    assert.ok(error instanceof NeoGraphError);
    assert.equal(error.type, 'provider');
    assert.equal(error.kind, 'authentication');
    assert.equal(error.httpStatus, 401);
    assert.ok(!error.message.includes(KEY));
    return true;
  });
});

await check('aborting a run cancels the model call', async () => {
  const before = server.aborted;
  const controller = new AbortController();
  const run = neograph.run({ graph: GRAPH, provider: provider('hang'), input: ask('x'), signal: controller.signal });
  await sleep(100);
  controller.abort();
  await assert.rejects(run, (error) => error instanceof NeoGraphError && error.type === 'cancelled');
  for (let i = 0; i < 40 && server.aborted === before; i += 1) await sleep(25);
  assert.equal(server.aborted, before + 1, 'the connection was not closed');
});

await check('an error thrown by onEvent ends that run and leaves the engine running', async () => {
  const boom = new Error('the listener failed');
  await assert.rejects(neograph.run({ graph: GRAPH, provider: provider(), input: ask('x'), onEvent: () => { throw boom; } }),
    (error) => error === boom);
  assert.equal((await neograph.run({ graph: GRAPH, provider: provider(), input: ask('after') })).text, replyTo('after'));
});

await check('a timeout ends a run that waits for ever', async () => {
  await assert.rejects(neograph.run({ graph: GRAPH, provider: provider('hang'), input: ask('x'), timeoutMs: 200 }),
    (error) => error instanceof NeoGraphError);
});

await check('an invalid graph or provider is refused when it is compiled', async () => {
  assert.throws(() => neograph.compile('{', provider()), NeoGraphError);
  assert.throws(() => neograph.compile({ nodes: { a: { type: 'no_such_type' } }, edges: [] }, provider()), NeoGraphError);
  assert.throws(() => neograph.compile(GRAPH, { model: 'm' }), NeoGraphError);
  assert.throws(() => neograph.compile(GRAPH, { ...provider(), family: 'no.such.family' }), NeoGraphError);
});

await server.close();
if (failures > 0) {
  console.log(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
