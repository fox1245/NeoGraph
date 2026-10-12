// Official @a2a-js/sdk 0.3.x client (a pure A2A 0.3 implementation) -> NeoGraph
// native A2AServer. NeoGraph answers the 0.3 dialect when A2A-Version is absent.
//
// Usage: node ts_client_v03_native_server.cjs <base-url> <sdk-0.3-dist-dir>
"use strict";

const path = require("node:path");
const [, , BASE, DIST] = process.argv;
const { ClientFactory } = require(path.join(DIST, "client", "index.cjs"));

const noise = [];
for (const k of ["warn", "error"]) {
  const orig = console[k];
  console[k] = (...a) => { noise.push(a.map(String).join(" ").slice(0, 400)); orig.apply(console, a); };
}
let failures = 0;
function report(check, ok, extra = {}) {
  if (!ok) failures++;
  process.stdout.write(JSON.stringify({ check, result: ok ? "PASS" : "FAIL", ...extra }) + "\n");
}
async function run(check, fn) {
  const before = noise.length;
  try {
    const detail = await fn();
    const warned = noise.slice(before);
    report(check, warned.length === 0, { detail, warnings: warned.length ? warned : undefined });
  } catch (e) {
    report(check, false, { error: `${e && e.constructor && e.constructor.name}: ${e && e.message}` });
  }
}
let seq = 0;
const msg = (text, extra = {}) => ({
  kind: "message", messageId: `ts03-${Date.now()}-${seq++}`, role: "user",
  parts: [{ kind: "text", text }], ...extra,
});
const stateOf = (ev) => (ev.kind === "task" ? ev.status.state : ev.kind === "status-update" ? ev.status.state : ev.kind);

(async () => {
  const P = "[0.3/sdk-0.3.14] ";
  let client;
  await run(P + "card+client", async () => {
    client = await new ClientFactory().createFromUrl(BASE);
    return "ok";
  });
  if (!client) process.exit(1);

  await run(P + "message/send", async () => {
    const r = await client.sendMessage({ message: msg("echo v03") });
    if (r.kind !== "task" || r.status.state !== "completed") throw new Error(JSON.stringify(r));
    return r.status.state;
  });

  await run(P + "message/stream.artifact", async () => {
    const events = [];
    for await (const ev of client.sendMessageStream({ message: msg("artifact v03") })) events.push(ev);
    const kinds = events.map((e) => `${e.kind}${e.final ? "(final)" : ""}`);
    const artifact = events.some((e) => e.kind === "artifact-update" ||
                                       (e.kind === "task" && (e.artifacts || []).length));
    if (!artifact) throw new Error(`no artifact reached the client: ${kinds}`);
    return kinds;
  });

  await run(P + "tasks/get", async () => {
    const r = await client.sendMessage({ message: msg("echo get") });
    const g = await client.getTask({ id: r.id });
    return g.status.state;
  });

  await run(P + "tasks/cancel.running", async () => {
    let canceled;
    const events = [];
    for await (const ev of client.sendMessageStream({ message: msg("slow v03") })) {
      events.push(ev);
      if (events.length === 1) canceled = await client.cancelTask({ id: ev.taskId || ev.id });
    }
    if (canceled.status.state !== "canceled") throw new Error(JSON.stringify(canceled.status));
    return events.map(stateOf);
  });

  await run(P + "tasks/cancel.terminal->TaskNotCancelableError", async () => {
    const r = await client.sendMessage({ message: msg("echo done") });
    try {
      const c = await client.cancelTask({ id: r.id });
      throw new Error(`no error; NeoGraph returned state ${c.status.state}`);
    } catch (e) {
      if (/NotCancelable/.test(e.constructor.name) || /-32002/.test(e.message)) return e.message;
      throw e;
    }
  });

  await run(P + "tasks/resubscribe", async () => {
    const it = client.sendMessageStream({ message: msg("slow resub") })[Symbol.asyncIterator]();
    const first = (await it.next()).value;
    const sub = [];
    for await (const ev of client.resubscribeTask({ id: first.taskId || first.id })) sub.push(stateOf(ev));
    for (;;) { if ((await it.next()).done) break; }
    return sub;
  });

  for (const [prompt, want] of [["fail", "failed"], ["interrupt", "input-required"]]) {
    await run(P + `message/stream.${prompt}`, async () => {
      const events = [];
      for await (const ev of client.sendMessageStream({ message: msg(prompt) })) events.push(ev);
      const st = stateOf(events[events.length - 1]);
      if (st !== want) throw new Error(`ended ${st}`);
      return events.map(stateOf);
    });
  }

  await run(P + "message/send.terminal_task->error", async () => {
    const r = await client.sendMessage({ message: msg("echo one") });
    try {
      const again = await client.sendMessage({ message: msg("echo two", { taskId: r.id, contextId: r.contextId }) });
      throw new Error(`no error; NeoGraph re-ran the terminal task -> ${again.status && again.status.state}`);
    } catch (e) {
      if (/UnsupportedOperation|InvalidParams/.test(e.constructor.name) || /-3200[0-9]|-32602/.test(e.message)) return e.message;
      throw e;
    }
  });
  process.exit(failures ? 1 : 0);
})();
