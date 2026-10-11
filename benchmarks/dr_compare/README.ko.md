<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=ko source_sha256=04ec3704b620015a6f954234c5ee83256367bbd43dbd1f84150346e03a6ab528 -->
# dr_compare: 심층 조사 오케스트레이션 비교

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

실행자는 라우터 → 계획 → 연구자 Send 분기 → 종합을 같은 프롬프트와 모델 선택으로 구현합니다. 엔진, 바인딩, 클라이언트, 체크포인트 구현이 다르므로 종단 간 시간은 엔진 비용이나 전송만 분리하지 않습니다. 아래 2026년 4월 관측은 과거 증거이며 현재 cutover 재실행이 아닙니다.

## 파일과 의존성

`dr_neograph.py`, `dr_langgraph.py`, `bench.py`, `bench_mock.py`, `mem_probe.py`, `mem_prod_stack.py`, `sweep.sh`, `_run_single.py`는 실제 호출, 일반 텍스트 모의 작업, 메모리 관측, sweep과 단일 실행 진단을 담당합니다. [Python 바인딩 안내](../../docs/python-binding.md)에 따라 현재 소스와 맞는 wheel을 설치하세요. `CompletionParams`/`OpenAIProvider`를 제공하는 과거 wheel은 현재 API가 아닙니다. Core 소스 빌드에도 외부 SchemaProvider SDK가 필요합니다.
현재 빌드에는 SDK `0.3.0`, interface revision/shared generation 6 및 병합된 SchemaProvider PR #21(`83112573ba59e3b561fc33c22394638be7aa5294`)과 일치하는 wheel/native 빌드가 필요합니다. 현재 통합 검증은 대기 중입니다. 이 비교 runner는 내장 Deep Research 복구 경로와 별개입니다.

워크플로는 requests, LangGraph, langchain-openai를 가져오고 메모리 관측에는 psutil이 필요합니다. PostgreSQL 모드에는 해당 체크포인트 패키지와 데이터베이스가 필요합니다. `mem_prod_stack.py`는 각 스택의 웹/DB/관측 패키지도 가져오므로 엔진만의 RSS 측정이 아닙니다.

## 현재 환경 설정

| Variable | Default | Purpose |
|---|---|---|
| `LLM_MOCK_MS` | `-1` | 0 미만은 실제 호출, >=0은 sleep을 포함한 텍스트 작업이며 Provider/outcome이 없습니다. |
| `MOCK_SEARCH` | `0` | 1은 Crawl4AI 대신 고정 증거를 반환합니다. |
| `FANOUT` | `5` | 연구자 분기 수/한도. |
| `USE_INMEMORY_CP` | `0` | 1은 메모리 체크포인트, 모의 모드도 메모리를 선택합니다. |
| `NG_TRANSPORT` | `http-chat` | NG: http-chat 또는 http-responses. WebSocket 없음; Responses는 다른 API입니다. |
| `NG_WORKER_COUNT` | `4` | NG fan-out 작업자 수. |
| `DR_MODEL` | `gpt-5.4-mini` | 양쪽 실제 호출의 명시적 모델. |
| `NG_EXAMPLE_MAX_TOKENS` | `1600` | NG/LG 양쪽의 호출별 출력 상한. 자동으로 늘리지 않습니다. |
| `NEOGRAPH_PG_DSN` | `empty` | NG PostgreSQL DSN, 없으면 메모리 사용. |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG PostgreSQL DSN 재정의. |
| `CRAWL4AI_URL` | `empty` | 검색 서비스, 없으면 모의 검색 외에는 검색 불가. |

`OPENAI_API_BASE`는 실제 호출의 승인된 origin/gateway prefix를 선택하며 NG의 `NG_PROVIDER_DESCRIPTOR`는 전체 descriptor를 지정할 수 있습니다. 자격 증명과 CA는 descriptor가 아니라 런타임 옵션(`OPENAI_API_KEY`, `NG_EXAMPLE_CA_FILE`)에 둡니다. NG 기본 HTTP Chat은 LG Chat API와 맞춥니다. HTTP Responses는 다른 API 비교입니다. HTTP/2는 libcurl과 상대 서버에 달려 있으며 이 실행자로 멀티플렉싱이나 고정 연결 수를 입증하지 않습니다.

## 과거 관측: 2026-04-26

1. 모의 LLM, FANOUT=5의 기록 중앙값은 NG 1.0 ms, LG 5.9 ms였습니다(해당 작업의 5.9× 비율).
2. 첫 원격 모델 실행은 NG p50 23.90 s(sd 5.90 s), LG 21.95 s(sd 1.23 s)였습니다.
3. 과거 연결 진단은 모델 호출 7회인 NG 실행에서 `connect()` 21회를 기록했습니다. 이 수만으로 TLS 세션 수, HTTP 버전, 페이로드 동등성을 입증하지 않습니다.
4. 과거 `6da4810` / `bc2ab4f` 풀링 수정에 관련된 기록은 NG p90 35.34 s → 25.28 s, sd 5.90 s → 1.28 s입니다. 당시 공급자 구현은 제거됐으며 현재 typed SchemaProvider는 외부 SDK/libcurl을 사용합니다.
5. FANOUT=50, LLM_MOCK_MS=100, NG_WORKER_COUNT=50 실험은 NG 307 ms, LG asyncio 711 ms를 보고했습니다. 별도 작업이며 보편적인 서버 용량 결과가 아닙니다.

