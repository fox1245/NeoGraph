// neograph::grpc CheckpointStore implementation (client + server).
//
// A flat field-per-column mapping (channel_values_json etc.) cannot carry
// the whole Checkpoint: it also holds a next_nodes vector, a CheckpointPhase
// enum, a barrier_state nested map and schema_version. The whole record
// therefore travels as one JSON blob.
//
// Built only with -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/grpc/grpc_checkpoint.h>
#include <neograph/json.h>

#include "neograph.grpc.pb.h"
#include "error_details.h"
#include "serve.h"
#include "../core/canonical_json.h"
#include "../core/managed_budget_journal.h"
#include "../core/text_safety.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace neograph::grpc {

using neograph::graph::Checkpoint;
using neograph::graph::CheckpointStore;
using neograph::graph::CheckpointPhase;
using neograph::graph::CheckpointWriteFence;
using neograph::graph::CheckpointWriteFenceError;
using neograph::graph::CheckpointWriteFenceRefusal;
using neograph::graph::PendingWrite;
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

void to_write_fence(const CheckpointWriteFence& wf, pb::WriteFenceProto* out) {
    wf.validate();
    if (!wf.active()) return;
    out->set_owner_scope(wf.owner_scope);
    out->set_run_id(wf.run_id);
    out->set_fence(wf.fence);
}

CheckpointWriteFence from_write_fence(
    bool has_wf, const pb::WriteFenceProto& proto, bool require_fence = false) {
    if (!has_wf) {
        if (require_fence) {
            throw std::invalid_argument(
                "write_fence is required by fencing CheckpointService");
        }
        return CheckpointWriteFence{};
    }
    CheckpointWriteFence wf{proto.owner_scope(), proto.run_id(), proto.fence()};
    wf.validate();
    if (!wf.active() && require_fence) {
        throw std::invalid_argument(
            "write_fence requires non-empty run_id and positive fence");
    }
    return wf;
}

void to_pending_write(const PendingWrite& pw, pb::PendingWriteProto* out) {
    if (pw.native_result)
        throw std::invalid_argument("Native pending writes require explicit archive serialization");
    out->set_task_id(pw.task_id);
    out->set_task_path(pw.task_path);
    out->set_node_name(pw.node_name);
    out->set_writes_json(pw.writes.dump());
    out->set_command_json(pw.command.dump());
    out->set_sends_json(pw.sends.dump());
    out->set_step(pw.step);
    out->set_timestamp(pw.timestamp);
}

PendingWrite from_pending_write(const pb::PendingWriteProto& proto) {
    PendingWrite pw;
    pw.task_id = proto.task_id();
    pw.task_path = proto.task_path();
    pw.node_name = proto.node_name();
    pw.writes = neograph::json::parse(proto.writes_json().empty() ? "null" : proto.writes_json());
    pw.command = neograph::json::parse(proto.command_json().empty() ? "null" : proto.command_json());
    pw.sends = neograph::json::parse(proto.sends_json().empty() ? "null" : proto.sends_json());
    pw.step = proto.step();
    pw.timestamp = proto.timestamp();
    return pw;
}

std::optional<std::uint64_t> parse_positive_u64_json_field(const neograph::json& j,
                                                           const char* key) {
    if (!j.is_object() || !j.contains(key)) return std::nullopt;
    const auto& v = j[key];
    if (v.is_number_unsigned()) {
        const auto n = v.get<std::uint64_t>();
        return n > 0 ? std::optional<std::uint64_t>(n) : std::nullopt;
    }
    if (v.is_number_integer()) {
        const auto n = v.get<std::int64_t>();
        return n > 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(n)) : std::nullopt;
    }
    if (v.is_string()) {
        const auto s = v.get<std::string>();
        if (s.empty()) return std::nullopt;
        std::size_t idx = 0;
        unsigned long long parsed = 0;
        try {
            parsed = std::stoull(s, &idx, 10);
        } catch (...) {
            return std::nullopt;
        }
        if (idx != s.size() || parsed == 0) return std::nullopt;
        return static_cast<std::uint64_t>(parsed);
    }
    return std::nullopt;
}

