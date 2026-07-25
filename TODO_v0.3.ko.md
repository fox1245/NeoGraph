<!-- neograph-i18n: source=TODO_v0.3.md locale=ko source_sha256=ee4fb3a3df1268f70a1cf98004b3825f972ee0f84ad3de77e0887a39e7bb80c5 -->
# v0.3 후속 작업

**Languages:** [English](TODO_v0.3.md) | [한국어](TODO_v0.3.ko.md) | [日本語](TODO_v0.3.ja.md) | [简体中文](TODO_v0.3.zh-CN.md)

원래 FastAPI SSE 채팅 데모 피드백(2026-05-04)에서 나온 목록이다.
v0.3.0에서 취소 전파(cancel propagation)를 배포했으며, 이 파일은 남아 있는
개념 모델(mental-model)과 사용 편의성(ergonomics) 간극을 추적한다.

## ✅ v0.3.1에서 마무리 (2026-05-04, 세션 2)

1. **같은 에 대한 자동 체크포인트 재개** — 선택적 `RunConfig.resume_if_exists`
 `engine.run/run_async/run_stream`   도입. 활성화되고 체크포인트 저장소가
 `thread_id` `input` 설정되어 있으면 이 의
 `messages`  최신 체크포인트를 불러온 뒤 을 채널 리듀서(reducer)를 통해
 `False`  덧붙인다(APPEND 리듀서의 는 새 턴과 함께 늘어난다).
 `tests/test_resume_if_exists.cpp`  기본값 는 기존의 새 시작 동작을 유지한다.
 `bindings/python/tests/test_resume_if_exists.py`  테스트:  (6) +
    (6).
2. **스트리밍 전용 노드에 더 나은 오류 메시지** — `GraphNode.execute()`
 `execute_stream` `execute_full_stream` (Python 기본 클래스)가 이제 하위 클래스 MRO를
 `NotImplementedError`  탐색해  / 이 정의되어 있으면
 `engine.run_stream() / run_stream_async()`  에 를
 `bindings/python/tests/test_streaming_only_error_hint.py`  가리키는 힌트를 포함한다. 테스트:
    (4).
3. **토큰 방출(emit) 도우미** — `from neograph_engine.streaming import emit_token`
 `GraphEvent`  이 기존 4줄의
 `emit_token(cb, self._name, token)`   생성 의식을 으로
 `bindings/python/tests/test_emit_token_helper.py`  압축한다. 테스트:
    (5).
4. **README "LangGraph와 다른 점" 섹션** — Python 바인딩 섹션 아래에
 `update_state(channel_writes)` `get_state` 추가. 선택적 다중 턴 재개,  모양,
 `Provider.complete`   중첩 사전, Python  전용, 스트리밍 전용
 `run_stream*` `emit_token` 노드는  필요, 새  도우미를 설명한다.
 `resume_if_exists` `RunConfig` 도  표에 추가.
7. **병렬 / Send 분기에 대한 취소 전파** — 정적 병렬은 공유 부모
   상태(shared parent state)를 통해 확인(이미 v0.3.0에서 정상).
   다중 Send 간극을 발견하고 수정: 동적 팬아웃(dynamic fan-out) 작업자가
 `GraphState` `serialize/restore` 로 격리된 를 만들었지만
 `run_cancel_token_`  이 채널 집합 바깥에 있어 누락되어 — 취소된 실행이
   Send로 생성된 분기에서도 여전히 비용 누수를 일으켰다.
 `GraphState::run_cancel_token_shared()`  와
 `NodeExecutor::run_sends_async`  가 이제 격리된 각 로
 `send_state`  전달한다. 테스트:
 `tests/test_cancel_token_propagation.cpp`   (3 — 정적 병렬,
   다중 Send, 팬아웃 중간 중단).

## 상태: v0.3.x 피드백 마무리

v0.3.x 피드백 배치(FastAPI SSE 채팅 데모 + 사용 편의성 개선)에서
나온 엔진에 영향을 주는 모든 항목이 반영되었다. 남은 항목 #9
(pgvector RAG 예제)는 순수한 예제 작업 — 엔진 간극 없음 — 으로,
의 Candidate 5로 기록된 향후 쿡북(cookbook) 트랙에서
다룬다. v0.3.x 시리즈는 마무리되었으며, 이후 엔진 작업은 v0.4 / v1.0을 `ROADMAP_v1.md`
목표로 한다.

## ✅ v0.3.2에서 마무리

