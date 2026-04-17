# NeoGraph API Reference (한국어)

> NeoGraph -- C++ 그래프 에이전트 엔진 라이브러리
>
> 버전: 1.0 | 네임스페이스: `neograph`, `neograph::graph`, `neograph::llm`, `neograph::mcp`, `neograph::util`

---

## 목차

1. [기본 타입 (`neograph/types.h`)](#1-기본-타입)
2. [Provider 인터페이스 (`neograph/provider.h`)](#2-provider-인터페이스)
3. [Tool 인터페이스 (`neograph/tool.h`)](#3-tool-인터페이스)
4. [그래프 타입 (`neograph/graph/types.h`)](#4-그래프-타입)
5. [GraphState (`neograph/graph/state.h`)](#5-graphstate)
6. [GraphNode (`neograph/graph/node.h`)](#6-graphnode)
7. [GraphEngine (`neograph/graph/engine.h`)](#7-graphengine)
8. [체크포인트 (`neograph/graph/checkpoint.h`)](#8-체크포인트)
9. [Store (`neograph/graph/store.h`)](#9-store)
10. [로더 / 레지스트리 (`neograph/graph/loader.h`)](#10-로더--레지스트리)
11. [ReAct 그래프 (`neograph/graph/react_graph.h`)](#11-react-그래프)
12. [LLM 모듈 (`neograph/llm/`)](#12-llm-모듈)
13. [MCP 모듈 (`neograph/mcp/`)](#13-mcp-모듈)
14. [유틸리티 (`neograph/util/`)](#14-유틸리티)

---

## 빠른 시작

NeoGraph의 모든 핵심 API는 단일 헤더로 포함할 수 있다.

```cpp
#include <neograph/neograph.h>   // 핵심 타입 + 그래프 엔진 전체
```

LLM provider, MCP, 유틸리티는 별도 헤더를 포함한다.

```cpp
#include <neograph/llm/openai_provider.h>  // OpenAI 호환 provider
#include <neograph/llm/schema_provider.h>  // 멀티 벤더 provider
#include <neograph/llm/agent.h>            // ReAct 에이전트 루프
#include <neograph/mcp/client.h>           // MCP 클라이언트
#include <neograph/util/request_queue.h>   // lock-free 워커풀
```

---

## 1. 기본 타입

**헤더:** `neograph/types.h`
**네임스페이스:** `neograph`

LLM API 통신에 필요한 기초 데이터 구조체와 JSON 직렬화 헬퍼를 정의한다.

### 1.1 ToolCall

LLM이 요청한 도구 호출 하나를 나타낸다.

```cpp
struct ToolCall {
    std::string id;         // 호출 고유 식별자 (예: "call_abc123")
    std::string name;       // 도구 이름 (예: "calculator")
    std::string arguments;  // JSON 문자열 형태의 인자
};
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `id` | `std::string` | provider가 부여한 호출 ID. tool 결과를 매칭할 때 사용 |
| `name` | `std::string` | 호출할 도구의 이름 |
| `arguments` | `std::string` | JSON 문자열. 실행 시 `json::parse()`로 파싱 |

ADL 기반 JSON 직렬화(`to_json` / `from_json`)가 제공된다.

---

### 1.2 ChatMessage

하나의 대화 메시지를 표현한다. 역할(role)에 따라 사용되는 필드가 달라진다.

```cpp
struct ChatMessage {
    std::string role;                    // "system" | "user" | "assistant" | "tool"
    std::string content;                 // 텍스트 내용
    std::vector<ToolCall> tool_calls;    // assistant 메시지의 도구 호출 목록
    std::string tool_call_id;           // tool 결과 메시지의 호출 ID
    std::string tool_name;              // tool 결과 메시지의 도구 이름
    std::vector<std::string> image_urls; // Vision용 이미지 (base64 data URL 또는 HTTP URL)
};
```

| role | 주로 쓰는 필드 |
|------|---------------|
| `"system"` | `content` |
| `"user"` | `content`, `image_urls` (Vision) |
| `"assistant"` | `content`, `tool_calls` |
| `"tool"` | `content`, `tool_call_id`, `tool_name` |

---

### 1.3 ChatTool

LLM에 전달할 도구 정의.

```cpp
struct ChatTool {
    std::string name;        // 도구 이름
    std::string description; // 도구 설명 (LLM이 참고)
    json parameters;         // JSON Schema 객체 (인자 스키마)
};
```

**사용 예:**

```cpp
neograph::ChatTool weather_tool{
    "get_weather",
    "지정한 도시의 현재 날씨를 조회합니다.",
    {{"type", "object"},
     {"properties", {
         {"city", {{"type", "string"}, {"description", "도시 이름"}}}
     }},
     {"required", {"city"}}}
};
```

---

### 1.4 ChatCompletion

LLM 호출 결과. 응답 메시지와 토큰 사용량을 포함한다.

```cpp
struct ChatCompletion {
    ChatMessage message;          // 응답 메시지
    struct Usage {
        int prompt_tokens = 0;    // 입력 토큰 수
        int completion_tokens = 0; // 출력 토큰 수
        int total_tokens = 0;      // 합계
    } usage;
};
```

---

### 1.5 헬퍼 함수

#### `messages_to_json()`

`ChatMessage` 벡터를 OpenAI API 형식 JSON 배열로 변환한다.
tool call, tool result, Vision(multi-modal) 메시지를 자동으로 올바른 형식으로 직렬화한다.

```cpp
json messages_to_json(const std::vector<ChatMessage>& messages);
```

**반환값:** OpenAI Chat API `messages` 배열 형식의 `json`.

#### `tools_to_json()`

`ChatTool` 벡터를 OpenAI API `tools` 배열 형식으로 변환한다.

```cpp
json tools_to_json(const std::vector<ChatTool>& tools);
```

**반환값:** `[{"type":"function","function":{...}}, ...]` 형식의 `json`.

#### `parse_response_message()`

OpenAI API 응답의 `choices[i]` 객체에서 `ChatMessage`를 추출한다.

```cpp
ChatMessage parse_response_message(const json& choice);
```

| 매개변수 | 설명 |
|---------|------|
| `choice` | `choices` 배열의 원소. `message` 키를 포함해야 함 |

**반환값:** 파싱된 `ChatMessage`. tool_calls가 있으면 함께 추출된다.

---

## 2. Provider 인터페이스

**헤더:** `neograph/provider.h`
**네임스페이스:** `neograph`

LLM provider의 추상 인터페이스를 정의한다. OpenAI, Claude, Gemini 등 모든 provider가 이 인터페이스를 구현한다.

### 2.1 StreamCallback

스트리밍 응답에서 토큰 단위로 호출되는 콜백.

```cpp
using StreamCallback = std::function<void(const std::string& chunk)>;
```

---

### 2.2 CompletionParams

LLM 호출 매개변수.

```cpp
struct CompletionParams {
    std::string model;                  // 모델 이름 (예: "gpt-4o-mini")
    std::vector<ChatMessage> messages;  // 대화 기록
    std::vector<ChatTool> tools;        // 사용 가능한 도구 목록
    float temperature = 0.7f;           // 샘플링 온도
    int max_tokens = -1;                // 최대 출력 토큰 (-1이면 provider 기본값)
};
```

---

### 2.3 Provider (추상 클래스)

```cpp
class Provider {
public:
    virtual ~Provider() = default;

    // 동기 호출
    virtual ChatCompletion complete(const CompletionParams& params) = 0;

    // 스트리밍 호출: 토큰마다 on_chunk를 호출하고, 완료 시 전체 결과를 반환
    virtual ChatCompletion complete_stream(const CompletionParams& params,
                                           const StreamCallback& on_chunk) = 0;

    // provider 이름 (예: "openai", "claude", "gemini")
    virtual std::string get_name() const = 0;
};
```

| 메서드 | 설명 |
|--------|------|
| `complete()` | 블로킹 호출. 전체 응답이 생성된 후 반환 |
| `complete_stream()` | 토큰이 생성될 때마다 `on_chunk`를 호출. 최종 `ChatCompletion`도 반환 |
| `get_name()` | provider 식별 문자열 |

**커스텀 provider 구현 예:**

```cpp
class MyProvider : public neograph::Provider {
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams& params) override {
        // HTTP 호출 등 구현
        neograph::ChatCompletion result;
        result.message.role = "assistant";
        result.message.content = "응답 내용";
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& params,
        const neograph::StreamCallback& on_chunk) override {
        auto result = complete(params);
        if (on_chunk) on_chunk(result.message.content);
        return result;
    }

    std::string get_name() const override { return "my_provider"; }
};
```

---

## 3. Tool 인터페이스

**헤더:** `neograph/tool.h`
**네임스페이스:** `neograph`

LLM이 호출할 수 있는 도구의 추상 인터페이스.

```cpp
class Tool {
public:
    virtual ~Tool() = default;

    // LLM에 전달할 도구 정의 (이름, 설명, 파라미터 스키마)
    virtual ChatTool get_definition() const = 0;

    // 도구 실행. JSON 인자를 받아 문자열 결과를 반환
    virtual std::string execute(const json& arguments) = 0;

    // 도구 이름
    virtual std::string get_name() const = 0;
};
```

**구현 예:**

```cpp
class CalculatorTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {
            "calculator",
            "수학 표현식을 계산합니다.",
            {{"type", "object"},
             {"properties", {
                 {"expression", {{"type", "string"}, {"description", "계산식"}}}
             }},
             {"required", {"expression"}}}
        };
    }

    std::string execute(const neograph::json& args) override {
        std::string expr = args.value("expression", "");
        // 계산 로직 ...
        return R"({"result": 42})";
    }

    std::string get_name() const override { return "calculator"; }
};
```

---

## 4. 그래프 타입

**헤더:** `neograph/graph/types.h`
**네임스페이스:** `neograph::graph`

그래프 엔진의 핵심 타입들을 정의한다.

### 4.1 ReducerType

채널에 값을 기록할 때 기존 값과 새 값을 합치는 전략.

```cpp
enum class ReducerType {
    OVERWRITE,  // 새 값으로 덮어쓰기 (기본값)
    APPEND,     // 기존 배열에 추가 (배열 채널용)
    CUSTOM      // 사용자 정의 리듀서 함수
};
```

#### ReducerFn

`CUSTOM` 리듀서의 함수 시그니처.

```cpp
using ReducerFn = std::function<json(const json& current, const json& incoming)>;
```

---

### 4.2 Channel

상태 그래프의 단일 채널. 이름, 리듀서 타입, 현재 값, 버전을 보유한다.

```cpp
struct Channel {
    std::string name;
    ReducerType reducer_type = ReducerType::OVERWRITE;
    ReducerFn   reducer;       // CUSTOM일 때만 사용
    json        value;         // 현재 값
    uint64_t    version = 0;   // 쓰기마다 증가
};
```

---

### 4.3 ChannelWrite

노드가 출력하는 채널 쓰기 하나.

```cpp
struct ChannelWrite {
    std::string channel;  // 대상 채널 이름
    json        value;    // 기록할 값
};
```

---

### 4.4 NodeInterrupt

노드 실행 중 동적으로 breakpoint를 발생시키는 예외.
HITL(Human-in-the-Loop) 워크플로에서 실행을 중단하고 사용자 입력을 기다릴 때 사용한다.

```cpp
class NodeInterrupt : public std::runtime_error {
public:
    explicit NodeInterrupt(const std::string& reason);
    const std::string& reason() const;
};
```

**사용 예:**

```cpp
std::vector<ChannelWrite> execute(const GraphState& state) override {
    auto amount = state.get("payment_amount");
    if (amount.get<int>() > 1000000) {
        throw NodeInterrupt("고액 결제 승인 필요: " + amount.dump());
    }
    // 정상 처리 계속...
}
```

`NodeInterrupt`가 발생하면 엔진이 checkpoint를 저장하고 `RunResult::interrupted = true`를 반환한다.
이후 `GraphEngine::resume()`으로 실행을 재개할 수 있다.

---

### 4.5 Send

동적 fan-out 요청. 하나의 노드가 여러 인스턴스로 분기 실행을 요청할 때 사용한다.
map-reduce 패턴의 "map" 단계에 해당한다.

```cpp
struct Send {
    std::string target_node;  // 실행할 대상 노드 이름
    json        input;        // 해당 실행에 주입할 채널 값 (채널명 -> 값)
};
```

`Send`는 `NodeResult::sends`에 담아 반환한다. 엔진이 각 Send에 대해
대상 노드를 개별적으로 실행하고, 결과를 리듀서를 통해 합산한다.

**사용 예:**

```cpp
NodeResult execute_full(const GraphState& state) override {
    NodeResult nr;
    std::vector<std::string> topics = {"AI", "반도체", "양자컴퓨팅"};

    for (const auto& topic : topics) {
        nr.sends.push_back(Send{
            "researcher",                         // 실행할 노드
            json({{"topic", topic}})              // 각 실행마다 다른 입력
        });
    }
    return nr;
}
```

---

### 4.6 Command

라우팅 오버라이드 + 상태 업데이트를 동시에 수행하는 지시.
기본 edge 라우팅을 무시하고 특정 노드로 직접 이동할 때 사용한다.

```cpp
struct Command {
    std::string                goto_node;  // 다음에 실행할 노드 (edge 라우팅 무시)
    std::vector<ChannelWrite>  updates;    // 라우팅 전에 적용할 상태 업데이트
};
```

`Command`는 `NodeResult::command`에 담아 반환한다.

**사용 예:**

```cpp
NodeResult execute_full(const GraphState& state) override {
    auto score = state.get("score").get<double>();
    NodeResult nr;

    if (score > 0.8) {
        nr.command = Command{
            "summarizer",   // 높은 점수 -> 요약 노드로 직행
            {ChannelWrite{"status", json("approved")}}
        };
    } else {
        nr.command = Command{
            "researcher",   // 낮은 점수 -> 추가 조사
            {ChannelWrite{"status", json("needs_more_research")}}
        };
    }
    return nr;
}
```

---

### 4.7 RetryPolicy

노드 실행 실패 시 재시도 정책.

```cpp
struct RetryPolicy {
    int    max_retries        = 0;      // 최대 재시도 횟수 (0 = 재시도 없음)
    int    initial_delay_ms   = 100;    // 첫 번째 재시도 대기 시간 (ms)
    float  backoff_multiplier = 2.0f;   // 지수 백오프 배수
    int    max_delay_ms       = 5000;   // 최대 대기 시간 상한 (ms)
};
```

재시도 간 대기 시간은 `initial_delay_ms * backoff_multiplier^(attempt-1)` 으로 계산되며,
`max_delay_ms`를 초과하지 않는다.

---

### 4.8 StreamMode

스트리밍 이벤트 필터링을 위한 비트플래그.

```cpp
enum class StreamMode : uint8_t {
    EVENTS   = 0x01,   // NODE_START, NODE_END, INTERRUPT, ERROR
    TOKENS   = 0x02,   // LLM_TOKEN
    VALUES   = 0x04,   // 각 단계 후 전체 상태
    UPDATES  = 0x08,   // 노드별 채널 쓰기 (delta)
    DEBUG    = 0x10,    // 내부 디버그 정보 (재시도, 라우팅 결정 등)
    ALL      = 0xFF    // 모든 이벤트
};
```

비트 OR로 조합 가능:

```cpp
config.stream_mode = StreamMode::EVENTS | StreamMode::TOKENS;
```

**보조 함수:**

```cpp
StreamMode operator|(StreamMode a, StreamMode b);
StreamMode operator&(StreamMode a, StreamMode b);
bool has_mode(StreamMode flags, StreamMode test);
```

---

### 4.9 Edge / ConditionalEdge

그래프의 노드 간 연결을 정의한다.

#### Edge (무조건 연결)

```cpp
struct Edge {
    std::string from;  // 출발 노드
    std::string to;    // 도착 노드
};
```

#### ConditionalEdge (조건부 연결)

```cpp
struct ConditionalEdge {
    std::string from;                                // 출발 노드
    std::string condition;                           // ConditionRegistry에 등록된 조건 이름
    std::map<std::string, std::string> routes;       // 조건 결과 -> 도착 노드
};
```

**JSON 정의 예:**

```json
{
  "from": "llm",
  "condition": "has_tool_calls",
  "routes": { "true": "tools", "false": "__end__" }
}
```

---

### 4.10 NodeContext

노드 생성 시 주입되는 의존성 컨텍스트.

```cpp
struct NodeContext {
    std::shared_ptr<Provider> provider;        // LLM provider
    std::vector<Tool*>        tools;           // 도구 목록 (비소유 포인터)
    std::string               model;           // 모델 이름
    std::string               instructions;    // 시스템 프롬프트
    json                      extra_config;    // 추가 설정
};
```

`tools`는 비소유(non-owning) 포인터다.
도구의 수명은 `GraphEngine::own_tools()` 또는 호출자가 관리해야 한다.

---

### 4.11 GraphEvent

스트리밍 실행 중 발생하는 이벤트.

```cpp
struct GraphEvent {
    enum class Type {
        NODE_START,      // 노드 실행 시작
        NODE_END,        // 노드 실행 완료
        LLM_TOKEN,       // LLM 스트리밍 토큰
        CHANNEL_WRITE,   // 채널 쓰기 발생
        INTERRUPT,       // HITL 인터럽트 발생
        ERROR            // 오류 발생
    };

    Type        type;        // 이벤트 종류
    std::string node_name;   // 관련 노드 이름
    json        data;        // 이벤트별 데이터
};
```

#### GraphStreamCallback

```cpp
using GraphStreamCallback = std::function<void(const GraphEvent&)>;
```

**특수 노드 이름:**

- `"__send__"` -- Send 실행 관련 이벤트. `data["sends"]`에 Send 목록 포함
- `"__routing__"` -- 라우팅 결정. `data["command_goto"]` 또는 `data["next_nodes"]` 포함

---

### 4.12 NodeResult

노드의 확장 실행 결과. 채널 쓰기 + Command + Send를 모두 포함할 수 있다.

```cpp
struct NodeResult {
    std::vector<ChannelWrite> writes;       // 채널 쓰기
    std::optional<Command>    command;      // 라우팅 오버라이드 (없으면 기본 edge 사용)
    std::vector<Send>         sends;        // 동적 fan-out 요청

    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w); // writes만으로 생성 (하위 호환)
};
```

---

### 4.13 ConditionFn / 특수 노드 이름

```cpp
using ConditionFn = std::function<std::string(const GraphState&)>;

constexpr const char* START_NODE = "__start__";  // 그래프 진입점
constexpr const char* END_NODE   = "__end__";    // 그래프 종료점
```

---

## 5. GraphState

**헤더:** `neograph/graph/state.h`
**네임스페이스:** `neograph::graph`

그래프 실행 중 공유되는 상태. 여러 채널로 구성되며, thread-safe한 읽기/쓰기를 지원한다.

```cpp
class GraphState {
public:
    // 채널 초기화 (리듀서 타입, 함수, 초기값 지정)
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json());

    // 채널 값 읽기 (shared lock -- 동시 읽기 가능)
    json get(const std::string& channel) const;

    // "messages" 채널을 vector<ChatMessage>로 읽기 (편의 메서드)
    std::vector<ChatMessage> get_messages() const;

    // 단일 채널 쓰기 (exclusive lock)
    void write(const std::string& channel, const json& value);

    // 여러 채널에 원자적(atomic) 쓰기
    void apply_writes(const std::vector<ChannelWrite>& writes);

    // 버전 조회
    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    // checkpoint를 위한 직렬화/복원
    json serialize() const;
    void restore(const json& data);

    // 채널 이름 목록
    std::vector<std::string> channel_names() const;
};
```

| 메서드 | 잠금 방식 | 설명 |
|--------|----------|------|
| `get()` | shared lock | 읽기 전용, 동시 접근 가능 |
| `get_messages()` | shared lock | `get("messages")`의 편의 래퍼 |
| `write()` | exclusive lock | 단일 채널에 리듀서를 적용하여 쓰기 |
| `apply_writes()` | exclusive lock | 여러 채널에 한 번에 쓰기 |
| `serialize()` | -- | 전체 상태를 JSON으로 직렬화 |
| `restore()` | -- | 직렬화된 JSON으로 상태 복원 |

**OVERWRITE vs APPEND 리듀서 동작:**

```cpp
// OVERWRITE 채널 "status": 마지막 값만 유지
state.write("status", json("running"));     // -> "running"
state.write("status", json("done"));        // -> "done"

// APPEND 채널 "messages": 배열에 요소 추가
state.write("messages", json::array({msg1})); // -> [msg1]
state.write("messages", json::array({msg2})); // -> [msg1, msg2]
```

---

## 6. GraphNode

**헤더:** `neograph/graph/node.h`
**네임스페이스:** `neograph::graph`

### 6.1 GraphNode (추상 클래스)

모든 그래프 노드의 기반 클래스.

```cpp
class GraphNode {
public:
    virtual ~GraphNode() = default;

    // 기본 실행: 상태를 읽고 채널 쓰기를 반환
    virtual std::vector<ChannelWrite> execute(const GraphState& state) = 0;

    // 스트리밍 실행 (기본: execute()에 위임)
    virtual std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    // 확장 실행: Send/Command를 포함한 NodeResult 반환
    // 기본 구현은 execute()를 호출하여 writes만 채운 NodeResult를 반환
    virtual NodeResult execute_full(const GraphState& state);

    // 확장 스트리밍 실행
    virtual NodeResult execute_full_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    // 노드 이름
    virtual std::string name() const = 0;
};
```

| 메서드 | 오버라이드 시점 |
|--------|---------------|
| `execute()` | 기본 노드. 채널 쓰기만 필요할 때 |
| `execute_full()` | Send 또는 Command를 사용해야 할 때 |
| `execute_stream()` | 스트리밍 이벤트를 발생시켜야 할 때 |
| `execute_full_stream()` | Send/Command + 스트리밍이 모두 필요할 때 |

엔진은 `execute_full()` (또는 스트리밍 시 `execute_full_stream()`)을 호출한다.
기본 구현이 `execute()`를 래핑하므로, 단순 노드는 `execute()`만 구현하면 된다.

---

### 6.2 LLMCallNode

LLM API를 호출하는 내장 노드. `messages` 채널에서 대화 기록을 읽어
LLM에 전달하고, 응답을 다시 `messages` 채널에 기록한다.

```cpp
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string name() const override;
};
```

- `NodeContext::provider`가 필수
- `NodeContext::tools`가 있으면 LLM에 도구 정의를 함께 전달
- `NodeContext::instructions`가 있으면 system 메시지로 추가

**JSON 정의에서의 타입 이름:** `"llm_call"`

---

### 6.3 ToolDispatchNode

LLM이 요청한 도구 호출을 실행하는 내장 노드.
`messages` 채널에서 마지막 assistant 메시지의 `tool_calls`를 읽어
각 도구를 실행하고, 결과를 `tool` role 메시지로 기록한다.

```cpp
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::string name() const override;
};
```

**JSON 정의에서의 타입 이름:** `"tool_dispatch"`

---

### 6.4 IntentClassifierNode

LLM을 사용하여 사용자 의도를 분류하는 노드.
분류 결과를 `__route__` 채널에 기록하고, `route_channel` 조건과 함께
조건부 라우팅에 사용한다.

```cpp
class IntentClassifierNode : public GraphNode {
public:
    IntentClassifierNode(const std::string& name,
                         const NodeContext& ctx,
                         const std::string& prompt,          // 분류 프롬프트
                         std::vector<std::string> valid_routes); // 유효한 경로 목록

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::string name() const override;
};
```

**JSON 정의에서의 타입 이름:** `"intent_classifier"`

**사용 예 (JSON 정의):**

```json
{
  "type": "intent_classifier",
  "config": {
    "prompt": "다음 문의를 분류하세요: billing, technical, general",
    "valid_routes": ["billing", "technical", "general"]
  }
}
```

---

### 6.5 SubgraphNode

컴파일된 `GraphEngine`을 하나의 노드로 실행하는 래퍼.
계층적 구성(Supervisor 패턴, 중첩 워크플로)을 구현할 때 사용한다.

```cpp
class SubgraphNode : public GraphNode {
public:
    // input_map:  부모 채널 -> 자식 채널 (부모에서 읽어 자식 입력으로)
    // output_map: 자식 채널 -> 부모 채널 (자식 결과를 부모에 기록)
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {});

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string name() const override;
};
```

**JSON 정의에서의 타입 이름:** `"subgraph"`

**채널 매핑 예:**

```cpp
// 부모의 "user_query" -> 자식의 "messages"
// 자식의 "result"    -> 부모의 "sub_output"
auto sub = std::make_unique<SubgraphNode>(
    "sub_agent",
    compiled_subgraph,
    {{"user_query", "messages"}},   // input_map
    {{"result", "sub_output"}}      // output_map
);
```

---

## 7. GraphEngine

**헤더:** `neograph/graph/engine.h`
**네임스페이스:** `neograph::graph`

NeoGraph의 핵심 실행 엔진. JSON 정의로 그래프를 컴파일하고 super-step 루프로 실행한다.

### 7.1 RunConfig

실행 설정.

```cpp
struct RunConfig {
    std::string thread_id;                       // checkpoint 연결용 스레드 ID
    json        input;                           // 초기 채널 값 (예: {"messages": [...]})
    int         max_steps  = 50;                 // 루프 안전 제한
    StreamMode  stream_mode = StreamMode::ALL;   // 수신할 이벤트 종류
};
```

---

### 7.2 RunResult

실행 결과.

```cpp
struct RunResult {
    json        output;                          // 최종 직렬화된 상태
    bool        interrupted       = false;       // HITL 인터럽트 발생 여부
    std::string interrupt_node;                  // 인터럽트가 발생한 노드
    json        interrupt_value;                 // 인터럽트 사유/값
    std::string checkpoint_id;                   // 마지막 checkpoint ID
    std::vector<std::string> execution_trace;    // 실행된 노드 순서
};
```

---

### 7.3 GraphEngine 클래스

```cpp
class GraphEngine {
public:
    // ── 컴파일 ──

    static std::unique_ptr<GraphEngine> compile(
        const json& definition,
        const NodeContext& default_context,
        std::shared_ptr<CheckpointStore> store = nullptr);

    // ── 실행 ──

    RunResult run(const RunConfig& config);

    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    RunResult resume(const std::string& thread_id,
                     const json& resume_value = json(),
                     const GraphStreamCallback& cb = nullptr);

    // ── 상태 조회/조작 ──

    std::optional<json> get_state(const std::string& thread_id) const;

    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;

    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "");

    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "");

    // ── 설정 ──

    void own_tools(std::vector<std::unique_ptr<Tool>> tools);
    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
    void set_store(std::shared_ptr<Store> store);
    std::shared_ptr<Store> get_store() const;
    void set_retry_policy(const RetryPolicy& policy);
    void set_node_retry_policy(const std::string& node_name,
                               const RetryPolicy& policy);
    const std::string& graph_name() const;
};
```

---

### 7.4 compile()

JSON 정의와 기본 컨텍스트로 그래프를 컴파일한다.

```cpp
static std::unique_ptr<GraphEngine> compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store = nullptr);
```

| 매개변수 | 설명 |
|---------|------|
| `definition` | 그래프 JSON 정의 |
| `default_context` | 모든 노드에 전달되는 기본 컨텍스트 |
| `store` | checkpoint 저장소 (nullptr이면 checkpoint 비활성화) |

**반환값:** 컴파일된 `GraphEngine` 인스턴스.

#### JSON 정의 스키마

```json
{
  "name": "my_graph",
  "channels": {
    "messages":     { "reducer": "append" },
    "status":       { "reducer": "overwrite" },
    "custom_field": { "reducer": "my_reducer", "initial_value": 0 }
  },
  "nodes": {
    "llm":   { "type": "llm_call" },
    "tools": { "type": "tool_dispatch" },
    "my_node": { "type": "my_custom_type", "config": { ... } }
  },
  "edges": [
    { "from": "__start__", "to": "llm" },
    { "from": "llm", "condition": "has_tool_calls",
      "routes": { "true": "tools", "false": "__end__" } },
    { "from": "tools", "to": "llm" }
  ],
  "interrupt_before": ["tools"],
  "interrupt_after": ["llm"]
}
```

| 키 | 필수 | 설명 |
|----|------|------|
| `name` | O | 그래프 이름 |
| `channels` | O | 채널 정의. 각 채널에 `reducer` 필수 |
| `nodes` | O | 노드 정의. 각 노드에 `type` 필수 |
| `edges` | O | 연결 정의. 무조건(`from`/`to`) 또는 조건부(`condition`/`routes`) |
| `interrupt_before` | X | HITL: 이 노드 실행 전에 인터럽트 |
| `interrupt_after` | X | HITL: 이 노드 실행 후에 인터럽트 |

**전체 예제:**

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;

int main() {
    // Provider + 도구 준비
    auto provider = std::make_shared<MyProvider>();
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<MyTool>());

    std::vector<Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    // 컨텍스트 구성
    NodeContext ctx;
    ctx.provider = provider;
    ctx.tools = tool_ptrs;
    ctx.instructions = "당신은 유용한 도우미입니다.";

    // JSON 정의
    json definition = {
        {"name", "assistant"},
        {"channels", {
            {"messages", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"llm",   {{"type", "llm_call"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"}, {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
            {{"from", "tools"}, {"to", "llm"}}
        })}
    };

    // 컴파일 + 실행
    auto engine = GraphEngine::compile(definition, ctx);
    engine->own_tools(std::move(tools));

    RunConfig config;
    config.input = {{"messages", json::array({
        {{"role", "user"}, {"content", "안녕하세요?"}}
    })}};

    auto result = engine->run(config);

    // 결과 확인
    std::cout << "Trace: ";
    for (const auto& n : result.execution_trace)
        std::cout << n << " -> ";
    std::cout << "END\n";
}
```

---

### 7.5 run()

그래프를 동기적으로 실행한다.

```cpp
RunResult run(const RunConfig& config);
```

그래프가 `__end__`에 도달하거나, 인터럽트가 발생하거나, `max_steps`에 도달하면 반환한다.

---

### 7.6 run_stream()

스트리밍 이벤트를 발생시키며 그래프를 실행한다.

```cpp
RunResult run_stream(const RunConfig& config,
                     const GraphStreamCallback& cb);
```

`cb`에 `GraphEvent`가 실시간으로 전달된다. `config.stream_mode`로 수신할 이벤트를 필터링한다.

**사용 예:**

```cpp
auto result = engine->run_stream(config,
    [](const GraphEvent& event) {
        switch (event.type) {
            case GraphEvent::Type::NODE_START:
                std::cout << "[시작] " << event.node_name << "\n";
                break;
            case GraphEvent::Type::LLM_TOKEN:
                std::cout << event.data.get<std::string>() << std::flush;
                break;
            case GraphEvent::Type::NODE_END:
                std::cout << "\n[완료] " << event.node_name << "\n";
                break;
            default:
                break;
        }
    });
```

---

### 7.7 resume()

HITL 인터럽트 후 실행을 재개한다.

```cpp
RunResult resume(const std::string& thread_id,
                 const json& resume_value = json(),
                 const GraphStreamCallback& cb = nullptr);
```

| 매개변수 | 설명 |
|---------|------|
| `thread_id` | 재개할 세션의 스레드 ID |
| `resume_value` | 사용자 응답 값 (인터럽트된 노드에 전달) |
| `cb` | 스트리밍 콜백 (nullptr이면 비스트리밍) |

**HITL 워크플로 전체 예:**

```cpp
// 1. checkpoint store 준비
auto store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::compile(definition, ctx, store);

// 2. 첫 실행 (tools 노드 전에 인터럽트)
RunConfig config;
config.thread_id = "session-001";
config.input = {{"messages", json::array({
    {{"role", "user"}, {"content", "MacBook Pro 1대 주문"}}
})}};

auto result = engine->run(config);

if (result.interrupted) {
    std::cout << "인터럽트 발생: " << result.interrupt_node << "\n";
    // 사용자에게 승인 요청...

    // 3. 승인 후 재개
    auto resumed = engine->resume("session-001", json("승인"));
    // 실행 계속...
}
```

---

### 7.8 get_state() / get_state_history()

```cpp
// 스레드의 최신 상태 조회
std::optional<json> get_state(const std::string& thread_id) const;

// 스레드의 checkpoint 이력 조회
std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                          int limit = 100) const;
```

`get_state()`는 checkpoint store에서 최신 checkpoint를 찾아
채널 값을 JSON으로 반환한다. 해당 스레드가 없으면 `std::nullopt`.

---

### 7.9 update_state()

실행 중이 아닌 스레드의 상태를 외부에서 수정한다.

```cpp
void update_state(const std::string& thread_id,
                  const json& channel_writes,
                  const std::string& as_node = "");
```

| 매개변수 | 설명 |
|---------|------|
| `thread_id` | 대상 스레드 ID |
| `channel_writes` | `{"채널이름": 값}` 형태의 업데이트 |
| `as_node` | 어떤 노드가 쓴 것처럼 기록 (빈 문자열이면 "external") |

---

### 7.10 fork()

기존 스레드의 checkpoint를 기반으로 새 스레드를 분기한다.
time-travel 디버깅이나 what-if 분석에 활용한다.

```cpp
std::string fork(const std::string& source_thread_id,
                 const std::string& new_thread_id,
                 const std::string& checkpoint_id = "");
```

| 매개변수 | 설명 |
|---------|------|
| `source_thread_id` | 원본 스레드 ID |
| `new_thread_id` | 새 스레드 ID |
| `checkpoint_id` | 분기 기준 checkpoint (빈 문자열이면 최신) |

**반환값:** 새로 생성된 checkpoint의 ID.

---

### 7.11 설정 메서드

```cpp
// 도구 소유권을 엔진에 이전 (수명 관리)
void own_tools(std::vector<std::unique_ptr<Tool>> tools);

// checkpoint 저장소 설정
void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);

// cross-thread 공유 메모리 설정
void set_store(std::shared_ptr<Store> store);
std::shared_ptr<Store> get_store() const;

// 전체 노드의 기본 재시도 정책
void set_retry_policy(const RetryPolicy& policy);

// 특정 노드의 재시도 정책
void set_node_retry_policy(const std::string& node_name,
                           const RetryPolicy& policy);

// 그래프 이름 조회
const std::string& graph_name() const;
```

**재시도 정책 설정 예:**

```cpp
auto engine = GraphEngine::compile(definition, ctx);

// 전체 기본: 3회 재시도, 200ms 시작, 2배 백오프, 최대 5초
engine->set_retry_policy({3, 200, 2.0f, 5000});

// LLM 노드만 5회 재시도
engine->set_node_retry_policy("llm", {5, 500, 2.0f, 10000});
```

---

## 8. 체크포인트

**헤더:** `neograph/graph/checkpoint.h`
**네임스페이스:** `neograph::graph`

그래프 실행 상태의 스냅샷을 저장하고 복원하는 기능을 제공한다.
HITL, time-travel 디버깅, 실행 기록 추적에 필수적이다.

### 8.1 Checkpoint

```cpp
struct Checkpoint {
    std::string id;               // UUID v4
    std::string thread_id;        // 세션 식별자
    json        channel_values;   // 직렬화된 채널 데이터
    json        channel_versions; // 채널별 버전 카운터
    std::string parent_id;        // 이전 checkpoint (time-travel 체인)
    std::string current_node;     // checkpoint 시점의 활성 노드
    std::vector<std::string> next_nodes;  // 재개 시 실행할 노드 목록
    CheckpointPhase interrupt_phase;  // Before | After | Completed | NodeInterrupt | Updated
    json        metadata;         // 사용자 정의 메타데이터
    int64_t     step;             // super-step 번호
    int64_t     timestamp;        // Unix epoch 밀리초
    int         schema_version = CHECKPOINT_SCHEMA_VERSION;  // 레이아웃 버전

    static std::string generate_id(); // UUID v4 생성
};
```

| 필드 | 설명 |
|------|------|
| `id` | 고유 식별자. `generate_id()`로 자동 생성 |
| `thread_id` | 대화/세션 단위 식별자 |
| `parent_id` | 이전 checkpoint와 연결 (체인) |
| `interrupt_phase` | `CheckpointPhase` enum. `Before` -- interrupt_before, `After` -- interrupt_after, `Completed` -- super-step 정상 종료, `NodeInterrupt` -- 노드 내부에서 `NodeInterrupt` throw, `Updated` -- `update_state()`로 외부 주입. 문자열 인코딩은 `to_string()` / `parse_checkpoint_phase()` |
| `step` | 몇 번째 super-step에서 생성되었는지 |

---

### 8.2 CheckpointStore (추상 클래스)

checkpoint 영속화 인터페이스. 데이터베이스, 파일 시스템 등으로 구현 가능.

```cpp
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    virtual void save(const Checkpoint& cp) = 0;
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id) = 0;
    virtual std::optional<Checkpoint> load_by_id(const std::string& id) = 0;
    virtual std::vector<Checkpoint> list(const std::string& thread_id,
                                          int limit = 100) = 0;
    virtual void delete_thread(const std::string& thread_id) = 0;
};
```

| 메서드 | 설명 |
|--------|------|
| `save()` | checkpoint 저장 |
| `load_latest()` | 해당 스레드의 최신 checkpoint 로드 |
| `load_by_id()` | ID로 특정 checkpoint 로드 |
| `list()` | 스레드의 checkpoint 이력 조회 (timestamp 순) |
| `delete_thread()` | 스레드의 모든 checkpoint 삭제 |

---

### 8.3 InMemoryCheckpointStore

메모리 기반 구현. 테스트 및 단일 프로세스 환경에 적합하다.

```cpp
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    size_t size() const;  // 전체 checkpoint 개수 (테스트용)
};
```

**사용 예:**

```cpp
auto store = std::make_shared<InMemoryCheckpointStore>();

auto engine = GraphEngine::compile(definition, ctx, store);
// 또는
engine->set_checkpoint_store(store);

// 실행 후 이력 조회
auto history = store->list("thread-001");
for (const auto& cp : history) {
    std::cout << "step=" << cp.step
              << " node=" << cp.current_node
              << " phase=" << to_string(cp.interrupt_phase) << "\n";
}
```

---

## 9. Store

**헤더:** `neograph/graph/store.h`
**네임스페이스:** `neograph::graph`

스레드 간 공유되는 영속적 키-값 저장소.
사용자 선호도, 장기 기억, 에이전트 간 공유 지식 등에 활용한다.

### 9.1 Namespace / StoreItem

```cpp
using Namespace = std::vector<std::string>;

struct StoreItem {
    Namespace   ns;          // 계층적 네임스페이스 경로
    std::string key;         // 항목 키
    json        value;       // 저장된 값
    int64_t     created_at;  // 생성 시각 (Unix epoch ms)
    int64_t     updated_at;  // 수정 시각 (Unix epoch ms)
};
```

`Namespace`는 계층적 경로를 나타내는 문자열 벡터이다.
예: `{"users", "user123", "preferences"}`

---

### 9.2 Store (추상 클래스)

```cpp
class Store {
public:
    virtual ~Store() = default;

    // 값 저장 (생성 또는 갱신)
    virtual void put(const Namespace& ns, const std::string& key,
                     const json& value) = 0;

    // 단일 항목 조회
    virtual std::optional<StoreItem> get(const Namespace& ns,
                                          const std::string& key) const = 0;

    // 네임스페이스 접두사로 검색
    virtual std::vector<StoreItem> search(const Namespace& ns_prefix,
                                           int limit = 100) const = 0;

    // 항목 삭제
    virtual void delete_item(const Namespace& ns,
                              const std::string& key) = 0;

    // 접두사 아래의 네임스페이스 목록
    virtual std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const = 0;
};
```

---

### 9.3 InMemoryStore

메모리 기반 구현. 프로세스 종료 시 데이터가 소멸된다.

```cpp
class InMemoryStore : public Store {
public:
    void put(const Namespace& ns, const std::string& key,
             const json& value) override;
    std::optional<StoreItem> get(const Namespace& ns,
                                  const std::string& key) const override;
    std::vector<StoreItem> search(const Namespace& ns_prefix,
                                   int limit = 100) const override;
    void delete_item(const Namespace& ns, const std::string& key) override;
    std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const override;

    size_t size() const;  // 전체 항목 수 (테스트용)
};
```

**사용 예:**

```cpp
auto store = std::make_shared<InMemoryStore>();
engine->set_store(store);

// 노드 내부에서 Store 접근 (engine->get_store())
// 사용자 선호도 저장
store->put({"users", "u001"}, "language", json("ko"));
store->put({"users", "u001"}, "theme", json("dark"));

// 조회
auto item = store->get({"users", "u001"}, "language");
if (item) {
    std::cout << "Language: " << item->value << "\n"; // "ko"
}

// 네임스페이스 아래 전체 검색
auto prefs = store->search({"users", "u001"});
for (const auto& p : prefs) {
    std::cout << p.key << " = " << p.value << "\n";
}
```

---

## 10. 로더 / 레지스트리

**헤더:** `neograph/graph/loader.h`
**네임스페이스:** `neograph::graph`

JSON 정의로 그래프를 컴파일할 때 노드 타입, 리듀서, 조건 함수를
이름으로 검색하는 싱글턴 레지스트리들.

### 10.1 ReducerRegistry

리듀서 함수 레지스트리.

```cpp
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;
};
```

**내장 리듀서:**

| 이름 | 동작 |
|------|------|
| `"overwrite"` | 새 값으로 완전히 대체 |
| `"append"` | 기존 JSON 배열에 새 배열의 원소를 추가 |

**커스텀 리듀서 등록:**

```cpp
auto& reg = ReducerRegistry::instance();
reg.register_reducer("sum", [](const json& current, const json& incoming) {
    return json(current.get<int>() + incoming.get<int>());
});
```

---

### 10.2 ConditionRegistry

조건 함수 레지스트리. `ConditionalEdge`의 `condition` 필드에서 이름으로 참조된다.

```cpp
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;
};
```

**내장 조건:**

| 이름 | 설명 | 반환값 |
|------|------|--------|
| `"has_tool_calls"` | `messages` 채널의 마지막 메시지에 tool_calls가 있는지 확인 | `"true"` / `"false"` |
| `"route_channel"` | `__route__` 채널의 값을 라우팅 키로 반환 | 채널 값 문자열 |

**커스텀 조건 등록:**

```cpp
auto& reg = ConditionRegistry::instance();
reg.register_condition("check_score", [](const GraphState& state) -> std::string {
    auto score = state.get("score");
    if (score.is_number() && score.get<double>() > 0.8)
        return "high";
    return "low";
});
```

이후 JSON 정의에서:

```json
{
  "from": "evaluator",
  "condition": "check_score",
  "routes": { "high": "summarizer", "low": "researcher" }
}
```

---

### 10.3 NodeFactory

노드 타입 팩토리. JSON 정의의 `type` 필드 값으로 노드 인스턴스를 생성한다.

```cpp
using NodeFactoryFn = std::function<std::unique_ptr<GraphNode>(
    const std::string& name,
    const json& config,
    const NodeContext& ctx)>;

class NodeFactory {
public:
    static NodeFactory& instance();

    void register_type(const std::string& type, NodeFactoryFn fn);
    std::unique_ptr<GraphNode> create(const std::string& type,
                                       const std::string& name,
                                       const json& config,
                                       const NodeContext& ctx) const;
};
```

**내장 노드 타입:**

| 타입 이름 | 클래스 | 설명 |
|----------|--------|------|
| `"llm_call"` | `LLMCallNode` | LLM API 호출 |
| `"tool_dispatch"` | `ToolDispatchNode` | 도구 실행 |
| `"intent_classifier"` | `IntentClassifierNode` | 의도 분류 |
| `"subgraph"` | `SubgraphNode` | 하위 그래프 실행 |

**커스텀 노드 타입 등록:**

```cpp
auto& factory = NodeFactory::instance();

factory.register_type("my_node",
    [](const std::string& name, const json& config, const NodeContext& ctx)
        -> std::unique_ptr<GraphNode> {
        return std::make_unique<MyCustomNode>(name, config, ctx);
    });
```

이후 JSON 정의에서:

```json
{
  "nodes": {
    "processor": { "type": "my_node", "config": { "threshold": 0.5 } }
  }
}
```

---

## 11. ReAct 그래프

**헤더:** `neograph/graph/react_graph.h`
**네임스페이스:** `neograph::graph`

표준 ReAct(Reasoning + Acting) 패턴을 2-노드 그래프로 자동 구성하는 편의 함수.

```cpp
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");
```

| 매개변수 | 설명 |
|---------|------|
| `provider` | LLM provider |
| `tools` | 사용할 도구 목록 (소유권 이전) |
| `instructions` | 시스템 프롬프트 |
| `model` | 모델 이름 (빈 문자열이면 provider 기본값) |

**반환값:** 컴파일된 `GraphEngine`. 내부 구조:

```
__start__ -> llm --(has_tool_calls)--> tools -> llm  (루프)
                 \--(no tool_calls)--> __end__
```

**사용 예:**

```cpp
auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-...",
    .default_model = "gpt-4o-mini"
});

std::vector<std::unique_ptr<neograph::Tool>> tools;
tools.push_back(std::make_unique<WeatherTool>());
tools.push_back(std::make_unique<CalculatorTool>());

auto engine = neograph::graph::create_react_graph(
    provider, std::move(tools),
    "당신은 날씨와 계산을 도와주는 어시스턴트입니다.");

neograph::graph::RunConfig config;
config.input = {{"messages", neograph::json::array({
    {{"role", "user"}, {"content", "서울 날씨와 화씨-섭씨 변환을 알려줘"}}
})}};

auto result = engine->run(config);
```

이 함수는 `Agent::run()`과 동일한 로직을 그래프 엔진으로 표현한 것이다.
checkpoint, HITL, Send/Command 등 그래프 엔진의 모든 기능을 활용할 수 있다.

---

## 12. LLM 모듈

**헤더:** `neograph/llm/openai_provider.h`, `neograph/llm/schema_provider.h`, `neograph/llm/agent.h`
**네임스페이스:** `neograph::llm`

### 12.1 OpenAIProvider

OpenAI 호환 API에 대한 구현. OpenAI 뿐 아니라
동일한 API 형식을 사용하는 서비스(Azure OpenAI, Ollama, vLLM 등)에도 사용 가능하다.

```cpp
class OpenAIProvider : public Provider {
public:
    struct Config {
        std::string api_key;
        std::string base_url = "https://api.openai.com";
        std::string default_model = "gpt-4o-mini";
        int timeout_seconds = 60;
    };

    static std::unique_ptr<OpenAIProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override; // "openai"
};
```

**사용 예:**

```cpp
auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = std::getenv("OPENAI_API_KEY"),
    .base_url = "https://api.openai.com",
    .default_model = "gpt-4o-mini",
    .timeout_seconds = 30
});

neograph::CompletionParams params;
params.messages = {{.role = "user", .content = "안녕하세요!"}};

auto result = provider->complete(params);
std::cout << result.message.content << "\n";
```

**Ollama 로컬 모델 사용:**

```cpp
auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "ollama",
    .base_url = "http://localhost:11434",
    .default_model = "llama3"
});
```

---

### 12.2 SchemaProvider

JSON 스키마 파일로 다양한 LLM provider를 지원하는 범용 구현.
provider별 API 차이(메시지 형식, 도구 호출 방식, 스트리밍 형식 등)를
JSON 설정 파일로 추상화한다.

```cpp
class SchemaProvider : public Provider {
public:
    struct Config {
        std::string schema_path;     // provider JSON 설정 파일 경로
        std::string api_key;         // API 키 (환경변수보다 우선)
        std::string default_model = "gpt-4o-mini";
        int timeout_seconds = 60;
    };

    static std::unique_ptr<SchemaProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override;
};
```

**내장 스키마:**

| 스키마 이름 | 경로 (빌드인) | 대상 API |
|------------|--------------|---------|
| `"openai"` | 빌드 시 임베딩 | OpenAI Chat Completions |
| `"claude"` | 빌드 시 임베딩 | Anthropic Messages API |
| `"gemini"` | 빌드 시 임베딩 | Google Gemini API |

내장 스키마는 `schema_path`에 스키마 이름(예: `"openai"`)만 지정하면 사용된다.

**사용 예:**

```cpp
// Claude API 사용
auto claude = neograph::llm::SchemaProvider::create({
    .schema_path = "claude",
    .api_key = std::getenv("ANTHROPIC_API_KEY"),
    .default_model = "claude-sonnet-4-20250514"
});

// Gemini API 사용
auto gemini = neograph::llm::SchemaProvider::create({
    .schema_path = "gemini",
    .api_key = std::getenv("GOOGLE_API_KEY"),
    .default_model = "gemini-2.0-flash"
});

// 커스텀 스키마 파일 사용
auto custom = neograph::llm::SchemaProvider::create({
    .schema_path = "/path/to/my_provider.json",
    .api_key = "my-key",
    .default_model = "my-model"
});
```

#### SchemaProvider 내부 전략

SchemaProvider는 provider 간 차이를 다음 전략 열거형으로 처리한다:

| 전략 카테고리 | 옵션 | 대상 provider |
|-------------|------|-------------|
| **SystemPromptStrategy** | `IN_MESSAGES` | OpenAI |
| | `TOP_LEVEL` | Claude |
| | `TOP_LEVEL_PARTS` | Gemini |
| **ToolCallStrategy** | `TOOL_CALLS_ARRAY` | OpenAI |
| | `CONTENT_ARRAY` | Claude |
| | `PARTS_ARRAY` | Gemini |
| **ToolResultStrategy** | `FLAT` | OpenAI |
| | `CONTENT_ARRAY` | Claude |
| | `PARTS_ARRAY` | Gemini |
| **ResponseStrategy** | `CHOICES_MESSAGE` | OpenAI |
| | `CONTENT_ARRAY` | Claude |
| | `CANDIDATES_PARTS` | Gemini |
| **StreamFormat** | `SSE_DATA` | OpenAI, Gemini |
| | `SSE_EVENTS` | Claude |

---

### 12.3 Agent

ReAct 루프를 직접 실행하는 고수준 에이전트.
그래프 엔진 없이 LLM 호출 -> 도구 실행 -> 반복 루프를 수행한다.

```cpp
class Agent {
public:
    Agent(std::shared_ptr<Provider> provider,
          std::vector<std::unique_ptr<Tool>> tools,
          const std::string& instructions = "",
          const std::string& model = "");

    // ReAct 루프 실행. 최종 텍스트 응답 반환
    std::string run(std::vector<ChatMessage>& messages,
                    int max_iterations = 10);

    // 스트리밍 ReAct 루프
    std::string run_stream(std::vector<ChatMessage>& messages,
                           const StreamCallback& on_chunk,
                           int max_iterations = 10);

    // 단일 LLM 호출 (도구 루프 없음)
    ChatCompletion complete(const std::vector<ChatMessage>& messages);
};
```

| 메서드 | 설명 |
|--------|------|
| `run()` | LLM 호출 -> tool_calls가 있으면 실행 -> 결과를 messages에 추가 -> 반복. tool_calls가 없으면 종료 |
| `run_stream()` | `run()`과 동일하되 최종 응답을 토큰 단위로 스트리밍 |
| `complete()` | 단일 LLM 호출. 도구 루프 없음 |

**사용 예:**

```cpp
auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-..."
});

std::vector<std::unique_ptr<neograph::Tool>> tools;
tools.push_back(std::make_unique<SearchTool>());

neograph::llm::Agent agent(provider, std::move(tools),
                            "당신은 검색 도우미입니다.");

std::vector<neograph::ChatMessage> messages = {
    {.role = "user", .content = "NeoGraph 라이브러리가 뭐야?"}
};

// 블로킹 실행
std::string answer = agent.run(messages);
std::cout << answer << "\n";

// 또는 스트리밍
std::string streamed = agent.run_stream(messages,
    [](const std::string& chunk) {
        std::cout << chunk << std::flush;
    });
```

**`Agent` vs `create_react_graph()` 비교:**

| 특성 | Agent | create_react_graph() |
|------|-------|---------------------|
| 도구 루프 | O | O |
| checkpoint / HITL | X | O |
| Send / Command | X | O |
| 상태 관리 | 외부 messages 벡터 | 내장 GraphState |
| 확장성 | 제한적 | 노드 추가/조건부 분기 등 자유롭게 확장 |

단순한 ReAct 에이전트에는 `Agent`를, 복잡한 워크플로에는 `GraphEngine`을 사용한다.

---

## 13. MCP 모듈

**헤더:** `neograph/mcp/client.h`
**네임스페이스:** `neograph::mcp`

[Model Context Protocol (MCP)](https://modelcontextprotocol.io) 서버에 연결하여
도구를 자동으로 발견하고 실행하는 기능을 제공한다.

### 13.1 MCPClient

MCP 서버 연결 및 도구 발견 클라이언트.

```cpp
class MCPClient {
public:
    explicit MCPClient(const std::string& server_url);

    // 서버 연결 초기화 + 핸드셰이크
    bool initialize(const std::string& client_name = "neograph");

    // 서버에서 도구 목록을 발견하여 Tool 인스턴스로 반환
    std::vector<std::unique_ptr<Tool>> get_tools();

    // 도구를 이름으로 직접 호출
    json call_tool(const std::string& name, const json& arguments);
};
```

| 메서드 | 설명 |
|--------|------|
| `MCPClient(url)` | MCP 서버 URL로 클라이언트 생성 |
| `initialize()` | JSON-RPC 핸드셰이크 수행. 성공 시 `true` 반환 |
| `get_tools()` | `tools/list`를 호출하여 `MCPTool` 인스턴스 벡터 반환 |
| `call_tool()` | `tools/call`을 호출하여 결과 JSON 반환 |

---

### 13.2 MCPTool

MCP 서버의 원격 도구를 로컬 `Tool` 인터페이스로 래핑.

```cpp
class MCPTool : public Tool {
public:
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    ChatTool get_definition() const override;
    std::string execute(const json& arguments) override;
    std::string get_name() const override;
};
```

`MCPClient::get_tools()`가 자동으로 `MCPTool` 인스턴스를 생성하므로,
직접 생성할 일은 거의 없다.

**사용 예:**

```cpp
// MCP 서버 연결
neograph::mcp::MCPClient mcp("http://localhost:3000");
if (!mcp.initialize("my_agent")) {
    std::cerr << "MCP 연결 실패\n";
    return 1;
}

// 도구 발견
auto mcp_tools = mcp.get_tools();
std::cout << "발견된 도구: " << mcp_tools.size() << "개\n";
for (const auto& t : mcp_tools) {
    auto def = t->get_definition();
    std::cout << "  - " << def.name << ": " << def.description << "\n";
}

// Agent와 통합
auto provider = neograph::llm::OpenAIProvider::create({.api_key = "sk-..."});
neograph::llm::Agent agent(provider, std::move(mcp_tools),
                            "MCP 도구를 사용하여 작업을 수행하세요.");

std::vector<neograph::ChatMessage> messages = {
    {.role = "user", .content = "최신 뉴스를 검색해줘"}
};
auto answer = agent.run(messages);

// GraphEngine과 통합
auto engine = neograph::graph::create_react_graph(
    provider, mcp.get_tools(), "MCP 도구를 활용하세요.");
```

---

## 14. 유틸리티

**헤더:** `neograph/util/request_queue.h`
**네임스페이스:** `neograph::util`

### 14.1 RequestQueue

lock-free 작업 큐와 워커 풀. HTTP 서버 등에서 연결 수락과 LLM 호출을
분리하여 동시성을 관리할 때 사용한다. backpressure 기능을 내장한다.

```cpp
class RequestQueue {
public:
    struct Stats {
        size_t pending;         // 대기 중인 작업 수
        size_t active;          // 실행 중인 작업 수
        size_t completed;       // 완료된 작업 수
        size_t rejected;        // 거부된 작업 수 (큐 가득 참)
        size_t num_workers;     // 워커 스레드 수
        size_t max_queue_size;  // 최대 큐 크기
    };

    // 생성자: 워커 수, 최대 큐 크기 지정
    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000);

    // 소멸자: 모든 워커를 종료하고 join
    ~RequestQueue();

    // 복사/대입 금지
    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    // 작업 제출. 큐가 가득 차면 {false, {}} 반환 (backpressure)
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task);

    // 현재 상태 조회
    Stats stats() const;
};
```

| 메서드 | 설명 |
|--------|------|
| `submit(task)` | 작업을 큐에 추가. 반환값: `{accepted, future}`. `accepted`가 `false`이면 큐 만석으로 거부됨 |
| `stats()` | 현재 큐 상태 (pending, active, completed, rejected 등) |

**사용 예:**

```cpp
neograph::util::RequestQueue queue(4, 1000);  // 4 워커, 최대 1000건

// 작업 제출
auto [accepted, future] = queue.submit([&]() {
    // LLM 호출 등 무거운 작업
    auto result = provider->complete(params);
    // 결과 처리...
});

if (!accepted) {
    // 서버 과부하 — 503 반환 등
    std::cerr << "큐 가득 참, 작업 거부\n";
    return;
}

// 결과 대기 (필요 시)
future.get();

// 상태 확인
auto s = queue.stats();
std::cout << "대기: " << s.pending
          << " 실행: " << s.active
          << " 완료: " << s.completed
          << " 거부: " << s.rejected << "\n";
```

내부적으로 [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue)를
사용하여 lock-free 큐잉을 구현한다. 워커 스레드는 `condition_variable`로 대기하므로
CPU busy-spin을 하지 않는다.

---

## 부록 A: JSON 경로 유틸리티

**헤더:** `neograph/llm/json_path.h`
**네임스페이스:** `neograph::llm::json_path`

SchemaProvider 내부에서 사용하는 JSON dot-path 탐색 유틸리티.

```cpp
// dot-path 분리: "choices.0.message" -> ["choices", "0", "message"]
std::vector<std::string> split_path(const std::string& path);

// 경로로 JSON 탐색 (읽기 전용). 없으면 nullptr
const json* at_path(const json& root, const std::string& path);

// 경로로 JSON 탐색 (쓰기 가능)
json* at_path_mut(json& root, const std::string& path);

// 경로 존재 여부
bool has_path(const json& root, const std::string& path);

// 경로의 값 조회. 없으면 기본값 반환
template<typename T>
T get_path(const json& root, const std::string& path, const T& default_val);

// 경로에 값 설정. 중간 객체 자동 생성
void set_path(json& root, const std::string& path, const json& value);
```

**사용 예:**

```cpp
using namespace neograph::llm::json_path;

json data = {{"choices", json::array({{{"message", {{"content", "Hello"}}}}})};

// 탐색
auto* content = at_path(data, "choices.0.message.content");
// -> json("Hello")

// 기본값 포함 조회
std::string text = get_path<std::string>(data, "choices.0.message.content", "");
// -> "Hello"

// 존재 확인
bool exists = has_path(data, "choices.0.message.tool_calls");
// -> false

// 값 설정
set_path(data, "metadata.model", json("gpt-4o"));
// data["metadata"]["model"] == "gpt-4o"
```

---

## 부록 B: 편의 헤더

`neograph/neograph.h`는 핵심 API 전체를 한 번에 포함하는 편의 헤더이다.

```cpp
#include <neograph/neograph.h>
```

포함되는 헤더:

| 헤더 | 내용 |
|------|------|
| `neograph/types.h` | 기본 타입 (ChatMessage, ToolCall, ...) |
| `neograph/provider.h` | Provider 추상 인터페이스 |
| `neograph/tool.h` | Tool 추상 인터페이스 |
| `neograph/graph/types.h` | 그래프 타입 (Send, Command, ...) |
| `neograph/graph/state.h` | GraphState |
| `neograph/graph/node.h` | GraphNode + 내장 노드 |
| `neograph/graph/engine.h` | GraphEngine |
| `neograph/graph/checkpoint.h` | Checkpoint, CheckpointStore |
| `neograph/graph/loader.h` | NodeFactory, ReducerRegistry, ConditionRegistry |
| `neograph/graph/react_graph.h` | create_react_graph() |
| `neograph/graph/store.h` | Store, InMemoryStore |

LLM provider, MCP, 유틸리티는 별도로 포함해야 한다.

---

## 부록 C: Send/Command 통합 예제

Send(동적 fan-out)와 Command(라우팅 오버라이드)를 함께 사용하는 리서치 에이전트 예제.

### 그래프 구조

```
__start__ -> planner --(Send: researcher x N)--> evaluator --(Command)--> summarizer -> __end__
                 ^                                                 |
                 |                (Command: 추가 조사 필요)          |
                 +------------------------------------------------+
```

### 노드 구현

```cpp
// PlannerNode: 주제를 분석하고 Send로 동적 fan-out
class PlannerNode : public GraphNode {
    int round_ = 0;
public:
    NodeResult execute_full(const GraphState& state) override {
        round_++;
        auto query = state.get("query").get<std::string>();

        std::vector<std::string> topics;
        if (round_ == 1)
            topics = {"market_size", "key_players", "trends"};
        else
            topics = {"risks", "outlook"};

        NodeResult nr;
        nr.writes.push_back(ChannelWrite{"plan", json({
            {"round", round_}, {"topics", topics}
        })});

        // 각 주제에 대해 researcher 노드를 개별 실행
        for (const auto& topic : topics) {
            nr.sends.push_back(Send{
                "researcher",
                json({{"topic", topic}})
            });
        }
        return nr;
    }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        return execute_full(state).writes;
    }

    std::string name() const override { return "planner"; }
};

// EvaluatorNode: 결과를 평가하고 Command로 분기
class EvaluatorNode : public GraphNode {
public:
    NodeResult execute_full(const GraphState& state) override {
        auto findings = state.get("findings");
        int count = findings.is_array() ? (int)findings.size() : 0;

        NodeResult nr;
        if (count >= 5) {
            nr.command = Command{
                "summarizer",
                {ChannelWrite{"status", json("sufficient")}}
            };
        } else {
            nr.command = Command{
                "planner",
                {ChannelWrite{"status", json("needs_more_data")}}
            };
        }
        return nr;
    }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        return execute_full(state).writes;
    }

    std::string name() const override { return "evaluator"; }
};
```

### 그래프 정의 및 실행

```cpp
// 커스텀 노드 등록
auto& factory = NodeFactory::instance();
factory.register_type("planner", [](const std::string&, const json&, const NodeContext&) {
    return std::make_unique<PlannerNode>();
});
factory.register_type("researcher", [](const std::string&, const json&, const NodeContext&) {
    return std::make_unique<ResearcherNode>();
});
factory.register_type("evaluator", [](const std::string&, const json&, const NodeContext&) {
    return std::make_unique<EvaluatorNode>();
});
factory.register_type("summarizer_node", [](const std::string&, const json&, const NodeContext&) {
    return std::make_unique<SummarizerNode>();
});

json definition = {
    {"name", "research_agent"},
    {"channels", {
        {"query",       {{"reducer", "overwrite"}}},
        {"plan",        {{"reducer", "overwrite"}}},
        {"topic",       {{"reducer", "overwrite"}}},   // Send가 주입하는 채널
        {"findings",    {{"reducer", "append"}}},       // 리서치 결과 누적
        {"status",      {{"reducer", "overwrite"}}},
        {"summary",     {{"reducer", "overwrite"}}}
    }},
    {"nodes", {
        {"planner",    {{"type", "planner"}}},
        {"researcher", {{"type", "researcher"}}},
        {"evaluator",  {{"type", "evaluator"}}},
        {"summarizer", {{"type", "summarizer_node"}}}
    }},
    {"edges", json::array({
        {{"from", "__start__"}, {"to", "planner"}},
        {{"from", "planner"},   {"to", "evaluator"}},
        {{"from", "summarizer"}, {"to", "__end__"}}
    })}
};

NodeContext ctx;
auto engine = GraphEngine::compile(definition, ctx);

RunConfig config;
config.input = {{"query", "AI 반도체 시장 분석"}};
config.max_steps = 20;

auto result = engine->run_stream(config, [](const GraphEvent& event) {
    if (event.type == GraphEvent::Type::NODE_START &&
        event.node_name == "__send__") {
        std::cout << "Fan-out: " << event.data["sends"].size() << " tasks\n";
    }
});

// execution_trace: planner -> researcher(x3) -> evaluator
//                  -> planner -> researcher(x2) -> evaluator
//                  -> summarizer -> END
```

핵심 포인트:
- **Send**의 `input`은 채널명-값 매핑이다. 엔진이 `apply_input()`과 동일한 방식으로 상태에 주입한다.
- **Command**의 `goto_node`는 기본 edge 라우팅을 완전히 무시하고 지정 노드로 이동한다.
- Send로 실행된 노드들의 결과는 리듀서를 통해 합산된다 (`append` 리듀서라면 결과가 배열에 누적).

---

## 부록 D: 전체 타입 색인

| 타입/클래스 | 네임스페이스 | 헤더 |
|------------|------------|------|
| `ToolCall` | `neograph` | `types.h` |
| `ChatMessage` | `neograph` | `types.h` |
| `ChatTool` | `neograph` | `types.h` |
| `ChatCompletion` | `neograph` | `types.h` |
| `StreamCallback` | `neograph` | `provider.h` |
| `CompletionParams` | `neograph` | `provider.h` |
| `Provider` | `neograph` | `provider.h` |
| `Tool` | `neograph` | `tool.h` |
| `ReducerType` | `neograph::graph` | `graph/types.h` |
| `ReducerFn` | `neograph::graph` | `graph/types.h` |
| `Channel` | `neograph::graph` | `graph/types.h` |
| `ChannelWrite` | `neograph::graph` | `graph/types.h` |
| `NodeInterrupt` | `neograph::graph` | `graph/types.h` |
| `Send` | `neograph::graph` | `graph/types.h` |
| `Command` | `neograph::graph` | `graph/types.h` |
| `RetryPolicy` | `neograph::graph` | `graph/types.h` |
| `StreamMode` | `neograph::graph` | `graph/types.h` |
| `Edge` | `neograph::graph` | `graph/types.h` |
| `ConditionalEdge` | `neograph::graph` | `graph/types.h` |
| `NodeContext` | `neograph::graph` | `graph/types.h` |
| `GraphEvent` | `neograph::graph` | `graph/types.h` |
| `NodeResult` | `neograph::graph` | `graph/types.h` |
| `ConditionFn` | `neograph::graph` | `graph/types.h` |
| `GraphState` | `neograph::graph` | `graph/state.h` |
| `GraphNode` | `neograph::graph` | `graph/node.h` |
| `LLMCallNode` | `neograph::graph` | `graph/node.h` |
| `ToolDispatchNode` | `neograph::graph` | `graph/node.h` |
| `IntentClassifierNode` | `neograph::graph` | `graph/node.h` |
| `SubgraphNode` | `neograph::graph` | `graph/node.h` |
| `RunConfig` | `neograph::graph` | `graph/engine.h` |
| `RunResult` | `neograph::graph` | `graph/engine.h` |
| `GraphEngine` | `neograph::graph` | `graph/engine.h` |
| `Checkpoint` | `neograph::graph` | `graph/checkpoint.h` |
| `CheckpointStore` | `neograph::graph` | `graph/checkpoint.h` |
| `InMemoryCheckpointStore` | `neograph::graph` | `graph/checkpoint.h` |
| `Namespace` | `neograph::graph` | `graph/store.h` |
| `StoreItem` | `neograph::graph` | `graph/store.h` |
| `Store` | `neograph::graph` | `graph/store.h` |
| `InMemoryStore` | `neograph::graph` | `graph/store.h` |
| `ReducerRegistry` | `neograph::graph` | `graph/loader.h` |
| `ConditionRegistry` | `neograph::graph` | `graph/loader.h` |
| `NodeFactory` | `neograph::graph` | `graph/loader.h` |
| `NodeFactoryFn` | `neograph::graph` | `graph/loader.h` |
| `OpenAIProvider` | `neograph::llm` | `llm/openai_provider.h` |
| `SchemaProvider` | `neograph::llm` | `llm/schema_provider.h` |
| `Agent` | `neograph::llm` | `llm/agent.h` |
| `MCPClient` | `neograph::mcp` | `mcp/client.h` |
| `MCPTool` | `neograph::mcp` | `mcp/client.h` |
| `RequestQueue` | `neograph::util` | `util/request_queue.h` |
