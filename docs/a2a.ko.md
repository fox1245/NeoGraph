<!-- neograph-i18n: source=docs/a2a.md locale=ko source_sha256=01490e5a0a068a4082b91e0571f88dbf747cccf6164cd0b1882eedfca6401b57 -->
# A2A: 에이전트끼리 호출하기

**Languages:** [English](a2a.md) | [한국어](a2a.ko.md) | [日本語](a2a.ja.md) | [简体中文](a2a.zh-CN.md)

Agent2Agent(A2A) 프로토콜은 한 에이전트가 다른 에이전트를 찾아 일을 맡길 수 있게 합니다. 서버는 `/.well-known/agent-card.json`에 에이전트 카드를 공개해 자신이 누구이고 무엇을 할 수 있는지 알립니다. 클라이언트는 HTTP 위의 JSON-RPC로 메시지를 보내고, 서버는 그 작업을 태스크로 추적합니다. 태스크는 working, input-required, completed, canceled, failed 같은 상태를 거칩니다. 클라이언트는 태스크를 Server-Sent Events 스트림으로 따라갈 수도 있습니다.

NeoGraph는 양쪽을 모두 지원합니다. `A2AServer`는 컴파일된 그래프를 에이전트로 공개합니다. `A2AClient`는 원격 에이전트를 호출하고, `A2ACallerNode`는 원격 에이전트를 그래프의 노드로 만듭니다.

## 구성 요소

| 역할 | C++ 타깃 | CMake 옵션(기본값) | Python |
|---|---|---|---|
| 서버와 클라이언트 | `neograph::a2a` | `NEOGRAPH_BUILD_A2A`(켜짐) | `neograph_engine.a2a`의 클라이언트 |

