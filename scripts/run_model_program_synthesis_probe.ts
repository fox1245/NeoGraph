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
The source must be a complete NeoGraph QuickJS Program with no markdown fences.

It must use the NeoGraph builder API, not a plain object or canonical graph JSON.
The source must contain the exact synchronous declaration export function define() and its first statement must create const graph = ng.graph("model-synthesized").
Call graph.channel(), graph.node(), graph.entry(), graph.edge(), and graph.exit(), then return that exact graph builder.

Construct exactly one graph using only this reviewed vocabulary:
- channels: value with reducer probe.overwrite and numeric initial 0; path with reducer probe.overwrite and string initial ""
- nodes, in this exact order: seed of type probe.seed, double of type probe.double, finish of type probe.finish
- entry seed, edge seed to double, edge double to finish, exit finish

The channel calls must be exactly graph.channel("value", {reducer: "probe.overwrite", initial: 0}); and graph.channel("path", {reducer: "probe.overwrite", initial: ""});. The reducer value must remain a quoted string and the property name must be initial, not init.
Build the nodes from this exact pair array: [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]].
For each pair call graph.node(name, {type}); the second argument must be an object containing the type property, never a string.
Use iteration only for those three graph.node() calls so this is genuine JavaScript topology authoring rather than copied canonical graph JSON. Do not construct or return an object with channels, nodes, edges, entry, or exit properties.

It must also export function* main(input) and return yield ng.callCore("model-synthesized", input, "model-generated:1").
Do not use imports, eval, Date, randomness, network, filesystem, environment variables, extra graphs, extra nodes, conditions, or authority-bearing commands.`

const response = await fetch("https://openrouter.ai/api/v1/chat/completions", {
  method: "POST",
  headers: {
    authorization: `Bearer ${apiKey}`,
    "content-type": "application/json",
    "http-referer": "https://github.com/fox1245/NeoGraph",
    "x-title": "NeoGraph model Program synthesis probe",
  },
  body: JSON.stringify({
    model,
    temperature: 0,
    response_format: { type: "json_object" },
    messages: [
      {
        role: "system",
        content:
          "You synthesize bounded NeoGraph QuickJS Program source. The host compiler and admission policy are authoritative.",
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
  throw new Error(`Program synthesis probe failed (${exitCode})`)
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
