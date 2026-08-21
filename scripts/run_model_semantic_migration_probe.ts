import { parseArgs } from "util"

const args = parseArgs({
  args: process.argv.slice(2),
  options: {
    probe: { type: "string" },
    model: { type: "string", default: "deepseek/deepseek-v4-flash-0731" },
  },
})

const probe = args.values.probe
const model = args.values.model!
if (!probe) throw new Error("--probe is required")
const apiKey = process.env.OPENROUTER_API_KEY
if (!apiKey) throw new Error("OPENROUTER_API_KEY is required")

const prompt = `Return one JSON object with exactly one string field named source.
The source must be a complete NeoGraph QuickJS declaration-only Program with no markdown fences.

It must export exactly one synchronous function named define() and MUST NOT export main(), imports, or any runtime control command. The host owns migration, admission, and execution.
The first statement in define() must be const graph = ng.graph("main").

Construct exactly one graph using only this reviewed vocabulary:
- channel: value with reducer semantic.overwrite and string initial ""
- nodes in this exact order: work with {type: "semantic.short-blocking", migration_epoch: 2}; followup with {type: "semantic.followup"}
- entry work, edge work to followup, exit followup

Use the exact calls:
graph.channel("value", {reducer: "semantic.overwrite", initial: ""});
graph.node("work", {type: "semantic.short-blocking", migration_epoch: 2});
graph.node("followup", {type: "semantic.followup"});
graph.entry("work");
graph.edge("work", "followup");
graph.exit("followup");

Return the graph builder. Do not emit a plain object/canonical JSON graph. Do not use eval, Date, randomness, network, filesystem, environment variables, extra graphs, conditions, tools, spawning, or authority-bearing commands.`

const response = await fetch("https://openrouter.ai/api/v1/chat/completions", {
  method: "POST",
  headers: {
    authorization: `Bearer ${apiKey}`,
    "content-type": "application/json",
    "http-referer": "https://github.com/fox1245/NeoGraph",
    "x-title": "NeoGraph semantic migration model probe",
  },
  body: JSON.stringify({
    model,
    temperature: 0,
    response_format: { type: "json_object" },
    messages: [
      {
        role: "system",
        content:
          "You synthesize bounded declaration-only NeoGraph topology source. The host compiler, admission policy, and migration adapter are authoritative.",
      },
      { role: "user", content: prompt },
    ],
  }),
  signal: AbortSignal.timeout(120_000),
})

const body = (await response.json()) as Record<string, any>
if (!response.ok) throw new Error(`OpenRouter request failed (${response.status}): ${JSON.stringify(body)}`)
const content = body.choices?.[0]?.message?.content
if (typeof content !== "string") throw new Error("Model response did not contain text content")

let payload: unknown
try {
  payload = JSON.parse(content)
} catch {
  const start = content.indexOf("{")
  const end = content.lastIndexOf("}")
  if (start < 0 || end <= start) throw new Error("Model response was not a JSON object")
  payload = JSON.parse(content.slice(start, end + 1))
}
if (!payload || typeof payload !== "object" || Array.isArray(payload))
  throw new Error("Model response payload must be an object")
const source = (payload as Record<string, unknown>).source
if (typeof source !== "string" || source.trim().length === 0)
  throw new Error("Model response source must be a non-empty string")

const child = Bun.spawn([probe], {
  stdin: new Blob([source]),
  stdout: "pipe",
  stderr: "pipe",
})
const [stdout, stderr, exitCode] = await Promise.all([
  new Response(child.stdout).text(),
  new Response(child.stderr).text(),
  child.exited,
])
if (exitCode !== 0) {
  process.stderr.write(
    `${JSON.stringify(
      {
        model,
        responseID: body.id,
        generatedSource: source,
        probeExitCode: exitCode,
        probeError: stderr.trim() || stdout.trim(),
      },
      null,
      2,
    )}\n`,
  )
  throw new Error(`Semantic migration probe failed (${exitCode})`)
}

const evidence = JSON.parse(stdout.trim()) as Record<string, unknown>
const sourceHash = new Bun.CryptoHasher("sha256").update(source).digest("hex")
process.stdout.write(
  `${JSON.stringify(
    {
      schemaVersion: 1,
      provider: "openrouter",
      model,
      responseID: body.id,
      finishReason: body.choices?.[0]?.finish_reason,
      generatedSourceSha256: sourceHash,
      generatedSource: source,
      probe: evidence,
    },
    null,
    2,
  )}\n`,
)
