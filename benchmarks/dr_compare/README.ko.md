<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=ko source_sha256=555ae454b4d6f5e579ec7eacb9d2f1b9c28f4b81e71cac671fd184c3c13e2de1 -->
# dr_compare: 심층 조사 오케스트레이션 비교

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

러너는 라우터 → 계획 → 연구자 Send 분기 → 종합 흐름을 같은 프롬프트와 모델 선택으로 구현합니다. 하지만 엔진, 바인딩, 클라이언트, 체크포인트 구현이 서로 달라서 종단 간 시간만으로는 엔진 비용이나 전송만을 따로 가려낼 수 없습니다. 아래의 2026년 4월 결과는 과거 기록이며, 현재 SDK로 다시 실행한 결과가 아닙니다.

## 파일과 의존성

`dr_neograph.py`, `dr_langgraph.py`, `bench.py`, `bench_mock.py`, `mem_probe.py`, `mem_prod_stack.py`, `sweep.sh`, `_run_single.py`는 실제 호출, 일반 텍스트 mock 작업, 메모리 측정, 스윕(sweep), 단발성 진단을 맡습니다. [Python 바인딩 가이드](../../docs/python-binding.md)에 따라 현재 소스와 맞는 wheel을 설치합니다. `CompletionParams`/`OpenAIProvider`가 있는 예전 wheel은 현재 API가 아닙니다. 소스에서 빌드할 때는 Core만 빌드하더라도 외부 SchemaProvider SDK가 필요합니다.
현재 빌드에는 SDK `0.3.4`, 인터페이스 리비전/공유 라이브러리 세대 6과 SchemaProvider `maint/interface6` 커밋 `0437853529b5932e7097a38629a95779d08f03aa`에 맞는 wheel/네이티브 빌드가 필요합니다. 이 비교 러너는 내장 Deep Research 복구 경로와는 별개입니다.

워크플로는 requests, LangGraph, langchain-openai를 import하고, 메모리 측정에는 psutil을 씁니다. PostgreSQL 모드에는 이에 맞는 체크포인트 패키지와 실행 중인 데이터베이스도 필요합니다. `mem_prod_stack.py`는 이름이 붙은 스택마다 웹/데이터베이스/관측성 패키지를 추가로 import하므로, 엔진만 놓고 RSS를 재는 측정기가 아닙니다.

## 현재 환경 변수

| 변수 | 기본값 | 용도 |
|---|---|---|
| `LLM_MOCK_MS` | `-1` | 0보다 작으면 실제 호출을 한다. 0 이상이면 sleep을 포함한 일반 텍스트 노드 작업을 하며, Provider와 outcome은 없다. |
| `MOCK_SEARCH` | `0` | 1이면 Crawl4AI를 건너뛰고 미리 준비한(canned) 증거를 반환한다. |
| `FANOUT` | `5` | 연구자 분기의 개수이자 상한이다. |
| `USE_INMEMORY_CP` | `0` | 1이면 메모리 체크포인트를 쓴다. mock 모드에서도 메모리 체크포인트를 쓴다. |
| `NG_TRANSPORT` | `http-chat` | NG의 전송 방식으로 http-chat 또는 http-responses를 고른다. WebSocket은 없다. Responses를 고르면 wire API가 달라진다. |
| `NG_WORKER_COUNT` | `4` | fan-out에 쓰는 NG 워커 수이다. |
| `DR_MODEL` | `gpt-5.4-mini` | 양쪽의 실제 호출에 쓰는 모델이며, 명시적으로 지정한다. |
| `DR_PLAN_TEMPERATURE` | 미설정 | 양쪽 플래너의 temperature이다. 미설정이면 보내지 않는다. `gpt-5.4-mini` 같은 추론 모델은 temperature를 거부하기 때문이다. |
| `NG_EXAMPLE_MAX_TOKENS` | `1600` | NG와 LG 양쪽의 호출당 출력 상한이다. 자동으로 올리지 않는다. |
| `NEOGRAPH_PG_DSN` | `empty` | NG의 PostgreSQL DSN이다. 없으면 메모리 체크포인트로 대체한다. |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG의 PostgreSQL DSN을 덮어쓴다. |
| `CRAWL4AI_URL` | `empty` | 검색 서비스 주소이다. 서비스가 없으면 mock 검색이 아닌 한 검색을 쓸 수 없다. |

