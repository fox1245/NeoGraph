<!-- neograph-i18n: source=tests/live/README.md locale=ko source_sha256=35dc6382d17b6c83ead0c106b60b92a35350a6139b38d9fa91a5019009e8872c -->
# 라이브 엔드투엔드 스위트

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

단위 테스트와 통합 테스트는 녹화했거나 스크립트로 만든 공급자 트래픽을 재생합니다. 그래서 공급자가 NeoGraph가 보내는 요청을 지금도 받아들이는지, NeoGraph가 공급자의 오늘 응답을 받아들이는지는 보여 주지 못합니다. 따라서 모든 릴리스는 실제 키로 이 스위트를 통과해야 하고, 해당 커밋에 통과한 리포트가 없으면 CI가 릴리스 게시를 거부합니다.

## 다루는 범위

- **Python** (`bindings/python/tests/live/`와 `bindings/python/tests/test_live_provider.py`):
  - family 프리셋 여덟 개(OpenAI Chat과 Responses, Anthropic Messages, Gemini Generate와 Interactions, OpenRouter Chat, Responses, Messages)를 각각 버퍼와 스트림으로 호출하며, 스트림 델타를 이어 붙인 결과가 최종 텍스트와 같은지 확인합니다;
  - Claude 5.5 모델;
  - 이미지 입력;
  - 모든 프리셋에서 강제 툴 호출, 그리고 툴 왕복 전체;
  - Gemini Generate가 생성한 이미지의 보존;
  - Generate의 응답 modality와 음성 설정을 쓰는 Gemini 음성 합성(버퍼와 스트림)과 Lyria 클립;
  - OpenAI Responses의 hosted 웹 검색(버퍼와 스트림);
  - Anthropic Messages 제어: thinking 모드와 effort, tool choice, temperature 규칙, 프롬프트 캐시, 그리고 OpenRouter를 거친 같은 제어;
  - 그래프에서 실행하는 긴 Anthropic 에이전트 실행(`test_live_anthropic_agent.py`): 서로 의존하는 도구 연쇄, 첫 턴을 기억해야 하는 최대 여섯 턴의 대화, 도구 라운드를 거쳐 다음 턴까지 이어지는 서명된 thinking과 redacted thinking, 병렬 도구 호출, 도구 오류와 재시도, 사람이 도구 호출마다 검토하거나 승인하는 흐름, 대화 전체에 걸친 프롬프트 캐시, Claude 5.5 adaptive thinking, 그리고 같은 루프를 스트림·비동기·OpenRouter로 실행;
  - 타입이 정해진 실패: 잘못된 키, 취소된 스트림, 이미 지난 deadline;
  - `Provider`를 거치는 SchemaProvider 엔드포인트(#531): OpenAI 임베딩, await로 기다리는 OpenRouter 임베딩, OpenAI moderation.
- **C++** (`tests/live_e2e_test.cpp`, 타깃 `neograph_live_e2e_tests`):
  - Claude 5.5 서버 툴, 생성 이미지, Interactions를 쓰는 공급자 어댑터;
  - `Provider`를 거치는 SchemaProvider 엔드포인트(#531): OpenAI·OpenRouter 임베딩, OpenAI moderation, Anthropic 토큰 수 세기, OpenAI 이미지, 텍스트로 다시 전사되는 OpenAI 음성(음성 엔드포인트의 유일한 라이브 테스트), 요청에서 참조하는 Gemini 파일, 제출부터 취소까지의 Anthropic·OpenAI 배치;
  - OpenAI와 Google의 SchemaProvider 실시간 전사 세션. OpenAI Realtime 세션이 소리 내어 읽은 음성을 넣고, 각 세션이 말한 단어가 담긴 사용자 턴 하나를 커밋하는지 확인합니다;
  - 네 공급자의 목록 전체에서 갱신한 모델 카탈로그, 갱신된 카탈로그에만 있는 모델 호출, 어느 카탈로그에도 없지만 그래도 전송되는 모델;
- **알려진 빈틈**은 그 릴리스 라인에서 반드시 실패해야 하는 케이스입니다(strict xfail). `tests/live/manifest.json`이 고칠 이슈와 함께 이를 나열합니다. 수정이 들어오면 케이스가 통과하기 시작하므로 매니페스트도 함께 바꿔야 합니다.

호출마다 싼 모델과 작은 상한을 씁니다. 전체 실행은 약 8분 걸리고 비용은 약 30미국 센트이며, 대부분은 Anthropic 에이전트 실행입니다.

## 실행

확인할 커밋의 Python 확장과 C++ 테스트를 빌드한 다음 다음을 실행합니다:

```bash
python scripts/live_e2e.py run --env-file path/to/keys.env \
    --python path/to/python --pythonpath path/to/build \
    --cxx path/to/build/tests/neograph_live_e2e_tests
```

- **키.** env 파일에는 `OPENAI_API_KEY`, `GEMINI_API_KEY`, `ANTHROPIC_API_KEY`, `OPENROUTER_API_KEY`가 들어갑니다. 스크립트는 이 값을 테스트 프로세스에만 넘깁니다. 출력하거나 저장하지 않으며, 실패 메시지에 키 값이 나타나면 다른 문자열로 바꿉니다.
- **모델.** `tests/live/manifest.json`의 `models` 섹션은 각 모델 역할(`openai.chat`, `anthropic.messages`, `anthropic.messages.adaptive` 등)을 기본 모델 ID에 매핑하며, Python 스위트는 `live_support.live_model(role)`로 이를 조회합니다. Google 모델은 OpenRouter를 거치지 않고 `GEMINI_API_KEY`(`google.generate`와 `google.interactions` 역할)로 직접 호출합니다. 환경 변수나 env 파일에 `NG_LIVE_MODEL_<ROLE>`(`.`과 `-`를 `_`로 바꾸고 대문자로 쓴 이름, 예: `NG_LIVE_MODEL_OPENAI_CHAT`)을 설정하면 코드를 바꾸지 않고 해당 역할의 모델을 덮어쓸 수 있습니다.
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

## master에서

`Live e2e report` 워크플로는 master에 푸시가 들어올 때마다 실제 키로 스위트 전체도 실행합니다. 그래서 공급자 쪽 변화나 회귀가 다음 릴리스가 아니라 들어온 그날 드러납니다. 실행 결과 `report.json`은 빌드 아티팩트로 올라갑니다. 리포트를 커밋하지 않으며, 릴리스를 승인하지도 않습니다.

- **키.** 네 키는 master만 쓸 수 있는 GitHub 환경 `live-e2e`의 시크릿입니다. GitHub은 시크릿 값을 누구에게도 보여 주지 않고, 로그에서 가리며, 풀 리퀘스트 워크플로에는 넘기지 않습니다. 잡은 러너 사용자만 읽을 수 있는 파일에 키를 쓰고, 잡이 끝나면 그 파일을 지웁니다.
- **한 번에 하나.** master의 실행은 중간에 취소되지 않습니다. 실행 중에 들어온 푸시는 기다리며, 기다리는 푸시 가운데 가장 새것만 다음에 실행됩니다.
