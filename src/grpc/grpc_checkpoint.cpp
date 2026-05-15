// neograph::grpc CheckpointStore implementation (client + server).
//
// Ported from NexaGraph src/nexagraph/grpc_checkpoint.cpp. NexaGraph
// flat-mapped a few fields (channel_values_json etc.); NeoGraph's
// Checkpoint is richer (next_nodes vector, CheckpointPhase enum,
// barrier_state nested map, schema_version) so the whole record is one
// JSON blob — generalizing NexaGraph's per-field-json approach.
//
// Built only with -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/grpc_checkpoint.h>
#include <neograph/json.h>

#include "neograph.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <stdexcept>
#include <utility>

namespace neograph::grpc {

using neograph::graph::Checkpoint;
using neograph::graph::CheckpointStore;
using neograph::graph::CheckpointPhase;
namespace pb = neograph::v1;

// ── Checkpoint ⇄ JSON ────────────────────────────────────────────────

std::string checkpoint_to_json(const Checkpoint& cp) {
    neograph::json j;
    j["id"]               = cp.id;
    j["thread_id"]        = cp.thread_id;
    j["channel_values"]   = cp.channel_values;
    j["channel_versions"] = cp.channel_versions;
    j["parent_id"]        = cp.parent_id;
    j["current_node"]     = cp.current_node;

    neograph::json nn = neograph::json::array();
    for (const auto& n : cp.next_nodes) nn.push_back(n);
    j["next_nodes"] = std::move(nn);

    // enum → canonical wire string (to_string is the public API that
    // reproduces the legacy stringly-typed phase exactly).
    j["interrupt_phase"] = neograph::graph::to_string(cp.interrupt_phase);

    neograph::json bs = neograph::json::object();
    for (const auto& [barrier, ups] : cp.barrier_state) {
        neograph::json arr = neograph::json::array();
        for (const auto& u : ups) arr.push_back(u);
        bs[barrier] = std::move(arr);
    }
    j["barrier_state"] = std::move(bs);

    j["metadata"]       = cp.metadata;
    j["step"]           = cp.step;
    j["timestamp"]      = cp.timestamp;
    j["schema_version"] = cp.schema_version;
    return j.dump();
}

Checkpoint checkpoint_from_json(const std::string& s) {
    neograph::json j = neograph::json::parse(s.empty() ? "{}" : s);
    Checkpoint cp;
    cp.id            = j.value("id", std::string{});
    cp.thread_id     = j.value("thread_id", std::string{});
    cp.channel_values = j.contains("channel_values")
        ? j["channel_values"] : neograph::json::object();
    cp.channel_versions = j.contains("channel_versions")
        ? j["channel_versions"] : neograph::json::object();
    cp.parent_id     = j.value("parent_id", std::string{});
    cp.current_node  = j.value("current_node", std::string{});

    if (j.contains("next_nodes") && j["next_nodes"].is_array())
        for (const auto& n : j["next_nodes"])
            cp.next_nodes.push_back(n.get<std::string>());

    cp.interrupt_phase = neograph::graph::parse_checkpoint_phase(
        j.value("interrupt_phase", std::string{"completed"}));

    if (j.contains("barrier_state") && j["barrier_state"].is_object()) {
        for (auto [barrier, arr] : j["barrier_state"].items()) {
            std::set<std::string> ups;
            if (arr.is_array())
                for (const auto& u : arr) ups.insert(u.get<std::string>());
            cp.barrier_state[barrier] = std::move(ups);
        }
    }

    cp.metadata = j.contains("metadata") ? j["metadata"]
                                         : neograph::json::object();
    cp.step      = j.value("step", static_cast<int64_t>(0));
    cp.timestamp = j.value("timestamp", static_cast<int64_t>(0));
    cp.schema_version = j.value(
        "schema_version",
        static_cast<std::uint32_t>(neograph::graph::CHECKPOINT_SCHEMA_VERSION));
    return cp;
}

namespace {

void to_blob(const Checkpoint& cp, pb::CheckpointBlob* b) {
    b->set_id(cp.id);
    b->set_thread_id(cp.thread_id);
    b->set_checkpoint_json(checkpoint_to_json(cp));
}

Checkpoint from_blob(const pb::CheckpointBlob& b) {
    return checkpoint_from_json(b.checkpoint_json());
}

}  // namespace

// ── Client: GrpcCheckpointStore ──────────────────────────────────────

struct GrpcCheckpointStore::Impl {
    std::shared_ptr<::grpc::Channel> channel;
    std::unique_ptr<pb::CheckpointService::Stub> stub;
};

GrpcCheckpointStore::GrpcCheckpointStore(const std::string& target)
    : impl_(std::make_unique<Impl>()) {
    impl_->channel = ::grpc::CreateChannel(
        target, ::grpc::InsecureChannelCredentials());
    impl_->stub = pb::CheckpointService::NewStub(impl_->channel);
}

GrpcCheckpointStore::~GrpcCheckpointStore() = default;

void GrpcCheckpointStore::save(const Checkpoint& cp) {
    pb::SaveCheckpointRequest req;
    to_blob(cp, req.mutable_checkpoint());
    pb::StatusResponse resp;
    ::grpc::ClientContext ctx;
    auto st = impl_->stub->SaveCheckpoint(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("SaveCheckpoint RPC failed: "
                                 + st.error_message());
    if (!resp.ok())
        throw std::runtime_error("SaveCheckpoint engine error: "
                                 + resp.error());
}

std::optional<Checkpoint>
GrpcCheckpointStore::load_latest(const std::string& thread_id) {
    pb::LoadLatestRequest req;
    req.set_thread_id(thread_id);
    pb::CheckpointResponse resp;
    ::grpc::ClientContext ctx;
    auto st = impl_->stub->LoadLatestCheckpoint(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("LoadLatestCheckpoint RPC failed: "
                                 + st.error_message());
    if (!resp.found()) return std::nullopt;
    return from_blob(resp.checkpoint());
}

std::optional<Checkpoint>
GrpcCheckpointStore::load_by_id(const std::string& id) {
    pb::LoadByIdRequest req;
    req.set_id(id);
    pb::CheckpointResponse resp;
    ::grpc::ClientContext ctx;
    auto st = impl_->stub->LoadCheckpointById(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("LoadCheckpointById RPC failed: "
                                 + st.error_message());
    if (!resp.found()) return std::nullopt;
    return from_blob(resp.checkpoint());
}

std::vector<Checkpoint>
GrpcCheckpointStore::list(const std::string& thread_id, int limit) {
    pb::ListCheckpointsRequest req;
    req.set_thread_id(thread_id);
    req.set_limit(limit);
    pb::ListCheckpointsResponse resp;
    ::grpc::ClientContext ctx;
    auto st = impl_->stub->ListCheckpoints(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("ListCheckpoints RPC failed: "
                                 + st.error_message());
    std::vector<Checkpoint> out;
    out.reserve(resp.checkpoints_size());
    for (const auto& b : resp.checkpoints()) out.push_back(from_blob(b));
    return out;
}

void GrpcCheckpointStore::delete_thread(const std::string& thread_id) {
    pb::DeleteThreadRequest req;
    req.set_thread_id(thread_id);
    pb::StatusResponse resp;
    ::grpc::ClientContext ctx;
    auto st = impl_->stub->DeleteThreadCheckpoints(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("DeleteThreadCheckpoints RPC failed: "
                                 + st.error_message());
}

// ── Server: CheckpointServiceImpl ────────────────────────────────────

namespace {

class CheckpointServiceImpl final : public pb::CheckpointService::Service {
public:
    explicit CheckpointServiceImpl(std::shared_ptr<CheckpointStore> backend)
        : backend_(std::move(backend)) {}

    ::grpc::Status SaveCheckpoint(
            ::grpc::ServerContext*,
            const pb::SaveCheckpointRequest* req,
            pb::StatusResponse* resp) override {
        try {
            backend_->save(checkpoint_from_json(
                req->checkpoint().checkpoint_json()));
            resp->set_ok(true);
        } catch (const std::exception& e) {
            resp->set_ok(false);
            resp->set_error(e.what());
        }
        return ::grpc::Status::OK;
    }

    ::grpc::Status LoadLatestCheckpoint(
            ::grpc::ServerContext*,
            const pb::LoadLatestRequest* req,
            pb::CheckpointResponse* resp) override {
        try {
            auto cp = backend_->load_latest(req->thread_id());
            if (cp) { resp->set_found(true);
                      to_blob(*cp, resp->mutable_checkpoint()); }
            else      resp->set_found(false);
        } catch (const std::exception&) { resp->set_found(false); }
        return ::grpc::Status::OK;
    }

    ::grpc::Status LoadCheckpointById(
            ::grpc::ServerContext*,
            const pb::LoadByIdRequest* req,
            pb::CheckpointResponse* resp) override {
        try {
            auto cp = backend_->load_by_id(req->id());
            if (cp) { resp->set_found(true);
                      to_blob(*cp, resp->mutable_checkpoint()); }
            else      resp->set_found(false);
        } catch (const std::exception&) { resp->set_found(false); }
        return ::grpc::Status::OK;
    }

    ::grpc::Status ListCheckpoints(
            ::grpc::ServerContext*,
            const pb::ListCheckpointsRequest* req,
            pb::ListCheckpointsResponse* resp) override {
        try {
            int limit = req->limit() > 0 ? req->limit() : 100;
            for (const auto& cp : backend_->list(req->thread_id(), limit))
                to_blob(cp, resp->add_checkpoints());
        } catch (const std::exception&) { /* empty list on error */ }
        return ::grpc::Status::OK;
    }

    ::grpc::Status DeleteThreadCheckpoints(
            ::grpc::ServerContext*,
            const pb::DeleteThreadRequest* req,
            pb::StatusResponse* resp) override {
        try {
            backend_->delete_thread(req->thread_id());
            resp->set_ok(true);
        } catch (const std::exception& e) {
            resp->set_ok(false);
            resp->set_error(e.what());
        }
        return ::grpc::Status::OK;
    }

private:
    std::shared_ptr<CheckpointStore> backend_;
};

}  // namespace

void run_checkpoint_server(const std::string& address,
                           std::shared_ptr<CheckpointStore> backend) {
    CheckpointServiceImpl svc(std::move(backend));
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(address,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&svc);
    std::unique_ptr<::grpc::Server> server(builder.BuildAndStart());
    if (!server)
        throw std::runtime_error(
            "failed to start CheckpointService on " + address);
    server->Wait();
}

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
