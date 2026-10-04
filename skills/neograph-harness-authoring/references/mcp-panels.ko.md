<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/mcp-panels.md locale=ko source_sha256=ce9166dfce50a6fe8b400d1e21c0f1eba65d4af1e7f990ee9a5d4b03a7f68e8a -->
# MCP 패널 절차

**Languages:** [English](mcp-panels.md) | [한국어](mcp-panels.ko.md) | [日本語](mcp-panels.ja.md) | [简体中文](mcp-panels.zh-CN.md)

1. `neograph_schema`를 호출하세요. 이 빌드가 반환한 프리셋과 필드만 사용하세요.
2. 정확한 목표, 수락 기준, 제한된 예산, 각 작업자의 JSON 출력 스키마를
   갖춘 요청 하나를 구성하세요.
3. 검토 작업에는 `pr_review_panel`을 사용하고 `policy.read_only`를 true로
   설정하세요. `policy.evidence_required`에는 모든 발견 사항 스키마에서
   요구하는 근거 필드를 지정하세요.
4. 각 작업자에게 필요한 도구 ID만 부여하세요. 읽기 전용 도구를 표시하고,
   경로를 담는 문자열 인수를 `path_arguments`에 나열하세요. 이러한 인수가
   있으면 항상 `policy.workspace_roots`를 명시적으로 설정하세요.
5. `neograph_compile`을 호출하세요. `ok`가 false이면 `phase`, `path`,
   `source`를 기준으로 진단을 해결하세요. 거부된 요청으로 start를 호출하지 마세요.
6. 보관한 `artifact_id`로 `neograph_start`를 호출하세요.
7. `run_id`로 `neograph_get`을 폴링하세요. 상태가 `awaiting_tool_results` 또는
   `input_required`이면 반환된 `pending` 호출만 처리한 뒤, 같은 `run_id`,
   정확한 `call_id`, `result_schema`를 따르는 결과로 `neograph_resume`을
   호출하세요. 동일한 중복 요청은 확인된 것으로 취급하세요.
   다른 호출 ID로 대체하지 마세요.
8. 상태가 종료 상태가 될 때까지 폴링하세요. 간결한 결과를 주 컨텍스트에 유지하세요.
9. 최종 답변에 작업자 세부 정보나 실행 추적이 필요한 경우에만, 반환된
   `neograph://runs/...` URI를 해당 `run_id`로 `neograph_get`을 호출하여 조회하세요.
10. 부분 결과, 발견 사항 없음, 시간 초과, 취소, 만료, 최대 단계 도달, 실패
    결과를 그대로 보고하세요. 일반적인 성공으로 바꾸어 보고하지 마세요.

## 피해야 할 패턴

- 인라인 요청에서 `neograph_compile`을 생략하지 마세요.
- 모든 작업자에게 광범위한 도구 목록을 붙이지 마세요.
- 작업 공간 루트 없이 경로를 담는 도구를 구성하지 마세요.
- 형식이 잘못되었거나 비어 있는 작업자 출력을 빈 발견 사항 목록으로 취급하지 마세요.
- 간결한 결과를 통해 필요성이 확인되기 전에 상세 추적을 가져오지 마세요.
- 읽기 전용 검토에 쓰기 가능한 도구를 추가하지 마세요.
- 호출 ID가 소비된 뒤 데이터를 바꿔 호스트 결과를 다시 제출하지 마세요.
- MCP Tasks가 핵심 프로토콜에 포함된 지원이라고 가정하지 마세요. 서버와 개별
  요청이 실험적 `io.modelcontextprotocol/tasks` 확장 사용을 명시적으로 선택한
  경우가 아니면 안정적인 `neograph_get` 폴링을 사용하세요.

## 예제

PR 검토에서는 호스트의 저장소 도구로 diff를 수집하여 작업 목표에 넣고,
정확성과 보안에 대해 서로 다른 지시를 받은 작업자 두 명을 사용하세요.
각 발견 사항에 `file`, `line`, `evidence`를 요구하세요. 전체 요청과 호스트
설정 명령은 저장소의 [HARNESS_MCP.md](../../../docs/HARNESS_MCP.md)를 참고하세요.
