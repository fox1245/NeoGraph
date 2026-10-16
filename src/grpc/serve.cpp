// The built-in gRPC servers' common loop: listen, health, stop.
//
// Built only with -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include "serve.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/server_builder.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace neograph::grpc::detail {
namespace {

// gRPC adds its default health service to the servers built while a
// process-wide flag is on. It is turned on only around this server's start
// and then set back, so the host's own servers keep the host's choice; the
// lock keeps two built-in servers that start at once from interleaving.
std::unique_ptr<::grpc::Server> build_with_health(::grpc::ServerBuilder& builder) {
    static std::mutex flag;
    std::lock_guard lock(flag);
    const bool previous = ::grpc::DefaultHealthCheckServiceEnabled();
    ::grpc::EnableDefaultHealthCheckService(true);
    std::unique_ptr<::grpc::Server> server;
    try {
        server = builder.BuildAndStart();
    } catch (...) {
        ::grpc::EnableDefaultHealthCheckService(previous);
        throw;
    }
    ::grpc::EnableDefaultHealthCheckService(previous);
    return server;
}

}  // namespace

void serve(const std::string& address, ::grpc::Service& service, const char* service_name,
           const ServeOptions& options, const char* what) {
    if (options.shutdown_grace < std::chrono::milliseconds::zero())
        throw std::invalid_argument("ServeOptions::shutdown_grace must not be negative");

    ::grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(address, ::grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = build_with_health(builder);
    if (!server) throw std::runtime_error(std::string("failed to start ") + what + " on " + address);
    auto* health = server->GetHealthCheckService();
    if (health) health->SetServingStatus(service_name, true);

    const auto stop = [&] {
        if (health) {
            health->SetServingStatus(false);
            health->Shutdown();  // and no later change
        }
        server->Shutdown(std::chrono::system_clock::now() + options.shutdown_grace);
        server->Wait();
    };
    if (options.on_serving) {
        try {
            options.on_serving(port);
        } catch (...) {
            stop();
            throw;
        }
    }
    if (!options.stop.stop_possible()) {
        server->Wait();
        return;
    }
    {
        std::mutex mutex;
        std::condition_variable_any requested;
        std::unique_lock lock(mutex);
        requested.wait(lock, options.stop, [] { return false; });
    }
    stop();
}

}  // namespace neograph::grpc::detail

#endif  // NEOGRAPH_HAVE_GRPC
