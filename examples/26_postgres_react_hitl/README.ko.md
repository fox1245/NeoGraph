<!-- neograph-i18n: source=examples/26_postgres_react_hitl/README.md locale=ko source_sha256=c03d573ec24eaf1dd00c474339be503d57dd52b0c8804806bd3035ff4901192f -->
# 예제 26 — HITL을 사용한 Postgres 기반 Deep Research

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

두 가지 NeoGraph 기능을 처음부터 끝까지 시연합니다:

1. **`PostgresCheckpointStore`** — 실제 PostgreSQL의 durable 체크포인트와 채널-블롭 중복 제거.
2. **NodeInterrupt 기반 HITL** — Deep Research 그래프가 보고서를 생성한 후 일시 중지하고, 인간이 검토하며, 승인(→ 종료) 또는 피드백(→ 다른 연구 라운드)으로 재개합니다.

데모는 의도적으로 **프로세스 비연속적**입니다: 바이너리는 보고서를 생성한 후 종료하므로, `resume`할 때 모든 것을 PG에서 다시 로드해야 하는 새로운 프로세스입니다. 그것이 핵심입니다 — 체크포인트가 실제로 프로세스 경계를 넘었음을 증명합니다.

## 시나리오

아래의 워크스루는 원래 아이디어를 반영합니다: 에이전트에게 최신 Vision Transformer 논문을 요청하고, 보고서가 URL을 인용하지 않는다는 것을 알아차리고, 인용을 위해 다시 보냅니다.

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

에이전트 프로세스는 여기서 **종료**됩니다 — 체크포인트는 PG에 있습니다. 이제 후속 조치하십시오:

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

완료하려면 승인하십시오:

```
$ docker compose run --rm agent resume dr-hitl-a1b2c3d4 approve
--- Final report (approved) ---
... final markdown ...
```

## 실제로 유지되었는지 검증

PG로 들어가서 행을 살펴보십시오 — `neograph_checkpoint_blobs`이 중복 제거 덕분에 `channels × checkpoints`보다 행 수가 적은 점을 주목하십시오:

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

`final_report`는 생성된 보고서당 하나의 행을 가지며, 슈퍼-스텝 간에 변경되지 않은 채널(`user_query`, `research_brief`)은 총 정확히 하나의 행을 가집니다.

### 실제 실행의 참조 번호

위의 multimodal-RAG 데모에서 완전한 실행-재개-재개 사이클(감독자 2라운드 × 연구원 2명 각각, OpenRouter를 통해 DeepSeek 고정)은 다음 PG 번호를 생성했습니다:

| 메트릭                      | 값      | 메모 |
|-----------------------------|------------|-------|
| `neograph_checkpoints`      | 15개 행    | 6개의 슈퍼스텝 + NodeInterrupt 1회 + 6개의 슈퍼스텝 + NodeInterrupt 1회 + 승인(admission) 체크포인트 1회 |
| `neograph_checkpoint_blobs` | 29개 행    | 이론적 15 cps × 9채널 = 135 대비 — **78.5% 중복 제거** |
| `neograph_checkpoint_writes`| **0개 행** | 정결 무결 — 모든 슈퍼스텝의 대기 로그가 커밋 시점에 비워짐 |
| `final_report` v13 (1라운드)| 2806 B     | arXiv URL 없음(모델은 `arXiv:NNNN.NNNNN` 약식 사용) |
| `final_report` v27 (2라운드)| 2752 B     | 사용자가 요청한 후 전체 `https://arxiv.org/abs/...` URL |
| 총 blob 바이트            | 41 KB      | 모든 LLM 트랜스크립트를 포함한 전체 스레드 상태 |

`final_report` v13 → v27 diff는 사용자 피드백이 실제로 에이전트의 출력을 변경했다는 결정적인 증거입니다. 두 번째 보고서는 산문을 줄이고 URL 인용을 추가했습니다. 이는 HITL 게이트가 피드백을 `supervisor_messages`에 다시 주입했기 때문에 감독자가 도달할 수 있었던 품질 개선입니다.

## 설정

1. 자격 증명을 복사하여 입력하세요:
   ```
   cp .env.example .env
   # set OPENROUTER_API_KEY; set CRAWL4AI_API_TOKEN to a fresh `openssl rand -hex 32` value
   ```
2. 지원 서비스를 불러옵니다:
   ```
   docker compose up -d postgres crawl4ai
   ```
3. 데모를 실행합니다(위의 "시나리오" 참조). 첫 번째 `docker compose run`가 `agent` 이미지 빌드를 트리거합니다(워밍업된 머신에서 약 1분 소요).

완료되면:
```
docker compose down       # stop services, keep PG volume
docker compose down -v    # drop the PG volume too
```

