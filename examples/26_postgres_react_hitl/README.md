# Example 26 — Postgres-backed Deep Research with HITL

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

Demonstrates two NeoGraph features end-to-end:

1. **`PostgresCheckpointStore`** — durable checkpoints in real PostgreSQL
   with channel-blob deduplication.
2. **NodeInterrupt-driven HITL** — the Deep Research graph pauses after
   it produces a report, lets a human review, and resumes either with
   approval (→ end) or with feedback (→ another research round).

The demo is intentionally **process-discontinuous**: the binary exits
after producing the report, so when you `resume` you're a fresh process
that reloads PG state and owner-private native history. PG alone is not
sufficient for typed native resume.

## The scenario

The walkthrough below mirrors the original idea: ask the agent for the
latest Vision Transformer papers, notice the report doesn't cite URLs,
and send it back for citations.

```
$ docker compose run --rm agent run "현재 최신 ViT 관련 논문 알려줘"
=== Postgres HITL Deep Research ===
Thread:  dr-hitl-a1b2c3d4
...
[start] supervisor
[send] fan-out to 2 researcher(s)
[done] researcher
[done] researcher
[start] supervisor
[cmd]  → final_report
[done] final_report
[start] human_review

--- HUMAN REVIEW REQUESTED ---
Awaiting human review of the report. Resume with 'approve' to finalize, or
pass any other text as feedback to trigger another research round.

--- REPORT ---
# Vision Transformer Recent Papers
... <report body> ...

To approve: ./example_postgres_react_hitl resume dr-hitl-a1b2c3d4 approve
To send feedback: ./example_postgres_react_hitl resume dr-hitl-a1b2c3d4 "give me URL citations"
```

The agent process **exits** here — checkpoint is in PG. Now follow up:

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 "give me URL citations"
=== Resuming thread dr-hitl-a1b2c3d4 ===
Feedback: give me URL citations

[start] human_review
[cmd]  → supervisor
[start] supervisor
[send] fan-out to 2 researcher(s)
[done] researcher
[start] supervisor
[cmd]  → final_report
[done] final_report
[start] human_review

--- HUMAN REVIEW REQUESTED (round 2+) ---
... new report with URLs this time ...
```

Approve to finish:

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 approve
--- Final report (approved) ---
... final markdown ...
```

## Verifying it really persisted

Hop into PG and look at the rows — note how `neograph_checkpoint_blobs`
has fewer rows than `channels × checkpoints` thanks to dedup:

```
$ docker compose exec postgres psql -U postgres -d neograph -c "
    SELECT step, current_node, interrupt_phase
    FROM neograph_checkpoints
    WHERE thread_id = 'dr-hitl-a1b2c3d4'
    ORDER BY step;"

$ docker compose exec postgres psql -U postgres -d neograph -c "
    SELECT channel, COUNT(*) AS versions
    FROM neograph_checkpoint_blobs
    WHERE thread_id = 'dr-hitl-a1b2c3d4'
    GROUP BY channel
    ORDER BY versions DESC;"
```

`final_report` will have one row per generated report; channels that
didn't change between super-steps (`user_query`, `research_brief`)
have exactly one row total.

### Reference numbers from a real run

Historical pre-cutover measurements, not execution evidence for the current
typed provider migration. The transcript above is historical/illustrative too.

A complete run-resume-resume cycle on the multimodal-RAG demo above
(2 supervisor rounds × 2 researchers each, pinned DeepSeek via OpenRouter)
produced these PG numbers:

| Metric                      | Value      | Notes |
|-----------------------------|------------|-------|
| `neograph_checkpoints`      | 15 rows    | 6 super-steps + 1 NodeInterrupt + 6 super-steps + 1 NodeInterrupt + 1 approve cp |
| `neograph_checkpoint_blobs` | 29 rows    | vs theoretical 15 cps × 9 channels = 135 — **78.5% dedup** |
| `neograph_checkpoint_writes`| **0 rows** | clean — every super-step's pending log was cleared on commit |
| `final_report` v13 (round 1)| 2806 B     | no arXiv URLs (model used `arXiv:NNNN.NNNNN` shorthand) |
| `final_report` v27 (round 2)| 2752 B     | full `https://arxiv.org/abs/...` URLs after the user asked for them |
| Total blob bytes            | 41 KB      | entire thread state, including all LLM transcripts |

The `final_report` v13 → v27 diff is the smoking-gun proof that user
feedback actually changed the agent's output: the second report
trimmed prose AND added URL citations — a quality improvement the
supervisor only reached because the HITL gate fed the feedback back
into `supervisor_messages`.

## Setup

1. Copy and fill in credentials:
   ```
   cp .env.example .env
   # set OPENROUTER_API_KEY; set CRAWL4AI_API_TOKEN to a fresh `openssl rand -hex 32` value
   ```
2. Bring up the supporting services:
   ```
   docker compose up -d postgres crawl4ai
   ```
3. Use Docker Compose >= 2.17 with BuildKit on a Linux/POSIX build host and set `SCHEMAPROVIDER_SOURCE`
   to the actual SchemaProvider checkout (default `../../../SchemaProvider`,
   relative to this directory). The Dockerfile installs `SchemaProvider::runtime`
   from this named additional build context before building NeoGraph. This is
   required even for Core-only builds; this guide does not qualify native
   macOS/Windows provider transport.
