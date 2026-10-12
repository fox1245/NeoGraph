#include "graph_host_contract.h"
#include <neograph/grpc/graph_service.h>
#include <neograph/grpc/tool_service.h>
#include <neograph/grpc/grpc_checkpoint.h>
#include "../src/grpc/error_details.h"
#include <neograph/hook_outbox.h>
#include <neograph/async/run_sync.h>
#include <neograph/tool_dispatch.h>
#include <neograph/provider.h>
#include <asio/error.hpp>
#include <asio/system_error.hpp>
#include "neograph.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <algorithm>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace {
using namespace graph_host_contract;
namespace pb = neograph::v1;
namespace detail = neograph::grpc::detail;

class GRPCHost final : public Host {
public:
    GRPCHost(GraphFixture& fixture, std::size_t limit) {
        service_ = neograph::grpc::make_graph_service(
            fixture.context, fixture.graph.dump(), fixture.engine, limit);
        ::grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        if (!server_ || port == 0) throw std::runtime_error("gRPC contract server failed to listen");
        auto channel = ::grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                              ::grpc::InsecureChannelCredentials());
        stub_ = pb::GraphService::NewStub(channel);
    }
    ~GRPCHost() override { shutdown(); }

    std::future<Result> start(const std::string& identity,
                               const std::string& prompt, int max_steps) override {
        auto context = std::make_shared<::grpc::ClientContext>();
        context->set_deadline(std::chrono::system_clock::now() + 3s);
        {
            std::lock_guard lock(mu_);
            calls_[identity] = context;
        }
        return std::async(std::launch::async, [this, context, identity, prompt, max_steps] {
            CallGuard guard{this, identity, context};
            pb::RunGraphRequest request;
            request.set_thread_id(identity);
            request.set_input_json(json{{"prompt", prompt}}.dump());
            if (max_steps > 0) request.set_max_steps(max_steps);
            auto stream = stub_->RunGraphStream(context.get(), request);
            pb::GraphEvent event;
            Result result;
            bool final = false;
            bool error = false;
            while (stream->Read(&event)) {
                if (event.kind() == pb::GraphEvent::NODE_START)
                    result.events.push_back("node-start");
                if (event.kind() == pb::GraphEvent::NODE_END)
                    result.events.push_back("node-end");
                if (event.kind() == pb::GraphEvent::FINAL) {
                    final = true;
                    result.events.push_back("terminal");
                    const auto payload = json::parse(event.payload_json());
                    if (payload.value("max_steps_exhausted", false))
                        result.terminal = Terminal::max_steps;
                    else if (payload.value("interrupted", false))
                        result.terminal = Terminal::interrupted;
                    else result.terminal = Terminal::completed;
                    auto output = payload.value("output", json::object());
                    if (output.contains("channels") && output["channels"].contains("response")) {
                        const auto& value = output["channels"]["response"]["value"];
                        if (value.is_string()) result.output = value.get<std::string>();
                    }
                }
                if (event.kind() == pb::GraphEvent::DEBUG &&
                    !event.payload_json().empty()) {
                    const auto payload = json::parse(event.payload_json());
                    if (payload.is_object() && payload.contains("error")) error = true;
                }
            }
            const auto status = stream->Finish();
            if (status.error_code() == ::grpc::StatusCode::RESOURCE_EXHAUSTED)
                result.terminal = Terminal::rejected;
            else if (status.error_code() == ::grpc::StatusCode::CANCELLED ||
                     status.error_code() == ::grpc::StatusCode::DEADLINE_EXCEEDED)
                result.terminal = Terminal::cancelled;
            else if (!status.ok() || error) result.terminal = Terminal::error;
            else if (!final) throw std::runtime_error("gRPC stream returned no terminal graph result");
            if (!final) result.events.push_back("terminal");
            return result;
        });
    }
    void cancel(const std::string& identity) override {
        std::lock_guard lock(mu_);
        if (auto it = calls_.find(identity); it != calls_.end()) it->second->TryCancel();
    }
    void shutdown() override {
        if (!server_) return;
        {
            std::lock_guard lock(mu_);
            for (const auto& [_, context] : calls_) context->TryCancel();
        }
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        server_->Wait();
        server_.reset();
    }
