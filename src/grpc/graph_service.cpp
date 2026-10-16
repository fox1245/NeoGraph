// neograph::grpc service implementation.
//
// Built only with -DNEOGRAPH_BUILD_GRPC=ON. The opt-in CI
// grpc-graph-contract job installs grpc++/protoc and executes the host
// conformance target; the default-OFF build never probes for either.

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/graph_service.h>
#include <neograph/neograph.h>

#include "neograph.grpc.pb.h"   // generated into ${build}/grpc_gen
#include "error_details.h"
#include "serve.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <mutex>
#include <atomic>
#include <optional>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <string>
#include <unordered_map>
#include <neograph/graph/execution.h>

#ifndef NEOGRAPH_VERSION_STRING
#define NEOGRAPH_VERSION_STRING "dev"
#endif

namespace neograph::grpc {

using namespace neograph::graph;
namespace pb = neograph::v1;

class GraphServiceImpl final : public pb::GraphService::Service {
public:
    GraphServiceImpl(NodeContext ctx, std::string default_graph_json,
                     std::shared_ptr<GraphEngine> default_engine = {},
                     std::size_t max_inflight_runs = 0)
        : ctx_(std::move(ctx)), default_def_(std::move(default_graph_json)),
          max_inflight_runs_(max_inflight_runs) {
        if (default_engine && !default_def_.empty())
            cache_.emplace(default_def_, std::move(default_engine));
    }

    ::grpc::Status RunGraph(::grpc::ServerContext* sctx,
                            const pb::RunGraphRequest* req,
                            pb::RunGraphResponse* resp) override {
        // A malformed request is refused before admission and before any
        // graph runs (INVALID_ARGUMENT, or FAILED_PRECONDITION without a
        // graph to run).
        Prepared prepared;
        if (auto refused = prepare(*req, prepared)) {
            resp->set_error(refused->message);
            return refused->status();
        }
        Admission admission(*this);
        if (!admission.accepted()) return at_capacity().status();
        std::shared_ptr<CancelToken> token;
        std::optional<CancelWatcher> watcher;
        try {
            token = std::make_shared<CancelToken>();
            prepared.config.cancel_token = token;
            watcher.emplace(sctx, token);

            RunInvocationRequest request;
            request.config = std::move(prepared.config);
            RunInvocation invocation(*prepared.engine, std::move(request));
            auto outcome = invocation.run();
            watcher->stop();
            if (!outcome.succeeded() || !outcome.run_result) {
                // A failure is a canonical status with ErrorInfo
                // (reference, "Protocol failure mapping"). `error` is kept,
                // with the public message only, for one release.
                const auto failure = detail::wire_failure(failure_of(outcome));
                resp->set_error(failure.message);
                return failure.status();
            }

            const auto& r = *outcome.run_result;
            resp->set_output_json(r.output.dump());
            resp->set_interrupted(r.interrupted);
            resp->set_interrupt_node(r.interrupt_node);
            resp->set_interrupt_value_json(r.interrupt_value.dump());
            resp->set_checkpoint_id(r.checkpoint_id);
            resp->set_max_steps_exhausted(r.max_steps_exhausted());
            for (const auto& n : r.execution_trace)
                resp->add_execution_trace(n);
        } catch (...) {
            if (watcher) watcher->stop();
            const auto failure = detail::wire_failure(classify_failure(std::current_exception()));
            resp->set_error(failure.message);
            return failure.status();
        }
        return ::grpc::Status::OK;
    }

