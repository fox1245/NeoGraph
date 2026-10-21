# Prompt-cache prefix probe

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

A provider's prompt cache reuses the start of a request that is identical, from its first byte, to an earlier request. NeoGraph's runtime assembles each request from a fixed set of context artifacts and a growing RAW history (see [`docs/STRICT_RUNTIME_INTERPOSITION.md`](../../docs/STRICT_RUNTIME_INTERPOSITION.md), "Artifact placement"). This probe measures how the placement of a small per-turn block changes how much of each request the provider can reuse (issue #329).

## What it does

`prompt_cache_probe.py` runs a conversation of 12 to 20 turns through `RuntimeTurnAssembler` and `ControlledProvider`:

- a long fixed block, a `RequiredSkill` artifact of 4,500 to 5,000 tokens (`--bible-entries`, 90 to 115), so that it is above the cache minimum of the providers;
- a RAW history that grows by a question and a reply every turn;
- a dynamic block of about 400 tokens (`--dynamic-lines`) that changes every turn and is placed by the scenario.

For each provider call it records the usage that the provider reported (input, cache read, cache write and uncached tokens) and how much of the encoded request is identical to the request before it, in bytes of the wire body and in characters of the prompt in reading order (tools, system, messages).

A scenario name is a placement of the dynamic block, optionally followed by `+compact`, which adds a compaction at `--compact-at`: the oldest turns leave the RAW window and a summary of them is added as an artifact. The placements are `none`, `default` (`BeforeLatestUser`), `before-history`, `after-history`, `raw-append` (the block is appended to the RAW feed instead of being an artifact), `system-tail`, `system-head` and `tie-break`. The summary is written by the probe from the recorded questions and replies; it does not call `compact_history`.

## Running it

Build the Python extension, then:

```bash
# No network and no key: only the request-prefix columns are filled.
PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py --offline \
    --preset anthropic.messages --model any-name --turns 14 --compact-at 10 \
    --scenarios none,default,before-history,after-history,raw-append,compaction

# Live: the key is read from the environment variable of the preset.
OPENAI_API_KEY=... PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py \
    --preset openai.chat --model <model> --rates input=...,cache_read=...,output=... \
    --budget-usd 0.05 --turns 16 --scenarios before-history,after-history,compaction
```

- **Presets.** `openai.chat`, `anthropic.messages`, `google.generate` and `openrouter.chat`. Use `--control NAME=VALUE` for a `ProviderControls` field, dotted for a family nest, and `--cache-control 5m` to send the Messages request-level cache control. Gemini needs `--control generate.history_mode=GeminiHistoryMode.PortableForeign`, because the probe records replies as portable text.
- **Cost.** A live run needs `--rates` (USD per million tokens) to bound and report its spend. It refuses to start when its worst case, with no cache hit, is above `--budget-usd`, and stops when the spend passes it. A provider that caches in the background, such as DeepSeek, needs `--pause-ms` of a few seconds between turns.
- **Key.** The probe reads the key from the environment and never prints or stores it.
- **Output.** A table per scenario, a summary, and with `--out` the per-call records as JSON. `--show-order N` prints the message order of call N.
