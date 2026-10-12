// Official @a2a-js/sdk (1.x) A2A JSON-RPC server with a deterministic stub
// executor, used as the reference peer for NeoGraph's A2A client.
//
// Usage: node ts_server.cjs <port> <sdk-dist-dir> <express-dir> [--compat03] [--record FILE]
// Behaviour by first word: echo | message | fail | interrupt | slow (see sdk_py_server.py).
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const [, , PORT, DIST, EXPRESS, ...rest] = process.argv;
const compat = rest.includes("--compat03");
const recIdx = rest.indexOf("--record");
const RECORD = recIdx >= 0 ? rest[recIdx + 1] : "";

const sdk = require(path.join(DIST, "index.cjs"));
const srv = require(path.join(DIST, "server", "index.cjs"));
const { jsonRpcHandler, agentCardHandler, UserBuilder } = require(path.join(DIST, "server", "express", "index.cjs"));
const express = require(EXPRESS);

const url = `http://127.0.0.1:${PORT}/`;
const interfaces = [{ url, protocolBinding: "JSONRPC", protocolVersion: "1.0" }];
if (compat) interfaces.push({ url, protocolBinding: "JSONRPC", protocolVersion: "0.3" });
const card = sdk.AgentCard.fromJSON({
  name: "ts-sdk-stub", description: "a2a-js reference server for NeoGraph client probes", version: "1.0.0",
  supportedInterfaces: interfaces, capabilities: { streaming: true },
  defaultInputModes: ["text/plain"], defaultOutputModes: ["text/plain"],
  skills: [{ id: "stub", name: "stub", description: "stub skill", tags: ["stub"] }],
});

const cancelled = new Set();
const text = (m) => (m.parts || []).map((p) => (p.content && p.content.$case === "text" ? p.content.value : "")).join(" ");
const agentMsg = (s, taskId, contextId) => sdk.Message.fromJSON({
  messageId: `ts-srv-${Date.now()}-${Math.random()}`, role: "ROLE_AGENT", parts: [{ text: s }], taskId, contextId,
});
const status = (taskId, contextId, state, s) => srv.AgentEvent.statusUpdate(sdk.TaskStatusUpdateEvent.fromJSON({
  taskId, contextId, status: { state, message: s ? sdk.Message.toJSON(agentMsg(s, taskId, contextId)) : undefined },
}));

const executor = {
  async execute(ctx, bus) {
    const t = text(ctx.userMessage);
    const word = t.split(/\s+/)[0];
    const { taskId, contextId } = ctx;
    if (word === "message") {
      bus.publish(srv.AgentEvent.message(agentMsg(`direct reply to ${t}`, undefined, contextId)));
      bus.finished();
      return;
    }
    if (!ctx.task) {
      bus.publish(srv.AgentEvent.task(sdk.Task.fromJSON({
        id: taskId, contextId, status: { state: "TASK_STATE_SUBMITTED" },
        history: [sdk.Message.toJSON(ctx.userMessage)],
      })));
    } else {
      bus.publish(srv.AgentEvent.task(ctx.task));
    }
    bus.publish(status(taskId, contextId, "TASK_STATE_WORKING", "working"));
    if (word === "fail") bus.publish(status(taskId, contextId, "TASK_STATE_FAILED", "stub failure"));
    else if (word === "interrupt") bus.publish(status(taskId, contextId, "TASK_STATE_INPUT_REQUIRED", "need input"));
    else {
      if (word === "slow") {
        for (let i = 0; i < 30 && !cancelled.has(taskId); i++) await new Promise((r) => setTimeout(r, 100));
        if (cancelled.has(taskId)) { bus.finished(); return; }
      }
      bus.publish(srv.AgentEvent.artifactUpdate(sdk.TaskArtifactUpdateEvent.fromJSON({
        taskId, contextId, lastChunk: true,
        artifact: { artifactId: `${taskId}-out`, parts: [{ text: `artifact for ${t}`, mediaType: "text/plain" }] },
      })));
      bus.publish(status(taskId, contextId, "TASK_STATE_COMPLETED", `done: ${t}`));
    }
    bus.finished();
  },
  async cancelTask(taskId, bus) {
    cancelled.add(taskId);
    bus.publish(status(taskId, "", "TASK_STATE_CANCELED"));
    bus.finished();
  },
};

const handler = new srv.DefaultRequestHandler(card, new srv.InMemoryTaskStore(), executor);
const app = express();
app.use((req, _res, next) => {
  if (!RECORD) return next();
  let body = "";
  req.on("data", (c) => { body += c; });
  req.on("end", () => {
    fs.appendFileSync(RECORD, JSON.stringify({ method: req.method, path: req.path, headers: req.headers, body }) + "\n");
  });
  next();
});
app.use("/.well-known/agent-card.json", agentCardHandler({
  agentCardProvider: async () => card, ...(compat ? { legacyCompat: { enabled: true } } : {}),
}));
app.use("/", jsonRpcHandler({
  requestHandler: handler, userBuilder: UserBuilder.noAuthentication,
  ...(compat ? { legacyCompat: { enabled: true } } : {}),
}));
app.listen(Number(PORT), "127.0.0.1", () => console.log(`READY ${PORT}`));
