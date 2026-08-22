<!-- neograph-i18n: source=examples/cookbook/the-beast/README.md locale=ko source_sha256=c70c7b805d11a43a76fb1402e0b7ab7160eea9d0b9137fc779776b717d66c453 -->
# The Beast — 생성 · 진화 · 롤백

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> 엄격한 Core JSON으로 자체 하네스를 작성하고, Core 컴파일러 아래에서 이를 진화시키며, 체크포인트 관리자를 통해 실행을 되감는 자기 진화 에이전트입니다. **생성되고, 진화하고, 되감겨도 The Beast는 남습니다.**

대부분의 "에이전트 프레임워크"는 그래프를 *빌드*하게 해 줍니다. The Beast는 정적 하네스가 할 수 없는 세 가지를 수행합니다 — 그리고 그 세 가지 모두 이 단일 프로그램에서 **실제이며, 오프라인이며, 결정론적입니다** (API 키 없이):

1. 실행 시간에 새 하네스를 **생성**하고, 단일 노드가 실행되기 전에 정합성을 증명합니다.
2. 실제 변이 연산자로 하네스를 **진화**시키며, 컴파일러 자체를 적합도 게이트로 사용합니다.
3. 체크포인트 관리자를 통해 실행 중인 하네스를 이전의 어떤 슈퍼스텝으로든 **롤백**합니다. 이는 재생(replay)이 아닌 진정한 시간 이동입니다.

