// A2A 1.0 / 0.3 conformance of the native A2AServer at the wire level: the
// audit findings A2A-01 … A2A-22 that the official SDKs depend on. The
// official-SDK suites live in bindings/python/tests/interop/a2a; these tests
// pin the same contract without any SDK.

#include <gtest/gtest.h>

#include <neograph/a2a/a2a_caller_node.h>
#include <neograph/a2a/client.h>
#include <neograph/a2a/server.h>
#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/a2a/collaboration.h>
#include <neograph/a2a/program_adapter.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#endif
#include <neograph/async/run_sync.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/state.h>
#include <neograph/a2a/types.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/hook_outbox.h>
#include <neograph/types.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::a2a;
using namespace std::chrono_literals;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace {

constexpr const char* kSecret = "db password=hunter2 at /srv/secret/path";

std::shared_ptr<const sp::Outcome> provider_failure(sp::ErrorKind kind, bool refusal) {
    sp::Failure failure;
    failure.error.kind = kind;
    failure.error.safe_message = std::string("provider said ") + kSecret;
    if (refusal) {
        sp::StopReason stop;
        stop.kind = sp::StopKind::Refusal;
        failure.partial.stop = stop;
    }
    return std::make_shared<const sp::Outcome>(std::move(failure));
}

/// Holds the runs of the `gate` prompt until a test opens it, so a test that
/// needs a run to be in flight does not depend on how long a sleep takes.
class RunGate {
  public:
    void open() {
        { std::lock_guard lk(mu_); open_ = true; }
        cv_.notify_all();
    }
    void close() {
        std::lock_guard lk(mu_);
        open_ = false;
    }
    bool wait_for(std::chrono::milliseconds limit) {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, limit, [&] { return open_; });
    }

  private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool open_ = false;
};

RunGate& run_gate() {
    static RunGate gate;
    return gate;
}

/// Closes the gate for one test and opens it again on the way out, so a test
/// that fails early never leaves a run blocked behind the server it owns.
/// Declare it after the Server: it is destroyed first.
struct GateScope {
    GateScope() { run_gate().close(); }
    ~GateScope() { run_gate().open(); }
    GateScope(const GateScope&) = delete;
    GateScope& operator=(const GateScope&) = delete;
    void open() { run_gate().open(); }
};

/// One node whose behaviour is chosen by the first word of the prompt.
class ConformanceNode : public GraphNode {
  public:
    explicit ConformanceNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        const std::string text = raw.is_string() ? raw.get<std::string>() : raw.dump();
        const auto verb = text.substr(0, text.find(' '));
        NodeOutput out;
        if (verb == "fail") throw std::runtime_error(std::string("tool exploded: ") + kSecret);
        if (verb == "coded") throw graph::NodeFailureError("app.tool_error", "the tool failed", false);
        if (verb == "logic") throw std::logic_error(kSecret);
        if (verb == "hook") {
            throw HookBoundaryBlocked("hook-1", HookExecutionState::Failed,
                                      std::string("blocked: ") + kSecret);
        }
        if (verb == "provider") throw ProviderFailure(provider_failure(sp::ErrorKind::Transport, false));
        if (verb == "budget") throw ProviderFailure(provider_failure(sp::ErrorKind::QuotaExhausted, false));
        if (verb == "refusal") throw ProviderFailure(provider_failure(sp::ErrorKind::RemoteFailure, true));
        if (verb == "interrupt") {
            if (in.ctx.resume_value) {
                const auto& v = *in.ctx.resume_value;
                out.writes.push_back({"response", "resumed:" + (v.is_string() ? v.get<std::string>() : v.dump())});
                co_return out;
            }
            throw graph::NodeInterrupt("approve the probe?", json{{"tool", "probe"}});
        }
        if (verb == "gate") {
            // Waits for the test; a cancel or a lost test ends it too.
            for (int i = 0; i < 3000 && !run_gate().wait_for(10ms); ++i) {
                if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) break;
            }
        }
        if (verb == "nap") {
            // "nap <microseconds>": a run that ends about when a subscriber joins.
            std::this_thread::sleep_for(std::chrono::microseconds(std::stoi(text.substr(text.find(' ') + 1))));
        }
        if (verb == "slow") {
            for (int i = 0; i < 300; ++i) {
                if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) break;
                std::this_thread::sleep_for(10ms);
            }
        }
        out.writes.push_back({"response", "[conformance] " + text});
        co_return out;
    }
    std::string get_name() const override { return name_; }

  private:
    std::string name_;
};

std::shared_ptr<GraphEngine> build_engine(bool with_checkpoints, json extra = json::object()) {
    NodeFactory::instance().register_type(
        "a2a_conformance", [](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<ConformanceNode>(n);
        });
    json def = {
        {"name", "a2a-conformance"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}},
                      {"response", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"probe", {{"type", "a2a_conformance"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "probe"}},
                               json{{"from", "probe"}, {"to", "__end__"}}})},
    };
    for (auto [key, value] : extra.items()) def[key] = value;
    if (with_checkpoints) {
        auto store = std::make_shared<graph::InMemoryCheckpointStore>();
        return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}, store));
    }
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}));
}

AgentCard conformance_card(bool streaming = true) {
    AgentCard card;
    card.name = "conformance";
    card.description = "A2A conformance probe";
    card.version = "1.0.0";
    card.default_input_modes = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.streaming = streaming;
    card.skill_names = {"probe"};
    return card;
}

struct Server {
    std::unique_ptr<A2AServer> server;
    explicit Server(bool streaming = true, bool checkpoints = true, json extra = json::object()) {
        server = std::make_unique<A2AServer>(build_engine(checkpoints, std::move(extra)),
                                             conformance_card(streaming));
        if (!server->start_async("127.0.0.1", 0)) throw std::runtime_error("bind failed");
    }
    /// Wraps a server the test built itself (a Program-backed one) and started.
    explicit Server(std::unique_ptr<A2AServer> started) : server(std::move(started)) {}
    ~Server() { server->stop(); }
    int port() const { return server->port(); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port()); }
};

