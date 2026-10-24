<!-- neograph-i18n: source=examples/cookbook/program-coding-swarm/README.md locale=ko source_sha256=cd6756c6ba1b7b59c59c32a59a1b1f97d2bd05b49a37e998fc635870b85d52a6 -->
<!-- neograph-i18n: source=examples/cookbook/program-coding-swarm/README.md -->
# Program 코딩 스웜 (`program-coding-swarm`)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

단일 선형 작성-테스트-리뷰 루프 대신 명시적이고 예산이 제한된 Program 계층 구조로 멀티 에이전트 코딩 워크플로를 실행하는 방법을 보여주는 실험적 개념 증명(experimental proof of concept)입니다. 자체 합성되거나 교체된 토폴로지 자체가 안전하거나 신뢰할 수 있다고 주장하지 않으며, 이 예제의 안전성은 호스트가 소유한 컴파일, 승인(admission), 예산 상한 및 결정론적 검증 게이트에서 비롯됩니다.

이 쿡북의 코딩 대상(`slugify`, `escape_html`, `ini_kv`)은 호스트 소유 검증 노드가 테스트 벡터와 대조하여 확인하는 소규모 결정론적 텍스트 변환 파이프라인(장난감 대상)입니다.

## 시연 항목

1. **병렬 작업 (`root -> children`)**: 루트 Program은 모델 기반 `orchestrator` 노드를 실행하여 모듈 분할을 계획하고, 모듈당 하나의 자식 Harness를 병렬(`ng.spawn` / `ng.await`)로 생성하여 실행한 뒤 결과를 결합하고 최종 통합 검토를 수행합니다. `--concurrency <n>` 또는 `--sequential`(`--concurrency 1`)을 전달하면 동일한 토폴로지와 입력을 제한된 병렬도 또는 순차 실행으로 구동하고 보고서에서 벽시계(wall-clock) 중첩을 비교할 수 있습니다.
2. **모듈별 리소스 할당**: 각 모듈에는 중요도(`standard` 또는 `critical`)가 지정됩니다. 중요 모듈(`escape_html`)은 루트 예산에서 분할된 더 큰 비갱신형(non-renewable) 토큰 및 금액 예산, 더 많은 재시도 횟수, 더 강력한 구현 역할(`implementer-strong`), 그리고 손자(grandchild) 세컨드 오피니언 Harness를 할당받습니다. 실행 종료 시 각 노드의 잔여 예산이 출력되고 보고서에 기록됩니다.
3. **역할 기반 모델 매핑**: 노드는 하드코딩된 모델 ID나 가격 대신 모델 역할(`orchestrator`, `implementer-cheap`, `implementer-strong`, `reviewer`, `second-opinion`)을 참조합니다. 역할 바인딩, 프로바이더 엔드포인트 및 `SchemaProvider` 모델 카탈로그 항목은 [`demo-models.json`](demo-models.json)(오프라인 기본값) 및 [`live-models.json`](live-models.json)(라이브 측정용)에 정의됩니다.
4. **호스트 소유 사전조건 게이트**: 모듈 자식은 호스트 소유 `verify` 노드가 모든 결정론적 테스트 케이스를 통과하고 **동시에** 리뷰어의 판정이 읽혀 `"accept"`인 경우(중요 모듈은 손자 세컨드 오피니언 판정 `"accept"` 포함)에만 결과를 게시(publish)할 수 있습니다. 오프라인 기본 실행에서 `slugify`의 1차 시도는 `verify`에 실패하고 `escape_html`의 1차 시도는 `review`에서 거부되며, 게이트가 두 시도의 게시를 차단하고 예산 내 재시도를 유도합니다.
5. **다단계 감독 (`root -> child -> grandchild`)**: 중요 모듈(`escape_html`)의 경우 1차 리뷰가 통과된 후 자식 Harness가 손자 Harness(`second-opinion:escape_html`)를 실행합니다. 각 계층(`root -> child -> grandchild`)의 역할, 모델 역할, 부여된 예산과 잔여 예산, 최종 결과를 출력합니다.
6. **데이터로서의 토폴로지 및 SQLite 영속화 (실행 중 교체 및 롤백)**: 컴파일된 모든 `ProgramBundle`, 승인된 `ProgramVersion`, 실행 계보(lineage) 및 세대(generation)는 SQLite(`SQLiteProgramStore`, `SQLiteProgramTransitionStore`, `SqliteCheckpointStore`)에 저장됩니다. 실행 도중 `slugify`의 1차 시도가 차단된 후 호스트는 유지된 `ng.checkpoint`에서 `slugify`의 자식 토폴로지를 더 엄격한 승인 버전(세대 2)으로 교체하며, 이때 비갱신형 잔여 예산이 리셋되지 않고 이월되며 자식 관계가 유지됩니다. 2차 시도가 통과되면 호스트는 이전 승인 버전(세대 3)으로 롤백하고 SQLite에서 읽어온 3개 세대 이력을 출력합니다.
7. **중앙(central) 대 위임(delegated) 합성 (`--synthesis=central|delegated`, 기본값 `central`)**:
   - `central`: 실행 시작 전에 호스트가 전체 트리(`root`, 모든 `child` 모듈, 중요 모듈의 `grandchild`)를 컴파일, 링크(`link_module_child`) 및 승인합니다. 자식은 사전 승인된 손자 템플릿의 파라미터만 채울 수 있습니다.
   - `delegated`: 호스트는 루트와 자식 모듈만 사전 승인합니다. 실행 중에 중요 자식(`escape_html`)이 체크포인트에서 일시 정지하고 검토된 템플릿 라이브러리로부터 손자 JavaScript 토폴로지를 제안합니다. 호스트는 예산을 초과하는 제안이 `authorize_program_child_synthesis`에서 거부(`admissions_refused`)됨을 검증한 뒤, `ProgramSynthesisGateway`와 `ProgramRuntime::prepare_child_synthesis`를 통해 자식의 권한 및 잔여 예산 범위 내에서 손자 토폴로지를 합성 및 승인합니다.
   - 동일한 작업, 모델, 예산에서 두 모드를 모두 실행하고 출력 및 JSON 보고서에 비교 결과를 기록합니다.

