<!-- neograph-i18n: source=docs/model-program-synthesis-evidence.md locale=ko source_sha256=b0a57aa0f6c4a726e417d88daf8d0e3f90178de20d610884fcba052b0f622105 -->
# 모델 생성 QuickJS Program 합성 증거

**Languages:** [English](model-program-synthesis-evidence.md) | [한국어](model-program-synthesis-evidence.ko.md) | [日本語](model-program-synthesis-evidence.ja.md) | [简体中文](model-program-synthesis-evidence.zh-CN.md)

- 상태: 제한된 PoC 검증 완료
- 관측일: 2026-08-21
- 모델: `deepseek/deepseek-v4-flash-0731` (OpenRouter를 통해)
- 공급자 응답: `gen-1787288110-o3PCpNZgsnE8eyQF1TzM`

## 검증된 체인

하나의 모델 응답이 다음 QuickJS Program 소스를 생성했습니다사합:

```javascript
export function define() {
    const graph = ng.graph("model-synthesized");
    graph.channel("value", { reducer: "probe.overwrite", initial: 0 });
    graph.channel("path", { reducer: "probe.overwrite", initial: "" });
    const pairs = [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]];
    for (const [name, type] of pairs) {
        graph.node(name, {type});
    }
    graph.entry("seed");
    graph.edge("seed", "double");
    graph.edge("double", "finish");
    graph.exit("finish");
    return graph;
}

export function* main(input) {
    return yield ng.callCore("model-synthesized", input, "model-generated:1");
}
```

호스트는 이 제안을 직접 실행하지 않았습니다. 검사기가 강제한 사항:

```text
model source
  -> immutable ProgramSynthesisProposal
  -> nonrenewable dynamic-compile reservation
  -> bounded QuickJS ProgramCompiler
  -> independent ProgramCatalog admission
  -> immutable ProgramVersion publication
  -> ProgramRuntime execution
```

체크인된 fixture가 동일한 호스트 파이프라인을 결정적으로 검증합니다. 그 영속적인 식별자는 다음과 같습니다:

| 증거 | ID |
|---|---|
| Program 소스 | `sha256:4e994637bfa31884f3a0090ffee7b0135f591656ee6217448d435d4a2b6384a3` |
| 제안서(Proposal) | `sha256:c51cd4737dc19939ee25a08799e9308a4dbf8943bffbcbd225e0cc9d7e361347` |
| 예약(Reservation) | `sha256:531ed6ed5fd713f8d5eda5d3d26df1bedf36d59d44d35b2af8cbfa53ff1ec628` |
| 번들(Bundle) | `sha256:3f21798666ddc5ad76c73ba9706e93db032064b76e68a4965f0bc49b2c89a375` |
| ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 합성 영수증(Synthesis receipt) | `sha256:350775f4c0bc9bb40937cec5c91fd5887f5678c7af349aa177406cec9c5e2f99` |

카탈로그 조회에서 정확히 승인된 버전을 찾았습니다. Program 실행이 트레이스 `seed -> double -> finish`와 함께 완료되었으며, 모든 노드는 정확히 한 번 실행되었습니다. 최종 채널은 `value = 12`과 `path = "model-generated"`였습니다.

## 런타임 토폴로지 교체

합성 게이트웨이는 이제 호스트 소유의 `ProgramBudgetBounds` 하에서 후속 버전을 컴파일합니다. 모든 리소스의 최대치는 예약의 `remaining_after_reservation`입니다. 따라서 이는 모델 출력이 아닌 이미 차감된 계보에서 파생된 권한 상한입니다. 하한은 JavaScript 런타임 구조적 하한일 뿐입니다: 하나의 벽시계 시간 단위, 하나의 워커, 하나의 Program 연산, 하나의 Core 단계. 소비 가능한 권한과 하위 권한은 제로 하한을 유지합니다. 역전된 호스트 경계는 소스 평가 전에 거부됩니다.

이를 통해 정확한 교체가 더 작고 벽시계 시간이 차감된 계보 잔여분을 비재생 가능한 예산을 확대하지 않고 운반할 수 있습니다. 일반적인 정확한 예산 컴파일러 오버로드는 고정된 단독 호출에 대해 계속 사용 가능합니다.

다른 소스 Program이 내구성 있는 최상위 `ng.checkpoint`에 도달했습니다. 호스트는 해당 인계를 소비하고 모델 토폴로지를 후속 버전으로 하여 `ProgramRuntime::replace`을 호출했습니다. 대상은 위의 합성 게이트웨이의 정확한 ProgramVersion이었으며, 두 번째 번들은 컴파일되거나 승인되지 않았습니다. 픽스처 전환이 생성했습니다:

| 증거 | 값 |
|---|---|
| 소스 ProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| 소스 실행 | `run-e153d50d90cc8d222c5f363c99399569` |
| 대상 실행 | `sha256:590048ab76d42119e184f89eb88701cd8df32a84bc244286f415c0b53086a089` |
| 대상 ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 교체 영수증 | `sha256:79a334ee185bf2ac0115d7d79038c2adff7b4bb854f294c7a5585924d8376fc3` |
| 활성 세대 | `2` |
| 대상 상태 | `completed` |

오래된 소스 노드는 제로 번 실행되었습니다. 후속 트레이스는 `seed -> double -> finish`였고, 모든 후속 노드는 한 번씩 실행되었으며, 최종 출력은 다시 `value = 12`와 `path = "model-generated"`를 포함했습니다.

최신 라이브 DeepSeek 실행에서 `replacement_uses_synthesis_version =
true`, 대상 상태 `completed`, 활성 생성 `2`, 그리고 동일한 스테일 노드 0개 결과가 보고되었습니다.

