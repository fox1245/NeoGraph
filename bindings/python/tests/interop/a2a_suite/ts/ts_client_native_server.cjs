// Official @a2a-js/sdk client -> NeoGraph native A2AServer (1.0 and 0.3 interfaces).
//
// Usage: node ts_client_native_server.cjs <base-url> <sdk-dist-dir>
//   <sdk-dist-dir> = .../node_modules/@a2a-js/sdk/dist  (pinned in ../package.json)
// Prints one JSON line per check: {"check", "result": PASS|FAIL, "error"?, "detail"?}
// and exits non-zero when any check fails. Warnings printed by the SDK through
// console.warn / console.error are captured and turn a PASS into a FAIL.
"use strict";

const path = require("node:path");
const [, , BASE, DIST] = process.argv;
if (!BASE || !DIST) {
  console.error("usage: ts_client_native_server.cjs <base-url> <sdk-dist-dir>");
  process.exit(2);
}
const sdk = require(path.join(DIST, "index.cjs"));
const { ClientFactory, ClientFactoryOptions, JsonRpcTransportFactory } =
  require(path.join(DIST, "client", "index.cjs"));

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
    report(check, false, { error: `${e && e.constructor && e.constructor.name}: ${e && e.message}`,
                           code: e && (e.code ?? e.jsonRpcCode) });
  }
}

let seq = 0;
function msg(text, extra = {}) {
  return sdk.Message.fromJSON({
    messageId: `ts-${Date.now()}-${seq++}`,
    role: "ROLE_USER",
    parts: [{ text }],
    ...extra,
  });
}
function req(text, extra = {}) {
  return sdk.SendMessageRequest.fromJSON({ message: sdk.Message.toJSON(msg(text, extra)) });
}
const stateName = (s) => sdk.taskStateToJSON(s);
function lastState(events) {
  const last = events[events.length - 1];
  const p = last && last.payload;
  if (!p) return undefined;
  if (p.$case === "task") return stateName(p.value.status.state);
  if (p.$case === "statusUpdate") return stateName(p.value.status.state);
  return p.$case;
}
function taskIdOf(ev) {
  const p = ev.payload;
  return p.$case === "task" ? p.value.id : p.value.taskId;
}

async function clientFor(dialect) {
  const factory = new ClientFactory();
  const resolved = await factory.createFromUrl(BASE);
  if (dialect === "1.0") return resolved;
  const card = await resolved.getAgentCard();
  // 0.3 needs the SDK's legacy JSON-RPC transport (legacyCompat) and a card
  // that only offers the 0.3 interface.
  const legacy = new ClientFactory(ClientFactoryOptions.createFrom(ClientFactoryOptions.default, {
    transports: [new JsonRpcTransportFactory({ legacyCompat: { enabled: true } })],
  }));
  const only03 = { ...card, supportedInterfaces: card.supportedInterfaces.filter(
    (i) => i.protocolVersion.startsWith("0.3")) };
  const c = await legacy.createFromAgentCard(only03);
  if (!String(c.protocolVersion).startsWith("0.3")) {
    throw new Error(`SDK did not select the 0.3 interface (protocolVersion=${c.protocolVersion})`);
  }
  return c;
}

