// example_acp_server — expose a NeoGraph as an Agent Client Protocol (ACP)
//                       agent over newline-delimited JSON-RPC on stdio.
//
// ACP is what Zed, Neovim/CodeCompanion, Gemini CLI etc. speak to talk
// to a coding agent — the editor↔agent analogue of LSP. Spec:
// https://agentclientprotocol.com/protocol/schema.md
//
// This binary launches a tiny ReAct-style graph (mock provider, no API
// key) and serves it as an ACP agent. Two run modes:
//
//   1) Drive on stdin/stdout (default, what an editor does):
//        ./example_acp_server
//      Then send NDJSON envelopes:
//        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{}}}
//        {"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":"/tmp","mcpServers":[]}}
//        {"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"<from #2>","prompt":[{"type":"text","text":"hello"}]}}
//
//   2) Self-test (loops a request through the in-process server):
//        ./example_acp_server --self-test

#include <neograph/neograph.h>
#include <neograph/acp/server.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace neograph;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;
using namespace neograph::acp;

namespace {

class UppercaseNode : public GraphNode {
  public:
    explicit UppercaseNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string p = raw.is_string() ? raw.get<std::string>() : raw.dump();
        std::string upper = "[neo-acp] ";
        for (char c : p)
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        NodeOutput out;
        out.writes.push_back({"response", std::move(upper)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

std::shared_ptr<GraphEngine> build_demo_engine() {
    NodeFactory::instance().register_type("uppercase",
        [](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<UppercaseNode>(n);
        });
    neograph::json def = {
        {"name", "uppercase-acp-agent"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"uppercase", {{"type", "uppercase"}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "uppercase"}},
            neograph::json{{"from", "uppercase"}, {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    return std::shared_ptr<GraphEngine>(std::move(engine));
}

int run_self_test(ACPServer& server) {
    std::cerr << "[*] self-test — driving the server in-process\n";

    // session/prompt is async-dispatched: we have to wait on the sink
    // for the response envelope to arrive instead of relying on
    // handle_message's return value.
    std::mutex              mu;
    std::condition_variable cv;
    bool                    saw_response = false;
    int                     notif_count  = 0;
    server.set_notification_sink([&](const neograph::json& env) {
        std::cerr << "    [notif] " << env.dump() << "\n";
        std::lock_guard lk(mu);
        if (env.value("method", std::string()) == "session/update") {
            ++notif_count;
        } else if (!env.contains("method")
                   && env.contains("id")
                   && env["id"].is_number_integer()
                   && env["id"].get<int>() == 3) {
            saw_response = true;
            cv.notify_all();
        }
    });

    {
        neograph::json env;
        env["jsonrpc"] = "2.0";
        env["id"]      = 1;
        env["method"]  = "initialize";
        env["params"]  = {{"protocolVersion", 1}, {"clientCapabilities", neograph::json::object()}};
        auto resp = server.handle_message(env);
        std::cerr << "    initialize -> " << resp.dump() << "\n";
    }

    std::string sid;
    {
        neograph::json env;
        env["jsonrpc"] = "2.0";
        env["id"]      = 2;
        env["method"]  = "session/new";
        env["params"]  = {{"cwd", "/tmp"}, {"mcpServers", neograph::json::array()}};
        auto resp = server.handle_message(env);
        std::cerr << "    session/new -> " << resp.dump() << "\n";
        sid = resp["result"].value("sessionId", std::string());
    }

    {
        neograph::json env;
        env["jsonrpc"] = "2.0";
        env["id"]      = 3;
        env["method"]  = "session/prompt";
        neograph::json prompt = neograph::json::array();
        prompt.push_back({{"type", "text"}, {"text", "hello acp"}});
        env["params"] = {{"sessionId", sid}, {"prompt", std::move(prompt)}};
        auto immediate = server.handle_message(env);
        std::cerr << "    session/prompt dispatched (async) -> "
                  << (immediate.is_null() ? std::string("null")
                                          : immediate.dump()) << "\n";
    }

    {
        std::unique_lock lk(mu);
        if (!cv.wait_for(lk, std::chrono::seconds(5), [&]{ return saw_response; })) {
            std::cerr << "[!] timed out waiting for session/prompt response\n";
            return 1;
        }
    }

    if (notif_count == 0) {
        std::cerr << "[!] expected at least one session/update notification\n";
        return 1;
    }
    std::cerr << "[*] OK — " << notif_count << " update(s) emitted, "
              << "response received\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool self_test = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) self_test = true;
    }

    auto engine = build_demo_engine();

    neograph::json info;
    info["name"]    = "neograph-acp-demo";
    info["version"] = "0.1.0";

    ACPServer server(engine, info);
    server.capabilities().session.close = false;  // baseline-only
    server.capabilities().prompt.image  = false;

    if (self_test) return run_self_test(server);

    std::cerr << "[*] ACP agent reading NDJSON on stdin (Ctrl-D to exit)\n";
    server.run();  // blocks on std::cin / std::cout
    return 0;
}
