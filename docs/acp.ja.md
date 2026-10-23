<!-- neograph-i18n: source=docs/acp.md locale=ja source_sha256=2cfa8ee50bd6b765e0c18b9f82a11f4154fff8b7c1c6f7fa589de795fa9c6984 -->
# ACP: グラフをエディターのエージェントとして使う

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

Agent Client Protocol(ACP)は、エディターをコーディング用や対話用のエージェントにつなぎます。エディター(Zed、CodeCompanion を使う Neovim、Gemini CLI など)はエージェントを子プロセスとして起動し、エージェントの stdin と stdout で、1行に1つの JSON オブジェクトという形の JSON-RPC 2.0 メッセージをやり取りします。エディターがセッションを開いてプロンプトを送ると、エージェントは答えを更新としてストリーミングで返します。エージェントがエディターに、許可や、ファイルおよびターミナルへのアクセスを求めることもあります。

`ACPServer` は、コンパイル済みの NeoGraph のグラフをこのようなエージェントとして動かします。NeoGraph が実装するのはエージェント側で、エディターがクライアントです。`ACPClient` は、エージェントがエディターへ自分のリクエストを送るためのハンドルです。エディターがエラーで答えると `acp::ACPClientError` を送出します。存在しないファイルへの `-32002` のように、リクエストが成功しえないことを示すコードなら `acp::ACPClientRefusal` になり、ノードはリトライされません。

## コンポーネント

| 役割 | C++ ターゲット | CMake オプション(既定値) | Python |
|---|---|---|---|
| エージェント(サーバー) | `neograph::acp` | `NEOGRAPH_BUILD_ACP`(オン) | 公式 SDK を使用 |
| Streamable HTTP と WebSocket のエージェント(実験的) | `neograph::acp_http_server` | `NEOGRAPH_BUILD_ACP_HTTP_SERVER`(**オフ**) | なし |

