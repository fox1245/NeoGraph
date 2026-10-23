import { parseArgs } from "util"
import { dirname, join } from "path"

type CapabilityCase = {
  id: string
  classification: string
  features: string[]
  fixture: string
  request: string
}

type CapabilityManifest = {
  schemaVersion: number
  dslProfile: string
  ngApiVersion: number
  cases: CapabilityCase[]
}

const args = parseArgs({
  args: process.argv.slice(2),
  options: {
    probe: { type: "string" },
    provider: { type: "string", default: "openrouter" },
    model: { type: "string", default: "z-ai/glm-5.3-flash" },
    skill: { type: "string" },
    manifest: { type: "string" },
    case: { type: "string" },
    attempts: { type: "string", default: "1" },
    "repair-attempts": { type: "string", default: "0" },
    "reasoning-effort": { type: "string" },
    temperature: { type: "string" },
    "provider-retries": { type: "string", default: "0" },
    "dry-run": { type: "boolean", default: false },
    output: { type: "string" },
  },
})

const probe = args.values.probe
if (!probe) throw new Error("--probe is required")
const providerName = args.values.provider!
if (providerName !== "openrouter" && providerName !== "anthropic")
  throw new Error("--provider must be openrouter or anthropic")
const dryRun = args.values["dry-run"]!
// The key comes from the environment only; a dry run never reads it, so it can build requests anywhere.
const keyVariable = providerName === "anthropic" ? "ANTHROPIC_API_KEY" : "OPENROUTER_API_KEY"
const apiKey = dryRun ? "" : (process.env[keyVariable] ?? "")
if (!dryRun && !apiKey) throw new Error(`${keyVariable} is required`)
const model = args.values.model!
const effort = args.values["reasoning-effort"]
if (effort && !["none", "minimal", "low", "medium", "high", "xhigh", "max"].includes(effort))
  throw new Error("unsupported --reasoning-effort")
if (effort && providerName === "anthropic")
  throw new Error("--reasoning-effort is an OpenRouter setting and is not sent to the Anthropic API")
// OpenRouter keeps its historical temperature 0. The Anthropic API is sent no temperature unless one is
// requested, because some current models reject the parameter and sample at the provider default.
const requestedTemperature = args.values.temperature
const temperature = requestedTemperature === undefined
  ? (providerName === "anthropic" ? undefined : 0)
  : Number(requestedTemperature)
if (temperature !== undefined && (!Number.isFinite(temperature) || temperature < 0 || temperature > 2))
  throw new Error("--temperature must be a number from 0 through 2")
const providerRetries = Number.parseInt(args.values["provider-retries"]!, 10)
if (!Number.isSafeInteger(providerRetries) || providerRetries < 0 || providerRetries > 8)
  throw new Error("--provider-retries must be an integer from 0 through 8")
const maxCompletionTokens = 4096
// What the report records as the sampling configuration; each provider sends its own request shape.
const generationSettings = providerName === "anthropic"
  ? { ...(temperature === undefined ? {} : { temperature }), max_tokens: maxCompletionTokens }
  : {
      temperature,
      max_completion_tokens: maxCompletionTokens,
      provider: { zdr: true },
      response_format: { type: "json_object" },
      ...(effort ? { reasoning_effort: effort } : {}),
    }
const skillPath = args.values.skill ?? join(import.meta.dir, "..", "skills", "neograph-harness-authoring", "SKILL.md")
const skillText = await Bun.file(skillPath).text()
const authoringReference = await Bun.file(join(dirname(skillPath), "references", "quickjs-authoring.md")).text()
const guidance = `${skillText}\n\n${authoringReference}`
if (Buffer.byteLength(guidance, "utf8") > 32768) throw new Error("authoring guidance exceeds 32 KiB")
const skillSha256 = new Bun.CryptoHasher("sha256").update(guidance).digest("hex")
const systemPrompt = "Active host surface: JavaScript source authoring through the capability evaluation bridge. Return one JSON object with exactly one string field named source. The host runs the compiler and returns diagnostics; do not invent tool calls. The supplied native manifest and case contract are authoritative.\n\n" + guidance
class ModelOutputError extends Error {
  constructor(message: string, readonly sample: Record<string, unknown>) { super(message) }
}
const attempts = Number.parseInt(args.values.attempts!, 10)
if (!Number.isSafeInteger(attempts) || attempts < 1 || attempts > 10)
  throw new Error("--attempts must be an integer from 1 through 10")
