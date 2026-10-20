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
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <algorithm>
#include <future>
#include <set>
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
        // #500: an ordinary failure or a timeout has used up its node's one
        // attempt, so only its class allows a retry; the provider runtime,
        // not the node, retries a provider call.
        const bool by_class = kind == "ordinary" || kind == "timeout" || kind == "provider";
        EXPECT_EQ(info.metadata.at("retryable"), kind == "provider" ? "true" : "false");
        EXPECT_EQ(info.metadata.at("retryable_by_class"), by_class ? "true" : "false");
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
        EXPECT_EQ(info.metadata.at("retryable"), "false");
        EXPECT_EQ(info.metadata.at("retryable_by_class"), "false");
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
// #451: a node's exception text never crosses the wire, on the stream
// either: the engine's error events go out without it, retries included.
TEST(GraphHostGRPCContract, StreamEventsNeverCarryTheThrownText) {
    NodeFactory::instance().register_type("grpc_typed_failure_node",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FailingNode>(name);
        });
    auto graph = definition("grpc-stream-failure", "grpc_typed_failure_node");
    graph["schema_version"] = 1;
    graph["node_retry_policies"] = {{"worker", {{"max_retries", 1}, {"initial_delay_ms", 10}}}};
    auto service = neograph::grpc::make_graph_service(NodeContext{}, graph.dump());
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    auto stub = pb::GraphService::NewStub(::grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));

    ::grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    pb::RunGraphRequest request;
    request.set_thread_id("stream-thrown-text");
    request.set_input_json(json{{"prompt", "ordinary"}}.dump());
    auto stream = stub->RunGraphStream(&context, request);
    pb::GraphEvent event;
    int node_errors = 0;
    int retries = 0;
    while (stream->Read(&event)) {
        EXPECT_EQ(event.payload_json().find("plain failure"), std::string::npos)
            << "kind " << event.kind() << ": " << event.payload_json();
        if (event.kind() != pb::GraphEvent::DEBUG) continue;
        const auto payload = json::parse(event.payload_json());
        if (payload.value("error", "") != "node failed") continue;
        ++node_errors;
        if (payload.contains("retry")) ++retries;
    }
    const auto status = stream->Finish();
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::UNKNOWN) << status.error_message();
    EXPECT_EQ(error_info(status).reason, "TASK_FAILED");
    EXPECT_EQ(retries, 1);
    EXPECT_EQ(node_errors, 2) << "one retry and the node's final error";
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}