9. **pgvector RAG 예제 → ROADMAP 쿡북 트랙** — 엔진 간극 없음을
 `PostgresCheckpointStore`  확인(기존  인프라로 충분; RAG 노드는
   순수 사용자 코드). 의 Candidate 5로 기록되었으며,
 `ROADMAP_v1.md`  #8과 같은 Research/Cookbook 섹션에 속한다. 엔진 버전 범프(v-bump)
   시리즈가 아닌 향후 쿡북 작업에 속한다.

6. ** 사전 모양을 평탄하게 보는 `engine.get_state_view(thread_id)` 도우미** —
   가 Pydantic v2 를
   반환하며, 채널을 최상위 속성으로 접근할 수 있다
   ( 대신 ).
   기본 클래스는 로 임의의 채널 이름을 허용하여 —
   사용자가 모델을 선언하지 않아도 어떤 그래프에서든 동작한다.
   선언된 필드로 를 상속하면 타입이 있는 접근이 가능하며,
   불일치 시 pydantic 가 발생한다(조용한 타입 강제
   변환 대신). 는 버전 / 메타데이터가 필요한 호출자를 위해
   평탄화되지 않은 사전을 보존한다.

   Pydantic v2가 필수 의존성으로 추가되었다(현대 LLM Python 생태계의
   기본 — FastAPI, LangChain, 데이터모델 라이브러리 모두 사용).

   테스트:  (12).

8. **자기 진화 그래프 v2 → 의 연구 트랙** — `ROADMAP_v1.md`
   토폴로지 인식 수정자 프롬프트(topology-aware modifier prompt) 방향이
   연구 후보 #4로 기록되었다. 엔진 측 변경은 LLM 평가가 어떤 내부
   구조 조회(introspection)가 실제로 효과를 내는지 보여준 뒤에 작을
   가능성이 높다. 배포된 엔진의 사용자 장애 요소가 아니므로 v0.3.x에서
   뒤로 미룸.

5. **가 사전과  모두 허용** —
   v0.3.1 README 설명("channel_writes는 ChannelWrite 목록")이 실제로는
   틀렸다: 엔진이 JSON 객체만 받아서 목록을 전달하면 **조용히 아무 일도
 `is_object()`  일어나지 않았다**(C++  검사가 거부). Pybind 바인딩이
   이제 입력 모양에 따라 분기한다:
 `dict` `{channel: value}` -   → 기존 경로(LangGraph의
 `values={...}`     모양, 키워드 인자 이름은 다름).
 `list[ChannelWrite]`  -  → 사전으로 줄임(채널별 마지막 쓰기 우선);
 `.channel` `.value`   / 객체도 덕 타이핑(duck-typed)으로 허용.
 `TypeError`  - 그 외 타입은  발생 — 조용한 무동작(silent-no-op) 함정이
     다시 생기지 않도록.

 `Differences from LangGraph`  README  섹션 수정.
 `bindings/python/tests/test_update_state_shapes.py`  테스트:  (11).

10. ** 전용 노드가 을 통해 디스패치됨** — `execute_stream` `run_stream`
    Python 바인딩과 C++ 엔진 수준 모두에서 수정.

 `PyGraphNode::execute_full_stream`   **Python**: 이 이제
 `execute_stream` `execute_full`  로 빠지기 전에 을 확인하므로,
 `execute_stream(state, cb)`   만 재정의한 Python 노드가
 `engine.run_stream()`    / 에서 올바르게 동작한다.
 `run_stream_async()`   v0.3.1에서 의 힌트 메시지가 더 이상 잘못된
 `GraphNode.execute()`   방향을 안내하지 않는다.

    **C++** (자매 수정): 만 재정의한 C++ 하위 클래스도
 `execute_stream`   같은 문제를 겪었다 — 기본 이 먼저
 `GraphNode::execute_full_stream` `execute_full`  을 호출했고, 이는  /  기본을
 `execute` `execute_async`  거쳐 의 재귀 검사를 건드렸다. 이
 `ExecuteDefaultGuard`   가 이
 `runtime_error`   실행되기 전에 탈출했다.
 `result.writes = execute_stream(state, cb)`   (의 하위 클래스, 이전 호환 유지)를 도입하여 해결 —
 `GraphNodeMissingOverride`   기본 재귀 보호 장치가 이 전용 타입을 던지고, 두
 `runtime_error`    기본이 이 타입만 잡아서
    로 빠진다. 실제 사용자가 던진 오류는
 `execute_full_stream{,_async}`   그대로 전파된다.

    우선순위(보존, 양 언어): execute_full_stream
    → execute_stream → execute_full → execute.

    테스트:
 `bindings/python/tests/test_execute_stream_dispatch.py`    (5),
 `tests/test_execute_stream_only_dispatch.cpp`    (2).