4. Set a stable `NEOGRAPH_NATIVE_ARCHIVE_OWNER` in `.env`, then provision once:
   ```
   docker compose run --rm agent init-archive
   ```
5. Run the demo (see the scenario). Live calls require valid OpenRouter/Crawl4AI
   credentials, network access, and provider credit; research/feedback rounds
   incur model charges. No fixed cost or current execution success is asserted.

Keep `pgdata`, `native-history`, and `native-keys` together across invocations.
The archive and key use separate private volume parents; resume requires the
same owner, compatible provider descriptor, original key, and archive records.
`init-archive` provisions custody; do not regenerate keys to resume old threads.
This is authenticated owner-private host custody, **not encryption or provider
issuer proof**. Protect PG, backups, prompts, feedback, reports and `.env` too.
Public diagnostics must not dump raw native records or archive/key contents.
The CLI prints queries, feedback and reports: use a private terminal/log sink.

When done:
```
docker compose down       # keep PG, native-history and native-keys
docker compose down -v    # delete all three; old native resume is lost
```

## Running the binary directly (no docker-compose for the agent)

You can also build the binary on the host and point it at the
docker-compose-managed Postgres + Crawl4AI:

```
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -B build -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_BUILD_POSTGRES=ON -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build --target example_postgres_react_hitl -j

# Load .env privately in the binary's working directory; keep archive paths stable.
mkdir -m 700 .native-keys
./build/example_postgres_react_hitl init-archive
./build/example_postgres_react_hitl run "...your query..."
./build/example_postgres_react_hitl resume <thread_id> "feedback"
./build/example_postgres_react_hitl status <thread_id>
```

The host-side .env's `POSTGRES_URL=postgresql://postgres:test@localhost:55432/neograph`
points at the compose-published port; `CRAWL4AI_URL` does the same for Crawl4AI.
`CRAWL4AI_API_TOKEN` is sent as its bearer credential; the compose file rejects
an empty value and publishes Crawl4AI only on `127.0.0.1`.

## Running the integration tests against this stack

The compose file provisions a **separate `neograph_test` database** on
the same Postgres instance specifically for the
PostgresCheckpointStore integration tests (which call `drop_schema()`
in SetUp and would otherwise wipe demo threads). Run them with:

```bash
NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir ../../build -R PostgresCheckpoint --output-on-failure
```

Pointing the test URL at `/neograph` instead of `/neograph_test` will
nuke any thread data you have in the demo DB — don't do that.

## Forensics tip — where the user's feedback lives in PG

If you query the `messages` channel directly (the channel the engine
uses to hand the resume value to `HumanReviewNode`) you will see only
empty arrays:

```
SELECT version, blob_data FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'messages';
-- 0 | null
-- N | []
-- M | []
```

That's intentional, not data loss: `HumanReviewNode` consumes the
incoming user message and immediately writes `messages: []` back into
its `Command.updates` so a future interrupt cycle starts from a clean
channel. By the time a checkpoint is taken, the array is already
empty.

The actual feedback text lives in the `supervisor_messages` channel,
prefixed with the marker `[USER FOLLOW-UP after reviewing...]`:

```
SELECT blob_data::text FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'supervisor_messages'
 ORDER BY version DESC LIMIT 1;
```

`grep` for that marker to recover everything the user said across all
HITL rounds in the thread.

## Implementation notes

- The HITL gate is built into the Deep Research graph behind the
  `DeepResearchConfig::enable_human_review` flag (default off, so
  example 25 is unaffected). With it on, a `HumanReviewNode` sits
  between `final_report` and `__end__`.
- That node throws `NodeInterrupt` on first execution. The engine
  catches it, saves a checkpoint at phase `NodeInterrupt`, and
  re-throws to the caller. On resume, the engine re-enters the same
  node with the user's reply written into the `messages` channel.
- The node distinguishes "approve" (→ Command(__end__)) from feedback
  (→ Command(supervisor) with the feedback appended to
  `supervisor_messages` and the iteration counter reset). Both paths
  end the run cleanly so PG always has a coherent latest cp.
- All three steps cross process boundaries. PG stores portable graph state;
  native continuation additionally needs the protected archive and original key.
- C++ requests/events are typed and return the full immutable `sp::Outcome`;
  portable report text is a projection, not native replay authority. This guide
  documents source migration only, not a new build, test, or live qualification.

## Empty-budget recovery and report quality

The supervisor, researcher, compression and final-report paths permit at most
two additional semantic calls after a completed empty `MaxTokens` outcome with
no visible text and no valid or invalid client tool call. The cap doubles up to
16,384; the research-brief call is outside this ladder. Each attempt has a new
broker ordinal, original-bank admission, retained outcome and usage, and the same
absolute deadline. Without an explicit deadline, one effect-free preparation
discovers the configured deadline, releases its handle before mediated invocation
and pins that deadline; an explicit deadline skips discovery.

Failures, observer/settlement errors and delivered streaming parts do not retry.
Empty final text raises an error before human review; a nonempty `MaxTokens`
report remains visibly `Incomplete` in the public projection without mutating
the immutable outcome or retrying its partial text. Exhausted empty compression
produces a diagnostic, not a fabricated successful provider result. These are
retained interface-4 source contracts, not a new live run or durability proof.

## Why no frontend?

The "binary exits → restart → continue" flow IS the frontend. A web UI
would only marshal the same arguments through HTTP and wouldn't add
anything to the demonstration of checkpoint durability. Inspect the PG
tables directly (above) for the visual proof.
