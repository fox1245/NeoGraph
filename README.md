# NexaGraph

C++ 기반 고성능 동적 에이전트 오케스트레이션 엔진.  
LangGraph의 Channel/Reducer/Super-step/Checkpoint 추상화를 C++17 + Taskflow로 구현.

## 핵심 특징

- **JSON 기반 그래프 정의** — 재컴파일 없이 에이전트 워크플로 변경
- **멀티 LLM 지원** — OpenAI, Claude, Gemini (SchemaProvider)
- **Taskflow 병렬 실행** — work-stealing 스케줄러로 fan-out/fan-in
- **체크포인팅 + HITL** — interrupt/resume, 시간여행 디버깅
- **계층적 서브그래프** — JSON 재귀 컴파일로 에이전트 안의 에이전트
- **의도 기반 라우팅** — LLM 분류 → 전문가 서브그래프 자동 분기

## 아키텍처

```
JSON 정의 → GraphEngine::compile()
  → NodeFactory (llm_call, tool_dispatch, subgraph, intent_classifier)
  → ReducerRegistry (overwrite, append)
  → ConditionRegistry (has_tool_calls, route_channel)

GraphEngine::run()
  → Super-step 루프 (Pregel BSP)
  → 단일 노드: 직접 호출 | 복수 노드: Taskflow 병렬
  → 체크포인트 자동 저장 (InMemory / gRPC+PostgreSQL)
  → interrupt_before/after → resume()
```

## 빌드

```bash
# 로컬 빌드 (gRPC 제외)
mkdir build && cd build
cmake .. && make -j$(nproc)

# 테스트
./graph_test

# 테스트 서버
OPENAI_API_KEY=... ./graph_server
```

## 테스트 서버 엔드포인트

```bash
# ReAct 에이전트 (계산기 도구)
curl -s http://localhost:9090/react \
  -H "Content-Type: application/json" \
  -d '{"message": "What is 123 * 456?"}'

# 의도 분류 → 전문가 라우팅
curl -s http://localhost:9090/expert \
  -H "Content-Type: application/json" \
  -d '{"message": "Translate hello to Korean"}'
```

## JSON 그래프 정의 예시

```json
{
  "name": "expert_panel",
  "channels": {
    "messages": {"reducer": "append"},
    "__route__": {"reducer": "overwrite"}
  },
  "nodes": {
    "classifier": {
      "type": "intent_classifier",
      "routes": ["math", "translation", "general"]
    },
    "math_agent": {
      "type": "subgraph",
      "definition": {
        "channels": {"messages": {"reducer": "append"}},
        "nodes": {
          "llm": {"type": "llm_call"},
          "tools": {"type": "tool_dispatch"}
        },
        "edges": [
          {"from": "__start__", "to": "llm"},
          {"from": "llm", "type": "conditional",
           "condition": "has_tool_calls",
           "routes": {"true": "tools", "false": "__end__"}},
          {"from": "tools", "to": "llm"}
        ]
      }
    }
  },
  "edges": [
    {"from": "__start__", "to": "classifier"},
    {"from": "classifier", "type": "conditional",
     "condition": "route_channel",
     "routes": {"math": "math_agent", ...}},
    {"from": "math_agent", "to": "__end__"}
  ]
}
```

## 프로젝트 구조

```
src/any_llm/
  provider.h/cpp           — LLM Provider 추상화 (OpenAI)
  schema_provider.h/cpp    — 멀티 LLM (Claude, Gemini)
  agent.h/cpp              — ReAct Agent (레거시)
  mcp_client.h/cpp         — MCP 프로토콜 클라이언트
  grpc_client.h/cpp        — gRPC RAG 클라이언트
  graph/
    graph_types.h          — Channel, Edge, NodeContext, GraphEvent
    graph_state.h/cpp      — 채널 상태 관리 (shared_mutex)
    graph_node.h/cpp       — LLMCallNode, ToolDispatchNode,
                             IntentClassifierNode, SubgraphNode
    graph_engine.h/cpp     — Super-step 루프 + Taskflow 병렬
    graph_loader.h/cpp     — JSON 파서, 레지스트리 (싱글턴)
    graph_checkpoint.h/cpp — Checkpoint + InMemoryStore
    grpc_checkpoint.h/cpp  — PostgreSQL 영속 저장
    react_graph.h/cpp      — create_react_graph() 편의 함수
```

## 기술 스택

| 구성 | 기술 |
|---|---|
| 언어 | C++17 |
| 병렬 | Taskflow 3.7 (work-stealing) |
| JSON | nlohmann/json |
| HTTP | cpp-httplib |
| 직렬화 | Protocol Buffers / gRPC |
| DB | PostgreSQL (pgvector) |
| 프론트엔드 | React |

## 라이선스

Private
