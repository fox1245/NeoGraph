<!-- neograph-i18n: source=benchmarks/prompt_cache/README.md locale=ko source_sha256=6a92a60b5c6c8db6ee25c9ea02812b828fbd6188a35b871288a902c7b7d1366e -->
# 프롬프트 캐시 접두부 프로브

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

제공자의 프롬프트 캐시는 첫 바이트부터 이전 요청과 똑같은 요청의 앞부분을 재사용합니다. NeoGraph 런타임은 고정된 컨텍스트 아티팩트 집합과 늘어나는 RAW 히스토리로 매 요청을 조립합니다([`docs/STRICT_RUNTIME_INTERPOSITION.md`](../../docs/STRICT_RUNTIME_INTERPOSITION.md)의 "Artifact placement" 참고). 이 프로브는 턴마다 바뀌는 작은 블록을 어디에 두느냐에 따라 제공자가 각 요청에서 재사용할 수 있는 양이 어떻게 달라지는지 측정합니다(이슈 #329).

## 하는 일

`prompt_cache_probe.py`는 12~20턴짜리 대화를 `RuntimeTurnAssembler`와 `ControlledProvider`로 실행합니다.

- 긴 고정 블록: 제공자의 캐시 최소 길이를 넘도록 4,500~5,000토큰 분량의 `RequiredSkill` 아티팩트(`--bible-entries`, 90~115).
- 매 턴 질문과 답이 하나씩 늘어나는 RAW 히스토리.
- 매 턴 바뀌고 시나리오가 배치를 정하는 약 400토큰짜리 동적 블록(`--dynamic-lines`).

제공자 호출마다 제공자가 보고한 사용량(입력, 캐시 읽기, 캐시 쓰기, 캐시되지 않은 토큰)과, 인코딩된 요청 중 직전 요청과 똑같은 부분을 기록합니다. 그 부분은 와이어 본문의 바이트 수와, 읽는 순서(도구, 시스템, 메시지)로 본 프롬프트의 문자 수로 잰 두 가지입니다.

시나리오 이름은 동적 블록의 배치이며, 뒤에 `+compact`를 붙이면 `--compact-at`에서 압축이 추가됩니다. 가장 오래된 턴이 RAW 창에서 빠지고, 그 턴들의 요약이 아티팩트로 추가됩니다. 배치는 `none`, `default`(`BeforeLatestUser`), `before-history`, `after-history`, `raw-append`(블록을 아티팩트로 두지 않고 RAW 피드에 덧붙임), `system-tail`, `system-head`, `tie-break`입니다. 요약은 프로브가 기록된 질문과 답으로 직접 쓰며 `compact_history`를 호출하지 않습니다.

## 실행

Python 확장을 빌드한 다음 실행합니다.

```bash
# No network and no key: only the request-prefix columns are filled.
PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py --offline \
    --preset anthropic.messages --model any-name --turns 14 --compact-at 10 \
    --scenarios none,default,before-history,after-history,raw-append,compaction

# Live: the key is read from the environment variable of the preset.
OPENAI_API_KEY=... PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py \
    --preset openai.chat --model <model> --rates input=...,cache_read=...,output=... \
    --budget-usd 0.05 --turns 16 --scenarios before-history,after-history,compaction
```

- **프리셋.** `openai.chat`, `anthropic.messages`, `google.generate`, `openrouter.chat`. `ProviderControls` 필드는 `--control NAME=VALUE`로 지정하며 family 중첩은 점으로 구분하고, Messages 요청 수준 캐시 제어는 `--cache-control 5m`으로 보냅니다. 프로브가 답을 이식 가능한 텍스트로 기록하므로 Gemini에는 `--control generate.history_mode=GeminiHistoryMode.PortableForeign`이 필요합니다.
- **비용.** 라이브 실행에는 지출을 제한하고 보고하기 위한 `--rates`(100만 토큰당 USD)가 필요합니다. 캐시 적중이 전혀 없는 최악의 경우가 `--budget-usd`를 넘으면 시작을 거부하고, 지출이 이를 넘으면 멈춥니다. DeepSeek처럼 백그라운드에서 캐시를 만드는 제공자는 턴 사이에 몇 초의 `--pause-ms`가 필요합니다.
- **키.** 프로브는 환경 변수에서 키를 읽으며 출력하거나 저장하지 않습니다.
- **출력.** 시나리오별 표와 요약이 나오고, `--out`을 주면 호출별 기록이 JSON으로 저장됩니다. `--show-order N`은 N번째 호출의 메시지 순서를 출력합니다.