## 부정적 증거 및 프롬프트 계약

이전 모델 출력은 실행 전에 거부되었습니다:

- `P_JS_DEFINE_MISSING`: 동기 내보내기 `define()` 없음;
- `P_JS_DEFINE_VALUE`: `define()`가 불투명한 `ng.graph()` 빌더 대신 단순한 그래프 형태의 데이터를 반환했고;
- `P_JS_GRAPH_ARGUMENT`: 채널/노드 빌더 인자가 검토된 DSL 스키마와 일치하지 않았으며;
- `P_JS_EVALUATION`: 따옴표로 묶이지 않은 리듀서 식별자가 제한된 QuickJS 컨텍스트에 존재하지 않는 환경 상태를 참조했습니다.

따라서 성공한 프롬프트는 정확히 신뢰되는 저작 표면(authoring surface)을 명시했습니다: `ng.graph`, `graph.channel`와(과) `initial`, `graph.node(name, {type})`, entry, edge, exit, 그리고 봉인된 `ng.callCore` 명령. 유효하지 않은 제안은 ProgramVersion을 생성하지 않았고 어떤 노드도 실행하지 않았습니다.

## 범위 경계

이는 외부 모델이 QuickJS 토폴로지 소스를 합성할 수 있음을 증명하며, 해당 소스는 이후 예약, 컴파일, 승인(admission), 게시, 실행, 그리고 내구성 있는 런타임 체크포인트에서 서로 다른 Program 세대로 선택됩니다. 또한 합성 게이트웨이 자체의 승인된 버전이 동적 컴파일 차변 이후 그 후속 세대가 될 수 있음을 증명합니다. 런타임은 제안을 권위로 재사용하지 않으며 교체 경로에서 소스를 재컴파일하거나 재생하지 않습니다.

이 Program 수준의 교체는 임의의 GraphEngine 상태/프론티어 마이그레이션과 혼동되어서는 안 됩니다. 소스 Core 토폴로지에서 모델 토폴로지로의 마이그레이션 계획은 `blocked`로 올바르게 분류되었는데, 그 이유는 구체화 및 런타임 계약이 다르기 때문입니다.

NeoGraph는 이제 의도적으로 좁은 P1 GraphEngine 경로를 갖습니다: `GraphSemanticMigrationAdapter`. 호스트는 정확히 승인된 소스와 대상 아티팩트로부터 이 불변 어댑터를 준비해야 합니다. 이 경로는 선언 전용(런타임 JavaScript 제어 없음), 단일 루트 `call_core` Program만 승인하며, 동일한 체크포인트 채널/리듀서, 노드 이름, 엣지, 라우팅, 배리어, 재시도/인309터럽트tni, fa 기능 바인딩, 권위, 그리고 입출력ore 계약을 요구합니다. 따라서 신원 신경N mapping frontier와 채널 스냅샷을 서로 다른 different sealed Core 정의 및 compiled plan identity를 가진 후속 세대0로 전달할 수 있습니다. 어댑터는 migration receipt에 저장되며 recovery access 중 재검증 detection.

QuickJS 제어, 노드/프런티어 이름 변경, 채널 또는 리듀서 변환, 변경된 배리어 멤버십, 보류 중인 효과, 자식, 그리고 임의의 토폴로지 편집은 여전히 실패 시 차단(fail-closed)됩니다. 해당 사례는 이후 매핑 클래스가 영향을 받는 모든 상태 차원을 증명할 때까지 명시적인 핸드오프/재시작을 계속 요구합니다. 자동 자식 바인딩/생성, 모든 합성 경계를 아우르는 크래시 복구, 그리고 Program 내 `ng.proposeProgram` 명령 표면도 별도의 자격 게이트로 남아 있습니다.

## 모델 생성 P1 GraphEngine 마이그레이션

P1 어댑터는 실시간 `deepseek/deepseek-v4-flash-0731` OpenRouter 응답 `gen-1787291529-fCOHp8pry7EwHHHu1MUH`으로 엔드투엔드로 실행되었습니다. 모델은 선언 전용 QuickJS `define()` 소스(SHA-256 `346329bf39790cc5557a9961a7faa5da0b35168f84257b12d6166565d594df08d`)를 생성했으며, 해당 토폴로지는 원본 그래프의 `work -> followup` 프런티어 형태를 보존하면서 `migration_epoch: 2`를 통해 별도의 대상 Core 정의를 도입했습니다.

```text
model QuickJS define()
  -> ProgramSynthesisProposal
  -> dynamic-compile reservation
  -> ProgramCompiler + ProgramCatalog admission
  -> GraphSemanticMigrationAdapter preparation
  -> durable GraphEngine generation-2 migration
  -> recovery-proof validation
```

변경된 번들/물질화에 필요한 대로, 일반적인 마이그레이션 계획은 `blocked`로 유지되었습니다. 호스트 생성 어댑터는 이후 좁은 신원 투영을 승인했습니다. 대상은 생성(generation) `2`에서 완료되었습니다; `work`는 소스 세대에서 한 번 실행되었고 `followup`는 후속 세대에서 한 번 실행되었습니다. 정확한 어댑터 신원은 마이그레이션 영수증에 지속되었습니다.

`program_model_synthesis_probe` 대상을 사용하여 재현하고:

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```

GraphEngine P1 경로의 경우, `program_model_semantic_migration_probe`를 사용하고 다음을 따르십시오:

```powershell
bun run scripts/run_model_semantic_migration_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_semantic_migration_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
