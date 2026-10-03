#include "managed_budget_journal.h"
#include "canonical_json.h"

#include <neograph/provider_outcome_codec.h>
#include <neograph/graph/state.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neograph::graph {

struct OwnedManagedBudgetLease::Impl {
    const ManagedBudgetLeaseScope scope;
    const std::string execution_thread;
    const std::string execution_storage_thread;
    const std::string actor;
    const std::string generation;
    std::atomic<std::uint64_t> revision;
    mutable std::mutex mutex;
    std::string checkpoint_id;
    std::string commitment;
    std::shared_ptr<sp::NativeArchive> native_archive;
    bool cpp_native_retention = false;

    Impl(ManagedBudgetLeaseScope value, std::string actor_value, std::string generation_value,
         std::uint64_t revision_value, std::string id, std::string digest,
         std::string execution_thread_value = {}, std::string execution_storage_value = {})
        : scope(std::move(value)),
          execution_thread(execution_thread_value.empty() ? scope.thread_id : std::move(execution_thread_value)),
          execution_storage_thread(execution_storage_value.empty()
              ? (scope.storage_thread_id.empty() ? scope.thread_id : scope.storage_thread_id)
              : std::move(execution_storage_value)),
          actor(std::move(actor_value)), generation(std::move(generation_value)),
          revision(revision_value), checkpoint_id(std::move(id)), commitment(std::move(digest)) {}
};

struct ManagedBudgetEffectReceipt::Impl {
    const std::string storage;
    const std::string actor;
    const std::string generation;
    const std::string id;
    const std::uint64_t amount;
    const std::string digest;
    const std::uint64_t begin_revision;
    mutable std::mutex custody_mutex;
    mutable sp::runtime::Result owned_outcome;

    Impl(std::string storage_value, std::string actor_value, std::string generation_value,
         std::string id_value, std::uint64_t amount_value, std::string digest_value,
         std::uint64_t revision_value)
        : storage(std::move(storage_value)), actor(std::move(actor_value)),
          generation(std::move(generation_value)), id(std::move(id_value)), amount(amount_value),
          digest(std::move(digest_value)), begin_revision(revision_value) {}
};

OwnedManagedBudgetLease::OwnedManagedBudgetLease(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
const ManagedBudgetLeaseScope& OwnedManagedBudgetLease::scope() const noexcept { return impl_->scope; }
const std::string& OwnedManagedBudgetLease::execution_thread_id() const noexcept {
    return impl_->execution_thread;
}
const std::string& OwnedManagedBudgetLease::execution_storage_thread_id() const noexcept {
    return impl_->execution_storage_thread;
}
const std::string& OwnedManagedBudgetLease::actor_id() const noexcept { return impl_->actor; }
const std::string& OwnedManagedBudgetLease::bank_generation() const noexcept { return impl_->generation; }
std::uint64_t OwnedManagedBudgetLease::revision() const noexcept {
    return impl_->revision.load(std::memory_order_acquire);
}
std::string OwnedManagedBudgetLease::head_checkpoint_id() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->checkpoint_id;
}
std::string OwnedManagedBudgetLease::head_commitment() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->commitment;
}
ManagedBudgetEffectReceipt::ManagedBudgetEffectReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
bool ManagedBudgetEffectReceipt::active() const noexcept { return static_cast<bool>(impl_); }
const std::string& ManagedBudgetEffectReceipt::effect_id() const {
    if (!impl_) throw std::logic_error("Managed budget effect receipt is invalid");
    return impl_->id;
}
std::uint64_t ManagedBudgetEffectReceipt::claim_amount() const {
    if (!impl_) throw std::logic_error("Managed budget effect receipt is invalid");
    return impl_->amount;
}
const std::string& ManagedBudgetEffectReceipt::request_digest() const {
    if (!impl_) throw std::logic_error("Managed budget effect receipt is invalid");
    return impl_->digest;
}

std::string managed_budget_checkpoint_commitment(const Checkpoint& checkpoint) {
    // Native C++ custody pointers are transient, not durable checkpoint fields.
    // Their durable projections/archive references are part of channel_values.
    auto barriers = json::object();
    for (const auto& [name, upstreams] : checkpoint.barrier_state) {
        auto signals = json::array();
        for (const auto& upstream : upstreams) signals.push_back(upstream);
        barriers[name] = std::move(signals);
    }
    const json value{{"schema", "neograph.managed-bank-checkpoint-commitment/v1"},
        {"id", checkpoint.id}, {"thread_id", checkpoint.thread_id},
        {"channel_values", checkpoint.channel_values}, {"channel_versions", checkpoint.channel_versions},
        {"parent_id", checkpoint.parent_id}, {"current_node", checkpoint.current_node},
        {"next_nodes", checkpoint.next_nodes}, {"interrupt_phase", to_string(checkpoint.interrupt_phase)},
        {"barrier_state", std::move(barriers)}, {"metadata", checkpoint.metadata},
        {"step", checkpoint.step}, {"timestamp", checkpoint.timestamp},
        {"schema_version", checkpoint.schema_version}};
    return neograph::detail::sha256_identity("NeoGraph", "managed-bank-full-checkpoint/v1",
                                            neograph::detail::canonical_json_bytes(value));
}

bool checkpoint_channel_blob_eligible(const json& channel) {
    if (!channel.is_object() || !channel.contains("version") || !channel.contains("value")) return false;
    const auto version = channel.at("version");
    if (version.is_number_unsigned())
        return version.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return version.is_number_integer() && version.get<std::int64_t>() >= 0;
}

