// Official TypeScript MCP SDK server used as the oracle peer for NeoGraph's MCP client.
//
// Pinned SDK: @modelcontextprotocol/sdk 1.32.1 (see ../package.json).  The SDK is
// loaded from NG_MCP_NODE_DEPS (a directory holding node_modules), never from the
// script directory.
//
//   node ts_official_server.cjs --transport stdio|http [--port N] [--path /mcp]
//        [--stateless] [--json-response] [--paginate] [--record FILE] [--events FILE]
//
// Tools mirror servers/py_official_server.py (lowlevel flavour).
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const crypto = require('node:crypto');
const { createRequire } = require('node:module');

const depsDir = process.env.NG_MCP_NODE_DEPS;
if (!depsDir) { console.error('NG_MCP_NODE_DEPS is required'); process.exit(2); }
const req = createRequire(path.join(depsDir, 'package.json'));
const { Server } = req('@modelcontextprotocol/sdk/server/index.js');
const { StdioServerTransport } = req('@modelcontextprotocol/sdk/server/stdio.js');
const { StreamableHTTPServerTransport } = req('@modelcontextprotocol/sdk/server/streamableHttp.js');
const types = req('@modelcontextprotocol/sdk/types.js');

const argv = process.argv.slice(2);
const opt = (name, dflt) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : dflt; };
const flag = (name) => argv.includes(name);
const ARGS = {
  transport: opt('--transport', 'stdio'),
  port: Number(opt('--port', '18250')),
  path: opt('--path', '/mcp'),
  stateless: flag('--stateless'),
  json: flag('--json-response'),
  paginate: flag('--paginate'),
  record: opt('--record', null),
  events: opt('--events', null),
  serverRequestTimeout: Number(opt('--server-request-timeout', '4000')),
};

function event(kind, fields = {}) {
  if (!ARGS.events) return;
  fs.appendFileSync(ARGS.events, JSON.stringify({ t: Date.now() / 1000, event: kind, ...fields }) + '\n');
}

const PNG = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgAAAAAgABVKJPXQAAAABJRU5ErkJggg==';
const WAV = Buffer.from('RIFF$\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00@\x1f\x00\x00\x80>\x00\x00\x02\x00\x10\x00data\x00\x00\x00\x00', 'binary').toString('base64');
const OBJ = { type: 'object' };
const ANSWER_SCHEMA = { type: 'object', properties: { answer: { type: 'integer' } }, required: ['answer'] };
const TOOLS = [
  { name: 'echo', description: 'Echo text', inputSchema: { type: 'object', properties: { text: { type: 'string' } }, required: ['text'] } },
  { name: 'structured', title: 'Structured answer', description: 'Returns structuredContent', inputSchema: { type: 'object', properties: { n: { type: 'integer' } } }, outputSchema: ANSWER_SCHEMA, annotations: { readOnlyHint: true, idempotentHint: true } },
  { name: 'tool_error', description: 'Returns isError=true', inputSchema: OBJ },
  { name: 'tool_error_with_schema', description: 'isError while an outputSchema is declared', inputSchema: OBJ, outputSchema: ANSWER_SCHEMA },
  { name: 'protocol_error', description: 'Raises a JSON-RPC error', inputSchema: OBJ },
  { name: 'all_content', description: 'Every content block type', inputSchema: OBJ },
  { name: 'log_then_answer', description: 'Request-scoped log notifications, then a result', inputSchema: OBJ },
  { name: 'ping_client', description: 'Server->client ping inside a tool call', inputSchema: OBJ },
  { name: 'roots_client', description: 'Server->client roots/list (undeclared capability)', inputSchema: OBJ },
  { name: 'slow', description: 'Sleeps; records cancellation', inputSchema: { type: 'object', properties: { seconds: { type: 'number' } } } },
  { name: 'structured_only', description: 'structuredContent with empty content', inputSchema: OBJ, outputSchema: ANSWER_SCHEMA },
];
const PAGE = 4;

function withTimeout(promise, ms) {
  let timer;
  return Promise.race([promise, new Promise((_, rej) => { timer = setTimeout(() => rej(new Error('timeout')), ms); })])
    .finally(() => clearTimeout(timer));
}

