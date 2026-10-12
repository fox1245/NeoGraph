#include "graph_host_contract.h"
#include <neograph/grpc/graph_service.h>
#include <neograph/grpc/tool_service.h>
#include <neograph/hook_outbox.h>
#include <neograph/async/run_sync.h>
#include <neograph/tool_dispatch.h>
#include <neograph/provider.h>
#include <asio/error.hpp>
#include <asio/system_error.hpp>
#include "neograph.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <thread>
#include <tuple>
#include <unordered_map>

namespace {
using namespace graph_host_contract;
namespace pb = neograph::v1;

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

TEST(GraphHostGRPCContract, TypedGraphFailuresKeepTheirCode) {
    ServedGraph served;
    const std::vector<std::tuple<std::string, std::string>> table{
        {"ordinary", "NG.TaskFailed"},  {"timeout", "NG.Timeout"},
        {"provider", "NG.ProviderFailed"}, {"budget", "NG.BudgetExhausted"},
        {"hook", "NG.HookBlocked"},
    };
    for (const auto& [kind, code] : table) {
        SCOPED_TRACE(kind);
        ::grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        pb::RunGraphRequest request;
        request.set_thread_id("typed-" + kind);
        request.set_input_json(json{{"prompt", kind}}.dump());
        pb::RunGraphResponse response;
        const auto status = served.stub->RunGraph(&context, request, &response);
        ASSERT_TRUE(status.ok()) << status.error_message();
        EXPECT_FALSE(response.error().empty());
        EXPECT_EQ(response.error_code(), code);
        if (kind == "hook") {
            const auto detail = json::parse(response.error_detail_json());
            EXPECT_EQ(detail.value("invocation_id", ""), "sha256:" + std::string(64, 'b'));
            EXPECT_EQ(detail.value("state", ""), "failed");
        }
    }
    {
        ::grpc::ClientContext context;
        pb::RunGraphRequest request;
        request.set_thread_id("typed-cancelled");
        request.set_input_json(json{{"prompt", "cancelled"}}.dump());
        pb::RunGraphResponse response;
        const auto status = served.stub->RunGraph(&context, request, &response);
        EXPECT_EQ(status.error_code(), ::grpc::StatusCode::CANCELLED);
        EXPECT_EQ(response.error_code(), "NG.Cancelled");
    }
    {
        // A streamed failure carries the same code in its error event.
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
        EXPECT_TRUE(stream->Finish().ok());
        EXPECT_EQ(code, "NG.HookBlocked");
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
    const std::vector<std::tuple<std::string, std::string>> table{
        {"ordinary", "NG.TaskFailed"}, {"timeout", "NG.Timeout"},
        {"provider", "NG.ProviderFailed"}, {"budget", "NG.BudgetExhausted"},
        {"hook", "NG.HookBlocked"}, {"interrupt", "NG.Interrupted"},
    };
    for (const auto& [kind, code] : table) {
        SCOPED_TRACE(kind);
        ::grpc::ClientContext context;
        pb::ToolCallRequest request;
        request.set_name("fail");
        request.set_arguments_json(json{{"kind", kind}}.dump());
        pb::ToolCallResponse response;
        ASSERT_TRUE(stub->CallTool(&context, request, &response).ok());
        EXPECT_FALSE(response.error().empty());
        EXPECT_EQ(response.error_code(), code);
        if (kind == "interrupt")
            EXPECT_EQ(json::parse(response.error_detail_json()).value("reason", ""),
                      "needs approval");
    }
    {
        ::grpc::ClientContext context;
        pb::ToolCallRequest request;
        request.set_name("fail");
        request.set_arguments_json(json{{"kind", "cancelled"}}.dump());
        pb::ToolCallResponse response;
        EXPECT_EQ(stub->CallTool(&context, request, &response).error_code(),
                  ::grpc::StatusCode::CANCELLED);
    }
    {
        ::grpc::ClientContext context;
        pb::ToolCallRequest request;
        request.set_name("missing");
        pb::ToolCallResponse response;
        ASSERT_TRUE(stub->CallTool(&context, request, &response).ok());
        EXPECT_EQ(response.error_code(), "NG.ToolNotFound");
    }
    {
        // A result with ill-formed UTF-8 still crosses (proto3 strings are
        // UTF-8): U+FFFD, not a parse failure on the client.
        ::grpc::ClientContext context;
        pb::ToolCallRequest request;
        request.set_name("bytes");
        pb::ToolCallResponse response;
        ASSERT_TRUE(stub->CallTool(&context, request, &response).ok());
        EXPECT_EQ(response.result_json(), "\"ok \xEF\xBF\xBD\"");
    }
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