struct Reply {
    int status = 0;
    std::string body;
    httplib::Headers headers;
    json envelope;
};

Reply post(const Server& srv, const std::string& body, const std::string& version = "1.0",
           const std::string& path = "/", const httplib::Headers& extra = {}) {
    httplib::Client c("127.0.0.1", srv.port());
    c.set_read_timeout(10, 0);
    httplib::Headers headers = extra;
    if (!version.empty()) headers.emplace("A2A-Version", version);
    auto res = c.Post(path, headers, body, "application/json");
    if (!res) throw std::runtime_error("no response");
    Reply r{res->status, res->body, res->headers, json()};
    try {
        r.envelope = json::parse(res->body);
    } catch (...) {
    }
    return r;
}

Reply rpc(const Server& srv, const std::string& method, const json& params,
          const std::string& version = "1.0", const httplib::Headers& extra = {}) {
    json body = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", method}, {"params", params}};
    return post(srv, body.dump(), version, "/", extra);
}

/// POST a streaming method and hand each piece of the body to @p on_chunk as
/// it arrives (with everything received so far), so a test can act while the
/// stream is still open. Returns the whole body.
Reply stream_rpc(const Server& srv, const std::string& method, const json& params,
                 const std::function<void(const std::string&)>& on_chunk,
                 int read_timeout_s = 10, const httplib::Headers& extra = {}) {
    httplib::Client c("127.0.0.1", srv.port());
    c.set_read_timeout(read_timeout_s, 0);
    const json body = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", method}, {"params", params}};
    std::string received;
    httplib::Headers headers = extra;
    headers.emplace("A2A-Version", "1.0");
    auto res = c.Post("/", headers, body.dump(),
                      "application/json", [&](const char* data, size_t size) {
                          received.append(data, size);
                          on_chunk(received);
                          return true;
                      });
    if (!res) throw std::runtime_error("no response");
    Reply r{res->status, received, res->headers, json()};
    try {
        r.envelope = json::parse(received);
    } catch (...) {
    }
    return r;
}

/// The data payloads of an SSE body.
std::vector<json> sse_events(const std::string& body) {
    std::vector<json> out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto end = body.find("\n\n", pos);
        if (end == std::string::npos) end = body.size();
        const auto frame = body.substr(pos, end - pos);
        if (frame.rfind("data: ", 0) == 0) out.push_back(json::parse(frame.substr(6)));
        pos = end + 2;
    }
    return out;
}

json v1_message(const std::string& text, const std::string& task_id = {},
                const std::string& context_id = {}) {
    static int seq = 0;
    json m = {{"messageId", "m-" + std::to_string(++seq)}, {"role", "ROLE_USER"},
              {"parts", json::array({json{{"text", text}}})}};
    if (!task_id.empty()) m["taskId"] = task_id;
    if (!context_id.empty()) m["contextId"] = context_id;
    return m;
}

int error_code(const json& envelope) {
    if (!envelope.is_object() || !envelope.contains("error")) return 0;
    return envelope["error"].value("code", 0);
}

json send_v1(const Server& srv, const std::string& text) {
    auto r = rpc(srv, "SendMessage", {{"message", v1_message(text)}});
    return r.envelope["result"]["task"];
}

#ifdef NEOGRAPH_A2A_PROGRAM
// --- a Program task recovered after a restart (#413) --------------------------

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

std::atomic<bool> g_program_node_interrupts{false};

/// A Program node that waits at the run gate, so a run the server recovers is
/// still working when a test subscribes to it.
class GatedProgramNode final : public GraphNode {
  public:
    explicit GatedProgramNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        for (int i = 0; i < 3000 && !run_gate().wait_for(10ms); ++i) {
            if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) break;
        }
        if (g_program_node_interrupts.load(std::memory_order_acquire))
            throw graph::NodeInterrupt("needs a reviewer");
        NodeOutput out;
        out.writes.push_back({"value", "completed"});
        co_return out;
    }
    std::string get_name() const override { return name_; }

  private:
    std::string name_;
};

CollaborationLink recovery_link() {
    CollaborationLinkSpec spec;
    spec.link_id = "link-1";
    spec.sender_owner_scope = "owner-a";
    spec.receiver_owner_scope = "owner-b";
    spec.sender_agent_id = "coordinator-a";
    spec.receiver_agent_id = "executor-b";
    spec.task_scope = "bounded-coding-task";
    spec.capability_allowlist = {"code-read"};
    spec.effect_allowlist = {"artifact-publish"};
    spec.artifact_allowlist = {"program-result:run-b"};
    spec.cancellation_rights = {"sender", "receiver"};
    spec.message_kind_allowlist = {"request", "completed", "failed", "canceled", "terminal"};
    spec.expires_at_unix_ms = 4102444800000ULL;
    spec.max_retries = 2;
    spec.acknowledgement_timeout_ms = 1000;
    return CollaborationLink::create(std::move(spec));
}

/// One Program version over in-memory stores that outlive a runtime.
struct RecoveryFixture {
    program::RegistrySnapshot registry;
    program::AdmissionProfile profile;
    program::PolicySnapshot policy;
    std::shared_ptr<program::InMemoryProgramStore> store;
    std::shared_ptr<program::EngineGenerationCache> engines;
    std::shared_ptr<program::ProgramCatalog> catalog;
    std::shared_ptr<graph::InMemoryCheckpointStore> checkpoints;
    std::shared_ptr<program::InMemoryProgramTransitionStore> transitions;
    std::shared_ptr<program::ProgramRuntime> runtime;
    std::optional<program::ProgramVersion> version;

    RecoveryFixture()
        : registry(make_registry()),
          profile(make_profile(registry)),
          policy(make_policy(profile)),
          store(std::make_shared<program::InMemoryProgramStore>()),
          checkpoints(std::make_shared<graph::InMemoryCheckpointStore>()),
          transitions(std::make_shared<program::InMemoryProgramTransitionStore>()) {
        restart();
        program::ProgramCompiler compiler(registry, {"a2a-recovery/v1"});
        version.emplace(catalog->admit(
            compiler.compile(program::ProgramSource::from_cpp_builder(
                "test:a2a-recovery", 1, source_document())),
            program::ProgramAdmission{"owner-b", profile, policy, {}}));
    }

