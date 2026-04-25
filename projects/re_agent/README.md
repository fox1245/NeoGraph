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

From the NeoGraph repo root (docker 경로 default — `:18080`):
```
./build-release/example_re_agent
# or with a stronger model / more steps:
./build-release/example_re_agent --model gpt-4o --max-steps 120
```

Native install (Ghidra가 호스트에서 직접 :8080 띄움):
```
GHIDRA_SERVER_URL=http://127.0.0.1:8080/ ./build-release/example_re_agent
```

Trace and tool calls go to **stderr**; the final JSON summary goes to
**stdout** so you can pipe / diff:

```
./build-release/example_re_agent > agent_out.json 2> agent_trace.log
diff <(jq -S . agent_out.json) <(jq -S '.functions' projects/re_agent/targets/ground_truth.json)
```

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
