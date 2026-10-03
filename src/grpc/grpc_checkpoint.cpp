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
#include "../core/canonical_json.h"
#include "../core/managed_budget_journal.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <stdexcept>
#include <utility>

namespace neograph::grpc {

using neograph::graph::Checkpoint;
using neograph::graph::CheckpointStore;
using neograph::graph::CheckpointPhase;
namespace pb = neograph::v1;
using neograph::graph::ManagedBudgetLeaseScope;
using neograph::graph::OwnedManagedBudgetLease;
using neograph::graph::ManagedBudgetEffectReceipt;
using neograph::graph::detail::ManagedBudgetJournalAccess;

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

neograph::json parse_journal_payload(const std::string& bytes) {
    auto value = neograph::detail::parse_json_strict(bytes);
    if (!value.is_object())
        throw std::invalid_argument("Managed-budget RPC requires an object payload");
    return value;
}

void require_rpc_success(const ::grpc::Status& status, const char* operation) {
    if (!status.ok())
        throw std::runtime_error(std::string(operation) + " RPC failed: " +
                                 status.error_message());
}

void to_scope(const ManagedBudgetLeaseScope& scope, pb::ManagedBudgetLeaseScope* value) {
    value->set_owner_scope(scope.owner_scope);
    value->set_thread_id(scope.thread_id);
    value->set_storage_thread_id(scope.storage_thread_id);
    value->set_graph_identity(scope.graph_identity);
    value->set_original_ceiling(scope.original_ceiling);
    value->set_has_original_deadline(scope.original_deadline_ticks.has_value());
    if (scope.original_deadline_ticks)
        value->set_original_deadline_ticks(*scope.original_deadline_ticks);
    value->set_deadline_clock_identity(scope.deadline_clock_identity);
}

ManagedBudgetLeaseScope from_scope(const pb::ManagedBudgetLeaseScope& value) {
    ManagedBudgetLeaseScope scope;
    scope.owner_scope = value.owner_scope();
    scope.thread_id = value.thread_id();
    scope.storage_thread_id = value.storage_thread_id();
    scope.graph_identity = value.graph_identity();
    scope.original_ceiling = value.original_ceiling();
    if (value.has_original_deadline())
        scope.original_deadline_ticks = value.original_deadline_ticks();
    else if (value.original_deadline_ticks() != 0)
        throw std::invalid_argument("Managed-budget deadline lacks presence");
    scope.deadline_clock_identity = value.deadline_clock_identity();
    return scope;
}

neograph::json encode_authority(const neograph::UsageAccumulator::AuthoritySnapshot& authority) {
    return {{"charged", authority.charged}, {"reserved", authority.reserved},
            {"provider_effects", authority.provider_effects},
            {"reports", neograph::provider_codec::encode_usage(authority.reports)},
            {"has_report", authority.has_report}};
}

neograph::UsageAccumulator::AuthoritySnapshot decode_authority(const neograph::json& value) {
    neograph::UsageAccumulator::AuthoritySnapshot authority;
    authority.charged = neograph::provider_codec::detail::counter(value.at("charged"));
    authority.reserved = neograph::provider_codec::detail::counter(value.at("reserved"));
    authority.provider_effects = value.at("provider_effects").get<std::vector<std::string>>();
    authority.reports = neograph::provider_codec::decode_usage(value.at("reports"));
    authority.has_report = value.at("has_report").get<bool>();
    if (encode_authority(authority) != value)
        throw std::invalid_argument("Noncanonical managed-budget authority snapshot");
    return authority;
}

std::string settlement_binding(const neograph::json& lease, const neograph::json& effect,
                               const neograph::json& authority) {
    return "neograph.grpc-managed-settlement/v1:" +
        neograph::detail::canonical_json_bytes(
            neograph::json{{"lease", lease}, {"effect", effect}, {"authority", authority}});
}

sp::runtime::Result authenticated_outcome(
    const neograph::json& value, const std::shared_ptr<sp::NativeArchive>& archive,
    const std::string& binding) {
    if (!archive || !value.contains("native_archive_reference") ||
        !value.at("native_archive_reference").is_string() ||
        value.at("native_archive_reference").get<std::string_view>().empty())
        throw std::invalid_argument("Managed-budget settlement requires genuine trusted archive custody");
    // The codec verifies archived metadata and exact projections and restores
    // native messages from SDK custody, never from the editable JSON projection.
    return neograph::provider_codec::decode_outcome(value, archive, binding);
}

void require_transportable_checkpoint(const Checkpoint& checkpoint) {
    // Do not silently discard in-memory-only native pointers. Durable archive
    // envelopes in channel_values already travel intact in CheckpointBlob.
    (void)neograph::graph::checkpoint_storage_metadata(checkpoint);
}

template<class Operation>
::grpc::Status journal_status(Operation&& operation) {
    try {
        operation();
        return ::grpc::Status::OK;
    } catch (const std::invalid_argument& error) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, error.what());
    } catch (const std::exception& error) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, error.what());
    } catch (...) {
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Managed-budget backend failure");
    }
}

}  // namespace