json checkpoint_storage_shape(const Checkpoint& checkpoint) {
    auto residual = json::object();
    auto blob_channels = json::array();
    if (!checkpoint.channel_values.is_object()) {
        residual = checkpoint.channel_values;
    } else {
        if (checkpoint.channel_values.contains("global_version"))
            residual["global_version"] = checkpoint.channel_values.at("global_version");
        if (checkpoint.channel_values.contains("channels")) {
            const auto channels = checkpoint.channel_values.at("channels");
            if (!channels.is_object()) {
                residual["channels"] = channels;
            } else {
                auto remaining = json::object();
                // items() deep-copies this JSON wrapper. Use its object view
                // iterators so a large blob value is never copied into residue.
                const auto end = channels.end();
                for (auto item = channels.begin(); item != end; ++item) {
                    const auto name = item.key();
                    const auto channel = item.value();
                    if (!checkpoint_channel_blob_eligible(channel)) {
                        remaining[name] = channel;
                        continue;
                    }
                    auto attributes = json::object();
                    const auto attributes_end = channel.end();
                    for (auto attribute = channel.begin(); attribute != attributes_end; ++attribute) {
                        const auto key = attribute.key();
                        if (key != "value") attributes[key] = attribute.value();
                    }
                    remaining[name] = std::move(attributes);
                    blob_channels.push_back(name);
                }
                residual["channels"] = std::move(remaining);
            }
        }
    }
    // Other top-level provider/state fields live in the existing metadata
    // envelope; retaining them here would duplicate whole outcome histories.
    return {{"schema", "neograph.checkpoint-state-shape/v1"}, {"channel_values", std::move(residual)},
        {"channel_versions", checkpoint.channel_versions}, {"blob_channels", std::move(blob_channels)}};
}

void restore_checkpoint_storage_shape(Checkpoint& checkpoint, const json& shape,
                                      const std::map<std::string, json>& blobs) {
    auto require_shape = [](bool condition) {
        if (!condition) throw std::runtime_error("Corrupt or unsupported checkpoint state shape");
    };
    require_shape(shape.is_object() && shape.size() == 4 && shape.contains("schema") &&
        shape.contains("channel_values") && shape.contains("channel_versions") && shape.contains("blob_channels"));
    require_shape(shape.at("schema").is_string() &&
                  shape.at("schema") == "neograph.checkpoint-state-shape/v1" && shape.at("blob_channels").is_array());
    const auto stored_residual = shape.at("channel_values");
    auto residual = neograph::detail::owned_json_copy(stored_residual);
    std::unordered_set<std::string> seen;
    for (const auto& item : shape.at("blob_channels")) {
        require_shape(item.is_string());
        const auto name = item.get<std::string>();
        require_shape(seen.insert(name).second && residual.is_object() && residual.contains("channels"));
        const auto channels = static_cast<const json&>(residual).at("channels");
        require_shape(channels.is_object() && channels.contains(name));
        const auto channel = channels.at(name);
        require_shape(channel.is_object() && channel.contains("version") && !channel.contains("value"));
        require_shape(checkpoint_channel_blob_eligible(json{{"version", channel.at("version")}, {"value", nullptr}}));
        const auto blob = blobs.find(name);
        if (blob == blobs.end()) throw std::runtime_error("Checkpoint state shape requires a missing channel blob");
        residual["channels"][name]["value"] = blob->second;
    }
    // Only named blob values are restored; scalars/null/absent fields and all
    // channel attributes remain exact, with independent CP versions untouched.
    auto versions = neograph::detail::owned_json_copy(shape.at("channel_versions"));
    checkpoint.channel_values = std::move(residual);
    checkpoint.channel_versions = std::move(versions);
}

