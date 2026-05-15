// neograph::grpc ToolService implementation. Built only with
// -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/tool_service.h>

#include "neograph.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace neograph::grpc {

namespace pb = neograph::v1;

namespace {

class ToolServiceImpl final : public pb::ToolService::Service {
public:
    explicit ToolServiceImpl(std::unordered_map<std::string, ToolFn> tools)
        : tools_(std::move(tools)) {}

    ::grpc::Status CallTool(::grpc::ServerContext*,
                            const pb::ToolCallRequest* req,
                            pb::ToolCallResponse* resp) override {
        auto it = tools_.find(req->name());
        if (it == tools_.end()) {
            resp->set_error("unknown tool: " + req->name());
            return ::grpc::Status::OK;
        }
        try {
            resp->set_result_json(it->second(req->arguments_json()));
        } catch (const std::exception& e) {
            resp->set_error(e.what());
        }
        return ::grpc::Status::OK;
    }

private:
    std::unordered_map<std::string, ToolFn> tools_;
};

}  // namespace

// ── Client side: GrpcRemoteTool ──────────────────────────────────────

struct GrpcRemoteTool::Impl {
    std::string name;
    std::string description;
    neograph::json parameters;
    std::unique_ptr<pb::ToolService::Stub> stub;
};

GrpcRemoteTool::GrpcRemoteTool(const std::string& target,
                               std::string name,
                               std::string description,
                               neograph::json parameters)
    : impl_(std::make_unique<Impl>()) {
    impl_->name = std::move(name);
    impl_->description = std::move(description);
    impl_->parameters = std::move(parameters);
    impl_->stub = pb::ToolService::NewStub(::grpc::CreateChannel(
        target, ::grpc::InsecureChannelCredentials()));
}

GrpcRemoteTool::~GrpcRemoteTool() = default;

ChatTool GrpcRemoteTool::get_definition() const {
    return ChatTool{impl_->name, impl_->description, impl_->parameters};
}

std::string GrpcRemoteTool::get_name() const { return impl_->name; }

std::string GrpcRemoteTool::execute(const neograph::json& arguments) {
    pb::ToolCallRequest req;
    req.set_name(impl_->name);
    req.set_arguments_json(arguments.dump());

    pb::ToolCallResponse resp;
    ::grpc::ClientContext ctx;
    ::grpc::Status st = impl_->stub->CallTool(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("gRPC CallTool transport error: " +
                                 st.error_message());
    if (!resp.error().empty())
        throw std::runtime_error(resp.error());  // tool threw remotely
    return resp.result_json();
}

void run_tool_server(const std::string& address,
                     std::unordered_map<std::string, ToolFn> tools) {
    ToolServiceImpl svc(std::move(tools));
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(address,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&svc);
    std::unique_ptr<::grpc::Server> server(builder.BuildAndStart());
    if (!server)
        throw std::runtime_error(
            "failed to start ToolService on " + address);
    server->Wait();
}

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
