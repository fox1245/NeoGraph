// The Workers example (wasm/examples/workers/worker.mjs) in plain Node: its exported fetch handler is called with a
// Request, as the platform calls it, against a local OpenAI-compatible server. No wrangler, no workerd, no network and
// no key. This shows the handler's logic and the module's use of Request/Response/fetch/TransformStream; it does not run
// on workerd.
//
//   node wasm/test/workers_test.mjs <directory with neograph.mjs, stores.mjs, neograph_module.mjs and neograph_module.wasm>
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { register } from 'node:module';
import { pathToFileURL } from 'node:url';
import { startFakeOpenAI, replyTo } from './fake_openai_server.mjs';

const here = path.dirname(new URL(import.meta.url).pathname);
const modules = path.resolve(process.argv[2] ?? '.');
const examples = path.resolve(here, '../examples/workers');

// The Worker's directory as a project has it: the example next to the module it imports.
const project = fs.mkdtempSync(path.join(os.tmpdir(), 'neograph-worker-'));
fs.copyFileSync(path.join(examples, 'worker.mjs'), path.join(project, 'worker.mjs'));
for (const name of ['neograph.mjs', 'stores.mjs', 'neograph_module.mjs', 'neograph_module.wasm']) {
  fs.copyFileSync(path.join(modules, name), path.join(project, name));
}
register(pathToFileURL(path.join(here, 'wasm_import_loader.mjs')));
const worker = (await import(pathToFileURL(path.join(project, 'worker.mjs')).href)).default;

const server = await startFakeOpenAI();
const KEY = 'worker-test-key';
const env = (model = 'fixture-model') => ({ BASE_URL: server.url, API_PATH: '/v1/chat/completions', MODEL: model, API_KEY: KEY });
const post = (body, bindings = env()) => {
  const background = [];
  const ctx = { waitUntil: (promise) => background.push(promise) };
  const request = new Request('https://worker.example/', { method: 'POST', body: typeof body === 'string' ? body : JSON.stringify(body) });
  return worker.fetch(request, bindings, ctx).then(async (response) => ({ response, finish: () => Promise.all(background) }));
};

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

await check('a request is answered with the model reply as JSON', async () => {
  const { response } = await post({ message: 'Hello from a Worker' });
  assert.equal(response.status, 200);
  const body = await response.json();
  assert.equal(body.text, replyTo('Hello from a Worker'));
  assert.deepEqual(body.usage, { input_tokens: 3, output_tokens: 5, total_tokens: 8 });
  const sent = server.requests.at(-1);
  assert.equal(sent.headers.authorization, `Bearer ${KEY}`);
  assert.equal(sent.body.model, 'fixture-model');
  assert.ok(!JSON.stringify(body).includes(KEY));
});

await check('a streamed request is answered with server-sent events', async () => {
  const { response, finish } = await post({ message: 'Stream it', stream: true });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('content-type'), 'text/event-stream');
  const text = await response.text();
  await finish();
  assert.equal(server.requests.at(-1).body.stream, true);
  const events = text.split('\n\n').filter(Boolean).map((block) => {
    const [name, data] = block.split('\n');
    return { name: name.slice('event: '.length), data: JSON.parse(data.slice('data: '.length)) };
  });
  const tokens = events.filter((e) => e.name === 'token').map((e) => e.data);
  assert.ok(tokens.length >= 3, `${tokens.length} token events`);
  assert.equal(tokens.join(''), replyTo('Stream it'));
  assert.equal(events.at(-1).name, 'done');
  assert.equal(events.at(-1).data.text, replyTo('Stream it'));
});

await check('requests in flight together are answered separately', async () => {
  const answers = await Promise.all(['a', 'b', 'c'].map(async (message) => {
    const { response } = await post({ message }, env(message === 'b' ? 'slow' : 'fixture-model'));
    return (await response.json()).text;
  }));
  assert.deepEqual(answers, ['a', 'b', 'c'].map(replyTo));
});

await check("the model's refusal is reported, not thrown", async () => {
  const { response } = await post({ message: 'x' }, env('fail-401'));
  assert.equal(response.status, 502);
  const body = await response.json();
  assert.equal(body.type, 'provider');
  assert.equal(body.kind, 'authentication');
  assert.ok(!JSON.stringify(body).includes(KEY));
});

await check('a malformed request is refused before any model call', async () => {
  const before = server.requests.length;
  assert.equal((await worker.fetch(new Request('https://worker.example/'), env(), {})).status, 405);
  assert.equal((await post('not json')).response.status, 400);
  assert.equal((await post({ nothing: true })).response.status, 400);
  assert.equal(server.requests.length, before);
});

await check('the wrangler.toml template holds no secret', async () => {
  const toml = fs.readFileSync(path.join(examples, 'wrangler.toml'), 'utf8');
  assert.match(toml, /^main = "worker\.mjs"$/m);
  assert.ok(!/API_KEY\s*=/.test(toml), 'the key must be a secret binding');
  assert.ok(!/(sk-|Bearer |token)/i.test(toml.replace(/#.*$/gm, '')));
});

await server.close();
fs.rmSync(project, { recursive: true, force: true });
if (failures > 0) {
  console.log(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