## 오프라인 모드 (기본값, CTest에서 실행)

API 키나 네트워크 호출 없이 빌드 및 실행합니다:

```bash
cmake --build build --target cookbook_program_coding_swarm
./build/cookbook_program_coding_swarm
ctest --test-dir build -R cookbook.program_coding_swarm --output-on-failure
```

## 라이브 모드 및 측정 보고서

라이브 모드는 [`live-models.json`](live-models.json)에 설정된 실제 프로바이더를 사용합니다:

- `orchestrator` (루트): Anthropic Messages API (`claude-sonnet-5-5`), `ANTHROPIC_API_KEY` 사용.
- `implementer`, `implementer-cheap`, `implementer-strong`, `reviewer` (자식): OpenRouter Chat Completions (`deepseek/deepseek-v4.1-flash-1017`), `OPENROUTER_API_KEY` 사용.
- `second-opinion` (손자): OpenRouter Chat Completions (`inception/mercury-3`), `OPENROUTER_API_KEY` 사용.

[`.env.example`](.env.example)을 `.env`로 복사하거나 환경 변수를 설정한 뒤 실행합니다:

```bash
export ANTHROPIC_API_KEY="..."
export OPENROUTER_API_KEY="..."

./build/cookbook_program_coding_swarm \
  --live \
  --models examples/cookbook/program-coding-swarm/live-models.json \
  --synthesis=central \
  --runs 3 \
  --max-cost-usd 0.50 \
  --max-cost-usd-other 0.25 \
  --report swarm-report.json
```

### CLI 플래그

- `--mock` (기본값) / `--live`: 결정론적 오프라인 스크립트 프로바이더 또는 라이브 프로바이더 선택.
- `--models <file.json>`: 역할 및 모델 카탈로그 설정 파일 경로 (`demo-models.json` 또는 `live-models.json`).
- `--synthesis=central|delegated`: 기본 합성 모드 선택 (`central` 기본값). 비교 대상 모드도 함께 실행되어 `synthesis_comparison`에 기록됩니다.
- `--concurrency <n>` / `--sequential`: 병렬로 실행할 모듈 자식 수 제한 (`--sequential`은 동시성 `1`).
- `--runs <k>`: 전체 작업을 `k`회 반복하고 병렬/순차 중앙값 벽시계 시간, 실행당 비용 중앙값, 게이트 차단 합계를 집계합니다.
- `--max-cost-usd <usd>`: `RunBudget::monetary_microunits` 및 `bind_priced_provider_call`로 강제되는 전체 실행 금액 예산 상한.
- `--max-cost-usd-other <usd>`: 비-Anthropic 프로바이더(OpenRouter 자식 및 손자 호출)에 대한 별도 금액 예산 상한.
- `--report <file.json>`: 기계 판독 가능한 측정 보고서를 JSON 파일로 저장.

### OpenRouter 라우팅 및 Zero Data Retention (`zdr`)

[`live-models.json`](live-models.json)에서 OpenRouter 역할은 다음과 같이 설정됩니다:

```json
"provider": {
  "sort": "price",
  "allow_fallbacks": true,
  "data_collection": "allow"
}
```