// ── Client: GrpcCheckpointStore ──────────────────────────────────────

struct GrpcCheckpointStore::Impl {
    std::shared_ptr<::grpc::Channel> channel;
    std::unique_ptr<pb::CheckpointService::Stub> stub;
    std::shared_ptr<sp::NativeArchive> native_archive;
};

GrpcCheckpointStore::GrpcCheckpointStore(const std::string& target)
    : GrpcCheckpointStore(target, {}) {}

GrpcCheckpointStore::GrpcCheckpointStore(
    const std::string& target, std::shared_ptr<sp::NativeArchive> native_archive)
    : impl_(std::make_unique<Impl>()) {
    impl_->native_archive = std::move(native_archive);
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

bool GrpcCheckpointStore::requires_managed_budget(const std::string& thread_id) {
    pb::RequiresManagedBudgetRequest req;
    req.set_thread_id(thread_id);
    pb::RequiresManagedBudgetResponse resp;
    ::grpc::ClientContext ctx;
    auto st = impl_->stub->RequiresManagedBudget(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("RequiresManagedBudget RPC failed: "
                                 + st.error_message());
    return resp.required();
}

std::shared_ptr<OwnedManagedBudgetLease> GrpcCheckpointStore::acquire_managed_budget_lease(
    const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
    const std::string& expected_checkpoint_commitment) {
    if (impl_->native_archive && impl_->native_archive->owner_scope() != scope.owner_scope)
        throw std::invalid_argument("Remote managed-budget archive owner differs from original scope");
    pb::AcquireManagedBudgetLeaseRequest request;
    to_scope(scope, request.mutable_scope());
    request.set_expected_checkpoint_id(expected_checkpoint_id);
    request.set_expected_checkpoint_commitment(expected_checkpoint_commitment);
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    require_rpc_success(impl_->stub->AcquireManagedBudgetLease(&context, request, &response),
                        "AcquireManagedBudgetLease");
    auto lease = ManagedBudgetJournalAccess::lease_from_transport(
        parse_journal_payload(response.lease_json()));
    if (!lease)
        throw std::runtime_error("AcquireManagedBudgetLease returned no owned receipt");
    const auto& actual = lease->scope();
    const auto storage_key = [](const ManagedBudgetLeaseScope& value) -> const std::string& {
        return value.storage_thread_id.empty() ? value.thread_id : value.storage_thread_id;
    };
    if (actual.owner_scope != scope.owner_scope || actual.thread_id != scope.thread_id ||
        storage_key(actual) != storage_key(scope) || actual.graph_identity != scope.graph_identity ||
        actual.original_ceiling != scope.original_ceiling ||
        actual.original_deadline_ticks != scope.original_deadline_ticks ||
        actual.deadline_clock_identity != scope.deadline_clock_identity ||
        lease->head_checkpoint_id() != expected_checkpoint_id ||
        lease->head_commitment() != expected_checkpoint_commitment)
        throw std::runtime_error("AcquireManagedBudgetLease returned a different source or scope");
    if (impl_->native_archive)
        ManagedBudgetJournalAccess::bind_native_archive(lease, impl_->native_archive);
    return lease;
}

ManagedBudgetEffectReceipt GrpcCheckpointStore::begin_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
    std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) {
    if (!impl_->native_archive)
        throw std::invalid_argument("Remote managed-budget effects require actual client archive activation");
    ManagedBudgetJournalAccess::bind_native_archive(lease, impl_->native_archive);
    pb::BeginManagedBudgetEffectRequest request;
    request.set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
    request.set_effect_id(effect_id);
    request.set_exact_claim_amount(exact_claim_amount);
    request.set_prepared_request_digest(prepared_request_digest);
    pb::ManagedBudgetEffectResponse response;
    ::grpc::ClientContext context;
    require_rpc_success(impl_->stub->BeginManagedBudgetEffect(&context, request, &response),
                        "BeginManagedBudgetEffect");
    auto effect = ManagedBudgetJournalAccess::effect_from_transport(
        parse_journal_payload(response.effect_json()));
    if (!effect.active() || effect.effect_id() != effect_id ||
        effect.claim_amount() != exact_claim_amount ||
        effect.request_digest() != prepared_request_digest)
        throw std::runtime_error("BeginManagedBudgetEffect returned a different claim");
    ManagedBudgetJournalAccess::refresh_transport(
        lease, parse_journal_payload(response.lease_json()));
    return effect;
}

void GrpcCheckpointStore::settle_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease,
    const ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
    const neograph::UsageAccumulator::AuthoritySnapshot& authority) {
    if (!genuine_outcome || !impl_->native_archive)
        throw std::invalid_argument("Remote settlement requires owned SDK outcome and trusted NativeArchive");
    const auto lease_claim = ManagedBudgetJournalAccess::lease_transport(lease);
    const auto effect_claim = ManagedBudgetJournalAccess::effect_transport(effect);
    const auto snapshot = encode_authority(authority);
    const auto binding = settlement_binding(lease_claim, effect_claim, snapshot);
    pb::SettleManagedBudgetEffectRequest request;
    request.set_lease_json(lease_claim.dump());
    request.set_effect_json(effect_claim.dump());
    request.set_authority_json(snapshot.dump());
    request.set_outcome_json(neograph::provider_codec::encode_outcome(
        *genuine_outcome, impl_->native_archive, binding).dump());
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    require_rpc_success(impl_->stub->SettleManagedBudgetEffect(&context, request, &response),
                        "SettleManagedBudgetEffect");
    ManagedBudgetJournalAccess::refresh_transport(
        lease, parse_journal_payload(response.lease_json()));
}

