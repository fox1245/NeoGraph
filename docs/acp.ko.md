<!-- neograph-i18n: source=docs/acp.md locale=ko source_sha256=9141b5a2b6a600de92af0370ad877ff8ee8ade87d01cd47484b4414acebe352a -->
# ACP: 그래프를 에디터 에이전트로 쓰기

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

Agent Client Protocol(ACP)은 에디터를 코딩 에이전트나 대화 에이전트에 연결합니다. 에디터(Zed, CodeCompanion을 쓰는 Neovim, Gemini CLI 등)는 에이전트를 자식 프로세스로 실행하고, 에이전트의 stdin과 stdout으로 JSON-RPC 2.0 메시지를 한 줄에 JSON 객체 하나씩 주고받습니다. 에디터가 세션을 열고 프롬프트를 보내면, 에이전트는 답을 업데이트로 스트리밍합니다. 에이전트가 에디터에 권한이나 파일, 터미널 접근을 요청할 수도 있습니다.

`ACPServer`는 컴파일된 NeoGraph 그래프를 이런 에이전트로 실행합니다. NeoGraph는 에이전트 쪽을 구현하며, 에디터가 클라이언트입니다. `ACPClient`는 에이전트가 에디터에 자신의 요청을 보내는 데 쓰는 핸들입니다. 에디터가 오류로 답하면 `acp::ACPClientError`를 던집니다. 없는 파일에 대한 `-32002`처럼 요청이 성공할 수 없다는 코드이면 `acp::ACPClientRefusal`이 되고, 노드는 재시도되지 않습니다.

## 구성 요소

| 역할 | C++ 타깃 | CMake 옵션(기본값) | Python |
|---|---|---|---|
| 에이전트(서버) | `neograph::acp` | `NEOGRAPH_BUILD_ACP`(켜짐) | 공식 SDK 사용 |
| Streamable HTTP·WebSocket 에이전트(실험적) | `neograph::acp_http_server` | `NEOGRAPH_BUILD_ACP_HTTP_SERVER`(**꺼짐**) | 없음 |

Python 패키지에는 네이티브 ACP 서버가 없습니다. Python으로 에이전트를 만들려면 공식 SDK(`pip install "neograph-engine[acp]"`)를 설치하고, 그 뒤에서 `neograph_engine.ProtocolHostAdapter`로 엔진을 실행하세요. 이 어댑터는 ACP 세션 ID를 NeoGraph `thread_id`에 대응시키고, 체크포인트에서 재개하고, 취소를 전달하고, 토큰을 스트리밍합니다. [Python 예제](../bindings/python/examples/README.md)에 이런 에이전트가 있습니다. ACP에는 Python 3.10 이상이 필요합니다.

## 버전

ACP 프로토콜 버전 1입니다. 메시지는 공식 ACP 스키마 v1.25.0과 대조해 검사하고, 서버는 CI(`.github/workflows/protocol-interop.yml`, 테스트 모음은 `bindings/python/tests/interop/acp_suite`)에서 공식 Python SDK(`agent-client-protocol` 0.12.1과 사전 릴리스 1.0.0rc2) 및 TypeScript SDK(`@agentclientprotocol/sdk` 1.8.0)와 함께 시험합니다.

## 그래프를 에이전트로 공개하기

이 프로그램은 [A2A 가이드](a2a.md)의 릴리스 노트 그래프입니다. `draft`는 프롬프트로 릴리스 노트를 쓰고, `publish`는 사람이 승인한 뒤에만 실행됩니다. API 키는 필요 없습니다.

