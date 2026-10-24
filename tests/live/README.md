# Live end-to-end suite

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

Unit and integration tests replay recorded or scripted provider traffic. They cannot show that a provider still accepts what NeoGraph sends, or that NeoGraph still accepts what the provider answers today. Every release therefore passes this suite with real keys, and CI refuses to publish a release that has no passing report for its commit.

## What it covers

- **Python** (`bindings/python/tests/live/` and `bindings/python/tests/test_live_provider.py`):
  - the eight family presets (OpenAI Chat and Responses, Anthropic Messages, Gemini Generate and Interactions, OpenRouter Chat, Responses and Messages), each buffered and streamed, with streamed deltas equal to the final text;
  - Claude 5.5 models;
  - image input;
  - a forced tool call on every preset and a full tool round trip;
  - generated images kept from Gemini Generate;
  - Gemini speech, buffered and streamed, and a Lyria clip, with the Generate response modalities and speech settings;
  - an OpenAI Responses hosted web search, buffered and streamed;
  - the Anthropic Messages controls: thinking modes and effort, tool choice, the temperature rule, prompt caching, and the same controls through OpenRouter;
  - long Anthropic agent runs in a graph (`test_live_anthropic_agent.py`): dependent tool chains, conversations of up to six turns that must recall their first turn, signed and redacted thinking carried through tool rounds and into a later turn, parallel tool calls, tool errors and retries, a person who reviews or approves each tool call, prompt caching over a conversation, Claude 5.5 adaptive thinking, and the same loops streamed, asynchronous and through OpenRouter;
  - typed failures: an invalid key, a cancelled stream, an expired deadline;
  - SchemaProvider endpoints through `Provider` (#531): OpenAI embeddings, OpenRouter embeddings awaited, and OpenAI moderation.
- **C++** (`tests/live_e2e_test.cpp`, target `neograph_live_e2e_tests`):
  - the provider adapter with a Claude 5.5 server tool, generated images and Interactions;
  - SchemaProvider endpoints through `Provider` (#531): OpenAI and OpenRouter embeddings, OpenAI moderation, an Anthropic token count, an OpenAI image, OpenAI speech transcribed back to text (the one live test of the speech endpoint), a Gemini file named in a request, and Anthropic and OpenAI batches from submission to cancellation;
  - SchemaProvider realtime transcription sessions at OpenAI and Google, fed with speech that an OpenAI Realtime session reads aloud, each committing one user turn with the words spoken;
  - the model catalogue refreshed from all four providers' listings, a call to a model that only the refreshed catalogue lists, and a model that no catalogue lists, which is still sent;
- **Known gaps** of the release line are cases that must fail (strict xfail). `tests/live/manifest.json` lists them with the issue that fixes them. When a fix lands, the case starts to pass and the manifest has to change with it.

Each call uses a cheap model and a small cap. A full run takes about eight minutes and costs about 30 US cents, most of it the Anthropic agent runs.

## Running it

Build the Python extension and the C++ tests of the commit you want to check, then:

```bash
python scripts/live_e2e.py run --env-file path/to/keys.env \
    --python path/to/python --pythonpath path/to/build \
    --cxx path/to/build/tests/neograph_live_e2e_tests
```

- **Keys.** The env file holds `OPENAI_API_KEY`, `GEMINI_API_KEY`, `ANTHROPIC_API_KEY` and `OPENROUTER_API_KEY`. The script passes them to the test processes only. It never prints or stores them, and it replaces any key value that appears in a failure message.
- **Models.** `tests/live/manifest.json` maps each model role (such as `openai.chat`, `anthropic.messages`, or `anthropic.messages.adaptive`) to its default model id under `models`, and `live_support.live_model(role)` resolves it for the Python suite. Google models are called directly with `GEMINI_API_KEY` (`google.generate` and `google.interactions` roles), not through OpenRouter. Setting `NG_LIVE_MODEL_<ROLE>` in the environment or the env file (with `.` and `-` replaced by `_` and uppercased, for example `NG_LIVE_MODEL_OPENAI_CHAT`) overrides that role's model without changing code.
- **Clean tree.** `run` refuses a working tree with uncommitted changes, because the report names a commit.
- **Version.** `run` refuses a `neograph_engine` whose version is not the tree's version.
- **Output.** It writes `tests/live/report.json`: the commit, a hash of the tree, the NeoGraph version, the SchemaProvider pin, and every case with its result.
- **Partial runs.** `--select` and `--cxx-filter` run part of the suite. A partial report never approves a release.

## The release gate

1. The release pull request (a `release/*` branch) commits `tests/live/report.json` from a run on its final commit.
2. The `Live e2e report` workflow runs `python scripts/live_e2e.py verify`, which uses no key. It fails unless all of these hold:
   - the report describes this tree (every tracked file but the report);
   - the report is for this version and comes from a clean tree;
   - every case in the manifest has its expected result: passed, or failed as a listed known gap;
   - no other case failed.
3. The Wheels workflow runs the same check on the release tag before it publishes to PyPI.

When cases are added or removed, `python scripts/live_e2e.py manifest --python … --pythonpath … --cxx …` rewrites the manifest from what the suite collects and keeps the known gaps. Review the difference before committing it.

## On master

After every push to master, the `Live e2e report` workflow also runs the whole suite with real keys. A provider change or a regression then shows up on the day it lands, not at the next release. The run uploads `report.json` as a build artifact. It does not commit the report, and it does not approve a release.

- **Keys.** The four keys are secrets of the `live-e2e` GitHub environment, which only master may use. GitHub shows a secret's value to no one, masks it in logs, and gives it to no pull request workflow. The job writes the keys to a file that only the runner's user can read, and deletes the file when the job ends.
- **One run at a time.** A run on master is never cancelled midway. A push that arrives during a run waits, and only the newest waiting push runs next.