// Streams two tokens, then writes its response.
class TokenNode final : public GraphNode {
public:
    explicit TokenNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        if (input.stream_cb) {
            (*input.stream_cb)(GraphEvent{GraphEvent::Type::LLM_TOKEN, name_, json("he")});
            (*input.stream_cb)(GraphEvent{GraphEvent::Type::LLM_TOKEN, name_, json("llo")});
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"response", json("hello")});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// #452: stream_mode selects the events (StreamMode's bits; 0 is all of
// them), a state snapshot is VALUES and a node's channel write UPDATES.
TEST(GraphHostGRPCContract, StreamModeSelectsTheEvents) {
    NodeFactory::instance().register_type("grpc_token_node",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<TokenNode>(name);
        });
    auto service = neograph::grpc::make_graph_service(
        NodeContext{}, definition("grpc-stream-mode", "grpc_token_node").dump());
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    auto stub = pb::GraphService::NewStub(::grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));

    struct Seen {
        std::set<int> kinds;
        bool state_as_updates = false;
        bool update_without_state = true;
        bool values_are_state = true;
        int final_events = 0;
    };
    const auto run = [&](std::uint32_t mode) {
        ::grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        pb::RunGraphRequest request;
        request.set_thread_id("stream-mode-" + std::to_string(mode));
        request.set_input_json(json{{"prompt", "hi"}}.dump());
        request.set_stream_mode(mode);
        auto stream = stub->RunGraphStream(&context, request);
        pb::GraphEvent event;
        Seen seen;
        while (stream->Read(&event)) {
            seen.kinds.insert(event.kind());
            if (event.kind() == pb::GraphEvent::UPDATES && event.node() == "__state__")
                seen.state_as_updates = true;
            if (event.kind() == pb::GraphEvent::VALUES && event.node() != "__state__")
                seen.values_are_state = false;
            if (event.kind() == pb::GraphEvent::FINAL) ++seen.final_events;
        }
        const auto status = stream->Finish();
        EXPECT_TRUE(status.ok()) << status.error_message();
        EXPECT_EQ(seen.final_events, 1);
        return seen;
    };
    const auto kinds = [](std::initializer_list<int> values) { return std::set<int>(values); };
    {
        SCOPED_TRACE("unset: every event");
        const auto seen = run(0);
        EXPECT_EQ(seen.kinds, kinds({pb::GraphEvent::TOKEN, pb::GraphEvent::NODE_START,
                                     pb::GraphEvent::NODE_END, pb::GraphEvent::VALUES,
                                     pb::GraphEvent::UPDATES, pb::GraphEvent::FINAL}));
        EXPECT_FALSE(seen.state_as_updates);
        EXPECT_TRUE(seen.values_are_state);
    }
    {
        SCOPED_TRACE("TOKENS");
        EXPECT_EQ(run(2).kinds, kinds({pb::GraphEvent::TOKEN, pb::GraphEvent::FINAL}));
    }
    {
        SCOPED_TRACE("VALUES");
        const auto seen = run(4);
        EXPECT_EQ(seen.kinds, kinds({pb::GraphEvent::VALUES, pb::GraphEvent::FINAL}));
        EXPECT_TRUE(seen.values_are_state);
    }
    {
        SCOPED_TRACE("UPDATES");
        const auto seen = run(8);
        EXPECT_EQ(seen.kinds, kinds({pb::GraphEvent::UPDATES, pb::GraphEvent::FINAL}));
        EXPECT_FALSE(seen.state_as_updates);
    }
    {
        SCOPED_TRACE("EVENTS");
        EXPECT_EQ(run(1).kinds, kinds({pb::GraphEvent::NODE_START, pb::GraphEvent::NODE_END,
                                       pb::GraphEvent::FINAL}));
    }
    {
        SCOPED_TRACE("TOKENS | UPDATES");
        EXPECT_EQ(run(2 | 8).kinds,
                  kinds({pb::GraphEvent::TOKEN, pb::GraphEvent::UPDATES, pb::GraphEvent::FINAL}));
    }
    {
        SCOPED_TRACE("only a bit this version does not know");
        EXPECT_EQ(run(0x100).kinds, kinds({pb::GraphEvent::FINAL}));
    }
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}

// #453: the server stores a Checkpoint, not the JSON, so it refuses a record
// it could not keep whole instead of dropping part of it. Free JSON inside
// channel_values and metadata is kept.
TEST(GraphHostGRPCContract, CheckpointServerRefusesRecordsItCannotStoreWhole) {
    auto backend = std::make_shared<InMemoryCheckpointStore>();
    auto service = neograph::grpc::make_checkpoint_service(backend);
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    auto stub = pb::CheckpointService::NewStub(::grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));
    const auto save = [&](const std::string& id, const json& record) {
        ::grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        pb::SaveCheckpointRequest request;
        request.mutable_checkpoint()->set_id(id);
        request.mutable_checkpoint()->set_thread_id("thread");
        request.mutable_checkpoint()->set_checkpoint_json(record.dump());
        pb::StatusResponse response;
        return stub->SaveCheckpoint(&context, request, &response);
    };
    Checkpoint checkpoint;
    checkpoint.id = "cp-whole";
    checkpoint.thread_id = "thread";
    checkpoint.channel_values = json{{"channels", {{"x", {{"value", {{"future", 1}}}}}}}};
    checkpoint.metadata = json{{"nested", {{"future_field", true}}}};
    checkpoint.step = 1;
    checkpoint.timestamp = 1;
    const auto record = json::parse(neograph::grpc::checkpoint_to_json(checkpoint));
    ASSERT_TRUE(save("cp-whole", record).ok());
    const auto stored = backend->load_by_id("cp-whole");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->metadata, checkpoint.metadata);
    EXPECT_EQ(stored->channel_values, checkpoint.channel_values);

    auto extra = record;
    extra["id"] = "cp-extra";
    extra["future_field"] = json{{"kept", true}};
    const auto refused = save("cp-extra", extra);
    EXPECT_EQ(refused.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(refused.error_message().find("future_field"), std::string::npos)
        << refused.error_message();
    EXPECT_FALSE(backend->load_by_id("cp-extra"));

    auto newer = record;
    newer["id"] = "cp-newer";
    newer["schema_version"] = CHECKPOINT_SCHEMA_VERSION + 1;
    EXPECT_EQ(save("cp-newer", newer).error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_FALSE(backend->load_by_id("cp-newer"));
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}

// grpc.health.v1.Health/Check over a generic stub. Its messages are small
// enough to encode by hand.
struct RawCall {
    ::grpc::Status status;
    std::string response;
};

RawCall raw_unary(int port, const std::string& method, const std::string& request) {
    ::grpc::GenericStub stub(::grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                                   ::grpc::InsecureChannelCredentials()));
    ::grpc::Slice slice(request);
    ::grpc::ByteBuffer request_buffer(&slice, 1);
    ::grpc::ByteBuffer response_buffer;
    ::grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    std::promise<::grpc::Status> done;
    stub.UnaryCall(&context, method, ::grpc::StubOptions(), &request_buffer, &response_buffer,
                   [&done](::grpc::Status status) { done.set_value(std::move(status)); });
    RawCall call{done.get_future().get(), {}};
    std::vector<::grpc::Slice> slices;
    if (call.status.ok() && response_buffer.Dump(&slices).ok())
        for (const auto& part : slices)
            call.response.append(reinterpret_cast<const char*>(part.begin()), part.size());
    return call;
}

