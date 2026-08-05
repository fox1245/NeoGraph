<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/pybind/README.md locale=ko source_sha256=097ce5a3394ec214d04a7245b7e4e7e6d937754bb1c18e589a850dc1bc41f54d -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# Python 모드 벤치마크 — Python의 NeoGraph-from-Python 대 LangGraph


핵심 질문: **pybind(노드 본문도 Python)를 통해 Python에서 NeoGraph를 사용하면 문제가 제거됩니까?
독립형 C++의 장점(시작 · RSS · 처리량)?**

답변: **아니오.** 부풀림은 Python 인터프리터가 아니라 LangChain 가져오기 트리에서 발생합니다.
NeoGraph-from-Python = 린 Python(10MB/30ms) + 단일 컴파일된 .so.

## 재현

먼저 Python 개발 헤더와 pybind11이 필요한 소스 빌드를 구성합니다.

```bash
cmake -S . -B build-pybind \
  -DNEOGRAPH_BUILD_PYBIND=ON -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-pybind --target _neograph -j
LD="$PWD/build-pybind"
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" \
  python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py neograph
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" \
  python3 examples/cookbook/jarvis/bench/pybind/perturn.py neograph 5000
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph_openai
python3 examples/cookbook/jarvis/bench/pybind/perturn.py langgraph 5000
```

|측정항목(모든 Python 프로세스)|Python의 NeoGraph|랭그래프|이점|
|---|---|---|---|
|턴당(Python 호출 가능 노드 5개, GIL 포함)|**0.38ms · ~2620 turns/s**|0.93ms · ~1075| 2.4× |
|시작(가져오기→컴파일)|**40ms**|462ms(베어) / 2977ms(+langchain_openai)| 11–73× |
|RSS|**36MB**|61MB(베어) / 561MB(+langchain_openai)| 1.7–15× |
|(참조) 베어 python3 RSS| — |9.9MB| |

## Python 모드에서도 빠른 이유

- **턴당**: BSP 엔진(슈퍼스텝 루프 · 스케줄러 · 채널 축소 · 라우팅 · 체크포인트
오버헤드)는 C++에서 실행되며 **노드 본문만 Python입니다**. LangGraph의 엔진은 순수한 Python Pregel입니다.
GIL는 두 노드 모두에서 노드 실행 중에 유지되지만 NeoGraph의 *간* 노드 오케스트레이션은 C++이므로 더 빠릅니다.
pybind/GIL 경계 비용은 거의 0이므로 독립형 C++ 모의(9개 노드 0.38ms) 및 Python 5 노드는
효과적으로 묶였습니다.
- **startup/RSS**: `import neograph_engine`는 단일 .so를 로드합니다. LangGraph의 462ms/
61MB는 langgraph+langchain-core 가져오기 트리이며 langchain_openai를 사용하면 최대 2977ms/561MB입니다. NeoGraph에는 그러한 트리가 없습니다.

## 시사점

Python에서 NeoGraph를 사용하면 **전체 Python 생태계(HF·OpenAI SDK·pandas 등)를 제공합니다.
노드 본체의 인라인) + 시작 · RSS · 동시에 처리량 이점**.
즉, "성능은 C++ 독립형이고 생태계는 Python"이라는 이분법은 잘못된 것입니다.
Python 모드는 두 가지를 모두 제공합니다. 독립형 C++는 한 단계 더 발전합니다(시작 8ms · RSS
7.5MB) 그러나 노드가 C++이거나 HTTP를 통해 호출되는 도구인 경우에만 해당됩니다.

주의: 노드가 torch/HF를 가져오는 경우 RSS는 해당 라이브러리에 의해 지배됩니다.
(엔진 소음입니다.) 이는 프레임워크가 아닌 워크로드 속성입니다. 둘 다 동일합니다.