    void restart() {
        runtime.reset();
        engines = std::make_shared<program::EngineGenerationCache>();
        catalog = std::make_shared<program::ProgramCatalog>(
            program::CatalogConfig{store, registry, engines, "a2a-recovery/v1"});
        runtime = std::make_shared<program::ProgramRuntime>(
            program::RuntimeConfig{catalog, checkpoints, {}, transitions, 1});
    }

    /// The mailbox of a receiver that accepted the request for run "run-b" and
    /// stopped before it started the run, as a new process reads it back.
    std::shared_ptr<CollaborationMailbox> mailbox_with_pending_request() const {
        const auto accepted = recovery_link().accept("executor-b", "consent-secret");
        const auto envelope = CollaborationEnvelope::bind_program(
            CollaborationEnvelope::create(accepted, "run-a", "run-b", "run-b", "context-1",
                                          "message-1", "correlation-1", 1, "request",
                                          "idem-recovery", json{{"prompt", "persisted"}}),
            *version);
        program::RunInvocation invocation;
        invocation.owner_scope = envelope.receiver_owner_scope;
        invocation.agent_id = envelope.receiver_agent_id;
        invocation.program_version_id = version->id();
        invocation.run_id = envelope.receiver_program_run_id;
        invocation.budget = program::RunBudget{20000, 1000, 1000, 1, 1, 20, 0, 0, 0};
        invocation.input = envelope.payload;
        invocation.message_sequence = envelope.sequence;
        invocation.idempotency_key = envelope.idempotency_key;
        invocation.correlation_id = envelope.correlation_id;
        CollaborationMailbox original("owner-b", "executor-b");
        original.accept_link(accepted, "consent-secret");
        if (original.submit_program(envelope, *version, invocation)
            != CollaborationSubmitResult::Accepted)
            throw std::runtime_error("the mailbox refused the request");
        return std::make_shared<CollaborationMailbox>(
            CollaborationMailbox::parse(original.serialize_canonical()));
    }

    /// A server over the restarted runtime. Starting it recovers the request:
    /// the run starts at once and waits at the gate.
    std::unique_ptr<A2AServer> server_over(
        std::shared_ptr<CollaborationMailbox> mailbox,
        ProgramAgentAdapter::ArtifactBuilder artifacts = {}) const {
        auto adapter = std::make_shared<ProgramAgentAdapter>(
            runtime, *version, "owner-b", std::move(mailbox),
            ProgramAgentAdapter::InvocationBuilder{}, std::move(artifacts));
        AgentCard card;
        card.name = "recovered-program";
        card.description = "A2A recovery conformance";
        card.version = "1.0.0";
        card.default_input_modes = {"text/plain"};
        card.default_output_modes = {"text/plain"};
        auto server = std::make_unique<A2AServer>(
            adapter, card,
            [](std::string_view authorization) -> std::optional<CollaborationPeerIdentity> {
                if (authorization == "Bearer coordinator-a")
                    return CollaborationPeerIdentity{"owner-a", "coordinator-a"};
                return std::nullopt;
            });
        if (!server->start_async("127.0.0.1", 0)) throw std::runtime_error("bind failed");
        return server;
    }

  private:
    static program::RegistrySnapshot make_registry() {
        using namespace program;
        RegistrySnapshotBuilder builder;
        builder.add_node(
            ExecutableManifest{{ExecutableKind::Node, "a2a-gated-node", "1.0.0", digest('4')},
                               EffectMode::Brokered, "attestation:a2a", {}, {}, {}},
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<GatedProgramNode>(name);
            },
            json{{"type", "object"}}, json::object());
        builder.add_reducer(
            ExecutableManifest{{ExecutableKind::Reducer, "a2a-overwrite", "1.0.0", digest('5')},
                               EffectMode::Brokered, "attestation:a2a", {}, {}, {}},
            [](const json&, const json& incoming) { return json(incoming); });
        return std::move(builder).build();
    }

    static program::AdmissionProfile make_profile(const program::RegistrySnapshot& registry) {
        using namespace program;
        AdmissionProfileBuilder builder;
        builder.id("a2a-recovery-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(1)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry.identities()) builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static program::PolicySnapshot make_policy(const program::AdmissionProfile& profile) {
        using namespace program;
        PolicySnapshotBuilder builder;
        builder.id("a2a-recovery-policy")
            .semantic_version("1.0.0")
            .owner_scope("owner-b")
            .admission_profile(profile)
            .budget_ceiling(BudgetLimits{20000, 1000, 1000, 1, 1, 20, 1, 1, 1});
        return std::move(builder).build();
    }

    static json source_document() {
        return json{
            {"program_schema_version", 1},
            {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"root",
             json{{"op", "call_core"},
                  {"name", "main"},
                  {"definition",
                   json{{"schema_version", 1},
                        {"name", "main"},
                        {"channels",
                         json{{"value", json{{"reducer", "a2a-overwrite"}, {"initial", ""}}}}},
                        {"nodes", json{{"work", json{{"type", "a2a-gated-node"}}}}},
                        {"edges",
                         json::array({json{{"from", "__start__"}, {"to", "work"}},
                                      json{{"from", "work"}, {"to", "__end__"}}})},
                        {"conditional_edges", json::array()}}}}},
            {"declared_budget_requirements",
             json::array(
                 {json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 20000}},
                  json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
                  json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
                  json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
                  json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
                  json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
                  json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
                  json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
                  json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}}})}};
    }
};

/// The recovering peer of the link: the coordinator that sent the request.
const httplib::Headers kCoordinator{{"Authorization", "Bearer coordinator-a"}};

/// Opens the gate on the way out, before the server and the fixture are torn
/// down, so a test that fails early never leaves a recovered run blocked.
struct ReleaseGate {
    ~ReleaseGate() { run_gate().open(); }
};
#endif

}  // namespace

// --- card (A2A-10, A2A-19, A2A-21) -------------------------------------------

