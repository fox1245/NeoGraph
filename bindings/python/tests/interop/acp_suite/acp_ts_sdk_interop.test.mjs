// Official ACP TypeScript SDK (@agentclientprotocol/sdk) -> native NeoGraph ACPServer.
//
//   ACP_TS_SDK_DIR=<node_modules/@agentclientprotocol/sdk> \
//   NEOGRAPH_ACP_AGENT=<neograph_acp_probe_agent> node --test acp_ts_sdk_interop.test.mjs
//
// Driven by test_acp_sdk_ts_interop.py, which also validates every transcript
// against the vendored ACP schema. The SDK version is pinned in package.json.
//
// The SDK validates every agent->client request and notification with its zod
// schemas; responses to client requests are additionally checked here with the
// SDK's own zod schemas (zInitializeResponse, zNewSessionResponse, ...). A check
// passes only when the SDK raised nothing and printed no console error/warning.
// Transcripts go to ACP_TRANSCRIPT_DIR (default ./transcripts/ts-<version>).

import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { Readable, Writable } from "node:stream";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const SDK_DIR = process.env.ACP_TS_SDK_DIR;
const AGENT = process.env.NEOGRAPH_ACP_AGENT;
const PYTHON = process.env.PYTHON || "python3";
const skip = !SDK_DIR || !AGENT || !existsSync(AGENT) ? "set ACP_TS_SDK_DIR and NEOGRAPH_ACP_AGENT" : false;

const acp = skip ? null : await import(pathToFileURL(join(SDK_DIR, "dist/acp.js")).href);
const zod = skip ? null : await import(pathToFileURL(join(SDK_DIR, "dist/schema/zod.gen.js")).href);
const SDK_VERSION = skip ? "none" : JSON.parse(readFileSync(join(SDK_DIR, "package.json"), "utf8")).version;
const TRANSCRIPTS = process.env.ACP_TRANSCRIPT_DIR || join(HERE, "transcripts", `ts-${SDK_VERSION}`);

function captureConsole() {
  const complaints = [];
  const original = { error: console.error, warn: console.warn };
  console.error = (...args) => complaints.push(["error", args.map(String).join(" ")]);
  console.warn = (...args) => complaints.push(["warn", args.map(String).join(" ")]);
  return { complaints, restore: () => Object.assign(console, original) };
}

function wire(transcript, dir = "a2c", contains) {
  if (!existsSync(transcript)) return "<no transcript>";
  const rows = readFileSync(transcript, "utf8").trim().split("\n").filter(Boolean).map((l) => JSON.parse(l));
  return "\n    " + rows.filter((r) => r.dir === dir && (!contains || r.raw.includes(contains)))
    .slice(-6).map((r) => r.raw).join("\n    ");
}

// Runs one scenario against a fresh agent process.
async function withAgent(name, { clientCapabilities = {}, protocolVersion, handlers = {} } = {}, op) {
  mkdirSync(TRANSCRIPTS, { recursive: true });
  const transcript = join(TRANSCRIPTS, `${name}.jsonl`);
  rmSync(transcript, { force: true });
  const child = spawn(PYTHON, ["-I", join(HERE, "acp_stdio_tap.py"), transcript, "--", AGENT],
    { stdio: ["pipe", "pipe", "ignore"] });
  const stream = acp.ndJsonStream(Writable.toWeb(child.stdin), Readable.toWeb(child.stdout));
  const seen = { updates: [], permission: [], reads: [], writes: [], terminal: [] };
  const console_ = captureConsole();
  let app = acp.client({ name: "acp-ts-interop-probe" })
    .onNotification(acp.methods.client.session.update, (ctx) => { seen.updates.push(ctx.params); })
    .onRequest(acp.methods.client.session.requestPermission, async (ctx) => {
      seen.permission.push(ctx.params);
      if (handlers.permission) return handlers.permission(ctx);
      const allow = ctx.params.options.find((o) => o.kind.startsWith("allow"));
      return { outcome: { outcome: "selected", optionId: allow.optionId } };
    })
    .onRequest(acp.methods.client.fs.readTextFile, async (ctx) => {
      seen.reads.push(ctx.params);
      return { content: "ts file content" };
    })
    .onRequest(acp.methods.client.fs.writeTextFile, async (ctx) => {
      seen.writes.push(ctx.params);
      return {};
    })
    .onRequest(acp.methods.client.terminal.create, async (ctx) => {
      seen.terminal.push(ctx.params);
      throw new acp.RequestError(-32601, "terminal not supported");
    });
  try {
    return await app.connectWith(stream, async (ctx) => {
      const init = await ctx.request(acp.methods.agent.initialize, {
        protocolVersion: protocolVersion ?? acp.PROTOCOL_VERSION,
        clientCapabilities,
        clientInfo: { name: "acp-ts-interop-probe", version: "1.0.0" },
      });
      zod.zInitializeResponse.parse(init);
      return await op({ ctx, init, seen, transcript, complaints: console_.complaints });
    });
  } finally {
    console_.restore();
    child.stdin.end();
    await new Promise((r) => { child.once("exit", r); setTimeout(() => { child.kill(); r(); }, 3000); });
  }
}

