<!-- neograph-i18n: source=examples/cookbook/the-beast/README.md locale=ko source_sha256=aa9675ba1cbeeb80c64724416d97b82171a94f2261f16551966e181ee742405d -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# The Beast — 생성 · 진화 · 롤백


> 자체 하네스를 작성하고, DSL 컴파일러 아래에서 그것을 진화시키며,
> 체크포인터를 통해 실행을 되감는 자체 진화 에이전트입니다.
> **생성되었습니다. 진화했습니다. 되감기. 야수는 남아있습니다.**

대부분의 "에이전트 프레임워크"를 사용하면 그래프를 *구축*할 수 있습니다. 야수는 세 가지를 한다
정적 하네스는 할 수 없는 것 — 세 가지 모두 **실제, 오프라인,
이 프로그램에서는 결정론적**입니다(API 키 없음).

1. **런타임에 새로운 하네스를 생성**하고 실행 전에 일관성을 입증합니다.
단일 노드가 실행됩니다.
2. 컴파일러 자체를 사용하여 실제 돌연변이 연산자로 **진화**합니다.
피트니스 게이트로.
3. **롤백** 러닝 하네스를 이전 수퍼 스텝으로 되돌립니다.
checkpointer — 리플레이가 아닌 진정한 시간 여행입니다.