NeoGraph에 HTTP/2 지원이 아직 필요하다는 과거 주장은 현재와 맞지 않습니다. 이 기록은 새 SDK, 현재 wheel, 다른 플랫폼이나 원격 추론 가속을 검증하지 않습니다. 모의 모드는 오케스트레이션과 설정한 sleep만 측정하며 공급자 증거를 만들지 않습니다. 실제 모드는 typed 소유 outcome에서 표시 텍스트를 추출하지만 native replay, portable history export, 공급자 보고량과 예산 부과량 정산을 벤치마크하지 않습니다.

## 새 측정 실행

아래 모의 명령은 원격 호출과 영속성을 사용하지 않습니다. 새 결과에는 소스/SDK/wheel 리비전, Python/의존성 버전, 호스트 제약, 작업자 수, 예열, 반복 수, 체크포인트 모드, 실패 수를 기록하세요. 과거 파일은 유지하세요.
두 harness는 빈 보고서를 거부하고 예열/측정 실행 중 하나라도 실패하면 nonzero로 종료합니다. 시간 통계에는 성공 표본만 포함합니다. 통과로 보고하려면 `Failed runs`가 0이어야 합니다. 저장소 root에서 `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py`로 공급자 없는 집계 회귀 테스트를 실행합니다.

`bench_mock.py`는 미설정 `LLM_MOCK_MS`를 0으로 설정하고 import 전에 모의 검색/메모리 체크포인트를 강제합니다. 음수 지연이나 이미 실제 모드로 import된 실행자는 dispatch 전에 거부하므로 상속된 키로 유료 호출하지 않습니다. 측정 반복은 양수, 예열은 0 이상이어야 합니다. LG는 실제 텍스트 블록만 추출하며 빈 응답/계획을 성공 표본으로 취급하지 않습니다.
실제 경로에서는 잘림/거부/tool/unknown 종료를 완료 보고서로 측정하지 않습니다. NG 오류는 원래 소유 Outcome, LG 오류는 원래 response를 보존합니다. 공급자 없는 회귀 테스트는 소비자 투영 정책만 검증하며 transport/SDK/live 모델 증거가 아닙니다.

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
env -u NG_WORKER_COUNT LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

fan-out 중첩을 관측하려면 `LLM_MOCK_MS=100`으로 다시 실행하고, `FANOUT=5`, 예열/반복/체크포인트 설정을 같게 유지한 채 unset `NG_WORKER_COUNT`(기본 4)와 명시적 `NG_WORKER_COUNT=1`을 비교하세요. stderr와 실패를 기록하세요. 직렬 fan-out 경고가 없다는 사실만으로 중첩을 입증하지는 않습니다. 모델 없는 작업이지 유료 공급자 증거가 아닙니다.

다음 원격 모델 명령은 요금을 발생시킬 수 있습니다. 자격 증명을 의도적으로 설정하세요. 메모리 체크포인트를 사용하며 PostgreSQL 비교에는 일치하는 DSN, 패키지, 별도의 내구성 범위 기록이 필요합니다. 시스템 호출이나 패킷 캡처만으로 의미적 동등성이나 공급자 청구를 입증하지 않습니다.

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```

이 명령은 지출 한도가 아닙니다. `FANOUT=5`인 조사 질의는 각 측에서 최대 7개의 논리 모델 호출(plan, 연구자 5개, synthesis)을 수행합니다. 양쪽 예열 2회와 측정 5회는 98개의 논리 호출입니다. 현재 NG SDK 기본값은 transport retry를 끄며 LG는 `max_retries=0`을 명시합니다. 양쪽 출력 상한은 `NG_EXAMPLE_MAX_TOKENS=1600`이며 명시적 값은 그대로 유지합니다. 호출 수와 최대 출력 허용량 합은 벤치마크 외부에서 별도로 승인/예약해야 합니다. 승인된 모델, endpoint, 자격 증명, 검색 서비스만 사용하세요. 줄인 cohort는 과거 여러 반복 측정과 별도로 표시하세요.

유한한 유료 공급자 진단(지연 비교 아님)은 다음과 같습니다.

```sh
# From benchmarks/dr_compare, only after a separate spending reservation.
: "${OPENAI_API_KEY:?Requires an intentionally authorized hosted credential}"
env -u NG_WORKER_COUNT LLM_MOCK_MS=-1 MOCK_SEARCH=1 USE_INMEMORY_CP=1 \
  FANOUT=5 NG_TRANSPORT=http-chat NG_EXAMPLE_MAX_TOKENS=8192 \
  python bench.py --only neograph --warmup 0 --iters 1
```

최대 모델 호출 7회, 설정한 출력 토큰 57,344개이며 입력 토큰은 별도입니다. 실제 웹 검색이 없으므로 실제 검색 qualification이 아니라 유료 공급자/모의 검색으로 표시해야 합니다. 통과하려면 exit 0, 성공 표본 1개, 비어 있지 않은 보고서, `Failed runs: 0`이 필요하며 계획, 답변, 보고서 중 하나라도 실패하거나 비어 있으면 실패입니다. 8,192토큰 허용량은 명시적 진단 선택이지 기본값 인상, 가격 추정, 지출 승인이 아닙니다. 승인된 Crawl4AI 서비스가 실행 중이면 `MOCK_SEARCH=0`으로 같은 모델 호출 상한에서 실제 검색을 실행합니다.