// HealthCheckRequest{service = 1}; the response's status is field 1 (SERVING = 1).
RawCall health_check(int port, const std::string& service) {
    std::string request;
    if (!service.empty()) {
        request.push_back('\x0a');
        request.push_back(static_cast<char>(service.size()));
        request += service;
    }
    return raw_unary(port, "/grpc.health.v1.Health/Check", request);
}
const std::string kServing("\x08\x01", 2);

// A built-in server on a thread, stopped through ServeOptions::stop.
class BuiltInServer {
public:
    template <class Run>
    explicit BuiltInServer(Run run) {
        auto serving = std::make_shared<std::promise<int>>();
        auto port = serving->get_future();
        thread_ = std::jthread([run = std::move(run), serving, this](std::stop_token stop) {
            neograph::grpc::ServeOptions options;
            options.stop = stop;
            options.shutdown_grace = 500ms;
            options.on_serving = [serving](int bound) { serving->set_value(bound); };
            try {
                run(options);
            } catch (...) {
                try { serving->set_exception(std::current_exception()); } catch (...) {}
            }
            returned_ = true;
        });
        if (port.wait_for(5s) != std::future_status::ready)
            throw std::runtime_error("built-in server did not start");
        port_ = port.get();
    }
    int port() const { return port_; }
    // Requests the stop and reports how long the server took to return.
    std::chrono::steady_clock::duration stop() {
        const auto started = std::chrono::steady_clock::now();
        thread_.request_stop();
        thread_.join();
        return std::chrono::steady_clock::now() - started;
    }
    bool returned() const { return returned_; }

private:
    int port_ = 0;
    std::atomic<bool> returned_{false};
    std::jthread thread_;  // last: joined before the members it writes go
};

