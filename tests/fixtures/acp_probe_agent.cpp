// acp_probe_agent — a deterministic NeoGraph ACP agent for the interop suites
// in bindings/python/tests/interop/acp/.
//
// It serves the native neograph::acp::ACPServer over newline-delimited
// JSON-RPC on stdio. No provider, no network, no keys. The first word of the
// text in a session/prompt selects what the single graph node does, so one
// agent process covers every scenario the suites need:
//
//   echo <text>        reply "[probe] <text>"
//   slow               wait up to 20 s, polling the run's CancelToken
//   fail               std::runtime_error whose message carries a fake secret
//   coded              NodeFailureError("app.tool_error", "the probe tool failed")
//   logic              std::invalid_argument carrying a fake secret (NG.LogicError)
//   hook               HookBoundaryBlocked (a mandatory Hook blocked)
//   budget             ProviderFailure{QuotaExhausted} (money or quota)
//   token-budget       crosses the run's model-token budget (max_tokens)
//   refusal            ProviderFailure whose partial stop is Refusal
//   provider           ProviderFailure{Transport} (transient provider)
//   interrupt          NodeInterrupt; the resumed run replies "resumed:<value>"
//   permission         session/request_permission, reply with the outcome
//   fsread <path>      fs/read_text_file, reply with the content
//   fswrite <path>     fs/write_text_file "probe-content", reply "written"
//   terminal           ACPServer::call_client("terminal/create", ...)
//   loop               route back to itself until RunConfig::max_steps
//   stream             stream the tokens "str", "eam" (LLM_TOKEN), reply "stream"
//   toolcall           write a model tool-call turn to `messages`: call_probe_ok
//                      succeeds, call_probe_fail fails with secret error text
//   pwd                reply "cwd:<session cwd>|dirs:<additional directories>"
//   mcp-list           reply "mcp-tools:<comma-separated session MCP tool names>"
//   mcp-call <tool> <json-arguments>
//                      call a session MCP tool, reply "mcp-result:<result>"
//   (anything else)    echo the text the default adapter passed to the graph
//
// Every failure verb puts "hunter2" and a filesystem path in its exception
// text; the suites assert that neither ever reaches the wire.
//
// Usage: neograph_acp_probe_agent [--agent-info JSON] [--load-session]
//                                 [--no-resume] [--image]

#include <neograph/neograph.h>
#include <neograph/acp/server.h>
#include <neograph/hook_outbox.h>
#include <neograph/tool.h>
#include <neograph/types.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace neograph;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;
using namespace neograph::acp;

