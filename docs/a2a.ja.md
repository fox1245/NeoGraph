<!-- neograph-i18n: source=docs/a2a.md locale=ja source_sha256=0c5c60196bee90e5e6009b6fc8ca7255e2b3365504528791a3a4adb985af8732 -->
# A2A: エージェントがエージェントを呼び出す

**Languages:** [English](a2a.md) | [한국어](a2a.ko.md) | [日本語](a2a.ja.md) | [简体中文](a2a.zh-CN.md)

Agent2Agent(A2A)プロトコルを使うと、あるエージェントが別のエージェントを見つけて仕事を任せられます。サーバーは `/.well-known/agent-card.json` にエージェントカードを公開し、自分が誰で何ができるかを伝えます。クライアントは HTTP 上の JSON-RPC でメッセージを送り、サーバーはその仕事をタスクとして追跡します。タスクは working、input-required、completed、canceled、failed などの状態を移り変わります。クライアントは、タスクを Server-Sent Events のストリームとして追うこともできます。

NeoGraph は両側に対応しています。`A2AServer` はコンパイル済みのグラフをエージェントとして公開します。`A2AClient` はリモートのエージェントを呼び出し、`A2ACallerNode` はリモートのエージェントをグラフのノードにします。

## コンポーネント

| 役割 | C++ ターゲット | CMake オプション(既定値) | Python |
|---|---|---|---|
| サーバーとクライアント | `neograph::a2a` | `NEOGRAPH_BUILD_A2A`(オン) | `neograph_engine.a2a` のクライアント |

Python パッケージにあるのはクライアントだけです。Python でエージェントを提供するには、公式の A2A Python SDK(`pip install "neograph-engine[a2a]"` で入ります)を使い、その背後で `neograph_engine.ProtocolHostAdapter` を使ってエンジンを動かします。このアダプターは、A2A のコンテキスト ID を NeoGraph の `thread_id` に対応づけ、チェックポイントから再開し、キャンセルを伝え、トークンをストリーミングします。[Python サンプル](../bindings/python/examples/README.md)にそのようなサーバーがあります。

## バージョン

A2A 1.0(v1.0.1)が主となるバージョンで、0.3 は同じ意味を持つ互換用の方言として残してあります。

- **サーバー。** リクエストごとに、`A2A-Version` ヘッダーまたは `A2A-Version` クエリパラメーターが示す方言で応答します。バージョンのないリクエストは 0.3 として扱います。`1.0` と `0.3` 以外のバージョンには、対応バージョンを列挙したエラー `-32009` を返します。2つのメソッド表記(`SendMessage` と `message/send`)は、どちらのバージョンでも使えます。エージェントカードはバージョンごとに提供します。`A2A-Version: 1.0` なら純粋な 1.0 のカード、ヘッダーがなければ 0.3 のカードが返り、どちらのカードも 1.0 と 0.3 のインターフェースを列挙します。
- **クライアント。** 最初の呼び出しの前にカードを取得し、バージョンが `1.` か `0.` で始まる最初の JSON-RPC インターフェースを使います。カードを取得できないときだけ探索し、1.0 を先に、次に 0.3 を試します。結果は `wire_dialect()` で分かります。カードがクライアントを別のオリジンに向けることはできません。`set_allow_cross_origin_interfaces(true)` を呼んだ場合だけ可能です。

公式 SDK は CI(`.github/workflows/protocol-interop.yml`、スイートは `bindings/python/tests/interop/a2a_suite`)で双方向にテストしています。対象は Python 用の `a2a-sdk` 1.2.2 と、`@a2a-js/sdk` の 1.3.0 および 0.3.14 です。

## グラフをエージェントとして公開する

このプログラムは2ノードのグラフを公開します。`draft` はプロンプトからリリースノートを書き、`publish` は人が承認したあとにだけ実行されます。API キーは不要です。

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

仕組みは次のとおりです。