namespace detail {
namespace {
constexpr const char* head_schema = "neograph.managed-bank-head/v1";
constexpr const char* effect_schema = "neograph.managed-bank-effect/v1";
constexpr const char* lease_schema = "neograph.managed-bank-lease-claim/v1";
constexpr const char* receipt_schema = "neograph.managed-bank-effect-claim/v1";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void closed(const json& value, std::initializer_list<std::string_view> fields) {
    require(value.is_object() && value.size() == fields.size(), "Corrupt managed budget journal object");
    for (const auto field : fields)
        require(value.contains(std::string(field)), "Corrupt managed budget journal field");
}
std::string text(const json& value, const char* field, bool allow_empty = false) {
    const auto& item = value.at(field);
    require(item.is_string(), "Corrupt managed budget journal string");
    auto result = item.get<std::string>();
    require(allow_empty || !result.empty(), "Empty managed budget journal identity");
    neograph::detail::validate_utf8(result);
    return result;
}
std::uint64_t number(const json& value, const char* field) {
    const auto& item = value.at(field);
    require(item.is_number_unsigned() || (item.is_number_integer() && item.get<std::int64_t>() >= 0),
            "Corrupt managed budget journal counter");
    return item.get<std::uint64_t>();
}
bool flag(const json& value, const char* field) {
    require(value.at(field).is_boolean(), "Corrupt managed budget journal flag");
    return value.at(field).get<bool>();
}
std::uint64_t add(std::uint64_t left, std::uint64_t right) {
    require(right <= std::numeric_limits<std::uint64_t>::max() - left,
            "Managed budget journal counter overflow");
    return left + right;
}
std::string nonce() {
    std::array<unsigned char, 32> bytes{};
#if defined(__linux__)
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "Managed budget journal requires platform cryptographic entropy");
        offset += static_cast<std::size_t>(count);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    ::arc4random_buf(bytes.data(), bytes.size());
#elif defined(_WIN32)
    const auto library = ::LoadLibraryW(L"advapi32.dll");
    require(library != nullptr, "Managed budget journal requires platform cryptographic entropy");
    using Random = BOOLEAN (APIENTRY*)(PVOID, ULONG);
    const auto random = reinterpret_cast<Random>(::GetProcAddress(library, "SystemFunction036"));
    const bool accepted = random && random(bytes.data(), static_cast<ULONG>(bytes.size()));
    ::FreeLibrary(library);
    require(accepted, "Managed budget journal requires platform cryptographic entropy");
#else
    throw std::runtime_error("Managed budget journal platform entropy is unsupported");
#endif
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 15];
    }
    return result;
}
void nonce_identity(const std::string& value) {
    require(value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }), "Corrupt managed budget journal nonce");
}
json scope_json(const ManagedBudgetLeaseScope& scope) {
    return {{"owner_scope", scope.owner_scope}, {"thread_id", scope.thread_id},
        {"storage_thread_id", ManagedBudgetJournalAccess::storage_key(scope)},
        {"graph_identity", scope.graph_identity}, {"original_ceiling", scope.original_ceiling},
        {"original_deadline_ticks", scope.original_deadline_ticks ? json(*scope.original_deadline_ticks) : json(nullptr)},
        {"deadline_clock_identity", scope.deadline_clock_identity}};
}
ManagedBudgetLeaseScope parse_scope(const json& value) {
    closed(value, {"owner_scope", "thread_id", "storage_thread_id", "graph_identity", "original_ceiling",
                   "original_deadline_ticks", "deadline_clock_identity"});
    ManagedBudgetLeaseScope scope;
    scope.owner_scope = text(value, "owner_scope", true);
    scope.thread_id = text(value, "thread_id");
    scope.storage_thread_id = text(value, "storage_thread_id");
    scope.graph_identity = text(value, "graph_identity");
    scope.original_ceiling = number(value, "original_ceiling");
    require(scope.original_ceiling != 0, "Observation-only source has no finite managed budget currency");
    if (!value.at("original_deadline_ticks").is_null()) {
        require(value.at("original_deadline_ticks").is_number_integer(), "Corrupt managed budget deadline");
        if (value.at("original_deadline_ticks").is_number_unsigned())
            require(value.at("original_deadline_ticks").get<std::uint64_t>() <=
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
                    "Corrupt managed budget deadline");
        scope.original_deadline_ticks = value.at("original_deadline_ticks").get<std::int64_t>();
        require(*scope.original_deadline_ticks >= 0, "Corrupt managed budget deadline");
    }
    scope.deadline_clock_identity = text(value, "deadline_clock_identity", true);
    require(scope.original_deadline_ticks ? !scope.deadline_clock_identity.empty() : scope.deadline_clock_identity.empty(),
            "Managed budget deadline has no verifiable clock origin");
    return scope;
}
void clock_scope(const ManagedBudgetLeaseScope& scope, bool spending) {
    if (!scope.original_deadline_ticks) return;
    require(scope.deadline_clock_identity == managed_budget_deadline_clock_identity(),
            "Managed budget deadline clock origin cannot be recovered");
    if (spending) {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        require(now < *scope.original_deadline_ticks, "Original managed budget deadline has expired");
    }
}
json authority_json(const UsageAccumulator::AuthoritySnapshot& authority) {
    return {{"charged", authority.charged}, {"reserved", authority.reserved},
        {"provider_effects", authority.provider_effects}, {"reports", provider_codec::encode_usage(authority.reports)},
        {"has_report", authority.has_report}};
}
void unique_effects(const json& effects) {
    require(effects.is_array(), "Corrupt managed budget effect identities");
    std::unordered_set<std::string> seen;
    for (const auto& item : effects) {
        require(item.is_string(), "Corrupt managed budget effect identity");
        auto id = item.get<std::string>();
        require(!id.empty(), "Corrupt managed budget effect identity");
        require(seen.insert(std::move(id)).second, "Duplicate managed budget effect identity");
    }
}
bool contains_effect(const json& effects, const json& id) {
    for (const auto& effect : effects)
        if (effect == id) return true;
    return false;
}
void validate_authority(const json& value) {
    closed(value, {"charged", "reserved", "provider_effects", "reports", "has_report"});
    (void)add(number(value, "charged"), number(value, "reserved"));
    unique_effects(value.at("provider_effects"));
    (void)provider_codec::decode_usage(value.at("reports"));
    (void)flag(value, "has_report");
}
void validate_head(const json& head) {
    closed(head, {"schema", "scope", "bank_generation", "head_checkpoint_id", "head_commitment", "revision",
                  "actor", "state", "charged", "reserved", "pending_count", "effect_dedup", "provider_effects",
                  "reports", "report_effects", "has_report", "source_consumed", "publication_count"});
    require(text(head, "schema") == head_schema, "Unsupported managed budget head schema");
    (void)parse_scope(head.at("scope"));
    nonce_identity(text(head, "bank_generation"));
    const auto id = text(head, "head_checkpoint_id", true);
    const auto commitment = text(head, "head_commitment", true);
    require(id.empty() ? commitment.empty() : neograph::detail::is_sha256_identity(commitment),
            "Corrupt managed budget head commitment");
    require(number(head, "revision") != 0, "Corrupt managed budget head revision");
    (void)add(number(head, "charged"), number(head, "reserved"));
    const auto pending = number(head, "pending_count");
    require(head.at("effect_dedup").is_object() && pending <= head.at("effect_dedup").size(),
            "Corrupt managed budget pending effects");
    for (const auto& [id_key, burned] : head.at("effect_dedup").items())
        require(!id_key.empty() && burned.is_boolean() && burned.get<bool>(), "Corrupt managed budget effect tombstone");
    unique_effects(head.at("provider_effects"));
    for (const auto& item : head.at("provider_effects"))
        require(head.at("effect_dedup").contains(item.get<std::string>()), "Managed budget effect tombstone is missing");
    unique_effects(head.at("report_effects"));
    for (const auto& item : head.at("report_effects"))
        require(head.at("effect_dedup").contains(item.get<std::string>()), "Managed budget report has no admitted effect");
    (void)provider_codec::decode_usage(head.at("reports"));
    (void)flag(head, "has_report");
    const auto publications = number(head, "publication_count");
    require((publications == 0) == id.empty(), "Corrupt managed budget publication identity");
    const auto actor = text(head, "actor", true);
    const auto state = text(head, "state");
    const bool consumed = flag(head, "source_consumed");
    if (state == "owned") nonce_identity(actor);
    else if (state == "available") require(actor.empty() && !consumed && pending == 0,
                                             "Corrupt available managed budget head");
    else if (state == "blocked") require(actor.empty() && consumed, "Corrupt blocked managed budget head");
    else throw std::runtime_error("Unsupported managed budget ownership state");
    require(pending == 0 || consumed, "Corrupt managed budget unconsumed pending source");
}
void owned(const json& head, const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    require(static_cast<bool>(lease), "Managed budget requires an owned lease receipt");
    validate_head(head);
    require(head.at("scope") == scope_json(lease->scope()) && head.at("bank_generation") == lease->bank_generation() &&
            head.at("actor") == lease->actor_id() && head.at("state") == "owned",
            "Managed budget lease does not own the current source head");
    // Revision orders durable head mutations, not concurrently active claims.
    clock_scope(lease->scope(), false);
}
void advance(json& head) { head["revision"] = add(number(head, "revision"), 1); }
void validate_effect(const json& effect) {
    closed(effect, {"schema", "bank_generation", "actor", "effect_id", "claim_amount", "request_digest",
                    "begin_revision", "state", "charged", "held", "outcome", "authority"});
    require(text(effect, "schema") == effect_schema, "Unsupported managed budget effect schema");
    nonce_identity(text(effect, "bank_generation"));
    nonce_identity(text(effect, "actor"));
    (void)text(effect, "effect_id");
    require(number(effect, "claim_amount") != 0 && number(effect, "begin_revision") != 0,
            "Corrupt managed budget effect claim");
    require(neograph::detail::is_sha256_identity(text(effect, "request_digest")),
            "Corrupt managed budget request digest");
    const auto charged = number(effect, "charged"), held = number(effect, "held");
    const auto state = text(effect, "state");
    if (state == "pending")
        require(charged == 0 && held == number(effect, "claim_amount") && effect.at("outcome").is_null() &&
                effect.at("authority").is_null(), "Corrupt managed budget pending effect");
    else {
        require(state == "charged" || state == "held" || state == "released", "Unsupported managed budget effect state");
        require(effect.at("outcome").is_object(), "Managed budget effect lost its genuine outcome evidence");
        validate_authority(effect.at("authority"));
        require(state == "held" ? charged == 0 && held == number(effect, "claim_amount") : held == 0,
                "Corrupt managed budget settled claim");
        require(state != "released" || charged == 0, "Corrupt managed budget released claim");
    }
}
void validate_bank_publication(const json& head, const json& bank,
                               const ManagedBudgetLeaseScope& scope, const std::string& execution_thread) {
    closed(bank, {"schema", "charged", "reserved", "provider_effects", "reports", "has_report", "ceiling",
                  "owner_scope", "thread_id", "graph_identity"});
    require(text(bank, "schema") == "neograph.graph-managed-bank/v1" &&
            number(bank, "ceiling") == scope.original_ceiling &&
            bank.at("owner_scope") == scope.owner_scope && bank.at("thread_id") == execution_thread &&
            bank.at("graph_identity") == scope.graph_identity,
            "Managed budget publication changed its immutable original bank scope");
    const auto charged = number(bank, "charged"), reserved = number(bank, "reserved");
    (void)add(charged, reserved);
    require(charged >= number(head, "charged") && reserved >= number(head, "reserved"),
            "Managed budget publication weakens durable charged or held currency");
    unique_effects(bank.at("provider_effects"));
    for (const auto& settled : head.at("provider_effects"))
        require(contains_effect(bank.at("provider_effects"), settled),
                "Managed budget publication removed accounted provider effects");
    require(bank.at("provider_effects").size() == head.at("report_effects").size(),
            "Managed budget publication changed its real atomic report frontier");
    for (const auto& observed : head.at("report_effects"))
        require(contains_effect(bank.at("provider_effects"), observed),
                "Managed budget publication removed genuine report evidence");
    require(flag(bank, "has_report") == flag(head, "has_report") && bank.at("reports") == head.at("reports"),
            "Managed budget publication changed genuine provider usage reports");
}
} // namespace