::grpc::Status fence_refusal_status(const CheckpointWriteFenceError& err) {
    neograph::json detail = neograph::json::object();
    detail["code"] = CheckpointWriteFenceError::kErrorCode;
    detail["owner_scope"] = err.refusal().owner_scope;
    detail["run_id"] = err.refusal().run_id;
    detail["writer_fence"] = static_cast<unsigned long long>(err.refusal().writer_fence);
    detail["stored_fence"] = static_cast<unsigned long long>(err.refusal().stored_fence);
    return ::grpc::Status(
        ::grpc::StatusCode::FAILED_PRECONDITION, err.what(), detail.dump());
}

void rethrow_if_fence_refusal(const ::grpc::Status& status,
                              const CheckpointWriteFence& fallback = {}) {
    if (status.error_code() != ::grpc::StatusCode::FAILED_PRECONDITION) return;
    const std::string& details = status.error_details();
    if (details.empty()) return;
    neograph::json j;
    try {
        j = neograph::json::parse(details);
    } catch (const std::exception&) {
        return;
    }
    if (!j.is_object() || !j.contains("code") || !j["code"].is_string()) {
        return;
    }
    if (j["code"].get<std::string_view>() != CheckpointWriteFenceError::kErrorCode) {
        return;
    }
    const auto writer_fence = parse_positive_u64_json_field(j, "writer_fence");
    const auto stored_fence = parse_positive_u64_json_field(j, "stored_fence");
    const std::string run_id =
        (j.contains("run_id") && j["run_id"].is_string())
            ? j["run_id"].get<std::string>()
            : std::string{};
    if (!writer_fence || !stored_fence || run_id.empty()) {
        throw std::runtime_error(
            "Malformed checkpoint write fence refusal from gRPC server: missing run_id or fence values");
    }
    CheckpointWriteFenceRefusal refusal;
    refusal.owner_scope =
        (j.contains("owner_scope") && j["owner_scope"].is_string())
            ? j["owner_scope"].get<std::string>()
            : fallback.owner_scope;
    refusal.run_id = run_id;
    refusal.writer_fence = *writer_fence;
    refusal.stored_fence = *stored_fence;
    throw CheckpointWriteFenceError(std::move(refusal));
}

neograph::json parse_journal_payload(const std::string& bytes) {
    auto value = neograph::detail::parse_json_strict(bytes);
    if (!value.is_object())
        throw std::invalid_argument("Managed-budget RPC requires an object payload");
    return value;
}

void require_rpc_success(const ::grpc::Status& status, const char* operation) {
    if (!status.ok()) {
        rethrow_if_fence_refusal(status);
        throw std::runtime_error(std::string(operation) + " RPC failed: " +
                                 status.error_message());
    }
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
    } catch (const CheckpointWriteFenceError& err) {
        return fence_refusal_status(err);
    } catch (const std::invalid_argument& error) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, error.what());
    } catch (...) {
        // Backend exception text never crosses the wire.
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Managed-budget backend failure");
    }
}

// The members checkpoint_to_json writes. channel_values and metadata hold
// free JSON and are kept as they are; anything else at the top level would
// be lost, because the backend stores a Checkpoint, not the JSON.
constexpr std::string_view kCheckpointMembers[] = {
    "id", "thread_id", "channel_values", "channel_versions", "parent_id",
    "current_node", "next_nodes", "interrupt_phase", "barrier_state",
    "metadata", "step", "timestamp", "schema_version"};

// Why this server cannot store `checkpoint_json` whole (a member it does not
// know, a layout newer than its own), or nothing when it can. A record it
// cannot parse is left to checkpoint_from_json.
std::optional<std::string> unstorable(const std::string& checkpoint_json) {
    neograph::json record;
    try {
        record = neograph::json::parse(checkpoint_json.empty() ? "{}" : checkpoint_json);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!record.is_object()) return std::nullopt;
    for (const auto& [key, value] : record.items()) {
        (void)value;
        if (std::find(std::begin(kCheckpointMembers), std::end(kCheckpointMembers), key) ==
            std::end(kCheckpointMembers))
            return "checkpoint_json member '" +
                   neograph::detail::sanitize_untrusted_text(key, 64) +
                   "' is not one this server stores";
    }
    if (record.contains("schema_version") && record["schema_version"].is_number_integer()) {
        const auto version = record["schema_version"].get<std::int64_t>();
        if (version > static_cast<std::int64_t>(neograph::graph::CHECKPOINT_SCHEMA_VERSION))
            return "checkpoint schema_version " + std::to_string(version) +
                   " is newer than this server's " +
                   std::to_string(neograph::graph::CHECKPOINT_SCHEMA_VERSION);
    }
    return std::nullopt;
}