TEST(A2AConformance, CardIsServedPerProtocolVersion) {
    Server srv;
    httplib::Client c("127.0.0.1", srv.port());
    auto v1 = c.Get("/.well-known/agent-card.json", httplib::Headers{{"A2A-Version", "1.0"}});
    ASSERT_TRUE(v1);
    const auto card = json::parse(v1->body);
    for (const char* legacy : {"url", "protocolVersion", "preferredTransport",
                               "supportsAuthenticatedExtendedCard"}) {
        EXPECT_FALSE(card.contains(legacy)) << legacy;
    }
    ASSERT_EQ(card["supportedInterfaces"].size(), 2u);
    EXPECT_EQ(card["supportedInterfaces"][0]["protocolVersion"], "1.0");
    EXPECT_EQ(card["supportedInterfaces"][0]["url"], srv.url());
    EXPECT_EQ(card["supportedInterfaces"][1]["protocolVersion"], "0.3");
    EXPECT_EQ(card["capabilities"]["streaming"], true);
    EXPECT_EQ(card["capabilities"]["pushNotifications"], false);
    EXPECT_EQ(card["capabilities"]["extendedAgentCard"], false);
    ASSERT_EQ(card["skills"].size(), 1u);
    EXPECT_EQ(card["skills"][0]["tags"], json::array({"probe"}));

    auto legacy = c.Get("/.well-known/agent-card.json");
    ASSERT_TRUE(legacy);
    const auto v03 = json::parse(legacy->body);
    EXPECT_EQ(v03["protocolVersion"], "0.3.0");
    EXPECT_EQ(v03["url"], srv.url());
    EXPECT_EQ(v03["supportedInterfaces"].size(), 2u);
    EXPECT_EQ(legacy->get_header_value("Vary"), "A2A-Version");

    auto unsupported = c.Get("/.well-known/agent-card.json",
                             httplib::Headers{{"A2A-Version", "9.9"}});
    ASSERT_TRUE(unsupported);
    EXPECT_EQ(unsupported->status, 400);
}

TEST(A2AConformance, CardCarriesCachingHeadersAndAnswersIfNoneMatch) {
    Server srv;
    httplib::Client c("127.0.0.1", srv.port());
    auto first = c.Get("/.well-known/agent-card.json");
    ASSERT_TRUE(first);
    const auto etag = first->get_header_value("ETag");
    ASSERT_FALSE(etag.empty());
    EXPECT_NE(first->get_header_value("Cache-Control").find("max-age"), std::string::npos);
    auto again = c.Get("/.well-known/agent-card.json", httplib::Headers{{"If-None-Match", etag}});
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 304);
}

// --- version and JSON-RPC envelope (A2A-11, A2A-17, A2A-28) ----------------------

TEST(A2AConformance, OnlyVersionsOnePointZeroAndZeroPointThreeAreSpoken) {
    Server srv;
    for (const char* version : {"0.2", "1.5", "2.0", "9.9", "1.0.x", "0"}) {
        EXPECT_EQ(error_code(rpc(srv, "SendMessage", {{"message", v1_message("echo")}}, version).envelope),
                  -32009) << version;
    }
    EXPECT_TRUE(rpc(srv, "SendMessage", {{"message", v1_message("echo")}}, "1.0.1")
                    .envelope["result"].contains("task"));
    // A2A-Version as a request parameter (spec 3.6.1).
    httplib::Client c("127.0.0.1", srv.port());
    json body = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "SendMessage"},
                 {"params", {{"message", v1_message("echo")}}}};
    auto res = c.Post("/?A2A-Version=1.0", body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_TRUE(json::parse(res->body)["result"].contains("task"));
}

TEST(A2AConformance, MalformedEnvelopesAreInvalidRequests) {
    Server srv;
    for (const char* raw : {
             R"({"jsonrpc":"1.0","id":1,"method":"GetTask","params":{"id":"x"}})",
             R"({"id":1,"method":"GetTask","params":{"id":"x"}})",
             R"({"jsonrpc":"2.0","id":1,"method":5})",
             R"({"jsonrpc":"2.0","id":1})",
             R"({"jsonrpc":"2.0","id":{"a":1},"method":"GetTask","params":{"id":"x"}})",
             R"([{"jsonrpc":"2.0","id":1,"method":"GetTask","params":{"id":"x"}}])"}) {
        const auto r = post(srv, raw);
        EXPECT_EQ(r.status, 200) << raw;
        EXPECT_EQ(error_code(r.envelope), -32600) << raw << " -> " << r.body;
    }
    const auto parse = post(srv, "{not json");
    EXPECT_EQ(error_code(parse.envelope), -32700);
    EXPECT_TRUE(parse.envelope["id"].is_null());
    // Typed details (A2A-18): the first one is an ErrorInfo.
    const auto info = parse.envelope["error"]["data"][0];
    EXPECT_EQ(info["@type"], "type.googleapis.com/google.rpc.ErrorInfo");
}

// --- method set (A2A-01, A2A-02, A2A-08) ------------------------------------------

TEST(A2AConformance, UnsupportedFeaturesAnswerTheirA2AErrors) {
    Server srv;
    for (const char* method : {"CreateTaskPushNotificationConfig", "GetTaskPushNotificationConfig",
                               "ListTaskPushNotificationConfigs", "DeleteTaskPushNotificationConfig"}) {
        EXPECT_EQ(error_code(rpc(srv, method, {{"taskId", "x"}}).envelope), -32003) << method;
    }
    for (const char* method : {"tasks/pushNotificationConfig/set", "tasks/pushNotificationConfig/get",
                               "tasks/pushNotificationConfig/list", "tasks/pushNotificationConfig/delete"}) {
        EXPECT_EQ(error_code(rpc(srv, method, {{"id", "x"}}, "").envelope), -32003) << method;
    }
    EXPECT_EQ(error_code(rpc(srv, "GetExtendedAgentCard", json::object()).envelope), -32004);
    EXPECT_EQ(error_code(rpc(srv, "agent/getAuthenticatedExtendedCard", json::object(), "").envelope),
              -32004);
    EXPECT_EQ(error_code(rpc(srv, "NoSuchMethod", json::object()).envelope), -32601);
}

