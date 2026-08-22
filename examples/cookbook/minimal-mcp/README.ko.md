<!-- neograph-i18n: source=examples/cookbook/minimal-mcp/README.md locale=ko source_sha256=aabe3dcc4da8e46fba45ac72d14b3c3a206736c485bd63248eb40c1abf57404e -->
# 최소 MCP — fastmcp 없음, SDK 없음, API 키 없음

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 저장소의 다른 모든 MCP 예제(03/20/21/22)는 고정된 OpenRouter DeepSeek 모델을 사용하는 ReAct 루프 안에 MCP 클라이언트를 감싸며, 대부분의 MCP 튜토리얼은 서버 측에서 `pip install fastmcp` (~60개 패키지를 가져옴)을 가정합니다. 이는 유용한 사실을 숨깁니다:

> **NeoGraph의 내장 MCP 클라이언트는 피어 측에 아무것도 필요하지 않습니다.
> 와이어 프로토콜을 말하는 프로세스 — 그리고 그 자체 측에서는 아무것도 아닌
> **예외 `libneograph_mcp` (이미 바이너리에 있음).**

이 쿡북은 가능한 가장 작은 설정으로 이를 증명합니다:

- **서버**: [`min_stdio_server.py`](min_stdio_server.py) — 약 60줄 분량의 순수 표준 라이브러리 Python 스크립트입니다. `fastmcp` 없음, `mcp` SDK 없음, pip 설치 없음. stdin/stdout을 통해 줄바꿈으로 구분된 JSON-RPC를 사용하며 세 가지 도구(`get_current_time`, `calculate`, `get_weather`)를 노출합니다.
- **클라이언트**: [`client_harness.cpp`](client_harness.cpp) — 서버를 하위 프로세스로 생성하고, `initialize` → `tools/list` → `tools/call`를 실행하며, 결과를 출력합니다. **LLM 없음, API 키 없음.**

## 실행합니다.

빌드 디렉터리에서(`-DNEOGRAPH_BUILD_MCP=ON`로 빌드된 경우, 예제에서는 기본적으로 켜져 있음):

```bash
./cookbook_minimal_mcp python3 ../examples/cookbook/minimal-mcp/min_stdio_server.py
```

예상 출력:

```
[*] Spawning stdio MCP server: python3 .../min_stdio_server.py
[*] initialize OK
[*] tools/list -> 3 tools:
    - get_current_time: Get the current UTC date and time (ISO format).
    - calculate: Evaluate a simple math expression (+ - * / ** % and parens).
    - get_weather: Return deterministic demo weather for a city.

[*] tools/call round-trips:
    get_current_time({"timezone":"UTC"}) -> 2026-05-31 12:00:00 (UTC)
    calculate({"expression":"2 ** 16 + 1"}) -> 65537
    get_weather({"city":"Tokyo"}) -> Tokyo: 22C, clear (demo)

[*] 3/3 MCP tool calls succeeded (no LLM, no fastmcp)
```

The `65537`는 호출이 실제로 서버에 도달하여 거기서 평가되었음을 증명합니다 — 이는 미리 만들어진 문자열이 아닙니다.

## 이 중요한 이유

- **가볍고, 양쪽 모두.** "배터리 포함(batteries included)" 주장은 사실입니다: NeoGraph는 MCP를 정적으로 링크하므로 별도의 패키지를 설치할 필요도 없고 드리프트할 수 있는 의존성도 없습니다. *피어* 서버는 stdlib이 허용하는 수준만큼 작을 수 있습니다. — edge devices, CI 환경, 또는 프레임워크 없이 로컬 도구 몇 개를 공개할 때 유용합니다.
- **Peer-agnostic.** `min_stdio_server.py`를 MCP over stdio를 사용하는 임의의 executable(Go binary, Rust server, fastmcp, official SDK)로 교체하십시오. C++ 쪽은 변경되지 않습니다.
- **키 없는 프로토콜 테스트.** 루프에 LLM이 없기 때문에 MCP 서버의 `tools/list` 및 `tools/call` 형태가 올바른지 에이전트에 연결하기 전에 스모크 테스트하는 가장 빠른 방법이기도 합니다.

## 에이전트에 통합하기

라운드 트립이 작동하면 `client.get_tools()`를 그래프 노드에 전달합니다(도구는 일반 `neograph::Tool` 인스턴스임) — LLM이 ReAct 루프를 통해 이를 호출할 수 있도록 하려면 해당 단계는 [`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp)를 참조하세요.