NeoGraph에서 하니스는 **데이터** 즉 토폴로지이기 때문에 안전합니다.
JSON(#56 문제)에 설명되어 있으며 DSL 컴파일러(#75 문제)는 다음을 수행할 수 있습니다.
*실행하기 전에 하네스의 일관성을 입증합니다*. 그거 치워버리고 "에이전트요.
스스로 그래프를 작성하는 기계'는 깨진 그래프를 생성하는 기계일 뿐이다.
컴파일러는 괴물을 책임에서 범주로 바꾸는 것입니다.

## 실행해 보세요

```console
$ cmake --build build --target cookbook_the_beast
$ ./build/cookbook_the_beast
```

```
── ACT I · generate a harness, prove it coherent ──
  ACCEPTED — 3 gates passed. Core lockfile nodes: s1_n s2_n s3_n
  (DSL surface expanded away: vars/templates/use gone.)

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

## 1막 — 생성 + 게이트

The Beast는 DSL **표면**(`vars` / `templates` /
`use`) 순서대로 3개의 일관성 게이트를 통과하도록 강제합니다. 하네스
어떤 게이트라도 실패하면 **폐기됩니다**.

|문|API|캐치|
|---|---|---|
|**1. 정교한**|`Elaborator::elaborate`|DSL 좌표에 대한 표면 오류 — 알 수 없는 템플릿, missing/extra `use` 인수, 가변 주기, 노드 이름 충돌. 전체 및 결정론적: 동일한 DSL는 항상 바이트와 동일한 코어를 생성하므로 게이트 2-3은 고정된 아티팩트에 대한 이유를 제시합니다.|
|**2. 컴파일 + TV**|`GraphCompiler::compile`(엄격, `schema_version: 1`) + `verify_roundtrip`|오타가 있거나 지원되지 않는 키는 자동 삭제가 아닌 *하드 오류*(사용된 키 계산)입니다. 그런 다음 번역 유효성 검사는 `canon(source) == canon(compile(source).to_json())`를 주장합니다. 컴파일러는 아무 것도 자동으로 다시 연결할 수 없습니다.|
|**3. 유효성 검사**|`GraphValidator::validate`|그래프 **의미**: 매달린 가장자리(E3), 절대 발사할 수 없는 장벽(E8), 불완전한 경로 지도(E10), 채널 효과 위반(E4/E6). *결코* 옳을 수 없는 구성에 대해서만 오류가 발생합니다. 나머지는 보푸라기입니다.|

시드는 `use`를 통해 세 번 인스턴스화된 하나의 `stage` 템플릿입니다.
정교화를 통해 이를 코어 체인 `s1_n → s2_n → s3_n`로 확장합니다.

## Act II - 진화(컴파일러는 피트니스 함수임)

`neograph::graph::evolve()`(#80 문제)는 **실제 돌연변이 연산자**를 실행합니다.
시드 위에 — `swap_template`, `add_use`, `remove_use`, `tune_param`,
`toggle_conditional_edge`, `toggle_barrier`, `add_edge`, `remove_edge`.
모든 자손은 **컴파일 게이트를 먼저** 통과합니다.: 잘못된 자손 다이
실행하지 않고도 무료로 제공됩니다. 거부율 그 자체가 건강
운영자에 대한 측정항목입니다.

주요 설계 선택: 돌연변이 공간은 원시가 아닌 **DSL(M4)입니다.
JSON**, 따라서 자손은 *구성에 따라* 구조적으로 유효합니다.
여기서 거부 횟수가 0인 이유는 다음과 같습니다. 게이트는 다음을 수행하는 안전망입니다.
제한 없는 진화는 안전하며 모든 어린이에게 무장 상태를 유지합니다.

각 실행은 `to_json(result)`를 통해 비교 가능한 계보를 내보냅니다.
개인의 부모, 세대, 돌연변이 및 핵심 잠금 파일. 저것
계보 **는** 진화적 규모의 롤백 표면 — 커밋
그것, 비교하고, 전체 세대를 되돌리십시오.

## Act III — 롤백(체크포인터 시간 이동)

살아남은 하네스는 `InMemoryCheckpointStore`로 생성됩니다.
`EngineConfig::checkpoint_store`를 통해 연결되었습니다. 엔진 스냅샷
모든 슈퍼 단계가 끝날 때마다 상태를 표시합니다. 나중에:

- `store->list("beast-run")`는 전체 타임라인을 반환합니다. *볼 수 있습니다*
`trail`는 단계당 하나의 노드를 성장시킵니다.
- `store->load_by_id(earlier.id)`는 정확한 채널 상태를 **복원**합니다.
이전 단계. 데모는 `["s1_n","s2_n","s3_n"]`에서 다음으로 롤백됩니다.
`["s1_n","s2_n"]` — 이후 단계는 실제로 사라졌습니다. 이것은
`load_by_id` / `load_latest` 시간 여행, 동일한 기계 HITL
interrupt/resume 및 스레드 포크가 내장되어 있습니다.

## 라이브 진행 - 모델이 실제로 하네스를 작성합니다.

`the_beast.cpp`는 오프라인입니다(스텁 작성자). [`the_beast_live.cpp`](the_beast_live.cpp)
실제입니다: 라이브 LLM가 `NodeFactory::export_schema()`에게 전달됩니다.
(이 엔진 빌드가 허용하는 정확한 팔레트 — 드리프트할 수 없습니다.
*는* 엔진의 스키마입니다. [`../../52_export_schema.cpp`](../../52_export_schema.cpp)를 참조하세요.
DSL 표면에서 하네스를 제작하도록 요청했습니다. 그것이 무엇이든 반환
같은 세 개의 문을 통과합니다. 거부 시 게이트 진단
대화에 곧바로 반영되고 모델이 다시 작성됩니다.
진정한 자가 수리 루프.

```console
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek v4 flash via OpenRouter
$ cmake --build build --target cookbook_the_beast_live
$ ./build/cookbook_the_beast_live                  # optional: pass a task string as argv[1]
```

```
── Attempt #1: asking the model to write a harness ──
  model returned 663 chars of JSON.
  ACCEPTED — all three gates passed.
  Core lockfile nodes: r_stage c_stage s_stage

── Spawning the model's harness (checkpointed) ──
  ran to completion, trail = ["r_stage","c_stage","s_stage"]
  checkpoint timeline (3 snapshots): ...
  >> ROLLBACK to step 1: restored trail = ["r_stage","c_stage"]

The model wrote it. The compiler proved it. The Beast ran it.
```

**실시간 실행 결과**(DeepSeek v4 플래시): *일관되게* 작성되었습니다.
선형 파이프라인을 통한 첫 번째 시도에서 하네스, 다이아몬드 팬아웃 /
배리어 팬인 및 조건부 라우터 - 자체 복구 루프가 활성화되었습니다.
그러나 유능한 모델은 거의 트립하지 않습니다. 문은 여전히 ​​​​그대로 유지되었습니다.
린트: 다이아몬드(E9)에 장벽이 없어 도달할 수 없다고 표시했습니다.
라우터(E7)의 핸들러를 경고로 사용합니다. 요점은 모델이 아니라
자주 실패합니다. **그렇게 되면 부러진 하네스를 얻을 수 없습니다.
컴파일러를 넘어** — 창의성은 무한하며 일관성은 입증됩니다.

여기의 노드는 결정적 `beast_node` 작업자이므로 실시간 실행 비용이 듭니다.
하나의 LLM 호출(저작)이 무료로 실행됩니다. 그것들을 교환해 보세요
`llm_call` 및 각 노드도 실시간 호출이 됩니다.

## Apex — 하네스가 도구를 삼켜 버립니다.

스텁 워커 데모는 생성된 하네스가 *일관적*임을 입증하지만
하네스는 절대 작동하지 않습니다. [`the_beast_apex.cpp`](the_beast_apex.cpp)는
괴물: 모델에게 **도구 카탈로그**가 전달되고
ReAct 에이전트 — `llm_call` ⇄ `has_tool_calls`에서 루핑되는 `tool_dispatch`.
그것이 작성하는 하네스는 일관성을 위해 게이트된 다음 **
도구 바인딩**(`ctx.tools` + `engine->own_tools`). 그런 다음 생성된 에이전트
호출할 도구와 시기를 자체적으로 결정합니다.

```console
$ cmake --build build --target cookbook_the_beast_apex
$ ./build/cookbook_the_beast_apex "What is 23 * 19, and the weather in Seoul?"
```

실제 실행 — 실제 자체 복구 루프 실행 후 자율 실행
도구 호출:

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

이것은 한 번의 실행으로 전체 논문입니다. 모델은 `nodes`를 환각했습니다.
스키마를 두 번(`id` 추가, 그 다음 `name` 키 추가) 엄격한 컴파일러의
**소비된 키 계산이 둘 다 거부됨** — 진단이 다시 시작되었습니다.
대화는 세 번째 시도에서 자체적으로 복구되었습니다. 그런 다음
기계로 작성되고 컴파일러로 검증된 에이전트가 라이브 ReAct 루프를 실행하고 호출했습니다.
두 개의 도구가 자동으로 작동합니다. 창의성은 무한하며, 도구 사용은 자율적입니다.
**일관성은 협상할 수 없습니다.**

## Forge — 도구가 부족하면 도구를 작성합니다.

[`the_beast_forge.cpp`](the_beast_forge.cpp)는 정점과 도구입니다
공급망. 주어진 작업은 다음과 같습니다.

1. **DISCOVER** — 기본 MCP stdio 서버를 생성하고 해당 도구를 나열합니다.
실제 MCP 프로토콜(`MCPClient::get_tools`).
2. **FORGE** — 작업에 필요하지만 카탈로그에 부족한 기능의 경우,
건축가 LLM는 이를 구현하는 **Python MCP 서버를 작성**합니다. 우리
디스크에 구체화하고 실행한 후 새 도구를 **재발견**하세요.
MCP 이상. (생성된 서버가 초기화에 실패할 경우 자가 복구합니다.)
3. **AUTHOR** — *결합된* 카탈로그에 ReAct 에이전트를 작성합니다. 삼
언제나처럼 게이트 ​​+ 자체 수리.
4. **SPAWN** — 발견된 모든 *및* 위조 도구를 바인딩하고
자동으로 호출하는 에이전트입니다.

실제 실행 — 모델이 누락된 도구를 작성하고 에이전트가 이를 사용했습니다.

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

두 개의 라이브 MCP 하위 프로세스(스톡 하나, Beast가 *이 실행*을 작성한 것 하나),
각각의 실제 `tools/list`, 실제 ReAct 루프. 저작 모델만
원격.

### 사용자 정의 *노드*도 정의할 수 있나요?

솔직히: NeoGraph 노드 **유형**은 다음을 통해 등록된 C++ 클래스입니다.
`NodeFactory::register_type` — 새로운 원자를 JIT 컴파일할 수 없습니다.
런타임 시 C++ 노드 유형. 그러나 의도는 세 가지 방식으로 다뤄집니다.
Beast는 데이터에서 구동할 수 *있습니다*:

- **복합 노드** — DSL의 `templates` / `use`(M4)는 모델을
재사용 가능한 node/topology 단위를 순수하게 데이터로 정의합니다. 그게 바로
`the_beast.cpp`의 시드가 하는 일.
- **재귀** — `subgraph` 노드는 전체 하네스를 하나의 노드로 포함합니다.
따라서 Beast가 제작한 하네스에는 Beast가 제작한 하위 하네스가 포함될 수 있습니다.
(N급 자기확산).
- **코드를 통한 사용자 정의 동작** — 위의 Forge 패턴은 *런타임*입니다.
모델이 작성한 동작: 모델이 작성한 도구는 디스패치 가능 항목이 됩니다.
단위. 동일한 트릭이 일반적인 `script_node` 유형으로 일반화됩니다(a
모델로 작성된 코드를 실행하는 사전 등록된 C++ 노드)
"LLM가 정의한 논리를 가진 새로운 원자 노드"를 얻는 정직한 방법입니다.

실제로 테이블에서 벗어난 한 가지 일은 새로운 *컴파일된 항목을 내보내는 것입니다.
런타임 시 C++ 노드 클래스*; 모델이 전문화하는 데 필요한 모든 것
동작은 이미 컴파일러의 data/script/subgraph 표면에 존재합니다.
게이트.

## 스크립트 - 범용 카트리지(모델 작성 노드 로직 + 흐름)

위의 모든 변형에서는 모델 작성자가 *도구*(리프 기능)를 사용할 수 있습니다.
[`the_beast_script.cpp`](the_beast_script.cpp)를 사용하면 **노드 로직을 작성할 수 있습니다.
— 도구가 절대적으로 할 수 없는 제어 흐름(`goto`) 포함
express.** `script_node`는 구성이 다음과 같은 사전 컴파일된 C++ 노드입니다.
모델로 작성된 Python; `run()`에서는 노드에 채널 상태를 전달하고
코드가 반환하는 모든 것(`{writes, goto, sends}`)을
그래프. 모델은 노드의 동작 *및* 그래프의 흐름을 다음과 같이 정의합니다.
데이터, 재컴파일 없음.

일관성은 협상할 수 없는 상태로 유지됩니다. 스크립트는 구성에서 계약을 선언합니다.
(`reads` / `writes` / `goto_targets`); 하네스는 3개의 DSL를 통과합니다.
Gates PLUS a Beast-layer **계약 확인**(선언된 쓰기는 다음과 같아야 함)
선언된 채널; goto 대상은 실제 노드여야 함) PLUS a **런타임
선언 외부의 write/goto를 거부하는 래퍼**입니다. 저것
**제로 변경으로 Beast 계층에서 effect/route 보장을 복원합니다.
NeoGraph 코어** — 추가 및 이전 버전과 호환됩니다.

```console
$ cmake --build build --target cookbook_the_beast_script
$ ./build/cookbook_the_beast_script --selftest   # offline, no API key
$ ./build/cookbook_the_beast_script              # live: DeepSeek writes the node logic
```

실시간 실행 - 모델이 자체 제어 흐름을 갖는 카운터 루프를 작성했습니다.
`goto`:

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

`tick`에는 정적 가장자리가 없습니다. 루프는
모델의 Python은 카운터가 3에 도달할 때까지 `{"goto": "tick"}`를 반환한 다음
`{"goto": "__end__"}`. `--selftest`는 다음과 동일한 메커니즘을 실행합니다.
API 키가 없는 고정 하네스이므로 CI는 오프라인에서 이를 실행할 수 있습니다.

**경계(정직).** 컴파일러는 그래프의 *모양*을 증명합니다. 그만큼
계약은 노드의 *표면*(channels/targets일 수 있음)을 증명합니다.
만지다); 스크립트의 *내부 논리*만 입증되지 않았습니다.
하위 프로세스의 `timeout` 및 실행의 `max_steps`. 달리기
모델로 작성된 코드는 임의의 코드 실행입니다. 로컬에는 괜찮습니다.
사용자 중심의 요리책이지만 프로덕션에서는 샌드박스를 원합니다.
통역사. 이는 **빌드 옵션**이며 기본적으로 꺼져 있습니다.

Sandboxed-api는 FetchContent를 통해 제대로 삽입되지 않으므로 사전 구축된 트리를 연결합니다.
(옵션 위의 CMake 주석에 레시피 작성):

```console
$ cmake -S . -B build -DNEOGRAPH_BEAST_SANDBOX=ON -DSANDBOX2_SRC=/path/to/sandboxed-api
$ cmake --build build --target cookbook_the_beast_script
```

이를 켜면 Python은 Google **Sandbox2**에서 실행됩니다.
user/pid/mount/net 네임스페이스, 읽기 전용 FS 보기는
인터프리터 + 두 개의 작업 파일 및 CPU/wall/file rlimits. 요구사항
`libcap-dev`, `libunwind-dev`, C++20 툴체인; Linux/WSL2에서 확인되었습니다.

**효과 계약에서 합성된 Seccomp 정책.** Python의 syscall
안전하게 허용 목록에 추가하기에는 공간이 너무 커서 기본 작업이 그대로 유지됩니다.
허용적 — 그러나 노드의 선언된 *기능*은 syscall을 뺍니다.
`"net"` 기능이 없다고 선언한 노드에는 `socket`/`connect`/`bind`/...가 있습니다.
seccomp 차단(EPERM); `"exec"` 기능 블록 `execve`/`execveat`가 없습니다.
정책은 손으로 작성한 것이 아니라 *선언된 계약서*에서 파생됩니다. 이것
**동일** 아래의 **동일** 파이썬인 부정적인 테스트로 확인되었습니다.
선언된 한도만 다른 샌드박스:

```
caps=[]     (no net cap): {"socket": "SOCKET_BLOCKED:EPERM"}   # seccomp denies the syscall itself
caps=[net]  (net cap):    {"socket": "SOCKET_CREATED"}         # capability grants it
```

따라서 이는 네트워크 네임스페이스 그 이상입니다. `net` 제한이 없으면
`socket()` *syscall*이 실패합니다(netns 위에서 심층적인 방어). 솔직한
범위: **컨테이너 등급 + 계약에서 파생된 seccomp 차단 목록**입니다.
전체 syscall 허용 목록이 아닙니다. 차단되지 않은 syscall을 통한 커널 악용은
아직 포함되지 않았습니다. 더욱 엄격해진 노드당 허용 목록(및 기능 기반)
비밀 중재)는 문서화된 다음 단계입니다.

## 진화 — 밈적(다윈주의 + 라마르크주의)

오프라인 `the_beast.cpp`는 의도적으로 `run_evaluation=false`를 사용하므로
구조적 유효성만으로 선택합니다. 일반 evolution API는 작업을 실행하고 기대한
채널 값과 정확히 비교해 점수를 매길 수도 있습니다.

[`the_beast_evolve.cpp`](the_beast_evolve.cpp)는 대신 연속적인 거리 지표를
사용합니다. 따라서 근접한 결과가 일반 채점기의 단일 출력 불일치 등급에
머무르지 않고 숫자 목표를 향해 개선될 수 있습니다.

- **작업**(실제 작업, 결과 점수 - 구조적 프록시 아님):
목표 숫자를 계산하는 ARITHMETIC PIPELINE를 조립합니다. 5개 작전
노드 존재 — `add2(+2) add3(+3) mul5(*5) mul2(*2) sub1(-1)` — 각 읽기
`acc` 채널(init 0)은 해당 작업을 적용하고 다시 씁니다. 하네스의
대답은 실행 후 `acc`가 보유하는 모든 것입니다. **피트니스 =
  `-(|acc - 20|)`**. *토폴로지*(작업이 어떤 순서로 실행되는지)
숫자를 결정하므로 배선을 발전시키면 계산도 발전합니다.
- **다윈주의**: 무작위 재배선(`all_operators()`) + 선택에 의한 선택
측정된 출력 — 20을 향해 비틀거립니다.
- **Lamarckian**: LLM는 산술을 수행하고 20에 도달하는 체인을 연결합니다.
그리고 획득한 용액을 유전 가능한 씨앗으로 주입합니다.

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

대조가 요점입니다. **맹인 돌연변이는
목표**(9세대까지 5→10→24→19→20, 시행하여 계산);
**LLM는 산술 연산을 수행** — `(0+2)*5*2 = 20` —하고 바로 점프합니다.
주사를 맞으면 답이 나온다. 획득한 솔루션이
유전적 챔피언(`origin 'LLM'`)은 Lamarckian입니다. 블라인드 변형 +
선택은 다윈주의적이다. 둘 다 실행하는 것은 밈적 알고리즘입니다.

정직한 메모: 순수한 다윈주의는 오프라인에서 검증되었으며 결정론적입니다. 그만큼
Lamarckian LLM 호출(deepseek-v4-flash)은 **가끔 불안정**합니다.
스트리밍된 응답은 때때로 구문 분석할 수 없는 상태로 돌아옵니다. 이 경우 실행은
`[Lamarckian] LLM returned no parseable harness` 및 Darwinian 로그를 기록합니다.
라운드를 수행합니다. 마지막 줄은 챔피언의 *실제* 출신을 보고합니다.
일어나지 않은 라마르크식 승리는 결코 없습니다.

## Gate-eval — 일관성 게이트가 실제로 건전한가요?

Beast의 전체 안전 주장은 정적 유효성 검사기가 *소리*라는 것입니다.
일관성 오라클: ERROR는 하네스가 실제로 오류가 있음을 의미합니다.
실행 시간; 오류가 없으면 실행된다는 의미입니다. 그것은 **주장되었지만 측정되지는 않았습니다** —
모든 리뷰어가 가장 먼저 묻는 질문입니다.

[`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp)가 이를 측정합니다. 그것은 실행됩니다
유효성 검사기를 통한 레이블이 지정된 토폴로지 코퍼스(예측 결과) AND
엔진(지상 진실)과 교차 점검을 통해. 오프라인, 결정론적,
키 없음 — 모든 판정이 실행과 일치하는 경우 `exit 0`이므로 **CI가 시작될 수 있습니다.
건강**.

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