- サーバーは、ユーザーのメッセージのテキストをグラフの `prompt` チャネルに入れ、エージェントの答えを `response` チャネルから読みます。これを変えるには、`GraphAgentAdapter` の `input_channel()`、`output_channel()`、`build_initial_state()`、`build_output_artifact()`(アーティファクトを返すとき)、`build_resume_value()` のうち必要なものをオーバーライドし、アダプターをコンストラクターの3番目の引数に渡します。
- 既定のアダプターは、最終的な答えを完了したタスクのステータスメッセージにだけ入れます。タスクにアーティファクトはないので、アーティファクトを読むクライアントは何も受け取れません。答えをアーティファクトとして返すには、`build_output_artifact()` をオーバーライドします。[サンプル 78](../examples/78_a2a_input_required.cpp) は、`response` チャネルをこの方法でアーティファクトにしています。
- グラフの実行1回がタスク1つで、タスク ID はその実行の `thread_id` です。タスクはそれぞれ専用のワーカースレッドで動くので、接続が切れてもタスクは続きます。サーバーが同時に受け付けるタスクは32件までで、それを超えると JSON-RPC エラー `-32603`(理由 `RESOURCE_EXHAUSTED`)が返ります。
- `interrupt_before`(またはノードが投げた `NodeInterrupt`)は、タスクを `input-required` にします。ステータスメッセージには理由が、`metadata["neograph/interrupt"]` にはノードとチェックポイントが入り、同じタスク ID で続きのメッセージを送ると、そのチェックポイントから実行が再開されます。続きのメッセージのテキストが再開値になります。チェックポイントストアがなければ、続きのメッセージを入力にしてグラフを最初からやり直すので、グラフが停止する可能性があるなら、エンジンにストアを持たせてください。
- カードは `GET /.well-known/agent-card.json` で、JSON-RPC のエンドポイントは `POST /` で提供されます。`AgentCard::url` がなければ、カードはクライアントがアクセスしたアドレスを `http` スキームで示します。TLS を終端するリバースプロキシの後ろでは、`AgentCard::url` に公開 URL を設定するか、`A2AServer::set_trust_forwarded_headers(true)` を呼んで、プロキシが付けた `Forwarded` (RFC 7239) または `X-Forwarded-Proto` / `X-Forwarded-Host` ヘッダーからスキームとホストを取るようにします。これらのヘッダーはクライアント自身も付けられるため既定ではオフで、自分で管理するプロキシがこれらを上書きする場合にだけオンにします。ループバックアドレスにバインドしたサーバーは、プロキシの公開 `Host` を `A2AHttpConfig::allowed_hosts` に列挙したときだけ受け入れます (次の項目を参照)。`url` なしで直列化したカードは空文字列を書かず、このメンバーを省きます (優先トランスポートの最初のインターフェースがあればその URL を使います)。
- このサーバーはリクエストを認証しません。認証関数を受け取るのは、Program ベースのコンストラクターだけです。ループバックアドレスにバインドするか、認証を行うプロキシを前に置いてください。
- **Host、Origin、リクエストサイズ。** 別のオリジンの Web ページは、自分のホスト名をこのマシンに向け直すと、ループバックアドレスにバインドしたサーバーに届きます (DNS リバインディング)。そのためループバックへのバインド (`127.0.0.1`、`::1`、`localhost`) は、`Host` が `localhost`、`127.0.0.1`、`[::1]` (ポートは問いません) か `A2AHttpConfig::allowed_hosts` にある名前のときだけ応答します。それ以外の `Host` は `403`、ないか `host[:port]` の形式でない `Host` は `400` です。`Origin` を持つリクエストは、そのオリジンが `allowed_origins` にあるか、ループバックへのバインドでサーバー自身のオリジン (`http://localhost:<port>`、`http://127.0.0.1:<port>`、`http://[::1]:<port>`) でなければ `403` です (各オリジンを初めて受け入れたときに stderr に 1 行書きます)。追加するには `start()` の前に `A2AServer::set_http_config()` を呼びます。`allowed_hosts` の項目は `agent.example.com` や `[::1]` のようにポートのない名前で、大文字小文字を区別せずに比較し、`*` は任意の `Host` を受け入れます。ループバック以外へのバインドでは、`allowed_hosts` が空なら `Host` を確認せず、空でなければ列挙した名前だけを受け入れます。確認はすべてのパス (カード、イベントストリームを含む JSON-RPC エンドポイント、その他のパス) に適用され、本文を読む前に行われます。`max_payload_bytes` を超える本文は `413` です。既定は 4 MiB で、長いメッセージや 3 MiB に少し足りないインラインファイル 1 つが収まります (それより大きなファイルは `uri` パートで送ります)。公開 `Host` をそのまま転送するローカルのリバースプロキシやトンネルの後ろでは、その名前 (または `*`) を列挙してください。