이를 통해 `zdr` 필터(저가 엔드포인트를 대거 제외함) 없이 폴백을 허용하면서 저렴한 엔드포인트를 선택합니다. 모든 호출은 디스패치 전에 `live-models.json`의 `ceiling_input` 및 `ceiling_output` 요율을 사용하여 `bind_priced_provider_call`에 의해 `provider.max_price`가 고정됩니다. 현재 C++ SDK의 `sp::OpenRouterRouting`은 `zdr`, `allow_fallbacks`, `require_parameters`, `data_collection`, `only`, `order`, `ignore`, `max_price`를 전달하며, `"sort"` 설정은 보고서의 `openrouter_routing` 메타데이터에 기록됩니다.

Zero Data Retention이 필요하거나 특정 업스트림 프로바이더로 고정하려면 `live-models.json`의 `"provider"` 블록을 다음과 같이 수정합니다:

```json
"provider": {
  "zdr": true,
  "allow_fallbacks": false,
  "data_collection": "deny",
  "only": ["deepinfra", "together"]
}
```

### `--report <file.json>` 읽는 법

- `primary_run.total_wall_time_ms` 및 `primary_run.sum_children_wall_time_ms`: 전체 벽시계 시간과 자식 실행 시간의 합을 비교합니다. `children_intervals_overlapped`는 자식들의 `[wall_clock_start_ms, wall_clock_end_ms]` 구간이 실제로 겹쳤음을 확인합니다.
- `primary_run.cost_by_module` 및 `primary_run.cost_by_model_role`: 모듈별 및 모델 역할별 비용 분포를 보여줍니다.
- `primary_run.nodes`: 노드 계층(`root`, `child`, `grandchild`), 해석된 프로바이더/모델, 시도 횟수, 입출력 토큰, 비용(`cost_source`: `"provider_reported"` 또는 `"catalogue"`), 시작/종료 시각, 결과, 부여 예산 대비 잔여 예산을 기록합니다.
- `primary_run.calls`: OpenRouter가 실제로 라우팅한 업스트림 프로바이더(`provider_routed_to`)를 포함한 호출별 텔레메트리입니다.
- `primary_run.gate_decisions`: 호스트 게이트가 평가한 모든 시도의 `verify_passed`, `reviewer_verdict_read`, `reviewer_verdict`, `second_opinion_verdict`, `published`, `block_reason`을 기록합니다.
- `primary_run.generation_transitions`: 각 세대 전환(`initial` -> `replace_stricter` -> `rollback_initial`) 전후의 예산과 SQLite 세대 이력을 기록합니다.
- `synthesis_comparison`: 동일한 작업, 모델, 예산에서 실행한 `central` 대 `delegated` 합성 비교 결과입니다.
- `aggregates`: 다회 실행(`--runs <k>`) 시의 중앙값 통계입니다.

## 라이브 실행 측정 결과 (2026-10-24)

- **설정**: `--live --synthesis=central --runs 3`, 상한 `--max-cost-usd 2.00 --max-cost-usd-other 0.50`; 모델은 [`live-models.json`](live-models.json) 기준(루트 `orchestrator`는 Anthropic Messages API, 자식 및 손자는 `{"sort":"price","allow_fallbacks":true,"data_collection":"allow"}`가 적용된 OpenRouter 경유). Linux x86-64.
- **3회 실행 전체**: 모든 모듈이 1차 시도에서 게시됨(실행당 3/3개 모듈, `central` 모드에서 게이트 차단 0건, 승인 거부 0건); 실행 검증은 `PASS`를 출력함.
- **벽시계 시간 (3회 중앙값)**: 모듈 자식을 병렬로 실행했을 때 23.6초, 동일한 작업을 순차 실행했을 때 32.6초(약 1.38배). 실행별 병렬 벽시계 시간은 23.6초, 21.6초, 28.8초였으며, 모든 실행에서 자식 실행 구간이 중첩됨.
- **실행당 비용 (실행의 금액 기록 기준)**: 0.0291~0.0299 USD, 중앙값 0.0298 USD; 이 중 OpenRouter 자식 및 손자 호출이 0.0014~0.0020 USD, 루트 `orchestrator`가 약 0.028 USD. 실행당 토큰: 입력 약 2.7k, 출력 3.9k~4.7k.
- **실행 중 토폴로지 교체 및 롤백**: 오프라인 실행과 동일하게 동작함: SQLite에서 3개 세대를 다시 읽어왔고, 비갱신형 예산이 이월되었으며(토큰 20000 -> 20000), 자식이 유지됨.
- **위임(`delegated`) 모드 (비교를 위해 동일 실행에서 함께 수행)**: 모듈 3개 게시, 게이트 차단 1건 및 승인 거부 1건(예산을 초과한 손자 제안으로, 설계대로 거부됨), 벽시계 시간 32.6초.
- **이 실행이 보여주지 않는 것**: 이번 라이브 모델에서는 `central` 모드에서 게이트가 차단할 상황이 발생하지 않았으므로, 게이트 차단 및 재시도 경로는 이번 라이브 실행이 아니라 오프라인 실행에서 시연됨; 3회 실행은 신뢰성 측정이 아님.