namespace {

constexpr const char* kSecret = "db password=hunter2 at /srv/secret/probe.env";

std::atomic<ACPServer*> g_server{nullptr};

std::shared_ptr<const sp::Outcome> provider_failure(sp::ErrorKind kind, bool refusal) {
    sp::Failure failure;
    failure.error.kind = kind;
    // ProviderFailure::what() is this text: it must never reach the wire.
    failure.error.safe_message = std::string("probe provider failure: ") + kSecret;
    if (refusal) {
        sp::StopReason stop;
        stop.kind = sp::StopKind::Refusal;
        failure.partial.stop = stop;
    }
    return std::make_shared<const sp::Outcome>(std::move(failure));
}

std::string text_of(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

class ProbeNode : public GraphNode {
  public:
    ProbeNode(std::string name, std::shared_ptr<ACPClient> client)
        : name_(std::move(name)), client_(std::move(client)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const std::string text = text_of(in.state.get("prompt"));
        const auto sid_raw = in.state.get("_acp_session_id");
        const std::string sid = sid_raw.is_string() ? sid_raw.get<std::string>() : "";
        std::string verb = text, arg;
        if (auto space = text.find(' '); space != std::string::npos) {
            verb = text.substr(0, space);
            arg = text.substr(space + 1);
        }

        NodeOutput out;
        auto reply = [&](std::string value) {
            out.writes.push_back({"response", std::move(value)});
        };

        if (verb == "echo") {
            reply("[probe] " + arg);
        } else if (verb == "slow") {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (std::chrono::steady_clock::now() < deadline) {
                if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            reply("slow finished");
        } else if (verb == "fail") {
            throw std::runtime_error(std::string("probe failure: ") + kSecret);
        } else if (verb == "coded") {
            throw neograph::graph::NodeFailureError("app.tool_error", "the probe tool failed", false);
        } else if (verb == "logic") {
            throw std::invalid_argument(std::string("probe logic error: ") + kSecret);
        } else if (verb == "hook") {
            throw HookBoundaryBlocked("probe-hook-1", HookExecutionState::Failed,
                                      std::string("mandatory hook blocked the run: ") + kSecret);
        } else if (verb == "budget") {
            throw ProviderFailure(provider_failure(sp::ErrorKind::QuotaExhausted, false));
        } else if (verb == "token-budget") {
            // What a budget-aware provider node does when it crosses the run's
            // model-token budget: flag it; the engine stops at the next step.
            if (in.ctx.budget_exhausted) in.ctx.budget_exhausted->store(true);
            out.command = neograph::graph::Command{name_, {}};
        } else if (verb == "refusal") {
            throw ProviderFailure(provider_failure(sp::ErrorKind::RemoteFailure, true));
        } else if (verb == "provider") {
            throw ProviderFailure(provider_failure(sp::ErrorKind::Transport, false));
        } else if (verb == "interrupt") {
            if (in.ctx.resume_value) {
                reply("resumed:" + text_of(*in.ctx.resume_value));
            } else {
                throw neograph::graph::NodeInterrupt("approval needed",
                                                     json{{"tool", "probe"}});
            }
        } else if (verb == "permission") {
            ToolCallUpdate call;
            call.tool_call_id = "call_probe_1";
            call.title = "Run probe tool";
            call.kind = "execute";
            call.status = "pending";
            std::vector<PermissionOption> options = {
                {"allow", "Allow once", "allow_once"},
                {"reject", "Reject", "reject_once"},
            };
            auto outcome = client_->request_permission(sid, call, options);
            reply(outcome.kind == PermissionOutcomeKind::Selected
                      ? "permission:selected:" + outcome.option_id
                      : std::string("permission:cancelled"));
        } else if (verb == "fsread") {
            reply("read:" + client_->read_text_file(sid, arg));
        } else if (verb == "fswrite") {
            client_->write_text_file(sid, arg, "probe-content");
            reply("written");
        } else if (verb == "terminal") {
            auto* server = g_server.load();
            auto result = server->call_client(
                "terminal/create", json{{"sessionId", sid}, {"command", "true"}},
                std::chrono::seconds(5));
            reply("terminal:" + result.dump());
        } else if (verb == "loop") {
            out.command = neograph::graph::Command{name_, {}};
        } else if (verb == "stream") {
            if (in.stream_cb) {
                for (const char* token : {"str", "eam"}) {
                    (*in.stream_cb)(neograph::graph::GraphEvent{
                        neograph::graph::GraphEvent::Type::LLM_TOKEN, name_, json(token)});
                }
            }
            reply("stream");
        } else if (verb == "toolcall") {
            // What a model node and tool_dispatch write: the assistant's tool
            // calls, then one tool message per call.
            json assistant = {
                {"role", "assistant"}, {"content", ""},
                {"tool_calls", json::array({
                    {{"id", "call_probe_ok"}, {"name", "probe_lookup"},
                     {"arguments", "{\"query\":\"acp\"}"}},
                    {{"id", "call_probe_fail"}, {"name", "probe_write"}, {"arguments", "{}"}},
                })},
            };
            json ok = {{"role", "tool"}, {"tool_call_id", "call_probe_ok"},
                       {"tool_name", "probe_lookup"}, {"tool_status", "succeeded"},
                       {"content", "lookup result"}};
            json failed = {{"role", "tool"}, {"tool_call_id", "call_probe_fail"},
                           {"tool_name", "probe_write"}, {"tool_status", "failed"},
                           {"content", json{{"error", std::string("write failed: ") + kSecret},
                                            {"status", "failed"}}.dump()}};
            out.writes.push_back({"messages", json::array({assistant, ok, failed})});
            reply("tools done");
        } else if (verb == "pwd") {
            const auto dirs = in.state.get("_acp_additional_directories");
            std::string joined;
            if (dirs.is_array()) {
                for (const auto& dir : dirs) {
                    if (!joined.empty()) joined += ",";
                    joined += text_of(dir);
                }
            }
            reply("cwd:" + text_of(in.state.get("_acp_cwd")) + "|dirs:" + joined);
        } else if (verb == "mcp-list") {
            std::string names;
            for (const auto& tool : client_->session_tools(sid)) {
                if (!names.empty()) names += ",";
                names += tool->get_name();
            }
            reply("mcp-tools:" + names);
        } else if (verb == "mcp-call") {
            const auto space = arg.find(' ');
            const auto tool_name = arg.substr(0, space);
            const auto arguments = space == std::string::npos ? json::object()
                                                              : json::parse(arg.substr(space + 1));
            for (const auto& tool : client_->session_tools(sid)) {
                if (tool->get_name() != tool_name) continue;
                reply("mcp-result:" + tool->execute(arguments));
                co_return out;
            }
            reply("mcp-result:<no such tool>");
        } else {
            reply("[probe-default] " + text);
        }
        co_return out;
    }

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::shared_ptr<ACPClient> client_;
};

std::shared_ptr<GraphEngine> build_engine(std::shared_ptr<ACPClient> client) {
    NodeFactory::instance().register_type(
        "acp_probe",
        [client](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<ProbeNode>(n, client);
        });
    json def = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "acp-probe-agent"},
        {"channels", {
            {"prompt",                      {{"reducer", "overwrite"}}},
            {"response",                    {{"reducer", "overwrite"}}},
            {"_acp_session_id",             {{"reducer", "overwrite"}}},
            {"_acp_cwd",                    {{"reducer", "overwrite"}}},
            {"_acp_additional_directories", {{"reducer", "overwrite"}}},
            {"_acp_mcp_tools",              {{"reducer", "overwrite"}}},
            {"messages",                    {{"reducer", "append"}}},
        }},
        {"nodes", {{"probe", {{"type", "acp_probe"}}}}},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "probe"}},
            json{{"from", "probe"}, {"to", "__end__"}},
        })},
    };
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}, store));
}

}  // namespace

int main(int argc, char** argv) {
    auto client = std::make_shared<ACPClient>();
    client->set_timeout(std::chrono::seconds(10));
    auto engine = build_engine(client);
    json info{{"name", "neograph-acp-probe"}, {"version", "0.0.1"}};
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--agent-info") info = json::parse(argv[i + 1]);
    }
    ACPServer server(engine, info);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--load-session") server.capabilities().load_session = true;
        if (a == "--no-resume") server.capabilities().session.resume = false;
        if (a == "--image") server.capabilities().prompt.image = true;
    }
    server.attach_client(client);
    g_server.store(&server);
    std::cerr << "[acp-probe] ready\n";
    server.run();
    g_server.store(nullptr);
    return 0;
}
