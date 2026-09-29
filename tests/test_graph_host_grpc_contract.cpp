#include "graph_host_contract.h"
#include <neograph/grpc/graph_service.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

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
} // namespace