// #454: the built-in servers answer the standard health service and can be
// stopped.
TEST(GraphHostGRPCContract, BuiltInServersAnswerHealthAndStop) {
    {
        SCOPED_TRACE("run_tool_server");
        BuiltInServer server([](const neograph::grpc::ServeOptions& options) {
            neograph::grpc::run_tool_server(
                "127.0.0.1:0",
                {{"echo", [](const std::string& arguments) { return arguments; }}}, options);
        });
        const auto overall = health_check(server.port(), "");
        ASSERT_TRUE(overall.status.ok()) << overall.status.error_message();
        EXPECT_EQ(overall.response, kServing);
        const auto named = health_check(server.port(), "neograph.v1.ToolService");
        ASSERT_TRUE(named.status.ok()) << named.status.error_message();
        EXPECT_EQ(named.response, kServing);
        EXPECT_EQ(health_check(server.port(), "no.such.Service").status.error_code(),
                  ::grpc::StatusCode::NOT_FOUND);
        EXPECT_LT(server.stop(), 3s);
        EXPECT_TRUE(server.returned());
    }
    {
        SCOPED_TRACE("run_server");
        BuiltInServer server([](const neograph::grpc::ServeOptions& options) {
            neograph::grpc::run_server("127.0.0.1:0", NodeContext{}, "", options);
        });
        const auto named = health_check(server.port(), "neograph.v1.GraphService");
        ASSERT_TRUE(named.status.ok()) << named.status.error_message();
        EXPECT_EQ(named.response, kServing);
        EXPECT_LT(server.stop(), 3s);
    }
    {
        SCOPED_TRACE("run_checkpoint_server");
        BuiltInServer server([](const neograph::grpc::ServeOptions& options) {
            neograph::grpc::run_checkpoint_server(
                "127.0.0.1:0", std::make_shared<InMemoryCheckpointStore>(), {}, options);
        });
        const auto named = health_check(server.port(), "neograph.v1.CheckpointService");
        ASSERT_TRUE(named.status.ok()) << named.status.error_message();
        EXPECT_EQ(named.response, kServing);
        EXPECT_LT(server.stop(), 3s);
    }
    // The process-wide default stays as the host left it: a server the host
    // builds itself after a built-in one has no health service.
    {
        auto service = neograph::grpc::make_tool_service({});
        ::grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service.get());
        auto own = builder.BuildAndStart();
        EXPECT_EQ(health_check(port, "").status.error_code(), ::grpc::StatusCode::UNIMPLEMENTED);
        own->Shutdown(std::chrono::system_clock::now() + 2s);
        own->Wait();
    }
    {
        // A stop requested before the server starts: it starts, then stops.
        std::stop_source stopped;
        stopped.request_stop();
        neograph::grpc::ServeOptions options;
        options.stop = stopped.get_token();
        int bound = 0;
        options.on_serving = [&bound](int port) { bound = port; };
        neograph::grpc::run_tool_server("127.0.0.1:0", {}, options);
        EXPECT_NE(bound, 0);
        options.shutdown_grace = -1ms;
        EXPECT_THROW(neograph::grpc::run_tool_server("127.0.0.1:0", {}, options),
                     std::invalid_argument);
    }
}

// GraphService.Health reports the version the library was built as.
TEST(GraphHostGRPCContract, HealthReportsTheLibraryVersion) {
    auto service = neograph::grpc::make_graph_service(NodeContext{});
    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    auto stub = pb::GraphService::NewStub(::grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));
    ::grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    pb::HealthResponse response;
    const auto status = stub->Health(&context, pb::HealthRequest{}, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.ok());
    EXPECT_EQ(response.version(), NEOGRAPH_EXPECTED_VERSION);
    EXPECT_FALSE(response.has_default_graph());
    server->Shutdown(std::chrono::system_clock::now() + 2s);
    server->Wait();
}

// A stop gives a call in flight the grace to finish.
TEST(GraphHostGRPCContract, StoppingABuiltInServerLetsACallInFlightFinish) {
    std::unordered_map<std::string, neograph::grpc::ToolFn> tools{
        {"slow", [](const std::string&) -> std::string {
             std::this_thread::sleep_for(300ms);
             return R"("done")";
         }},
    };
    BuiltInServer server([tools](const neograph::grpc::ServeOptions& options) {
        neograph::grpc::run_tool_server("127.0.0.1:0", tools, options);
    });
    auto stub = pb::ToolService::NewStub(::grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(server.port()), ::grpc::InsecureChannelCredentials()));
    auto call = std::async(std::launch::async, [&] {
        ::grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        pb::ToolCallRequest request;
        request.set_name("slow");
        request.set_arguments_json("{}");
        pb::ToolCallResponse response;
        const auto status = stub->CallTool(&context, request, &response);
        return std::make_pair(status, response.result_json());
    });
    std::this_thread::sleep_for(100ms);
    // The 500 ms grace covers the rest of the call: it completes.
    EXPECT_LT(server.stop(), 3s);
    const auto [status, result] = call.get();
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(result, R"("done")");
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