`OPENAI_API_BASE`는 실제 호출에 쓸 승인(admission)된 origin/게이트웨이 접두사를 고르고, NeoGraph 쪽에서는 `NG_PROVIDER_DESCRIPTOR`로 전체 descriptor를 줄 수 있습니다. 자격 증명과 사용자 지정 CA는 descriptor 데이터가 아니라 런타임 옵션(`OPENAI_API_KEY`, `NG_EXAMPLE_CA_FILE`)에 넣습니다. NG의 기본 경로는 HTTP Chat이며 LG의 Chat wire API와 같습니다. HTTP Responses를 고르면 일부러 다른 API를 비교하게 됩니다. HTTP/2를 쓸 수 있는지는 libcurl과 상대 서버에 달려 있으며, 이 러너들은 멀티플렉싱이나 고정된 연결 수를 증명하지 않습니다.

## 과거 결과: 2026-04-26

1. mock LLM, FANOUT=5: 기록된 중앙값은 NeoGraph 1.0 ms, LangGraph 5.9 ms였습니다(이 워크로드에서 5.9배).
2. 호스팅 모델을 쓴 첫 라운드: NG p50 23.90 s (sd 5.90 s), LG 21.95 s (sd 1.23 s).
3. 당시 연결 진단에서는 모델 호출이 7번인 NG 실행에서 `connect()` 시스콜 21회가 기록되었습니다. 이 횟수만으로 TLS 세션이 21개였다거나 HTTP 버전, payload가 같았다는 것이 증명되지는 않습니다.
4. 커밋 `6da4810` / `bc2ab4f`로 확인된 과거의 풀링 변경은 NG p90 35.34 s → 25.28 s, sd 5.90 s → 1.28 s와 연관되었습니다. 이 변경은 이미 제거된 provider 구현에 관한 것이며, 현재의 typed SchemaProvider는 외부 SDK/libcurl 런타임을 씁니다.
5. 워커 수 실험에서는 FANOUT=50, LLM_MOCK_MS=100, NG_WORKER_COUNT=50일 때 NG 307 ms, LG asyncio 711 ms로 보고되었습니다. 이는 별도의 워크로드이며 서버 용량으로 일반화할 수 있는 결과가 아닙니다.

NeoGraph에 아직 HTTP/2 지원이 필요하다는 예전 주장은 더 이상 유효하지 않습니다. 기록된 시간은 새 SDK와 현재의 Python wheel 이전에 측정한 값이므로, 다른 플랫폼에는 적용되지 않으며 원격 추론의 속도 향상을 예측하지도 않습니다. mock 모드가 측정하는 것은 오케스트레이션과 설정한 sleep뿐이며 provider 증거는 만들지 않습니다. 실제 모드는 typed 소유 outcome에서 화면에 보이는 텍스트를 추출하지만, 네이티브 재생(replay), 이식 가능한 기록(history) 내보내기, provider 보고/예산 청구 정산(settlement)은 벤치마크하지 않습니다.

## 새 측정 실행하기

아래 mock 명령은 호스팅 모델 호출과 영속성을 쓰지 않습니다. 새 결과와 함께 소스/SDK/wheel 리비전, Python과 의존성 버전, 호스트의 제한, 워커 수, 워밍업, 반복 횟수, 체크포인트 모드, 실패 횟수를 기록합니다. 과거 파일은 수정하지 않고 그대로 둡니다.
두 하네스는 빈 보고서를 거부하며, 워밍업이나 측정 실행이 하나라도 실패하면 0이 아닌 코드로 종료합니다. 시간은 성공한 샘플만으로 계산합니다. 측정 그룹을 통과로 보고하려면 `Failed runs`가 0이어야 합니다. provider를 쓰지 않는 집계 회귀 테스트는 저장소 루트에서 `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py`로 실행합니다.