async function scenarios(dialect) {
  const P = `[${dialect}] `;
  const client = await clientFor(dialect);
  await run(P + "card.protocolVersion", async () => client.protocolVersion);

  await run(P + "SendMessage.unary", async () => {
    const r = await client.sendMessage(req("echo ts"));
    const task = r.payload ? r.payload.value : r;
    const st = stateName(task.status.state);
    if (st !== "TASK_STATE_COMPLETED") throw new Error(`state ${st}`);
    return st;
  });

  await run(P + "SendStreamingMessage.artifact", async () => {
    const events = [];
    for await (const ev of client.sendMessageStream(req("artifact ts"))) events.push(ev);
    const kinds = events.map((e) => e.payload.$case);
    if (dialect === "1.0" && kinds[0] !== "task") throw new Error(`first event ${kinds[0]}`);
    const sawArtifact = kinds.includes("artifactUpdate") ||
      events.some((e) => e.payload.$case === "task" && e.payload.value.artifacts.length);
    if (!sawArtifact) throw new Error(`artifact never reached the SDK: ${kinds}`);
    if (lastState(events) !== "TASK_STATE_COMPLETED" && lastState(events) !== "task") {
      throw new Error(`last ${lastState(events)}`);
    }
    return kinds;
  });

  await run(P + "GetTask", async () => {
    const r = await client.sendMessage(req("echo get"));
    const t = r.payload ? r.payload.value : r;
    const got = await client.getTask(sdk.GetTaskRequest.fromJSON({ id: t.id }));
    return stateName(got.status.state);
  });

  await run(P + "GetTask.not_found->TaskNotFoundError", async () => {
    try {
      await client.getTask(sdk.GetTaskRequest.fromJSON({ id: "no-such-task" }));
    } catch (e) {
      if (/TaskNotFound/.test(e.constructor.name)) return e.message;
      throw e;
    }
    throw new Error("no error for an unknown task");
  });

  await run(P + "CancelTask.running", async () => {
    const events = [];
    let canceled;
    for await (const ev of client.sendMessageStream(req("slow ts"))) {
      events.push(ev);
      if (events.length === 1) {
        canceled = await client.cancelTask(sdk.CancelTaskRequest.fromJSON({ id: taskIdOf(ev) }));
      }
    }
    const st = stateName(canceled.status.state);
    if (st !== "TASK_STATE_CANCELED") throw new Error(`cancel returned ${st}`);
    return { cancel: st, streamEnd: lastState(events) };
  });

  await run(P + "CancelTask.terminal->TaskNotCancelableError", async () => {
    const r = await client.sendMessage(req("echo done"));
    const t = r.payload ? r.payload.value : r;
    try {
      const c = await client.cancelTask(sdk.CancelTaskRequest.fromJSON({ id: t.id }));
      throw new Error(`no error; NeoGraph returned state ${stateName(c.status.state)}`);
    } catch (e) {
      if (/TaskNotCancelable/.test(e.constructor.name)) return e.message;
      throw e;
    }
  });

  if (dialect === "1.0") {
    await run(P + "ListTasks", async () => {
      const r = await client.listTasks(sdk.ListTasksRequest.fromJSON({}));
      return r.tasks.length;
    });
  }

  await run(P + "SubscribeToTask", async () => {
    const it = client.sendMessageStream(req("slow sub"))[Symbol.asyncIterator]();
    const first = (await it.next()).value;
    const sub = [];
    for await (const ev of client.resubscribeTask(sdk.SubscribeToTaskRequest.fromJSON({ id: taskIdOf(first) }))) {
      sub.push(ev.payload.$case);
    }
    for (;;) { if ((await it.next()).done) break; }
    return sub;
  });

  for (const [prompt, want] of [["fail", "TASK_STATE_FAILED"], ["interrupt", "TASK_STATE_INPUT_REQUIRED"]]) {
    await run(P + `stream.${prompt}`, async () => {
      const events = [];
      for await (const ev of client.sendMessageStream(req(prompt))) events.push(ev);
      const st = lastState(events) === "task" ? stateName(events[events.length - 1].payload.value.status.state)
                                               : lastState(events);
      if (st !== want) throw new Error(`ended ${st}, expected ${want}`);
      return st;
    });
  }

  await run(P + "SendMessage.terminal_task->UnsupportedOperationError", async () => {
    const r = await client.sendMessage(req("echo one"));
    const t = r.payload ? r.payload.value : r;
    try {
      const again = await client.sendMessage(req("echo two", { taskId: t.id, contextId: t.contextId }));
      const t2 = again.payload ? again.payload.value : again;
      throw new Error(`no error; NeoGraph re-ran the terminal task -> ${stateName(t2.status.state)}`);
    } catch (e) {
      if (/UnsupportedOperation/.test(e.constructor.name)) return e.message;
      throw e;
    }
  });
}

(async () => {
  for (const d of ["1.0", "0.3"]) {
    try {
      await scenarios(d);
    } catch (e) {
      report(`[${d}] setup`, false, { error: `${e.constructor.name}: ${e.message}` });
    }
  }
  process.exit(failures ? 1 : 0);
})();
