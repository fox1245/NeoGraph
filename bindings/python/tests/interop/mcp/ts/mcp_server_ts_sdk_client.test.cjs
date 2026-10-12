// Official TypeScript MCP SDK clients -> NeoGraph MCP server (node:test).
//
//   SDK_NODE_MODULES=<dir with node_modules from ../package.json> \
//   NEOGRAPH_BUILD_DIR=<build> NG_PYTHON=<python with the pinned Python SDK> \
//   node --test mcp_server_ts_sdk_client.test.cjs
//
// Variants: @modelcontextprotocol/sdk 1.32.1 (v1 line, still the most deployed) and
// @modelcontextprotocol/client 2.3.1 (dual-era v2) in its default 'legacy' mode and
// in versionNegotiation 'auto' (server/discover probe + fallback). Zod result schemas
// in these SDKs are the strict oracle; `client.onerror` events count as failures.
// NG_MCP_TRANSCRIPTS (optional) collects the wire for the schema check of the
// Python suite (test_mcp_server_zz_schema.py).
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const os = require('node:os');
const net = require('node:net');
const { spawn } = require('node:child_process');

const N = process.env.SDK_NODE_MODULES;
const BUILD = process.env.NEOGRAPH_BUILD_DIR;
if (!N || !BUILD) {
  console.error('SDK_NODE_MODULES and NEOGRAPH_BUILD_DIR are required');
  process.exit(2);
}
const req = require('node:module').createRequire(path.join(N, 'node_modules') + '/');
const PROBE = process.env.NG_MCP_PROBE_SERVER || path.join(BUILD, 'tests', 'neograph_mcp_interop_probe');
const HARNESS = path.join(BUILD, 'example_harness_mcp_server');
const PY = process.env.NG_PYTHON || 'python3';
const HERE = __dirname;
const TEE = path.join(HERE, '..', 'helpers', 'stdio_tee.py');
const PROXY = path.join(HERE, '..', 'helpers', 'http_record_proxy.py');
const TMP = os.tmpdir();
const TRANSCRIPTS = process.env.NG_MCP_TRANSCRIPTS || fs.mkdtempSync(path.join(TMP, 'neograph-mcp-ts-'));
fs.mkdirSync(TRANSCRIPTS, { recursive: true });

const v1 = {
  Client: req('mcp-sdk-v1/client/index.js').Client,
  Stdio: req('mcp-sdk-v1/client/stdio.js').StdioClientTransport,
  Http: req('mcp-sdk-v1/client/streamableHttp.js').StreamableHTTPClientTransport,
};
const v2mod = req('@modelcontextprotocol/client');
const v2 = { Client: v2mod.Client, Http: v2mod.StreamableHTTPClientTransport,
  Stdio: req('@modelcontextprotocol/client/stdio').StdioClientTransport };
const SDKS = {
  'sdk-1.32.1': { ...v1, opts: {}, call: (c, p, o) => c.callTool(p, undefined, o) },
  'client-2.3.1-legacy': { ...v2, opts: {}, call: (c, p, o) => c.callTool(p, o) },
  'client-2.3.1-auto': { ...v2, opts: { versionNegotiation: { mode: 'auto' } },
    call: (c, p, o) => c.callTool(p, o) },
};

let nextPort = 18150;
async function freePort() {
  for (; nextPort < 18200; nextPort++) {
    const p = nextPort;
    const ok = await new Promise((res) => { const s = net.createServer(); s.once('error', () => res(false));
      s.listen(p, '127.0.0.1', () => s.close(() => res(true))); });
    if (ok) { nextPort++; return p; }
  }
  throw new Error('no free port');
}
async function waitPort(port) {
  for (let i = 0; i < 200; i++) {
    const ok = await new Promise((res) => { const s = net.connect(port, '127.0.0.1');
      s.once('connect', () => { s.destroy(); res(true); }); s.once('error', () => res(false)); });
    if (ok) return;
    await new Promise((r) => setTimeout(r, 50));
  }
  throw new Error(`port ${port} not ready`);
}
const children = [];
function start(cmd, args, env) {
  const ch = spawn(cmd, args, { env: { ...process.env, ...(env || {}) }, cwd: TMP, stdio: 'ignore' });
  children.push(ch);
  return ch;
}
test.after(() => { for (const c of children) c.kill('SIGTERM'); });

function logPath(name) { const p = path.join(TRANSCRIPTS, `ts-${name.replace(/[^A-Za-z0-9_.-]/g, '_')}.jsonl`);
  fs.writeFileSync(p, ''); return p; }