std::string managed_budget_deadline_clock_identity() {
#if defined(__linux__)
    // Linux steady_clock is CLOCK_MONOTONIC: bind both this kernel boot and
    // its real time-namespace offset, never a process nonce or fake clock.
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string boot;
    require(static_cast<bool>(input) && static_cast<bool>(std::getline(input, boot)),
            "Managed budget deadline requires a verifiable Linux boot identity");
    require(boot.size() == 36, "Invalid Linux managed budget clock identity");
    for (std::size_t index = 0; index != boot.size(); ++index) {
        const char ch = boot[index];
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        require(separator ? ch == '-' : (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'),
                "Invalid Linux managed budget clock identity");
    }
    std::ifstream offsets("/proc/self/timens_offsets");
    require(static_cast<bool>(offsets), "Managed budget clock namespace cannot be verified");
    std::string clock;
    std::int64_t seconds = 0, nanoseconds = 0;
    bool found = false;
    std::string origin;
    while (offsets >> clock) {
        require(static_cast<bool>(offsets >> seconds >> nanoseconds) && nanoseconds >= 0 &&
                nanoseconds < 1000000000, "Corrupt managed budget clock namespace offset");
        if (clock == "monotonic") {
            require(!found, "Ambiguous managed budget clock namespace origin");
            found = true;
            origin = std::to_string(seconds) + ":" + std::to_string(nanoseconds);
        }
    }
    require(offsets.eof() && found, "Managed budget monotonic clock origin cannot be recovered");
    return "linux:CLOCK_MONOTONIC:boot:" + boot + ":offset:" + origin;
#else
    throw std::runtime_error("Managed budget deadline clock recovery is unsupported on this platform");
#endif
}

bool ManagedBudgetJournalAccess::checkpoint_requires_obligation(const Checkpoint& checkpoint) {
    if (checkpoint.metadata.is_object() &&
        checkpoint.metadata.contains("_neograph_managed_budget_scope")) return true;
    if (!checkpoint.channel_values.is_object() ||
        !checkpoint.channel_values.contains("provider_managed_budget")) return false;
    try {
        const auto& bank = checkpoint.channel_values.at("provider_managed_budget").at("data");
        if (!bank.is_object() ||
            bank.at("schema").get<std::string_view>() != "neograph.graph-managed-bank/v1") return true;
        const auto ceiling = number(bank, "ceiling");
        (void)number(bank, "charged");
        (void)number(bank, "reserved");
        const auto& effects = bank.at("provider_effects");
        if (!effects.is_array()) return true;
        // Unbounded usage (including real charges and effects) is historical
        // observation, not an original finite scope. Journal heads and prior
        // obligation flags independently remain monotonic denial evidence.
        return ceiling != 0;
    } catch (const std::exception&) {
        return true; // Ambiguous legacy currency remains denial-only evidence.
    }
}

std::string ManagedBudgetJournalAccess::storage_key(const ManagedBudgetLeaseScope& scope) {
    const auto& key = scope.storage_thread_id.empty() ? scope.thread_id : scope.storage_thread_id;
    require(!key.empty(), "Managed budget requires its original storage namespace");
    neograph::detail::validate_utf8(key);
    return key;
}

void ManagedBudgetJournalAccess::select_branch_head(
    json& head, const std::string& checkpoint_id, const std::string& commitment) {
    validate_head(head);
    require(head.at("state") == "available" && !flag(head, "source_consumed") && number(head, "pending_count") == 0,
            "Managed budget shared bank is owned, consumed, or awaiting genuine reconciliation");
    require(!checkpoint_id.empty() && neograph::detail::is_sha256_identity(commitment) &&
            number(head, "publication_count") != 0,
            "Managed budget branch has no authenticated publication");
    // The backend authenticated these private branch coordinates. All original
    // financial counters, fences, deadline, generation and revision stay intact.
    head["head_checkpoint_id"] = checkpoint_id;
    head["head_commitment"] = commitment;
}

std::string ManagedBudgetJournalAccess::validate_shared_bank_fork(
    const json& head, const Checkpoint& source, const Checkpoint& forked) {
    validate_head(head);
    require(head.at("state") == "available" && !flag(head, "source_consumed") && number(head, "pending_count") == 0,
            "Managed budget fork source is owned, consumed, or awaiting genuine reconciliation");
    require(head.at("head_checkpoint_id") == source.id &&
            head.at("head_commitment") == managed_budget_checkpoint_commitment(source),
            "Managed budget fork source is not the authenticated current branch head");
    const auto scope = parse_scope(head.at("scope"));
    clock_scope(scope, false);
    require(!forked.id.empty() && forked.id != source.id && !forked.thread_id.empty() &&
            forked.thread_id != source.thread_id && forked.parent_id == source.id,
            "Managed budget fork is not a fresh distinct branch of its source");
    require(source.native_history && forked.native_history && source.native_history->budget_ &&
            forked.native_history->budget_,
            "Managed budget fork requires real original C++ shared-bank custody");
    const auto& original = *source.native_history->budget_;
    const auto& fork = *forked.native_history->budget_;
    const auto original_thread = [](const NativeGraphCheckpoint::BudgetBinding& binding) -> const std::string& {
        return binding.original_thread_id.empty() ? binding.thread_id : binding.original_thread_id;
    };
    require(original.managed && fork.managed && original.bank && original.bank == fork.bank &&
            original.original_ceiling == scope.original_ceiling && fork.original_ceiling == scope.original_ceiling &&
            original.ceiling <= scope.original_ceiling && fork.ceiling <= scope.original_ceiling &&
            original.owner_scope == scope.owner_scope && fork.owner_scope == scope.owner_scope &&
            original.graph_identity == scope.graph_identity && fork.graph_identity == scope.graph_identity &&
            original_thread(original) == scope.thread_id && original_thread(fork) == scope.thread_id &&
            fork.fork_source_thread_id == original.thread_id && fork.thread_id != original.thread_id,
            "Managed budget fork changed original bank identity or lacks genuine fork provenance");
    require(source.thread_id != original.thread_id || forked.thread_id == fork.thread_id,
            "Managed budget fork changed its actual execution namespace");
    require(source.channel_values.is_object() && source.channel_values.contains("provider_managed_budget") &&
            source.channel_values.at("provider_managed_budget").at("data").at("thread_id") == original.thread_id,
            "Managed budget fork source changed its actual native execution thread");
    require(source.native_history->projection_ == source.channel_values &&
            forked.native_history->projection_ == forked.channel_values,
            "Managed budget fork projection differs from its actual C++ custody");
    require(source.metadata.is_object() && forked.metadata.is_object() &&
            source.metadata.contains("_neograph_managed_budget_scope") &&
            forked.metadata.contains("_neograph_managed_budget_scope") &&
            source.metadata.at("_neograph_managed_budget_scope") ==
                forked.metadata.at("_neograph_managed_budget_scope"),
            "Managed budget fork changed or stripped the original source lease scope");
    require(forked.channel_values.contains("provider_managed_budget"),
            "Managed budget fork stripped its original bank");
    const auto& envelope = forked.channel_values.at("provider_managed_budget");
    require(envelope.is_object() && envelope.contains("data"), "Managed budget fork lacks its bank projection");
    const auto& bank = envelope.at("data");
    validate_bank_publication(head, bank, scope, fork.thread_id);
    const auto authority = fork.bank->authority_snapshot();
    require(number(bank, "charged") == authority.charged && number(bank, "reserved") == authority.reserved &&
            bank.at("provider_effects") == json(authority.provider_effects) &&
            bank.at("reports") == provider_codec::encode_usage(authority.reports) &&
            flag(bank, "has_report") == authority.has_report,
            "Managed budget fork does not retain the actual current shared bank authority");
    return fork.thread_id;
}

std::shared_ptr<OwnedManagedBudgetLease> ManagedBudgetJournalAccess::acquire(
    json& head, bool prior_obligation, const ManagedBudgetLeaseScope& requested_scope,
    const std::string& expected_id, const std::string& expected_commitment) {
    return acquire_branch(head, prior_obligation, requested_scope, expected_id, expected_commitment,
                          requested_scope.thread_id, storage_key(requested_scope));
}

std::shared_ptr<OwnedManagedBudgetLease> ManagedBudgetJournalAccess::acquire_branch(
    json& head, bool prior_obligation, const ManagedBudgetLeaseScope& requested_scope,
    const std::string& expected_id, const std::string& expected_commitment,
    const std::string& execution_thread, const std::string& execution_storage_thread) {
    require(!execution_thread.empty() && !execution_storage_thread.empty(),
            "Managed budget branch lacks its trusted execution namespace");
    neograph::detail::validate_utf8(execution_thread);
    neograph::detail::validate_utf8(execution_storage_thread);
    const auto encoded_scope = scope_json(requested_scope);
    auto scope = parse_scope(encoded_scope);
    clock_scope(scope, true);
    std::string generation;
    std::uint64_t revision = 1;
    if (head.is_null()) {
        require(!prior_obligation, "Managed budget obligation has no recoverable source journal");
        require(expected_id.empty() && expected_commitment.empty(),
                "Archived managed budget source has no authenticated current source journal");
        generation = nonce();
    } else {
        validate_head(head);
        require(head.at("scope") == encoded_scope, "Managed budget original source scope cannot be replaced");
        require(head.at("head_checkpoint_id") == expected_id && head.at("head_commitment") == expected_commitment,
                "Managed budget checkpoint is not the exact current source head");
        require(head.at("state") == "available" && !flag(head, "source_consumed") && number(head, "pending_count") == 0,
                "Managed budget source is owned, consumed, or requires genuine outcome reconciliation");
        generation = text(head, "bank_generation");
        revision = add(number(head, "revision"), 1);
    }
    const auto actor = nonce();
    auto impl = std::make_shared<OwnedManagedBudgetLease::Impl>(std::move(scope), actor, generation,
        revision, expected_id, expected_commitment, execution_thread, execution_storage_thread);
    auto lease = std::shared_ptr<OwnedManagedBudgetLease>(new OwnedManagedBudgetLease(std::move(impl)));
    if (head.is_null()) {
        head = {{"schema", head_schema}, {"scope", encoded_scope}, {"bank_generation", generation},
            {"head_checkpoint_id", ""}, {"head_commitment", ""}, {"revision", revision}, {"actor", actor},
            {"state", "owned"}, {"charged", std::uint64_t{0}}, {"reserved", std::uint64_t{0}},
            {"pending_count", std::uint64_t{0}}, {"effect_dedup", json::object()}, {"provider_effects", json::array()},
            {"reports", provider_codec::encode_usage(sp::Usage{})}, {"has_report", false},
            {"report_effects", json::array()},
            {"source_consumed", false}, {"publication_count", std::uint64_t{0}}};
    } else {
        head["actor"] = actor;
        head["state"] = "owned";
        head["revision"] = revision;
    }
    return lease;
}

ManagedBudgetEffectReceipt ManagedBudgetJournalAccess::begin(
    json& head, json& effect, const std::shared_ptr<OwnedManagedBudgetLease>& lease,
    const std::string& id, std::uint64_t amount, const std::string& digest) {
    owned(head, lease);
    clock_scope(lease->scope(), true);
    {
        std::lock_guard lock(lease->impl_->mutex);
        require(lease->impl_->native_archive || lease->impl_->cpp_native_retention,
                "Managed budget effect requires actual archive activation or backend-owned C++ native retention");
    }
    require(!id.empty() && amount != 0, "Managed budget effect requires an exact positive prepared claim");
    neograph::detail::validate_utf8(id);
    require(neograph::detail::is_sha256_identity(digest), "Managed budget effect requires its prepared request digest");
    require(effect.is_null() && !head.at("effect_dedup").contains(id),
            "Managed budget effect slot is permanently consumed");
    const auto committed = add(number(head, "charged"), number(head, "reserved"));
    require(committed <= lease->scope().original_ceiling && amount <= lease->scope().original_ceiling - committed,
            "Managed budget claim exceeds remaining original source currency");
    const auto revision = add(number(head, "revision"), 1);
    const auto pending = add(number(head, "pending_count"), 1);
    auto impl = std::make_shared<const ManagedBudgetEffectReceipt::Impl>(storage_key(lease->scope()), lease->actor_id(),
        lease->bank_generation(), id, amount, digest, revision);
    effect = {{"schema", effect_schema}, {"bank_generation", lease->bank_generation()}, {"actor", lease->actor_id()},
        {"effect_id", id}, {"claim_amount", amount}, {"request_digest", digest}, {"begin_revision", revision},
        {"state", "pending"}, {"charged", std::uint64_t{0}}, {"held", amount}, {"outcome", nullptr}, {"authority", nullptr}};
    head["reserved"] = add(number(head, "reserved"), amount);
    head["pending_count"] = pending;
    head["effect_dedup"][id] = true;
    head["source_consumed"] = true;
    head["revision"] = revision;
    return ManagedBudgetEffectReceipt(std::move(impl));
}

void ManagedBudgetJournalAccess::settle(
    json& head, json& stored_effect, const std::shared_ptr<OwnedManagedBudgetLease>& lease,
    const ManagedBudgetEffectReceipt& effect, sp::runtime::Result outcome,
    const UsageAccumulator::AuthoritySnapshot& authority) {
    owned(head, lease);
    require(effect.impl_ && outcome, "Managed budget settlement requires a genuine owned outcome and effect receipt");
    validate_effect(stored_effect);
    const auto& claim = *effect.impl_;
    require(claim.storage == storage_key(lease->scope()) && claim.actor == lease->actor_id() &&
            claim.generation == lease->bank_generation() && stored_effect.at("actor") == claim.actor &&
            stored_effect.at("bank_generation") == claim.generation && stored_effect.at("effect_id") == claim.id &&
            stored_effect.at("claim_amount") == claim.amount && stored_effect.at("request_digest") == claim.digest &&
            stored_effect.at("begin_revision") == claim.begin_revision && stored_effect.at("state") == "pending" &&
            head.at("effect_dedup").contains(claim.id), "Managed budget effect is not this actor's pending claim");
    require(number(head, "pending_count") != 0 && number(head, "reserved") >= claim.amount,
            "Managed budget pending claim lost its exact durable reservation");
    auto observed_authority = authority_json(authority);
    validate_authority(observed_authority);
    require(authority.has_report && std::find(authority.provider_effects.begin(), authority.provider_effects.end(),
                                             claim.id) != authority.provider_effects.end(),
            "Managed budget settlement lacks the real atomic accounted effect snapshot");
    std::shared_ptr<sp::NativeArchive> archive;
    {
        std::lock_guard lock(lease->impl_->mutex);
        archive = lease->impl_->native_archive;
    }
    const auto& messages = outcome_messages(*outcome);
    const bool has_native = std::any_of(messages.begin(), messages.end(),
        [](const sp::Message& message) { return static_cast<bool>(message.native); });
    json evidence;
    if (archive) {
        const auto binding = neograph::detail::sha256_identity("NeoGraph", "managed-bank-effect-custody/v1",
            neograph::detail::canonical_json_bytes(json{{"scope", scope_json(lease->scope())},
                {"effect", effect_transport(effect)}, {"authority", observed_authority}}));
        evidence = provider_codec::encode_outcome(*outcome, archive, binding);
    } else if (!has_native) {
        evidence = provider_codec::encode_outcome(*outcome);
    } else {
        // Financial evidence is explicitly NOT replay/continuation custody.
        // The genuine owned Result remains in the C++ sidecar and the actual
        // checkpoint/exception; never decode an observational native projection.
        const auto* completion = std::get_if<sp::Completion>(outcome.get());
        evidence = {{"schema", "neograph.managed-bank-financial-proof/v1"},
            {"kind", completion ? "completion" : "failure"},
            {"usage", provider_codec::encode_usage(outcome_usage(*outcome))},
            {"attempt", provider_codec::detail::attempt(completion ? completion->attempt :
                std::get<sp::Failure>(*outcome).error.attempt)},
            {"error", completion ? json(nullptr) : provider_codec::detail::error(std::get<sp::Failure>(*outcome).error)}};
    }
    std::uint64_t charged = 0;
    std::uint64_t held = claim.amount;
    const char* state = "held";
    if (const auto* completion = std::get_if<sp::Completion>(outcome.get())) {
        if (!completion->attempt.prior_usage_unknown && completion->attempt.transport_internal_resends == 0) {
            if (const auto amount = UsageAccumulator::conservative_final_charge(completion->usage)) {
                charged = *amount;
                held = 0;
                state = "charged";
            }
        }
    } else if (provider_failure_proves_not_sent(std::get<sp::Failure>(*outcome))) {
        held = 0;
        state = "released";
    }
    const auto next_charged = add(number(head, "charged"), charged);
    const auto next_reserved = add(number(head, "reserved") - claim.amount, held);
    (void)add(next_charged, next_reserved);
    // Aggregate snapshots can include not-yet-begun reservations or settlements
    // whose database transaction has not committed. Never use one to refund a
    // different effect's write-ahead hold or replace the durable bank totals.
    require(authority.charged >= charged && authority.reserved >= held,
            "Managed budget outcome disagrees with its atomic authority snapshot");
    const auto& prior_frontier = head.at("report_effects");
    const auto& next_frontier = observed_authority.at("provider_effects");
    auto includes = [](const json& superset, const json& subset) {
        for (const auto& id : subset)
            if (!contains_effect(superset, id)) return false;
        return true;
    };
    for (const auto& id : next_frontier)
        require(head.at("effect_dedup").contains(id.get<std::string>()),
                "Managed budget atomic snapshot includes an unadmitted provider effect");
    const bool newer_report = includes(next_frontier, prior_frontier);
    require(newer_report || includes(prior_frontier, next_frontier),
            "Managed budget atomic report snapshot changed bank lineage");
    if (newer_report && next_frontier.size() == prior_frontier.size())
        require(head.at("reports") == observed_authority.at("reports") &&
                flag(head, "has_report") == authority.has_report,
                "Managed budget atomic snapshot changed an unchanged report frontier");
    const auto next_revision = add(number(head, "revision"), 1);
    stored_effect["state"] = state;
    stored_effect["charged"] = charged;
    stored_effect["held"] = held;
    stored_effect["outcome"] = std::move(evidence);
    head["charged"] = next_charged;
    head["reserved"] = next_reserved;
    head["pending_count"] = number(head, "pending_count") - 1;
    head["provider_effects"].push_back(claim.id);
    if (newer_report) {
        head["reports"] = observed_authority.at("reports");
        head["report_effects"] = next_frontier;
        head["has_report"] = authority.has_report;
    }
    stored_effect["authority"] = std::move(observed_authority);
    head["revision"] = next_revision;
    {
        std::lock_guard lock(claim.custody_mutex);
        claim.owned_outcome = std::move(outcome);
    }
}

void ManagedBudgetJournalAccess::publish(json& head, const std::shared_ptr<OwnedManagedBudgetLease>& lease,
                                         const Checkpoint& checkpoint) {
    owned(head, lease);
    require(number(head, "pending_count") == 0, "Managed budget publication has pending provider effects");
    require(!checkpoint.id.empty() && checkpoint.thread_id == lease->execution_storage_thread_id(),
            "Managed budget publication differs from its trusted execution storage namespace");
    const auto current_head = head.at("head_checkpoint_id").get<std::string_view>();
    require(checkpoint.parent_id == current_head && checkpoint.id != current_head,
            "Managed budget publication is not a fresh successor of the current source head");
    require(checkpoint.channel_values.is_object() && checkpoint.channel_values.contains("provider_managed_budget"),
            "Managed budget publication stripped its original currency custody");
    const auto& envelope = checkpoint.channel_values.at("provider_managed_budget");
    require(envelope.is_object() && envelope.contains("data"), "Managed budget publication lacks its bank state");
    const auto& bank = envelope.at("data");
    validate_bank_publication(head, bank, lease->scope(), lease->execution_thread_id());
    const auto charged = number(bank, "charged"), reserved = number(bank, "reserved");
    // Keep the source identity sealed even for a same-ID replacement. The full
    // checkpoint and this candidate head must be committed by one transaction.
    auto commitment = managed_budget_checkpoint_commitment(checkpoint);
    const auto next_revision = add(number(head, "revision"), 1);
    const auto publications = add(number(head, "publication_count"), 1);
    head["head_checkpoint_id"] = checkpoint.id;
    head["head_commitment"] = std::move(commitment);
    head["charged"] = charged;
    head["reserved"] = reserved;
    head["source_consumed"] = false;
    head["publication_count"] = publications;
    head["revision"] = next_revision;
}

void ManagedBudgetJournalAccess::release(json& head, const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    owned(head, lease);
    const auto next_revision = add(number(head, "revision"), 1);
    // Only a run with no begin since its last genuine publication may release
    // the unchanged current source. Unknown/crashed/settled-but-unpublished work
    // permanently blocks reacquisition, with no timeout-based refund or reset.
    head["state"] = flag(head, "source_consumed") ? "blocked" : "available";
    head["actor"] = "";
    head["revision"] = next_revision;
}

void ManagedBudgetJournalAccess::refresh(const std::shared_ptr<OwnedManagedBudgetLease>& lease, const json& head) {
    require(static_cast<bool>(lease), "Managed budget refresh requires an owned receipt");
    validate_head(head);
    require(head.at("scope") == scope_json(lease->scope()) && head.at("bank_generation") == lease->bank_generation(),
            "Managed budget receipt refresh changed original source identity");
    const auto actor = text(head, "actor", true);
    require(actor == lease->actor_id() || actor.empty(), "Managed budget receipt refresh belongs to another actor");
    const auto revision = number(head, "revision");
    std::lock_guard lock(lease->impl_->mutex);
    const auto current = lease->impl_->revision.load(std::memory_order_relaxed);
    if (revision < current) return;
    auto id = text(head, "head_checkpoint_id", true);
    auto commitment = text(head, "head_commitment", true);
    if (revision == current) {
        require(id == lease->impl_->checkpoint_id && commitment == lease->impl_->commitment,
                "Managed budget head changed without advancing revision");
        return;
    }
    lease->impl_->checkpoint_id = std::move(id);
    lease->impl_->commitment = std::move(commitment);
    lease->impl_->revision.store(revision, std::memory_order_release);
}

void ManagedBudgetJournalAccess::bind_native_archive(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, std::shared_ptr<sp::NativeArchive> archive) {
    require(lease && archive, "Managed budget native custody requires an actual host-activated archive");
    require(archive->owner_scope() == lease->scope().owner_scope,
            "Managed budget native archive changed its original owner scope");
    std::lock_guard lock(lease->impl_->mutex);
    require(!lease->impl_->native_archive || lease->impl_->native_archive == archive,
            "Managed budget actor cannot replace its trusted native archive activation");
    lease->impl_->native_archive = std::move(archive);
}

void ManagedBudgetJournalAccess::bind_cpp_native_retention(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const json& actual_owned_head) {
    owned(actual_owned_head, lease);
    std::lock_guard lock(lease->impl_->mutex);
    lease->impl_->cpp_native_retention = true;
}

bool ManagedBudgetJournalAccess::retains_native_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    if (!lease) return false;
    std::lock_guard lock(lease->impl_->mutex);
    return lease->impl_->cpp_native_retention;
}