`neograph::core` と `neograph::a2a` をリンクします。

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::a2a)
```

## エージェントを呼び出す

上のプログラムを起動してから、このクライアントを接続します。エージェントを見つけ、メッセージを送り、タスクが入力待ちになるのを確認して、答えます。

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

Python でも同じです。

```python
from neograph_engine import a2a

agent = a2a.A2AClient("http://127.0.0.1:8087")
print(agent.fetch_agent_card().name)

task = agent.send_message("version 2.0")  # returns when the task finishes or needs input
print(task.state)
task = agent.send_message("approved", task.id, task.context_id)  # answer on the same task
print(task.state)
```

C++ のクライアントは次のように出力します。

```text
agent: release-notes
first: input-required - Interrupt before node: publish
second: completed - Published: Release notes: version 2.0
```

Python スクリプトは次のように出力します。

```text
release-notes
TaskState.InputRequired
TaskState.Completed
```

クライアントのその他の呼び出しは次のとおりです。`send_message_stream(text, on_event)` は、ステータスとアーティファクトの更新を届いた順に渡し、組み立てたタスクを返します。`get_task(id)` はタスクを読み、`cancel_task(id)` は、入力待ちのタスクを含め、タスクをキャンセルします。すでに終了したタスクにメッセージを送ると、`-32004` で失敗します。この Python クライアントは 0.14.0 のホイールにも入っており、上の Python スクリプトはホイールでもこのサーバーで動きます。

## リモートエージェントをノードとして使う

`A2ACallerNode` は、1つのチャネルを読み、そのテキストをリモートエージェントに送り、返答を別のチャネルに書きます。複数のノードが1つのクライアントを共有でき、その呼び出しは並行して実行されます。

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

既定のキーでは、`response`、`response_context_id`(会話を続けるための値)、`response_task_id`(タスクが入力待ちの間だけ値が入り、次の呼び出しがそのタスクを再開する)に書き込みます。上のプログラムは、エージェントのステータステキストとこのタスク ID を出力します。

## インタラプト、キャンセル、エラー

- **インタラプト。** 停止したグラフは、上で見たとおり `input-required` のタスクです。タスクが待っている間、アクティブな実行はなく、その状態はエンジンのチェックポイントストアにあります。
- **キャンセル。** `CancelTask` は実行のトークンをキャンセルし、タスクは `canceled` で終わります。すでに終了したタスクのキャンセルは `-32002` です。
- **失敗。** 失敗した実行は `failed` のタスクで、ステータスメッセージは失敗の公開メッセージ、`metadata["neograph/failure"]` は `{code, retryable, retryable_by_class}` です。`retryable` はエンジンがノードの試行を使い切ると `false` になり、`retryable_by_class` は失敗の分類が再試行を許すかどうかを示します。例外のテキストはネットワークに出ません。どのノードも終わる前に Hook がブロックした場合は `rejected` になります。`max_steps` に達したグラフは、コード `NG.StepLimit` で失敗します。
- **リクエストのエラー**(未知のタスク `-32001`、終了済みタスクへのメッセージ `-32004`、タスクのものと異なる `contextId` は `-32602`、まだ実行中のタスクは `-32603` と理由 `TASK_BUSY`)は、リファレンスの [A2A エラーマッピング](reference-en.md#a2a-error-mapping)に、すべてのプロトコルの失敗クラスは[プロトコル失敗マッピング](reference-en.md#protocol-failure-mapping)にあります。ここには再掲しません。
- **Program。** Program ベースのエージェント(`A2AServer` の背後にある承認済みの `ProgramVersion`)は、NeoGraph のコラボレーションエンベロープも受け付けます。[A2A コラボレーション拡張](reference-en.md#a2a-collaboration-extension)を参照してください。入力待ちの Program タスクには、まだ A2A で答えられません。メッセージには `-32004` が返り、タスクは待ち続けます。サーバーを再起動すると、`CollaborationMailbox` から Program タスクを復旧します。復旧したタスクがまだ実行中のときに `SubscribeToTask` を呼ぶと、先に Task を送り、永続 run が終わるまで追って、最終状態で終わります。run を追えないときは、空のまま閉じず、エラーイベントで終わります。

## テストと相互運用性

`tests/fixtures/a2a_probe_server.cpp` は、公開 API で作った小さな A2A サーバーです。相互運用スイートは、公式 SDK のクライアントをこのサーバーに、NeoGraph のクライアントを SDK のサーバーに接続して検査します。単体テストは `tests/test_a2a_server.cpp`、`tests/test_a2a_client.cpp`、`tests/test_a2a_conformance.cpp` です。