::grpc::Status invalid_request(std::string message) {
    return detail::request_failure(::grpc::StatusCode::INVALID_ARGUMENT, "NG.InvalidRequest",
                                   std::move(message))
        .status();
}

::grpc::Status backend_failure() {
    return detail::request_failure(::grpc::StatusCode::UNAVAILABLE, "NG.BackendUnavailable",
                                   "checkpoint backend failed")
        .status();
}

}  // namespace

// ── Client: GrpcCheckpointStore ──────────────────────────────────────

struct GrpcCheckpointStore::Impl {
    enum class FenceCapabilityState : std::uint8_t { Unknown, Supported, Unsupported };

    std::shared_ptr<::grpc::Channel> channel;
    std::unique_ptr<pb::CheckpointService::Stub> stub;
    std::shared_ptr<sp::NativeArchive> native_archive;
    std::chrono::milliseconds rpc_timeout{std::chrono::seconds(30)};
    mutable std::mutex cap_mu;
    mutable FenceCapabilityState fence_capability_state = FenceCapabilityState::Unknown;

    // Every RPC has a deadline, so a stalled server cannot hold a run.
    void bound(::grpc::ClientContext& context) const {
        context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                 rpc_timeout));
    }

    bool ensure_write_fence_capability() const {
        std::lock_guard<std::mutex> lock(cap_mu);
        if (fence_capability_state == FenceCapabilityState::Supported) return true;
        if (fence_capability_state == FenceCapabilityState::Unsupported) return false;
        pb::CheckpointCapabilitiesRequest req;
        pb::CheckpointCapabilitiesResponse resp;
        ::grpc::ClientContext ctx;
        bound(ctx);
        auto st = stub->GetCheckpointCapabilities(&ctx, req, &resp);
        if (st.ok()) {
            fence_capability_state = resp.supports_write_fence()
                ? FenceCapabilityState::Supported
                : FenceCapabilityState::Unsupported;
            return resp.supports_write_fence();
        }
        if (st.error_code() == ::grpc::StatusCode::UNIMPLEMENTED) {
            fence_capability_state = FenceCapabilityState::Unsupported;
            return false;
        }
        throw std::runtime_error(
            "gRPC GetCheckpointCapabilities failed: " + st.error_message());
    }

    CheckpointWriteFence resolve_fenced_write_capability(
        const CheckpointWriteFence& write_fence) const {
        write_fence.validate();
        if (!write_fence.active()) return {};
        return neograph::graph::resolve_store_write_fence(
            ensure_write_fence_capability(), write_fence);
    }
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

bool GrpcCheckpointStore::supports_write_fence() const {
    return impl_->ensure_write_fence_capability();
}

std::uint64_t GrpcCheckpointStore::raise_fence(
    const std::string& owner_scope,
    const std::string& run_id,
    std::uint64_t min_fence) {
    if (run_id.empty() || min_fence == 0) {
        throw std::invalid_argument(
            "raise_fence requires non-empty run_id and positive min_fence");
    }
    if (!impl_->ensure_write_fence_capability()) {
        throw neograph::graph::CheckpointException(
            "CheckpointStore does not support write fencing");
    }
    CheckpointWriteFence fence{owner_scope, run_id, min_fence};
    pb::RaiseCheckpointFenceRequest req;
    to_write_fence(fence, req.mutable_write_fence());
    req.set_owner_scope(owner_scope);
    req.set_run_id(run_id);
    req.set_new_fence(min_fence);
    pb::RaiseCheckpointFenceResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->RaiseCheckpointFence(&ctx, req, &resp);
    if (!st.ok()) {
        rethrow_if_fence_refusal(st, fence);
        throw std::runtime_error("RaiseCheckpointFence RPC failed: " + st.error_message());
    }
    if (!resp.ok())
        throw std::runtime_error("RaiseCheckpointFence engine error: " + resp.error());
    if (resp.stored_fence() == 0)
        throw std::runtime_error("RaiseCheckpointFence returned invalid stored_fence 0");
    return resp.stored_fence();
}