const repairAttempts = Number.parseInt(args.values["repair-attempts"]!, 10)
if (!Number.isSafeInteger(repairAttempts) || repairAttempts < 0 || repairAttempts > 5)
  throw new Error("--repair-attempts must be an integer from 0 through 5")

const defaultManifest = join(import.meta.dir, "..", "tests", "fixtures", "dsl_capabilities", "cases.json")
const manifestPath = args.values.manifest ?? defaultManifest
const manifest = (await Bun.file(manifestPath).json()) as CapabilityManifest
if (manifest.schemaVersion !== 1 || !Array.isArray(manifest.cases))
  throw new Error("unsupported DSL capability manifest")

const selected = new Set(
  (args.values.case ?? "")
    .split(",")
    .map((value) => value.trim())
    .filter(Boolean),
)
const cases = selected.size === 0
  ? manifest.cases
  : manifest.cases.filter((value) => selected.has(value.id))
if (selected.size > 0 && cases.length !== selected.size) {
  const known = new Set(cases.map((value) => value.id))
  throw new Error(`unknown capability case(s): ${[...selected].filter((value) => !known.has(value)).join(", ")}`)
}

async function loadNativeCapabilityManifest() {
  const child = Bun.spawn([probe!, "--manifest"], { stdout: "pipe", stderr: "pipe" })
  const [stdout, stderr, exitCode] = await Promise.all([
    new Response(child.stdout).text(),
    new Response(child.stderr).text(),
    child.exited,
  ])
  if (exitCode !== 0)
    throw new Error(`native capability manifest failed (${exitCode}): ${stderr.trim()}`)
  return JSON.parse(stdout) as Record<string, any>
}

const nativeManifest = await loadNativeCapabilityManifest()
if (nativeManifest.ng_api_version !== manifest.ngApiVersion ||
    nativeManifest.javascript_profile !== manifest.dslProfile)
  throw new Error("evaluation cases do not match the installed JavaScript capability profile")
const graphSignatures = nativeManifest.define.graph_builder_methods
  .map((value: Record<string, unknown>) => value.signature)
  .join(", ")
const commandSignatures = nativeManifest.main.command_methods
  .map((value: Record<string, unknown>) => `  ng.${value.signature}`)
  .join("\n")
const constraints = nativeManifest.constraints
  .map((value: string) => `- ${value}`)
  .join("\n")
const apiReference = `NeoGraph QuickJS authoring contract (native manifest schema ${nativeManifest.schema_version}):
- Source entry: ${nativeManifest.define.entry}, returning exactly one open builder from ng.graph("capability").
- Graph builder methods: ${graphSignatures}.
- Program entry when requested: ${nativeManifest.main.entry}. Exact command signatures:
${commandSignatures}
- Structured options support max_in_flight and failure_policy. The generic join mode is all, race, or quorum; quorum option key is required_successes.
- Minimal capability graph means one work node of type probe.node, with work as entry and exit.
- Available registry identifiers are node types probe.node, probe.dynamic-send, probe.dynamic-interrupt; reducer probe.overwrite; condition probe.route.
${constraints}
- Do not return canonical graph JSON, CommonJS, require(), module.exports, or invented APIs. Do not include Markdown fences.`

function extractSource(content: string): string {
  let payload: unknown
  try {
    payload = JSON.parse(content)
  } catch {
    const start = content.indexOf("{")
    const end = content.lastIndexOf("}")
    if (start < 0 || end <= start) throw new Error("model response was not a JSON object")
    payload = JSON.parse(content.slice(start, end + 1))
  }
  if (!payload || typeof payload !== "object" || Array.isArray(payload))
    throw new Error("model response payload must be an object")
  const keys = Object.keys(payload)
  if (keys.length !== 1 || keys[0] !== "source")
    throw new Error("model response must contain exactly the source field")
  const source = (payload as Record<string, unknown>).source
  if (typeof source !== "string" || source.trim().length === 0)
    throw new Error("model response source must be a non-empty string")
  return source
}