void GrpcCheckpointStore::publish_managed_budget_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint) {
    require_transportable_checkpoint(checkpoint);
    pb::PublishManagedBudgetCheckpointRequest request;
    request.set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
    to_blob(checkpoint, request.mutable_checkpoint());
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    require_rpc_success(impl_->stub->PublishManagedBudgetCheckpoint(&context, request, &response),
                        "PublishManagedBudgetCheckpoint");
    ManagedBudgetJournalAccess::refresh_transport(
        lease, parse_journal_payload(response.lease_json()));
}

void GrpcCheckpointStore::release_managed_budget_lease(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    pb::ReleaseManagedBudgetLeaseRequest request;
    request.set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    require_rpc_success(impl_->stub->ReleaseManagedBudgetLease(&context, request, &response),
                        "ReleaseManagedBudgetLease");
    ManagedBudgetJournalAccess::refresh_transport(
        lease, parse_journal_payload(response.lease_json()));
}

// ── Server: CheckpointServiceImpl ────────────────────────────────────

namespace {

class CheckpointServiceImpl final : public pb::CheckpointService::Service {
public:
    CheckpointServiceImpl(std::shared_ptr<CheckpointStore> backend,
                          std::shared_ptr<sp::NativeArchive> native_archive)
        : backend_(std::move(backend)), native_archive_(std::move(native_archive)) {
        if (!backend_)
            throw std::invalid_argument("CheckpointService requires a real backend");
    }

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

