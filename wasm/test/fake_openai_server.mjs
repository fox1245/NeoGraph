// A local HTTP server that answers like an OpenAI-compatible Chat Completions endpoint, for the tests that run the
// WebAssembly module offline: buffered JSON and server-sent events. It needs no network and no key.
//
// The reply to a conversation is `You said: <last user message> (안녕)`. A streamed reply arrives in three events, and the
// two UTF-8 bytes-halves of a Korean syllable are written apart so that a chunk ends inside a multi-byte character.
// A few model names select a behavior of the fixture, so that one server serves every test:
//   fail-401   answers 401 with an OpenAI-style error body
//   hang       accepts the request and never answers; `server.aborted` counts the connections the client closed
//   slow       waits `slowMs` before it answers, for tests that watch the host's event loop
import http from 'node:http';
import { setTimeout as sleep } from 'node:timers/promises';

export const KOREAN = '안녕';

export function replyTo(prompt) {
  return `You said: ${prompt} (${KOREAN})`;
}

function chunk(model, delta, finish = null, usage) {
  return `data: ${JSON.stringify({
    id: 'chatcmpl-fake', object: 'chat.completion.chunk', created: 1, model,
    choices: [{ index: 0, delta, finish_reason: finish }],
    ...(usage && { usage }),
  })}\n\n`;
}

export async function startFakeOpenAI({ slowMs = 150 } = {}) {
  const server = {
    requests: [], // { method, url, headers, body }
    aborted: 0,
    url: '',
    close: async () => {},
  };
  const http_ = http.createServer(async (req, res) => {
    const chunks = [];
    for await (const part of req) chunks.push(part);
    const text = Buffer.concat(chunks).toString('utf8');
    let body = {};
    try { body = text ? JSON.parse(text) : {}; } catch { /* recorded as given */ }
    server.requests.push({ method: req.method, url: req.url, headers: req.headers, body });
    res.socket?.setNoDelay(true);

    const model = body.model ?? 'unknown';
    if (model === 'hang') {
      req.socket.on('close', () => { server.aborted += 1; });
      return; // never answered
    }
    if (model === 'fail-401') {
      res.writeHead(401, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ error: { message: 'Incorrect API key', type: 'invalid_request_error', code: 'invalid_api_key' } }));
      return;
    }
    if (model === 'slow') await sleep(slowMs);
    if (req.headers.authorization === undefined) {
      res.writeHead(401, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ error: { message: 'No key', type: 'invalid_request_error', code: 'missing_key' } }));
      return;
    }

    const messages = body.messages ?? [];
    const last = [...messages].reverse().find((m) => m.role === 'user');
    // A message's content is a string, or a list of typed parts (the SDK sends the list).
    const content = last?.content;
    const prompt = typeof content === 'string' ? content
      : Array.isArray(content) ? content.filter((part) => part.type === 'text').map((part) => part.text).join('')
      : JSON.stringify(content);
    const reply = replyTo(prompt);
    const usage = { prompt_tokens: 3, completion_tokens: 5, total_tokens: 8 };

    if (!body.stream) {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify({
        id: 'chatcmpl-fake', object: 'chat.completion', created: 1, model,
        choices: [{ index: 0, message: { role: 'assistant', content: reply }, finish_reason: 'stop' }],
        usage,
      }));
      return;
    }

    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
    const pieces = [`You said: `, `${prompt} (`, `${KOREAN})`];
    const events = [
      chunk(model, { role: 'assistant', content: pieces[0] }),
      chunk(model, { content: pieces[1] }),
      chunk(model, { content: pieces[2] }),
      chunk(model, {}, 'stop', usage),
      'data: [DONE]\n\n',
    ];
    for (const event of events) {
      const bytes = Buffer.from(event, 'utf8');
      // Cut the event between the bytes of a Korean syllable (three bytes in UTF-8), and write the halves apart.
      const at = event.includes(KOREAN) ? Buffer.from(event.slice(0, event.indexOf(KOREAN) + 1), 'utf8').length - 1 : -1;
      if (at > 0) {
        res.write(bytes.subarray(0, at));
        await sleep(15);
        res.write(bytes.subarray(at));
      } else {
        res.write(bytes);
      }
      await sleep(5);
    }
    res.end();
  });
  await new Promise((resolve) => http_.listen(0, '127.0.0.1', resolve));
  server.url = `http://127.0.0.1:${http_.address().port}`;
  server.close = () => new Promise((resolve) => { http_.closeAllConnections(); http_.close(resolve); });
  return server;
}
