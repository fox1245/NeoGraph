// Paired prompt evaluation for the chatbot's internal Harness-selector role.
// No chat runtime is started and no Harness is admitted by this evaluator.
import { parseArgs } from "util"
import { dirname, join } from "path"

const { values } = parseArgs({
  args: process.argv.slice(2),
  options: {
    before: { type: "string" },
    after: { type: "string" },
    cases: { type: "string" },
    output: { type: "string" },
    repeats: { type: "string", default: "2" },
    model: { type: "string", default: "z-ai/glm-5.3-flash" },
    provider: { type: "string" },
  },
})
if (!values.before || !values.after || !values.output)
  throw new Error("--before, --after and --output are required")
const key = process.env.OPENROUTER_API_KEY
if (!key) throw new Error("OPENROUTER_API_KEY is required")
const repeats = Number(values.repeats)
if (!Number.isInteger(repeats) || repeats < 1 || repeats > 3)
  throw new Error("--repeats must be 1..3")
const sha = (text: string) => new Bun.CryptoHasher("sha256").update(text).digest("hex")
const redact = (text: string) => text.split(key).join("[REDACTED]")
const prefix = "Active host surface: chat-template-proposal. Follow only that route in the " +
  "skill below. No model-callable tools are exposed in this mode.\n\n"
const settings = {
  model: values.model,
  temperature: 0.2,
  max_completion_tokens: 4096,
  reasoning_effort: "low",
  response_format: { type: "json_object" },
  provider: { zdr: true, ...(values.provider
    ? { only: [values.provider], allow_fallbacks: false } : {}) },
}
async function packet(path: string) {
  const skill = await Bun.file(path).text()
  const reference = await Bun.file(join(dirname(path), "references", "chat-template-proposals.md")).text()
  const guidance = skill + "\n\n" + reference
  if (Buffer.byteLength(guidance, "utf8") > 32768) throw new Error("guidance exceeds 32 KiB")
  return { system: prefix + guidance, guidanceSha256: sha(guidance) }
}
const arms = { before: await packet(values.before), after: await packet(values.after) }
type Case = { id: string; expectedPlan: string; payload: Record<string, unknown> }
const cases: Case[] = await Bun.file(values.cases ??
  join(import.meta.dir, "..", "tests", "fixtures", "harness_skill_eval", "chat_cases.json")).json()
if (!Array.isArray(cases) || cases.length < 1 || cases.length > 6 ||
    cases.some(c => !c.id || !["direct", "review"].includes(c.expectedPlan) ||
      !c.payload || c.payload.phase !== "evolve" ||
      Buffer.byteLength(JSON.stringify(c.payload), "utf8") > 65536))
  throw new Error("invalid or oversized evaluation cases")

function validate(content: string, expected: string) {
  let p: any
  try { p = JSON.parse(content) }
  catch { return { schemaValid: false, expectedPlan: false, reason: "not_json" } }
  if (!p || Array.isArray(p) || typeof p !== "object" ||
      Object.keys(p).sort().join(",") !== "confidence,plan,reason" ||
      !["direct", "review"].includes(p.plan) || typeof p.reason !== "string" ||
      !p.reason.length || Buffer.byteLength(p.reason, "utf8") > 1000 ||
      typeof p.confidence !== "number" || !Number.isFinite(p.confidence) ||
      p.confidence < 0 || p.confidence > 1)
    return { schemaValid: false, expectedPlan: false, reason: "invalid_fields", proposal: p }
  return { schemaValid: true, expectedPlan: p.plan === expected && p.confidence >= 0.7,
    reason: p.plan !== expected ? "unexpected_plan" : p.confidence < 0.7 ? "low_confidence" : "pass",
    proposal: p }
}
const records: any[] = []
const startedAt = new Date().toISOString()
const totals = () => Object.fromEntries(Object.keys(arms).map(arm => {
  const rows = records.filter(r => r.arm === arm)
  return [arm, { calls: rows.length, schemaValid: rows.filter(r => r.validation?.schemaValid).length,
    expectedPlan: rows.filter(r => r.validation?.expectedPlan).length,
    requestErrors: rows.filter(r => r.error).length,
    reportedTokens: rows.reduce((n, r) => n + (r.usage?.total_tokens ?? 0), 0) }]
}))
async function checkpoint() {
  await Bun.write(values.output!, JSON.stringify({
    schemaVersion: 1, startedAt, updatedAt: new Date().toISOString(), settings,
    repeats, caseCount: cases.length, arms, cases,
    summary: totals(), records,
    scope: "Paired schema/intent evaluation, not runtime admission or proof of answer quality.",
  }, null, 2) + "\n")
}
for (const c of cases) {
  for (let repeat = 0; repeat < repeats; repeat++) {
    // Alternate AB/BA order to reduce a fixed first-arm/caching bias.
    const order = repeat % 2 === 0 ? ["before", "after"] as const : ["after", "before"] as const
    for (const arm of order) {
      const messages = [{ role: "system", content: arms[arm].system },
        { role: "user", content: JSON.stringify(c.payload) }]
      const record: any = { case: c.id, repeat, arm, expectedPlan: c.expectedPlan,
        requestSha256: sha(JSON.stringify({ ...settings, messages })) }
      const started = Date.now()
      try {
        const response = await fetch("https://openrouter.ai/api/v1/chat/completions", {
          method: "POST",
          headers: { authorization: "Bearer " + key, "content-type": "application/json" },
          body: JSON.stringify({ ...settings, messages }),
          signal: AbortSignal.timeout(120000),
        })
        record.httpStatus = response.status
        const text = redact(await response.text())
        let body: any
        try { body = JSON.parse(text) }
        catch { throw new Error("non-JSON provider response: " + text.slice(0, 1000)) }
        record.responseID = body.id
        record.provider = body.provider ?? null
        record.returnedModel = body.model ?? null
        record.usage = body.usage
        record.finishReason = body.choices?.[0]?.finish_reason
        record.rawContent = body.choices?.[0]?.message?.content
        if (!response.ok) throw new Error("HTTP " + response.status + ": " + JSON.stringify(body.error))
        record.validation = typeof record.rawContent === "string"
          ? validate(record.rawContent, c.expectedPlan)
          : { schemaValid: false, expectedPlan: false, reason: "no_text" }
      } catch (error) {
        record.error = redact(error instanceof Error ? error.message : String(error))
      }
      record.elapsedMs = Date.now() - started
      records.push(record)
      await checkpoint()
      process.stderr.write(c.id + " " + arm + " " + (record.validation?.reason ?? "request_error") + "\n")
    }
  }
}
console.log(JSON.stringify({ output: values.output, summary: totals() }))
if (records.some(r => r.error)) process.exitCode = 1
