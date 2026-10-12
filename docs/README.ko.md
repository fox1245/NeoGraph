<!-- neograph-i18n: source=docs/README.md locale=ko source_sha256=21d29bec5271896709470458f9bf7b867b830a0685740d7846d48feba6c96c96 -->
# NeoGraph 문서

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 페이지는 이 디렉터리의 문서와 저장소 곳곳의 주요 README를 나열하고, 각 문서를 누가 읽으면 좋은지 알려 줍니다. 처음이라면 첫 번째 표를 순서대로 읽으세요. 클래스나 함수를 찾는다면 레퍼런스로 가세요. 마지막 두 표는 설계 기록과 측정 자료입니다. 코드가 왜 지금 모양인지 설명하는 자료이며 사용 안내서는 아닙니다.

## 시작하기

| 문서 | 읽는 이유 |
|---|---|
| [README](../README.md) | NeoGraph가 무엇인지, API 키 없이 실행되는 Python·C++ 퀵스타트, 빌드 옵션. |
| [Concepts](concepts.md) | 채널, 리듀서, 노드, 엣지, `Send`, `Command`, 체크포인트, 인터럽트, 스트리밍. 그래프를 만들 때 쓰는 순서대로 설명합니다. |
| [C++ 예제](../examples/README.md)와 [Python 예제](../bindings/python/examples/README.md) | 바로 실행할 수 있는 프로그램. 대부분 API 키가 필요 없으며, 키가 필요한 예제는 각 목록에 표시되어 있습니다. |
| [문제 해결](troubleshooting.md) | 빌드, 설치, 실행, Python 오류와 각각의 원인·해결 방법. |

## 가이드

| 문서 | 이럴 때 읽습니다 |
|---|---|
| [Python 바인딩](python-binding.md) | Python에서 NeoGraph를 쓰고 싶을 때: 그래프, 타입이 지정된 provider 요청, Program, Hook, 체크포인트, 프로토콜 클라이언트. |
| [Program 가이드](program-guide.md) | 그래프를 병렬로 실행하고, 실패한 노드를 처리하고, 사람의 승인을 기다렸다가 재개하며, 예산 안에서 동작하는 제한된 JavaScript 프로그램을 작성하고 싶을 때. |
| [Program 그래프 기능](program-graphs.md) | Program의 `define()` 그래프로 표현할 수 있는 것을 찾아볼 때: 메타데이터, `any_of` 조인, 노드별 재시도, 실패 엣지와 종료, 시도별 타임아웃, 건전성 검사. |
| [진단](diagnostics.md) | 컴파일러나 런타임의 진단 코드(`P_JS_*`, `P_CORE_*`, `P_RUNTIME_*`)와 해결 방법을 찾을 때. |
| [MCP](mcp.md) | 그래프를 MCP 도구로 공개하거나, 그래프에서 MCP 서버의 도구를 호출하고 싶을 때. |
| [A2A](a2a.md) | 그래프를 Agent2Agent 에이전트로 공개하거나, 원격 A2A 에이전트를 호출하고 싶을 때. |
| [ACP](acp.md) | 에디터가 stdio로 실행할 수 있는 Agent Client Protocol 에이전트로 그래프를 돌리고 싶을 때. |
| [Harness MCP](HARNESS_MCP.md) | Harness를 쓰고 싶을 때: 제한된 다중 워커 워크플로를 컴파일해 실행하고, 리플레이와 검토를 지원하는 MCP 도구 6개. |
| [비동기 가이드](ASYNC_GUIDE.md)와 [동시성](concurrency.md) | 실행기를 고르고, 엔진 하나를 여러 세션이 공유하고, 비동기 provider와 도구를 작성하고, 실행을 취소하고 싶을 때. |
| [런타임 컨텍스트와 엄격한 개입 계층](STRICT_RUNTIME_INTERPOSITION.md) | 필수 컨텍스트, Hook, provider 디스패치 영수증을 모델의 재량에 맡기지 않고 강제하고 싶을 때. |
| [마이그레이션 가이드](migration-v0.4-to-v1.0.md) | 이전 노드 API(`execute`와 그 변형)로 작성한 코드를 `run(NodeInput)`으로, `CompletionParams`와 `complete`로 작성한 코드를 타입이 지정된 provider 요청으로 옮길 때. |
| [Valgrind와 새니타이저](VALGRIND.md) | NeoGraph의 메모리 검사와 새니타이저 검사를 실행할 때. |