const scrub = (text: string) => (apiKey ? text.split(apiKey).join("[REDACTED]") : text)

type ProviderRequest = { url: string; headers: Record<string, string>; body: Record<string, unknown> }

// One pure function builds the request for both a live call and --dry-run, so a dry run shows exactly what is sent.
function buildRequest(capability: CapabilityCase, correction?: { source: string; evidence: unknown }): ProviderRequest {
  const correctionPrompt = correction
    ? `\n\nThe previous full source was rejected by the authoritative probe. Correct it and return the complete replacement source, not a patch.\nPrevious source:\n${correction.source}\nProbe evidence:\n${JSON.stringify(correction.evidence)}`
    : ""
  const userContent = `${apiReference}\n\nCapability case ${capability.id}:\n${capability.request}${correctionPrompt}`
  if (providerName === "anthropic") {
    return {
      url: "https://api.anthropic.com/v1/messages",
      headers: {
        "x-api-key": apiKey,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
      },
      body: {
        model,
        ...generationSettings,
        system: systemPrompt,
        messages: [{ role: "user", content: userContent }],
      },
    }
  }
  return {
    url: "https://openrouter.ai/api/v1/chat/completions",
    headers: {
      authorization: `Bearer ${apiKey}`,
      "content-type": "application/json",
      "http-referer": "https://github.com/fox1245/NeoGraph",
      "x-title": "NeoGraph DSL capability synthesis eval",
    },
    body: {
      model,
      ...generationSettings,
      messages: [
        { role: "system", content: systemPrompt },
        { role: "user", content: userContent },
      ],
    },
  }
}

// What a provider response contributes to the evidence, independent of the wire format.
function readResponse(body: Record<string, any>) {
  if (providerName === "anthropic") {
    const blocks = Array.isArray(body.content) ? body.content : []
    const text = blocks.filter((block: any) => block?.type === "text" && typeof block.text === "string")
    return {
      id: body.id as string | undefined,
      upstream: undefined as unknown,
      returnedModel: body.model as string | undefined,
      finishReason: body.stop_reason as string | undefined,
      content: text.length > 0 ? text.map((block: any) => block.text).join("") : undefined,
      usage: body.usage,
    }
  }
  const content = body.choices?.[0]?.message?.content
  return {
    id: body.id as string | undefined,
    upstream: body.provider as unknown,
    returnedModel: body.model as string | undefined,
    finishReason: body.choices?.[0]?.finish_reason as string | undefined,
    content: typeof content === "string" ? content : undefined,
    usage: body.usage,
  }
}

// Every response that carries usage is counted, including repairs and replies whose envelope was unusable.
const usageTotals = { responses: 0, inputTokens: 0, outputTokens: 0 }
function tallyUsage(usage: unknown) {
  const value = (usage ?? {}) as Record<string, unknown>
  const count = (...keys: string[]) =>
    keys.reduce((sum, key) => sum + (typeof value[key] === "number" ? (value[key] as number) : 0), 0)
  usageTotals.responses += 1
  usageTotals.inputTokens += count("input_tokens", "prompt_tokens", "cache_creation_input_tokens", "cache_read_input_tokens")
  usageTotals.outputTokens += count("output_tokens", "completion_tokens")
}

// A provider failure (network, rate limit, overload, rejected request) says nothing about the model.
class ProviderError extends Error {
  retries = 0
  constructor(message: string, readonly retriable: boolean, readonly retryAfterMs?: number) { super(message) }
}

async function sendOnce(request: ProviderRequest) {
  let response: Response
  try {
    response = await fetch(request.url, {
      method: "POST",
      headers: request.headers,
      body: JSON.stringify(request.body),
      signal: AbortSignal.timeout(120_000),
    })
  } catch (error) {
    const detail = error instanceof Error ? `${error.name}: ${error.message}` : String(error)
    throw new ProviderError(`${providerName} request did not complete (${scrub(detail)})`, true)
  }
  let body: Record<string, any>
  try {
    body = JSON.parse(scrub(await response.text())) as Record<string, any>
  } catch {
    body = { error: "non-JSON provider response" }
  }
  if (!response.ok) {
    const retryAfter = Number.parseFloat(response.headers.get("retry-after") ?? "")
    throw new ProviderError(
      `${providerName} request failed (${response.status}): ${JSON.stringify(body)}`,
      [408, 409, 429].includes(response.status) || response.status >= 500,
      Number.isFinite(retryAfter) ? retryAfter * 1000 : undefined,
    )
  }
  return body
}