```cpp
#include <neograph/acp/server.h>
#include <neograph/neograph.h>

#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// draft: write a draft from the incoming prompt.
class DraftNode final : public SyncGraphNode {
public:
    DraftNode() : SyncGraphNode("draft") {}

protected:
    NodeOutput run_sync(NodeInput input) override {
        const auto topic = input.state.get(ChannelKey<std::string>{"prompt"});
        return NodeOutput{{ChannelWrite{"draft", json("Release notes: " + topic)}}};
    }
};

// publish: runs only after a person has answered; the answer is the resume value.
class PublishNode final : public SyncGraphNode {
public:
    PublishNode() : SyncGraphNode("publish") {}

protected:
    NodeOutput run_sync(NodeInput input) override {
        const auto draft = input.state.get(ChannelKey<std::string>{"draft"});
        const auto answer = input.ctx.resume_value ? input.ctx.resume_value->get<std::string>() : "";
        const bool approved = answer.rfind("approve", 0) == 0;  // "approve" or "approved"
        const auto reply = approved ? "Published: " + draft : "Not published (" + answer + "): " + draft;
        return NodeOutput{{ChannelWrite{"response", json(reply)}}};
    }
};

}  // namespace

int main() {
    NodeFactory::instance().register_type(
        "docs.draft", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<DraftNode>();
        });
    NodeFactory::instance().register_type(
        "docs.publish", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PublishNode>();
        });

    // The ACP server reads the user's text from the "prompt" channel and the
    // agent's answer from the "response" channel.
    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "release-notes"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"draft", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"response", {{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", {{"draft", {{"type", "docs.draft"}}},
                   {"publish", {{"type", "docs.publish"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "draft"}},
                   {{"from", "draft"}, {"to", "publish"}},
                   {{"from", "publish"}, {"to", "__end__"}}}},
        {"interrupt_before", json::array({"publish"})},
    };

    // A checkpoint store is what lets a turn wait for an answer and resume.
    EngineConfig config;
    config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    std::shared_ptr<GraphEngine> engine = GraphEngine::build_strict(definition, std::move(config));

    // Anything the agent prints on stdout is protocol traffic; log to stderr.
    acp::ACPServer server(engine, json{{"name", "release-notes"}, {"version", "1.0.0"}});
    server.run();  // newline-delimited JSON-RPC on stdin and stdout, until EOF
    return 0;
}
```

동작 방식은 다음과 같습니다.

- `session/prompt`는 프롬프트의 텍스트를 그래프의 `prompt` 채널에 넣습니다. `response` 채널의 텍스트는 `agent_message_chunk` 업데이트 하나로 돌아가며, 턴은 `stopReason: "end_turn"`으로 끝납니다. 이를 바꾸려면 `ACPGraphAdapter`에서 `input_channel()`, `output_channel()`, `build_session_state()`(작업 디렉터리 같은 채널 값을 채울 때), 또는 아래의 인터럽트 훅을 재정의하고, 어댑터를 생성자의 세 번째 인자로 넘기세요.
- 그래프 인터럽트는 같은 턴 안의 권한 요청이 됩니다. `interrupt_before`나 `NodeInterrupt`가 발생하면 서버가 `approve`(`allow_once`)와 `reject`(`reject_once`) 옵션을 담아 `session/request_permission`을 보냅니다. 에디터가 고른 옵션의 ID가 재개 값이므로, 위 `publish`는 `"approve"` 또는 `"reject"`를 받습니다. 옵션이나 값을 바꾸려면 `interrupt_permission()`과 `interrupt_resume_value()`를 재정의하세요. 에디터가 `cancelled`로 답하면 턴은 `cancelled`로 끝납니다.
- `run()`은 stdin이 닫힐 때까지 읽습니다. 프로그램이 stdout에 다른 것을 쓰면 프로토콜이 깨지므로, 로그는 stderr로 보내세요.
- 서버는 프롬프트를 최대 32개까지 동시에 처리하며, 세션당 하나입니다(`set_max_inflight_prompts`). 거절된 프롬프트는 `data.reason`이 `"overloaded"` 또는 `"session_busy"`인 JSON-RPC 오류 `-32603`을 받습니다.

`neograph::core`와 `neograph::acp`를 링크합니다.

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::acp)
```

에디터는 실행 파일을 시작해 그 stdin과 stdout으로 ACP를 주고받습니다. 등록 방법은 에디터마다 다릅니다.

## 에디터 없이 해 보기

이 스크립트가 에디터 역할을 합니다. 에이전트를 시작하고, 세션을 열고, 프롬프트를 보내고, 권한 요청을 승인한 뒤 일어난 일을 출력합니다.

```python
import json
import subprocess
import sys

# Start the agent the way an editor does: a child process, JSON-RPC on stdin/stdout.
agent = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)


def send(message):
    agent.stdin.write(json.dumps(message) + "\n")
    agent.stdin.flush()


def call(request_id, method, params):
    """Send a request and return its result, answering the agent's own requests on the way."""
    send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
    while True:
        message = json.loads(agent.stdout.readline())
        if message.get("method") == "session/request_permission":
            options = [option["optionId"] for option in message["params"]["options"]]
            print("agent asks:", message["params"]["toolCall"]["title"], options)
            send({"jsonrpc": "2.0", "id": message["id"],
                  "result": {"outcome": {"outcome": "selected", "optionId": "approve"}}})
        elif message.get("method") == "session/update":
            print("update:", message["params"]["update"]["content"]["text"])
        elif "method" not in message and message.get("id") == request_id:
            return message["result"]


