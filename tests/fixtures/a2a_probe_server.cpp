// a2a_probe_server — NeoGraph's native A2AServer with a deterministic graph,
// driven by the official A2A SDKs in bindings/python/tests/interop/a2a.
//
// No provider, no network beyond loopback, no keys. The single node picks its
// behaviour from the first word of the inbound text, so one server process
// covers every task state and failure class the A2A surface reports:
//
//   echo <x>    completes with "[probe] echo <x>"
//   artifact    completes and emits a DataPart artifact
//   fail        throws std::runtime_error carrying a fake secret and a path
//   coded       throws NodeFailureError("app.tool_error", ...)
//   hook        throws HookBoundaryBlocked (run control)
//   provider    throws ProviderFailure (transient transport failure)
//   interrupt   NodeInterrupt; a follow-up message resumes it from the checkpoint
//   slow        sleeps up to 4 s, polling the run's CancelToken
//
// Usage: a2a_probe_server <port> [--no-streaming]
// Prints "READY <port>" on stdout once listening; stops on SIGTERM/SIGINT.

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/hook_outbox.h>
#include <neograph/types.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <signal.h>

using namespace neograph;
using namespace neograph::graph;
using namespace neograph::a2a;

namespace {

volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_signal(int) { g_stop = 1; }

class ProbeNode : public GraphNode {
  public:
    explicit ProbeNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string p = raw.is_string() ? raw.get<std::string>() : raw.dump();
        NodeOutput out;
        if (p.rfind("fail", 0) == 0) {
            throw std::runtime_error(
                "probe-internal-detail: tool exploded at /srv/secret/path, password=hunter2");
        }
        if (p.rfind("coded", 0) == 0) {
            throw NodeFailureError("app.tool_error", "the probe tool failed", false);
        }
        if (p.rfind("hook", 0) == 0) {
            throw HookBoundaryBlocked("probe-invocation", HookExecutionState::Failed,
                                      "mandatory hook blocked the run at /srv/secret/path");
        }
        if (p.rfind("provider", 0) == 0) {
            sp::Failure failure;
            failure.error.kind = sp::ErrorKind::Transport;
            failure.error.safe_message = "upstream reset, password=hunter2";
            throw ProviderFailure(std::make_shared<const sp::Outcome>(std::move(failure)));
        }
        if (p.rfind("interrupt", 0) == 0) {
            if (in.ctx.resume_value) {
                const auto& v = *in.ctx.resume_value;
                out.writes.push_back({"response",
                    "[probe] resumed: " + (v.is_string() ? v.get<std::string>() : v.dump())});
                co_return out;
            }
            throw NodeInterrupt("probe needs approval", json{{"tool", "probe"}});
        }
        if (p.rfind("slow", 0) == 0) {
            for (int i = 0; i < 40; ++i) {
                if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (p.rfind("artifact", 0) == 0) {
            out.writes.push_back({"data", json{{"answer", 42}, {"input", p}}});
        }
        out.writes.push_back({"response", "[probe] " + p});
        co_return out;
    }
    std::string get_name() const override { return name_; }

  private:
    std::string name_;
};

class ProbeAdapter final : public GraphAgentAdapter {
  public:
    std::optional<a2a::Artifact> build_output_artifact(const json& output,
                                                       const std::string& task_id) const override {
        if (!output.contains("channels")) return std::nullopt;
        const auto& ch = output["channels"];
        if (!ch.contains("data") || !ch["data"].contains("value") || ch["data"]["value"].is_null())
            return std::nullopt;
        a2a::Part part;
        part.kind = "data";
        part.data = ch["data"]["value"];
        a2a::Artifact a;
        a.artifact_id = task_id + "-data";
        a.name = "probe-data";
        a.parts.push_back(std::move(part));
        return a;
    }
};

std::shared_ptr<GraphEngine> build_engine() {
    NodeFactory::instance().register_type(
        "a2a_probe", [](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<ProbeNode>(n);
        });
    json def = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "a2a-probe"},
        {"channels",
         {{"prompt", {{"reducer", "overwrite"}}},
          {"response", {{"reducer", "overwrite"}}},
          {"data", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"probe", {{"type", "a2a_probe"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "probe"}},
                               json{{"from", "probe"}, {"to", "__end__"}}})},
    };
    // A checkpoint store lets a follow-up message resume an interrupted run.
    auto store = std::make_shared<InMemoryCheckpointStore>();
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}, store));
}

}  // namespace

int main(int argc, char** argv) {
    int port = 0;
    bool streaming = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-streaming")) streaming = false;
        else port = std::atoi(argv[i]);
    }
    // std::signal, not sigaction: the probe also builds with MSVC.
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    AgentCard card;
    card.name = "neograph-a2a-probe";
    card.description = "NeoGraph native A2AServer probe agent";
    card.version = "0.1.0";
    card.default_input_modes = {"text/plain"};
    card.default_output_modes = {"text/plain", "application/json"};
    card.streaming = streaming;
    card.skill_names = {"probe"};

    A2AServer server(build_engine(), card, std::make_shared<ProbeAdapter>());
    if (!server.start_async("127.0.0.1", port)) {
        std::cerr << "bind failed on " << port << "\n";
        return 1;
    }
    std::cout << "READY " << server.port() << std::endl;
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
    return 0;
}
