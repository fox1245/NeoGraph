<!-- neograph-i18n: source=docs/mcp.md locale=ko source_sha256=6830afee07902c0353afb9c7bddaee959f53339a0c42a85033545c317dd36a03 -->
# MCP: 그래프를 도구로 공개하고, 도구를 쓰는 그래프 만들기

**Languages:** [English](mcp.md) | [한국어](mcp.ko.md) | [日本語](mcp.ja.md) | [简体中文](mcp.zh-CN.md)

Model Context Protocol(MCP)은 클라이언트가 서버의 도구를 찾고 호출하는 방법을 JSON-RPC 2.0 위에 정의합니다. 클라이언트는 보통 에이전트 호스트, 에디터, 모델 애플리케이션입니다. NeoGraph는 두 역할을 모두 맡을 수 있습니다. 서버로서는 도구를 공개하며, 대개 그래프 하나가 도구 하나입니다. 클라이언트로서는 다른 서버가 공개한 도구를 그래프 노드나 모델의 도구 호출에서 부릅니다.

이 페이지를 [Harness MCP](HARNESS_MCP.md)와 혼동하지 마세요. Harness MCP 문서는 다중 워커 워크플로를 컴파일하고 실행하는 도구 6개를 갖춘, 이미 만들어진 MCP 서버를 설명합니다. 이 페이지는 `neograph::mcp` 라이브러리로 직접 서버나 클라이언트를 만드는 방법을 다룹니다.

## 구성 요소

| 역할 | C++ 타깃 | CMake 옵션(기본값) | Python |
|---|---|---|---|
| 클라이언트(stdio, Streamable HTTP) | `neograph::mcp` | `NEOGRAPH_BUILD_MCP_CLIENT`(켜짐) | `neograph_engine.mcp` |
| stdio 서버 | `neograph::mcp_server` | `NEOGRAPH_BUILD_MCP_SERVER`(켜짐) | 없음 |
| Streamable HTTP 서버 | `neograph::mcp_http_server` | `NEOGRAPH_BUILD_MCP_HTTP_SERVER`(**꺼짐**) | 없음 |

`NEOGRAPH_BUILD_MCP`(켜짐)가 클라이언트와 stdio 서버 옵션의 기본값을 정합니다. HTTP 서버는 직접 켜야 합니다. Python 패키지에는 MCP 서버가 없습니다. Python에서 서버를 만들려면 공식 MCP Python SDK를 쓰고, 도구 함수 안에서 컴파일된 엔진을 호출하세요.

## 프로토콜 버전