std::uint64_t GrpcCheckpointStore::load_fence(
    const std::string& owner_scope, const std::string& run_id) {
    if (run_id.empty()) {
        throw std::invalid_argument("load_fence requires non-empty run_id");
    }
    pb::LoadCheckpointFenceRequest req;
    req.set_owner_scope(owner_scope);
    req.set_run_id(run_id);
    pb::LoadCheckpointFenceResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->LoadCheckpointFence(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("LoadCheckpointFence RPC failed: " + st.error_message());
    if (!resp.found()) return 0;
    return resp.fence();
}

void GrpcCheckpointStore::save(const Checkpoint& cp) {
    save(cp, CheckpointWriteFence{});
}

void GrpcCheckpointStore::save(const Checkpoint& cp,
                               const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    pb::SaveCheckpointRequest req;
    to_blob(cp, req.mutable_checkpoint());
    if (effective_fence.active())
        to_write_fence(effective_fence, req.mutable_write_fence());
    pb::StatusResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->SaveCheckpoint(&ctx, req, &resp);
    if (!st.ok()) {
        rethrow_if_fence_refusal(st, effective_fence);
        throw std::runtime_error("SaveCheckpoint RPC failed: "
                                 + st.error_message());
    }
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
    impl_->bound(ctx);
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
    impl_->bound(ctx);
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
    impl_->bound(ctx);
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
    delete_thread(thread_id, CheckpointWriteFence{});
}

void GrpcCheckpointStore::delete_thread(const std::string& thread_id,
                                        const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    pb::DeleteThreadRequest req;
    req.set_thread_id(thread_id);
    if (effective_fence.active())
        to_write_fence(effective_fence, req.mutable_write_fence());
    pb::StatusResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->DeleteThreadCheckpoints(&ctx, req, &resp);
    if (!st.ok()) {
        rethrow_if_fence_refusal(st, effective_fence);
        throw std::runtime_error("DeleteThreadCheckpoints RPC failed: "
                                 + st.error_message());
    }
    // A server that reports the failure in the payload has not deleted.
    if (!resp.ok())
        throw std::runtime_error("DeleteThreadCheckpoints engine error: " + resp.error());
}

void GrpcCheckpointStore::put_writes(const std::string& thread_id,
                                     const std::string& parent_checkpoint_id,
                                     const PendingWrite& write) {
    put_writes(thread_id, parent_checkpoint_id, write, CheckpointWriteFence{});
}

void GrpcCheckpointStore::put_writes(const std::string& thread_id,
                                     const std::string& parent_checkpoint_id,
                                     const PendingWrite& write,
                                     const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    pb::PutCheckpointWritesRequest req;
    req.set_thread_id(thread_id);
    req.set_parent_checkpoint_id(parent_checkpoint_id);
    to_pending_write(write, req.mutable_write());
    if (effective_fence.active())
        to_write_fence(effective_fence, req.mutable_write_fence());
    pb::StatusResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->PutCheckpointWrites(&ctx, req, &resp);
    if (!st.ok()) {
        rethrow_if_fence_refusal(st, effective_fence);
        throw std::runtime_error("PutCheckpointWrites RPC failed: " + st.error_message());
    }
    if (!resp.ok())
        throw std::runtime_error("PutCheckpointWrites engine error: " + resp.error());
}

std::vector<PendingWrite> GrpcCheckpointStore::get_writes(
    const std::string& thread_id, const std::string& parent_checkpoint_id) {
    pb::GetCheckpointWritesRequest req;
    req.set_thread_id(thread_id);
    req.set_parent_checkpoint_id(parent_checkpoint_id);
    pb::GetCheckpointWritesResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->GetCheckpointWrites(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("GetCheckpointWrites RPC failed: " + st.error_message());
    std::vector<PendingWrite> out;
    out.reserve(resp.writes_size());
    for (const auto& w : resp.writes()) out.push_back(from_pending_write(w));
    return out;
}

void GrpcCheckpointStore::clear_writes(const std::string& thread_id,
                                       const std::string& parent_checkpoint_id) {
    clear_writes(thread_id, parent_checkpoint_id, CheckpointWriteFence{});
}

void GrpcCheckpointStore::clear_writes(const std::string& thread_id,
                                       const std::string& parent_checkpoint_id,
                                       const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    pb::ClearCheckpointWritesRequest req;
    req.set_thread_id(thread_id);
    req.set_parent_checkpoint_id(parent_checkpoint_id);
    if (effective_fence.active())
        to_write_fence(effective_fence, req.mutable_write_fence());
    pb::StatusResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->ClearCheckpointWrites(&ctx, req, &resp);
    if (!st.ok()) {
        rethrow_if_fence_refusal(st, effective_fence);
        throw std::runtime_error("ClearCheckpointWrites RPC failed: " + st.error_message());
    }
    if (!resp.ok())
        throw std::runtime_error("ClearCheckpointWrites engine error: " + resp.error());
}

void GrpcCheckpointStore::set_rpc_timeout(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("GrpcCheckpointStore RPC timeout must be positive");
    impl_->rpc_timeout = timeout;
}

bool GrpcCheckpointStore::requires_managed_budget(const std::string& thread_id) {
    pb::RequiresManagedBudgetRequest req;
    req.set_thread_id(thread_id);
    pb::RequiresManagedBudgetResponse resp;
    ::grpc::ClientContext ctx;
    impl_->bound(ctx);
    auto st = impl_->stub->RequiresManagedBudget(&ctx, req, &resp);
    if (!st.ok())
        throw std::runtime_error("RequiresManagedBudget RPC failed: "
                                 + st.error_message());
    return resp.required();
}

std::shared_ptr<OwnedManagedBudgetLease> GrpcCheckpointStore::acquire_managed_budget_lease(
    const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
    const std::string& expected_checkpoint_commitment) {
    return acquire_managed_budget_lease(
        scope, expected_checkpoint_id, expected_checkpoint_commitment, CheckpointWriteFence{});
}

std::shared_ptr<OwnedManagedBudgetLease> GrpcCheckpointStore::acquire_managed_budget_lease(
    const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
    const std::string& expected_checkpoint_commitment,
    const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    if (impl_->native_archive && impl_->native_archive->owner_scope() != scope.owner_scope)
        throw std::invalid_argument("Remote managed-budget archive owner differs from original scope");
    pb::AcquireManagedBudgetLeaseRequest request;
    to_scope(scope, request.mutable_scope());
    request.set_expected_checkpoint_id(expected_checkpoint_id);
    request.set_expected_checkpoint_commitment(expected_checkpoint_commitment);
    if (effective_fence.active())
        to_write_fence(effective_fence, request.mutable_write_fence());
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    impl_->bound(context);
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
    return begin_managed_budget_effect(
        lease, effect_id, exact_claim_amount, prepared_request_digest, CheckpointWriteFence{});
}

ManagedBudgetEffectReceipt GrpcCheckpointStore::begin_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
    std::uint64_t exact_claim_amount, const std::string& prepared_request_digest,
    const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    if (!impl_->native_archive)
        throw std::invalid_argument("Remote managed-budget effects require actual client archive activation");
    ManagedBudgetJournalAccess::bind_native_archive(lease, impl_->native_archive);
    pb::BeginManagedBudgetEffectRequest request;
    request.set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
    request.set_effect_id(effect_id);
    request.set_exact_claim_amount(exact_claim_amount);
    request.set_prepared_request_digest(prepared_request_digest);
    if (effective_fence.active())
        to_write_fence(effective_fence, request.mutable_write_fence());
    pb::ManagedBudgetEffectResponse response;
    ::grpc::ClientContext context;
    impl_->bound(context);
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
    settle_managed_budget_effect(
        lease, effect, std::move(genuine_outcome), authority, CheckpointWriteFence{});
}

void GrpcCheckpointStore::settle_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease,
    const ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
    const neograph::UsageAccumulator::AuthoritySnapshot& authority,
    const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
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
    if (effective_fence.active())
        to_write_fence(effective_fence, request.mutable_write_fence());
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    impl_->bound(context);
    require_rpc_success(impl_->stub->SettleManagedBudgetEffect(&context, request, &response),
                        "SettleManagedBudgetEffect");
    ManagedBudgetJournalAccess::refresh_transport(
        lease, parse_journal_payload(response.lease_json()));
}

