<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/pybind/README.md locale=ko source_sha256=721dbef65598b467d85737ce2bfa971f2362af4c95c8f724b9310338a1b887e9 -->
# Python 그래프 벤치마크: NeoGraph와 LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 스크립트는 Python 그래프 실행과 프로세스 기동을 측정한다. 모델 제공자를 호출하지 않으며 API 키도 필요 없다. 타입 제공자 요청·이벤트·결과 바인딩을 검증하는 벤치마크가 아니다. 아래 명령은 현재 패키지가 설치된 환경을 대상으로 하며, 이번 변경에서는 실행하지 않고 소스만 대조했다.

## 재현

현재 `neograph-engine` wheel을 설치한 Python 환경을 사용한다. 비교에는 `langgraph`, 더 큰 import 스택 측정에는 `langchain-openai`를 설치한다. 저장소 루트에서 실행한다.

```bash
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py neograph
python3 examples/cookbook/jarvis/bench/pybind/perturn.py neograph 5000
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph_openai
python3 examples/cookbook/jarvis/bench/pybind/perturn.py langgraph 5000
```

`perturn.py`는 준비 실행 후 다섯 노드 체인을 반복한다. 각 Python 노드는 `v` 채널을 하나 증가시키고, 매 측정 실행은 0에서 시작한다. 평균, p50, p90, 초당 실행 횟수를 출력한다. 실행 횟수는 양수여야 한다.

`startup_rss.py`는 새 프로세스에서 기동 시간(ms)과 최대 RSS를 출력한다. NeoGraph 분기는 패키지를 import하고 그래프 심볼 세 개를 참조하며 그래프를 compile하지 않는다. LangGraph 분기는 패키지를 import하고 한 노드 그래프를 compile한다. 작업 범위가 다르므로 시간 비율을 그래프 compile 속도 향상으로 해석하지 않는다. RSS 변환은 Linux의 `ru_maxrss` 단위(KiB)를 가정한다. macOS에서는 같은 변환을 그대로 사용하지 않는다. Windows에는 Python `resource` 모듈이 없다.

## 과거 측정

저장소는 이전에 아래 값을 보고했다. 현재 전환 빌드의 측정값이 아니며 이번 변경에서 다시 실행하지 않았다.

| 지표 | Python의 NeoGraph | LangGraph |
|---|---|---|
| 실행당 Python 노드 다섯 개 | 0.38 ms; 약 2620 회/s | 0.93 ms; 약 1075 회/s |
| 위의 서로 다른 범위로 측정한 기동 | 40 ms | 462 ms; `langchain_openai` 포함 시 2977 ms |
| 최대 RSS | 36 MB | 61 MB; `langchain_openai` 포함 시 561 MB |

NeoGraph는 그래프 스케줄러와 채널 reduction을 C++에서 실행하며 Python 노드 본문은 GIL을 획득한다. 이 측정은 GIL 경계 비용을 분리하지 않으며 다른 작업의 성능을 입증하지 않는다. 노드 안에서 PyTorch 같은 패키지를 import하면 그 메모리도 프로세스 사용량에 포함된다.