서버와 클라이언트는 MCP `2025-11-25`를 쓰며 `2025-06-18`, `2025-03-26`, `2024-11-05`도 받아들입니다. 서버는 클라이언트가 요청한 리비전으로 응답하고, 그 밖의 요청에는 `2025-11-25`로 응답합니다. 전송 방식은 stdio와 Streamable HTTP뿐입니다. 이전의 HTTP+SSE 전송은 지원하지 않습니다. `2026-07-28` 리비전(`initialize`와 세션이 없음)도 아직 지원하지 않습니다. 그 리비전만 제공하는 서버는 핸드셰이크를 `-32022`로 거절하며, 호출자에게는 `MCPError`로 전달됩니다. 자세한 내용은 [C++ 레퍼런스](reference-en.md#13-mcp-module)에 있습니다.

## stdio로 그래프를 도구로 공개하기

이 프로그램은 단어 수를 세는 노드 하나짜리 그래프를 만들어 `word_count` 도구로 공개합니다. API 키는 필요 없습니다. 도구를 호출할 때마다 그래프를 한 번 실행합니다.

```cpp
#include <neograph/neograph.h>
#include <neograph/mcp/server.h>

#include <memory>
#include <sstream>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

// A graph node: count the words in the "text" channel and write the number to "count".
class WordCountNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        std::istringstream words(input.state.get(ChannelKey<std::string>{"text"}));
        int count = 0;
        for (std::string word; words >> word;) ++count;
        co_return NodeOutput{{ChannelWrite{"count", json(count)}}};
    }
    std::string get_name() const override { return "word_count"; }
};

std::shared_ptr<GraphEngine> build_engine() {
    NodeFactory::instance().register_type(
        "docs.word_count", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<WordCountNode>();
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"count"})},
             {"exports", json::array({"count"})}});
    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "word-count"},
        {"channels", {{"text", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"count", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"word_count", {{"type", "docs.word_count"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "word_count"}},
                   {{"from", "word_count"}, {"to", "__end__"}}}},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::build_strict(definition, EngineConfig{}));
}

std::unique_ptr<mcp::MCPServer> make_server(std::shared_ptr<GraphEngine> engine) {
    mcp::MCPServerConfig config;
    config.server_info = {{"name", "word-count-graph"}, {"version", "1.0.0"}};
    auto server = std::make_unique<mcp::MCPServer>(std::move(config));

    mcp::ToolDefinition tool;
    tool.name = "word_count";
    tool.description = "Count the words in a text by running a NeoGraph graph.";
    tool.input_schema = {{"type", "object"},
                         {"required", json::array({"text"})},
                         {"properties", {{"text", {{"type", "string"}}}}},
                         {"additionalProperties", false}};

    // One call is one graph run. Calls arrive on the server's worker threads;
    // the engine may be shared, and the client's cancellation reaches the run
    // through the call's CancelToken.
    server->register_tool(std::move(tool), [engine](const json& arguments, const auto& cancel) {
        RunConfig run;
        run.input = {{"text", arguments.at("text")}};
        run.cancel_token = cancel;
        const auto result = engine->run(run);

        mcp::CallToolResult reply;
        reply.content = json::array(
            {{{"type", "text"}, {"text", std::to_string(result.channel<int>("count"))}}});
        return reply;
    });
    return server;
}

}  // namespace

int main() {
    make_server(build_engine())->run();  // newline-delimited JSON-RPC on stdin and stdout
    return 0;
}
```

동작 방식은 다음과 같습니다.

- 도구는 이름, 인자의 JSON Schema(`input_schema`에 `"type": "object"`가 있어야 함), 핸들러로 이루어집니다. 서버는 핸들러를 실행하기 전에 모든 호출의 인자를 스키마로 검사합니다. 인자가 잘못되면 `isError: true`, 검증 메시지, 코드가 `NG.InvalidRequest`인 `_meta["io.neograph/failure"]`가 담긴 결과가 돌아갑니다. NeoGraph 클라이언트는 이를 핸들러가 실행되지 않았다는 표시로 읽습니다.
- 핸들러는 고정 크기 워커 풀에서 실행됩니다. `MCPServerConfig::max_concurrent_calls`(4)개가 동시에 실행되고 `max_pending_calls`(64)개가 대기합니다. 이를 넘는 호출은 JSON-RPC 오류 `-32000`("MCP tool queue is full")을 받습니다. 엔진은 그 스레드들이 공유합니다. 노드는 여러 실행에서 재사용되므로, 실행마다 달라지는 데이터는 노드 객체가 아니라 채널에 두세요. 엔진에 체크포인트 저장소가 있다면 호출마다 서로 다른 `RunConfig::thread_id`를 지정하세요.
- 클라이언트가 `notifications/cancelled`를 보내면 해당 호출의 `CancelToken`이 취소됩니다. 이 토큰을 `run.cancel_token`으로 넘기면 그래프 실행도 멈춥니다.
- `run()`은 stdin에서 한 줄에 JSON-RPC 메시지 하나씩 읽고 stdout에 씁니다. 프로그램이 stdout에 다른 것을 출력하면 프로토콜이 깨지므로, 로그는 stderr로 보내세요.

`neograph::core`와 `neograph::mcp_server`를 링크합니다.

```cmake
add_executable(word_count_server word_count_server.cpp)
target_link_libraries(word_count_server PRIVATE neograph::core neograph::mcp_server)
```

MCP 호스트에서 쓰려면 이 실행 파일을 인자 없는 stdio 서버로 등록하세요.

## 클라이언트에서 호출하기

서버를 자식 프로세스로 시작하고 도구를 호출합니다. `get_tool_definitions()`는 모든 결과 페이지를 따라가며, `call_tool_result()`는 응답의 content, `structuredContent`, `isError`를 돌려줍니다.

```cpp
#include <neograph/neograph.h>
#include <neograph/mcp/client.h>

#include <iostream>

using namespace neograph;

int main(int argc, char** argv) {
    // Start the server as a child process and talk to it over its stdin and stdout.
    mcp::MCPClient client(std::vector<std::string>{argv[1]});
    client.initialize("docs-client");

    for (const auto& definition : client.get_tool_definitions())
        std::cout << "tool: " << definition.name << " - " << definition.description << '\n';

    const auto result =
        client.call_tool_result("word_count", {{"text", "graphs keep state in channels"}});
    std::cout << "is_error: " << std::boolalpha << result.is_error << '\n'
              << "text: " << result.content[0]["text"].get<std::string>() << '\n';
    client.shutdown();
    return 0;
}
```

Python도 같습니다. 클라이언트 객체가 해제되면 자식 프로세스가 종료됩니다.

```python
from neograph_engine import mcp

client = mcp.MCPClient(["./word_count_server"])  # stdio: starts the server as a child process
# client = mcp.MCPClient("http://127.0.0.1:8000/mcp")  # Streamable HTTP: the endpoint URL
client.initialize("docs-client")

for definition in client.get_tool_definitions():
    print(definition.name, "-", definition.description)

result = client.call_tool_result("word_count", {"text": "graphs keep state in channels"})
print(result.is_error, result.content[0]["text"])
```

C++ 프로그램을 `./mcp_client ./word_count_server`로 실행하면 다음을 출력합니다.

```text
tool: word_count - Count the words in a text by running a NeoGraph graph.
is_error: false
text: 5
```

Python 스크립트는 다음을 출력합니다.

```text
word_count - Count the words in a text by running a NeoGraph graph.
False 5
```

0.14.0 휠에도 이 Python 클라이언트가 들어 있습니다. stdio 전송은 위 서버와, HTTP 클라이언트는 아래에서 설명하는 HTTP 서버와 동작하며, `MCPClient.shutdown()`도 있습니다. 0.13.1 이하의 휠에는 `shutdown()`이 없고, 그 휠의 HTTP 클라이언트는 해당 HTTP 서버에 세션을 열지 못했습니다. 그런 휠이라면 현재 소스로 빌드하세요.

HTTP에서는 `MCPClient("http://127.0.0.1:8000/mcp")`처럼 서버의 엔드포인트 URL을 주며, 이 URL은 그대로 사용됩니다. 찾아낸 도구는 자신의 세션을 유지하므로 `MCPClient` 객체가 소멸된 뒤에도 쓸 수 있습니다.

## 그래프 노드에서 도구 호출하기

노드가 클라이언트를 들고 있다가 도구를 호출할 수 있습니다. 실행의 마감 시각과 취소 토큰이 호출을 제한합니다. 실행이 취소되면 클라이언트가 서버에 `notifications/cancelled`를 보내고 늦게 도착한 응답은 버립니다.

```cpp
#include <neograph/mcp/client.h>
#include <neograph/neograph.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// A node that calls the "word_count" tool of an MCP server and writes the reply.
class RemoteWordCount final : public GraphNode {
public:
    explicit RemoteWordCount(std::shared_ptr<mcp::MCPClient> client) : client_(std::move(client)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto text = input.state.get(ChannelKey<std::string>{"text"});
        const json arguments = {{"text", text}};
        // The run's deadline and cancel token bound the call. If the run is
        // cancelled, the client sends notifications/cancelled to the server.
        const auto deadline =
            input.ctx.deadline.value_or(std::chrono::steady_clock::time_point::max());
        const auto reply = co_await client_->call_tool_result_async(
            "word_count", arguments, deadline, input.ctx.cancel_token);
        if (reply.is_error) throw std::runtime_error("word_count failed");

        NodeOutput output;
        const int count = std::stoi(reply.content[0]["text"].get<std::string>());
        output.writes.push_back(ChannelWrite{"count", json(count)});
        co_return output;
    }

    std::string get_name() const override { return "remote_word_count"; }

private:
    std::shared_ptr<mcp::MCPClient> client_;
};

}  // namespace

int main(int argc, char** argv) {
    auto client = std::make_shared<mcp::MCPClient>(std::vector<std::string>{argv[1]});
    client->initialize("docs-graph");

    NodeFactory::instance().register_type(
        "docs.remote_word_count", [client](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<RemoteWordCount>(client);
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"count"})},
             {"exports", json::array({"count"})}});

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "remote-word-count"},
        {"channels", {{"text", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"count", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"count_words", {{"type", "docs.remote_word_count"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "count_words"}},
                   {{"from", "count_words"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig run;
    run.input = {{"text", "tools can be called from a graph node"}};
    std::cout << "count: " << engine->run(run).channel<int>("count") << '\n';
    return 0;
}
```

앞 절의 서버 경로를 넘겨 `./remote_word_count ./word_count_server`로 실행하면 `count: 8`을 출력합니다.

모델이 도구를 직접 고르게 하려면 `MCPClient::get_tools()`로 서버의 도구를 NeoGraph `Tool` 객체로 받습니다. 이를 `NodeContext::tools`에 넣으면(Python에서는 `ng.NodeContext(tools=client.get_tools())`) `tool_dispatch` 노드가 모델이 요청한 호출을 실행합니다. [C++ 예제](../examples/README.md)에는 MCP 에이전트(03, 20, 22~24)가 있으며 API 키가 필요 없습니다. 표준 라이브러리만 쓰는 데모 서버 `examples/mcp_demo_server.py`가 도구를 제공하고, 스크립트로 만든 모델이 LLM 역할을 합니다. 실제 모델을 쓰려면 `--live`를 붙이며, 이때는 `OPENROUTER_API_KEY`가 필요합니다. [Python 예제 26](../bindings/python/examples/README.md)은 로컬 MCP 서버를 직접 띄우며 역시 키가 필요 없습니다.

## Streamable HTTP로 공개하기

`neograph::mcp_http_server`를 링크하고 `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON`으로 빌드한 뒤, 위 프로그램의 `main`을 바꿉니다. `make_server`는 그 목록에 있는 함수이며, 프로그램에는 `#include <neograph/mcp/http_server.h>`, `<cstdlib>`, `<string_view>`도 필요합니다.

```cpp
int main(int argc, char** argv) {
    auto engine = build_engine();

    mcp::MCPHttpServerConfig http;
    http.host = "127.0.0.1";
    http.port = argc > 1 ? std::atoi(argv[1]) : 8000;  // 0 picks a free port
    http.endpoint = "/mcp";

    // The factory runs once per HTTP session and must return a fresh server.
    mcp::MCPHttpServer server(
        [engine](std::string_view /*authorization scope*/) {
            mcp::MCPHttpServerSession session;
            session.server = make_server(engine);
            return session;
        },
        http);
    server.start();  // blocks until stop(); start_async() returns at once
    return 0;
}
```

- HTTP 세션 하나가 `MCPServer` 하나입니다. 팩토리는 세션마다 한 번 실행되며 새 서버에 도구를 등록해야 합니다. 팩토리의 인자는 `bearer_authorizer`가 돌려준 스코프이므로, 주체별로 세션을 분리할 수 있습니다.
- `MCPHttpServerConfig::host`의 기본값은 `127.0.0.1`입니다. 다른 주소에 바인드하면 `bearer_authorizer`가 설정되어 있지 않은 한 `std::invalid_argument`가 발생합니다. 인증기는 베어러 토큰을 받아 호출자의 스코프를 돌려줍니다. `std::nullopt`나 빈 문자열을 돌려주면 토큰을 `401`로 거절합니다. 세션은 스코프에 묶이므로, 다른 주체가 유출된 `Mcp-Session-Id`를 재사용할 수 없습니다.
- `Origin` 헤더가 있는 요청은 그 출처가 `allowed_origins`에 없으면 `403`으로 거절합니다. loopback 바인드에서는 서버 자신의 loopback 출처, 즉 바인드한 포트의 `http://localhost:<port>`, `http://127.0.0.1:<port>` 또는 `http://[::1]:<port>`도 받아들이고, 각 출처를 처음 받아들일 때 stderr에 한 줄 씁니다. 브라우저 인스펙터나 개발 서버처럼 다른 포트의 페이지는 목록에 넣어야 합니다. DNS 리바인딩 페이지는 자기 출처를 보내므로 거절됩니다.
- loopback 바인드에서는 서버가 `Host`도 확인합니다. 포트와 관계없이 `localhost`, `127.0.0.1`, `[::1]`과 `allowed_hosts`에 나열한 이름에만 응답합니다. 그 밖의 `Host`는 `Origin`이 있든 없든 `403`으로 거절하므로, DNS 리바인딩 페이지는 `Origin`을 싣지 않은 요청에서도 거절됩니다. `Host`가 없거나 `host[:port]` 형태가 아닌 요청은 `400`을 받습니다. 항목은 `mcp.example.com`이나 `[::1]`처럼 포트 없는 이름이고 대소문자를 구분하지 않고 비교하며, `*`는 어떤 `Host`든 받아들입니다. 공개 이름을 그대로 전달하는 로컬 리버스 프록시나 터널 뒤에서는 그 이름을 목록에 넣으세요. 다른 바인드에서는 목록이 비어 있으면 아무것도 확인하지 않고, 비어 있지 않으면 나열한 이름만 받아들입니다.
- `initialize` 이후의 요청은 `MCP-Protocol-Version` 헤더에 `MCP_SUPPORTED_PROTOCOL_VERSIONS`의 어느 리비전이든 적을 수 있습니다. 세션은 협상한 리비전을 그대로 쓰고, 한 세션에서 다른 리비전을 적은 첫 요청은 stderr에 기록됩니다. 지원하지 않거나 형식이 틀린 값은 `400`으로 거절하고, 헤더가 없는 요청은 협상한 리비전을 씁니다.
- `protected_resource_metadata`를 설정하면 RFC 9728 메타데이터를 공개하고 모든 `401` 응답의 챌린지에 그 위치를 알립니다. 토큰을 OAuth 인가 서버에서 받는 배포를 위한 기능입니다. 설정하지 않으면 고정 베어러 토큰에는 디스커버리가 필요 없습니다.
- 제한: `max_sessions`(64, 가장 오래 쓰지 않은 유휴 세션부터 제거), `session_idle_timeout`(30분), `max_payload_bytes`(1 MiB), `response_timeout`(10분, 이보다 오래 걸리는 요청은 `504`를 받고 해당 호출이 취소됨).
- 이 전송은 POST마다 `application/json` 응답 하나를 돌려주고 `GET`에는 `405`로 답합니다. 서버에서 클라이언트로 가는 이벤트 스트림은 제공하지 않습니다.

## 인터럽트, 취소, 오류

- MCP 도구 호출은 일시 중지했다가 재개할 수 없습니다. 핸들러가 실행한 그래프가 멈추면(`RunResult::interrupted`) 클라이언트에게 무엇을 보일지는 핸들러가 정합니다. 핸들러가 `NodeInterrupt`를 던지면 그 호출에 입력이 필요하다는 뜻이 되어, 클라이언트는 JSON-RPC 오류 `-32011`과 `data` `{code: "NG.Interrupted", reason, value}`를 받습니다.
- 핸들러가 다른 예외를 던지면 `isError: true`인 결과가 가고, `_meta["io.neograph/failure"]`에 안정적인 코드가 담깁니다. 클라이언트에는 실패의 공개 메시지만 전달되고 예외 텍스트는 전달되지 않습니다. 코드와 메시지를 직접 정하려면 `NodeFailureError`를 던지세요.
- 클라이언트가 취소한 호출에는 어떤 전송에서든 응답이 가지 않습니다. 알 수 없는 도구는 `-32602`입니다.
- 에이전트에서 `MCPTool`(`client.get_tools()`로 얻은 도구)이 실패했을 때 모델이 읽는 내용은 서버가 도구를 실행했는지에 따라 달라집니다. 서버가 실행하기 전에 거부한 요청은 서버의 메시지를 담고 `retryable`과 `effect_uncertain`이 모두 false인 평범한 `failed` 결과입니다. JSON-RPC 오류 `-32700`, `-32600`, `-32601`, `-32602`와, `NG.InvalidRequest`로 표시된 `isError` 결과가 여기에 해당하며, `MCPServer`는 입력 스키마에 맞지 않는 인자를 이렇게 표시합니다. 아무것도 실행되지 않았으므로 모델은 인자를 고칠 수 있습니다. 그 밖의 실패는 도구가 시작된 뒤에 일어났을 수 있으므로(핸들러가 낸 `isError` 결과, `-32603`, 서버 오류), 효과를 배제하는 `ToolExecutionPolicy`가 없으면 `effect_uncertain`이 true인 `reconciliation_required`입니다. 평범한 `failed`를 받으려면 읽기 전용이거나 멱등인 도구를 정책에 선언하세요. [도구 실패 계약](reference-en.md#tool-failure-contract)을 참조하세요.
- MCP, gRPC, A2A, ACP 전반의 실패 분류 표는 레퍼런스의 [프로토콜 실패 매핑](reference-en.md#protocol-failure-mapping)에 있으며, 여기에 다시 옮기지 않았습니다.

## 테스트와 상호 운용성

NeoGraph의 서버와 클라이언트는 CI(`.github/workflows/mcp-interop.yml`)에서 공식 SDK와 대조해 검사합니다. Python SDK `mcp` 2.3.0과 1.30.0, TypeScript SDK 1.32.1과 2.3.1이 대상입니다. 테스트 모음은 `bindings/python/tests/interop/mcp`에 있고, 테스트가 접속하는 서버는 `tests/fixtures/mcp_interop_probe_server.cpp`입니다. 이 서버는 구조화된 출력, 오류, 취소, 메타데이터를 다루는 도구를 등록하므로, 읽어 보면 서버 API 전체를 빠르게 파악할 수 있습니다. 위와 같은 그래프 서버는 고치지 않아도 공식 Python SDK 클라이언트(`streamable_http_client`, `ClientSession`)에 응답합니다.
