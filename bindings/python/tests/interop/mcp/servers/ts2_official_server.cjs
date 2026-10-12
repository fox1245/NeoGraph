// Official TypeScript MCP SDK v2 server (@modelcontextprotocol/server 2.3.1, the
// stable line implementing 2026-07-28 while serving 2025-era handshake traffic as
// "legacy").  Loaded from NG_MCP_NODE_DEPS.
//
//   node ts2_official_server.cjs --transport stdio|http [--port N] [--path /mcp]
//        [--record FILE] [--events FILE]
//
// HTTP uses createMcpHandler (the v2 recommended entry; 2025 traffic is served
// statelessly, GET/DELETE answer 405).  Tools: a subset of ts_official_server.cjs.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { createRequire } = require('node:module');

const depsDir = process.env.NG_MCP_NODE_DEPS;
if (!depsDir) { console.error('NG_MCP_NODE_DEPS is required'); process.exit(2); }
const req = createRequire(path.join(depsDir, 'package.json'));
const sdk = req('@modelcontextprotocol/server');
const stdio = req('@modelcontextprotocol/server/stdio');

const argv = process.argv.slice(2);
const opt = (name, dflt) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : dflt; };
const ARGS = {
  transport: opt('--transport', 'stdio'), port: Number(opt('--port', '0')), path: opt('--path', '/mcp'),
  record: opt('--record', null), events: opt('--events', null),
  legacy: opt('--legacy', 'stateless'),   // 'reject' = modern-only (2026-07-28) deployment
};
const event = (kind, fields = {}) => {
  if (ARGS.events) fs.appendFileSync(ARGS.events, JSON.stringify({ t: Date.now() / 1000, event: kind, ...fields }) + '\n');
};

const ANSWER = { type: 'object', properties: { answer: { type: 'integer' } }, required: ['answer'] };
const TOOLS = [
  { name: 'echo', description: 'Echo text', inputSchema: { type: 'object', properties: { text: { type: 'string' } }, required: ['text'] } },
  { name: 'structured', description: 'structuredContent', inputSchema: { type: 'object', properties: { n: { type: 'integer' } } }, outputSchema: ANSWER },
  { name: 'tool_error', description: 'isError', inputSchema: { type: 'object' } },
  { name: 'protocol_error', description: 'JSON-RPC error', inputSchema: { type: 'object' } },
  { name: 'log_then_answer', description: 'logs then answers', inputSchema: { type: 'object' } },
  { name: 'ping_client', description: 'server->client ping', inputSchema: { type: 'object' } },
];

function factory() {
  const server = new sdk.Server({ name: 'ng-probe-ts2', version: '1.0.0' },
    { capabilities: { tools: {}, logging: {} } });
  server.setRequestHandler('tools/list', async () => ({ tools: TOOLS }));
  server.setRequestHandler('tools/call', async (request, ctx) => {
    const { name } = request.params;
    const args = request.params.arguments || {};
    switch (name) {
      case 'echo': return { content: [{ type: 'text', text: String(args.text ?? '') }] };
      case 'structured': { const n = Number(args.n ?? 41); return { content: [{ type: 'text', text: String(n + 1) }], structuredContent: { answer: n + 1 } }; }
      case 'tool_error': return { content: [{ type: 'text', text: 'boom: tool failed' }], isError: true };
      case 'protocol_error': throw new sdk.ProtocolError(-32602, 'Invalid params: probe', { probe: true });
      case 'log_then_answer':
        for (let i = 0; i < 3; i++) await ctx.mcpReq.notify({ method: 'notifications/message', params: { level: 'info', logger: 'probe', data: { step: i } } });
        return { content: [{ type: 'text', text: 'answered after logs' }] };
      case 'ping_client':
        try {
          await Promise.race([ctx.mcpReq.send({ method: 'ping' }), new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 4000))]);
          event('ping_client', { ok: true });
          return { content: [{ type: 'text', text: 'client answered ping' }] };
        } catch (e) {
          event('ping_client', { ok: false, error: String(e && e.message || e) });
          return { content: [{ type: 'text', text: 'ping failed: ' + e }], isError: true };
        }
      default: throw new sdk.ProtocolError(-32602, `Unknown tool: ${name}`);
    }
  });
  return server;
}

async function runHttp() {
  const handler = sdk.createMcpHandler(factory, { legacy: ARGS.legacy });
  const httpServer = http.createServer(async (nreq, nres) => {
    const started = Date.now() / 1000;
    const chunks = [];
    for await (const c of nreq) chunks.push(c);
    const body = Buffer.concat(chunks);
    const pathOnly = nreq.url.split('?')[0];
    let status = 404; let headers = {}; let text = 'not the MCP endpoint';
    if (pathOnly === ARGS.path) {
      const request = new Request(`http://${nreq.headers.host}${nreq.url}`, {
        method: nreq.method, headers: Object.entries(nreq.headers).map(([k, v]) => [k, String(v)]),
        body: ['GET', 'HEAD'].includes(nreq.method) ? undefined : body,
      });
      const response = await handler.fetch(request);
      status = response.status;
      headers = Object.fromEntries(response.headers.entries());
      nres.writeHead(status, headers);
      const out = [];
      if (response.body) {
        for await (const chunk of response.body) { out.push(Buffer.from(chunk)); nres.write(chunk); }
      }
      text = Buffer.concat(out).toString('utf8');
      nres.end();
    } else {
      nres.writeHead(status, headers);
      nres.end(text);
    }
    if (ARGS.record) {
      fs.appendFileSync(ARGS.record, JSON.stringify({
        t: started, transport: 'http', method: nreq.method, path: pathOnly, query: '',
        request_headers: Object.entries(nreq.headers).map(([k, v]) => [k, String(v)]),
        request_body: body.toString('utf8'), status,
        response_headers: Object.entries(headers), response_body: text,
      }) + '\n');
    }
  });
  // --port 0: the OS picks; READY <port> says the server listens.
  httpServer.listen(ARGS.port, '127.0.0.1', () => console.log(`READY ${httpServer.address().port}`));
  process.on('SIGTERM', () => { httpServer.close(); process.exit(0); });
}

if (ARGS.transport === 'http') runHttp();
else stdio.serveStdio(factory);