private:
    struct CallGuard {
        GRPCHost* host;
        std::string identity;
        std::shared_ptr<::grpc::ClientContext> context;
        ~CallGuard() {
            std::lock_guard lock(host->mu_);
            auto it = host->calls_.find(identity);
            if (it != host->calls_.end() && it->second == context)
                host->calls_.erase(it);
        }
    };
    std::unique_ptr<pb::GraphService::Service> service_;
    std::unique_ptr<::grpc::Server> server_;
    std::unique_ptr<pb::GraphService::Stub> stub_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<::grpc::ClientContext>> calls_;
};

TEST(GraphHostGRPCContract, SharedBoundary) {
    check_contract([](GraphFixture& fixture, std::size_t limit) {
        return std::make_unique<GRPCHost>(fixture, limit);
    });
}

// ── Typed failures on the wire (reference, "Protocol failure mapping") ────

std::shared_ptr<const sp::Outcome> failed_provider(sp::ErrorKind kind) {
    sp::Failure failure;
    failure.error.kind = kind;
    failure.error.safe_message = "provider failed";
    return std::make_shared<const sp::Outcome>(std::move(failure));
}

// Raises the failure its prompt names.
[[noreturn]] void raise_failure(const std::string& kind) {
    if (kind == "hook")
        throw HookBoundaryBlocked("sha256:" + std::string(64, 'b'), HookExecutionState::Failed,
                                  "audit hook blocked");
    if (kind == "hook-timeout")
        throw HookBoundaryBlocked("sha256:" + std::string(64, 'c'), HookExecutionState::TimedOut,
                                  "audit hook timed out");
    if (kind == "timeout") throw asio::system_error(asio::error::timed_out, "too slow");
    if (kind == "provider") throw ProviderFailure(failed_provider(sp::ErrorKind::Overloaded));
    if (kind == "budget") throw ProviderFailure(failed_provider(sp::ErrorKind::QuotaExhausted));
    if (kind == "interrupt") throw NodeInterrupt("needs approval", json{{"cmd", "deploy"}});
    if (kind == "cancelled") throw CancelledException("inner run cancelled");
    throw std::runtime_error("plain failure");
}

class FailingNode final : public GraphNode {
public:
    explicit FailingNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto kind = input.state.get("prompt").get<std::string>();
        if (!kind.empty()) raise_failure(kind);
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

struct ServedGraph {
    std::unique_ptr<pb::GraphService::Service> service;
    std::unique_ptr<::grpc::Server> server;
    std::unique_ptr<pb::GraphService::Stub> stub;

    ServedGraph() {
        NodeFactory::instance().register_type("grpc_typed_failure_node",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<FailingNode>(name);
            });
        service = neograph::grpc::make_graph_service(
            NodeContext{}, definition("grpc-typed-failures", "grpc_typed_failure_node").dump());
        ::grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service.get());
        server = builder.BuildAndStart();
        stub = pb::GraphService::NewStub(::grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));
    }
    ~ServedGraph() {
        server->Shutdown(std::chrono::system_clock::now() + 2s);
        server->Wait();
    }
};

// The ErrorInfo of a failed call (google.rpc.Status details). Its reason
// follows AIP-193 (UPPER_SNAKE, at most 63 characters); the stable NeoGraph
// code is metadata["code"].
detail::ErrorInfo error_info(const ::grpc::Status& status) {
    const auto info = detail::error_info_of(status.error_details());
    EXPECT_TRUE(info) << "no google.rpc.ErrorInfo in the status details";
    const auto result = info.value_or(detail::ErrorInfo{});
    const auto& reason = result.reason;
    const bool upper_snake =
        reason.size() >= 2 && reason.size() <= 63 && reason.front() >= 'A' &&
        reason.front() <= 'Z' && reason.back() != '_' &&
        std::all_of(reason.begin(), reason.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        });
    EXPECT_TRUE(upper_snake) << "ErrorInfo.reason is not AIP-193 UPPER_SNAKE: " << reason;
    EXPECT_EQ(result.domain, "neograph");
    EXPECT_TRUE(result.metadata.contains("code")) << "no metadata[\"code\"] for " << reason;
    return result;
}

