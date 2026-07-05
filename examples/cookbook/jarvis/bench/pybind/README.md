# 파이썬 모드 벤치 — NeoGraph-from-Python vs LangGraph

핵심 질문: **pybind 로 파이썬에서 NeoGraph 를 쓰면(노드 본문도 파이썬), C++
standalone 의 우위(기동·RSS·처리량)가 증발하는가?**

답: **아니다.** bloat 은 파이썬-인터프리터가 아니라 LangChain import 트리의
것이었다. NeoGraph-from-Python = lean 파이썬(10MB/30ms) + 단일 컴파일 .so.

## 실측 (2026-07-05, WSL2, python3.12)

```bash
cd <neograph>/build-pybind
LD=$(pwd)
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/startup_rss.py neograph
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/perturn.py   neograph 5000
python3 <bench>/pybind/startup_rss.py langgraph          # bare
python3 <bench>/pybind/startup_rss.py langgraph_openai   # 실제 챗봇 스택
python3 <bench>/pybind/perturn.py   langgraph 5000
```

| 지표 (전부 파이썬 프로세스) | NeoGraph-from-Python | LangGraph | 우위 |
|---|---|---|---|
| per-turn (5 파이썬-콜러블 노드, GIL 포함) | **0.38ms · ~2620 turns/s** | 0.93ms · ~1075 | 2.4× |
| 기동 (import→compile) | **40ms** | 462ms (bare) / 2977ms (+langchain_openai) | 11–73× |
| RSS | **36MB** | 61MB (bare) / 561MB (+langchain_openai) | 1.7–15× |
| (참고) bare python3 RSS | — | 9.9MB | |

## 왜 파이썬 모드에서도 빠른가

- **per-turn**: BSP 기계(슈퍼스텝 루프·스케줄·채널 리듀스·라우팅·체크포인트
  부기)가 C++ 로 돌고 **노드 본문만 파이썬**이다. LangGraph 는 그 기계 전체가
  순수 파이썬 Pregel. GIL 은 양쪽 다 노드 실행 중 쥐지만, 노드 *사이*의
  오케스트레이션이 NeoGraph 는 C++ 라 싸다. pybind/GIL 경계 비용이 거의 없어
  standalone C++ mock(9노드 0.38ms)과 파이썬 5노드가 사실상 동률.
- **기동/RSS**: `import neograph_engine` 은 단일 .so 로드. LangGraph 의 462ms/
  61MB 는 langgraph+langchain-core 의 import 트리, langchain_openai 까지 얹으면
  2977ms/561MB. NeoGraph 는 그 트리가 없다.

## 함의

파이썬에서 NeoGraph 를 쓰면 **파이썬 생태계 전체(HF·OpenAI SDK·pandas 등을
노드 본문에서 인라인 호출) + 기동·RSS·처리량 우위를 동시에** 가진다.
즉 "성능은 C++ standalone, 생태계는 파이썬" 이라는 이분법은 틀렸다 —
파이썬 모드가 둘 다 준다. standalone C++ 는 여기서 한 발 더(기동 8ms·RSS
7.5MB) 나가지만 노드를 C++ 로 짜거나 도구를 HTTP 로 밖에 부를 때만.

주의: 노드가 torch/HF 를 import 하면 RSS 는 그 라이브러리가 지배한다
(엔진은 노이즈). 이건 프레임워크가 아니라 워크로드의 성질이며 양쪽 동일.
