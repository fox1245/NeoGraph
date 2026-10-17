<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=ko source_sha256=883ad166c7992f45e4e72718ccfd2d561be4f613dae951c414b3be91f5cd904f -->
# Python 클라이언트 오버헤드: 현재 러너와 과거 결과

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 디렉터리는 NeoGraph Python 바인딩과 Python SDK 클라이언트를, 로컬 프로세스 안에서 동작하는 프로토콜 서버(peer)와 맞붙여 비교합니다. 측정한 실제 경과 시간에는 클라이언트 작업, 서버 스케줄링, HTTP 교환이 모두 들어 있으며, 서버 비용이 일정하다는 것은 입증되지 않았습니다. 여기서는 실제 모델을 실행하지 않습니다. 표는 x86_64 Ubuntu 24.04(WSL2), Python 3.12.3에서 2026-04-29에 측정한 값을 그대로 보존한 것이며, 업데이트된 SDK 전환 이후의 측정값이 아닙니다.

## 과거 순차 오버헤드: K=1

`bench_a2a_clients.py`로 로컬의 미리 준비한(canned) A2A 응답을 사용했고, 기록된 중앙값 비율은 1.93×였습니다.

| 클라이언트 (2026-04-29) | 중앙값 | P95 | 처리량 |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | 1,137 µs | 1,381 µs | 860 req/s |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

`bench_openai_clients.py`로 로컬의 canned Chat 응답을 사용했고, 기록된 중앙값 비율은 1.54×였습니다. 표에 남아 있는 예전 NeoGraph provider 이름은 당시 구현을 가리키기 위한 표기이며, 현재 쓸 수 있는 import가 아닙니다.

| 클라이언트 (2026-04-29, 구 provider) | 중앙값 | P95 | 처리량 |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` (제거됨) | 1,252 µs | 1,423 µs | 789 req/s |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

## 과거 동시 처리량

`bench_concurrent.py`로 로컬 A2A 서버를 사용했고, K ∈ {1, 4, 16, 64}, 행마다 요청 500건입니다.

| K (2026-04-29, A2A) | NeoGraph req/s | a2a-sdk req/s | 비율 |
|----:|---------------:|--------------:|--------:|
| 1 | 881 | 448 | 1.97× |
| 4 | 1,461 | 446 | 3.28× |
| 16 | 403 | 390 | 1.03× |
| 64 | 343 | 275 | 1.25× |

K=16/64에서 처리량이 떨어진 데에는 `ThreadingHTTPServer`와 클라이언트 사이의 상호작용이 섞여 있습니다. 표준 라이브러리의 한계를 따로 가려낸 것도, asyncio에 일반적인 제약이 있음을 증명한 것도 아닙니다. K=4의 결과 역시 거의 선형에 가까운 확장을 입증하지 않습니다. 표본 하나로 측정한 설정의 결과일 뿐입니다.

## 현재 API와 의존성

현재의 OpenAI 비교는 승인(admission)된 HTTP Chat descriptor와 런타임 옵션으로 `SchemaProvider`를 만든 다음 `make_provider_request`와 `invoke`를 사용합니다. 실제로 반환된 typed 소유 outcome을 받아서, 실패/완료 여부와 화면에 보이는 텍스트를 확인합니다. `OpenAIProvider`, `CompletionParams`, `complete()`는 제거된 API입니다. 모델은 요청에 명시하며, 컨트롤(controls)은 따로 지정하지 않으면 타입이 있는 팩토리 기본값을 씁니다. 예전 provider 생성자의 기본값이 아닙니다.

두 클라이언트는 같은 비공개 HTTP 루프백 서버와 Chat 경로를 쓰며, 응답은 텍스트가 `ok`인 canned 응답입니다. 서버는 경로, 모델, 프롬프트를 확인합니다. 호스팅 서비스의 자격 증명, 사용자 지정 CA, 유료 호출은 필요하지 않습니다. 이 벤치마크는 HTTP/1.0으로 로컬에서 프로토콜 작업을 돌리는 것일 뿐, TLS/HTTP2 전송의 검증이나 네이티브 재생(replay) 벤치마크가 아닙니다. canned 응답에 들어 있는 토큰 사용량은 provider가 보고한 것처럼 만든 픽스처 데이터이며, 측정한 토큰 수도 예산 청구액도 아닙니다.

[Python 바인딩 가이드](../../docs/python-binding.md)에 따라 현재 소스와 맞는 wheel을 설치한 뒤, 아래의 비교용 SDK를 설치합니다. 소스에서 빌드할 때는 Core만 빌드하더라도 외부 `SchemaProvider::runtime`이 필요하므로 [빌드 가이드](../../README.md)를 따릅니다. Python 래퍼만으로는 오래된 wheel에 타입이 있는 네이티브 API를 추가할 수 없습니다.
NeoGraph `0.14.2`의 wheel과 네이티브 소비자는 알파 SDK `0.3.3`, 인터페이스 리비전/공유 라이브러리 세대 6과 일치해야 합니다.

## 새 측정 실행하기

저장소 루트에서 실행합니다. wheel/소스/SDK 리비전, Python 빌드(GIL 모드 포함), 비교용 패키지 버전, 플랫폼, 서버 프로토콜, 워밍업, 반복 횟수, 실패 내역을 기록해 둡니다. 러너의 기본 요청 수는 500건이고 측정 전에 워밍업을 합니다. 동시성 러너는 K=1/4/16/64를 차례로 돌려 봅니다.

```bash
# First install a wheel matching this source via docs/python-binding.md.
python -m pip install a2a-sdk openai httpx
python benchmarks/python_clients/bench_a2a_clients.py 500
python benchmarks/python_clients/bench_openai_clients.py 500
python benchmarks/python_clients/bench_concurrent.py 500
```

예전 표는 새 출력과 따로 보관합니다. 계층별 비율을 곱해서 OpenAI-inside-A2A의 종단 간 속도 향상을 구할 수는 없습니다. 그렇게 결합한 작업은 여기서 측정하지 않았습니다. 현재 구현에 대해 클라이언트가 2–3배 빠르다는 일반적인 보장이나 ±5% 재현성을 주장하지도 않습니다.