TEST(GraphHostGRPCContract, TypedGraphFailuresAreCanonicalStatuses) {
    ServedGraph served;
    const std::vector<std::tuple<std::string, ::grpc::StatusCode, std::string, std::string>> table{
        {"ordinary", ::grpc::StatusCode::UNKNOWN, "TASK_FAILED", "NG.TaskFailed"},
        {"timeout", ::grpc::StatusCode::DEADLINE_EXCEEDED, "TIMEOUT", "NG.Timeout"},
        {"provider", ::grpc::StatusCode::UNAVAILABLE, "PROVIDER_FAILED", "NG.ProviderFailed"},
        {"budget", ::grpc::StatusCode::RESOURCE_EXHAUSTED, "BUDGET_EXHAUSTED",
         "NG.BudgetExhausted"},
        {"hook", ::grpc::StatusCode::FAILED_PRECONDITION, "HOOK_BLOCKED", "NG.HookBlocked"},
        {"hook-timeout", ::grpc::StatusCode::DEADLINE_EXCEEDED, "HOOK_BLOCKED",
         "NG.HookBlocked"},
        {"cancelled", ::grpc::StatusCode::CANCELLED, "CANCELLED", "NG.Cancelled"},
    };
    for (const auto& [kind, code, reason, ng_code] : table) {
        SCOPED_TRACE(kind);
        ::grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        pb::RunGraphRequest request;
        request.set_thread_id("typed-" + kind);
        request.set_input_json(json{{"prompt", kind}}.dump());
        pb::RunGraphResponse response;
        const auto status = served.stub->RunGraph(&context, request, &response);
        EXPECT_EQ(status.error_code(), code) << status.error_message();
        const auto info = error_info(status);
        EXPECT_EQ(info.reason, reason);
        EXPECT_EQ(info.metadata.at("code"), ng_code);
        // The public message only: never the exception text.
        EXPECT_EQ(status.error_message().find("plain failure"), std::string::npos);
        EXPECT_EQ(status.error_message().find("too slow"), std::string::npos);
        if (kind == "hook") {
            EXPECT_EQ(info.metadata.at("invocation_id"), "sha256:" + std::string(64, 'b'));
            EXPECT_EQ(info.metadata.at("state"), "failed");
        }
    }
    {
        // A failed stream reports the failure as an event and then ends with
        // the same status as a unary call.
        ::grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        pb::RunGraphRequest request;
        request.set_thread_id("typed-stream-hook");
        request.set_input_json(json{{"prompt", "hook"}}.dump());
        auto stream = served.stub->RunGraphStream(&context, request);
        pb::GraphEvent event;
        std::string code;
        while (stream->Read(&event)) {
            if (event.kind() != pb::GraphEvent::DEBUG || event.payload_json().empty()) continue;
            const auto payload = json::parse(event.payload_json());
            if (payload.is_object() && payload.contains("error")) code = payload.value("code", "");
        }
        const auto status = stream->Finish();
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::FAILED_PRECONDITION);
        EXPECT_EQ(error_info(status).reason, "HOOK_BLOCKED");
        // The stream's event names the stable code, as the ErrorInfo metadata does.
        EXPECT_EQ(code, "NG.HookBlocked");
    }
}

TEST(GraphHostGRPCContract, InvalidGraphRequestsAreRefusedBeforeTheyRun) {
    ServedGraph served;
    const std::vector<std::tuple<std::string, std::string, std::string>> table{
        // thread id, input_json, graph_def_json
        {"", R"({"prompt":"hello"})", ""},
        {"bad-input", "[1, 2]", ""},
        {"bad-json", "{", ""},
        {"bad-graph", R"({"prompt":"hello"})", "{\"nodes\":"},
    };
    for (const auto& [thread, input, graph] : table) {
        SCOPED_TRACE(thread + " / " + input);
        ::grpc::ClientContext context;
        pb::RunGraphRequest request;
        request.set_thread_id(thread);
        request.set_input_json(input);
        request.set_graph_def_json(graph);
        pb::RunGraphResponse response;
        const auto status = served.stub->RunGraph(&context, request, &response);
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
        const auto info = error_info(status);
        EXPECT_EQ(info.reason, "INVALID_REQUEST");
        EXPECT_EQ(info.metadata.at("code"), "NG.InvalidRequest");
    }
}