async function generate(
  capability: CapabilityCase,
  correction?: { source: string; evidence: unknown },
) {
  const request = buildRequest(capability, correction)
  let retries = 0
  let body: Record<string, any>
  while (true) {
    try { body = await sendOnce(request); break }
    catch (error) {
      if (!(error instanceof ProviderError)) throw error
      error.retries = retries
      if (!error.retriable || retries >= providerRetries) throw error
      retries += 1
      const delay = error.retryAfterMs ?? Math.min(60_000, 2_000 * 2 ** (retries - 1)) * (0.75 + Math.random() / 2)
      process.stderr.write(`[${capability.id}] provider failure, retry ${retries}/${providerRetries} in ${Math.round(delay)} ms\n`)
      await new Promise((resolve) => setTimeout(resolve, delay))
    }
  }
  const reply = readResponse(body)
  tallyUsage(reply.usage)
  const content = reply.content
  const sample = {
    responseID: reply.id,
    provider: reply.upstream,
    returnedModel: reply.returnedModel,
    finishReason: reply.finishReason,
    rawContent: typeof content === "string" ? scrub(content) : null,
    usage: reply.usage,
    providerRetries: retries,
  }
  if (typeof content !== "string")
    throw new ModelOutputError("model response did not contain text content", sample)
  let strictEnvelope = false
  try {
    const value = JSON.parse(content)
    strictEnvelope = Boolean(value && !Array.isArray(value) && typeof value === "object" &&
      Object.keys(value).length === 1 && typeof value.source === "string" && value.source.trim().length > 0)
  } catch {}
  try { return { ...sample, strictEnvelope, source: extractSource(content) } }
  catch (error) {
    throw new ModelOutputError(error instanceof Error ? error.message : String(error), sample)
  }
}

async function runProbe(capability: CapabilityCase, source: string) {
  const child = Bun.spawn([probe!, capability.id], {
    stdin: new Blob([source]),
    stdout: "pipe",
    stderr: "pipe",
  })
  const [stdout, stderr, exitCode] = await Promise.all([
    new Response(child.stdout).text(),
    new Response(child.stderr).text(),
    child.exited,
  ])
  let evidence: unknown
  const output = (exitCode === 0 ? stdout : stderr || stdout).trim()
  try {
    evidence = output.length > 0 ? JSON.parse(output) : null
  } catch {
    evidence = { raw: output }
  }
  return { exitCode, evidence }
}

// "provider": no usable reply (network, rate limit, overload, rejected request); "model_output": the model
// replied but not with a usable source envelope. Only the second says something about the model.
function failureKind(error: unknown) {
  return error instanceof ProviderError ? "provider" : error instanceof ModelOutputError ? "model_output" : "other"
}

if (dryRun) {
  const secret = new Set(["authorization", "x-api-key"])
  const show = (label: string, request: ProviderRequest) => ({
    label,
    url: request.url,
    headers: Object.fromEntries(Object.entries(request.headers).map(([name, value]) =>
      [name, secret.has(name.toLowerCase()) ? "<redacted>" : value])),
    body: request.body,
  })
  const first = cases[0]
  if (!first) throw new Error("no capability case selected")
  process.stdout.write(`${JSON.stringify({
    dryRun: true,
    provider: providerName,
    model,
    skillSha256,
    generationSettings,
    requests: [
      show(`${first.id} first turn`, buildRequest(first)),
      show(`${first.id} repair turn`, buildRequest(first, {
        source: "export function define() {}",
        evidence: { error: "example diagnostic" },
      })),
    ],
  }, null, 2)}\n`)
  process.exit(0)
}