function wire(p) { return fs.readFileSync(p, 'utf8').split('\n').filter(Boolean).map((l) => {
  const r = JSON.parse(l); try { r.msg = r.raw ? JSON.parse(r.raw) : null; } catch { r.msg = null; } return r; }); }

async function httpUpstream(argv, env) {
  const port = await freePort();
  start(argv[0], [...argv.slice(1), ...(argv[0] === PROBE ? ['http', String(port)] : [])],
    { ...(env || {}), NEOGRAPH_HARNESS_HTTP_PORT: String(port) });
  await waitPort(port);
  return port;
}
async function recordingProxy(upstream, log) {
  const port = await freePort();
  start(PY, ['-I', '-B', PROXY, String(port), String(upstream), log]);
  await waitPort(port);
  return `http://127.0.0.1:${port}/mcp`;
}

async function connect(sdk, transport, log, { serverArgv = [PROBE], url, env, headers } = {}) {
  const S = SDKS[sdk];
  const client = new S.Client({ name: 'ng-interop-ts', version: '0.0.1' }, S.opts);
  const errors = [];
  client.onerror = (e) => errors.push(String((e && e.message) || e));
  let t;
  if (transport === 'stdio') {
    const args = serverArgv[0] === PROBE ? ['stdio', ...serverArgv.slice(1)] : serverArgv.slice(1);
    t = new S.Stdio({ command: PY, args: ['-I', '-B', TEE, log, '--', serverArgv[0], ...args],
      env: { PATH: process.env.PATH, ...(env || {}) }, stderr: 'pipe', cwd: TMP });
  } else {
    t = new S.Http(new URL(url), headers ? { requestInit: { headers } } : undefined);
  }
  await client.connect(t);
  return { client, errors, S };
}

const responsesFor = (log, id) => wire(log).filter((r) => r.dir === 's2c' && r.msg && r.msg.id === id).map((r) => r.msg);
const requestIdOf = (log, name) => { const r = wire(log).find((x) => x.dir === 'c2s' && x.msg &&
  x.msg.method === 'tools/call' && x.msg.params && x.msg.params.name === name); return r && r.msg.id; };