TEST(GraphHostGRPCContract, ToolServiceKeepsTypedFailuresDistinct) {
    std::unordered_map<std::string, neograph::grpc::ToolFn> tools{
        {"fail", [](const std::string& arguments) -> std::string {
             raise_failure(json::parse(arguments).value("kind", ""));
         }},
        {"bytes", [](const std::string&) -> std::string { return "\"ok \xff\""; }},
    };
    auto service = neograph::grpc::make_tool_service(std::move(tools));
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    auto stub = pb::ToolService::NewStub(::grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));
    const auto call = [&](const std::string& name, const std::string& arguments,
                          pb::ToolCallResponse& response) {
        ::grpc::ClientContext context;
        pb::ToolCallRequest request;
        request.set_name(name);
        request.set_arguments_json(arguments);
        return stub->CallTool(&context, request, &response);
    };
    // A tool's own failure stays in the payload, like MCP isError.
    const std::vector<std::tuple<std::string, std::string>> in_band{
        {"ordinary", "NG.TaskFailed"}, {"timeout", "NG.Timeout"},
        {"provider", "NG.ProviderFailed"}, {"budget", "NG.BudgetExhausted"},
    };
    for (const auto& [kind, code] : in_band) {
        SCOPED_TRACE(kind);
        pb::ToolCallResponse response;
        ASSERT_TRUE(call("fail", json{{"kind", kind}}.dump(), response).ok());
        EXPECT_FALSE(response.error().empty());
        EXPECT_EQ(response.error().find("plain failure"), std::string::npos);
        EXPECT_EQ(response.error_code(), code);
    }
    // A run control signal is a status with ErrorInfo.
    const std::vector<std::tuple<std::string, ::grpc::StatusCode, std::string, std::string>> control{
        {"hook", ::grpc::StatusCode::FAILED_PRECONDITION, "HOOK_BLOCKED", "NG.HookBlocked"},
        {"interrupt", ::grpc::StatusCode::FAILED_PRECONDITION, "INTERRUPTED", "NG.Interrupted"},
        {"cancelled", ::grpc::StatusCode::CANCELLED, "CANCELLED", "NG.Cancelled"},
    };
    for (const auto& [kind, code, reason, ng_code] : control) {
        SCOPED_TRACE(kind);
        pb::ToolCallResponse response;
        const auto status = call("fail", json{{"kind", kind}}.dump(), response);
        EXPECT_EQ(status.error_code(), code);
        const auto info = error_info(status);
        EXPECT_EQ(info.reason, reason);
        EXPECT_EQ(info.metadata.at("code"), ng_code);
        if (kind == "interrupt") EXPECT_EQ(info.metadata.at("reason"), "needs approval");
    }
    {
        pb::ToolCallResponse response;
        const auto status = call("missing", "{}", response);
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::NOT_FOUND);
        const auto info = error_info(status);
        EXPECT_EQ(info.reason, "TOOL_NOT_FOUND");
        EXPECT_EQ(info.metadata.at("code"), "NG.ToolNotFound");
    }
    {
        pb::ToolCallResponse response;
        const auto status = call("fail", "not json", response);
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    }
    {
        // A result with ill-formed UTF-8 still crosses (proto3 strings are
        // UTF-8): U+FFFD, not a parse failure on the client.
        pb::ToolCallResponse response;
        ASSERT_TRUE(call("bytes", "{}", response).ok());
        EXPECT_EQ(response.result_json(), "\"ok \xEF\xBF\xBD\"");
    }
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}

// A checkpoint backend that is down.
class FailingCheckpointStore final : public CheckpointStore {
public:
    void save(const Checkpoint&) override { fail(); }
    std::optional<Checkpoint> load_latest(const std::string&) override { fail(); }
    std::optional<Checkpoint> load_by_id(const std::string&) override { fail(); }
    std::vector<Checkpoint> list(const std::string&, int) override { fail(); }
    void delete_thread(const std::string&) override { fail(); }

private:
    [[noreturn]] static void fail() {
        throw std::runtime_error("db password=hunter2 unreachable at /srv/secret");
    }
};