function buildServer() {
  const server = new Server({ name: 'ng-probe-ts', version: '1.0.0' },
    { capabilities: { tools: { listChanged: true }, logging: {} }, instructions: 'probe server' });
  server.setRequestHandler(types.ListToolsRequestSchema, async (request) => {
    const cursor = request.params && request.params.cursor;
    event('tools_list', { cursor: cursor ?? null });
    if (!ARGS.paginate) return { tools: TOOLS };
    const start = cursor ? Number(cursor) : 0;
    const res = { tools: TOOLS.slice(start, start + PAGE) };
    if (start + PAGE < TOOLS.length) res.nextCursor = String(start + PAGE);
    return res;
  });
  server.setRequestHandler(types.CallToolRequestSchema, async (request, extra) => {
    const { name } = request.params;
    const args = request.params.arguments || {};
    switch (name) {
      case 'echo': return { content: [{ type: 'text', text: String(args.text ?? '') }] };
      case 'structured': {
        const n = Number(args.n ?? 41);
        return { content: [{ type: 'text', text: JSON.stringify({ answer: n + 1 }) }], structuredContent: { answer: n + 1 } };
      }
      case 'structured_only': return { content: [], structuredContent: { answer: 7 } };
      case 'tool_error': return { content: [{ type: 'text', text: 'boom: tool failed' }], isError: true };
      case 'tool_error_with_schema': return { content: [{ type: 'text', text: 'boom: schema tool failed' }], isError: true };
      case 'protocol_error': throw new types.McpError(-32602, 'Invalid params: probe', { probe: true });
      case 'all_content': return { content: [
        { type: 'text', text: 'text block' },
        { type: 'image', data: PNG, mimeType: 'image/png' },
        { type: 'audio', data: WAV, mimeType: 'audio/wav' },
        { type: 'resource_link', uri: 'file:///probe/readme.txt', name: 'readme', mimeType: 'text/plain' },
        { type: 'resource', resource: { uri: 'file:///probe/inline.txt', mimeType: 'text/plain', text: 'inline text' } },
        { type: 'resource', resource: { uri: 'file:///probe/inline.bin', mimeType: 'application/octet-stream', blob: PNG } },
      ] };
      case 'log_then_answer':
        for (let i = 0; i < 3; i++) {
          await extra.sendNotification({ method: 'notifications/message', params: { level: 'info', logger: 'probe', data: { step: i } } });
        }
        return { content: [{ type: 'text', text: 'answered after logs' }] };
      case 'ping_client':
        try {
          await withTimeout(extra.sendRequest({ method: 'ping' }, types.EmptyResultSchema), ARGS.serverRequestTimeout);
          event('ping_client', { ok: true });
          return { content: [{ type: 'text', text: 'client answered ping' }] };
        } catch (e) {
          event('ping_client', { ok: false, error: String(e && e.message || e) });
          return { content: [{ type: 'text', text: 'client never answered ping: ' + e }], isError: true };
        }
      case 'roots_client':
        try {
          await withTimeout(extra.sendRequest({ method: 'roots/list' }, types.ListRootsResultSchema), ARGS.serverRequestTimeout);
          event('roots_client', { ok: true });
          return { content: [{ type: 'text', text: 'client listed roots' }] };
        } catch (e) {
          const code = e && typeof e.code === 'number' ? e.code : null;
          event('roots_client', { ok: false, error: code !== null ? 'jsonrpc' : String(e && e.message || e), code });
          return { content: [{ type: 'text', text: 'roots/list failed: ' + e }], isError: code === null };
        }
      case 'slow': {
        const seconds = Number(args.seconds ?? 3);
        event('slow_started', { request_id: extra.requestId, seconds });
        const cancelled = await new Promise((resolve) => {
          const timer = setTimeout(() => resolve(false), seconds * 1000);
          extra.signal.addEventListener('abort', () => { clearTimeout(timer); resolve(true); });
        });
        event(cancelled ? 'slow_cancelled' : 'slow_finished', { request_id: extra.requestId, reason: cancelled ? String(extra.signal.reason) : null });
        return { content: [{ type: 'text', text: 'slow done' }] };
      }
      default: throw new types.McpError(-32602, `Unknown tool: ${name}`);
    }
  });
  server.setNotificationHandler(types.CancelledNotificationSchema, async (n) => {
    event('client_cancelled', { params: n.params });
  });
  return server;
}

