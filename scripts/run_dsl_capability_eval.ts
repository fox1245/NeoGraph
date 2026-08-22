import { parseArgs } from "util"
import { join } from "path"

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
    model: { type: "string", default: "deepseek/deepseek-v4-flash-0731" },
    manifest: { type: "string" },
    case: { type: "string" },
    attempts: { type: "string", default: "1" },
    "repair-attempts": { type: "string", default: "0" },
    output: { type: "string" },
  },
})

const probe = args.values.probe
if (!probe) throw new Error("--probe is required")
const apiKey = process.env.OPENROUTER_API_KEY
if (!apiKey) throw new Error("OPENROUTER_API_KEY is required")
const model = args.values.model!
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

async function generate(
  capability: CapabilityCase,
  correction?: { source: string; evidence: unknown },
) {
  const correctionPrompt = correction
    ? `\n\nThe previous full source was rejected by the authoritative probe. Correct it and return the complete replacement source, not a patch.\nPrevious source:\n${correction.source}\nProbe evidence:\n${JSON.stringify(correction.evidence)}`
    : ""
  const response = await fetch("https://openrouter.ai/api/v1/chat/completions", {
    method: "POST",
    headers: {
      authorization: `Bearer ${apiKey}`,
      "content-type": "application/json",
      "http-referer": "https://github.com/fox1245/NeoGraph",
      "x-title": "NeoGraph DSL capability synthesis eval",
    },
    body: JSON.stringify({
      model,
      temperature: 0,
      response_format: { type: "json_object" },
      messages: [
        {
          role: "system",
          content:
            "You author bounded NeoGraph QuickJS Programs. Return one JSON object with exactly one string field named source. The host compiler is authoritative.",
        },
        {
          role: "user",
          content: `${apiReference}\n\nCapability case ${capability.id}:\n${capability.request}${correctionPrompt}`,
        },
      ],
    }),
    signal: AbortSignal.timeout(120_000),
  })
  const body = (await response.json()) as Record<string, any>
  if (!response.ok)
    throw new Error(`OpenRouter request failed (${response.status}): ${JSON.stringify(body)}`)
  const content = body.choices?.[0]?.message?.content
  if (typeof content !== "string") throw new Error("model response did not contain text content")
  return {
    responseID: body.id as string | undefined,
    finishReason: body.choices?.[0]?.finish_reason as string | undefined,
    source: extractSource(content),
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
        finishReason: generated.finishReason,
        generatedSource: generated.source,
        probeExitCode: probeResult.exitCode,
        probe: probeResult.evidence,
      }]
      let repairsUsed = 0
      let repairError: string | undefined
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
          process.stderr.write(`[${capability.id}] repair generation error\n`)
          break
        }
        probeResult = await runProbe(capability, generated.source)
        turns.push({
          turn: repairsUsed,
          responseID: generated.responseID,
          finishReason: generated.finishReason,
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
        turns,
      })
      process.stderr.write(
        `[${capability.id}] ${probeResult.exitCode === 0 ? "passed" : `probe rejected (${probeResult.exitCode})`}\n`,
      )
    } catch (error) {
      results.push({
        ...base,
        status: "generation_error",
        error: error instanceof Error ? error.message : String(error),
      })
      process.stderr.write(`[${capability.id}] generation error\n`)
    }
  }
}

const passed = results.filter((value) => value.status === "passed").length
const report = {
  schemaVersion: 1,
  provider: "openrouter",
  model,
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
