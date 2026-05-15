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