// A store outage is a failure, never "no checkpoint" (a resumed run would
// silently start over) or a successful delete; no backend text crosses.
TEST(GraphHostGRPCContract, CheckpointBackendFailuresAreStatuses) {
    auto service = neograph::grpc::make_checkpoint_service(
        std::make_shared<FailingCheckpointStore>());
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    neograph::grpc::GrpcCheckpointStore store("127.0.0.1:" + std::to_string(port));
    store.set_rpc_timeout(5s);
    const auto expect_failure = [](const std::function<void()>& operation) {
        try {
            operation();
            ADD_FAILURE() << "a backend outage looked like success";
        } catch (const std::exception& error) {
            EXPECT_EQ(std::string(error.what()).find("hunter2"), std::string::npos);
        }
    };
    expect_failure([&] { (void)store.load_latest("thread"); });
    expect_failure([&] { (void)store.load_by_id("id"); });
    expect_failure([&] { (void)store.list("thread", 10); });
    expect_failure([&] { store.delete_thread("thread"); });
    Checkpoint checkpoint;
    checkpoint.id = "cp-1";
    checkpoint.thread_id = "thread";
    expect_failure([&] { store.save(checkpoint); });

    // A malformed SaveCheckpoint is INVALID_ARGUMENT.
    auto stub = pb::CheckpointService::NewStub(::grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));
    ::grpc::ClientContext context;
    pb::SaveCheckpointRequest request;
    request.mutable_checkpoint()->set_id("other");
    request.mutable_checkpoint()->set_thread_id("thread");
    request.mutable_checkpoint()->set_checkpoint_json(
        neograph::grpc::checkpoint_to_json(checkpoint));
    pb::StatusResponse response;
    EXPECT_EQ(stub->SaveCheckpoint(&context, request, &response).error_code(),
              ::grpc::StatusCode::INVALID_ARGUMENT);
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}

// A remote gRPC tool is bounded by the run like an MCP tool: the run's cancel
// token cancels the RPC in flight, and the run's deadline is the RPC deadline.
TEST(GraphHostGRPCContract, RemoteToolCallsAreBoundedByTheRun) {
    std::unordered_map<std::string, neograph::grpc::ToolFn> tools{
        {"slow", [](const std::string&) -> std::string {
             std::this_thread::sleep_for(1500ms);
             return R"("late")";
         }},
    };
    auto service = neograph::grpc::make_tool_service(std::move(tools));
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    neograph::grpc::GrpcRemoteTool remote("127.0.0.1:" + std::to_string(port), "slow", "slow",
                                          json::object());
    auto controller = std::make_shared<ToolExecutionController>();
    {
        ToolExecutionContext context;
        context.controller = controller;
        context.cancel_token = std::make_shared<CancelToken>();
        auto token = context.cancel_token;
        std::jthread stop([token] {
            std::this_thread::sleep_for(100ms);
            token->cancel();
        });
        const auto started = std::chrono::steady_clock::now();
        EXPECT_THROW(neograph::async::run_sync(
                         dispatch_tool_calls({{"c1", "slow", "{}"}}, {&remote}, {}, {}, context)),
                     CancelledException);
        EXPECT_LT(std::chrono::steady_clock::now() - started, 1200ms);
    }
    {
        ToolExecutionContext context;
        context.controller = controller;
        context.deadline = std::chrono::steady_clock::now() + 100ms;
        const auto messages = neograph::async::run_sync(
            dispatch_tool_calls({{"c2", "slow", "{}"}}, {&remote}, {}, {}, context));
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages[0].tool_status, "timed_out") << messages[0].content;
    }
    server->Shutdown(std::chrono::system_clock::now() + 3s);
    server->Wait();
}
} // namespace