TEST(A2AConformance, StreamingDisabledCardRejectsStreamingMethods) {
    Server srv(/*streaming=*/false);
    for (const char* method : {"SendStreamingMessage", "SubscribeToTask"}) {
        // An upfront error is a plain JSON-RPC response (as the SDK servers do).
        const auto r = rpc(srv, method, {{"message", v1_message("echo")}, {"id", "x"}});
        EXPECT_EQ(error_code(r.envelope), -32004) << method << ": " << r.body;
    }
}

TEST(A2AConformance, ListTasksPagesNewestFirstWithFilters) {
    Server srv;
    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(send_v1(srv, "echo " + std::to_string(i))["id"].get<std::string>());
        std::this_thread::sleep_for(3ms);
    }
    auto page = rpc(srv, "ListTasks", {{"pageSize", 2}}).envelope["result"];
    ASSERT_EQ(page["tasks"].size(), 2u);
    EXPECT_EQ(page["tasks"][0]["id"], ids[2]);
    EXPECT_EQ(page["tasks"][1]["id"], ids[1]);
    EXPECT_EQ(page["totalSize"], 3);
    EXPECT_EQ(page["pageSize"], 2);
    const auto token = page["nextPageToken"].get<std::string>();
    ASSERT_FALSE(token.empty());
    auto rest = rpc(srv, "ListTasks", {{"pageSize", 2}, {"pageToken", token}}).envelope["result"];
    ASSERT_EQ(rest["tasks"].size(), 1u);
    EXPECT_EQ(rest["tasks"][0]["id"], ids[0]);
    EXPECT_EQ(rest["nextPageToken"], "");
    // Artifacts are omitted unless asked for; historyLength 0 omits history.
    auto bare = rpc(srv, "ListTasks", {{"historyLength", 0}}).envelope["result"];
    EXPECT_FALSE(bare["tasks"][0].contains("history"));
    auto completed = rpc(srv, "ListTasks", {{"status", "TASK_STATE_FAILED"}}).envelope["result"];
    EXPECT_EQ(completed["tasks"].size(), 0u);
    EXPECT_EQ(error_code(rpc(srv, "ListTasks", {{"pageSize", 0}}).envelope), -32602);
    EXPECT_EQ(error_code(rpc(srv, "ListTasks", {{"pageToken", "zz"}}).envelope), -32602);
}

TEST(A2AConformance, SubscribeToTaskStreamsTheRunningTaskAndRefusesTerminalOnes) {
    Server srv;
    GateScope gate;
    auto started = rpc(srv, "SendMessage", {{"message", v1_message("gate subscribe")},
                                            {"configuration", {{"returnImmediately", true}}}});
    const auto id = started.envelope["result"]["task"]["id"].get<std::string>();
    // The run waits at the gate, so the task is running when the subscription
    // is made; the gate opens once the subscriber holds the current Task.
    const auto r = stream_rpc(srv, "SubscribeToTask", {{"id", id}}, [&](const std::string& so_far) {
        if (so_far.find("\n\n") != std::string::npos) gate.open();
    });
    const auto events = sse_events(r.body);
    ASSERT_GE(events.size(), 2u) << r.body;
    EXPECT_EQ(events.front()["result"]["task"]["id"], id);  // the current Task first
    EXPECT_EQ(events.back()["result"]["statusUpdate"]["status"]["state"], "TASK_STATE_COMPLETED");

    const auto terminal = rpc(srv, "SubscribeToTask", {{"id", id}});
    EXPECT_EQ(error_code(terminal.envelope), -32004);
    const auto unknown = rpc(srv, "SubscribeToTask", {{"id", "nope"}});
    EXPECT_EQ(error_code(unknown.envelope), -32001);
    // 0.3 clients read tasks/resubscribe only as an event stream.
    const auto legacy = rpc(srv, "tasks/resubscribe", {{"id", "nope"}}, "");
    EXPECT_EQ(error_code(sse_events(legacy.body).at(0)), -32001);
}

TEST(A2AConformance, ASubscriberJoiningAsTheRunFinishesGetsTheFinalStatusOrTheTerminalError) {
    Server srv;
    // Runs of 0 to 3 ms, so the subscription lands before, during and after
    // the final publication.
    for (int i = 0; i < 200; ++i) {
        auto started = rpc(srv, "SendMessage",
                           {{"message", v1_message("nap " + std::to_string((i % 30) * 100))},
                            {"configuration", {{"returnImmediately", true}}}});
        const auto id = started.envelope["result"]["task"]["id"].get<std::string>();
        const auto r = rpc(srv, "SubscribeToTask", {{"id", id}});
        if (r.envelope.is_object() && r.envelope.contains("error")) {
            ASSERT_EQ(error_code(r.envelope), -32004) << r.body;
            continue;
        }
        const auto events = sse_events(r.body);
        ASSERT_GE(events.size(), 2u) << r.body;
        EXPECT_EQ(events.front()["result"]["task"]["id"], id);
        EXPECT_EQ(events.back()["result"]["statusUpdate"]["status"]["state"], "TASK_STATE_COMPLETED")
            << r.body;
    }
}

TEST(A2AConformance, AQuietStreamIsKeptAliveWhileTheRunWorks) {
    Server srv;
    // A run can sit in one node longer than a client waits for bytes; the
    // stream says so with SSE comments instead of staying silent.
    neograph::a2a::test::A2AServerTestAccess::set_stream_keepalive(*srv.server, 40ms);
    GateScope gate;
    auto started = rpc(srv, "SendMessage", {{"message", v1_message("gate quiet")},
                                            {"configuration", {{"returnImmediately", true}}}});
    const auto id = started.envelope["result"]["task"]["id"].get<std::string>();
    const auto count_comments = [](const std::string& body) {
        std::size_t n = 0;
        for (auto at = body.find(": keep-alive"); at != std::string::npos;
             at = body.find(": keep-alive", at + 1)) {
            ++n;
        }
        return n;
    };
    // The gate stays shut until three comments have arrived, so the stream was
    // quiet for as long as it took to send them. Without them the read times out.
    const auto r = stream_rpc(srv, "SubscribeToTask", {{"id", id}},
                              [&](const std::string& so_far) {
                                  if (count_comments(so_far) >= 3) gate.open();
                              }, /*read_timeout_s=*/3);
    EXPECT_GE(count_comments(r.body), 3u);
    const auto events = sse_events(r.body);  // comments are not events
    ASSERT_GE(events.size(), 2u) << r.body;
    EXPECT_EQ(events.front()["result"]["task"]["id"], id);
    EXPECT_EQ(events.back()["result"]["statusUpdate"]["status"]["state"], "TASK_STATE_COMPLETED");
}

