<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=ko source_sha256=649e0973ac9e600a33d13f10df707c76d463ff1d0d7c8ccb7315b07471bb6f51 -->
# Python 클라이언트 오버헤드: 현재 실행자와 과거 결과

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 디렉토리는 로컬 프로세스 내 프로토콜 서버로 NeoGraph Python 바인딩과 Python SDK를 비교합니다. 시간에는 클라이언트 작업, 서버 스케줄링, HTTP 교환이 포함되며 서버 비용이 일정하다고 입증하지 않습니다. 실제 모델은 실행하지 않습니다. 표는 x86_64 Ubuntu 24.04(WSL2), Python 3.12.3에서 2026-04-29 측정한 과거 기록이며 새 SDK cutover 결과가 아닙니다.

## 과거 순차 오버헤드: K=1

`bench_a2a_clients.py`의 로컬 고정 A2A 응답 기록이며 중앙값 비율은 1.93×였습니다.

| Client (2026-04-29) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | 1,137 µs | 1,381 µs | 860 req/s |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

`bench_openai_clients.py`의 로컬 고정 Chat 응답 기록이며 중앙값 비율은 1.54×였습니다. 과거 공급자 이름은 당시 구현 식별용이며 현재 import가 아닙니다.

| Client (2026-04-29, legacy provider) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` (removed) | 1,252 µs | 1,423 µs | 789 req/s |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

## 과거 동시 처리량

`bench_concurrent.py`의 로컬 A2A 서버, K ∈ {1, 4, 16, 64}, 행마다 500 요청 기록입니다.

| K (2026-04-29, A2A) | NeoGraph req/s | a2a-sdk req/s | Ratio |
|----:|---------------:|--------------:|--------:|
| 1 | 881 | 448 | 1.97× |
| 4 | 1,461 | 446 | 3.28× |
| 16 | 403 | 390 | 1.03× |
| 64 | 343 | 275 | 1.25× |

K=16/64 감소에는 `ThreadingHTTPServer`와 클라이언트의 상호작용이 포함됩니다. 표준 라이브러리 한계나 보편적인 asyncio 제약을 분리하지 않습니다. K=4도 선형 확장을 입증하지 않고 한 설정의 관측만 보고합니다.

## 현재 API와 의존성

현재 OpenAI 비교는 승인된 HTTP Chat descriptor와 런타임 옵션으로 `SchemaProvider`를 구성하고 `make_provider_request`, `invoke`를 사용합니다. 진짜 typed 소유 outcome을 받아 실패/완료와 표시 텍스트를 확인합니다. `OpenAIProvider`, `CompletionParams`, `complete()`는 제거된 API입니다. 모델은 요청에 명시하고 controls는 지정하지 않으면 typed factory 기본값을 씁니다. 과거 공급자 생성자 기본값이 아닙니다.

양쪽은 같은 비공개 HTTP loopback 서버와 Chat 경로를 사용하며 고정 응답 텍스트는 `ok`입니다. 서버가 경로, 모델, 프롬프트를 확인합니다. 원격 자격 증명, 사용자 CA, 유료 호출은 필요 없습니다. HTTP/1.0 로컬 작업이며 TLS/HTTP2 검증이나 native replay 벤치마크가 아닙니다. 고정 토큰 usage는 공급자 보고 fixture 데이터이지 실제 측정 토큰이나 예산 부과량이 아닙니다.

[Python 바인딩 안내](../../docs/python-binding.md)로 현재 소스에 맞는 wheel을 설치하고 아래 비교 SDK를 설치하세요. Core 소스 빌드에도 외부 `SchemaProvider::runtime`이 필요합니다. [빌드 안내](../../README.md)를 따르세요. Python 래퍼만으로 과거 wheel에 새 native API를 추가할 수 없습니다. 새 wheel 검증이나 벤치마크 통과를 주장하지 않습니다.
NeoGraph `0.13.1` wheel과 native 소비자는 alpha SDK `0.1.1`, interface revision/shared generation 4와 일치해야 합니다. 현재 통합 검증은 대기 중입니다.

## 새 측정 실행

저장소 루트에서 실행하세요. wheel/소스/SDK 리비전, Python 빌드와 GIL 모드, 비교 패키지 버전, 플랫폼, 서버 프로토콜, 예열, 반복 수, 실패를 기록하세요. 기본 요청 수는 500이고 측정 전에 예열합니다. 동시 실행자는 K=1/4/16/64를 순회합니다.

```bash
# First install a wheel matching this source via docs/python-binding.md.
python -m pip install a2a-sdk openai httpx
python benchmarks/python_clients/bench_a2a_clients.py 500
python benchmarks/python_clients/bench_openai_clients.py 500
python benchmarks/python_clients/bench_concurrent.py 500
```

과거 표와 새 출력을 구분하세요. 레이어별 비율을 곱해서 OpenAI-inside-A2A 종단 간 가속으로 계산할 수 없습니다. 그 결합 작업은 측정하지 않았습니다. 현재 구현의 보편적인 2–3× 보장이나 ±5% 재현성을 주장하지 않습니다.
