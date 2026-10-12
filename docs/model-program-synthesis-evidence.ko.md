<!-- neograph-i18n: source=docs/model-program-synthesis-evidence.md locale=ko source_sha256=c34041ccc8bf5143dd152f292a26c02eb4a487757b4657e0ecfe75b4740a3373 -->
# 모델이 생성한 QuickJS Program 합성 증거

**Languages:** [English](model-program-synthesis-evidence.md) | [한국어](model-program-synthesis-evidence.ko.md) | [日本語](model-program-synthesis-evidence.ja.md) | [简体中文](model-program-synthesis-evidence.zh-CN.md)

- 상태: 범위가 한정된 PoC의 과거 증거. 현재 바인딩을 새로 검증한 결과는 아님
- 관찰일: 2026-08-21
- 모델: OpenRouter를 거친 `deepseek/deepseek-v4-flash-0731`
- provider 응답: `gen-1787288110-o3PCpNZgsnE8eyQF1TzM`

## 검증된 흐름

모델이 응답 한 번으로 다음 QuickJS Program 소스를 만들었습니다.

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

호스트는 이 제안을 곧바로 실행하지 않았습니다. 프로브는 다음 순서를 강제했습니다.

```text
model source
  -> immutable ProgramSynthesisProposal
  -> nonrenewable dynamic-compile reservation
  -> bounded QuickJS ProgramCompiler
  -> independent ProgramCatalog admission
  -> immutable ProgramVersion publication
  -> ProgramRuntime execution
```

저장소에 포함된 픽스처는 같은 호스트 파이프라인을 결정적으로 검증합니다. 이 픽스처의 영속 식별자는 다음과 같습니다.

| 증거 | 식별자 |
|---|---|
| Program 소스 | `sha256:4e994637bfa31884f3a0090ffee7b0135f591656ee6217448d435d4a2b6384a3` |
| 제안(Proposal) | `sha256:c51cd4737dc19939ee25a08799e9308a4dbf8943bffbcbd225e0cc9d7e361347` |
| 예약(Reservation) | `sha256:531ed6ed5fd713f8d5eda5d3d26df1bedf36d59d44d35b2af8cbfa53ff1ec628` |
| 번들(Bundle) | `sha256:3f21798666ddc5ad76c73ba9706e93db032064b76e68a4965f0bc49b2c89a375` |
| ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 합성 영수증 | `sha256:350775f4c0bc9bb40937cec5c91fd5887f5678c7af349aa177406cec9c5e2f99` |

Catalog 조회에서 승인(admission)된 정확한 버전이 확인되었습니다. Program 실행은 `seed -> double -> finish` 순서로 완료되었고, 모든 노드가 정확히 한 번씩 실행되었습니다. 최종 채널 값은 `value = 12`, `path = "model-generated"`였습니다.

## 런타임 토폴로지 교체

합성 게이트웨이는 이제 호스트가 소유하는 `ProgramBudgetBounds` 아래에서 후속 Program을 컴파일합니다. 모든 리소스의 최댓값은 예약의 `remaining_after_reservation`입니다. 따라서 이 값은 모델 출력이 아니라 이미 차감을 마친 계보(lineage)에서 나온 권한 상한입니다. 하한은 JavaScript 런타임이 구조상 필요로 하는 최소값뿐이며, wall-time 1단위, 워커 1개, Program 연산 1개, Core 스텝 1개입니다. 소비형(consumable) grant와 자식(child) grant의 하한은 0으로 유지합니다. 호스트가 준 상·하한이 뒤집혀 있으면 소스를 평가하기 전에 거부합니다.

덕분에 정확한 교체(replacement)는 wall-time이 차감되어 더 작아진 계보의 잔여분을 그대로 이어받을 수 있고, 갱신할 수 없는 예산은 하나도 늘어나지 않습니다. 정확한 예산을 지정하는 기존 컴파일러 오버로드는 고정된 독립 실행 호출용으로 그대로 남아 있습니다.

별도의 원본 Program이 영속적인 최상위 `ng.checkpoint`에 도달했습니다. 호스트는 그 handoff를 소비한 뒤, 모델이 만든 토폴로지를 후속 Program으로 지정해 `ProgramRuntime::replace`를 호출했습니다. 교체 대상은 위에서 합성 게이트웨이가 만든 바로 그 ProgramVersion이었고, 두 번째 번들을 컴파일하거나 승인(admission)하지 않았습니다. 이 픽스처의 전환 결과는 다음과 같습니다.

| 증거 | 값 |
|---|---|
| 원본 ProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| 원본 run | `run-e153d50d90cc8d222c5f363c99399569` |
| 대상 run | `sha256:590048ab76d42119e184f89eb88701cd8df32a84bc244286f415c0b53086a089` |
| 대상 ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 교체 영수증 | `sha256:79a334ee185bf2ac0115d7d79038c2adff7b4bb854f294c7a5585924d8376fc3` |
| 활성 세대(generation) | `2` |
| 대상 상태 | `completed` |

교체 뒤에 낡은 것이 된 원본 노드는 한 번도 실행되지 않았습니다. 후속 Program의 실행 순서는 `seed -> double -> finish`였고 모든 노드가 한 번씩 실행되었으며, 최종 출력에도 `value = 12`, `path = "model-generated"`가 다시 들어 있었습니다.

가장 최근에 DeepSeek으로 실제 실행한 결과는 `replacement_uses_synthesis_version =
true`, 대상 상태 `completed`, 활성 세대 `2`였고, 낡은 노드가 실행되지 않았다는 결과도 같았습니다.

## 실패 사례와 프롬프트 계약