void ManagedBudgetJournalAccess::refresh_transport(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const json& transport) {
    require(static_cast<bool>(lease), "Managed budget transport refresh requires an owned receipt");
    // Parsing produces a claim, never a new actor or grant. Validate even late
    // responses against all immutable receipt fields before considering order.
    auto response = lease_from_transport(transport);
    require(scope_json(response->scope()) == scope_json(lease->scope()) &&
            response->actor_id() == lease->actor_id() && response->bank_generation() == lease->bank_generation() &&
            response->execution_thread_id() == lease->execution_thread_id() &&
            response->execution_storage_thread_id() == lease->execution_storage_thread_id(),
            "Managed budget transport refresh changed actor or original source scope");
    std::lock_guard lock(lease->impl_->mutex);
    const auto revision = response->revision();
    const auto current = lease->impl_->revision.load(std::memory_order_relaxed);
    if (revision < current) return;
    if (revision == current) {
        require(lease->impl_->checkpoint_id == response->impl_->checkpoint_id &&
                lease->impl_->commitment == response->impl_->commitment,
                "Managed budget transport changed the head without advancing revision");
        return;
    }
    lease->impl_->checkpoint_id = std::move(response->impl_->checkpoint_id);
    lease->impl_->commitment = std::move(response->impl_->commitment);
    lease->impl_->revision.store(revision, std::memory_order_release);
}

