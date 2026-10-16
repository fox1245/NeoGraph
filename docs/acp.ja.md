<!-- neograph-i18n: source=docs/acp.md locale=ja source_sha256=3617734aebc1bcbee95c4407fb07e702416c58627b2137e686d6c9ff0571b8b8 -->
# ACP: グラフをエディターのエージェントとして使う

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

Agent Client Protocol(ACP)は、エディターをコーディング用や対話用のエージェントにつなぎます。エディター(Zed、CodeCompanion を使う Neovim、Gemini CLI など)はエージェントを子プロセスとして起動し、エージェントの stdin と stdout で、1行に1つの JSON オブジェクトという形の JSON-RPC 2.0 メッセージをやり取りします。エディターがセッションを開いてプロンプトを送ると、エージェントは答えを更新としてストリーミングで返します。エージェントがエディターに、許可や、ファイルおよびターミナルへのアクセスを求めることもあります。

`ACPServer` は、コンパイル済みの NeoGraph のグラフをこのようなエージェントとして動かします。NeoGraph が実装するのはエージェント側で、エディターがクライアントです。`ACPClient` は、エージェントがエディターへ自分のリクエストを送るためのハンドルです。

## コンポーネント

| 役割 | C++ ターゲット | CMake オプション(既定値) | Python |
|---|---|---|---|
| エージェント(サーバー) | `neograph::acp` | `NEOGRAPH_BUILD_ACP`(オン) | 公式 SDK を使用 |

Python パッケージにはネイティブの ACP サーバーがありません。Python でエージェントを書くには、公式 SDK(`pip install "neograph-engine[acp]"`)を入れ、その背後で `neograph_engine.ProtocolHostAdapter` を使ってエンジンを動かします。このアダプターは、ACP のセッション ID を NeoGraph の `thread_id` に対応づけ、チェックポイントから再開し、キャンセルを伝え、トークンをストリーミングします。[Python サンプル](../bindings/python/examples/README.md)にそのようなエージェントがあります。ACP には Python 3.10 以降が必要です。

## バージョン

ACP プロトコルのバージョン 1 です。メッセージは公式の ACP スキーマ v1.25.0 と突き合わせて検査し、サーバーは CI(`.github/workflows/protocol-interop.yml`、スイートは `bindings/python/tests/interop/acp_suite`)で、公式の Python SDK(`agent-client-protocol` 0.12.1 と、プレリリースの 1.0.0rc2)および TypeScript SDK(`@agentclientprotocol/sdk` 1.8.0)を相手にテストしています。

## グラフをエージェントとして公開する

このプログラムは、[A2A ガイド](a2a.md)のリリースノートのグラフです。`draft` はプロンプトからリリースノートを書き、`publish` は人が承認したあとにだけ実行されます。API キーは不要です。

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

仕組みは次のとおりです。

- `session/prompt` は、プロンプトのテキストをグラフの `prompt` チャネルに入れます。`response` チャネルのテキストは `agent_message_chunk` の更新1つとして返り、ターンは `stopReason: "end_turn"` で終わります。これを変えるには、`ACPGraphAdapter` の `input_channel()`、`output_channel()`、`build_session_state()`(作業ディレクトリなどのチャネルに初期値を入れるとき)、または後述のインタラプト用フックをオーバーライドし、アダプターをコンストラクターの3番目の引数に渡します。
- グラフのインタラプトは、同じターンの中で許可リクエストになります。`interrupt_before` か `NodeInterrupt` が起きると、サーバーは `approve`(`allow_once`)と `reject`(`reject_once`)の選択肢を付けて `session/request_permission` を送ります。エディターが選んだ選択肢の ID が再開値になるので、上の `publish` は `"approve"` か `"reject"` を受け取ります。選択肢や値を変えるには、`interrupt_permission()` と `interrupt_resume_value()` をオーバーライドします。エディターが `cancelled` と答えると、ターンは `cancelled` で終わります。
- `run()` は stdin が閉じるまで読み続けます。プログラムが stdout にそれ以外を書くとプロトコルが壊れるので、ログは stderr に出してください。
- サーバーは同時に最大32件のプロンプトを処理し、セッションごとに1件までです(`set_max_inflight_prompts`)。拒否されたプロンプトには、`data.reason` が `"overloaded"` または `"session_busy"` の JSON-RPC エラー `-32603` が返ります。

