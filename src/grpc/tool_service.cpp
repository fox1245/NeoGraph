// neograph::grpc ToolService implementation. Built only with
// -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/tool_service.h>

#include "neograph.grpc.pb.h"
#include "../core/text_safety.h"

#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <asio/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <neograph/graph/cancel.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
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
            resp->set_error("unknown tool: " + ::neograph::detail::valid_utf8(req->name()));
            resp->set_error_code("NG.ToolNotFound");
            resp->set_error_detail_json("{}");
            return ::grpc::Status::OK;
        }
        std::exception_ptr failure;
        try {
            // proto3 strings must be UTF-8: the result crosses as such.
            resp->set_result_json(::neograph::detail::valid_utf8(it->second(req->arguments_json())));
        } catch (...) {
            failure = std::current_exception();
        }
        if (!failure) return ::grpc::Status::OK;
        // Typed failures stay distinct on the wire (reference, "Protocol
        // failure mapping"): cancellation is the CANCELLED status; every
        // other failure is the payload error with its stable code.
        const auto verdict = neograph::graph::classify_failure(failure);
        std::string message = "unknown exception";
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& e) {
            message = ::neograph::detail::valid_utf8(e.what());
        } catch (...) {
        }
        if (verdict.failure_class == neograph::graph::NodeFailureClass::Cancelled)
            return ::grpc::Status(::grpc::StatusCode::CANCELLED, message);
        neograph::json detail_json = neograph::json::object();
        try {
            if (verdict.signal) std::rethrow_exception(verdict.signal);
        } catch (const neograph::HookBoundaryBlocked& blocked) {
            detail_json = {{"invocation_id", blocked.invocation_id()},
                           {"state", std::string(neograph::to_string(blocked.state()))}};
        } catch (const neograph::graph::NodeInterrupt& interrupt) {
            detail_json = {{"reason", interrupt.reason()}, {"value", interrupt.value()}};
        } catch (...) {
        }
        resp->set_error(message);
        resp->set_error_code(verdict.code);
        resp->set_error_detail_json(detail_json.dump());
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
    std::shared_ptr<pb::ToolService::Stub> stub;
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

namespace {

std::string response_or_throw(const ::grpc::Status& status, const pb::ToolCallResponse& resp) {
    if (status.error_code() == ::grpc::StatusCode::CANCELLED)
        throw neograph::graph::CancelledException("gRPC CallTool cancelled");
    if (status.error_code() == ::grpc::StatusCode::DEADLINE_EXCEEDED)
        throw asio::system_error(asio::error::timed_out, "gRPC CallTool deadline exceeded");
    if (!status.ok())
        throw std::runtime_error("gRPC CallTool transport error: " + status.error_message());
    if (!resp.error().empty())
        throw std::runtime_error(resp.error());  // tool threw remotely
    return resp.result_json();
}

}  // namespace

std::string GrpcRemoteTool::execute(const neograph::json& arguments) {
    pb::ToolCallRequest req;
    req.set_name(impl_->name);
    req.set_arguments_json(arguments.dump());

    pb::ToolCallResponse resp;
    ::grpc::ClientContext ctx;
    ::grpc::Status st = impl_->stub->CallTool(&ctx, req, &resp);
    return response_or_throw(st, resp);
}

asio::awaitable<std::string> GrpcRemoteTool::execute_async(
    const neograph::json& arguments, neograph::ToolExecutionContext context) {
    if (context.cancel_token) context.cancel_token->throw_if_cancelled("before gRPC CallTool");
    struct Call {
        pb::ToolCallRequest request;
        pb::ToolCallResponse response;
        ::grpc::ClientContext client;
        ::grpc::Status status;
        std::atomic<bool> done{false};
    };
    auto call = std::make_shared<Call>();
    call->request.set_name(impl_->name);
    call->request.set_arguments_json(arguments.dump());
    if (context.deadline) {
        const auto remaining = *context.deadline - std::chrono::steady_clock::now();
        call->client.set_deadline(
            std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining));
    }
    // The blocking stub runs on its own thread; this coroutine waits for it,
    // and cancels the RPC in flight when the run's token is cancelled. The
    // thread owns the call state, so a late completion touches nothing else.
    std::thread([call, stub = impl_->stub] {
        call->status = stub->CallTool(&call->client, call->request, &call->response);
        call->done.store(true, std::memory_order_release);
    }).detach();
    asio::steady_timer poll(co_await asio::this_coro::executor);
    bool cancelled = false;
    while (!call->done.load(std::memory_order_acquire)) {
        if (!cancelled && context.cancel_token && context.cancel_token->is_cancelled()) {
            call->client.TryCancel();
            cancelled = true;
        }
        poll.expires_after(std::chrono::milliseconds(5));
        co_await poll.async_wait(asio::use_awaitable);
    }
    if (cancelled) throw neograph::graph::CancelledException("gRPC CallTool cancelled");
    co_return response_or_throw(call->status, call->response);
}

std::unique_ptr<::grpc::Service> make_tool_service(
    std::unordered_map<std::string, ToolFn> tools) {
    return std::make_unique<ToolServiceImpl>(std::move(tools));
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