    ::grpc::Status RunGraphStream(
            ::grpc::ServerContext* sctx,
            const pb::RunGraphRequest* req,
            ::grpc::ServerWriter<pb::GraphEvent>* writer) override {
        Prepared prepared;
        if (auto refused = prepare(*req, prepared)) return refused->status();
        Admission admission(*this);
        if (!admission.accepted()) return at_capacity().status();
        std::shared_ptr<CancelToken> token;
        std::optional<CancelWatcher> watcher;
        // A failed stream reports the failure as a DEBUG event (kept for one
        // release) and then ends with the same status as a unary call.
        const auto fail = [writer](const detail::WireFailure& failure) {
            pb::GraphEvent err;
            err.set_kind(pb::GraphEvent::DEBUG);
            err.set_payload_json(neograph::json{{"error", failure.message},
                                                {"code", detail::error_code_of(failure.info)}}
                                     .dump());
            writer->Write(err);
            return failure.status();
        };
        try {
            token = std::make_shared<CancelToken>();
            prepared.config.cancel_token = token;

            RunInvocationRequest request;
            request.config = std::move(prepared.config);
            request.on_event = [writer, token](const GraphEvent& ev) {
                pb::GraphEvent out;
                out.set_node(ev.node_name);
                switch (ev.type) {
                    case GraphEvent::Type::LLM_TOKEN:
                        out.set_kind(pb::GraphEvent::TOKEN); break;
                    case GraphEvent::Type::NODE_START:
                        out.set_kind(pb::GraphEvent::NODE_START); break;
                    case GraphEvent::Type::NODE_END:
                        out.set_kind(pb::GraphEvent::NODE_END); break;
                    case GraphEvent::Type::CHANNEL_WRITE:
                        out.set_kind(pb::GraphEvent::UPDATES); break;
                    case GraphEvent::Type::INTERRUPT:
                    case GraphEvent::Type::ERROR:
                    default:
                        out.set_kind(pb::GraphEvent::DEBUG); break;
                }
                out.set_payload_json(ev.data.dump());
                if (!writer->Write(out)) token->cancel();
            };

            watcher.emplace(sctx, token);

            RunInvocation invocation(*prepared.engine, std::move(request));
            auto outcome = invocation.run();
            watcher->stop();
            if (outcome.cancelled()) return detail::wire_failure(failure_of(outcome)).status();
            if (!outcome.succeeded() || !outcome.run_result)
                return fail(detail::wire_failure(failure_of(outcome)));

            const auto& r = *outcome.run_result;
            pb::GraphEvent fin;
            fin.set_kind(pb::GraphEvent::FINAL);
            neograph::json f = {
                {"output",              r.output},
                {"interrupted",         r.interrupted},
                {"interrupt_node",      r.interrupt_node},
                {"interrupt_value",     r.interrupt_value},
                {"checkpoint_id",       r.checkpoint_id},
                {"execution_trace",     r.execution_trace},
                {"max_steps_exhausted", r.max_steps_exhausted()},
            };
            fin.set_payload_json(f.dump());
            if (!writer->Write(fin))
                return ::grpc::Status(::grpc::StatusCode::CANCELLED,
                                      "gRPC stream closed by client");
        } catch (...) {
            if (watcher) watcher->stop();
            return fail(detail::wire_failure(classify_failure(std::current_exception())));
        }
        return ::grpc::Status::OK;
    }

    ::grpc::Status Health(::grpc::ServerContext* /*sctx*/,
                          const pb::HealthRequest* /*req*/,
                          pb::HealthResponse* resp) override {
        resp->set_ok(true);
        resp->set_version(NEOGRAPH_VERSION_STRING);
        resp->set_has_default_graph(!default_def_.empty());
        return ::grpc::Status::OK;
    }

private:
    class CancelWatcher {
    public:
        CancelWatcher(::grpc::ServerContext* context, std::shared_ptr<CancelToken> token)
            : worker_([context, token = std::move(token)](std::stop_token stop) {
                while (!stop.stop_requested() && !context->IsCancelled())
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!stop.stop_requested() && context->IsCancelled()) token->cancel();
            }) {}
        ~CancelWatcher() { stop(); }
        void stop() {
            worker_.request_stop();
            if (worker_.joinable()) worker_.join();
        }
    private:
        std::jthread worker_;
    };
    class Admission {
    public:
        explicit Admission(GraphServiceImpl& owner) : owner_(owner) {
            auto current = owner_.inflight_runs_.load(std::memory_order_relaxed);
            while (owner_.max_inflight_runs_ == 0 ||
                   current < owner_.max_inflight_runs_) {
                if (owner_.inflight_runs_.compare_exchange_weak(
                        current, current + 1, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    accepted_ = true;
                    break;
                }
            }
        }
        ~Admission() {
            if (accepted_) owner_.inflight_runs_.fetch_sub(1, std::memory_order_release);
        }
        bool accepted() const noexcept { return accepted_; }
    private:
        GraphServiceImpl& owner_;
        bool accepted_ = false;
    };
    struct Prepared {
        std::optional<GraphExecution> engine;
        RunConfig config;
    };