json ManagedBudgetJournalAccess::lease_transport(const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    require(static_cast<bool>(lease), "Managed budget transport requires an owned receipt");
    require(lease->execution_thread_id() == lease->scope().thread_id &&
            lease->execution_storage_thread_id() == storage_key(lease->scope()),
            "Shared-bank branch execution authority cannot be transported as an original lease");
    std::lock_guard lock(lease->impl_->mutex);
    return {{"schema", lease_schema}, {"scope", scope_json(lease->scope())}, {"actor", lease->actor_id()},
        {"bank_generation", lease->bank_generation()}, {"revision", lease->impl_->revision.load(std::memory_order_relaxed)},
        {"head_checkpoint_id", lease->impl_->checkpoint_id}, {"head_commitment", lease->impl_->commitment}};
}
std::shared_ptr<OwnedManagedBudgetLease> ManagedBudgetJournalAccess::lease_from_transport(const json& value) {
    closed(value, {"schema", "scope", "actor", "bank_generation", "revision", "head_checkpoint_id", "head_commitment"});
    require(text(value, "schema") == lease_schema, "Unsupported managed budget lease transport schema");
    auto scope = parse_scope(value.at("scope"));
    auto actor = text(value, "actor"), generation = text(value, "bank_generation");
    nonce_identity(actor);
    nonce_identity(generation);
    const auto revision = number(value, "revision");
    require(revision != 0, "Invalid managed budget lease transport revision");
    auto id = text(value, "head_checkpoint_id", true), commitment = text(value, "head_commitment", true);
    require(id.empty() ? commitment.empty() : neograph::detail::is_sha256_identity(commitment),
            "Invalid managed budget lease transport commitment");
    auto impl = std::make_shared<OwnedManagedBudgetLease::Impl>(std::move(scope), std::move(actor), std::move(generation),
                                                               revision, std::move(id), std::move(commitment));
    return std::shared_ptr<OwnedManagedBudgetLease>(new OwnedManagedBudgetLease(std::move(impl)));
}
json ManagedBudgetJournalAccess::effect_transport(const ManagedBudgetEffectReceipt& effect) {
    require(effect.active(), "Managed budget transport requires an actual effect receipt");
    const auto& value = *effect.impl_;
    return {{"schema", receipt_schema}, {"storage_thread_id", value.storage}, {"actor", value.actor},
        {"bank_generation", value.generation}, {"effect_id", value.id}, {"claim_amount", value.amount},
        {"request_digest", value.digest}, {"begin_revision", value.begin_revision}};
}
ManagedBudgetEffectReceipt ManagedBudgetJournalAccess::effect_from_transport(const json& value) {
    closed(value, {"schema", "storage_thread_id", "actor", "bank_generation", "effect_id", "claim_amount",
                   "request_digest", "begin_revision"});
    require(text(value, "schema") == receipt_schema, "Unsupported managed budget effect transport schema");
    auto storage = text(value, "storage_thread_id"), actor = text(value, "actor"), generation = text(value, "bank_generation");
    nonce_identity(actor);
    nonce_identity(generation);
    auto id = text(value, "effect_id"), digest = text(value, "request_digest");
    require(neograph::detail::is_sha256_identity(digest), "Invalid managed budget effect transport digest");
    const auto amount = number(value, "claim_amount"), revision = number(value, "begin_revision");
    require(amount != 0 && revision != 0, "Invalid managed budget effect transport claim");
    auto impl = std::make_shared<const ManagedBudgetEffectReceipt::Impl>(std::move(storage), std::move(actor),
        std::move(generation), std::move(id), amount, std::move(digest), revision);
    return ManagedBudgetEffectReceipt(std::move(impl));
}

} // namespace detail
} // namespace neograph::graph