void GrpcCheckpointStore::publish_managed_budget_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint) {
    publish_managed_budget_checkpoint(lease, checkpoint, CheckpointWriteFence{});
}

void GrpcCheckpointStore::publish_managed_budget_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint,
    const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    require_transportable_checkpoint(checkpoint);
    pb::PublishManagedBudgetCheckpointRequest request;
    request.set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
    to_blob(checkpoint, request.mutable_checkpoint());
    if (effective_fence.active())
        to_write_fence(effective_fence, request.mutable_write_fence());
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    impl_->bound(context);
    require_rpc_success(impl_->stub->PublishManagedBudgetCheckpoint(&context, request, &response),
                        "PublishManagedBudgetCheckpoint");
    ManagedBudgetJournalAccess::refresh_transport(
        lease, parse_journal_payload(response.lease_json()));
}

void GrpcCheckpointStore::release_managed_budget_lease(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    release_managed_budget_lease(lease, CheckpointWriteFence{});
}

void GrpcCheckpointStore::release_managed_budget_lease(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease,
    const CheckpointWriteFence& write_fence) {
    const auto effective_fence = impl_->resolve_fenced_write_capability(write_fence);
    pb::ReleaseManagedBudgetLeaseRequest request;
    request.set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
    if (effective_fence.active())
        to_write_fence(effective_fence, request.mutable_write_fence());
    pb::ManagedBudgetLeaseResponse response;
    ::grpc::ClientContext context;
    impl_->bound(context);
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

    ::grpc::Status GetCheckpointCapabilities(
            ::grpc::ServerContext*,
            const pb::CheckpointCapabilitiesRequest*,
            pb::CheckpointCapabilitiesResponse* resp) override {
        try {
            resp->set_supports_write_fence(backend_->supports_write_fence());
            return ::grpc::Status::OK;
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status RaiseCheckpointFence(
            ::grpc::ServerContext*,
            const pb::RaiseCheckpointFenceRequest* req,
            pb::RaiseCheckpointFenceResponse* resp) override {
        CheckpointWriteFence wf;
        if (req->has_write_fence()) {
            try {
                wf = from_write_fence(true, req->write_fence(), true);
            } catch (const std::invalid_argument& e) {
                return invalid_request(e.what());
            }
        } else {
            if (req->run_id().empty() || req->new_fence() == 0) {
                return invalid_request(
                    "RaiseCheckpointFence requires non-empty run_id and positive new_fence");
            }
            wf.owner_scope = req->owner_scope();
            wf.run_id = req->run_id();
            wf.fence = req->new_fence();
        }
        try {
            const auto stored = backend_->raise_fence(wf.owner_scope, wf.run_id, wf.fence);
            resp->set_ok(true);
            resp->set_stored_fence(stored);
            return ::grpc::Status::OK;
        } catch (const CheckpointWriteFenceError& err) {
            return fence_refusal_status(err);
        } catch (const std::invalid_argument& err) {
            return invalid_request(err.what());
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status LoadCheckpointFence(
            ::grpc::ServerContext*,
            const pb::LoadCheckpointFenceRequest* req,
            pb::LoadCheckpointFenceResponse* resp) override {
        if (req->run_id().empty())
            return invalid_request("LoadCheckpointFence requires run_id");
        try {
            const auto f = backend_->load_fence(req->owner_scope(), req->run_id());
            if (f > 0) {
                resp->set_found(true);
                resp->set_fence(f);
            } else {
                resp->set_found(false);
                resp->set_fence(0);
            }
            return ::grpc::Status::OK;
        } catch (const std::invalid_argument& err) {
            return invalid_request(err.what());
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status SaveCheckpoint(
            ::grpc::ServerContext*,
            const pb::SaveCheckpointRequest* req,
            pb::StatusResponse* resp) override {
        // A malformed record is INVALID_ARGUMENT; a backend failure is a
        // status too, never an OK response (reference, "Protocol failure
        // mapping"). No backend exception text crosses the wire.
        if (!req->has_checkpoint())
            return invalid_request("SaveCheckpoint requires a checkpoint");
        if (auto reason = unstorable(req->checkpoint().checkpoint_json()))
            return invalid_request(std::move(*reason));
        Checkpoint checkpoint;
        try {
            checkpoint = checkpoint_from_json(req->checkpoint().checkpoint_json());
        } catch (const std::exception&) {
            return invalid_request("checkpoint_json is not a valid checkpoint");
        }
        if (checkpoint.id.empty() || checkpoint.thread_id.empty())
            return invalid_request("checkpoint requires an id and a thread_id");
        if (req->checkpoint().id() != checkpoint.id ||
            req->checkpoint().thread_id() != checkpoint.thread_id)
            return invalid_request("indexed id and thread_id must equal the checkpoint's");
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                req->has_write_fence(), req->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        try {
            backend_->save(checkpoint, wf);
            resp->set_ok(true);
            return ::grpc::Status::OK;
        } catch (const CheckpointWriteFenceError& err) {
            return fence_refusal_status(err);
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status LoadLatestCheckpoint(
            ::grpc::ServerContext*,
            const pb::LoadLatestRequest* req,
            pb::CheckpointResponse* resp) override {
        // A backend outage is a status, not "no checkpoint": a resumed run
        // must not silently start over.
        try {
            auto cp = backend_->load_latest(req->thread_id());
            if (cp) { resp->set_found(true);
                      to_blob(*cp, resp->mutable_checkpoint()); }
            else      resp->set_found(false);
        } catch (...) { return backend_failure(); }
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
        } catch (...) { return backend_failure(); }
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
        } catch (...) { return backend_failure(); }
        return ::grpc::Status::OK;
    }

    ::grpc::Status DeleteThreadCheckpoints(
            ::grpc::ServerContext*,
            const pb::DeleteThreadRequest* req,
            pb::StatusResponse* resp) override {
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                req->has_write_fence(), req->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        try {
            backend_->delete_thread(req->thread_id(), wf);
            resp->set_ok(true);
        } catch (const CheckpointWriteFenceError& err) {
            return fence_refusal_status(err);
        } catch (...) {
            return backend_failure();
        }
        return ::grpc::Status::OK;
    }

    ::grpc::Status PutCheckpointWrites(
            ::grpc::ServerContext*,
            const pb::PutCheckpointWritesRequest* req,
            pb::StatusResponse* resp) override {
        if (!req->has_write())
            return invalid_request("PutCheckpointWrites requires write");
        CheckpointWriteFence wf;
        PendingWrite pw;
        try {
            pw = from_pending_write(req->write());
            wf = from_write_fence(
                req->has_write_fence(), req->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (const neograph::json::exception& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        try {
            backend_->put_writes(req->thread_id(), req->parent_checkpoint_id(), pw, wf);
            resp->set_ok(true);
            return ::grpc::Status::OK;
        } catch (const CheckpointWriteFenceError& err) {
            return fence_refusal_status(err);
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status GetCheckpointWrites(
            ::grpc::ServerContext*,
            const pb::GetCheckpointWritesRequest* req,
            pb::GetCheckpointWritesResponse* resp) override {
        try {
            for (const auto& pw : backend_->get_writes(req->thread_id(), req->parent_checkpoint_id()))
                to_pending_write(pw, resp->add_writes());
            return ::grpc::Status::OK;
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status ClearCheckpointWrites(
            ::grpc::ServerContext*,
            const pb::ClearCheckpointWritesRequest* req,
            pb::StatusResponse* resp) override {
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                req->has_write_fence(), req->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        try {
            backend_->clear_writes(req->thread_id(), req->parent_checkpoint_id(), wf);
            resp->set_ok(true);
            return ::grpc::Status::OK;
        } catch (const CheckpointWriteFenceError& err) {
            return fence_refusal_status(err);
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status RequiresManagedBudget(
            ::grpc::ServerContext*,
            const pb::RequiresManagedBudgetRequest* req,
            pb::RequiresManagedBudgetResponse* resp) override {
        try {
            resp->set_required(backend_->requires_managed_budget(req->thread_id()));
            return ::grpc::Status::OK;
        } catch (...) {
            return backend_failure();
        }
    }

    ::grpc::Status AcquireManagedBudgetLease(
        ::grpc::ServerContext*, const pb::AcquireManagedBudgetLeaseRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                request->has_write_fence(), request->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        return journal_status([&] {
            if (!request->has_scope())
                throw std::invalid_argument("Managed-budget acquire lacks scope");
            const auto scope = from_scope(request->scope());
            if (native_archive_ && native_archive_->owner_scope() != scope.owner_scope)
                throw std::invalid_argument("Managed-budget server archive owner differs from original scope");
            auto lease = backend_->acquire_managed_budget_lease(
                scope, request->expected_checkpoint_id(),
                request->expected_checkpoint_commitment(), wf);
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
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                request->has_write_fence(), request->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        return journal_status([&] {
            auto lease = ManagedBudgetJournalAccess::lease_from_transport(
                parse_journal_payload(request->lease_json()));
            if (!native_archive_)
                throw std::invalid_argument("Remote managed-budget effects require actual server archive activation");
            ManagedBudgetJournalAccess::bind_native_archive(lease, native_archive_);
            auto effect = backend_->begin_managed_budget_effect(
                lease, request->effect_id(), request->exact_claim_amount(),
                request->prepared_request_digest(), wf);
            response->set_effect_json(ManagedBudgetJournalAccess::effect_transport(effect).dump());
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

    ::grpc::Status SettleManagedBudgetEffect(
        ::grpc::ServerContext*, const pb::SettleManagedBudgetEffectRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                request->has_write_fence(), request->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
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
            backend_->settle_managed_budget_effect(lease, effect, std::move(outcome), authority, wf);
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

    ::grpc::Status PublishManagedBudgetCheckpoint(
        ::grpc::ServerContext*, const pb::PublishManagedBudgetCheckpointRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                request->has_write_fence(), request->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        return journal_status([&] {
            auto lease = ManagedBudgetJournalAccess::lease_from_transport(
                parse_journal_payload(request->lease_json()));
            if (!request->has_checkpoint())
                throw std::invalid_argument("Managed-budget publication lacks checkpoint");
            if (auto reason = unstorable(request->checkpoint().checkpoint_json()))
                throw std::invalid_argument(*reason);
            auto checkpoint = from_blob(request->checkpoint());
            if (checkpoint.id != request->checkpoint().id() ||
                checkpoint.thread_id != request->checkpoint().thread_id())
                throw std::invalid_argument("Managed-budget checkpoint indexed identity mismatch");
            require_transportable_checkpoint(checkpoint);
            backend_->publish_managed_budget_checkpoint(lease, checkpoint, wf);
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

    ::grpc::Status ReleaseManagedBudgetLease(
        ::grpc::ServerContext*, const pb::ReleaseManagedBudgetLeaseRequest* request,
        pb::ManagedBudgetLeaseResponse* response) override {
        CheckpointWriteFence wf;
        try {
            wf = from_write_fence(
                request->has_write_fence(), request->write_fence(), backend_->supports_write_fence());
        } catch (const std::invalid_argument& e) {
            return invalid_request(e.what());
        } catch (...) {
            return backend_failure();
        }
        return journal_status([&] {
            auto lease = ManagedBudgetJournalAccess::lease_from_transport(
                parse_journal_payload(request->lease_json()));
            backend_->release_managed_budget_lease(lease, wf);
            response->set_lease_json(ManagedBudgetJournalAccess::lease_transport(lease).dump());
        });
    }

private:
    std::shared_ptr<CheckpointStore> backend_;
    std::shared_ptr<sp::NativeArchive> native_archive_;
};

}  // namespace

std::unique_ptr<::grpc::Service> make_checkpoint_service(
    std::shared_ptr<CheckpointStore> backend, std::shared_ptr<sp::NativeArchive> native_archive) {
    return std::make_unique<CheckpointServiceImpl>(std::move(backend), std::move(native_archive));
}

void run_checkpoint_server(const std::string& address,
                           std::shared_ptr<CheckpointStore> backend) {
    run_checkpoint_server(address, std::move(backend), {});
}

void run_checkpoint_server(const std::string& address,
                           std::shared_ptr<CheckpointStore> backend,
                           std::shared_ptr<sp::NativeArchive> native_archive) {
    run_checkpoint_server(address, std::move(backend), std::move(native_archive),
                          ServeOptions{});
}

void run_checkpoint_server(const std::string& address,
                           std::shared_ptr<CheckpointStore> backend,
                           std::shared_ptr<sp::NativeArchive> native_archive,
                           const ServeOptions& options) {
    CheckpointServiceImpl svc(std::move(backend), std::move(native_archive));
    detail::serve(address, svc, pb::CheckpointService::service_full_name(), options,
                  "CheckpointService");
}

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
