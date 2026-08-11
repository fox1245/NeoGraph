<!-- neograph-i18n: source=examples/26_postgres_react_hitl/README.md locale=ko source_sha256=b8c1274535db9f44a2a3c254c0b4de2c4dba30f23d37f69e80c0f31a365e6511 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# 예 26 — HITL를 사용한 Postgres 지원 심층 연구


두 가지 NeoGraph 기능을 엔드 투 엔드로 보여줍니다.

1. **`PostgresCheckpointStore`** — 실제 PostgreSQL의 내구성 있는 체크포인트
채널 Blob 중복 제거를 사용합니다.
2. **NodeInterrupt 기반 HITL** — Deep Research 그래프는 다음 이후에 일시 중지됩니다.
보고서를 생성하고 사람이 검토한 다음 다음 중 하나를 통해 재개합니다.
승인(→ 종료) 또는 피드백(→ 다른 연구 라운드).

데모는 의도적으로 **프로세스가 연속되지 않습니다**: 바이너리가 종료됩니다.
보고서를 생성한 후 `resume`를 사용하면 새로운 프로세스가 됩니다.
PG에서 모든 것을 다시 로드해야 합니다. 그게 요점이야 - 증명해
체크포인트가 실제로 프로세스 경계를 ​​넘었습니다.

## 시나리오

아래 연습은 원래 아이디어를 반영합니다. 상담원에게
최신 Vision Transformer 논문에서 보고서에 URL이 인용되지 않았음을 확인하세요.
그리고 인용을 위해 다시 보내세요.

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

에이전트 프로세스는 여기서 **종료**됩니다. 체크포인트는 PG에 있습니다. 이제 후속 조치를 취하십시오.

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

완료 승인:

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 approve
--- Final report (approved) ---
... final markdown ...
```

## 실제로 지속되는지 확인

PG에 들어가서 행을 살펴보세요. `neograph_checkpoint_blobs`가 어떻게 표시되는지 확인하세요.
중복 제거 덕분에 `channels × checkpoints`보다 행 수가 적습니다.

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

`final_report`에는 생성된 보고서당 하나의 행이 있습니다. 채널
슈퍼 단계(`user_query`, `research_brief`) 간에 변경되지 않았습니다.
총 행이 정확히 1개 있습니다.

### 실제 실행의 참조 번호

위의 다중 모달-RAG 데모에 대한 완전한 실행-재개-재개 주기
(감독자 2회 × 연구원 각 2명, OpenRouter를 통한 고정 DeepSeek)
다음 PG 번호를 생성했습니다.

|미터법|값|메모|
|-----------------------------|------------|-------|
|`neograph_checkpoints`|15줄|6개의 슈퍼 단계 + 1개의 NodeInterrupt + 6개의 슈퍼 단계 + 1개의 NodeInterrupt + 1개의 승인 CP|
|`neograph_checkpoint_blobs`|29줄|vs 이론적 15cps × 9개 채널 = 135 — **78.5% 중복 제거**|
|`neograph_checkpoint_writes`|**0행**|clean — 커밋 시 모든 상위 단계의 보류 로그가 지워졌습니다.|
|`final_report` v13(1라운드)|2806B|arXiv URL 없음(모델에서는 `arXiv:NNNN.NNNNN` 약칭을 사용함)|
|`final_report` v27(2라운드)|2752B|사용자가 요청한 후 전체 `https://arxiv.org/abs/...` URL|
|총 Blob 바이트|41KB|모든 LLM 성적표를 포함한 전체 스레드 상태|

`final_report` v13 → v27 diff는 사용자가
피드백이 실제로 에이전트의 출력을 변경했습니다. 두 번째 보고서
다듬어진 산문 AND에 URL 인용 추가 — 품질 개선
HITL 게이트가 피드백을 피드백했기 때문에 감독관에게만 도달했습니다.
`supervisor_messages`에.

## 설정

1. 자격 증명을 복사하고 입력하세요.
   ```
   cp .env.example .env
   # OPENROUTER_API_KEY를 설정하고 CRAWL4AI_API_TOKEN에는 새 `openssl rand -hex 32` 값을 넣으세요.
   ```
2. 지원 서비스를 시작하세요.
   ```
   docker compose up -d postgres crawl4ai
   ```
3. 데모를 실행합니다(위의 "시나리오" 참조). 첫 번째 `docker compose run`
`agent` 이미지 빌드를 트리거합니다(웜 머신에서는 ~1분).

완료되면:
```
docker compose down       # stop services, keep PG volume
docker compose down -v    # drop the PG volume too
```

