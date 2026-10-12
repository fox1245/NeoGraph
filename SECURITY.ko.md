<!-- neograph-i18n: source=SECURITY.md locale=ko source_sha256=1846450e26db18b21a90809734487fbc5fef135a3425887bd70fae57a20beb4e -->
# 보안 정책

**Languages:** [English](SECURITY.md) | [한국어](SECURITY.ko.md) | [日本語](SECURITY.ja.md) | [简体中文](SECURITY.zh-CN.md)

## 지원 버전

NeoGraph는 1.0 이전 단계입니다. 보안 수정은 먼저 `master`에 반영되고, 현재
마이너 라인의 다음 릴리스로 배포됩니다. 이전 마이너 라인은 패치하지 않으므로
최신 릴리스로 업그레이드하십시오.

| 버전 | 지원 |
| ---- | ---- |
| `master` | 예, 수정이 가장 먼저 반영됨 |
| 0.13.x (최신 릴리스) | 예 |
| < 0.13 | 아니요 |

## 취약점 신고

보안 문제는 공개 이슈로 올리지 마십시오. GitHub에서 비공개로 신고해 주십시오.
저장소의 **Security** 탭에서 **Report a vulnerability**를 선택하면 됩니다
(<https://github.com/fox1245/NeoGraph/security/advisories/new>).

가능하면 다음 내용을 포함해 주십시오.

- 영향을 받는 버전이나 커밋
- 해당 구성 요소: Core 엔진, Program 런타임, Python 바인딩, MCP·A2A·ACP·gRPC 서버나
  클라이언트, 체크포인트 저장소, SchemaProvider 연동 중 어디인지
- 최소 재현 방법과 관찰한 영향

신고에 실제 API 키, 자격 증명, 개인정보를 넣지 마십시오.

처리 일정: 7일 안에 접수 확인, 14일 안에 1차 평가를 드립니다. 수정 일정은
심각도에 따라 정합니다. 수정은 패치된 릴리스가 나온 뒤 GitHub Security
Advisory로 공개하며, 해당하면 CVE도 발급합니다.

## 범위

대상: 이 저장소의 NeoGraph 소스, 배포된 wheel, 그리고 NeoGraph가 포함해서 배포하는
서드파티 코드(예: QuickJS, cpp-httplib) 중 NeoGraph를 통해 도달할 수 있는 문제.

대상 아님: NeoGraph가 노출하지 않는 서드파티 의존성의 문제(해당 업스트림에
신고해 주십시오), `docs/promo` 아래의 홍보 영상 도구, 그리고 예제입니다. 예제는
사용법을 보여 주기 위한 것이며 보안을 강화한 배포 구성이 아닙니다.

## 보안 모델 요약

- Program 소스와 그래프 정의는 코드처럼 다뤄야 합니다. 신뢰할 수 있는
  작성자에게서 받거나 승인(admission) 정책을 거친 것만 받아들이고, 실행 예산으로
  비용을 제한하십시오.
- 도구 결과와 모델 출력은 신뢰할 수 없는 데이터입니다. 모델이 주입된 지시를
  거부해 주리라 기대하지 말고, 부작용이 있는 도구는 ToolGate와 Hook으로 통제하십시오.
- API 키는 환경 변수나 호스트 콜백으로 전달하고, Program이나 그래프 JSON 안에
  넣지 마십시오. NeoGraph는 키를 로그, 이벤트, 와이어 오류에 기록하지 않습니다.
- NeoGraph와 SchemaProvider 전송 계층이 외부로 나가는 HTTPS 연결에는 TLS 1.2 이상이
  필요합니다.