#ifdef NEOGRAPH_A2A_PROGRAM
TEST(A2AConformance, ASubscriberToARecoveredProgramTaskFollowsItsRunToTheFinalStatus) {
    // The server restarted while the request was accepted and its run had not
    // finished: the run is not this server's own, so no channel carries its
    // events. The stream still ends with the final status (#413).
    GateScope gate;
    RecoveryFixture fixture;
    Server srv(fixture.server_over(fixture.mailbox_with_pending_request()));
    ReleaseGate release;

    const auto running = rpc(srv, "GetTask", {{"id", "run-b"}}, "1.0", kCoordinator);
    ASSERT_EQ(running.envelope["result"]["status"]["state"], "TASK_STATE_WORKING") << running.body;

    // The run waits at the gate, so it is working when the subscription is
    // made; the gate opens once the subscriber holds the current Task.
    const auto r = stream_rpc(srv, "SubscribeToTask", {{"id", "run-b"}},
                              [&](const std::string& so_far) {
                                  if (so_far.find("\n\n") != std::string::npos) run_gate().open();
                              }, 10, kCoordinator);
    const auto events = sse_events(r.body);
    ASSERT_GE(events.size(), 2u) << r.body;
    EXPECT_EQ(events.front()["result"]["task"]["id"], "run-b");
    EXPECT_EQ(events.front()["result"]["task"]["status"]["state"], "TASK_STATE_WORKING");
    const auto& last = events.back()["result"]["statusUpdate"];
    EXPECT_EQ(last["status"]["state"], "TASK_STATE_COMPLETED") << r.body;
    EXPECT_EQ(last["taskId"], "run-b");
    EXPECT_TRUE(last["metadata"].contains("program_result_id")) << r.body;
    // The result artifact arrives before the final status, as for a run this
    // server started.
    bool saw_artifact = false;
    for (const auto& event : events) {
        if (event["result"].contains("artifactUpdate")) {
            saw_artifact = true;
            EXPECT_EQ(event["result"]["artifactUpdate"]["artifact"]["artifactId"],
                      "program-result:run-b");
        }
    }
    EXPECT_TRUE(saw_artifact) << r.body;

    // The task is terminal now, so there is nothing left to join.
    const auto again = rpc(srv, "SubscribeToTask", {{"id", "run-b"}}, "1.0", kCoordinator);
    EXPECT_EQ(error_code(again.envelope), -32004) << again.body;
    const auto fetched = rpc(srv, "GetTask", {{"id", "run-b"}}, "1.0", kCoordinator);
    EXPECT_EQ(fetched.envelope["result"]["status"]["state"], "TASK_STATE_COMPLETED");
}

TEST(A2AConformance, ASubscriberToARecoveredProgramTaskThatPausesEndsWithInputRequired) {
    GateScope gate;
    g_program_node_interrupts.store(true, std::memory_order_release);
    struct Reset { ~Reset() { g_program_node_interrupts.store(false, std::memory_order_release); } } reset;
    RecoveryFixture fixture;
    Server srv(fixture.server_over(fixture.mailbox_with_pending_request()));
    ReleaseGate release;

    const auto r = stream_rpc(srv, "SubscribeToTask", {{"id", "run-b"}},
                              [&](const std::string& so_far) {
                                  if (so_far.find("\n\n") != std::string::npos) run_gate().open();
                              }, 10, kCoordinator);
    const auto events = sse_events(r.body);
    ASSERT_GE(events.size(), 2u) << r.body;
    EXPECT_EQ(events.front()["result"]["task"]["status"]["state"], "TASK_STATE_WORKING");
    EXPECT_EQ(events.back()["result"]["statusUpdate"]["status"]["state"],
              "TASK_STATE_INPUT_REQUIRED") << r.body;
}

TEST(A2AConformance, ACancelEndsTheStreamOfARecoveredProgramTask) {
    GateScope gate;
    RecoveryFixture fixture;
    Server srv(fixture.server_over(fixture.mailbox_with_pending_request()));
    ReleaseGate release;

    bool cancelled = false;
    const auto r = stream_rpc(srv, "SubscribeToTask", {{"id", "run-b"}},
                              [&](const std::string& so_far) {
                                  if (cancelled || so_far.find("\n\n") == std::string::npos) return;
                                  cancelled = true;
                                  const auto reply = rpc(srv, "CancelTask", {{"id", "run-b"}}, "1.0",
                                                         kCoordinator);
                                  EXPECT_EQ(error_code(reply.envelope), 0) << reply.body;
                              }, 10, kCoordinator);
    const auto events = sse_events(r.body);
    ASSERT_GE(events.size(), 2u) << r.body;
    EXPECT_EQ(events.back()["result"]["statusUpdate"]["status"]["state"], "TASK_STATE_CANCELED")
        << r.body;
}

TEST(A2AConformance, AStreamThatCannotFollowARecoveredRunEndsWithAnErrorNotSilence) {
    // The run settles, but its result cannot be projected into a Task (here the
    // artifact builder throws): the stream says so.
    GateScope gate;
    RecoveryFixture fixture;
    Server srv(fixture.server_over(
        fixture.mailbox_with_pending_request(),
        [](const program::ProgramResult&, std::string_view) -> std::optional<Artifact> {
            throw std::runtime_error("the artifact store is down");
        }));
    ReleaseGate release;

    const auto r = stream_rpc(srv, "SubscribeToTask", {{"id", "run-b"}},
                              [&](const std::string& so_far) {
                                  if (so_far.find("\n\n") != std::string::npos) run_gate().open();
                              }, 10, kCoordinator);
    const auto events = sse_events(r.body);
    ASSERT_GE(events.size(), 2u) << r.body;
    EXPECT_EQ(events.front()["result"]["task"]["id"], "run-b");
    EXPECT_EQ(error_code(events.back()), -32603) << r.body;
    EXPECT_EQ(r.body.find("artifact store"), std::string::npos) << "no internal text: " << r.body;
}
#endif

