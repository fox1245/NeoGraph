<!-- neograph-i18n: source=examples/cookbook/minimal-mcp/README.md locale=ko source_sha256=018efba21b0004352a4b23c8947e0d18299157eb31070d941304799863f60d82 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# 최소 MCP — fastmcp 없음, SDK 없음, API 키 없음


이 저장소(03/20/21/22)의 다른 모든 MCP 예제는 MCP를 래핑합니다.
`OPENAI_API_KEY` 및 대부분의 MCP가 필요한 ReAct 루프 내부의 클라이언트
튜토리얼에서는 `pip install fastmcp`(~60개의 패키지를 가져옴)를 가정합니다.
서버 측에서. 여기에는 유용한 사실이 숨겨져 있습니다.

> **NeoGraph의 내장 MCP 클라이언트는 피어 측에서 다음을 제외하고는 아무것도 필요하지 않습니다.
> 유선 프로토콜을 말하는 프로세스 - 그 자체로는 아무것도 없습니다.
> `libneograph_mcp`는 제외합니다(이미 바이너리에 있음).**

이 요리책은 가능한 가장 작은 설정으로 이를 증명합니다.

- **서버**: [`min_stdio_server.py`](min_stdio_server.py) — ~60라인
순수 표준 라이브러리 Python 스크립트. `fastmcp` 없음, `mcp` SDK 없음, pip 설치 없음.
stdin/stdout를 통해 개행으로 구분된 JSON-RPC를 말하고 노출합니다.
세 가지 도구(`get_current_time`, `calculate`, `get_weather`).
- **클라이언트**: [`client_harness.cpp`](client_harness.cpp) —
서버를 하위 프로세스로 실행하고 `initialize` → `tools/list` →
`tools/call`를 실행하고 결과를 인쇄합니다. **LLM 없음, API 키 없음.**

## 실행해 보세요

빌드 디렉터리(`-DNEOGRAPH_BUILD_MCP=ON`로 빌드됨)에서
예를 들어 기본적으로 켜져 있음):

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

`65537`는 호출이 실제로 서버에 도달하여 평가되었음을 증명합니다.
거기 — 그것은 미리 준비된 문자열이 아닙니다.

## 이것이 중요한 이유

- **양쪽 모두 가볍습니다.** "배터리 포함"이라는 주장은 사실입니다.
NeoGraph는 MCP를 정적으로 연결하므로 별도의 패키지가 없습니다.
설치하고 드리프트할 수 있는 종속성이 없습니다. *피어* 서버는 다음과 같습니다.
stdlib가 허용하는 만큼 작음 — 에지 장치, CI 또는 다음과 같은 경우에 유용합니다.
프레임워크 없이 몇 가지 로컬 도구를 노출하고 싶을 뿐입니다.
- **피어에 구애받지 않습니다.** `min_stdio_server.py`를 실행 파일로 바꾸세요.
stdio(Go 바이너리, Rust 서버, fastmcp,
공식 SDK). C++ 쪽은 절대 변하지 않습니다.
- **키 없는 프로토콜 테스트.** 루프에 LLM가 없기 때문에 이
또한 MCP 서버의 연기 테스트를 수행하는 가장 빠른 방법이기도 합니다.
배선하기 전에 `tools/list` 및 `tools/call` 모양이 정확합니다.
대리인으로.

## 에이전트에 연결

왕복이 작동하면 `client.get_tools()`를 그래프 노드에 전달합니다.
(도구는 일반 `neograph::Tool` 인스턴스입니다.) LLM가 호출할 수 있습니다.
ReAct 루프를 통해 — [`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp) 참조
그 단계를 위해.