const startedAt = new Date().toISOString()
const results: Record<string, unknown>[] = []
for (const capability of cases) {
  for (let attempt = 1; attempt <= attempts; ++attempt) {
    process.stderr.write(`[${capability.id}] attempt ${attempt}/${attempts}\n`)
    const base = {
      case: capability.id,
      classification: capability.classification,
      features: capability.features,
      attempt,
    }
    try {
      let generated = await generate(capability)
      let probeResult = await runProbe(capability, generated.source)
      const turns: Record<string, unknown>[] = [{
        turn: 0,
        responseID: generated.responseID,
        provider: generated.provider,
        returnedModel: generated.returnedModel,
        finishReason: generated.finishReason,
        rawContent: generated.rawContent,
        strictEnvelope: generated.strictEnvelope,
        usage: generated.usage,
        generatedSource: generated.source,
        probeExitCode: probeResult.exitCode,
        probe: probeResult.evidence,
      }]
      let repairsUsed = 0
      let repairError: string | undefined
      let repairFailureKind: string | undefined
      let repairSample: Record<string, unknown> | undefined
      while (probeResult.exitCode !== 0 && repairsUsed < repairAttempts) {
        repairsUsed += 1
        process.stderr.write(`[${capability.id}] repair ${repairsUsed}/${repairAttempts}\n`)
        try {
          generated = await generate(capability, {
            source: generated.source,
            evidence: probeResult.evidence,
          })
        } catch (error) {
          repairError = error instanceof Error ? error.message : String(error)
          repairSample = error instanceof ModelOutputError ? error.sample : undefined
          repairFailureKind = failureKind(error)
          process.stderr.write(`[${capability.id}] repair generation error\n`)
          break
        }
        probeResult = await runProbe(capability, generated.source)
        turns.push({
          turn: repairsUsed,
          responseID: generated.responseID,
          provider: generated.provider,
          returnedModel: generated.returnedModel,
          finishReason: generated.finishReason,
          rawContent: generated.rawContent,
          strictEnvelope: generated.strictEnvelope,
          usage: generated.usage,
          generatedSource: generated.source,
          probeExitCode: probeResult.exitCode,
          probe: probeResult.evidence,
        })
      }
      const sourceHash = new Bun.CryptoHasher("sha256").update(generated.source).digest("hex")
      results.push({
        ...base,
        status: probeResult.exitCode === 0 ? "passed" : "probe_rejected",
        responseID: generated.responseID,
        finishReason: generated.finishReason,
        generatedSourceSha256: sourceHash,
        generatedSource: generated.source,
        probeExitCode: probeResult.exitCode,
        probe: probeResult.evidence,
        repairsUsed,
        repairError,
        repairFailureKind,
        repairSample,
        turns,
      })
      process.stderr.write(
        `[${capability.id}] ${probeResult.exitCode === 0 ? "passed" : `probe rejected (${probeResult.exitCode})`}\n`,
      )
    } catch (error) {
      results.push({
        ...base,
        status: "generation_error",
        failureKind: failureKind(error),
        error: error instanceof Error ? error.message : String(error),
        sample: error instanceof ModelOutputError ? error.sample : undefined,
      })
      process.stderr.write(`[${capability.id}] generation error\n`)
    }
  }
}

const passed = results.filter((value) => value.status === "passed").length
const report = {
  schemaVersion: 1,
  provider: providerName,
  model,
  skillSha256,
  systemPrompt,
  apiReference,
  generationSettings,
  dslProfile: manifest.dslProfile,
  ngApiVersion: manifest.ngApiVersion,
  startedAt,
  finishedAt: new Date().toISOString(),
  summary: {
    cases: cases.length,
    attemptsPerCase: attempts,
    maxRepairAttempts: repairAttempts,
    attempts: results.length,
    passed,
    failed: results.length - passed,
    passRate: results.length === 0 ? 0 : passed / results.length,
    usage: usageTotals,
  },
  results,
}
const encoded = `${JSON.stringify(report, null, 2)}\n`
if (args.values.output) {
  await Bun.write(args.values.output, encoded)
  process.stderr.write(`wrote DSL capability evidence to ${args.values.output}\n`)
}
process.stdout.write(encoded)
if (passed !== results.length) process.exitCode = 1