// --- task rules (A2A-05, A2A-06, A2A-07, A2A-09, A2A-20) -----------------------------

TEST(A2AConformance, TaskIdsAreServerGeneratedAndFollowUpsAreChecked) {
    Server srv;
    EXPECT_EQ(error_code(rpc(srv, "SendMessage", {{"message", v1_message("echo", "client-chosen")}}).envelope),
              -32001);
    const auto done = send_v1(srv, "echo once");
    const auto id = done["id"].get<std::string>();
    EXPECT_TRUE(std::regex_match(id, std::regex("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")))
        << id;
    EXPECT_EQ(error_code(rpc(srv, "SendMessage",
                             {{"message", v1_message("echo again", id, done["contextId"].get<std::string>())}}).envelope),
              -32004);
    EXPECT_EQ(error_code(rpc(srv, "CancelTask", {{"id", id}}).envelope), -32002);
    EXPECT_EQ(rpc(srv, "GetTask", {{"id", id}}).envelope["result"]["status"]["state"],
              "TASK_STATE_COMPLETED");

    const auto paused = send_v1(srv, "interrupt please");
    EXPECT_EQ(error_code(rpc(srv, "SendMessage",
                             {{"message", v1_message("yes", paused["id"].get<std::string>(), "other")}}).envelope),
              -32602);
}

TEST(A2AConformance, InvalidParamsAreRejectedWithFieldViolations) {
    Server srv;
    const std::vector<json> cases = {
        json::object(),
        {{"message", {{"role", "ROLE_USER"}, {"parts", json::array({json{{"text", "x"}}})}}}},
        {{"message", {{"messageId", "m"}, {"role", "ROLE_USER"}, {"parts", json::array()}}}},
        {{"message", {{"messageId", "m"}, {"parts", json::array({json{{"text", "x"}}})}}}},
        {{"message", "not-an-object"}},
        {{"message", {{"messageId", "m"}, {"role", "ROLE_USER"},
                      {"parts", json::array({json{{"text", "x"}, {"url", "y"}}})}}}},
    };
    for (const auto& params : cases) {
        const auto r = rpc(srv, "SendMessage", params);
        EXPECT_EQ(error_code(r.envelope), -32602) << params.dump();
        EXPECT_EQ(r.envelope["error"]["data"][1]["@type"], "type.googleapis.com/google.rpc.BadRequest");
    }
    EXPECT_EQ(error_code(rpc(srv, "GetTask", json::object()).envelope), -32602);
    EXPECT_EQ(error_code(rpc(srv, "GetTask", {{"id", "x"}, {"historyLength", -1}}).envelope), -32602);
}

TEST(A2AConformance, ReturnImmediatelyAndHistoryLength) {
    Server srv;
    const auto start = std::chrono::steady_clock::now();
    auto r = rpc(srv, "SendMessage", {{"message", v1_message("slow")},
                                     {"configuration", {{"returnImmediately", true}}}});
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    const auto task = r.envelope["result"]["task"];
    EXPECT_EQ(task["status"]["state"], "TASK_STATE_SUBMITTED");
    EXPECT_TRUE(task["status"].contains("timestamp"));
    EXPECT_EQ(rpc(srv, "CancelTask", {{"id", task["id"]}}).envelope["result"]["status"]["state"],
              "TASK_STATE_CANCELED");

    auto none = rpc(srv, "SendMessage", {{"message", v1_message("echo h")},
                                        {"configuration", {{"historyLength", 0}}}});
    EXPECT_FALSE(none.envelope["result"]["task"].contains("history"));
    auto got = rpc(srv, "GetTask", {{"id", none.envelope["result"]["task"]["id"]}, {"historyLength", 1}});
    EXPECT_EQ(got.envelope["result"]["history"].size(), 1u);
}

// --- streams (A2A-01, A2A-14) -----------------------------------------------------

TEST(A2AConformance, LegacyStreamOpensWithTaskAndEndsOnFinal) {
    Server srv;
    json m = {{"kind", "message"}, {"messageId", "m-legacy"}, {"role", "user"},
              {"parts", json::array({json{{"kind", "text"}, {"text", "echo legacy"}}})}};
    const auto r = rpc(srv, "message/stream", {{"message", m}}, "");
    const auto events = sse_events(r.body);
    ASSERT_GE(events.size(), 3u) << r.body;
    EXPECT_EQ(events.front()["result"]["kind"], "task");
    const auto last = events.back()["result"];
    EXPECT_EQ(last["kind"], "status-update");
    EXPECT_EQ(last["final"], true);
    EXPECT_EQ(last["status"]["state"], "completed");
    for (std::size_t i = 0; i + 1 < events.size(); ++i) {
        EXPECT_NE(events[i]["result"].value("final", false), true) << "nothing after final";
    }
}

// --- failure mapping and the no-leak contract (A2A-15, D3) ----------------------------