그것은 NeoGraph에서 하네스가 **데이터**이기 때문에만 안전합니다 — 엄격한 Core JSON(이슈 #56)으로 기술된 토폴로지 — 그리고 Core 컴파일러는 실행 전에 하네스의 일관성을 *증명*할 수 있습니다. 엄격한 Core JSON은 교환 아티팩트이지, 제2의 소스 언어가 아닙니다. 컴파일러가 그 괴물을 부채에서 카테고리로 바꾸는 것입니다.

## 실행합니다.

```console
$ cmake --build build --target cookbook_the_beast
$ ./build/cookbook_the_beast
```

```
── ACT I · generate a harness, prove it coherent ──
  ACCEPTED — strict compile and validation gates passed. Core nodes: s1_n s2_n s3_n
  (strict Core JSON is already the canonical interchange representation.)

── ACT II · evolve the harness (compiler = fitness) ──
  generations: 4 · offspring: 36 · survived compile gate: 36 · rejected (invalid, never run): 0
  sample mutations that produced offspring:
    gen 1: remove_edge: removed edges[0]        →  3 nodes
    gen 1: toggle_ce: added conditional edge from s2_n  →  3 nodes
    gen 1: toggle_barrier: added barrier on s3_n →  3 nodes
  (full diffable lineage via to_json(result) — the evolutionary rollback surface.)

── ACT III · spawn + roll back through the checkpointer ──
  ran to completion, trail = ["s1_n","s2_n","s3_n"]
  checkpoint timeline (3 snapshots):
    step 0  id=aac922ed  trail=["s1_n"]
    step 1  id=4b74daa9  trail=["s1_n","s2_n"]
    step 2  id=a528eb9d  trail=["s1_n","s2_n","s3_n"]
  >> ROLLBACK to step 1 (id=4b74daa9)
     final trail was ["s1_n","s2_n","s3_n"]; restored trail = ["s1_n","s2_n"]  (later steps gone)

Generated. Evolved. Rewound. The Beast remains.
```

## Act I — 생성 + 게이트

The Beast는 하네스를 엄격한 Core JSON으로 직접 작성하고 컴파일러와 검증 게이트를 순서대로 통과시킵니다. 어떤 게이트든 통과하지 못하는 하네스는 **폐기됩니다**.

| 게이트 | API | 캐치 |
|---|---|---|
| **1. 컴파일 + TV** | `GraphCompiler::compile` (strict, `schema_version: 1`) + `verify_roundtrip` | 오타가 있거나 지원되지 않는 키는 조용히 버려지는 것이 아니라 *hard error*(소비된 키 회계)이다. 번역 검증은 `canon(source) == canon(compile(source).to_json())`를 단언한다. |
| **2. 검증하기** | `GraphValidator::validate` | 그래프가 **의미**하는 것: dangling edges (E3), 절대 발화할 수 없는 장벽 (E8), 불완전한 경로 맵 (E10), channel-effect 위반 (E4/E6). |

시드에는 코어 체인 `s1_n → s2_n → s3_n`으로 연결된 세 개의 명시적 노드가 포함되어 있다.

## Act II — 진화 (컴파일러가 적합성 함수다)

`neograph::graph::evolve()` (이슈 #80)은 시드에 대해 **실제 돌연변이 연산자**를 실행한다 — `toggle_conditional_edge`, `toggle_barrier`, `add_edge`, 및 `remove_edge`. 모든 자손은 **먼저 compile gate를 통과**한다: 유효하지 않은 자손은 실행 없이 공짜로 죽는다. 거부율 자체가 연산자에 대한 건강 지표이다.

돌연변이 공간은 제한된 strict Core 토폴로지이며, 소스 언어가 아니다. 자손은 표준 상호 교환 표현으로 유지되며, 컴파일러 게이트는 모든 자식에 대해 계속 무장된 상태를 유지한다.

각 실행은 `to_json(result)`를 통해 diff 가능한 계보를 방출한다: 각 개체의 부모, 세대, 돌연변이, 및 core lockfile. 그 계보는 진화적 규모에서의 롤백 표면이다 — 커밋하고, diff하고, 전체 세대를 되돌린다.

## 제3막 — 롤백 (체크포인트 시간 여행)

생존한 하네스는 `InMemoryCheckpointStore`가 `EngineConfig::checkpoint_store`를 통해 연결된 채로 생성된다. 엔진은 모든 super-step의 끝에서 상태를 스냅샷한다. 이후:

- `store->list("beast-run")`는 전체 타임라인을 반환한다 — `trail`가 스텝마다 노드 하나씩 성장하는 것을 *볼* 수 있다.
- `store->load_by_id(earlier.id)`는 이전 스텝에서의 정확한 채널 상태를 **복원**한다. 데모는 `["s1_n","s2_n","s3_n"]`에서 `["s1_n","s2_n"]`로 롤백한다 — 이후의 스텝은 진정으로 사라진다. 이것은 `load_by_id` / `load_latest` time-travel이며, HITL 인터럽트/재개와 스레드 포킹이 구축된 것과 동일한 메커니즘이다.

## 라이브 전환 — 모델이 실제로 하네스를 작성합니다

`the_beast.cpp`는 오프라인이다 (스텁 작성자). [`the_beast_live.cpp`](the_beast_live.cpp)가 실제다: 라이브 LLM에 `NodeFactory::export_schema()` (이 엔진 빌드가 수용하는 정확한 팔레트 — 엔진의 스키마 자체이므로 드리프트할 수 없다, [`../../52_export_schema.cpp`](../../52_export_schema.cpp) 참조)이 전달되고 엄격한 Core JSON으로 하네스를 작성하도록 요청받는다. 반환된 것은 무엇이든 동일한 컴파일러와 검증 게이트를 통과한다; 거부 진단 시 진단이 대화에 직접 피드백되고 모델이 다시 작성한다 — 진정한 self-repair loop.

```console
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek V4 Flash 0731 via OpenRouter
$ cmake --build build --target cookbook_the_beast_live
$ ./build/cookbook_the_beast_live                  # optional: pass a task string as argv[1]
```

`the_beast_live.cpp`는 `~deepseek/deepseek-v4-flash-latest`를 `provider: {"zdr": true, "only": ["morph"], "allow_fallbacks": false}`에 고정한다. 검증 시점에 OpenRouter는 Morph의 데이터센터를 US로 나열했고 해당 모델/프로바이더 엔드포인트를 ZDR-capable로 나열했다. 이것은 OpenRouter의 지역 내 상주 보장이 아닌 엄격한 프로바이더 선택이다: 문서화된 지역 내 보장은 현재 엔터프라이즈 EU 경로이다. Morph의 적격 엔드포인트를 사용할 수 없으면 프롬프트를 다른 프로바이더로 보내는 대신 요청이 실패한다.

라이브 쿡북은 공급자 타임아웃을 180초로 설정합니다: 이 추론 모델의 4,000토큰 생성 예산은 일반적인 60초 기본값보다 오래 지속될 수 있습니다.



```
── Attempt #1: asking the model to write a harness ──
  model returned 663 chars of JSON.
  ACCEPTED — strict compile and validation gates passed.
  Core lockfile nodes: r_stage c_stage s_stage

── Spawning the model's harness (checkpointed) ──
  ran to completion, trail = ["r_stage","c_stage","s_stage"]
  checkpoint timeline (3 snapshots): ...
  >> ROLLBACK to step 1: restored trail = ["r_stage","c_stage"]

The model wrote it. The compiler proved it. The Beast ran it.
```

**라이브 실행 결과가 보여준 것** (DeepSeek v4 flash): 선형 파이프라인, 다이아몬드 fan-out/배리어 fan-in, 조건부 라우터에서 첫 시도로 *일관된* 하네스를 작성했습니다 — 자기 수리 루프는 작동하지만, 유능한 모델은 거의 트리거하지 않습니다. 게이트는 여전히 린트(lint)로서 그 가치를 입증했습니다. 다이아몬드에서 누락된 배리어(E9)와 라우터의 도달 불가능한 핸들러(E7)를 경고로 플래그했습니다. 핵심은 모델이 자주 실패한다는 것이 아니라, **실패할 때 컴파일러를 통과하여 깨진 하네스를 통과시킬 수 없다는 것**입니다. 창의력은 무한하지만, 일관성은 입증됩니다.

여기의 노드는 결정론적 `beast_node` 워커이므로 live run은 LLM 호출 한 번(작성)만으로 비용이 들고 실행은 무료입니다. 이를 `llm_call`로 교체하면 각 노드도 라이브 호출이 됩니다.

## Copy Ninja — 검증된 로컬 기능이 그래프 노드가 됩니다.

[`the_beast_copy_ninja.cpp`](the_beast_copy_ninja.cpp)은 하나의 좁은 capability-to-harness path를 실행 가능하게 만듭니다. A2A 카드를 코드로 바꾸지는 **않습니다**:

1. 합성 루프백 서버는 하나의 잘 알려진 Agent Card를 노출합니다; 컬렉션은 정확히 해당 GET을 수행하며 카드가 광고하는 RPC URL을 절대 따라가지 않습니다;
2. `AgentCardCandidateCompiler`는 자유 형식 카드 텍스트, 엔드포인트, 자격 증명, 실행 가능한 소스를 제외한 불변의 **미승인(admission)** 디스크립터를 생성합니다;
3. 독립적으로 제공되고 다이제스트로 고정된 동작 프로필이 유일한 `copy-ninja.hello-world-echo.v1` 템플릿을 검증한 다음 이를 로컬 `CopyNinjaNode`로 구체화합니다; 그리고
4. 라이브 Beast는 오직 2채널, 1노드 토폴로지만 작성합니다. 일반적인 엄격한 컴파일/왕복 → 검증 게이트가 먼저 실행됩니다. 그런 다음 네 번째 로컬 바인딩 게이트는 정확히 `copy_ninja_local` 사이에 `__start__` 및 `__end__`.

호출자의 프롬프트는 의도적으로 LLM 메시지에 없습니다: 모델은 토폴로지를 작성하는 반면, 로컬 그래프만이 프롬프트를 소비합니다. 합성 소스 서버가 어떤 RPC도 관찰하면 실행 역시 실패합니다. 이는 소스 코드 전송, 위임, 승인(admission), 또는 일반적 동작 등가성 **이 아닌**, 하나의 고정된 로컬 동작에 대한 증거입니다.

```console
$ cmake -S . -B build -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_A2A=ON
$ cmake --build build --target cookbook_the_beast_copy_ninja
$ ./build/cookbook_the_beast_copy_ninja "Grace"
```

2026-08-08에 관찰된 라이브 결과: 작성 모델이 첫 시도에서 네 게이트를 모두 통과했습니다. 그래프는 디스커버리 GET 하나와 소스 에이전트 RPC 0회로 `Hello, World! I have received your request (Grace)`를 반환했습니다.

## Apex — 하네스가 도구를 삼킨다

스텁 워커 데모는 생성된 하네스가 *일관성*이 있음을 증명하지만, 하네스는 결코 행동하지 않습니다. [`the_beast_apex.cpp`](the_beast_apex.cpp)이 진짜 핵심입니다: 모델에 **도구 카탈로그**가 주어지고 ReAct 에이전트를 작성하라는 요청을 받습니다 — `llm_call` ⇄ `tool_dispatch`가 `has_tool_calls`에서 반복됩니다. 작성된 하네스는 일관성에 대해 게이트 검사를 받은 다음 **도구가 바인딩된 채로 스폰됩니다** (`ctx.tools` + `engine->own_tools`). 스폰된 에이전트는 그런 다음 스스로 어떤 도구를 언제 호출할지 결정합니다.

```console
$ cmake --build build --target cookbook_the_beast_apex
$ ./build/cookbook_the_beast_apex "What is 23 * 19, and the weather in Seoul?"
```

실제 실행 — 자기 수리 루프가 실제로 발동하고, 그다음 자율적 도구 호출:

```
Tool catalog offered: calculator get_weather

── Attempt #1: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'id'    (strict, schema_version 1)
  → feeding diagnostics back for self-repair.
── Attempt #2: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'name'
  → feeding diagnostics back for self-repair.
── Attempt #3: model authors a tool-calling agent ──
  ACCEPTED — coherent tool-calling agent. Nodes: agent(llm_call) tools(tool_dispatch)

── Spawning the agent it wrote — live, tools bound ──
  user task: What is 23 multiplied by 19, and what's the weather in Seoul?
  [the harness is calling tools autonomously]
    tool → {"result":437.0}
    tool → {"weather":"19C, clear"}
  tool calls executed by the harness: 2
  final answer: 23 × 19 = 437; Weather in Seoul: 19°C, clear.

The model wrote the agent. The compiler proved it. The agent ate the tools.
```

이것이 한 번의 실행으로 드러난 전체 논지입니다: 모델이 `nodes` 스키마를 두 번 환각했습니다 (`id`, 그 다음 `name` 키를 추가), 그리고 엄격한 컴파일러의 **소비 키 회계가 둘 다 거부했습니다** — 진단이 대화로 다시 들어갔고 세 번째 시도에서 스스로 수리했습니다. 그런 다음 기계가 작성하고 컴파일러가 증명한 에이전트가 라이브 ReAct 루프를 실행하고 두 도구를 자율적으로 호출했습니다. 창의성은 무제한이고, 도구 사용은 자율적이며, **일관성은 타협할 수 없습니다.**

## Forge — 도구가 없을 때, 도구를 직접 만든다

[`the_beast_forge.cpp`](the_beast_forge.cpp)은 정점에 도구 공급망을 더한 것입니다. 작업이 주어지면 다음을 수행합니다:

1. **DISCOVER** — 스톡 MCP stdio 서버를 스폰하고 실제 MCP 프로토콜을 통해 도구를 나열합니다 (`MCPClient::get_tools`).
2. **FORGE** — 작업에 필요한 기능이 카탈로그에 없을 경우, 아키텍트 LLM이 이를 구현하는 **Python MCP 서버를 작성**합니다; 우리는 이를 디스크에 구체화하고, 실행한 뒤, MCP를 통해 새 도구를 **재발견**합니다. (생성된 서버가 초기화에 실패하면 자가 복구합니다.)
3. **AUTHOR** — *결합된* 카탈로그를 기반으로 ReAct 에이전트를 작성합니다; 항상 그렇듯 3개의 게이트와 자가 복구를 포함합니다.
4. **SPAWN** — 발견된 *그리고* 위조된 모든 도구를 바인딩하고, 이들을 자율적으로 호출하는 에이전트를 실행합니다.

실제 실행 — 모델이 누락된 도구를 작성했고 에이전트가 이를 사용했습니다:

```
── DISCOVER · stock MCP server ──
  tools: get_current_time calculate get_weather

── FORGE · the model writes a Python MCP server for what's missing ──
  attempt #1: wrote 5225 bytes → /tmp/beast_forged_server.py
  FORGED + re-discovered over MCP: reverse_string

── AUTHOR · the model writes a ReAct agent over the full catalog ──
  #1 REJECTED at 'compile': ... unknown or unconsumed key 'id' → self-repair.
  ACCEPTED — coherent agent: agent(llm_call) tools(tool_dispatch)

── SPAWN · run the agent it wrote, tools bound ──
  [harness dispatching tools autonomously]
    tool → retsnom                         # the forged reverse_string
    tool → 2026-07-10 06:13:21 (UTC)       # the discovered get_current_time
  final answer: Reversed 'monster' → retsnom; current UTC time is 2026-07-10 06:13:21.

It discovered tools, forged the missing one, and used them all.
```

두 개의 라이브 MCP 하위 프로세스 (하나는 스톡, 하나는 Beast가 *이번 실행에서* 작성), 각각에 실제 `tools/list`, 실제 ReAct 루프. 작성 모델만 원격입니다.

### 사용자 정의 *노드*도 정의할 수 있나요?

솔직히 말하면: NeoGraph 노드 **유형**은 `NodeFactory::register_type`를 통해 등록된 C++ 클래스입니다 — 런타임에 완전히 새로운 원자적 C++ 노드 유형을 JIT 컴파일할 수 없습니다. 그러나 의도는 Beast가 데이터에서 구동할 수 있는 세 가지 방식으로 충족됩니다:

- **복합 노드(Composite nodes)** — 명시적 Core 노드와 엣지를 통해 모델이 재사용 가능한 토폴로지 단위를 순수 데이터로 정의할 수 있습니다. 이것이 정확히 `the_beast.cpp`의 시드가 하는 일입니다.
- **재귀(Recursion)** — `subgraph` 노드는 전체 하네스를 하나의 노드로 내장하므로, Beast가 작성한 하네스는 Beast가 작성한 서브하네스를 포함할 수 있습니다(N-레벨 자기 증식).
- **코드를 통한 사용자 정의 동작** — 위의 forge 패턴은 모델이 작성한 런타임 동작입니다. 즉, 모델이 작성한 도구가 디스패치 가능한 단위가 됩니다. 동일한 기법은 일반적인 `script_node` 타입(모델이 작성한 코드를 실행하는 사전 등록된 C++ 노드)으로 일반화되며, 이는 "LLM이 정의한 로직을 가진 새로운 원자 노드"를 얻는 정직한 방법입니다.

진정으로 불가능한 유일한 것은 런타임에 새 *컴파일된 C++ 노드 클래스*를 생성하는 것입니다; 모델이 동작을 특수화하는 데 필요한 모든 것은 컴파일러가 이미 게이트하는 데이터/스크립트/서브그래프 표면에 있습니다.

## Script — 범용 카트리지(모델 작성 노드 로직 + 흐름)

위의 모든 변형은 모델이 *도구*(리프 기능)를 작성하게 합니다. [`the_beast_script.cpp`](the_beast_script.cpp)는 모델이 **노드 로직 — 도구가 범주적으로 표현할 수 없는 제어 흐름(`goto`)을 포함하여** 작성하게 합니다. `script_node`는 구성에 모델이 작성한 Python을 담는 하나의 사전 컴파일된 C++ 노드이며, `run()`에서 노드에 채널 상태를 전달하고 코드가 반환하는 것(`{writes, goto, sends}`)을 그래프에 적용합니다. 모델은 노드의 동작 *그리고* 그래프의 흐름을 데이터로 정의하며, 재컴파일이 필요 없습니다.

일관성은 양보할 수 없습니다. 스크립트는 구성에서 계약을 선언하고(`reads` / `writes` / `goto_targets`), 하네스는 엄격한 Core 컴파일러/검증 게이트에 더해 Beast 계층 **계약 검사**(선언된 쓰기는 선언된 채널이어야 하고, goto 대상은 실제 노드여야 함)에 더해 **런타임 래퍼**(선언 외부의 쓰기/goto를 거부)를 통과시킵니다. 이는 NeoGraph 코어에 **변경 없이** Beast 계층에서 효과/경로 보장을 복원합니다 — 추가적이고 하위 호환적입니다.

```console
$ cmake --build build --target cookbook_the_beast_script
$ ./build/cookbook_the_beast_script --selftest   # offline, no API key
$ ./build/cookbook_the_beast_script              # live: DeepSeek writes the node logic
```

라이브 실행 — 모델은 제어 흐름이 자체 `goto`인 카운터 루프를 작성했습니다:

```
── Attempt #1: model writes node logic ──
  ACCEPTED — coherent, and the script's write/goto surface is contract-checked.

── Spawning — the node's own code drives the loop via goto ──
  [tick #1 — script decides: continue or exit]
  [tick #2 — script decides: continue or exit]
  [tick #3 — script decides: continue or exit]
  trace: tick -> tick -> tick -> END
  final counter = 3  (the model's goto logic ran the loop, contract-enforced)
```

`tick`에서 나가는 정적 엣지는 없습니다. 루프는 모델의 Python이 카운터가 3에 도달할 때까지 `{"goto": "tick"}`을 반환한 다음 `{"goto": "__end__"}`을 반환하기 때문에만 존재합니다. `--selftest`는 API 키 없이 사전 준비된 하네스에서 동일한 메커니즘을 실행하므로 CI가 오프라인에서 이를 실행할 수 있습니다.

**경계(정직하게).** 컴파일러는 그래프의 *형태*를 증명하고, 계약은 노드의 *표면*(접근할 수 있는 채널/대상)을 증명하며, 오직 스크립트의 *내부 로직*만 증명되지 않습니다 — 서브프로세스의 `timeout`과 실행의 `max_steps`로 제한됩니다. 모델이 작성한 코드를 실행하는 것은 임의 코드 실행입니다. 로컬의 사용자 주도 쿡북에는 적합하지만, 프로덕션은 인터프리터 주변에 샌드박스를 원합니다. 이는 **빌드 옵션**이며, 기본적으로 꺼져 있습니다:

Sandboxed-api는 FetchContent로는 잘 embed되지 않으므로, 미리 빌드된 트리를 링크하십시오(옵션 위의 CMake 주석에 빌드 레시피가 있음).

```console
$ cmake -S . -B build -DNEOGRAPH_BEAST_SANDBOX=ON -DSANDBOX2_SRC=/path/to/sandboxed-api
$ cmake --build build --target cookbook_the_beast_script
```

이 옵션을 켜면 Python은 Google **Sandbox2**에서 실행됩니다 — 자체 사용자/pid/mount/net 네임스페이스, 인터프리터와 두 개의 작업 파일로 제한된 읽기 전용 FS 뷰, CPU/벽시계/파일 rlimit. `libcap-dev`, `libunwind-dev`, C++20 툴체인이 필요하며, Linux/WSL2에서 검증되었습니다.

**효과 계약에서 합성된 Seccomp 정책.** Python의 syscall 풋프린트는 안전하게 허용 목록을 만들기에는 너무 크므로 기본 동작은 허용적(permissive)으로 유지됩니다 — 그러나 노드의 선언된 *기능*은 syscall을 차감합니다: `"net"` 기능을 선언하지 않은 노드는 `socket`/`connect`/`bind`/…이 seccomp로 차단됩니다(EPERM); `"exec"` 기능이 없으면 `execve`/`execveat`이 차단됩니다. 정책은 *선언된 계약에서 파생*되며, 수작업으로 작성되지 않습니다. 이는 음성 테스트로 검증되었습니다 — **동일한** python, **동일한** 샌드박스, 선언된 기능만 다름:

```
caps=[]     (no net cap): {"socket": "SOCKET_BLOCKED:EPERM"}   # seccomp denies the syscall itself
caps=[net]  (net cap):    {"socket": "SOCKET_CREATED"}         # capability grants it
```

따라서 이는 네트워크 네임스페이스 이상입니다. `net` 기능이 없으면 `socket()` *syscall*이 실패합니다(netns 위의 심층 방어). 정직한 범위: 이는 **컨테이너급 + 계약 파생 seccomp 차단 목록**이며, 전체 syscall 허용 목록이 아닙니다 — 차단되지 않은 syscall을 통한 커널 익스플로잇은 여전히 격리되지 않습니다. 더 엄격한 노드별 허용 목록(및 기능 기반 비밀 중재)이 문서화된 다음 단계입니다.

## 진화 — memetic (다윈적 + 라마르크식)

오프라인 `the_beast.cpp`는 의도적으로 `run_evaluation=false`을 설정하므로 선택은 구조 전용입니다. 일반 진화 API는 또한 작업을 실행하고 정확한 예상 채널 값을 점수화할 수 있습니다.

[`the_beast_evolve.cpp`](the_beast_evolve.cpp)는 대신 사용자 정의 연속 거리 메트릭을 사용합니다. 근접 실패는 일반 스코어러의 단일 출력 불일치 클래스를 받는 대신 숫자 목표를 향해 개선될 수 있습니다.

- **과제**(실제 과제, 출력 점수 기반 — 구조적 프록시가 아님): 목표 숫자를 계산하는 ARITHMETIC PIPELINE을 조립합니다. 다섯 개의 연산 노드가 존재합니다 — `add2(+2) add3(+3) mul5(*5) mul2(*2) sub1(-1)` — 각 노드는 `acc` 채널(초기값 0)을 읽고, 연산을 적용하고, 다시 씁니다. 하네스의 답은 실행 후 `acc`가 보유한 값입니다; **적합도 = `-(|acc - 20|)`**. *토폴로지*(어떤 연산이 어떤 순서로 실행되는지)가 숫자를 결정하므로, 배선을 진화시키는 것은 계산을 진화시키는 것입니다.
- **다윈적**: 무작위 재배선(`all_operators()`) + 측정된 출력에 의한 선택 — 20을 향해 더듬거리며 나아갑니다.
- **Lamarckian**: LLM이 산술을 수행하고, 정확히 20에 도달하는 체인을 연결한 후, 그 획득한 해법을 유전 가능한 시드로 주입합니다.

```console
$ ./build/cookbook_the_beast_evolve --darwin-only   # offline, deterministic
gen 0  seed acc=5   fitness -15
gen 1  best acc=10  fitness -10  (mut)
gen 2  best acc=24  fitness -4   (mut)   # overshoot
gen 6  best acc=19  fitness -1   (mut)
gen 9  best acc=20  fitness -0   (mut)   → Solved
champion: acc=20, origin 'mut'. Pure Darwinian mutation + selection.

$ ./build/cookbook_the_beast_evolve                 # + Lamarckian (needs OPENROUTER_API_KEY)
gen 2  best acc=24  fitness -4  (mut)
gen 3  [Lamarckian] LLM refinement acc=20  fitness -0  → injected (heritable)
       Solved via Lamarckian injection.
champion: acc=20, origin 'LLM'. The winner is a Lamarckian acquired trait ...
```

대비가 핵심입니다: **맹목적 돌연변이는 목표를 향해 더듬거립니다**(9세대까지 5→10→24→19→20, 시행착오로 숫자를 계산); **LLM은 산술을 수행합니다** — `(0+2)*5*2 = 20` — 그리고 주입될 때 답으로 바로 점프합니다. 획득한 해법이 유전 가능한 챔피언(`origin 'LLM'`)이 되기 때문에 이것은 라마르크적입니다; 맹목적 변이 + 선택은 다윈적이고, 둘 다 실행하는 것은 밈 알고리즘입니다.

정직한 메모: 순수 다윈적 방식은 오프라인에서 검증되었고 결정적입니다. 라마르크적 LLM 호출(deepseek-v4-flash)은 **때때로 불안정합니다** — 스트리밍된 응답이 때때로 파싱 불가능하게 돌아오며, 이 경우 실행은 `[Lamarckian] LLM returned no parseable harness`를 기록하고 다윈적 방식이 라운드를 수행합니다; 마지막 줄은 챔피언의 *실제* 기원을 보고하며, 발생하지 않은 라마르크적 승리를 절대 보고하지 않습니다.

## 게이트 평가(Gate-eval) — 일관성 게이트(coherence gate)는 실제로 건전한가?

The Beast의 전체 안전 논증은 정적 검증기가 *건전한* 일관성 오라클이라는 것입니다: ERROR는 하네스가 런타임에 실제로 오류를 일으킬 것임을 의미하고, 오류가 없으면 실행된다는 것입니다. 이는 **단언되었을 뿐, 측정되지 않았습니다** — 모든 리뷰어가 처음으로 물어본 사항이었습니다.

[`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp)가 이를 측정합니다. 라벨링된 토폴로지 코퍼스를 검증기(예측 판정)와 엔진(실측)을 통해 실행하고 교차 검증합니다. 오프라인, 결정적, 키 없음 — 모든 판정이 실행과 일치한 경우에만 `exit 0`이므로 **CI는 건전성에 대해 게이트할 수 있습니다**.

```console
$ ./build/cookbook_the_beast_gate_eval
case                     | validator      | runtime | sound?
coherent                 | ok             | CLEAN   | yes
E4-undeclared-write      | ERROR:E4       | FAULT   | yes   # reject ⇒ genuine runtime throw
E3-dangling-edge         | ERROR:E3       | FAULT   | yes
E7-unreachable(warn)     | ok             | CLEAN   | yes   # a warning does NOT reject a correct graph
E10-empty-routes         | ERROR:E10      | not run | yes   # dispatch is UB by design — the gate stops it
runtime cross-check: 4/4 cases where the validator's verdict matched execution.
```

테스트 대상 속성:

> 검증기가 ERROR를 보고 ⟹ 그래프가 실행 시 오류를 발생시킴; 검증기가 오류를 보고하지 않음 ⟹ 그래프가 깨끗하게 실행됨.

첫 번째 줄은 *건전성*입니다(오류로 플래그된 그래프가 깨끗하게 실행되면 건전성 구멍이 됩니다); 경고로 플래그된 그래프가 깨끗하게 실행되면 게이트가 *과도하게* 거부하지 않음을 보여줍니다. E10/E8-클래스 오류는 판정 전용입니다 — 빈 경로 맵을 실행하면 `rend()`(UB)를 역참조하는데, 이는 게이트가 존재하는 이유인 바로 그 오류입니다. 따라서 검사되지만 실행되지는 않습니다. 이것은 데모 코퍼스이며 모든 진단의 완전한 커버리지는 아닙니다 — 그러나 "게이트는 건전하다"를 슬로건에서 측정되고 CI로 강제되는 4/4로 바꿉니다.

## Gate-fuzz — 보증과 그 경계, 대규모로

[`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp)는 gate_eval을 5개의 수동 라벨링 사례에서 수천 개의 퍼징 사례로 확장합니다 — 그러나 정직하게. 순진한 접근(그래프 N개를 퍼징하고 정밀도 1.0을 출력)은 연극이 될 것입니다: **엔진은 컴파일 시 검증기를 재실행하고 오류가 있으면 throw하므로**, "검증기-오류 ⟹ 엔진-오류"는 *구성상* 참입니다. 따라서 프로그램은 실제로 정보가 있는 두 가지를 측정합니다:

```console
$ ./build/cookbook_the_beast_gate_fuzz 2>/dev/null   # lint → stderr
LAYER 1 — static gate vs engine over 2000 honest-contract mutants:
  gate rejected 1586, gate passed 414;  agreements 2000, DISAGREEMENTS 0
  runtime faults AFTER the gate passed (soundness holes): 0
LAYER 2 — a node that LIES about its effect contract (500 mutants):
  static gate PASSED (blind to the lie): 500/500
  runtime GraphState guard FAULTED (backstop caught it): 500/500
CI gate (Layer 1: 0 disagreements over 2000; Layer 2: runtime backstops 100%): PASS
```

- **레이어 1 — 규모에서의 일관성.** 응집력 있는 시드(s)를 무작위 구조 변형자(끊어진 간선 → E3, 선언되지 않은 쓰기 → E4, 궤도가 없는 작성자, 제거된 간선 → E7 *경고*, extra 유효 간선)로 퍼즈합니다. 2000개가 넘는 변이체에서 컴파일러 게이트와 엔진이 불일치하지 않습니다. 이는 건전성 *발견*이 아닙니다(구성상 일부는 확보된 것) — 이는**회귀 보장**입니다: 향후 변경으로 정적 게이트와 런타임이 분기하면 실패하는 것입니다.
- **레이어 2 — 경계.** 게이트는 각 노드의 선언된 **효과 계약**을 신뢰합니다. *거짓말*하는 노드 — `writes:["out"]`를 선언하지만 실제로 런타임에 선언되지 않은 `phantom` 채널을 쓰는 — 정적 게이트를 통과하고(500/500), **런타임 `GraphState` 쓰기 가드**가 모든 것을 잡습니다(500/500). 이것은 게이트 버그가 아니라 설계된 업무 분담입니다.

그 결과는 기본 보증에 대한 *정확한* 진술로 — 의심스럽게 완벽한 혼동 행렬보다 더 정직합니다: **정적 게이트는 정직한 계약에 대해 건전하며, 부정직한 계약에는 런타임 방어막이 있습니다** — 레이어 1과 레이어 2 각각 CI로 강제됩니다.

공식 동반 문서인 [`SOUNDNESS.md`](SOUNDNESS.md)는 이를 *증명*합니다: 슈퍼스텝 실행의 작은 단계 의미론, 효과 격자 `(𝒫(Chan), ⊆)`, 잘 형성성 판정 `⊢ G ok`로서의 게이트, 그리고 정직한 계약 하에서 게이트를 통과하는 그래프가 절대 오류를 발생시키지 않는다는 Progress 정리(정직성 가설이 필요함이 증명되고 런타임 쓰기 가드가 fail-stop 백스톱으로 작동). 모든 전제는 엔진 소스에 대해 검사됩니다; `gate_eval`/`gate_fuzz`는 모델의 충실도 검사입니다. 여기 두 하네스는 해당 문서의 Cor 6.4와 Prop 6.5를 실행한 것입니다.

## Baldwin — 밈(memetic)이 맹목(blind)보다 뛰어넘나, 그리고 상속은 중요할까?

`evolve` 변형은 다윈적 돌연변이 + 라마르크적 LLM 주입을 보여주었습니다. 모든 리뷰어가 제기한 더 날카로운 연구 질문: **맹목적 진화와 일회성 솔버가 모두 정체되지만 밈 조합이 승리하는 과제가 있는가 — 그리고 학습된 형질을 *어떻게* 상속하는지가 문헌이 예측하는 대로 결과를 바꾸는가?** (Whitley 1994; Hinton & Nowlan 1987.)

[`the_beast_baldwin.cpp`](the_beast_baldwin.cpp)는 실제 NeoGraph 하네스에서 실행된 그 실험입니다. 게놈은 아핀 파이프라인의 배선입니다. 각 단계는 연산에 커밋되거나 **또는 평생 학습이 해결하도록 플라스틱(`?`)으로 남겨집니다.** 적합도는 **실행 시 조립된 하네스의 시그니처**이며, 시작 시 교차 검증은 빠른 해석적 적합도가 200개 토폴로지에서 컴파일된 엔진과 동일함을 증명합니다(게이트 평가와 동일한 규율). 풍경은 **기만적**입니다: 어디서나 보이는 넓은 미끼 언덕(0.85)과, 오직 *학습*만이 찾을 수 있는 — 플라스틱 유전자가 펼치는 이웃을 탐색하는 — 좁고 **기울기 없는** 전역 고원(1.0)이 있습니다.

```console
$ ./build/cookbook_the_beast_baldwin          # offline, deterministic, no key
engine/analytic cross-check: 200/200 topologies execute exactly as modeled → real harness.
  Darwinian  | assimilated global  6/24 | mean committed → global  25%  decoy  70%
  Baldwinian | assimilated global 21/24 | mean committed → global  74%  decoy  17%
  Lamarckian | assimilated global 23/24 | mean committed → global  78%  decoy   9%
CI gate (blind Darwin near the 25% chance floor, learners assimilate >65% by a
  >25-pt margin, faithful fitness): PASS
```

적합도가 *무엇*이라는 것에 대한 참고: 각 게놈은 실제 NeoGraph 토폴로지로 컴파일되고, 교차검증은 엔진이 200개의 토폴로지를 분석 모델이 예측하는 대로 정확히 실행한다는 것을 증명합니다 — *기반* 은 진실되고 충실히 실행된 하네스입니다. GA가 최적화하는 *목표*는 배선 위의 기만적인 해밍 풍경(동역학 연구를 위한 통제된 테스트베드)이며, 원시 실행 출력이 아닙니다. 두 사실 모두 명확히 구분되어 기술되었습니다.

서로 다른 기준으로 보완되는 두 발견:

1. **Memetic이 맹목적 진화를 압도함(견고함 — CI 게이트 적용).** 맹목적 다윈식 진화는 전역을 약 25%만 동화하는데, 이는 확률 하한선입니다. 플래토에는 커밋된 공간(committed-space)의 기울기가 없어서 선택이 유인물(decoy)을 따라가다 함정에 빠지기 때문입니다. 학습은 플래토를 노출시키고 약 75%를 동화합니다. 게이트는 *마진*(24개 시드에 대한 평균)을 단언하며, 실행별 임계 개수는 단언하지 않습니다. 실행별 개수는 초기화 운에 영향을 받기 때문입니다. 25% 대 75% 마진이 안정적인 신호입니다.
2. **Baldwin 통제 (측정 — 절대 게이트 적용 안 함).** Baldwin(학습된 형질을 유전하지 않음) vs. Lamarck(게놈에 기록함): 여기서는 전역 기준 **74% vs 78%** — Lamarck식이 근소하게 앞서며, 이는 기만적이지만 적대적이지 않은 지형에서 *예상되는* 결과입니다(기록의 속도가 다양성 비용을 능가함). Whitley의 **역전**(Baldwin > Lamarck)은 구체적으로 적대적 지형이 필요합니다. 이 단순한 이중 피크 구조는 이를 견고하게 나타내지 못하며, 이는 **튜닝으로 얻은 우연이 아니라 정직하게 보고됨.** (실제로 미묘한 문제: 초기 버전에서 인덱스 기반 타이브레이크가 *역전*을 보여주는 듯했지만 그것은 인공물이었습니다. 선택 경계에서의 타이는 이제 시드별 무작위 추출로 해소되고 스윕에 걸쳐 **평균화되며**, ~74 vs 78 순서는 시드 베이스에 걸쳐 안정적입니다. 겉보기 역전은 그 수정 이후 유지되지 않았습니다.)

이것은 리뷰어들이 요청한 결과의 정직한 형태입니다: 견고한 주장(학습 유도 진화가 맹목 진화가 해결 못하는 문제를 해결함)은 측정되고 강제 수단으로 게이트 적용되며; 미묘한 주장(비상속이 상속을 이김)은 측정되고 그대로 보고되며, 부정적 결과는 숨기지 않고 명명됩니다.

## Baldwin-adv — 적대적 지형 + 실제 언덕 오르기 학습

[`the_beast_baldwin_adv.cpp`](the_beast_baldwin_adv.cpp)는 이전 실험의 양쪽을 모두 날카롭게 합니다. 학습은 이제 **실제 로컬 탐색**(플라스틱 유전자에 대한 다중 재시작 언덕 오르기로 로컬 최적점에 도달 — 리파이너의 이산적 유사체이자 LLM이 연결되는 슬롯)이며, 풍경은 진정으로 **적대적**입니다: 기울기가 작고 가파른 전역 공에서 *멀어지는* 방향을 가리키는 넓은 미끼 언덕. 맹목적인 커밋 공간 탐색은 우연의 바닥에 있지 않습니다 — 미끼 기울기를 따라 적극적으로 **기만당합니다.**

```console
$ ./build/cookbook_the_beast_baldwin_adv        # offline, deterministic, no key
  Darwinian  | committed → global   5%   decoy  92%
  Baldwinian | committed → global  76%   decoy  19%
  Lamarckian | committed → global  98%   decoy   1%
CI gate (blind deceived onto decoy >50%, both learners solve >60%, faithful): PASS
```

- **Memetic > Blind (강건, CI-gated).** Darwin은 미끼에 속아 ~5% 전역/~92% 미끼에 도달합니다; 학습은 단일 게놈이 발견할 수 없는 전역 공을 발견합니다(76-98%). 이는 고원보다 *더 강한* 분리입니다 — 맹목적인 베이스라인은 맹목일 뿐만 아니라 오도됩니다. 종자 기준 전반에 걸쳐 안정적입니다(Darwin 2-5%, 학습자 76-98%).
- **볼드윈 vs 라마르크: 역전은 재현되지 않습니다.** 라마르크식 기록-백이 안정적인 격차로 승리합니다 (98% vs 76%). 도달 가능/불가능 경계 전체에 걸친 매개변수 스윕(30개 이상의 구성, 3회 스윕)에서 비유전이 기록-백을 견고하게 이기는 **영역은 없었습니다**: 전역이 도달 가능하면 기록-백의 속도가 지배하고, 도달 불가능하면 둘 다 실패하며 볼드윈 다양성에서 약간의 차이(~3-4pt)만 나타납니다. 이것이 "Whitley의 볼드윈 > 라마크 역전이 하네스 토폴로지에서 재현되는가?"에 대한 정직한 실증적 답입니다 — **아니요**, 이 이산 영역에서는 그렇지 않으며, 프로그램은 메커니즘을 명명한 채 그렇게 말합니다. (Whitley의 역전은 실수값 지역 탐색을 사용한 *연속* 다중 모드 함수에서 확립되었습니다; 여기의 이산 토폴로지-GA는 이를 나타내지 않습니다.)

## Baldwin-llm — 모델이 학습 연산자입니다

위의 기계적 학습자(무작위 추측, 언덕 오르기)는 항상 LLM 리파이너가 연결되는 *슬롯(slot)*이었습니다. [`the_beast_baldwin_llm.cpp`](the_beast_baldwin_llm.cpp)는 모델이 실제로 추론할 수 있는 작업에서 이를 연결합니다: 산술 파이프라인의 `?` 단계를 채워 `acc`가 목표에 도달하게 합니다. **학습 연산자는 모델입니다**(`?` 단계에 대한 연산을 선택합니다); 적합도는 조립된 하네스 *실행*입니다. Baldwin/Lamarck 토글은 문자 그대로가 됩니다:

- **Baldwinian**은 모델의 채움을 점수화하지만 유전자를 `?`로 유지합니다 — 다음 세대에 모델을 **다시** 상담해야 합니다. 학습은 유전되지 않습니다.
- **Lamarckian**은 채움을 게놈에 기록합니다 — `?`가 커밋됩니다. 획득 형질은 **유전 가능**하며, 모델을 다시 상담할 필요가 없습니다.

```console
$ ./build/cookbook_the_beast_baldwin_llm       # oracle learner (default, offline)
  Baldwinian (learner = oracle):
    gen 0: … committed genes 16/24 | learner calls 5
    gen 3: … committed genes 10/24 | learner calls 6      # re-learns every gen
  Lamarckian (learner = oracle):
    gen 0: … committed genes 24/24 | learner calls 5
    gen 3: … committed genes 24/24 | learner calls 0      # banked; no re-learning
total learner invocations: Baldwin 23 vs Lamarck 5  (Lamarck banked its way to fewer)
```

관찰 가능한 차이는 적합성(둘 다 목표에 도달함)이 아니라 **게놈 경제성(genome economy)**입니다: Lamarck은 학습자의 작업을 유전에 은행처럼 저장합니다(유전자가 확정되고 호출이 0으로 떨어짐); Baldwin은 매 세대마다 다시 학습합니다(유전자는 가소성을 유지하고 호출은 높게 유지됨). 기본값인 결정적 **오라클(oracle)** 학습자로 오프라인 실행하거나, `--llm`와 함께 `OPENROUTER_API_KEY`를 사용하여 **모델**을 학습자로 만들 수 있습니다 — 이 경우 해당 호출은 실제 API 호출이며, 유전은 말 그대로 모델에 한 번 지불하는 것과 매 세대마다 지불하는 것의 차이입니다. 이것이 "모델의 수정이 유전 가능해지는가?"의 구체적 의미입니다 — 단언이 아닌 추적으로 표시됩니다. (`--llm` 경로는 네트워크가 필요하며, 호출/파싱 실패 시 오라클로 폴백하고 로그를 남기므로 데모는 항상 완료됩니다.)

## Novelist — 전제 하나가 들어가면 라이트노벨 길이의 `.txt`가 나옵니다

가장 단순하면서도 실질적으로 유용한 글쓰기 하네스이자 "NovelWriter" 아이디어의 정직한 형태: 전제를 주면 전체 라이트노벨 크기 원고를 일반 텍스트로 돌려받습니다. [`the_beast_novelist.cpp`](the_beast_novelist.cpp)는 **중간에서 길을 잃음**에 대한 구체화된 치료법입니다 — 긴 이야기는 *하나의 거대한 컨텍스트*로 작성되지 않습니다. 이는 **명시적 이야기 상태**에 대한 작은 그래프입니다:

    channels:  premise · outline · bible · summary · book · idx · total

따라서 각 장은 **압축된 외부화 상태에 대해 새롭게 생성된다**(개요 비트, 스토리 바이블, 실행 요약) — 6만 자를 다시 읽는 대신에. 모델은 소설 전체에서 등장인물이 누구인지 *기억*할 필요가 없다 — `bible` 채널을 읽는다.

```console
$ ./build/cookbook_the_beast_novelist "a librarian's returned books whisper futures" 12
harness passed the coherence gate. writing (live — this takes a few minutes)…
  … chapter 1/12 written (4180 chars)
  …
done — 51k characters across 12 chapters.
manuscript: /abs/path/novel_12ch.txt
```

그래프는 `__start__ → planner → writer ⟲`: `planner` 전제를 개요 + 초기 바이블로 변환하고; `writer` 장 `idx` 을(를) `book` 에 작성하고 **`summary` 및 `bible`를 업데이트합니다** 그래야 다음 반복이 근거를 유지하며, 그런 다음 `Command` goto로 **자체 루프**를 돌다가 `idx+1 == total`까지 진행합니다. 효과 계약이 선언되므로 **일관성 게이트는 한 단어도 쓰기 전에 배선을 증명합니다** — 모든 스토리 상태 채널이 실제로 소비되며, 매달린 단계가 없습니다.

**일괄 생성, 일괄 진화 — 장마다 뚜렷한 느낌.** 각 장은 효과적으로 격리된 서브에이전트입니다(공유 이야기 상태에 의해서만 근거가 주어진 새로운 `writer` 호출). 동일하게 읽히지 않도록, 작가는 장마다 **스타일 게놈**을 진화시킵니다 — 5차원 스타일 공간의 한 점(POV · 시제 · 분위기 · 렌즈 · 페이싱, 480가지 조합). 미니 GA(`baldwin` 밈적 루프, 목표 대신 *다양성*을 목표로 함)를 실행합니다: 이미 사용된 스타일과의 거리인 **참신성**을 최대화하도록 진화된 후보 게놈 배치 — 그런 다음 승자가 커밋되고 `styles_used`에 푸시되어 다음 장이 그로부터 멀어지도록 압력을 받습니다. 오프라인에서 스타일 추적은 결정적이고 눈에 띄게 다양합니다:

```console
  … chapter 1/8  [style: epistolary/journal, present tense, melancholic, dialogue-driven, brisk]
  … chapter 2/8  [style: omniscient third, past tense, wry and whimsical, atmospheric, slow-burn]
  … chapter 4/8  [style: first-person, past tense, melancholic, kinetic action, staccato]
  … chapter 6/8  [style: close third-person, present tense, cold and clinical, kinetic, slow-burn]
```

참신성 탐색은 구별성을 극대화합니다(모든 차원이 다르다는 것을 *보장*하지는 않습니다) — 정직하며, 단일 모델의 중견형 글쓰기 실패를 깨기에 충분합니다.

오프라인(키 없음)에서 **결정적 스텁** 플래너/작성기가 *정확히 동일한 그래프*를 실행하므로 파이프라인 — 상태 스레딩, goto 루프, 누적, `.txt` 출력 — 은 네트워크 없이 검증 가능합니다; `OPENROUTER_API_KEY`는 실제 산문을 위해 모델을 교체합니다. 정직한 범위: 게이트는 *배관*(이야기 상태가 배선되고 스레딩됨)을 증명하며 *산문*은 아닙니다 — 내러티브 품질은 모델의 몫이며, 구조를 넘어선 연속성은 체커 노드(런타임 백스톱 패턴)가 될 것이며, 추가할 명백한 다음 노드로 남겨집니다.

## 표면화된 마찰

- **E6 "기록되었지만 읽히지 않음" on `trail`**은 린트(lint)로 방출되며 — 이는 *정확합니다*: `trail`는 다운스트림 *노드*가 소비하지 않는 터미널 출력 채널입니다. 호스트만 `RunResult::channel`를 통해 이를 다시 읽습니다. 검증기는 그래프의 채널 표면에 대해 정밀하게 동작하는 것이지, 틀린 것이 아닙니다. 효과 분석이 작동 중임을 보여주기 위해 의도적으로 표시해 둔 것입니다.
- **직렬화된 체크포인트 상태는 채널로 래핑되어 있습니다** (`channel_values["channels"]["trail"]["value"]`), 평면(flat)이 아닙니다 — 데모의 `channel_of()` 헬퍼가 이를 언랩합니다. `RunResult::channel`가 읽는 것과 동일한 형태입니다.
- 코어 잠금 파일은 검증 전반에 걸쳐 `schema_version: 1`를 유지하며, 이는 표준 상호 교환 표현을 엄격하게 유지하고 진화 루프가 조용히 그 일관성 보장을 다운그레이드하는 것을 방지합니다.