앞선 모델 출력은 실행 전에 거부되었습니다.

- `P_JS_DEFINE_MISSING`: 동기 방식으로 export된 `define()`이 없음
- `P_JS_DEFINE_VALUE`: `define()`이 불투명한 `ng.graph()` 빌더가 아니라 그래프 모양의 일반 데이터를 반환함
- `P_JS_GRAPH_ARGUMENT`: channel/node 빌더 인자가 검토를 거친 DSL 스키마와 맞지 않음
- `P_JS_EVALUATION`: 따옴표 없는 reducer 식별자가 제한된 QuickJS 컨텍스트에 존재하지 않는 주변(ambient) 상태를 참조함

그래서 성공한 프롬프트는 신뢰할 수 있는 작성(authoring) 인터페이스를 정확히 명시했습니다. `ng.graph`, `initial`을 가진 `graph.channel`, `graph.node(name, {type})`, entry, edge, exit, 봉인된 `ng.callCore` command입니다. 유효하지 않은 제안은 ProgramVersion을 만들지 못했고 노드도 실행하지 못했습니다.

## 범위의 한계

이 증거가 보여 주는 것은 외부 모델이 QuickJS 토폴로지 소스를 합성할 수 있고, 그 소스를 예약하고 컴파일하고 승인(admission)하고 게시하고 실행한 뒤, 영속적인 런타임 체크포인트에서 다른 Program 세대로 선택할 수 있다는 점입니다. 또한 합성 게이트웨이가 동적 컴파일 차감을 마치고 승인(admission)한 바로 그 버전이 후속 Program이 될 수 있다는 점도 보여 줍니다. 교체 경로에서 런타임은 제안을 권한으로 다시 쓰지 않고 소스를 다시 컴파일하지도 않습니다.

이 Program 수준의 교체를 임의의 GraphEngine 상태/frontier 마이그레이션과 혼동해서는 안 됩니다. 원본 Core 토폴로지에서 모델 토폴로지로 가는 마이그레이션 계획은 구체화(materialization) 방식과 런타임 계약이 달랐기 때문에 `blocked`로 분류되었고, 이는 올바른 결과였습니다.

NeoGraph에는 이제 일부러 범위를 좁힌 P1 GraphEngine 경로인 `GraphSemanticMigrationAdapter`도 있습니다. 호스트는 승인(admission)된 정확한 원본과 대상 아티팩트로 이 불변 어댑터를 준비해야 합니다. 어댑터가 승인(admission)하는 Program은 선언만으로 이루어진(런타임 JavaScript 제어가 없는) 단일 루트 `call_core` Program이어야 하고, 체크포인트되는 채널/리듀서, 노드 이름, 엣지, 라우팅, barrier, 재시도/인터럽트 구조, capability 바인딩, 권한, 입출력 계약이 모두 같아야 합니다. 따라서 identity 매핑이 적용된 frontier와 채널 스냅샷을 이어받되, 봉인된 Core 정의와 컴파일된 plan의 식별자는 다른 후속 Program으로 옮길 수 있습니다. 어댑터는 마이그레이션 영수증에 저장되고 복구할 때 다시 검증됩니다.

QuickJS 제어, 노드/frontier 이름 변경, 채널이나 리듀서 변환, barrier 구성원 변경, 대기 중인 효과, 자식 Program, 임의의 토폴로지 편집은 여전히 실패 시 차단 방식입니다. 이런 경우에는 이후의 매핑 유형이 영향받는 모든 상태 차원을 증명하기 전까지 명시적인 handoff/재시작이 계속 필요합니다. 자식 Program의 자동 바인딩/스폰, 모든 합성 경계에 걸친 크래시 복구, Program 안에서 쓰는 `ng.proposeProgram` command 인터페이스도 별도의 자격 검증 단계로 남아 있습니다.

## 모델이 생성한 P1 GraphEngine 마이그레이션

P1 어댑터는 실제 `deepseek/deepseek-v4-flash-0731` OpenRouter 응답 `gen-1787291529-fCOHp8pry7EwHHHu1MUH`로 처음부터 끝까지 실행해 보기도 했습니다. 모델은 선언만으로 이루어진 QuickJS `define()` 소스(SHA-256 `346329bf39790cc5557a9961a7faa5da0b35168f84257b12d6166565d594df08d`)를 생성했습니다. 이 소스의 토폴로지는 원본 그래프의 `work -> followup` frontier 형태를 유지하면서, `migration_epoch: 2`로 서로 다른 대상 Core 정의를 만들어 냈습니다.

```text
model QuickJS define()
  -> ProgramSynthesisProposal
  -> dynamic-compile reservation
  -> ProgramCompiler + ProgramCatalog admission
  -> GraphSemanticMigrationAdapter preparation
  -> durable GraphEngine generation-2 migration
  -> recovery-proof validation
```

번들이나 구체화(materialization)가 바뀌었을 때 필요한 대로, 일반 마이그레이션 계획은 `blocked`로 남았습니다. 그다음 호스트가 만든 어댑터가 범위가 좁은 identity 투영을 승인(admission)했습니다. 대상은 세대 `2`에서 완료되었고, `work`는 원본 세대에서 한 번, `followup`은 후속 세대에서 한 번 실행되었습니다. 어댑터의 정확한 식별자는 마이그레이션 영수증에 저장되었습니다.

재현하려면 `program_model_synthesis_probe` 타깃을 빌드하고 다음을 실행합니다.

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```

GraphEngine P1 경로는 `program_model_semantic_migration_probe`를 사용해 다음과 같이 실행합니다.

```powershell
bun run scripts/run_model_semantic_migration_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_semantic_migration_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