for (const sdk of Object.keys(SDKS)) {
  for (const transport of ['stdio', 'http']) {
    test(`${sdk} ${transport}: NeoGraph probe server`, async (t) => {
      const log = logPath(`${sdk}-${transport}-probe`);
      let url;
      if (transport === 'http') url = await recordingProxy(await httpUpstream([PROBE]), log);
      const { client, errors, S } = await connect(sdk, transport, log, { url });
      try {
        await t.test('initialize: server identity and instructions', () => {
          assert.equal(client.getServerVersion().name, 'neograph-mcp-probe');
          assert.equal(client.getInstructions(), 'NeoGraph MCP interop probe server.');
          assert.ok(client.getServerCapabilities().tools);
        });
        await t.test('tools/list passes the SDK result schema', async () => {
          const { tools } = await client.listTools();
          const d = tools.find((x) => x.name === 'decorated');
          assert.equal(tools.length, 7);
          assert.equal(d.icons[0].src, 'https://example.com/icon.png');
          assert.deepEqual(d.execution, { taskSupport: 'forbidden' });
          assert.equal(d._meta['io.neograph.probe/kind'], 'decorated');
        });
        await t.test('tools/call structured output passes SDK outputSchema validation', async () => {
          const r = await S.call(client, { name: 'echo', arguments: { message: 'hi' } });
          assert.equal(r.isError, false);
          assert.deepEqual(r.structuredContent, { message: 'hi' });
          const rich = await S.call(client, { name: 'decorated', arguments: {} });
          assert.deepEqual(rich.content.map((c) => c.type), ['text', 'image', 'resource_link']);
        });
        await t.test('tool failures are isError results', async () => {
          for (const name of ['fail', 'soft_error', 'bad_output']) {
            const r = await S.call(client, { name, arguments: {} });
            assert.equal(r.isError, true, name);
          }
          const bad = await S.call(client, { name: 'echo', arguments: { message: 3 } });
          assert.equal(bad.isError, true);
        });
        await t.test('unknown tool is a JSON-RPC error (-32602)', async () => {
          let r;
          try { r = await S.call(client, { name: 'no_such_tool', arguments: {} }); }
          catch (e) { assert.equal(e.code, -32602, String(e)); return; }
          assert.fail(`unknown tool answered with a result: ${JSON.stringify(r)}`);
        });
        await t.test('ping', async () => { assert.deepEqual(await client.ping(), {}); });
        await t.test('cancellation: no late response, no SDK error event', async () => {
          const before = errors.length;
          const ac = new AbortController();
          const p = S.call(client, { name: 'slow', arguments: { seconds: 3 } }, { signal: ac.signal });
          setTimeout(() => ac.abort('user cancel'), 250);
          await assert.rejects(p);
          await new Promise((r) => setTimeout(r, 900));
          const ok = await S.call(client, { name: 'echo', arguments: { message: 'after' } });
          assert.deepEqual(ok.structuredContent, { message: 'after' });
          const rid = requestIdOf(log, 'slow');
          assert.deepEqual(errors.slice(before), [],
            `SDK error events after cancel; NeoGraph wire: ${JSON.stringify(responsesFor(log, rid))}`);
        });
        await t.test('no SDK error events overall', () => {
          assert.deepEqual(errors.filter((e) => !e.includes('unknown message ID')), []);
        });
      } finally {
        await client.close();
      }
    });
  }

  test(`${sdk} stdio: tool registered with the default inputSchema`, async () => {
    const log = logPath(`${sdk}-stdio-untyped`);
    const { client } = await connect(sdk, 'stdio', log, { serverArgv: [PROBE, '--untyped-tool'] });
    try {
      try { await client.listTools(); }
      catch (e) {
        const listed = wire(log).filter((r) => r.dir === 's2c' && r.msg && r.msg.result && r.msg.result.tools).pop();
        assert.fail(`SDK rejected tools/list: ${String(e.message).replace(/\s+/g, ' ').slice(0, 400)}\n` +
          `offending tool: ${JSON.stringify(listed.msg.result.tools.find((x) => x.name === 'untyped'))}`);
      }
    } finally { await client.close(); }
  });

  for (const transport of ['stdio', 'http']) {
    test(`${sdk} ${transport}: NeoGraph Harness MCP server (smoke provider)`, async () => {
      const log = logPath(`${sdk}-${transport}-harness`);
      const env = { NEOGRAPH_HARNESS_SMOKE: '1' };
      let url;
      if (transport === 'http') {
        url = await recordingProxy(await httpUpstream([HARNESS], { ...env, NEOGRAPH_HARNESS_TRANSPORT: 'http' }), log);
      }
      const { client, errors, S } = await connect(sdk, transport, log, { serverArgv: [HARNESS], url, env });
      try {
        const names = (await client.listTools()).tools.map((x) => x.name).sort();
        assert.deepEqual(names, ['neograph_cancel', 'neograph_compile', 'neograph_get',
          'neograph_resume', 'neograph_schema', 'neograph_start']);
        const r = await S.call(client, { name: 'neograph_schema', arguments: {} });
        assert.equal(r.isError, false);
        assert.ok(r.structuredContent.request_schema);
        const c = await S.call(client, { name: 'neograph_cancel', arguments: { run_id: 'missing' } });
        assert.deepEqual(c.structuredContent, { run_id: 'missing', cancelled: false });
        assert.deepEqual(errors, []);
      } finally { await client.close(); }
    });
  }

  test(`${sdk} http: static bearer token`, async () => {
    const log = logPath(`${sdk}-http-bearer`);
    const url = await recordingProxy(await httpUpstream([PROBE], { NG_PROBE_BEARER: 's3cret' }), log);
    const { client, errors } = await connect(sdk, 'http', log, { url, headers: { Authorization: 'Bearer s3cret' } });
    try {
      assert.equal((await client.listTools()).tools.length, 7);
      assert.deepEqual(errors, []);
    } finally { await client.close(); }
  });
}

// Official TS clients do not send DELETE on close(); a server with a fixed session
// capacity must expire or evict idle sessions ("The server MAY terminate the session
// at any time"), or the 65th client is refused forever.
for (const sdk of ['sdk-1.32.1', 'client-2.3.1-legacy']) {
  test(`${sdk} http: sessions abandoned by close() do not exhaust the server`, async () => {
    const port = await httpUpstream([PROBE]);
    const S = SDKS[sdk];
    for (let i = 1; i <= 70; i++) {
      const c = new S.Client({ name: 'ng-interop-ts', version: '0.0.1' }, S.opts);
      try { await c.connect(new S.Http(new URL(`http://127.0.0.1:${port}/mcp`))); }
      catch (e) { assert.fail(`connect #${i} rejected: ${String(e.message).slice(0, 300)}`); }
      await c.close();
    }
  });
}