## 바이너리를 직접 실행(에이전트에 docker-compose 사용 안 함)

호스트에서 바이너리를 빌드하여 docker-compose로 관리되는 Postgres + Crawl4AI를 가리킬 수도 있습니다:

```
cmake -B build -DNEOGRAPH_BUILD_POSTGRES=ON -DNEOGRAPH_BUILD_TESTS=OFF
cmake --build build --target example_postgres_react_hitl -j

./build/example_postgres_react_hitl run "...your query..."
./build/example_postgres_react_hitl resume <thread_id> "feedback"
./build/example_postgres_react_hitl status <thread_id>
```

호스트 측 .env의 `POSTGRES_URL=postgresql://postgres:test@localhost:55432/neograph`는 compose로 게시된 포트를 가리킵니다. `CRAWL4AI_URL`도 Crawl4AI에 대해 동일하게 수행합니다. `CRAWL4AI_API_TOKEN`가 bearer 자격 증명으로 전송됩니다; compose 파일은 빈 값을 거부하고 Crawl4AI를 `127.0.0.1`에만 게시합니다.

## 이 스택에 대해 통합 테스트 실행

compose 파일은 동일한 Postgres 인스턴스에 **별도의 `neograph_test` 데이터베이스**를 프로비저닝하며, 이는 특히 PostgresCheckpointStore 통합 테스트를 위한 것입니다(이 테스트는 SetUp에서 `drop_schema()`를 호출하며, 그렇지 않으면 데모 스레드를 삭제합니다). 다음 명령으로 실행하십시오:

```bash
NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir ../../build -R PostgresCheckpoint --output-on-failure
```

테스트 URL을 `/neograph` 대신 `/neograph_test`로 지정하면 데모 DB에 있는 모든 스레드 데이터가 날아갑니다 — 그러지 마세요.

## 포렌식 팁 — 사용자 피드백이 PG의 어디에 저장되는지

`messages` 채널(엔진이 재개 값을 `HumanReviewNode`에 전달하는 데 사용하는 채널)을 직접 쿼리하면 빈 배열만 표시됩니다.

```
SELECT version, blob_data FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'messages';
-- 0 | null
-- N | []
-- M | []
```

이는 의도된 것이며 데이터 손실이 아닙니다: `HumanReviewNode`는 들어오는 사용자 메시지를 소비하고 즉시 `messages: []`를 자체 `Command.updates`에 다시 기록하므로 향후 인터럽트 주기가 깨끗한 채널에서 시작됩니다. 체크포인트가 수집될 시점에 배열은 이미 비어 있습니다.

실제 피드백 텍스트는 `supervisor_messages` 채널에 있으며 마커 `[USER FOLLOW-UP after reviewing...]`가 접두사로 붙습니다.

```
SELECT blob_data::text FROM neograph_checkpoint_blobs
 WHERE thread_id = '...' AND channel = 'supervisor_messages'
 ORDER BY version DESC LIMIT 1;
```

`grep` 해당 마커를 위해 스레드의 모든 HITL 라운드에서 사용자가 말한 모든 것을 복구합니다.

## 구현 노트

- HITL 게이트는 `DeepResearchConfig::enable_human_review` 플래그 뒤의 Deep Research 그래프에 내장되어 있습니다(기본적으로 꺼져 있어 예제 25는 영향을 받지 않습니다). 이를 켜면 `HumanReviewNode`가 `final_report`와 `__end__` 사이에 위치합니다.
- 해당 노드는 첫 실행 시 `NodeInterrupt`를 던집니다. 엔진이 이를 포착하고 `NodeInterrupt` 단계에서 체크포인트를 저장한 뒤 호출자에게 다시 throw 합니다. 재개 시 엔진은 사용자의 응답이 `messages` 채널에 기록된 상태로 동일한 노드에 다시 진입합니다.
- 노드는 "승인"(→ Command(__end__))과 피드백(→ Command(supervisor)에 피드백을 `supervisor_messages`에 추가하고 반복 카운터를 재설정)을 구별합니다. 두 경로 모두 실행을 깨끗하게 종료하므로 PG는 항상 일관된 최신 체크포인트를 보유합니다.
- 시나리오의 세 단계(초기 실행, 피드백 포함 재개, 승인 포함 재개)는 모두 프로세스 경계를 넘나듭니다 — 엔진 상태는 호출 사이에 전적으로 PG에 상주합니다.

## 왜 프론트엔드가 없나요?

"binary exits → restart → continue" 흐름이 바로 프론트엔드입니다. 웹 UI는 동일한 인수를 HTTP를 통해 마샬링할 뿐이며 체크포인트 내구성 데모에 아무것도 추가하지 않습니다. 시각적 증거를 위해 위의 PG 테이블을 직접 검사하세요.