TEST(A2AConformance, FailureClassesMapToStatesWithoutExceptionText) {
    Server srv;
    struct Case { const char* prompt; const char* state; const char* code; };
    for (const auto& c : {Case{"fail", "TASK_STATE_FAILED", "NG.TaskFailed"},
                          Case{"coded", "TASK_STATE_FAILED", "app.tool_error"},
                          Case{"logic", "TASK_STATE_FAILED", "NG.LogicError"},
                          Case{"hook", "TASK_STATE_REJECTED", "NG.HookBlocked"},
                          Case{"provider", "TASK_STATE_FAILED", "NG.ProviderFailed"},
                          Case{"budget", "TASK_STATE_FAILED", "NG.BudgetExhausted"},
                          Case{"refusal", "TASK_STATE_FAILED", "NG.SafetyBlocked"}}) {
        for (const bool streaming : {false, true}) {
            const auto r = rpc(srv, streaming ? "SendStreamingMessage" : "SendMessage",
                               {{"message", v1_message(c.prompt)}});
            EXPECT_EQ(r.body.find("hunter2"), std::string::npos) << c.prompt << ": " << r.body;
            EXPECT_EQ(r.body.find("/srv/secret"), std::string::npos) << c.prompt << ": " << r.body;
            json task;
            if (streaming) {
                const auto events = sse_events(r.body);
                ASSERT_FALSE(events.empty());
                const auto id = events.front()["result"]["task"]["id"];
                task = rpc(srv, "GetTask", {{"id", id}}).envelope["result"];
                EXPECT_EQ(events.back()["result"]["statusUpdate"]["status"]["state"], c.state);
            } else {
                task = r.envelope["result"]["task"];
            }
            EXPECT_EQ(task["status"]["state"], c.state) << c.prompt;
            EXPECT_EQ(task["metadata"]["neograph/failure"]["code"], c.code) << c.prompt;
            EXPECT_FALSE(task["status"]["message"]["parts"][0]["text"].get<std::string>().empty());
            const auto listed = rpc(srv, "ListTasks", {{"historyLength", 10}}).body;
            EXPECT_EQ(listed.find("hunter2"), std::string::npos);
        }
    }
    const auto coded = send_v1(srv, "coded");
    EXPECT_EQ(coded["status"]["message"]["parts"][0]["text"], "the tool failed");
    EXPECT_EQ(coded["metadata"]["neograph/failure"]["retryable"], false);
    const auto provider = send_v1(srv, "provider");
    EXPECT_EQ(provider["metadata"]["neograph/failure"]["retryable"], true);
}

TEST(A2AConformance, InterruptIsInputRequiredAndAFollowUpResumes) {
    Server srv;
    const auto paused = send_v1(srv, "interrupt approve");
    EXPECT_EQ(paused["status"]["state"], "TASK_STATE_INPUT_REQUIRED");
    const auto parts = paused["status"]["message"]["parts"];
    EXPECT_EQ(parts[0]["text"], "approve the probe?");
    EXPECT_EQ(parts[1]["data"], json({{"tool", "probe"}}));
    EXPECT_EQ(paused["metadata"]["neograph/interrupt"]["node"], "probe");

    const auto id = paused["id"].get<std::string>();
    const auto resumed = rpc(srv, "SendMessage",
        {{"message", v1_message("yes", id, paused["contextId"].get<std::string>())}});
    const auto task = resumed.envelope["result"]["task"];
    EXPECT_EQ(task["id"], id);
    EXPECT_EQ(task["status"]["state"], "TASK_STATE_COMPLETED");
    EXPECT_EQ(task["status"]["message"]["parts"][0]["text"], "resumed:yes");
}

TEST(A2AConformance, NativeClientResumesAnInputRequiredTask) {
    Server srv;
    A2AClient client(srv.url());
    const auto paused = client.send_message_sync("interrupt approve");
    ASSERT_EQ(paused.status.state, TaskState::InputRequired);
    const auto resumed = client.send_message_sync("approved", paused.id, paused.context_id);
    EXPECT_EQ(resumed.id, paused.id);
    EXPECT_EQ(resumed.status.state, TaskState::Completed);
    ASSERT_TRUE(resumed.status.message);
    EXPECT_EQ(resumed.status.message->parts[0].text, "resumed:approved");
}

TEST(A2AConformance, CallerNodeContinuesByContextAndResumesOnlyOpenTasks) {
    // A terminal task accepts no follow-up (spec 3.1.1), so the caller node
    // keeps a task id only while the task waits for input.
    Server srv;
    auto client = std::make_shared<A2AClient>(srv.url());
    A2ACallerNode node("caller", client);
    graph::GraphState state;
    for (const char* channel : {"prompt", "response", "response_task_id", "response_context_id"}) {
        state.init_channel(channel, graph::ReducerType::OVERWRITE,
                           [](const json&, const json& incoming) { return incoming; });
    }
    graph::RunContext ctx;
    auto call = [&](const std::string& prompt) {
        state.write("prompt", prompt);
        auto out = neograph::async::run_sync(node.run({state, ctx}));
        for (const auto& write : out.writes) state.write(write.channel, write.value);
        return state.get("response").get<std::string>();
    };
    EXPECT_EQ(call("echo one"), "[conformance] echo one");
    EXPECT_EQ(state.get("response_task_id"), json(""));
    const auto context = state.get("response_context_id").get<std::string>();
    EXPECT_EQ(call("echo two"), "[conformance] echo two");  // new task, same context
    EXPECT_EQ(state.get("response_context_id").get<std::string>(), context);

    EXPECT_EQ(call("interrupt now"), "approve the probe?");
    const auto open_task = state.get("response_task_id").get<std::string>();
    EXPECT_FALSE(open_task.empty());
    EXPECT_EQ(call("granted"), "resumed:granted");
    EXPECT_EQ(state.get("response_task_id"), json(""));
}

TEST(A2AConformance, AttemptTimeoutIsAFailedTaskThatMayBeRetried) {
    // NG.Timeout: the attempt ran past attempt_timeout_ms and stopped.
    // Failure-routing keys are read under schema_version >= 1 only.
    Server srv(true, true, json{{"schema_version", 1}, {"node_retry_policies",
        json{{"probe", json{{"attempt_timeout_ms", 100}, {"max_retries", 0}}}}}});
    const auto r = rpc(srv, "SendMessage", {{"message", v1_message("slow timeout")}});
    const auto task = r.envelope["result"]["task"];
    EXPECT_EQ(task["status"]["state"], "TASK_STATE_FAILED") << r.body;
    EXPECT_EQ(task["metadata"]["neograph/failure"]["code"], "NG.Timeout") << r.body;
    EXPECT_EQ(task["metadata"]["neograph/failure"]["retryable"], true) << r.body;
    EXPECT_EQ(r.body.find("timed out after"), std::string::npos) << "what() text on the wire";
}