info = call(1, "initialize", {"protocolVersion": 1, "clientCapabilities": {}})["agentInfo"]
print("agent:", info["name"], info["version"])
session = call(2, "session/new", {"cwd": "/tmp", "mcpServers": []})["sessionId"]
result = call(3, "session/prompt",
              {"sessionId": session, "prompt": [{"type": "text", "text": "version 2.0"}]})
print("stop reason:", result["stopReason"])
agent.stdin.close()
agent.wait()
```

에이전트 경로를 넘겨 `python acp_client.py ./release_notes_agent`로 실행하면 다음을 출력합니다.

```text
agent: release-notes 1.0.0
agent asks: Interrupt before node: publish ['approve', 'reject']
update: Published: Release notes: version 2.0
stop reason: end_turn
```

`"approve"`를 `"reject"`로 바꾸면 업데이트가 `Not published (reject): Release notes: version 2.0`으로 바뀝니다.

## 취소, 세션, 오류

- **취소.** `session/cancel`은 실행 중인 턴을 취소하고 그 턴은 `stopReason: "cancelled"`로 끝납니다. 실행 중인 턴이 없을 때의 취소는 무시됩니다. 실행 중인 `session/prompt`를 요청 id로 지목한 `$/cancel_request`도 같은 방식으로 그 턴을 취소하며, 다른 요청을 지목한 것은 무시됩니다. stdin이 파일 끝에 이르면 `run()`은 실행 중인 턴이 끝나도록 `set_eof_grace`(1초)만큼 기다린 뒤 취소합니다.
- **턴이 끝나는 방식.** 실행이 완료되면 `end_turn`, 취소되면 `cancelled`, 실행이 `RunConfig::max_steps`에 이르면 `max_turn_requests`, 모델 토큰 예산을 넘으면 `max_tokens`, provider가 거절하면 `refusal`입니다. 그 밖의 실패는 `data`가 `{code, retryable, retryable_by_class}`인 JSON-RPC 오류 `-32603`입니다. `retryable`은 엔진이 노드의 시도를 다 쓰면 `false`가 되고, `retryable_by_class`는 실패의 분류가 재시도를 허용하는지를 알려 줍니다. 이 오류의 메시지는 실패의 공개 메시지이고 예외 텍스트는 아닙니다. 전체 표는 레퍼런스의 [ACP](reference-en.md#neographacp--agent-client-protocol)와 [프로토콜 실패 매핑](reference-en.md#protocol-failure-mapping)에 있습니다.
- **세션.** `session/new`는 세션을 만들고, `session/resume`은 에이전트가 아는 세션을 이어 갑니다. 엔진에 체크포인트 저장소가 있으면, 권한 요청에서 멈춘 세션을 에이전트가 재시작된 뒤 `session/resume`으로 복원할 수 있습니다. 이때는 다음 프롬프트의 텍스트가 대기 중이던 질문에 대한 답이 됩니다. `initialize` 전의 호출은 `-32600`, 알 수 없는 세션은 `-32002`입니다.
- **MCP 서버.** 클라이언트는 `session/new`에 stdio MCP 서버를 넘길 수 있습니다. 에이전트는 NeoGraph의 MCP 클라이언트로 이 서버들을 세션의 `cwd`에서 최소한의 환경(`PATH`와 클라이언트의 `env` 항목)으로 시작하므로, 에이전트 환경에 있는 provider 키는 전달되지 않습니다. 노드는 `ACPClient::session_tools(session_id)`로 이 도구들에 접근합니다. HTTP·SSE MCP 서버는 `-32602`로 거절합니다. [MCP 가이드](mcp.md)를 보세요.
- **기능.** `ACPServer::capabilities()`가 `initialize`에서 알리는 내용입니다. 프롬프트는 텍스트와 `resource_link` 블록을 받으며, 이미지, 오디오, 임베디드 리소스는 `capabilities().prompt`에서 켠 경우에만 받습니다.

## 원격 전송(실험적)

stdio는 ACP가 정의하고 모든 에디터가 구현하는 전송입니다. 원격 전송, 곧 같은 엔드포인트에서 WebSocket 업그레이드를 함께 쓰는 Streamable HTTP는 ACP 프로젝트의 초안 제안으로만 있습니다. `neograph::acp_http_server`는 에이전트를 이 두 전송으로 제공하며 초안이 분명히 정한 부분은 따르므로, 초안이 바뀌면 와이어 세부가 바뀔 수 있습니다. **실험적이며 기본값은 꺼짐**입니다. 요청하지 않으면 아무것도 링크하지 않는 별도 라이브러리(`NEOGRAPH_BUILD_ACP_HTTP_SERVER`, 기본값 꺼짐)이고, 서버는 `start()` 뒤에야 연결을 받습니다. stdio의 `ACPServer::run()`은 바뀌지 않습니다.

```cpp
#include <neograph/acp/http_server.h>