`neograph::core` と `neograph::acp` をリンクします。

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::acp)
```

エディターは実行ファイルを起動し、その stdin と stdout で ACP を話します。登録の仕方はエディターによって異なります。

## エディターなしで試す

このスクリプトがエディターの役を務めます。エージェントを起動し、セッションを開き、プロンプトを送り、許可リクエストを承認して、起きたことを出力します。

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

エージェントのパスを渡して `python acp_client.py ./release_notes_agent` と実行すると、次のように出力されます。

```text
agent: release-notes 1.0.0
agent asks: Interrupt before node: publish ['approve', 'reject']
update: Published: Release notes: version 2.0
stop reason: end_turn
```

`"approve"` を `"reject"` に変えると、更新は `Not published (reject): Release notes: version 2.0` になります。

## キャンセル、セッション、エラー

- **キャンセル。** `session/cancel` は実行中のターンをキャンセルし、そのターンは `stopReason: "cancelled"` で終わります。実行中のターンがないときのキャンセルは無視されます。実行中の `session/prompt` をリクエスト id で指す `$/cancel_request` も同じようにそのターンをキャンセルし、ほかのリクエストを指すものは無視されます。stdin がファイルの終端に達すると、`run()` は実行中のターンが終わるまで `set_eof_grace`(1秒)待ち、そのあとキャンセルします。
- **ターンの終わり方。** 実行が完了すれば `end_turn`、キャンセルされれば `cancelled`、実行が `RunConfig::max_steps` に達すれば `max_turn_requests`、モデルのトークン予算を超えれば `max_tokens`、provider が拒否すれば `refusal` です。それ以外の失敗は、`data` が `{code, retryable}` の JSON-RPC エラー `-32603` で、メッセージは失敗の公開メッセージであり、例外のテキストではありません。完全な表は、リファレンスの [ACP](reference-en.md#neographacp--agent-client-protocol) と[プロトコル失敗マッピング](reference-en.md#protocol-failure-mapping)にあります。
- **セッション。** `session/new` はセッションを作り、`session/resume` はエージェントが知っているセッションを続けます。エンジンにチェックポイントストアがあれば、許可リクエストで止まったセッションを、エージェントの再起動後に `session/resume` で復元できます。このとき、次のプロンプトのテキストが、待っていた質問への答えになります。`initialize` の前の呼び出しは `-32600`、未知のセッションは `-32002` です。
- **MCP サーバー。** クライアントは `session/new` で stdio の MCP サーバーを渡せます。エージェントは NeoGraph の MCP クライアントを使い、セッションの `cwd` で、最小限の環境(`PATH` とクライアントの `env` の項目)でそれらを起動するので、エージェントの環境にある provider のキーは渡りません。ノードは `ACPClient::session_tools(session_id)` でそれらのツールにアクセスします。HTTP と SSE の MCP サーバーは `-32602` で拒否されます。[MCP ガイド](mcp.md)を参照してください。
- **ケイパビリティ。** `ACPServer::capabilities()` が `initialize` で通知する内容です。プロンプトはテキストと `resource_link` ブロックを受け付け、画像、音声、埋め込みリソースは、`capabilities().prompt` で有効にしたときだけ受け付けます。

## テストと相互運用性

`tests/fixtures/acp_probe_agent.cpp` は、公開 API で作った小さな ACP エージェントです。相互運用スイートは、公式 SDK のクライアントをこのエージェントに接続して検査します。単体テストは `tests/test_acp_server.cpp` と `tests/test_acp_types.cpp` です。