Python パッケージにはネイティブの ACP サーバーがありません。Python でエージェントを書くには、公式 SDK(`pip install "neograph-engine[acp]"`)を入れ、その背後で `neograph_engine.ProtocolHostAdapter` を使ってエンジンを動かします。このアダプターは、ACP のセッション ID を NeoGraph の `thread_id` に対応づけ、チェックポイントから再開し、キャンセルを伝え、トークンをストリーミングし、グラフのインタラプトを権限リクエストにします([Python でホストするときの権限リクエスト](#permission-requests-when-you-host-from-python)を参照)。[Python サンプル](../bindings/python/examples/README.md)にそのようなエージェントがあります。ACP には Python 3.10 以降が必要です。

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
- **ターンの終わり方。** 実行が完了すれば `end_turn`、キャンセルされれば `cancelled`、実行が `RunConfig::max_steps` に達すれば `max_turn_requests`、モデルのトークン予算を超えれば `max_tokens`、provider が拒否すれば `refusal` です。それ以外の失敗は、`data` が `{code, retryable, retryable_by_class}` の JSON-RPC エラー `-32603` です。`retryable` はエンジンがノードの試行を使い切ると `false` になり、`retryable_by_class` は失敗の分類が再試行を許すかどうかを示します。このエラーのメッセージは失敗の公開メッセージであり、例外のテキストではありません。完全な表は、リファレンスの [ACP](reference-en.md#neographacp--agent-client-protocol) と[プロトコル失敗マッピング](reference-en.md#protocol-failure-mapping)にあります。
- **セッション。** `session/new` はセッションを作り、`session/resume` はエージェントが知っているセッションを続けます。エンジンにチェックポイントストアがあれば、許可リクエストで止まったセッションを、エージェントの再起動後に `session/resume` で復元できます。このとき、次のプロンプトのテキストが、待っていた質問への答えになります。`initialize` の前の呼び出しは `-32600`、未知のセッションは `-32002` です。
- **MCP サーバー。** クライアントは `session/new` で stdio の MCP サーバーを渡せます。エージェントは NeoGraph の MCP クライアントを使い、セッションの `cwd` で、最小限の環境(`PATH` とクライアントの `env` の項目)でそれらを起動するので、エージェントの環境にある provider のキーは渡りません。ノードは `ACPClient::session_tools(session_id)` でそれらのツールにアクセスします。HTTP と SSE の MCP サーバーは `-32602` で拒否されます。[MCP ガイド](mcp.md)を参照してください。
- **ケイパビリティ。** `ACPServer::capabilities()` が `initialize` で通知する内容です。プロンプトはテキストと `resource_link` ブロックを受け付け、画像、音声、埋め込みリソースは、`capabilities().prompt` で有効にしたときだけ受け付けます。

## リモートトランスポート(実験的)

stdio は、ACP が定義し、すべてのエディターが実装しているトランスポートです。リモートトランスポート、つまり同じエンドポイントで WebSocket へのアップグレードも受け付ける Streamable HTTP は、ACP プロジェクトのドラフト提案としてしか存在しません。`neograph::acp_http_server` はエージェントをこの両方で提供し、ドラフトが明示している部分には従うので、ドラフトが変わればワイヤ上の詳細も変わることがあります。**実験的で、既定はオフ**です。求めない限り何もリンクされない別ライブラリ(`NEOGRAPH_BUILD_ACP_HTTP_SERVER`、既定はオフ)であり、サーバーは `start()` の後でしか待ち受けません。stdio の `ACPServer::run()` は変わりません。

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

`neograph::acp` も一緒に引き込む `neograph::acp_http_server` をリンクします。ファクトリは接続ごとに 1 回、任意の `authenticator` が返したプリンシパル(なければ空文字列)とともに呼ばれます。

- **接続。** ACP の接続 1 つが `ACPServer` 1 つで、stdio とまったく同じように駆動されます。メッセージは届いた順に処理され、終わった接続は入力の終端なので、実行中のプロンプトはキャンセルされます。接続は互いに独立で、同じエンジン上でプロンプトを同時に実行でき、`max_connections`(16)が数を制限します。セッション id は生きている接続 1 つに属します。別の生きている接続が持つ id を `session/resume` すると、`-32603` と `data.reason` `"session_busy"` で拒否されます。接続のセッションは接続とともに終わり、再接続の後は `session/resume` がエンジンのチェックポイントストアからセッションを復元します。エンジン上でプロンプトが実行されている間は(どの接続のものでも)、この接続が作っていないセッションの `session/resume` は最大 2 秒待ち、その間この接続は他のメッセージを読みません。それでも実行中なら `-32603` と `data.reason` `"engine_busy"` で応答するので、再試行してください。エディターを呼び出すノードは `ACPHttpServer::client()` をキャプチャするか(またはバインドされていない `ACPClient` を `attach_client()` に渡し)、各呼び出しはそのセッションを持つ接続に届きます。
- **Streamable HTTP** は `ACPHttpConfig::endpoint`(`/acp`)で提供されます。`Acp-Connection-Id` なしで `initialize` を `POST` すると接続が作られ、`200` で応答が本文として返り、id は `Acp-Connection-Id` ヘッダーに入ります。それ以外の `POST` はこのヘッダーを付け、本文なしの `202` で応答され、実際の答えはストリームで届きます。`Accept: text/event-stream` の `GET` は server-sent-event ストリームを開きます。ヘッダーだけなら接続ストリーム(`session/new` と `session/resume` への答え)、`Acp-Session-Id` もあればセッションストリーム(`session/update`、`session/request_permission` や `fs/*` などエージェントのリクエスト、そのセッションのリクエストへの答え)です。`session/resume` で復元したセッションは、クライアントがストリームを開くまで専用のストリームを持たず、その間のメッセージは接続ストリームで届きます。`DELETE` は接続を終わらせます。`params.sessionId` でセッションを指すリクエストは同じ `Acp-Session-Id` を送る必要があり、そのセッションは接続のものでなければなりません(`session/resume` は例外で、セッションを持ち込みます)。バッチは `501` です。開いているストリームがないときのメッセージは、上限のあるキューで待ちます。接続を失った後に再送されるものはなく、ストリームもリクエストもない状態が `connection_idle_timeout`(60 秒)続いた接続は閉じられ、そのプロンプトはキャンセルされます。
- **WebSocket** は同じエンドポイント(`Upgrade: websocket` の `GET`)で提供されます。テキストフレーム 1 つが、どちらの向きでも JSON-RPC メッセージ 1 つで、最初が `initialize` です。バイナリフレームは無視されます。フレームは RFC 6455 に従います。クライアントのフレームはマスクされている必要があり、ping には応答し、メッセージは分割されていてもかまいません。`max_payload_bytes` を超えるメッセージはコード 1009 で接続を閉じ、ソケットを閉じると接続が終わります。
- **ドラフトとの違い。** ドラフトは HTTP/2 を必須としていますが、このサーバーは HTTP/1.1 なので、ストリームと `POST` のそれぞれが別の TCP 接続になり、多重化はリバースプロキシに任せます。WebSocket アップグレードの 101 応答には `Acp-Connection-Id` がありません。クッキーは設定せず、`Acp-Protocol-Version` は検査せず、ストリームにイベント id や再開はありません。`initialize` の応答は接続 id をヘッダーでのみ返し、本文に `connectionId` としては入れません。`ACPServer` には `session/load` がないため、ドラフトの再接続の流れは `session/resume` が担い、セッションストリームは `session/new` または `session/resume` が応答した後でないと開けません。エージェントのリクエストへのクライアントの応答である `POST` では、`Acp-Session-Id` ヘッダーは任意です。同じ範囲の 2 本目のストリームは 1 本目を置き換えます。
- **セキュリティ。** 別のホストを指定しない限り `127.0.0.1` にバインドし、ループバックでないバインドは `authenticator` がなければ拒否されます。すべてのリクエスト、ストリーム、アップグレード、およびサーバーが提供しないパスも、本文を読む前に検査されます。まず Host と Origin(`allowed_hosts` と `allowed_origins`。[MCP](mcp.md) と [A2A](a2a.md) のサーバーと同じ規則)、次に `authenticator`(HTTP 401 と `WWW-Authenticate: Bearer`)、最後に `max_payload_bytes`(HTTP 413、既定は 4 MiB)です。接続は、それを作ったプリンシパルにだけ応答します。クライアントはサーバーにプロセスを起動させることができません。`mcpServers` を含む `session/new` や `session/resume` は、`allow_client_mcp_servers` を有効にしない限り `-32602` で拒否され(ACP はエージェントがクライアントの列挙した stdio MCP サーバーを起動することを期待しますが、ここではそれらはクライアントではなくサーバーのホストで動きます)、クライアントが指定した `cwd` は `_acp_cwd` としてグラフに渡るので、サーバー上でそれを使うノードが自分で検査する必要があります。エージェントを待つメッセージの量には上限があります(`max_payload_bytes` の 2 倍、最小 1 MiB)。それを超える `POST` には `Retry-After` 付きの `503` を、WebSocket のメッセージには JSON-RPC エラーを返します。遅い TCP 接続や遊んでいる TCP 接続、および 1 つのプリンシパルが使える `max_connections` とストリーム枠の割合は、相手ごとには制限されません。インターネットに面するサーバーは、それらを制限するリバースプロキシの背後に置いてください。エージェントはエディターの名前でファイルを読み書きするので、リモートバインドはそれを公開することだと考えてください。ブラウザーは WebSocket に `Authorization` を付けられないので、そうした接続の認証はリバースプロキシで行ってください。
<a id="permission-requests-when-you-host-from-python"></a>
## Python でホストするときの権限リクエスト

`ProtocolHostAdapter` は、停止するグラフ(`interrupt_before`、`NodeInterrupt`、ツールゲート)を、ネイティブサーバーと同じ形にします。同じターンの中の `session/request_permission` です。[サンプル 28](../bindings/python/examples/28_acp_agent.py)がそのエージェントです。

- **質問。** `adapter.stream(payload, thread_id=..., request_id=..., interrupts=True)` は、グラフが停止すると最終イベントの代わりに `interrupt` イベントで終わります。このイベントの `ProtocolInterrupt` には `reason`、`payload`(ノードが `NodeInterrupt` に渡した値)、`node`、`checkpoint_id` があります。`prompt()` は接続に対して `request_permission` を送ります。`ToolCallUpdate` のタイトルは理由、`rawInput` はペイロードで、選択肢は `approve`(`allow_once`)と `reject`(`reject_once`)です。ネイティブサーバーと同じです。
- **答え。** クライアントが選択肢を選ぶと、`prompt()` は `adapter.resume_stream(option_id, thread_id=..., request_id=..., interrupt=interrupt)` を呼びます。この呼び出しは、実行が停止したチェックポイントから再開し、残りのターンを `agent_message_chunk` の更新としてストリーミングします。選択肢の ID が、停止したノードが `input.ctx.resume_value` から読む値なので、ノードは `"approve"` か `"reject"` を受け取ります。グラフがまた質問すれば、ループはもう一度尋ねます。ノードが別の形を期待するなら、`ProtocolHostAdapter` に `resume_value=lambda answer, interrupt: ...` を渡してください。
- **キャンセル。** クライアントが `cancelled` と答えると、ターンは `stopReason: "cancelled"` で終わります。リクエストの待機中や、再開したターンの実行中の `session/cancel` も同じです。どちらの場合も、エージェントは `adapter.discard_interrupt(thread_id)` を呼ぶので、質問は残りません。セッションの次のプロンプトは新しいターンを始め、答えとは見なされません。放棄された停止はチェックポイントストアに開いたまま残り、エンジンはその直後の停止を再開できません(`NodeInterrupt ancestry crosses super-step boundaries`)。次のプロンプトが再び権限を求めた場合、セッションのいずれかのターンが完了するまで、その要求に答えると失敗します。
- **`interrupts=True` なしの場合。** グラフが停止すると、`stream()`、`run()`、`run_payload()` は `ProtocolInterrupted` を送出します。以前は、状態の最後のメッセージ、つまりユーザー自身のテキストを、エージェントの答えのように返していました。
- **チェックポイントストア。** 停止した実行を再開するには、エンジンにストアが必要です。サンプルは、永続ストアを渡されない限りメモリ上のストアを使います。
- **再起動。** 質問はターンの中にあります。待機中にエージェントを再起動すると、ターンは接続とともに終わり、次のプロンプトは新しいターンを始めます。ネイティブサーバーはそのようなセッションを `session/resume` で復元できますが、アダプターにはできません。
- **バージョン。** サンプルは `agent-client-protocol` 0.12.1 でテストしています。

## テストと相互運用性

`tests/fixtures/acp_probe_agent.cpp` は、公開 API で作った小さな ACP エージェントです。相互運用スイートは、公式 SDK のクライアントをこのエージェントに接続して検査します。単体テストは `tests/test_acp_server.cpp` と `tests/test_acp_types.cpp` で、リモートトランスポートには専用の `tests/test_acp_http_server.cpp` があります。`bindings/python/tests/interop/acp_suite/test_acp_sdk_remote_interop.py` は、公式 Python SDK(`agent-client-protocol` 0.12)がドラフトのトランスポート用に提供するクライアントを `neograph_acp_probe_agent --http` に対して両方のトランスポートで実行します。TypeScript SDK はこれらのトランスポートに対しては実行していません。
