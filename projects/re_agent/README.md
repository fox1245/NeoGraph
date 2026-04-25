# NeoGraph RE Agent — Self-Test MVP

Goal: drive `ghidra-mcp` from a NeoGraph ReAct loop to recover meaningful
function names + summaries from a stripped binary, with a ground-truth
oracle so we can score the agent objectively.

## Layout

```
projects/re_agent/
├── README.md                    ← you are here
├── targets/
│   ├── crackme01.c              ← source (ground truth, NOT shown to agent)
│   ├── crackme01                ← stripped ELF (the actual RE target)
│   ├── crackme01.dbg            ← unstripped (debug-only sanity check)
│   ├── ground_truth.json        ← function name/sig/summary oracle
│   └── build.sh                 ← rebuilds both flavors
└── (next iteration: scorer.py to diff agent output vs ground_truth.json)
```

The NeoGraph-side code lives at [examples/35_re_agent.cpp](../../examples/35_re_agent.cpp)
(wired into the existing `NEOGRAPH_BUILD_MCP` block in the root CMake).

## Prerequisites — one-time setup

1. **Build the target** (already done if you can see `targets/crackme01`):
   ```
   ./targets/build.sh
   ```
2. **Build the agent**:
   ```
   cmake -S . -B build-release -DNEOGRAPH_BUILD_MCP=ON -DNEOGRAPH_BUILD_EXAMPLES=ON
   cmake --build build-release --target example_re_agent -j
   ```
3. **OpenAI key** — write to `.env` at the NeoGraph root:
   ```
   OPENAI_API_KEY=sk-...
   ```

## Per-run setup — load the binary into Ghidra

**→ 셋업 절차 전체는 [GHIDRA_SETUP.md](GHIDRA_SETUP.md) 참고.** Ghidra
plugin enable은 4단계 (Install Extensions → restart → Developer 패키지
enable → plugin enable) 로 나뉘고 중간에 restart가 필요해서 처음
하시는 분은 그 가이드를 그대로 따라가는 게 안전합니다.

요약 (docker 권장 경로):
```
cd projects/re_agent/docker && docker compose up -d
# → WSLg 데스크탑에 Ghidra 창이 뜸
# → GHIDRA_SETUP.md 의 1~4단계 진행 (1회성)
curl -s http://127.0.0.1:18080/methods | head      # 검증
```

## Run the agent

세 가지 LLM backend 선택 (env-driven, 코드 변경 없음):

### A. OpenAI Responses over WebSocket (default — `OPENAI_API_KEY` 사용)
```
./build-release/example_re_agent > /tmp/agent_out.json 2> /tmp/agent_trace.log
./build-release/example_re_agent --model gpt-5.4-mini --max-steps 120
```

### B. OpenRouter (또는 vLLM / trtllm-serve / 기타 OpenAI-compat HTTP)
```
LLM_BASE_URL=https://openrouter.ai/api \
LLM_API_KEY=sk-or-v1-... \
  ./build-release/example_re_agent --model deepseek/deepseek-v4-pro
```
(`.env`에 `LLM_BASE_URL` / `LLM_API_KEY`를 두면 cppdotenv가 자동 로드.)

### C. Ollama 로컬 (실험적 — 8B 이하 모델은 27 tools 처리에 약함)
```
LLM_BASE_URL=http://127.0.0.1:11434 \
  ./build-release/example_re_agent --model qwen2.5:7b-instruct
```

### Ghidra native install (8080 직접 사용 시)
```
GHIDRA_SERVER_URL=http://127.0.0.1:8080/ ./build-release/example_re_agent
```

## Score the result

`scorer.py` runs an LLM judge (gpt-5.4-mini, OpenAI Responses) comparing
the agent's `recovered` list against `targets/ground_truth.json` on three
axes per function: name semantics, summary semantics, and rationale.
Exit 0 = pass (default threshold `matched_score >= 0.83` == 5/6).

One-time venv setup (the scorer is the only Python in this project):
```
cd projects/re_agent
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

Score a run:
```
projects/re_agent/.venv/bin/python projects/re_agent/scorer.py /tmp/agent_out.json
```

Output: per-function table (PASS/PARTIAL/FAIL on name + summary) +
extra-agent-entries (likely false positives like CRT glue the agent
mistook for user code) + aggregate score. Use `--report-json` to also
dump the raw judge JSON for CI archival.

Trace and tool calls go to **stderr**; the final JSON summary goes to
**stdout** so you can pipe / diff:

```
./build-release/example_re_agent > agent_out.json 2> agent_trace.log
diff <(jq -S . agent_out.json) <(jq -S '.functions' projects/re_agent/targets/ground_truth.json)
```

### External-binary ground truths (no source available)

For binaries without source (e.g. `targets/j2kengine_ground_truth.json`),
the GT file is **a self-consistency baseline derived from the agent's own
first-pass output**, not a verified oracle. Treat these as drafts that
require human domain review (rename obviously wrong entries, drop CRT
false positives, fill `signature` placeholders) before the
`matched_score` numbers carry any weight. Source-derived GTs like
`crackme01` remain the trusted reference.

## Scoring (next iteration — not yet built)

`scorer.py` will compare `agent_out.json` against `ground_truth.json` on
three axes:

| Axis              | Method                                              |
|-------------------|-----------------------------------------------------|
| Name semantics    | LLM judge: does agent's name mean the same thing?  |
| Signature shape   | arg count + return polarity (returns int? void?)   |
| Summary tag overlap | Jaccard over `tags` field                        |

The MVP target is **5/6 functions correctly named** on `crackme01` with
`gpt-4o-mini`. Once that's reliable we expand to:

- crackme02 (more functions, struct recovery)
- per-function ToT meaning inference (replace single-shot LLM with
  [examples/16_tree_of_thoughts.cpp](../../examples/16_tree_of_thoughts.cpp))
- parallel fan-out (per-function subgraphs run concurrently —
  `burst concurrency robustness` axis lights up here)
- clean-room reimplementation stage (separate agent context, spec-only
  input, differential testing harness)

## Known gotchas

- **Ghidra GUI must stay open** while the agent runs. Closing the
  CodeBrowser drops the 8080 server → next tool call fails with
  `connection refused`.
- **Don't load `crackme01.dbg`** by accident — that has symbols and the
  agent will trivially "win".
- **WSLg quirks**: if `ghidraRun` opens but no window appears, check
  `echo $DISPLAY` (should be `:0`) and that `xeyes` works.