    static FailureVerdict failure_of(const InvocationResult& outcome) {
        if (outcome.failure) return *outcome.failure;
        FailureVerdict verdict;
        verdict.failure_class = outcome.cancelled() ? NodeFailureClass::Cancelled
                                                    : NodeFailureClass::Unknown;
        verdict.code = node_failure_code(verdict.failure_class);
        return verdict;
    }

    static detail::WireFailure at_capacity() {
        return detail::request_failure(::grpc::StatusCode::RESOURCE_EXHAUSTED, "NG.Capacity",
                                       "graph host in-flight limit reached");
    }

    // Validates the request and resolves its graph; a refusal on failure.
    std::optional<detail::WireFailure> prepare(const pb::RunGraphRequest& req,
                                               Prepared& prepared) {
        const auto invalid = [](std::string message) {
            return detail::request_failure(::grpc::StatusCode::INVALID_ARGUMENT,
                                           "NG.InvalidRequest", std::move(message));
        };
        if (req.thread_id().empty()) return invalid("thread_id is required");
        RunConfig cfg;
        cfg.thread_id = req.thread_id();
        if (!req.input_json().empty()) {
            try {
                cfg.input = neograph::json::parse(req.input_json());
            } catch (const std::exception&) {
                return invalid("input_json is not valid JSON");
            }
            if (!cfg.input.is_object()) return invalid("input_json must be a JSON object");
        }
        if (req.max_steps() > 0)
            cfg.max_steps = static_cast<int>(req.max_steps());
        cfg.resume_if_exists = req.resume_if_exists();
        if (req.graph_def_json().empty() && default_def_.empty())
            return detail::request_failure(::grpc::StatusCode::FAILED_PRECONDITION, "NG.NoGraph",
                                           "graph_def_json is empty and no default graph is "
                                           "configured on this service");
        try {
            prepared.engine.emplace(get_engine(req.graph_def_json()));
        } catch (const std::exception&) {
            return invalid("graph_def_json does not compile");
        }
        prepared.config = std::move(cfg);
        return std::nullopt;
    }

    // Hash-keyed compile cache — the same multi-tenant pattern as the
    // multi_tenant_chatbot cookbook. Distinct graph_def → one engine,
    // shared across requests + threads (GraphEngine is concurrent-safe
    // with distinct thread_ids).
    GraphExecution get_engine(const std::string& def_json) {
        const std::string& key =
            def_json.empty() ? default_def_ : def_json;
        if (key.empty())
            throw std::runtime_error(
                "RunGraphRequest.graph_def_json empty and no default "
                "graph configured on this service");
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = cache_.find(key);
            if (it != cache_.end()) return it->second;
        }
        auto eng = GraphExecution(std::shared_ptr<GraphEngine>(
            GraphEngine::compile(neograph::json::parse(key), ctx_).release()));
        std::lock_guard<std::mutex> lk(mu_);
        auto [it, inserted] = cache_.emplace(key, eng);
        return it->second;
    }

    NodeContext ctx_;
    std::string default_def_;
    std::mutex  mu_;
    std::size_t max_inflight_runs_ = 0;
    std::atomic<std::size_t> inflight_runs_{0};
    std::unordered_map<std::string, GraphExecution> cache_;
};

std::unique_ptr<pb::GraphService::Service> make_graph_service(
        NodeContext ctx, std::string default_graph_json,
        std::shared_ptr<GraphEngine> default_engine,
        std::size_t max_inflight_runs) {
    return std::make_unique<GraphServiceImpl>(
        std::move(ctx), std::move(default_graph_json),
        std::move(default_engine), max_inflight_runs);
}

void run_server(const std::string& address,
                NodeContext ctx,
                std::string default_graph_json) {
    run_server(address, std::move(ctx), std::move(default_graph_json), ServeOptions{});
}

void run_server(const std::string& address,
                NodeContext ctx,
                std::string default_graph_json,
                const ServeOptions& options) {
    GraphServiceImpl svc(std::move(ctx), std::move(default_graph_json));
    detail::serve(address, svc, pb::GraphService::service_full_name(), options, "gRPC server");
}

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