// One ACPServer per connection: the factory builds it, the transport drives it.
acp::ACPHttpServer server([engine](std::string_view /*principal*/) {
    acp::ACPHttpConnection connection;
    connection.server = std::make_unique<acp::ACPServer>(
        engine, json{{"name", "release-notes"}, {"version", "1.0.0"}});
    return connection;
});
server.start_async();  // 127.0.0.1 and a free port, see server.port()
```

`neograph::acp`를 함께 가져오는 `neograph::acp_http_server`를 링크하세요. 팩토리는 연결마다 한 번, 선택 사항인 `authenticator`가 돌려준 주체(principal)와 함께 호출됩니다(없으면 빈 문자열).

- **연결.** ACP 연결 하나는 `ACPServer` 하나이며 stdio와 똑같이 구동됩니다. 메시지는 도착한 순서대로 처리되고, 끝난 연결은 입력의 끝이므로 실행 중인 프롬프트가 취소됩니다. 연결은 서로 독립이며 같은 엔진에서 프롬프트를 동시에 실행할 수 있고, `max_connections`(16)가 개수를 제한합니다. 세션 ID는 살아 있는 연결 하나에 속합니다. 다른 살아 있는 연결이 가진 ID를 `session/resume`하면 `-32603`과 `data.reason` `"session_busy"`로 거절됩니다. 연결의 세션은 연결과 함께 끝나며, 다시 연결한 뒤에는 `session/resume`이 엔진의 체크포인트 저장소에서 세션을 복원합니다. 엔진에서 프롬프트가 실행되는 동안에는(어느 연결에서든) 이 연결이 만들지 않은 세션의 `session/resume`이 최대 2초 기다리며, 그동안 이 연결은 다른 메시지를 읽지 않습니다. 그 뒤에도 실행 중이면 `-32603`과 `data.reason` `"engine_busy"`로 답하니 다시 시도하세요. 에디터를 호출하는 노드는 `ACPHttpServer::client()`를 캡처하거나(또는 연결되지 않은 `ACPClient`를 `attach_client()`에 넘기거나) 하며, 각 호출은 그 세션을 가진 연결에 도달합니다.
- **Streamable HTTP**는 `ACPHttpConfig::endpoint`(`/acp`)에서 제공됩니다. `Acp-Connection-Id` 없이 `initialize`를 `POST`하면 연결이 만들어지고, `200`으로 답하며 응답이 본문이 되고 ID는 `Acp-Connection-Id` 헤더에 들어갑니다. 그 밖의 `POST`는 이 헤더를 싣고, 본문 없이 `202`로 답하며, 실제 답은 스트림으로 옵니다. `Accept: text/event-stream`인 `GET`은 server-sent-event 스트림을 엽니다. 헤더만 있으면 연결 스트림(`session/new`와 `session/resume`의 답)이고, `Acp-Session-Id`까지 있으면 세션 스트림(`session/update`, `session/request_permission`·`fs/*` 같은 에이전트의 요청, 그 세션 요청의 답)입니다. `session/resume`으로 되살린 세션은 클라이언트가 스트림을 열 때까지 자기 스트림이 없으며, 그동안 그 메시지는 연결 스트림으로 갑니다. `DELETE`는 연결을 끝냅니다. `params.sessionId`로 세션을 지목하는 요청은 같은 `Acp-Session-Id`를 보내야 하고, 그 세션은 연결의 것이어야 합니다(`session/resume`은 예외로, 세션을 처음 들여옵니다). 배치는 `501`입니다. 열린 스트림이 없을 때의 메시지는 크기가 제한된 큐에서 기다리며, 연결을 잃은 뒤에는 아무것도 다시 보내지 않습니다. 스트림도 요청도 `connection_idle_timeout`(60초) 동안 없는 연결은 닫히고 그 프롬프트는 취소됩니다.
- **WebSocket**은 같은 엔드포인트(`Upgrade: websocket`인 `GET`)에서 제공됩니다. 텍스트 프레임 하나가 양방향 JSON-RPC 메시지 하나이고 `initialize`가 먼저 옵니다. 바이너리 프레임은 무시됩니다. 프레임은 RFC 6455를 따릅니다. 클라이언트 프레임은 마스킹되어야 하고, ping에는 응답하며, 메시지는 조각으로 나뉠 수 있습니다. `max_payload_bytes`를 넘는 메시지는 코드 1009로 연결을 닫고, 소켓을 닫으면 연결이 끝납니다.
- **초안과 다른 점.** 초안은 HTTP/2를 요구하지만 이 서버는 HTTP/1.1을 쓰므로 스트림과 `POST`마다 별도의 TCP 연결이 되며, 다중화는 리버스 프록시가 맡아야 합니다. WebSocket 업그레이드의 101 응답에는 `Acp-Connection-Id`가 없습니다. 쿠키를 설정하지 않고, `Acp-Protocol-Version`을 검사하지 않으며, 스트림에는 이벤트 ID도 재개도 없습니다. `initialize` 응답은 연결 ID를 헤더로만 주고 본문에 `connectionId`로 넣지 않습니다. `ACPServer`에는 `session/load`가 없으므로 초안의 재연결 흐름은 `session/resume`이 맡고, 세션 스트림은 `session/new`나 `session/resume`이 답한 뒤에야 열 수 있습니다. 클라이언트가 에이전트의 요청에 답하는 `POST`에서는 `Acp-Session-Id` 헤더가 선택입니다. 같은 범위의 두 번째 스트림은 첫 번째를 대체합니다.
- **보안.** 다른 호스트를 지정하지 않으면 `127.0.0.1`에 바인드하며, 루프백이 아닌 바인드는 `authenticator`가 없으면 거절됩니다. 모든 요청, 스트림, 업그레이드와 서버가 제공하지 않는 경로까지 본문을 읽기 전에 검사합니다. 먼저 Host와 Origin(`allowed_hosts`, `allowed_origins`. [MCP](mcp.md)·[A2A](a2a.md) 서버와 같은 규칙), 다음에 `authenticator`(HTTP 401과 `WWW-Authenticate: Bearer`), 마지막에 `max_payload_bytes`(HTTP 413, 기본 4 MiB)입니다. 연결은 그것을 만든 주체에게만 응답합니다. 클라이언트는 서버가 프로세스를 띄우게 할 수 없습니다. `mcpServers`를 담은 `session/new`나 `session/resume`은 `allow_client_mcp_servers`를 켜지 않는 한 `-32602`로 거절되며(ACP는 에이전트가 클라이언트가 나열한 stdio MCP 서버를 띄우기를 기대하지만, 여기서는 그 서버가 클라이언트가 아니라 서버 호스트에서 실행됩니다), 클라이언트가 지정한 `cwd`는 `_acp_cwd`로 그래프에 전달되므로 서버에서 그것을 쓰는 노드가 직접 검사해야 합니다. 에이전트를 기다리는 메시지의 양은 제한됩니다(`max_payload_bytes`의 두 배, 최소 1 MiB). 이를 넘는 `POST`는 `Retry-After`가 붙은 `503`으로, WebSocket 메시지는 JSON-RPC 오류로 답합니다. 느리거나 놀고 있는 TCP 연결, 그리고 한 주체가 쓸 수 있는 `max_connections`와 스트림 자리의 몫은 상대별로 제한하지 않으니, 인터넷에 노출하는 서버는 이를 제한하는 리버스 프록시 뒤에 두세요. 에이전트는 에디터를 대신해 파일을 읽고 쓰므로 원격 바인드는 그것을 노출하는 것으로 보세요. 브라우저는 WebSocket에 `Authorization`을 붙일 수 없으니, 그런 연결의 인증은 리버스 프록시에서 하세요.

## 테스트와 상호 운용성

`tests/fixtures/acp_probe_agent.cpp`는 공개 API로 만든 작은 ACP 에이전트입니다. 상호 운용 테스트 모음은 공식 SDK 클라이언트를 이 에이전트에 붙여 검사합니다. 단위 테스트는 `tests/test_acp_server.cpp`와 `tests/test_acp_types.cpp`이고, 원격 전송에는 별도의 `tests/test_acp_http_server.cpp`가 있습니다. `bindings/python/tests/interop/acp_suite/test_acp_sdk_remote_interop.py`는 공식 Python SDK(`agent-client-protocol` 0.12)가 초안 전송용으로 제공하는 클라이언트를 `neograph_acp_probe_agent --http`에 두 전송 모두로 붙여 실행하며, TypeScript SDK는 이 전송들에 대해 실행하지 않습니다.