Python 패키지에는 클라이언트만 있습니다. Python에서 에이전트를 서비스하려면 공식 A2A Python SDK(`pip install "neograph-engine[a2a]"`로 설치됨)를 쓰고, 그 뒤에서 `neograph_engine.ProtocolHostAdapter`로 엔진을 실행하세요. 이 어댑터는 A2A 컨텍스트 ID를 NeoGraph `thread_id`에 대응시키고, 체크포인트에서 재개하고, 취소를 전달하고, 토큰을 스트리밍하고, 그래프 인터럽트를 `input-required` 태스크로 바꿉니다([Python에서 호스팅할 때의 인터럽트](#interrupts-when-you-host-from-python) 참고). [Python 예제](../bindings/python/examples/README.md)에 이런 서버가 있습니다.

## 버전

A2A 1.0(v1.0.1)이 기본 버전이고, 0.3은 같은 의미를 가진 호환 방언으로 유지합니다.

- **서버.** 요청마다 `A2A-Version` 헤더나 `A2A-Version` 쿼리 매개변수가 가리키는 방언으로 응답합니다. 버전이 없는 요청은 0.3으로 처리합니다. `1.0`과 `0.3` 외의 버전은 지원 버전을 알려 주는 오류 `-32009`를 받습니다. 두 메서드 표기(`SendMessage`와 `message/send`)는 어느 버전에서나 동작합니다. 에이전트 카드는 버전별로 제공합니다. `A2A-Version: 1.0`이면 순수한 1.0 카드를, 헤더가 없으면 0.3 카드를 받으며, 두 카드 모두 1.0과 0.3 인터페이스를 나열합니다.
- **클라이언트.** 첫 호출 전에 카드를 가져와, 버전이 `1.` 또는 `0.`으로 시작하는 첫 JSON-RPC 인터페이스를 씁니다. 카드를 가져올 수 없을 때만 탐색하며, 1.0을 먼저 시도하고 그다음 0.3을 시도합니다. `wire_dialect()`가 그 결과를 알려 줍니다. 카드가 클라이언트를 다른 출처로 보낼 수는 없으며, `set_allow_cross_origin_interfaces(true)`를 호출해야 가능합니다.

공식 SDK는 CI(`.github/workflows/protocol-interop.yml`, 테스트 모음은 `bindings/python/tests/interop/a2a_suite`)에서 양방향으로 시험합니다. 대상은 Python용 `a2a-sdk` 1.2.2와 `@a2a-js/sdk` 1.3.0, 0.3.14입니다.

## 그래프를 에이전트로 공개하기

이 프로그램은 노드 두 개짜리 그래프를 공개합니다. `draft`는 프롬프트로 릴리스 노트를 쓰고, `publish`는 사람이 승인한 뒤에만 실행됩니다. API 키는 필요 없습니다.

```cpp
#include <neograph/a2a/server.h>
#include <neograph/neograph.h>

#include <iostream>
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

    // The A2A server reads the user's text from the "prompt" channel and the
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

    // A checkpoint store is what lets the task wait for an answer and resume.
    EngineConfig config;
    config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    std::shared_ptr<GraphEngine> engine = GraphEngine::build_strict(definition, std::move(config));

    a2a::AgentCard card;
    card.name = "release-notes";
    card.description = "Drafts release notes and publishes them after approval.";
    card.version = "1.0.0";
    card.default_input_modes = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"release-notes"};

    a2a::A2AServer server(engine, card);
    std::cout << "A2A agent on http://127.0.0.1:8087\n" << std::flush;
    server.start("127.0.0.1", 8087);  // blocks until server.stop()
    return 0;
}
```

동작 방식은 다음과 같습니다.

- 서버는 사용자 메시지의 텍스트를 그래프의 `prompt` 채널에 넣고, 에이전트의 답을 `response` 채널에서 읽습니다. 이를 바꾸려면 `GraphAgentAdapter`에서 `input_channel()`, `output_channel()`, `build_initial_state()`, `build_output_artifact()`(아티팩트를 돌려줄 때), `build_resume_value()` 중 필요한 것을 재정의하고, 어댑터를 생성자의 세 번째 인자로 넘기세요.
- 기본 어댑터는 최종 답을 완료된 태스크의 상태 메시지에만 담습니다. 태스크에는 아티팩트가 없으므로 아티팩트를 읽는 클라이언트는 아무것도 받지 못합니다. 답을 아티팩트로 돌려주려면 `build_output_artifact()`를 재정의하세요. [예제 78](../examples/78_a2a_input_required.cpp)이 `response` 채널을 이런 방식으로 아티팩트로 만듭니다.
- 그래프 실행 하나가 태스크 하나이며, 태스크 ID는 그 실행의 `thread_id`입니다. 태스크마다 자신의 워커 스레드에서 실행되므로, 연결이 끊겨도 태스크는 계속됩니다. 서버는 동시에 태스크 32개까지 받으며, 그 이상은 JSON-RPC 오류 `-32603`(사유 `RESOURCE_EXHAUSTED`)을 받습니다.
- `interrupt_before`(또는 노드가 던진 `NodeInterrupt`)가 태스크를 `input-required` 상태로 만듭니다. 상태 메시지에는 사유가, `metadata["neograph/interrupt"]`에는 노드와 체크포인트가 담기며, 같은 태스크 ID로 후속 메시지를 보내면 그 체크포인트에서 실행을 재개합니다. 후속 메시지의 텍스트가 재개 값이 됩니다. 체크포인트 저장소가 없으면 후속 메시지를 입력으로 삼아 그래프를 처음부터 다시 실행하므로, 그래프가 멈출 수 있다면 엔진에 저장소를 지정하세요.
- 카드는 `GET /.well-known/agent-card.json`으로, JSON-RPC 엔드포인트는 `POST /`로 제공합니다. `AgentCard::url`이 없으면 카드는 클라이언트가 접속한 주소를 `http` 스킴으로 알립니다. TLS를 종료하는 리버스 프록시 뒤에서는 `AgentCard::url`을 공개 URL로 정하거나, `A2AServer::set_trust_forwarded_headers(true)`를 호출해 프록시가 보낸 `Forwarded`(RFC 7239) 또는 `X-Forwarded-Proto` / `X-Forwarded-Host` 헤더에서 스킴과 호스트를 가져오게 합니다. 이 헤더는 클라이언트가 직접 넣을 수 있으므로 기본값은 꺼짐이며, 직접 관리하는 프록시가 이 헤더를 덮어쓸 때만 켭니다. 루프백 주소에 바인드한 서버는 프록시의 공개 `Host`를 `A2AHttpConfig::allowed_hosts`에 나열했을 때만 받아들입니다(아래 항목 참조). `url` 없이 직렬화한 카드는 빈 문자열을 쓰지 않고 이 멤버를 생략합니다(선호 전송의 첫 인터페이스가 있으면 그 URL을 씁니다).
- 이 서버는 요청을 인증하지 않습니다. 인증기를 받는 것은 Program 기반 생성자뿐입니다. 루프백 주소에 바인드하거나, 인증하는 프록시를 앞에 두세요.
- **Host, Origin, 요청 크기.** 다른 출처의 웹 페이지는 자기 호스트 이름을 이 머신으로 다시 가리키게 하면 루프백 주소에 바인드한 서버에 닿을 수 있습니다(DNS 리바인딩). 그래서 루프백 바인드(`127.0.0.1`, `::1`, `localhost`)는 포트와 관계없이 `Host`가 `localhost`, `127.0.0.1`, `[::1]`이거나 `A2AHttpConfig::allowed_hosts`에 있는 이름일 때만 응답합니다. 다른 `Host`는 `403`, 없거나 `host[:port]` 형태가 아닌 `Host`는 `400`입니다. `Origin`이 있는 요청은 그 출처가 `allowed_origins`에 있거나, 루프백 바인드에서 서버 자신의 출처(`http://localhost:<port>`, `http://127.0.0.1:<port>`, `http://[::1]:<port>`)가 아니면 `403`입니다(각 출처를 처음 받아들일 때 stderr에 한 줄 씁니다). 더 넣으려면 `start()` 전에 `A2AServer::set_http_config()`를 호출하세요. `allowed_hosts`의 항목은 `agent.example.com`이나 `[::1]`처럼 포트 없는 이름이고 대소문자를 구분하지 않고 비교하며, `*`는 어떤 `Host`든 받아들입니다. 루프백이 아닌 바인드에서는 `allowed_hosts`가 비어 있으면 `Host`를 확인하지 않고, 비어 있지 않으면 나열한 이름만 받아들입니다. 확인은 모든 경로(카드, 이벤트 스트림을 포함한 JSON-RPC 엔드포인트, 그 밖의 모든 경로)에 적용되며 본문을 읽기 전에 실행됩니다. `max_payload_bytes`를 넘는 본문은 `413`을 받습니다. 기본값은 4 MiB로, 긴 메시지나 3 MiB가 조금 안 되는 인라인 파일 하나가 들어갑니다(더 큰 파일은 `uri` 파트로 보냅니다). 공개 `Host`를 그대로 전달하는 로컬 리버스 프록시나 터널 뒤에서는 그 이름(또는 `*`)을 목록에 넣으세요.

`neograph::core`와 `neograph::a2a`를 링크합니다.

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::a2a)
```

## 에이전트 호출하기

위 프로그램을 시작한 뒤, 이 클라이언트로 접속합니다. 에이전트를 찾아 메시지를 보내고, 태스크가 입력을 기다리는 것을 확인한 다음 답합니다.

```cpp
#include <neograph/a2a/client.h>
#include <neograph/neograph.h>

#include <iostream>
#include <string>

using namespace neograph;

namespace {

void print(const char* label, const a2a::Task& task) {
    std::cout << label << ": " << a2a::task_state_to_string(task.status.state);
    if (task.status.message && !task.status.message->parts.empty())
        std::cout << " - " << task.status.message->parts[0].text;
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    a2a::A2AClient client(argc > 1 ? argv[1] : "http://127.0.0.1:8087");

    const auto card = client.fetch_agent_card();  // GET /.well-known/agent-card.json
    std::cout << "agent: " << card.name << '\n';

    // The graph stops before "publish", so the task waits for input.
    auto task = client.send_message_sync("version 2.0");
    print("first", task);

    // Answer on the same task. The server resumes the run from its checkpoint.
    task = client.send_message_sync("approved", task.id, task.context_id);
    print("second", task);
    return 0;
}
```

Python도 같습니다.

```python
from neograph_engine import a2a

agent = a2a.A2AClient("http://127.0.0.1:8087")
print(agent.fetch_agent_card().name)

task = agent.send_message("version 2.0")  # returns when the task finishes or needs input
print(task.state)
task = agent.send_message("approved", task.id, task.context_id)  # answer on the same task
print(task.state)
```

C++ 클라이언트는 다음을 출력합니다.

```text
agent: release-notes
first: input-required - Interrupt before node: publish
second: completed - Published: Release notes: version 2.0
```

Python 스크립트는 다음을 출력합니다.

```text
release-notes
TaskState.InputRequired
TaskState.Completed
```

클라이언트의 다른 호출은 다음과 같습니다. `send_message_stream(text, on_event)`은 상태와 아티팩트 업데이트를 도착하는 대로 전달하고 조립된 태스크를 돌려줍니다. `get_task(id)`는 태스크를 읽고, `cancel_task(id)`는 입력을 기다리는 태스크를 포함해 태스크를 취소합니다. 이미 종료된 태스크에 메시지를 보내면 `-32004`로 실패합니다. 이 Python 클라이언트는 0.14.0 휠에도 있으며, 위 Python 스크립트는 휠로도 이 서버와 동작합니다.

응답은 `Content-Type`이 `text/event-stream`일 때만 서버 전송 이벤트로 읽고, 그때 JSON-RPC 메시지는 첫 `data:` 이벤트입니다. 그 외에는 JSON으로 읽습니다. 응답에 담긴 텍스트는 `data: `가 들어 있어도 판단에 쓰이지 않습니다.

호출에는 자체 기한이 없습니다. `send_message_sync`는 에이전트가 답할 때까지, 스트림은 에이전트가 끝낼 때까지 기다립니다. 작업이 몇 분 걸릴 수 있기 때문입니다. 연결에는 한도가 있습니다. 이름 해석, TCP 연결, TLS 핸드셰이크를 합쳐 30초이며(이미 진행 중인 이름 조회는 중단할 수 없어 이를 넘길 수 있습니다) `set_connect_timeout(seconds)`로 바꿀 수 있습니다. `set_timeout(seconds)`는 각 요청의 교환 전체(응답이나 스트림을 끝까지 읽는 일 포함)를 제한하며, 넘으면 호출이 예외를 던집니다. 둘 다 0이면 한도를 없앱니다. Python의 `set_timeout`은 초를 받아 정수 초로 잘라 쓰므로 1 미만 값은 한도를 없앱니다. Python에는 `set_connect_timeout`이 없습니다. 호출자가 `on_event`에서 `false`를 반환해 끝낸 스트림은 끝까지 읽지 않고 즉시 닫습니다. 클라이언트가 기한을 두지 않는 호출도 그 호출이 속한 작업과 함께 끝납니다. `A2ACallerNode`의 호출은 실행의 cancel token이나 노드의 `attempt_timeout_ms`로, `make_harness_capability_executor`가 한 호출은 Harness 작업의 cancel token이나 `call_timeout`(기본 30초, 0이면 없음)으로 끝나며, `send_message_sync`는 그런 token을 마지막 인자로 받습니다.

## 원격 에이전트를 노드로 쓰기

`A2ACallerNode`는 채널 하나를 읽어 그 텍스트를 원격 에이전트에 보내고, 응답을 다른 채널에 씁니다. 여러 노드가 클라이언트 하나를 공유할 수 있으며, 그 호출들은 동시에 실행됩니다.

```cpp
#include <neograph/a2a/a2a_caller_node.h>
#include <neograph/a2a/client.h>
#include <neograph/neograph.h>

#include <iostream>
#include <memory>

using namespace neograph;
using namespace neograph::graph;

int main(int argc, char** argv) {
    auto client = std::make_shared<a2a::A2AClient>(argc > 1 ? argv[1] : "http://127.0.0.1:8087");

    // The node reads "prompt", sends it to the remote agent and writes the
    // agent's text to "response". It also writes "response_task_id" while the
    // task waits for input, and "response_context_id".
    NodeFactory::instance().register_type(
        "docs.remote_agent", [client](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<a2a::A2ACallerNode>(name, client, "prompt", "response");
        });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "ask-remote-agent"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"response", {{"reducer", "overwrite"}}},
                      {"response_task_id", {{"reducer", "overwrite"}}},
                      {"response_context_id", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"ask", {{"type", "docs.remote_agent"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "ask"}},
                   {{"from", "ask"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig run;
    run.input = {{"prompt", "version 2.0"}};
    const auto result = engine->run(run);
    std::cout << "response: " << result.channel<std::string>("response") << '\n'
              << "waiting task: " << result.channel<std::string>("response_task_id") << '\n';
    return 0;
}
```

기본 키를 쓰면 `response`, `response_context_id`(대화를 이어 가기 위한 값), `response_task_id`(태스크가 입력을 기다리는 동안에만 채워지며, 다음 호출이 그 태스크를 재개함)에 씁니다. 위 프로그램은 에이전트의 상태 텍스트와 이 태스크 ID를 출력합니다.

## 인터럽트, 취소, 오류

- **인터럽트.** 멈춘 그래프는 위에서 본 것처럼 `input-required` 태스크입니다. 태스크가 기다리는 동안 활성 실행은 없고, 그 상태는 엔진의 체크포인트 저장소에 있습니다.
- **취소.** `CancelTask`는 실행의 토큰을 취소하고 태스크는 `canceled`로 끝납니다. 이미 종료된 태스크를 취소하면 `-32002`입니다.
- **실패.** 실패한 실행은 `failed` 태스크이며, 상태 메시지는 실패의 공개 메시지, `metadata["neograph/failure"]`는 `{code, retryable, retryable_by_class}`입니다. `retryable`은 엔진이 노드의 시도를 다 쓰면 `false`가 되고, `retryable_by_class`는 실패의 분류가 재시도를 허용하는지를 알려 줍니다. 예외 텍스트는 네트워크로 나가지 않습니다. 어떤 노드도 끝나기 전에 Hook이 막으면 `rejected`가 됩니다. `max_steps`에 도달한 그래프는 코드 `NG.StepLimit`으로 실패합니다.
- **요청 오류**(알 수 없는 태스크 `-32001`, 종료된 태스크에 메시지 `-32004`, 태스크의 것과 다른 `contextId` `-32602`, 아직 실행 중인 태스크 `-32603`과 사유 `TASK_BUSY`)는 레퍼런스의 [A2A 오류 매핑](reference-en.md#a2a-error-mapping)에, 모든 프로토콜의 실패 분류는 [프로토콜 실패 매핑](reference-en.md#protocol-failure-mapping)에 있습니다. 여기에 다시 옮기지 않았습니다.
- **Program.** Program 기반 에이전트(`A2AServer` 뒤의 승인된 `ProgramVersion`)는 NeoGraph 협업 봉투(collaboration envelope)도 받습니다. [A2A 협업 확장](reference-en.md#a2a-collaboration-extension)을 보세요. 입력을 기다리는 Program 태스크에는 아직 A2A로 답할 수 없습니다. 메시지는 `-32004`를 받고 태스크는 계속 기다립니다. 서버를 다시 시작하면 `CollaborationMailbox`에서 Program 태스크를 복구합니다. 복구한 태스크가 아직 실행 중일 때 `SubscribeToTask`를 호출하면 Task를 먼저 보낸 다음, 영속 run이 끝날 때까지 따라가서 최종 상태로 마칩니다. run을 따라갈 수 없으면 빈 채로 닫지 않고 오류 이벤트로 마칩니다.

<a id="interrupts-when-you-host-from-python"></a>
## Python에서 호스팅할 때의 인터럽트

`ProtocolHostAdapter`는 멈추는 그래프(`interrupt_before`, `NodeInterrupt`, 도구 게이트)를 네이티브 서버와 같은 모양으로 만듭니다. 후속 메시지가 재개하는 `input-required` 태스크입니다. [예제 27](../bindings/python/examples/27_a2a_server.py)이 그 실행기입니다.

- **질문.** `adapter.stream(text, thread_id=..., request_id=..., interrupts=True)`는 그래프가 멈추면 최종 이벤트 대신 `interrupt` 이벤트로 끝납니다. 이 이벤트의 `ProtocolInterrupt`에는 `reason`, `payload`(노드가 `NodeInterrupt`에 넘긴 값), `node`, `checkpoint_id`가 있습니다. 실행기는 `TASK_STATE_INPUT_REQUIRED`로 가는 상태 업데이트를 게시합니다. 그 메시지에는 이유가 텍스트로, 페이로드가 데이터 파트로 들어 있고, `metadata["neograph/interrupt"]`에는 노드, 체크포인트, 인터럽트 ID가 들어 있습니다. 네이티브 서버와 같습니다.
- **답.** 그 태스크로 오는 다음 메시지는 태스크가 아직 `input-required`인 채로 `execute()`에 도착합니다. 실행기는 그 텍스트를 태스크 자신의 메타데이터가 가리키는 멈춤과 함께 `adapter.resume_stream(text, thread_id=..., request_id=..., interrupt=...)`에 넘깁니다. 이 호출은 실행이 멈춘 체크포인트에서 재개하고, 나머지 턴을 다른 턴과 똑같이 스트리밍합니다. 태스크는 `completed`로 끝나며, 그래프가 또 물으면 다시 `input-required`가 됩니다. 이 텍스트가 멈춘 노드가 `input.ctx.resume_value`로 읽는 값이고, 네이티브 서버도 마찬가지입니다. 노드가 다른 모양(예를 들어 dict)을 기대하면 `ProtocolHostAdapter`에 `resume_value=lambda answer, interrupt: ...`를 넘기세요.
- **상태.** 어댑터는 멈춤을 스레드별로 메모리에 기억하며, 엔진에는 이를 재개하기 위한 체크포인트 저장소가 필요합니다. 예제는 태스크 자신의 메타데이터가 가리키는 멈춤에서 그 태스크에 답합니다. `metadata["neograph/interrupt"]`로 `ProtocolInterrupt(thread_id=..., checkpoint_id=..., interrupt_id=...)`를 다시 만들어 `interrupt=`로 넘기므로, 재시작한 뒤에도 동작합니다. 기억된 멈춤도 없고 `interrupt=`도 없으면 `resume_stream()`은 `LookupError`를 던집니다. 멈추지 않은 스레드를 재개하면 마지막 턴의 텍스트가 돌아오기 때문입니다. 태스크가 취소되면 `adapter.discard_interrupt(thread_id)`를 호출하세요. 기다리는 태스크에 대한 `CancelTask`는 위에서 설명한 대로 태스크를 `canceled`로 끝냅니다. 어댑터는 스레드를 A2A 컨텍스트로 구분하며, 네이티브 서버는 태스크로 구분합니다. 같은 컨텍스트의 새 태스크는 앞선 태스크들이 남긴 상태에서 시작합니다. 어댑터는 스레드마다 질문을 하나만 기억하므로 더 새로운 멈춤이 앞선 멈춤의 기록을 대체하고, 그래서 예제는 멈춤을 어댑터가 아니라 태스크에서 가져옵니다. 아무도 답하지 않은 멈춤은 체크포인트 저장소에 열린 채로 남으며, 엔진은 그 바로 뒤에 오는 멈춤을 재개하지 못합니다(`NodeInterrupt ancestry crosses super-step boundaries`). 기다리는 태스크에 `CancelTask`를 보낸 뒤나, 다른 태스크가 기다리는 동안 새 태스크가 멈춘 경우, 그 컨텍스트의 다음 멈춤은 답할 때 실패하며 컨텍스트의 어떤 턴이 완료될 때까지 이어집니다.
- **`interrupts=True` 없이.** 그래프가 멈추면 `stream()`, `run()`, `run_payload()`는 `ProtocolInterrupted`를 던집니다. 예전에는 상태의 마지막 메시지, 곧 사용자 자신의 텍스트를 에이전트의 답처럼 돌려주었습니다.
- **다루지 않는 것.** NeoGraph 협업 봉투와 Program 기반 태스크는 네이티브 서버의 기능입니다. SDK는 표준 메서드만 제공합니다.

## 테스트와 상호 운용성

`tests/fixtures/a2a_probe_server.cpp`는 공개 API로 만든 작은 A2A 서버입니다. 상호 운용 테스트 모음은 공식 SDK 클라이언트를 이 서버에 붙이고, NeoGraph 클라이언트를 SDK 서버에 붙여 검사합니다. 단위 테스트는 `tests/test_a2a_server.cpp`, `tests/test_a2a_client.cpp`, `tests/test_a2a_conformance.cpp`입니다.