테스트 중인 속성:

> 검증인은 ERROR ⟹ 실행 시 그래프 오류를 보고합니다.
> 검증인은 오류를 보고하지 않습니다. ⟹ 그래프가 깔끔하게 실행됩니다.

첫 번째 줄은 *건전성*입니다(정상적으로 실행된 오류 플래그가 지정된 그래프는
건전성 구멍); 깨끗하게 실행되는 경고 플래그 그래프는 게이트를 보여줍니다.
*과도하게* 거부하지 않습니다. E10/E8 클래스 오류는 판정 전용입니다.
빈 경로 맵은 `rend()`(UB)를 역참조합니다. 이는 정확히 결함입니다.
Gate는 방지하기 위해 존재하므로 확인만 하고 실행하지는 않습니다. 이것은
모든 진단을 철저하게 다루지는 않지만 데모 자료입니다.
슬로건의 "게이트는 소리입니다"를 측정된 CI 적용 4/4로 바꿉니다.

## Gate-fuzz — 규모에 따른 보장 및 경계

[`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp)는 5에서 Gate_eval을 푸시합니다.
수천 개의 모호한 케이스에 손으로 라벨을 붙인 케이스 – 그러나 솔직히 말해서. 순진한 움직임
(퍼즈 N 그래프, 인쇄 정밀도 1.0)은 극장이 될 것입니다. **엔진이
컴파일 시 유효성 검사기를 실행하고 오류**가 발생하면 "validator-error ⟹
엔진 결함"은 *구성상* 사실입니다. 따라서 프로그램은 두 가지를 측정합니다.
실제로 유익한 정보입니다.

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

- **레이어 1 - 규모에 따른 일관성.** 무작위로 일관된 시드를 퍼지합니다.
구조적 돌연변이(매달린 가장자리 → E3, 선언되지 않은 쓰기 → E4, 고아 작성자,
드롭 에지 → E7 *경고*, 추가 유효 에지). 2000개 이상의 돌연변이 컴파일러
게이트와 엔진은 결코 동의하지 않습니다. 이것은 건전한 *발견*이 아닙니다.
부분적으로 구성에 따라) — **회귀 보장**입니다. 향후 변경 사항이 있는 경우
정적 게이트와 런타임이 갈라지면 실패합니다.
- **레이어 2 - 경계.** 게이트는 각 노드가 선언한 효과를 신뢰합니다.
계약**. *거짓말*을 하는 노드 — `writes:["out"]`를 선언하지만 실제로는 씁니다.
런타임에 선언되지 않은 `phantom` 채널 — 정적 게이트를 지나 항해합니다.
(500/500) 및 **런타임 `GraphState` 쓰기 가드**는 모든 것을 포착합니다.
(500/500). 그것은 게이트 버그가 아닙니다. 그것은 설계된 노동분업이다.

그 결과는 보증에 대한 *정확한* 진술이며, 이는 보증서보다 더 정직합니다.
의심스러울 정도로 완벽한 혼동 행렬: **정적 게이트는 상대적으로 건전합니다.
정직한 계약, 부정직한 계약에 대한 런타임 백스톱** — 레이어 1 및
레이어 2는 각각 CI를 적용합니다.

공식적인 동반자인 [`SOUNDNESS.md`](SOUNDNESS.md)는 다음을 *증명*합니다.
슈퍼 스텝 실행의 의미, 효과 격자 `(𝒫(Chan), ⊆)`, 게이트
잘 구성된 판단 `⊢ G ok` 및 진행 정리(게이트 통과 그래프)
정직한 계약에서는 결코 잘못이 없습니다) 정직 가설이 필요하다는 것이 입증되었습니다
오류 방지 백스톱으로 런타임 쓰기 보호 기능을 제공합니다. 모든 전제가 확인됩니다
엔진 소스에 대해; `gate_eval`/`gate_fuzz`는 모델의 충실도입니다.
체크 무늬. 여기에 있는 두 개의 하네스는 해당 문서의 Cor 6.4 및 Prop 6.5입니다.

## 볼드윈 — 밈이 맹인을 이기는가, 그리고 상속이 중요한가?

`evolve` 변종은 다윈 돌연변이 + 라마르크 LLM 주입을 보여주었습니다.
모든 리뷰어가 제기한 더 날카로운 연구 질문: **다음과 같은 작업이 있습니까?
맹목적인 진화 AND 단발 해결사 둘 다 정지하지만 밈 조합
승리 - 학습된 특성을 *어떻게* 상속받느냐에 따라 결과가 다음과 같이 변경됩니다.
문헌은 어떻게 예측합니까?** (Whitley 1994; Hinton & Nowlan 1987.)

[`the_beast_baldwin.cpp`](the_beast_baldwin.cpp)가 바로 그 실험입니다.
실제 NeoGraph 하네스. 게놈은 아핀 파이프라인의 배선입니다. 각
스테이지는 평생 학습을 위해 op **또는 왼쪽 플라스틱(`?`)**에 전념합니다.
해결하다. **달릴 때 조립되는 하네스**의 특징은 피트니스입니다.
시작 교차 점검은 빠른 분석 적합성이 컴파일된 것과 동일하다는 것을 증명합니다.
엔진은 200개 토폴로지에 있습니다(게이트 평가와 동일한 원칙). 풍경은
**기만적**: 어디에서나 볼 수 있는 넓은 미끼 언덕(0.85)과 좁은
*학습*만 하는 **그라디언트 없는** 전역 고원(1.0) —
플라스틱 유전자로 둘러싸인 이웃을 찾을 수 있습니다.

```console
$ ./build/cookbook_the_beast_baldwin          # offline, deterministic, no key
engine/analytic cross-check: 200/200 topologies execute exactly as modeled → real harness.
  Darwinian  | assimilated global  6/24 | mean committed → global  25%  decoy  70%
  Baldwinian | assimilated global 21/24 | mean committed → global  74%  decoy  17%
  Lamarckian | assimilated global 23/24 | mean committed → global  78%  decoy   9%
CI gate (blind Darwin near the 25% chance floor, learners assimilate >65% by a
  >25-pt margin, faithful fitness): PASS
```

적합성*이 무엇인지*에 대한 참고 사항: 각 게놈은 실제 NeoGraph로 컴파일됩니다.
토폴로지와 교차 점검을 통해 엔진이 200개를 정확히
분석 모델은 예측합니다 — *기재*는 진짜이고 충실하게 실행된 것입니다.
마구. GA가 최적화하는 *목적*은 기만적인 해밍 환경입니다.
원시 실행이 아닌 배선(역학을 위한 제어된 테스트베드)
산출. 두 가지 사실 모두 모호하지 않고 분명하게 기술되어 있습니다.

서로 다른 기준에 따른 두 가지 조사 결과:

1. **밈이 맹인을 이긴다(강력함 — CI 제어).** 맹인 다윈주의 진화
전역을 최대 25%(기회 최저점)까지만 동화합니다. 왜냐하면 고원이
커밋된 공간 그라데이션이 없으므로 선택은 미끼를 따라가며 트랩됩니다.
학습은 고원을 노출시키고 그것을 ~75% 동화시킵니다. 게이트는 다음과 같이 주장한다.
*마진*(24개 이상의 시드를 의미), 실행당 임계값 개수가 아닙니다.
실행당 횟수는 초기화 운에 따라 결정됩니다. 25% 대 75% 마진이 안정적입니다.
신호.
2. **볼드윈식 제어(측정됨 - 게이트되지 않음).** 볼드윈(상속되지 않음)
학습된 특성) 대 Lamarck(게놈에 기록): 여기 **74% 대 78%**
글로벌 — Lamarckian이 약간 앞서 있습니다. 이는 환경에 대한 *예상* 결과입니다.
이는 기만적이지만 적대적이지는 않습니다(다시 쓰기 속도가
다양성 비용). Whitley의 **반전**(Baldwin > Lamarck)에는
특히 적대적인 풍경; 이 단순한 2피크 구조는
그것은 **요행으로 조정되지 않고 솔직하게 보고됩니다.**
(정말 섬세합니다. 인덱스 기반 타이브레이크가 있는 초기 버전입니다.
반전을 보여주기 위해 *나타났다* — 인공물. 선택 경계에서 연결
이제 시드당 무작위 추첨으로 분류되고 **스윕 전체에서 평균**됩니다.
~74-vs-78 순서는 시드 베이스 전체에서 안정적입니다. 명백한 반전이 그랬다
그 수정에서 살아남지 마십시오.)

리뷰어들이 요구한 결과의 솔직한 모습은 다음과 같습니다. 강력한 주장
(학습 유도 진화는 맹목적인 진화가 할 수 없는 것을 해결합니다) 측정되고
CI 시행; 섬세한 주장(비상속이 상속을 이긴다)이 측정됩니다.
부정적인 결과를 숨기지 않고 이름을 붙여 있는 그대로 보고했습니다.

## Baldwin-adv — 적대적인 풍경 + 실제 언덕 오르기 학습

[`the_beast_baldwin_adv.cpp`](the_beast_baldwin_adv.cpp)는 양쪽을 선명하게 합니다.
이전 실험. 학습은 이제 **실제 지역 검색**입니다(다중 재시작).
가소성 유전자를 넘어 지역적 최적점까지 오르는 것 - 이산적 유사체
LLM가 연결되는 슬롯), 풍경은 진정으로
**적대적**: 경사가 작은 언덕에서 *멀리* 향하는 넓은 미끼 언덕
가파른 글로벌 공. 맹목적인 커밋 공간 검색은 가능성이 없습니다.
미끼 그라데이션 아래로 적극적으로 **기만**합니다.

```console
$ ./build/cookbook_the_beast_baldwin_adv        # offline, deterministic, no key
  Darwinian  | committed → global   5%   decoy  92%
  Baldwinian | committed → global  76%   decoy  19%
  Lamarckian | committed → global  98%   decoy   1%
CI gate (blind deceived onto decoy >50%, both learners solve >60%, faithful): PASS
```

- **밈적 ≫ 맹인(견고함, CI 제어).** 다윈은 미끼에 속아 넘어갑니다.
(~5% 글로벌 / ~92% 미끼); 학습은 단일 게놈의 글로벌 공을 찾습니다
할 수 없습니다(76-98%). 이것은 고원보다 *더 강한* 분리입니다.
기준선은 단순히 눈이 먼 것이 아니라 잘못된 방향으로 인도됩니다. 종자 기반 전체에서 안정적임(Darwin 2-5%,
학습자 76-98%).
- **Baldwin 대 Lamarck: 반전은 NOT를 재현합니다.** Lamarckian
후기입은 안정적인 마진으로 승리합니다(98% 대 76%). 매개변수 스윕
전체 reachable/unreachable 경계(30개 이상의 구성, 3개의 스윕)가 발견됨 **아니요
비상속이 쓰기 저장을 강력하게 능가하는 체제**: 전역이
도달 가능하고 다시 쓰기 속도가 지배적입니다. 그렇지 않은 경우 둘 다 실패합니다.
한계(~3-4pt) 볼드윈 다양성 가장자리. 이것이 솔직한 경험적 답변이다
"Whitley의 Baldwin > Lamarck 반전이 하네스를 통해 재현됩니까?"
토폴로지?" — **아니요**, 이 개별 체제에서는 프로그램이 그렇게 말합니다.
명명된 메커니즘. (Whitley의 반전은 *연속*에서 확립되었습니다.
실제 값 지역 검색을 포함한 다중 모드 기능; 개별 토폴로지 -GA
여기에는 전시하지 않습니다.)

## Baldwin-llm — 모델은 학습 연산자입니다.

위의 기계적 학습자(무작위 추측, 언덕 오르기)는 항상 *슬롯*이었습니다.
LLM 리파이너가 연결됩니다. [`the_beast_baldwin_llm.cpp`](the_beast_baldwin_llm.cpp)
모델이 실제로 추론할 수 있는 작업에 연결하면 `?` 단계 채우기
산술 파이프라인을 사용하여 `acc`가 목표에 도달하도록 합니다. **학습 연산자는 다음과 같습니다.
모델**(`?` 단계에 대한 작업 선택) 피트니스는 조립이다
하네스 *달리기*. Baldwin/Lamarck 토글은 리터럴이 됩니다.

- **Baldwinian**은 모델의 채우기에 점수를 매기지만 `?` 유전자를 유지합니다. 모델은 다음을 수행해야 합니다.
다음 세대에 **다시** 상담을 받으세요. 학습은 유전되지 않습니다.
- **Lamarckian**은 게놈에 채우기를 기록합니다. 즉, `?`가 커밋됩니다.
획득한 특성은 **유전 가능**합니다. 모델에 대해 다시 문의할 필요가 없습니다.

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

관찰 가능한 차이점은 체력(둘 다 목표에 도달함)이 아니라 **게놈입니다.
경제**: 라마르크는 학습자의 작업을 유전에 저장합니다(유전자가 커밋하고 호출합니다).
0으로 떨어지다); 볼드윈은 모든 세대를 다시 배웁니다(유전자는 가소성을 유지하고
높은 상태를 유지하세요). 결정적 **oracle** 학습자(기본값)를 사용하여 오프라인으로 실행하거나
`--llm`와 `OPENROUTER_API_KEY`를 사용하여 **모델**을 학습자로 만듭니다.
해당 호출이 실제 API 호출인 경우 유전은 말 그대로
모델을 한 번만 지불하는 것과 매 세대마다 비용을 지불하는 것의 차이입니다. 이것은
"모델의 수정 사항이 유전됩니까?"의 구체적인 의미는 무엇입니까? -로 표시
추적, 주장되지 않음. (`--llm` 경로에는 네트워크가 필요합니다.
oracle을 실행하고 call/parse 오류에 로그온하므로 데모는 항상 완료됩니다.)

## 소설가 — 전제 입력, 가벼운 소설 길이의 `.txt` 출력

가장 단순하고 진정으로 유용한 글쓰기 도구이자 정직한 형식의
"NovelWriter" 아이디어: 전제를 부여하고 전체 라이트 노벨 크기를 되찾습니다.
일반 텍스트로 원고. [`the_beast_novelist.cpp`](the_beast_novelist.cpp)는
**중간 길을 잃음**에 대한 치료법이 구체화되었습니다. 긴 이야기는 기록되지 *않습니다*
하나의 거대한 맥락에서. **명시적인 스토리 상태**에 대한 작은 그래프입니다.

    channels:  premise · outline · bible · summary · book · idx · total

그래서 각 장은 **압축된 외부화 상태에 비해 신선하게** 생성됩니다.
(개요 비트, 이야기 성경, 연속 요약) 60k를 다시 읽는 대신
문자. 모델은 소설 속 등장인물이 누구인지 *기억*할 필요가 없습니다.
`bible` 채널을 읽습니다.

```console
$ ./build/cookbook_the_beast_novelist "a librarian's returned books whisper futures" 12
harness passed the coherence gate. writing (live — this takes a few minutes)…
  … chapter 1/12 written (4180 chars)
  …
done — 51k characters across 12 chapters.
manuscript: /abs/path/novel_12ch.txt
```

그래프는 `__start__ → planner → writer ⟲`입니다. `planner`는 전제를 다음과 같이 바꿉니다.
개요 + 초기 성경; `writer`는 `idx` 장을 `book`에 쓰고
**`summary` 및 `bible`를 업데이트**하여 다음 반복이 안정적인 상태를 유지하도록 합니다.
**`idx+1 == total`까지 `Command` goto를 사용한 자가 루프**입니다. 효과계약은
선언되었으므로 **일관성 게이트는 단어가 작성되기 전에 연결을 증명합니다** —
모든 스토리 상태 채널은 실제로 소비되며, 매달린 단계는 없습니다.

**일괄 생성, 일괄 진화 — 장별로 독특한 느낌을 줍니다.** 각 장은
사실상 격리된 하위 에이전트(만 기반으로 하는 새로운 `writer` 호출)
공유된 스토리 상태). 그들이 같은 내용을 읽지 못하도록 하기 위해 작가는 다음과 같은 방법을 사용합니다.
장당 **스타일 게놈** — 5차원 스타일 공간의 한 지점(POV · 긴장감 · 기분 ·
렌즈 · 페이싱, 480개 조합). 그것은 미니 GA(`baldwin` 밈적 루프,
목표 대신 *다양성*을 목표로 함): 후보 게놈 배치가 다음과 같이 진화했습니다.
**참신함** 극대화 - 이미 사용된 스타일과의 거리 - 그러면 승자는
커밋하고 `styles_used`에 푸시하여 다음 장이
그것. 오프라인에서 스타일 추적은 결정적이며 눈에 띄게 다양합니다.

```console
  … chapter 1/8  [style: epistolary/journal, present tense, melancholic, dialogue-driven, brisk]
  … chapter 2/8  [style: omniscient third, past tense, wry and whimsical, atmospheric, slow-burn]
  … chapter 4/8  [style: first-person, past tense, melancholic, kinetic action, staccato]
  … chapter 6/8  [style: close third-person, present tense, cold and clinical, kinetic, slow-burn]
```

참신함 검색은 차별성을 극대화합니다(모든 차원을 *보장*하지는 않습니다).
다름) — 정직하고 단일 모델의 단조로운 목소리 실패를 깨기에 충분합니다.
긴 형식.

오프라인(키 없음) **결정적 스텁** planner/writer는 *완전히 동일하게 실행됩니다.
그래프*이므로 파이프라인 — 상태 스레딩, goto 루프, 누적,
`.txt` 출력 — 네트워크 없이도 검증 가능합니다. `OPENROUTER_API_KEY` 스왑 인
실제 산문의 모델. 정직한 범위: 게이트가 *배관*을 증명합니다.
(스토리 상태는 연결되어 있고 연결되어 있습니다), *산문*이 아닙니다 — 내러티브 품질은
모델의 작업과 구조를 넘어서는 연속성은 체커 노드(
런타임 백스톱 패턴), 추가할 확실한 다음 노드로 남겨두었습니다.

## 마찰 표면

- **`trail`**의 E6 "기록했지만 읽지 않음"은 보푸라기로 방출됩니다.
*올바름*: `trail`는 다운스트림이 없는 터미널 출력 채널입니다.
*노드*는 소비합니다. 호스트만 `RunResult::channel`를 통해 다시 읽습니다.
유효성 검사기는 그래프의 채널 표면에 대해 정확합니다.
잘못된. 효과 분석이 작동하고 있음을 보여주기 위해 일부러 표시한 상태로 둡니다.
- **직렬화된 체크포인트 상태는 채널 래핑됨**
(`channel_values["channels"]["trail"]["value"]`), 플랫이 아님 — 데모의
`channel_of()` 도우미가 이를 풀어줍니다. 같은 모양 `RunResult::channel`
읽습니다.
- 코어 잠금 파일은 정교함을 통해 `schema_version: 1`를 유지합니다.
게이트 2를 엄격 모드로 선택하는 것 — DSL 표면에서 작성
절대로 조용히 다운그레이드하지 않습니다. 일관성이 진화 루프를 보장합니다.
에 따라 달라집니다.