// A remote Hook block or interrupt is the remote's run control, not this
// run's: GrpcRemoteTool reports it as an ordinary failure carrying the remote
// code (NG.HookBlocked, NG.Interrupted), never retried, and the local run goes
// on with a failed tool result. Native human-in-the-loop propagation across
// protocols is a later phase.
TEST(GraphHostGRPCContract, RemoteRunControlSignalsAreOrdinaryTypedFailures) {
    std::unordered_map<std::string, neograph::grpc::ToolFn> tools{
        {"fail", [](const std::string& arguments) -> std::string {
             raise_failure(json::parse(arguments).value("kind", ""));
         }},
    };
    auto service = neograph::grpc::make_tool_service(std::move(tools));
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    neograph::grpc::GrpcRemoteTool remote("127.0.0.1:" + std::to_string(port), "fail", "fail",
                                          json::object());
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy read_only;
    read_only.effect = ToolEffectClass::ReadOnly;
    policies->upsert("fail", read_only);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});
    const std::vector<std::tuple<std::string, std::string>> cases{
        {"hook", "NG.HookBlocked"},
        {"hook-timeout", "NG.HookBlocked"},
        {"interrupt", "NG.Interrupted"},
    };
    for (const auto& [kind, code] : cases) {
        SCOPED_TRACE(kind);
        ToolExecutionContext context;
        context.controller = controller;
        // The classified failure: ordinary, the remote code, not retried, and
        // the remote's public message.
        try {
            (void)neograph::async::run_sync(
                remote.execute_async(json{{"kind", kind}}, context));
            ADD_FAILURE() << "a remote control signal must fail the call";
        } catch (...) {
            const auto verdict = classify_failure(std::current_exception());
            EXPECT_EQ(verdict.failure_class, NodeFailureClass::Ordinary);
            EXPECT_EQ(verdict.code, code);
            EXPECT_FALSE(verdict.retryable);
            EXPECT_FALSE(verdict.public_message.empty());
            EXPECT_EQ(verdict.public_message.find("audit hook"), std::string::npos)
                << "the remote public message, never the remote exception text";
        }
        // In a tool batch the local run does not stop: the call is a failed
        // tool result that names the code.
        std::vector<ChatMessage> messages;
        EXPECT_NO_THROW(messages = neograph::async::run_sync(dispatch_tool_calls(
                            {{"c-" + kind, "fail", json{{"kind", kind}}.dump()}}, {&remote}, {},
                            {}, context)));
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages[0].tool_status, "failed") << messages[0].content;
        EXPECT_NE(messages[0].content.find(code), std::string::npos) << messages[0].content;
    }
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}

// A request the ToolService refused before running anything (an unknown tool:
// NOT_FOUND; arguments that are not a JSON object: INVALID_ARGUMENT) has a
// known outcome, so it reaches the model as a plain failure, where a tool that
// ran and failed is reconciliation_required unless its policy rules out an
// effect (reference, "Tool failure contract"). The default policy is in force
// here: no tool declares an effect.
TEST(GraphHostGRPCContract, RefusedRemoteRequestsAreFailuresNotReconciliations) {
    std::unordered_map<std::string, neograph::grpc::ToolFn> tools{
        {"explode", [](const std::string&) -> std::string {
             throw std::runtime_error("remote tool exploded");
         }},
    };
    auto service = neograph::grpc::make_tool_service(std::move(tools));
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    const auto target = "127.0.0.1:" + std::to_string(port);
    neograph::grpc::GrpcRemoteTool missing(target, "missing", "not served", json::object());
    neograph::grpc::GrpcRemoteTool explode(target, "explode", "fails", json::object());
    ToolExecutionContext context;
    context.controller = std::make_shared<ToolExecutionController>();

    const auto messages = neograph::async::run_sync(dispatch_tool_calls(
        {{"c-missing", "missing", "{}"}, {"c-explode", "explode", "{}"}}, {&missing, &explode}, {},
        {}, context));
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].tool_status, "failed") << messages[0].content;
    EXPECT_FALSE(messages[0].tool_effect_uncertain) << messages[0].content;
    EXPECT_FALSE(messages[0].tool_retryable) << messages[0].content;
    EXPECT_NE(messages[0].content.find("NG.ToolNotFound"), std::string::npos)
        << "the model reads why: " << messages[0].content;
    EXPECT_EQ(messages[1].tool_status, "reconciliation_required") << messages[1].content;
    EXPECT_TRUE(messages[1].tool_effect_uncertain) << messages[1].content;

    // Arguments the service refuses as not being an object: the same.
    const auto refused = neograph::async::run_sync(
        context.controller->execute_result_async(explode, json::array({1, 2}), context));
    EXPECT_EQ(refused.status, ToolTerminalStatus::Failed) << refused.error;
    EXPECT_FALSE(refused.effect_uncertain) << refused.error;
    EXPECT_NE(refused.error.find("NG.InvalidRequest"), std::string::npos) << refused.error;
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}