## 바이너리를 직접 실행(에이전트에 대한 docker-compose 없음)

호스트에서 바이너리를 빌드하고 이를 가리킬 수도 있습니다.
docker-compose-managed Postgres + Crawl4AI:

```
cmake -B build -DNEOGRAPH_BUILD_POSTGRES=ON -DNEOGRAPH_BUILD_TESTS=OFF
cmake --build build --target example_postgres_react_hitl -j

./build/example_postgres_react_hitl run "...your query..."
./build/example_postgres_react_hitl resume <thread_id> "feedback"
./build/example_postgres_react_hitl status <thread_id>
```

호스트 측 .env의 `POSTGRES_URL=postgresql://postgres:test@localhost:55432/neograph`는
compose가 공개한 포트를 가리키며, `CRAWL4AI_URL`도 Crawl4AI에 동일하게 적용됩니다.
`CRAWL4AI_API_TOKEN`은 bearer 자격 증명으로 전송됩니다. compose 파일은 빈 값을
거부하고 Crawl4AI를 `127.0.0.1`에만 공개합니다.

## 이 스택에 대해 통합 테스트 실행

작성 파일은 **별도의 `neograph_test` 데이터베이스**를 프로비저닝합니다.
특히 동일한 Postgres 인스턴스
PostgresCheckpointStore 통합 테스트(`drop_schema()` 호출)
그렇지 않으면 데모 스레드가 지워집니다). 다음을 사용하여 실행하세요.

```bash
NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir ../../build -R PostgresCheckpoint --output-on-failure
```

`/neograph_test` 대신 `/neograph`에서 테스트 URL를 가리키면
데모 DB에 있는 스레드 데이터를 핵폭탄으로 처리하지 마십시오. 그렇게 하지 마십시오.

## 포렌식 팁 — 사용자 피드백이 PG에 저장되는 곳

`messages` 채널을 직접 쿼리하는 경우(채널은 엔진
이력서 값을 `HumanReviewNode`에 전달하는 데 사용됨)
빈 배열:

```
SELECT version, blob_data FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'messages';
-- 0 | null
-- N | []
-- M | []
```

이는 데이터 손실이 아니라 의도적인 것입니다. `HumanReviewNode`는
들어오는 사용자 메시지를 즉시 `messages: []`에 다시 씁니다.
`Command.updates`이므로 향후 인터럽트 주기는 깨끗한 상태에서 시작됩니다.
채널. 체크포인트가 생성될 때까지 어레이는 이미
비어 있는.

실제 피드백 텍스트는 `supervisor_messages` 채널에 있습니다.
`[USER FOLLOW-UP after reviewing...]` 마커가 앞에 붙습니다:

```
SELECT blob_data::text FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'supervisor_messages'
 ORDER BY version DESC LIMIT 1;
```

해당 마커에 대한 `grep`는 사용자가 전체에서 말한 모든 내용을 복구합니다.
HITL는 스레드를 반올림합니다.

## 구현 노트

- HITL 게이트는 Deep Research 그래프 뒤에 내장되어 있습니다.
`DeepResearchConfig::enable_human_review` 플래그(기본값은 꺼져 있으므로
예제 25는 영향을 받지 않습니다.) 이를 켜면 `HumanReviewNode`가 앉습니다.
`final_report`와 `__end__` 사이.
- 해당 노드는 첫 번째 실행 시 `NodeInterrupt`를 발생시킵니다. 엔진
이를 포착하고 `NodeInterrupt` 단계에 체크포인트를 저장합니다.
호출자에게 다시 던집니다. 다시 시작하면 엔진이 동일한 상태로 다시 들어갑니다.
사용자의 응답이 `messages` 채널에 기록된 노드입니다.
- 노드는 "승인"(→ Command(__end__))과 피드백을 구별합니다.
(→ 피드백이 첨부된 Command(supervisor)
`supervisor_messages` 및 반복 카운터 재설정). 두 경로 모두
실행을 깔끔하게 종료하여 PG가 항상 일관된 최신 CP를 갖게 됩니다.
- 시나리오의 세 단계 모두(초기 실행, 피드백을 통한 재개,
승인으로 재개) 프로세스 경계 간 — 엔진 상태
호출 사이에 전적으로 PG에 존재합니다.

## 왜 프론트엔드가 없나요?

"바이너리 종료 → 다시 시작 → 계속" 흐름은 프런트엔드입니다. 웹 UI
HTTP를 통해서만 동일한 인수를 마샬링하고 추가하지 않습니다.
체크포인트 내구성을 입증하는 데 필요한 모든 것. PG 검사
시각적 증거를 위해 직접 (위) 테이블.