async function newSession(ctx) {
  const resp = await ctx.request(acp.methods.agent.session.new, { cwd: "/tmp", mcpServers: [] });
  zod.zNewSessionResponse.parse(resp);
  return resp.sessionId;
}

async function promptText(ctx, sessionId, text) {
  const resp = await ctx.request(acp.methods.agent.session.prompt,
    { sessionId, prompt: [{ type: "text", text }] });
  zod.zPromptResponse.parse(resp);
  return resp;
}

const texts = (seen) => seen.updates.filter((u) => u.update.sessionUpdate === "agent_message_chunk")
  .map((u) => u.update.content.text);

test("initialize: v1 negotiated and accepted by zInitializeResponse", { skip }, async () => {
  await withAgent("initialize", {}, async ({ init, complaints, transcript }) => {
    assert.equal(init.protocolVersion, 1, wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});

test("initialize: a v2 client is answered with v1", { skip }, async () => {
  await withAgent("initialize_v2", { protocolVersion: 2 }, async ({ init, transcript }) => {
    assert.equal(init.protocolVersion, 1, wire(transcript));
  });
});

test("session/new + session/prompt via ActiveSession streams one chunk then end_turn", { skip }, async () => {
  await withAgent("active_session_prompt", {}, async ({ ctx, complaints, transcript }) => {
    const result = await ctx.buildSession("/tmp").withSession(async (session) => {
      session.prompt("echo from ts");
      const chunks = [];
      for (;;) {
        const message = await session.nextUpdate();
        if (message.kind === "stop") return { chunks, stop: message.response };
        chunks.push(message.update);
      }
    });
    zod.zPromptResponse.parse(result.stop);
    assert.equal(result.stop.stopReason, "end_turn", wire(transcript));
    assert.deepEqual(result.chunks.map((c) => c.content.text), ["[probe] from ts"], wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});

test("session/request_permission is accepted by zRequestPermissionRequest", { skip }, async () => {
  await withAgent("permission", {}, async ({ ctx, seen, complaints, transcript }) => {
    const sid = await newSession(ctx);
    const resp = await promptText(ctx, sid, "permission");
    assert.equal(resp.stopReason, "end_turn", wire(transcript));
    assert.equal(seen.permission.length, 1, wire(transcript));
    assert.deepEqual(texts(seen), ["permission:selected:allow"], wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});

test("fs/read_text_file and fs/write_text_file with capability", { skip }, async () => {
  const caps = { fs: { readTextFile: true, writeTextFile: true } };
  await withAgent("fs", { clientCapabilities: caps }, async ({ ctx, seen, complaints, transcript }) => {
    const sid = await newSession(ctx);
    await promptText(ctx, sid, "fsread /tmp/a.txt");
    await promptText(ctx, sid, "fswrite /tmp/b.txt");
    assert.deepEqual(seen.reads, [{ sessionId: sid, path: "/tmp/a.txt" }], wire(transcript));
    assert.deepEqual(seen.writes, [{ sessionId: sid, path: "/tmp/b.txt", content: "probe-content" }], wire(transcript));
    assert.deepEqual(texts(seen), ["read:ts file content", "written"], wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});

test("session/cancel resolves the prompt with stopReason cancelled", { skip }, async () => {
  await withAgent("cancel", {}, async ({ ctx, complaints, transcript }) => {
    const sid = await newSession(ctx);
    const pending = promptText(ctx, sid, "slow");
    await new Promise((r) => setTimeout(r, 500));
    await ctx.notify(acp.methods.agent.session.cancel, { sessionId: sid });
    const resp = await pending;
    assert.equal(resp.stopReason, "cancelled", wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});

test("cancel while session/request_permission is pending", { skip }, async () => {
  let ctxRef;
  const handlers = {
    permission: async (pctx) => {
      await ctxRef.notify(acp.methods.agent.session.cancel, { sessionId: pctx.params.sessionId });
      return { outcome: { outcome: "cancelled" } };
    },
  };
  await withAgent("cancel_permission", { handlers }, async ({ ctx, transcript }) => {
    ctxRef = ctx;
    const sid = await newSession(ctx);
    const resp = await promptText(ctx, sid, "permission");
    assert.equal(resp.stopReason, "cancelled", wire(transcript));
  });
});

test("session/resume answer is accepted by zResumeSessionResponse", { skip }, async () => {
  await withAgent("resume", {}, async ({ ctx, init, transcript }) => {
    assert.ok(init.agentCapabilities.sessionCapabilities?.resume, wire(transcript));
    const sid = await newSession(ctx);
    await promptText(ctx, sid, "echo first");
    const resp = await ctx.request(acp.methods.agent.session.resume, { sessionId: sid, cwd: "/tmp", mcpServers: [] });
    zod.zResumeSessionResponse.parse(resp);
  });
});

test("error codes stay within ACP ErrorCode", { skip }, async () => {
  await withAgent("error_codes", {}, async ({ ctx, transcript }) => {
    const codeOf = async (p) => p.then(() => "result", (e) => e.code);
    const unknownPrompt = await codeOf(ctx.request(acp.methods.agent.session.prompt,
      { sessionId: "missing", prompt: [{ type: "text", text: "x" }] }));
    const unknownResume = await codeOf(ctx.request(acp.methods.agent.session.resume,
      { sessionId: "missing", cwd: "/tmp", mcpServers: [] }));
    const load = await codeOf(ctx.request(acp.methods.agent.session.load,
      { sessionId: "missing", cwd: "/tmp", mcpServers: [] }));
    assert.equal(unknownPrompt, -32002, wire(transcript));
    assert.equal(load, -32601, wire(transcript));
    assert.equal(unknownResume, -32002, "session/resume of an unknown session" + wire(transcript, "a2c", '"error"'));
  });
});

test("failed graph turn is a JSON-RPC error without exception text", { skip }, async () => {
  await withAgent("failure", {}, async ({ ctx, seen, transcript }) => {
    const sid = await newSession(ctx);
    let error;
    try {
      const resp = await promptText(ctx, sid, "fail");
      assert.fail(`stopReason=${resp.stopReason}, text=${JSON.stringify(texts(seen))}` + wire(transcript));
    } catch (e) {
      error = e;
    }
    assert.equal(error.code, -32603, wire(transcript));
    assert.deepEqual(error.data, { code: "NG.TaskFailed", retryable: true }, wire(transcript));
    assert.ok(!JSON.stringify([error.message, error.data]).includes("hunter2"), wire(transcript));
    assert.deepEqual(texts(seen), [], wire(transcript));
  });
});

test("provider refusal ends the turn with stopReason refusal", { skip }, async () => {
  await withAgent("refusal", {}, async ({ ctx, seen, complaints, transcript }) => {
    const sid = await newSession(ctx);
    const resp = await promptText(ctx, sid, "refusal");
    assert.equal(resp.stopReason, "refusal", wire(transcript));
    assert.deepEqual(texts(seen), [], wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});

test("an interrupt asks session/request_permission and resumes in the same turn", { skip }, async () => {
  await withAgent("interrupt", {}, async ({ ctx, seen, complaints, transcript }) => {
    const sid = await newSession(ctx);
    const resp = await promptText(ctx, sid, "interrupt");
    assert.equal(resp.stopReason, "end_turn", wire(transcript));
    assert.equal(seen.permission.length, 1, wire(transcript));
    const ask = seen.permission[0];
    zod.zRequestPermissionRequest.parse(ask);
    assert.equal(ask.toolCall.title, "approval needed", wire(transcript));
    assert.deepEqual(ask.toolCall.rawInput, { tool: "probe" }, wire(transcript));
    assert.deepEqual(texts(seen), ["resumed:approve"], wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});

test("a busy session is not reported as authentication required", { skip }, async () => {
  await withAgent("busy", {}, async ({ ctx, transcript }) => {
    const sid = await newSession(ctx);
    const first = promptText(ctx, sid, "slow");
    await new Promise((r) => setTimeout(r, 300));
    let error;
    try {
      await promptText(ctx, sid, "echo second");
    } catch (e) {
      error = e;
    }
    await ctx.notify(acp.methods.agent.session.cancel, { sessionId: sid });
    await first;
    assert.ok(error, "second prompt on a busy session was accepted" + wire(transcript));
    assert.equal(error.code, -32603, wire(transcript, "a2c", '"error"'));
    assert.equal(error.data && error.data.reason, "session_busy", wire(transcript, "a2c", '"error"'));
  });
});

test("tool calls are tool_call / tool_call_update notifications accepted by zod", { skip }, async () => {
  await withAgent("tool_calls", {}, async ({ ctx, seen, complaints, transcript }) => {
    const sid = await newSession(ctx);
    const resp = await promptText(ctx, sid, "toolcall");
    assert.equal(resp.stopReason, "end_turn", wire(transcript));
    const calls = seen.updates.map((u) => u.update)
      .filter((u) => u.sessionUpdate === "tool_call" || u.sessionUpdate === "tool_call_update");
    for (const u of seen.updates) zod.zSessionNotification.parse(u);
    assert.deepEqual(calls.map((u) => [u.sessionUpdate, u.toolCallId, u.status]), [
      ["tool_call", "call_probe_ok", "in_progress"],
      ["tool_call", "call_probe_fail", "in_progress"],
      ["tool_call_update", "call_probe_ok", "completed"],
      ["tool_call_update", "call_probe_fail", "failed"],
    ], wire(transcript));
    assert.deepEqual(texts(seen), ["tools done"], wire(transcript));
    assert.deepEqual(complaints, [], wire(transcript));
  });
});
