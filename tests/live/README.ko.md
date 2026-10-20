<!-- neograph-i18n: source=tests/live/README.md locale=ko source_sha256=ad1342183f4ed785446bb9e1ad229fa82969da6557f52beee7bb313a6d961138 -->
# 라이브 엔드투엔드 스위트

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

단위 테스트와 통합 테스트는 녹화했거나 스크립트로 만든 공급자 트래픽을 재생합니다. 그래서 공급자가 NeoGraph가 보내는 요청을 지금도 받아들이는지, NeoGraph가 공급자의 오늘 응답을 받아들이는지는 보여 주지 못합니다. 따라서 모든 릴리스는 실제 키로 이 스위트를 통과해야 하고, 해당 커밋에 통과한 리포트가 없으면 CI가 릴리스 게시를 거부합니다.

## 다루는 범위

- **Python** (`bindings/python/tests/live/`와 `bindings/python/tests/test_live_provider.py`):
  - family 프리셋 여덟 개(OpenAI Chat과 Responses, Anthropic Messages, Gemini Generate와 Interactions, OpenRouter Chat, Responses, Messages)를 각각 버퍼와 스트림으로 호출하며, 스트림 델타를 이어 붙인 결과가 최종 텍스트와 같은지 확인합니다;
  - Claude 5.5 모델;
  - 이미지 입력;
  - 모든 프리셋에서 강제 툴 호출, 그리고 툴 왕복 전체;
  - OpenRouter Chat과 Gemini Generate가 생성한 이미지의 보존;
  - Anthropic Messages 제어: thinking 모드와 effort, tool choice, temperature 규칙, 프롬프트 캐시, 그리고 OpenRouter를 거친 같은 제어;
  - 타입이 정해진 실패: 잘못된 키, 취소된 스트림, 이미 지난 deadline.
- **C++** (`tests/live_e2e_test.cpp`, 타깃 `neograph_live_e2e_tests`):
  - Claude 5.5 서버 툴, 생성 이미지, Interactions를 쓰는 공급자 어댑터;
  - `Provider`를 거치는 SchemaProvider 엔드포인트(#531): OpenAI·OpenRouter 임베딩, OpenAI moderation, Anthropic 토큰 수 세기;
- **알려진 빈틈**은 그 릴리스 라인에서 반드시 실패해야 하는 케이스입니다(strict xfail). `tests/live/manifest.json`이 고칠 이슈와 함께 이를 나열합니다. 수정이 들어오면 케이스가 통과하기 시작하므로 매니페스트도 함께 바꿔야 합니다.

호출마다 싼 모델과 작은 상한을 씁니다. 전체 실행 비용은 몇 미국 센트입니다.

## 실행

확인할 커밋의 Python 확장과 C++ 테스트를 빌드한 다음 다음을 실행합니다:

```bash
python scripts/live_e2e.py run --env-file path/to/keys.env \
    --python path/to/python --pythonpath path/to/build \
    --cxx path/to/build/tests/neograph_live_e2e_tests
```

- **키.** env 파일에는 `OPENAI_API_KEY`, `GEMINI_API_KEY`, `ANTHROPIC_API_KEY`, `OPENROUTER_API_KEY`가 들어갑니다. 스크립트는 이 값을 테스트 프로세스에만 넘깁니다. 출력하거나 저장하지 않으며, 실패 메시지에 키 값이 나타나면 다른 문자열로 바꿉니다.
- **깨끗한 트리.** 리포트는 커밋을 가리키므로, `run`은 커밋되지 않은 변경이 있는 작업 트리를 거부합니다.
- **버전.** `run`은 트리의 버전과 다른 `neograph_engine`을 거부합니다.
- **출력.** `tests/live/report.json`을 씁니다. 커밋, 트리 해시, NeoGraph 버전, SchemaProvider 고정 커밋, 그리고 모든 케이스와 그 결과가 담깁니다.
- **부분 실행.** `--select`와 `--cxx-filter`로 스위트의 일부만 실행할 수 있습니다. 부분 리포트로는 릴리스를 승인할 수 없습니다.

## 릴리스 게이트

1. 릴리스 풀 리퀘스트(`release/*` 브랜치)는 최종 커밋에서 실행해 얻은 `tests/live/report.json`을 커밋합니다.
2. `Live e2e report` 워크플로가 키 없이 `python scripts/live_e2e.py verify`를 실행합니다. 다음이 모두 성립해야 통과합니다:
   - 리포트가 이 트리(리포트를 뺀 모든 추적 파일)를 가리킨다;
   - 리포트가 이 버전의 것이고 깨끗한 트리에서 만들어졌다;
   - 매니페스트의 모든 케이스가 예상한 결과를 냈다. 통과했거나, 목록에 있는 알려진 빈틈으로서 실패했다;
   - 그 밖에 실패한 케이스가 없다.
3. Wheels 워크플로는 PyPI에 게시하기 전에 릴리스 태그에서 같은 검사를 합니다.

케이스를 추가하거나 지우면 `python scripts/live_e2e.py manifest --python … --pythonpath … --cxx …`가 스위트가 수집한 케이스로 매니페스트를 다시 쓰고, 알려진 빈틈은 그대로 둡니다. 커밋하기 전에 차이를 검토하세요.
