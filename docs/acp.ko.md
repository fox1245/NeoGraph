<!-- neograph-i18n: source=docs/acp.md locale=ko source_sha256=3617734aebc1bcbee95c4407fb07e702416c58627b2137e686d6c9ff0571b8b8 -->
# ACP: 그래프를 에디터 에이전트로 쓰기

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

Agent Client Protocol(ACP)은 에디터를 코딩 에이전트나 대화 에이전트에 연결합니다. 에디터(Zed, CodeCompanion을 쓰는 Neovim, Gemini CLI 등)는 에이전트를 자식 프로세스로 실행하고, 에이전트의 stdin과 stdout으로 JSON-RPC 2.0 메시지를 한 줄에 JSON 객체 하나씩 주고받습니다. 에디터가 세션을 열고 프롬프트를 보내면, 에이전트는 답을 업데이트로 스트리밍합니다. 에이전트가 에디터에 권한이나 파일, 터미널 접근을 요청할 수도 있습니다.

`ACPServer`는 컴파일된 NeoGraph 그래프를 이런 에이전트로 실행합니다. NeoGraph는 에이전트 쪽을 구현하며, 에디터가 클라이언트입니다. `ACPClient`는 에이전트가 에디터에 자신의 요청을 보내는 데 쓰는 핸들입니다.

## 구성 요소

| 역할 | C++ 타깃 | CMake 옵션(기본값) | Python |
|---|---|---|---|
| 에이전트(서버) | `neograph::acp` | `NEOGRAPH_BUILD_ACP`(켜짐) | 공식 SDK 사용 |

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
- **턴이 끝나는 방식.** 실행이 완료되면 `end_turn`, 취소되면 `cancelled`, 실행이 `RunConfig::max_steps`에 이르면 `max_turn_requests`, 모델 토큰 예산을 넘으면 `max_tokens`, provider가 거절하면 `refusal`입니다. 그 밖의 실패는 `data`가 `{code, retryable}`인 JSON-RPC 오류 `-32603`이며, 메시지는 실패의 공개 메시지이고 예외 텍스트는 아닙니다. 전체 표는 레퍼런스의 [ACP](reference-en.md#neographacp--agent-client-protocol)와 [프로토콜 실패 매핑](reference-en.md#protocol-failure-mapping)에 있습니다.
- **세션.** `session/new`는 세션을 만들고, `session/resume`은 에이전트가 아는 세션을 이어 갑니다. 엔진에 체크포인트 저장소가 있으면, 권한 요청에서 멈춘 세션을 에이전트가 재시작된 뒤 `session/resume`으로 복원할 수 있습니다. 이때는 다음 프롬프트의 텍스트가 대기 중이던 질문에 대한 답이 됩니다. `initialize` 전의 호출은 `-32600`, 알 수 없는 세션은 `-32002`입니다.
- **MCP 서버.** 클라이언트는 `session/new`에 stdio MCP 서버를 넘길 수 있습니다. 에이전트는 NeoGraph의 MCP 클라이언트로 이 서버들을 세션의 `cwd`에서 최소한의 환경(`PATH`와 클라이언트의 `env` 항목)으로 시작하므로, 에이전트 환경에 있는 provider 키는 전달되지 않습니다. 노드는 `ACPClient::session_tools(session_id)`로 이 도구들에 접근합니다. HTTP·SSE MCP 서버는 `-32602`로 거절합니다. [MCP 가이드](mcp.md)를 보세요.
- **기능.** `ACPServer::capabilities()`가 `initialize`에서 알리는 내용입니다. 프롬프트는 텍스트와 `resource_link` 블록을 받으며, 이미지, 오디오, 임베디드 리소스는 `capabilities().prompt`에서 켠 경우에만 받습니다.

## 테스트와 상호 운용성

`tests/fixtures/acp_probe_agent.cpp`는 공개 API로 만든 작은 ACP 에이전트입니다. 상호 운용 테스트 모음은 공식 SDK 클라이언트를 이 에이전트에 붙여 검사합니다. 단위 테스트는 `tests/test_acp_server.cpp`와 `tests/test_acp_types.cpp`입니다.
