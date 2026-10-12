<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/mcp-panels.md locale=ko source_sha256=ce9166dfce50a6fe8b400d1e21c0f1eba65d4af1e7f990ee9a5d4b03a7f68e8a -->
# MCP 패널 절차

**Languages:** [English](mcp-panels.md) | [한국어](mcp-panels.ko.md) | [日本語](mcp-panels.ja.md) | [简体中文](mcp-panels.zh-CN.md)

1. `neograph_schema`를 호출하고, 이 빌드가 반환한 프리셋과 필드만 사용합니다.
2. 요청 하나를 만듭니다. 구체적인 목표, 수락 기준, 한정된 예산을 넣고, 모든 워커에 JSON
   출력 스키마를 지정합니다.
3. 리뷰 작업에는 `pr_review_panel`을 쓰고 `policy.read_only`를 true로 설정합니다.
   `policy.evidence_required`에는 모든 지적 사항 스키마가 요구하는 근거 필드를 지정합니다.
4. 각 워커에는 필요한 도구 ID만 줍니다. 읽기 전용 도구는 표시해 두고, 경로를 받는 문자열
   인자는 `path_arguments`에 나열합니다. 이런 인자가 있으면 `policy.workspace_roots`를
   반드시 명시합니다.
5. `neograph_compile`을 호출합니다. `ok`가 false이면 진단 결과의 `phase`, `path`,
   `source`를 보고 고칩니다. 거부된 요청으로 start를 호출해서는 안 됩니다.
6. 보관해 둔 `artifact_id`로 `neograph_start`를 호출합니다.
7. `run_id`로 `neograph_get`을 폴링합니다. 상태가 `awaiting_tool_results`나
   `input_required`이면 반환된 `pending` 호출만 처리한 뒤, 같은 `run_id`, 정확한
   `call_id`, `result_schema`에 맞는 결과로 `neograph_resume`을 호출합니다. 내용이 똑같은
   중복 요청은 이미 확인된 것으로 봅니다. 다른 호출 ID로 바꿔서 보내면 안 됩니다.
8. 종료 상태가 될 때까지 계속 폴링합니다. 간결한 결과는 메인 컨텍스트에 보관합니다.
9. 반환된 `neograph://runs/...` URI는, 최종 답변에 워커별 세부 내용이나 실행 추적이 필요할
   때만 그 `run_id`로 `neograph_get`을 호출해 조회합니다.
10. 부분 결과, 지적 사항 없음, 타임아웃, 취소, 만료, 최대 스텝 도달, 실패는 있는 그대로
    보고하고, 일반적인 성공으로 뭉뚱그리지 않습니다.

## 안티패턴

- 인라인 요청이라도 `neograph_compile`을 건너뛰면 안 됩니다.
- 모든 워커에 광범위한 도구 카탈로그를 붙이면 안 됩니다.
- 워크스페이스 루트 없이 경로를 받는 도구를 설정하면 안 됩니다.
- 형식이 잘못되었거나 비어 있는 워커 출력을 빈 지적 사항 목록으로 취급하면 안 됩니다.
- 간결한 결과로 필요성이 확인되기 전에 상세 추적을 가져오면 안 됩니다.
- 읽기 전용 리뷰에 쓰기 가능한 도구를 추가하면 안 됩니다.
- 호출 ID가 이미 사용된 뒤에는 데이터를 바꿔서 호스트 결과를 다시 보내면 안 됩니다.
- MCP Tasks가 코어 프로토콜의 지원 범위라고 가정하면 안 됩니다. 서버와 개별 요청이 모두
  실험적 확장 `io.modelcontextprotocol/tasks`를 쓰겠다고 명시하지 않았다면, 안정적인
  `neograph_get` 폴링을 씁니다.

## 예제

PR 리뷰에서는 호스트의 저장소 도구로 diff를 모아 작업 목표에 넣고, 정확성과 보안에 대해
서로 다른 지시를 준 워커 두 개를 씁니다. 각 지적 사항에 `file`, `line`, `evidence`를
요구합니다. 전체 요청과 호스트 설정 명령은 저장소의
[HARNESS_MCP.md](../../../docs/HARNESS_MCP.md)를 참고하세요.