function recordExchange(entry) {
  if (!ARGS.record) return;
  fs.appendFileSync(ARGS.record, JSON.stringify(entry) + '\n');
}

async function runHttp() {
  const sessions = new Map();
  const httpServer = http.createServer(async (req, res) => {
    const started = Date.now() / 1000;
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const bodyText = Buffer.concat(chunks).toString('utf8');
    const outChunks = [];
    const origWrite = res.write.bind(res);
    const origEnd = res.end.bind(res);
    const origWriteHead = res.writeHead.bind(res);
    let headHeaders = {};
    res.writeHead = (status, ...rest) => {
      const h = rest.find((x) => x && typeof x === 'object');
      if (h) headHeaders = Array.isArray(h) ? Object.fromEntries(h) : h;
      return origWriteHead(status, ...rest);
    };
    res.write = (chunk, ...rest) => { if (chunk) outChunks.push(Buffer.from(chunk)); return origWrite(chunk, ...rest); };
    res.end = (chunk, ...rest) => {
      if (chunk && typeof chunk !== 'function') outChunks.push(Buffer.from(chunk));
      const r = origEnd(chunk, ...rest);
      recordExchange({
        t: started, transport: 'http', method: req.method, path: req.url.split('?')[0],
        query: req.url.includes('?') ? req.url.split('?')[1] : '',
        request_headers: Object.entries(req.headers).map(([k, v]) => [k, String(v)]),
        request_body: bodyText, status: res.statusCode,
        response_headers: Object.entries({ ...res.getHeaders(), ...headHeaders }).map(([k, v]) => [k.toLowerCase(), String(v)]),
        response_body: Buffer.concat(outChunks).toString('utf8'),
      });
      return r;
    };
    if (req.url.split('?')[0] !== ARGS.path) { res.statusCode = 404; res.end('not the MCP endpoint'); return; }
    let parsed;
    try { parsed = bodyText ? JSON.parse(bodyText) : undefined; } catch { parsed = undefined; }
    try {
      if (ARGS.stateless) {
        const server = buildServer();
        const transport = new StreamableHTTPServerTransport({ sessionIdGenerator: undefined, enableJsonResponse: ARGS.json });
        res.on('close', () => { transport.close(); server.close(); });
        await server.connect(transport);
        await transport.handleRequest(req, res, parsed);
        return;
      }
      const sid = req.headers['mcp-session-id'];
      let transport = sid ? sessions.get(sid) : undefined;
      if (!transport) {
        if (sid) {
          // Spec (2025-11-25 transports, session management 3): a terminated or unknown
          // session id MUST be answered with 404.  (The SDK's own express example answers
          // 400 here; this wrapper follows the spec so the client side can be judged.)
          res.writeHead(404, { 'content-type': 'application/json' });
          res.end(JSON.stringify({ jsonrpc: '2.0', error: { code: -32001, message: 'Session not found' }, id: null }));
          return;
        }
        if (!(parsed && parsed.method === 'initialize')) {
          // Let the SDK produce its own rejection (400/404) with a throwaway transport.
          const t = new StreamableHTTPServerTransport({ sessionIdGenerator: () => crypto.randomUUID(), enableJsonResponse: ARGS.json });
          await buildServer().connect(t);
          await t.handleRequest(req, res, parsed);
          return;
        }
        transport = new StreamableHTTPServerTransport({
          sessionIdGenerator: () => crypto.randomUUID(), enableJsonResponse: ARGS.json,
          onsessioninitialized: (id) => { sessions.set(id, transport); event('session_open', { id }); },
          onsessionclosed: (id) => { sessions.delete(id); event('session_closed', { id }); },
        });
        await buildServer().connect(transport);
      }
      await transport.handleRequest(req, res, parsed);
    } catch (e) {
      if (!res.headersSent) { res.statusCode = 500; res.end(String(e)); }
    }
  });
  httpServer.listen(ARGS.port, '127.0.0.1');
  process.on('SIGTERM', () => { httpServer.close(); process.exit(0); });
}

async function runStdio() {
  const server = buildServer();
  await server.connect(new StdioServerTransport());
}

(ARGS.transport === 'http' ? runHttp() : runStdio()).catch((e) => { console.error(e); process.exit(1); });
