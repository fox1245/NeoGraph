// neograph::grpc service implementation.
//
// NOTE: not compiled in the reference CI environment (no grpc++/
// protoc). Built only when -DNEOGRAPH_BUILD_GRPC=ON. The OFF-default
// engine build is fully verified; confirm protoc codegen + this TU on
// the first grpc++-equipped build. See ROADMAP_v1.md.

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/graph_service.h>
#include <neograph/neograph.h>

#include "neograph.grpc.pb.h"   // generated into ${build}/grpc_gen

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifndef NEOGRAPH_VERSION_STRING
#define NEOGRAPH_VERSION_STRING "dev"
#endif

namespace neograph::grpc {

using namespace neograph::graph;
namespace pb = neograph::v1;

class GraphServiceImpl final : public pb::GraphService::Service {
public:
    GraphServiceImpl(NodeContext ctx, std::string default_graph_json)
        : ctx_(std::move(ctx)),
          default_def_(std::move(default_graph_json)) {}

    ::grpc::Status RunGraph(::grpc::ServerContext* /*sctx*/,
                            const pb::RunGraphRequest* req,
                            pb::RunGraphResponse* resp) override {
        try {
            auto engine = get_engine(req->graph_def_json());
            RunConfig cfg = build_config(*req);
            auto r = engine->run(cfg);

            resp->set_output_json(r.output.dump());
            resp->set_interrupted(r.interrupted);
            resp->set_interrupt_node(r.interrupt_node);
            resp->set_interrupt_value_json(r.interrupt_value.dump());
            resp->set_checkpoint_id(r.checkpoint_id);
            for (const auto& n : r.execution_trace)
                resp->add_execution_trace(n);
        } catch (const std::exception& e) {
            // Engine-side failure surfaces in the payload, not as a
            // transport error — caller distinguishes "graph threw"
            // from "gRPC broke".
            resp->set_error(e.what());
        }
        return ::grpc::Status::OK;
    }

    ::grpc::Status RunGraphStream(
            ::grpc::ServerContext* /*sctx*/,
            const pb::RunGraphRequest* req,
            ::grpc::ServerWriter<pb::GraphEvent>* writer) override {
        try {
            auto engine = get_engine(req->graph_def_json());
            RunConfig cfg = build_config(*req);

            auto r = engine->run_stream(cfg, [&](const GraphEvent& ev) {
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
                writer->Write(out);
            });

            pb::GraphEvent fin;
            fin.set_kind(pb::GraphEvent::FINAL);
            neograph::json f = {
                {"output",          r.output},
                {"interrupted",     r.interrupted},
                {"interrupt_node",  r.interrupt_node},
                {"checkpoint_id",   r.checkpoint_id},
                {"execution_trace", r.execution_trace},
            };
            fin.set_payload_json(f.dump());
            writer->Write(fin);
        } catch (const std::exception& e) {
            pb::GraphEvent err;
            err.set_kind(pb::GraphEvent::DEBUG);
            err.set_payload_json(
                neograph::json{{"error", e.what()}}.dump());
            writer->Write(err);
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
    RunConfig build_config(const pb::RunGraphRequest& req) {
        RunConfig cfg;
        cfg.thread_id = req.thread_id();
        if (!req.input_json().empty())
            cfg.input = neograph::json::parse(req.input_json());
        if (req.max_steps() > 0)
            cfg.max_steps = static_cast<int>(req.max_steps());
        cfg.resume_if_exists = req.resume_if_exists();
        return cfg;
    }

    // Hash-keyed compile cache — the same multi-tenant pattern as the
    // multi_tenant_chatbot cookbook. Distinct graph_def → one engine,
    // shared across requests + threads (GraphEngine is concurrent-safe
    // with distinct thread_ids).
    std::shared_ptr<GraphEngine> get_engine(const std::string& def_json) {
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
        auto eng = std::shared_ptr<GraphEngine>(
            GraphEngine::compile(neograph::json::parse(key), ctx_)
                .release());
        std::lock_guard<std::mutex> lk(mu_);
        auto [it, inserted] = cache_.emplace(key, eng);
        return it->second;
    }

    NodeContext ctx_;
    std::string default_def_;
    std::mutex  mu_;
    std::unordered_map<std::string, std::shared_ptr<GraphEngine>> cache_;
};

std::unique_ptr<GraphServiceImpl> make_graph_service(
        NodeContext ctx, std::string default_graph_json) {
    return std::make_unique<GraphServiceImpl>(
        std::move(ctx), std::move(default_graph_json));
}

void run_server(const std::string& address,
                NodeContext ctx,
                std::string default_graph_json) {
    GraphServiceImpl svc(std::move(ctx), std::move(default_graph_json));
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(address,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&svc);
    std::unique_ptr<::grpc::Server> server(builder.BuildAndStart());
    if (!server)
        throw std::runtime_error("failed to start gRPC server on " + address);
    server->Wait();
}

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