`bench_mock.py`는 값이 없을 때 `LLM_MOCK_MS=0`을 설정하고, 어느 러너를 import하기 전에 mock 검색과 메모리 체크포인트를 강제로 켭니다. 지연이 음수이거나 캐시된 non-mock 러너가 있으면 디스패치 전에 거부하므로, 상속된 자격 증명으로 유료 작업이 선택되는 일은 없습니다. 두 하네스 모두 측정 반복 횟수는 양수, 워밍업 횟수는 0 이상이어야 합니다. LG는 비어 있거나 풍부한(rich) 응답을 문자열로 바꾸지 않고 실제 텍스트 블록을 그대로 보존합니다. 비어 있는 중간 응답이나 계획은 성공한 벤치마크 샘플이 아니라 실패입니다. 실제 호출 경로도 잘리거나(truncated), 거부되거나(refused), 도구 호출이거나(tool), 알 수 없는(unknown) 종료 상태를 완료된 보고서로 계산하지 않고 거부합니다. NG 오류는 원래의 소유 Outcome을 그대로 유지하고, LG 오류는 원래의 응답을 유지합니다. provider를 쓰지 않는 회귀 테스트는 소비자 쪽의 투영 정책만 따로 검사하며, 전송/SDK나 호스팅 모델에 대한 증거는 아닙니다.

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
env -u NG_WORKER_COUNT LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

fan-out이 겹쳐 실행되는지 보려면 `LLM_MOCK_MS=100`으로 다시 실행하고, `NG_WORKER_COUNT`를 설정하지 않았을 때(기본값 4)와 `NG_WORKER_COUNT=1`로 명시했을 때를 비교합니다. 이때 `FANOUT=5`, 워밍업, 반복 횟수, 체크포인트 모드는 같게 둡니다. stderr와 실패를 함께 기록합니다. 직렬 fan-out 경고가 없다는 사실만으로 겹쳐 실행되었다는 증거가 되지는 않습니다. 이 워크로드들은 모델을 쓰지 않으며 유료 provider의 증거가 아닙니다.

다음 호스팅 모델 명령은 비용이 발생할 수 있습니다. 자격 증명은 의도한 경우에만 설정해야 하며, 이 명령은 메모리 체크포인트를 씁니다. PostgreSQL로 비교하려면 맞는 DSN과 패키지 설정이 필요하고, 영속성의 범위도 따로 기록해야 합니다. 시스콜 추적이나 패킷 캡처만으로는 의미가 같다는 것도, provider 청구액도 입증되지 않습니다.

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```

이 명령은 지출 한도가 아닙니다. `FANOUT=5`이면 조사 질의 하나가 한쪽당 논리적 모델 호출을 최대 7번(계획 1번, 연구자 5번, 종합 1번) 수행합니다. 워밍업 2번과 측정 5번을 양쪽에서 실행하면 논리적 호출이 최대 98번 나갈 수 있습니다. 현재 NG SDK의 기본값은 전송 재시도를 끄는 것이고, LG는 `max_retries=0`을 명시적으로 설정합니다. 호출 수와 설정된 출력 상한의 합계에 대한 한도는 벤치마크 바깥에서 정해 두어야 합니다. 양쪽 모두 기본값으로 `NG_EXAMPLE_MAX_TOKENS=1600`을 쓰며, 명시적으로 준 값은 그대로 적용됩니다. 사용해도 되는 모델, 엔드포인트, 자격 증명, 검색 서비스만 사용합니다. 규모를 줄인 대표 측정 그룹은 과거의 여러 번 반복한 측정과 다르므로 따로 표시해야 합니다.

유한한 비용이 드는 provider 진단(지연 시간 비교가 아님)은 다음과 같습니다.

```sh
# From benchmarks/dr_compare; this makes paid provider calls.
: "${OPENAI_API_KEY:?Requires an intentionally authorized hosted credential}"
env -u NG_WORKER_COUNT LLM_MOCK_MS=-1 MOCK_SEARCH=1 USE_INMEMORY_CP=1 \
  FANOUT=5 NG_TRANSPORT=http-chat NG_EXAMPLE_MAX_TOKENS=8192 \
  python bench.py --only neograph --warmup 0 --iters 1
```

이 명령은 모델 호출이 최대 7번이고 설정된 출력 토큰은 57,344개이며, 입력 토큰은 별도입니다. 실제 웹 검색은 없으므로 실제 검색을 실행한 것이 아니라 유료 provider/mock 검색으로 표시해야 합니다. 통과하려면 종료 코드 0, 성공한 샘플 1개, 비어 있지 않은 보고서, `Failed runs: 0`이 필요하며, 계획, 답변, 보고서 중 하나라도 실패하거나 비어 있으면 실패입니다. 8,192토큰 허용량은 진단을 위해 명시적으로 고른 값이지, 기본값을 올린 것도, 비용 추정도, 지출 승인도 아닙니다. 승인받은 Crawl4AI 서비스가 실행 중이면 `MOCK_SEARCH=0`으로 같은 모델 호출 상한에서 실제 검색을 확인할 수 있습니다.