    ::grpc::Status RequiresManagedBudget(
            ::grpc::ServerContext*,
            const pb::RequiresManagedBudgetRequest* req,
            pb::RequiresManagedBudgetResponse* resp) override {
        try {
            resp->set_required(backend_->requires_managed_budget(req->thread_id()));
            return ::grpc::Status::OK;
        } catch (const std::exception& e) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
        } catch (...) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL,
                                  "RequiresManagedBudget backend failure");
        }
    }

    ::grpc::Status AcquireManagedBudgetLease(
        ::grpc::ServerContext*, const pb::AcquireManagedBudgetLeaseRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        return journal_status([&] {
            if (!request->has_scope())
                throw std::invalid_argument("Managed-budget acquire lacks scope");
            const auto scope = from_scope(request->scope());
            if (native_archive_ && native_archive_->owner_scope() != scope.owner_scope)
                throw std::invalid_argument("Managed-budget server archive owner differs from original scope");
            auto lease = backend_->acquire_managed_budget_lease(
                scope, request->expected_checkpoint_id(),
                request->expected_checkpoint_commitment());
            if (!lease)
                throw std::runtime_error("Managed-budget backend returned no owned lease");
            if (native_archive_)
                ManagedBudgetJournalAccess::bind_native_archive(lease, native_archive_);
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

    ::grpc::Status BeginManagedBudgetEffect(
        ::grpc::ServerContext*, const pb::BeginManagedBudgetEffectRequest* request,
        pb::ManagedBudgetEffectResponse* response) override {
        return journal_status([&] {
            auto lease = ManagedBudgetJournalAccess::lease_from_transport(
                parse_journal_payload(request->lease_json()));
            if (!native_archive_)
                throw std::invalid_argument("Remote managed-budget effects require actual server archive activation");
            ManagedBudgetJournalAccess::bind_native_archive(lease, native_archive_);
            auto effect = backend_->begin_managed_budget_effect(
                lease, request->effect_id(), request->exact_claim_amount(),
                request->prepared_request_digest());
            response->set_effect_json(ManagedBudgetJournalAccess::effect_transport(effect).dump());
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

    ::grpc::Status SettleManagedBudgetEffect(
        ::grpc::ServerContext*, const pb::SettleManagedBudgetEffectRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        return journal_status([&] {
            const auto lease_claim = parse_journal_payload(request->lease_json());
            const auto effect_claim = parse_journal_payload(request->effect_json());
            const auto snapshot = parse_journal_payload(request->authority_json());
            auto lease = ManagedBudgetJournalAccess::lease_from_transport(lease_claim);
            auto effect = ManagedBudgetJournalAccess::effect_from_transport(effect_claim);
            auto outcome = authenticated_outcome(
                parse_journal_payload(request->outcome_json()), native_archive_,
                settlement_binding(lease_claim, effect_claim, snapshot));
            ManagedBudgetJournalAccess::bind_native_archive(lease, native_archive_);
            const auto authority = decode_authority(snapshot);
            backend_->settle_managed_budget_effect(lease, effect, std::move(outcome), authority);
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

    ::grpc::Status PublishManagedBudgetCheckpoint(
        ::grpc::ServerContext*, const pb::PublishManagedBudgetCheckpointRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        return journal_status([&] {
            auto lease = ManagedBudgetJournalAccess::lease_from_transport(
                parse_journal_payload(request->lease_json()));
            if (!request->has_checkpoint())
                throw std::invalid_argument("Managed-budget publication lacks checkpoint");
            auto checkpoint = from_blob(request->checkpoint());
            if (checkpoint.id != request->checkpoint().id() ||
                checkpoint.thread_id != request->checkpoint().thread_id())
                throw std::invalid_argument("Managed-budget checkpoint indexed identity mismatch");
            require_transportable_checkpoint(checkpoint);
            backend_->publish_managed_budget_checkpoint(lease, checkpoint);
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

    ::grpc::Status ReleaseManagedBudgetLease(
        ::grpc::ServerContext*, const pb::ReleaseManagedBudgetLeaseRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        return journal_status([&] {
            auto lease = ManagedBudgetJournalAccess::lease_from_transport(
                parse_journal_payload(request->lease_json()));
            backend_->release_managed_budget_lease(lease);
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

private:
    std::shared_ptr<CheckpointStore> backend_;
    std::shared_ptr<sp::NativeArchive> native_archive_;
};

}  // namespace

void run_checkpoint_server(const std::string& address,
                           std::shared_ptr<CheckpointStore> backend) {
    run_checkpoint_server(address, std::move(backend), {});
}

void run_checkpoint_server(const std::string& address,
                           std::shared_ptr<CheckpointStore> backend,
                           std::shared_ptr<sp::NativeArchive> native_archive) {
    CheckpointServiceImpl svc(std::move(backend), std::move(native_archive));
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