## 레퍼런스

| 문서 | 이럴 때 읽습니다 |
|---|---|
| [C++ 레퍼런스](reference-en.md) | 타입, 함수, 실패 코드를 찾아볼 때. MCP, gRPC, A2A, ACP의 프로토콜 실패 매핑 표도 여기에 있습니다. |
| [생성된 API 레퍼런스](doxygen-mainpage.md) | `include/neograph/`의 공개 헤더를 Doxygen 메인 페이지에서 시작해 볼 때. |
| [ABI 정책](ABI_POLICY.md) | NeoGraph를 업그레이드할 때 내 코드를 다시 빌드해야 하는 경우를 알고 싶을 때. |
| [스키마](../schemas) | NeoGraph가 주고받는 JSON을 검증할 때: Core 정의, Hook, 컨텍스트 아티팩트, 영수증. |
| [변경 이력](../CHANGELOG.md), [보안 정책](../SECURITY.md), [로드맵](../ROADMAP_v1.md) | 무엇이 바뀌었는지 보거나, 취약점을 신고하거나, 이전 주기에서 남긴 설계 제안을 읽을 때. |

## 설계 기록

Program 계층과 QuickJS 작성 인터페이스의 설계를 설명하는 문서입니다. 런타임을 확장하거나 정확한 계약이 필요할 때 읽으세요. 사용하기만 한다면 읽지 않아도 됩니다.

| 문서 | 주제 |
|---|---|
| [v1 아키텍처](V1_ARCHITECTURE.md)와 [v1 마이그레이션 계획](V1_MIGRATION_PLAN.md) | Core와 Program의 아키텍처, 구현 순서. 영어로만 제공합니다. |
| [QuickJS 제어 아키텍처](QUICKJS_CONTROL_ARCHITECTURE.md), [마이그레이션](QUICKJS_CONTROL_MIGRATION.md), [실행 프로파일](QUICKJS_EXECUTION_PROFILES.md), [공개 작성 경계](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md) | 내장 JavaScript 작성 인터페이스와 그것이 할 수 있는 일. 영어로만 제공합니다. |
| [QuickJS 마이그레이션 완료](QUICKJS_MIGRATION_COMPLETION.md) | 해당 마이그레이션을 마무리하기 위한 과거의 실행 절차서. 계획 기록입니다. |
| [Program 기능 컨텍스트](PROGRAM_CAPABILITY_CONTEXT.md) | 노드가 승인된 설정에 선언된 기능만 보게 하는 방법. |
| [재귀적 Harness](PROGRAM_RECURSIVE_HARNESSES.md) | 부모, 자식, 손자 에이전트 트리. 에이전트마다 Program 버전과 예산이 따로 있습니다. |
| 자식 합성: [계약](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md), [영속화](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md), [복구](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | Program이 호스트에 생성된 자식 소스의 승인을 요청하는 방법과, 재시작 후에도 그 상태가 유지되는 방식. |
| [자기 진화 에이전트 컨트롤러](SELF_EVOLVING_AGENT_CONTROLLER.md) | 호스트의 승인 아래 자기 그래프를 고쳐 쓰는 컨트롤러. 영어로만 제공합니다. |
| [비동기 3단계 설계](ASYNC_STAGE3_DESIGN.md) | asio 기반 비동기 실행기의 설계. |
| [DSL 기능 평가](DSL_CAPABILITY_EVAL.md)와 [모델의 Program 합성 증거](model-program-synthesis-evidence.md) | 모델이 올바른 Program을 쓸 수 있는지 시험한 결과와 날짜. |

## 측정 자료

| 문서 | 주제 |
|---|---|
| [성능 심층 분석](performance-deep-dive.md)과 [벤치마크](../benchmarks/README.md) | 엔진 오버헤드와 동시성 측정값과 각 실행의 구성. 보장하는 값은 아닙니다. |
| [Program 비용 측정](PROGRAM_COST_MEASUREMENT.md)과 [2026-09-07 기준선](PROGRAM_COST_BASELINE_2026-09-07.md) | Program 부기 비용과 저장소 비용을 측정하는 방법. |
| [명령 헤드](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md), [정규 JSON](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md), [SHA-256](PROGRAM_SHA256_OPTIMIZATION.md) 최적화 | 이 비용을 줄이려고 무엇을 바꿨는지, 그리고 전후 수치. |
