#include <neograph/program/store.h>
#include "lock_audit.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <mutex>
#include <tuple>
#include <stdexcept>
#include <string>
#include <utility>

namespace neograph::program {

namespace {

template <typename Value>
struct StoredValue {
    Value       value;
    std::string canonical_bytes;
};
}  // namespace


struct InMemoryProgramStore::Impl {
    mutable std::mutex                                              mutex;
    std::map<std::string, StoredValue<ProgramBundle>, std::less<>>  bundles;
    std::map<std::string, StoredValue<ProgramVersion>, std::less<>> versions;
    std::map<std::string, StoredValue<ProgramActivation>, std::less<>> activations;
};

InMemoryProgramStore::InMemoryProgramStore() : impl_(std::make_unique<Impl>()) {}

InMemoryProgramStore::InMemoryProgramStore(InMemoryProgramStore&&) noexcept = default;

InMemoryProgramStore& InMemoryProgramStore::operator=(InMemoryProgramStore&&) noexcept = default;

InMemoryProgramStore::~InMemoryProgramStore() = default;

void InMemoryProgramStore::publish_admitted_sync(const ProgramBundle&  bundle,
                                            const ProgramVersion& version) {
    if (version.bundle_id() != bundle.id()) {
        throw std::invalid_argument("Program version does not bind the published bundle");
    }

    const std::string bundle_id     = bundle.id();
    const std::string version_id    = version.id();
    std::string       bundle_bytes  = bundle.serialize_canonical();
    std::string       version_bytes = version.serialize_canonical();

    std::lock_guard lock(impl_->mutex);

    const auto existing_bundle = impl_->bundles.find(bundle_id);
    if (existing_bundle != impl_->bundles.end() &&
        existing_bundle->second.canonical_bytes != bundle_bytes) {
        throw std::invalid_argument("Program bundle content identity collision");
    }

    const auto existing_version = impl_->versions.find(version_id);
    if (existing_version != impl_->versions.end() &&
        existing_version->second.canonical_bytes != version_bytes) {
        throw std::invalid_argument("Program version content identity collision");
    }

    if (existing_bundle != impl_->bundles.end() && existing_version != impl_->versions.end()) {
        return;
    }

    auto inserted_bundle = impl_->bundles.end();
    if (existing_bundle == impl_->bundles.end()) {
        auto [position, inserted] = impl_->bundles.try_emplace(
            bundle_id, StoredValue<ProgramBundle>{bundle, std::move(bundle_bytes)});
        if (inserted) {
            inserted_bundle = position;
        }
    }

    try {
        if (existing_version == impl_->versions.end()) {
            impl_->versions.try_emplace(
                version_id, StoredValue<ProgramVersion>{version, std::move(version_bytes)});
        }
    } catch (...) {
        if (inserted_bundle != impl_->bundles.end()) {
            impl_->bundles.erase(inserted_bundle);
        }
        throw;
    }
}

std::optional<ProgramBundle> InMemoryProgramStore::get_bundle_sync(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto      found = impl_->bundles.find(id);
    if (found == impl_->bundles.end()) {
        return std::nullopt;
    }
    return found->second.value;
}

std::optional<ProgramVersion> InMemoryProgramStore::get_version_sync(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto      found = impl_->versions.find(id);
    if (found == impl_->versions.end()) {
        return std::nullopt;
    }
    return found->second.value;
}

std::optional<ProgramVersion>
InMemoryProgramStore::get_version_sync(std::string_view owner_scope, std::string_view id) const {
    if (owner_scope.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto      found = impl_->versions.find(id);
    if (found == impl_->versions.end() || found->second.value.ownership_scope() != owner_scope)
        return std::nullopt;
    return found->second.value;
}

std::optional<ProgramBundle>
InMemoryProgramStore::get_bundle_sync(std::string_view owner_scope, std::string_view id) const {
    if (owner_scope.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto bundle = impl_->bundles.find(id);
    if (bundle == impl_->bundles.end()) return std::nullopt;
    for (const auto& [version_id, stored] : impl_->versions) {
        (void)version_id;
        if (stored.value.ownership_scope() == owner_scope &&
            stored.value.bundle_id() == id)
            return bundle->second.value;
    }
    return std::nullopt;
}

std::optional<ProgramActivation>
InMemoryProgramStore::get_activation_sync(std::string_view owner_scope) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->activations.find(owner_scope);
    if (found == impl_->activations.end()) return std::nullopt;
    return found->second.value;
}
std::optional<ProgramActivationBinding>
InMemoryProgramStore::get_active_binding_sync(std::string_view owner_scope) const {
    if (owner_scope.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto active = impl_->activations.find(owner_scope);
    if (active == impl_->activations.end()) return std::nullopt;
    const auto version = impl_->versions.find(active->second.value.active_version_id());
    if (version == impl_->versions.end() ||
        version->second.value.ownership_scope() != owner_scope ||
        version->second.value.policy_snapshot().fingerprint() !=
            active->second.value.policy_snapshot_hash()) {
        throw std::runtime_error("Program activation target is missing or has a mismatched policy");
    }
    return ProgramActivationBinding{active->second.value, version->second.value};
}

ProgramActivationResult InMemoryProgramStore::compare_activate_sync(
    std::string_view owner_scope,
    std::uint64_t expected_generation,
    std::string_view version_id,
    std::string_view policy_snapshot_hash) {
    if (owner_scope.empty())
        throw std::invalid_argument("Program activation owner scope must not be empty");
    std::lock_guard lock(impl_->mutex);
    const auto version = impl_->versions.find(version_id);
    if (version == impl_->versions.end())
        throw std::invalid_argument("Cannot activate an unpublished Program version");
    if (version->second.value.ownership_scope() != owner_scope)
        throw std::invalid_argument("Program activation owner scope does not match the version");
    if (version->second.value.policy_snapshot().fingerprint() != policy_snapshot_hash)
        throw std::invalid_argument("Program activation policy hash does not match the version");

    const auto current = impl_->activations.find(owner_scope);
    const auto generation =
        current == impl_->activations.end() ? std::uint64_t{0} : current->second.value.generation();
    if (generation != expected_generation) return ProgramActivationResult::Conflict;
    if (current != impl_->activations.end() &&
        current->second.value.active_version_id() == version_id &&
        current->second.value.policy_snapshot_hash() == policy_snapshot_hash)
        return ProgramActivationResult::AlreadyPresent;
    if (expected_generation == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Program activation generation exhausted");

    auto activation = ProgramActivation::create(
        ProgramActivationData{std::string(owner_scope), std::string(version_id),
                              expected_generation + 1, std::string(policy_snapshot_hash)});
    auto bytes = activation.serialize_canonical();
    impl_->activations.insert_or_assign(
        std::string(owner_scope),
        StoredValue<ProgramActivation>{std::move(activation), std::move(bytes)});
    return ProgramActivationResult::Activated;
}

std::vector<ProgramVersion>
InMemoryProgramStore::list_versions_sync(std::string_view owner_scope) const {
    if (owner_scope.empty())
        throw std::invalid_argument("Program version owner scope must not be empty");
    std::lock_guard lock(impl_->mutex);
    std::vector<ProgramVersion> result;
    for (const auto& [id, stored] : impl_->versions)
        if (stored.value.ownership_scope() == owner_scope) result.push_back(stored.value);
    return result;
}

ProgramRetentionReport InMemoryProgramStore::collect_garbage_sync(
    std::string_view owner_scope,
    const std::vector<std::string>& pinned_version_ids) {
    if (owner_scope.empty())
        throw std::invalid_argument("Program retention owner scope must not be empty");
    std::lock_guard lock(impl_->mutex);
    for (const auto& pinned_id : pinned_version_ids) {
        const auto pinned = impl_->versions.find(pinned_id);
        if (pinned == impl_->versions.end())
            throw std::invalid_argument("Program retention pin references an unpublished version");
        if (pinned->second.value.ownership_scope() != owner_scope)
            throw std::invalid_argument("Program retention pin crosses an owner scope boundary");
    }
    std::set<std::string, std::less<>> keep(pinned_version_ids.begin(), pinned_version_ids.end());
    const auto active = impl_->activations.find(owner_scope);
    ProgramRetentionReport report;
    for (const auto& pinned_id : pinned_version_ids)
        report.references.push_back({pinned_id, "host_pin"});
    if (active != impl_->activations.end()) {
        keep.insert(active->second.value.active_version_id());
        report.references.push_back({active->second.value.active_version_id(), "active_pointer"});
    }
    std::set<std::string, std::less<>> bundles_to_check;
    for (auto it = impl_->versions.begin(); it != impl_->versions.end();) {
        if (it->second.value.ownership_scope() == owner_scope && !keep.contains(it->first)) {
            bundles_to_check.insert(it->second.value.bundle_id());
            it = impl_->versions.erase(it);
            ++report.versions_removed;
        } else {
            ++it;
        }
    }
    for (const auto& bundle_id : bundles_to_check) {
        bool referenced = false;
        for (const auto& [id, version] : impl_->versions)
            if (version.value.bundle_id() == bundle_id) {
                referenced = true;
                break;
            }
        if (!referenced && impl_->bundles.erase(bundle_id) != 0) ++report.bundles_removed;
    }
    std::sort(report.references.begin(), report.references.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.version_id, lhs.reason) <
                         std::tie(rhs.version_id, rhs.reason);
              });
    return report;
}


// ---- ProgramStore : the synchronous implementer base and the blocking facade (#556) ----

asio::awaitable<void> SyncProgramStore::publish_admitted(
    const ProgramBundle& bundle,
    const ProgramVersion& version) {
    detail::assert_no_pure_lock_held("ProgramStore::publish_admitted");
    if (!offloads()) {
        publish_admitted_sync(bundle, version);
        co_return;
    }
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<void>([this, bundle = bundle, version = version]() mutable -> void {
        return publish_admitted_sync(bundle, version);
    });
}

asio::awaitable<std::optional<ProgramBundle>> SyncProgramStore::get_bundle(std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_bundle");
    if (!offloads()) co_return get_bundle_sync(id);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::optional<ProgramBundle>>([this, id = std::string(id)]() mutable -> std::optional<ProgramBundle> {
        return get_bundle_sync(id);
    });
}

asio::awaitable<std::optional<ProgramVersion>> SyncProgramStore::get_version(
    std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_version");
    if (!offloads()) co_return get_version_sync(id);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::optional<ProgramVersion>>([this, id = std::string(id)]() mutable -> std::optional<ProgramVersion> {
        return get_version_sync(id);
    });
}

asio::awaitable<std::optional<ProgramBundle>> SyncProgramStore::get_bundle(
    std::string_view owner_scope,
    std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_bundle");
    if (!offloads()) co_return get_bundle_sync(owner_scope, id);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::optional<ProgramBundle>>([this, owner_scope = std::string(owner_scope), id = std::string(id)]() mutable -> std::optional<ProgramBundle> {
        return get_bundle_sync(owner_scope, id);
    });
}

asio::awaitable<std::optional<ProgramVersion>> SyncProgramStore::get_version(
    std::string_view owner_scope,
    std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_version");
    if (!offloads()) co_return get_version_sync(owner_scope, id);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::optional<ProgramVersion>>([this, owner_scope = std::string(owner_scope), id = std::string(id)]() mutable -> std::optional<ProgramVersion> {
        return get_version_sync(owner_scope, id);
    });
}

asio::awaitable<std::optional<ProgramActivation>> SyncProgramStore::get_activation(
    std::string_view owner_scope) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_activation");
    if (!offloads()) co_return get_activation_sync(owner_scope);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::optional<ProgramActivation>>([this, owner_scope = std::string(owner_scope)]() mutable -> std::optional<ProgramActivation> {
        return get_activation_sync(owner_scope);
    });
}

asio::awaitable<std::optional<ProgramActivationBinding>> SyncProgramStore::get_active_binding(
    std::string_view owner_scope) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_active_binding");
    if (!offloads()) co_return get_active_binding_sync(owner_scope);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::optional<ProgramActivationBinding>>([this, owner_scope = std::string(owner_scope)]() mutable -> std::optional<ProgramActivationBinding> {
        return get_active_binding_sync(owner_scope);
    });
}

asio::awaitable<ProgramActivationResult> SyncProgramStore::compare_activate(
    std::string_view owner_scope,
    std::uint64_t expected_generation,
    std::string_view version_id,
    std::string_view policy_snapshot_hash) {
    detail::assert_no_pure_lock_held("ProgramStore::compare_activate");
    if (!offloads()) co_return compare_activate_sync(owner_scope, expected_generation, version_id, policy_snapshot_hash);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<ProgramActivationResult>([this, owner_scope = std::string(owner_scope), expected_generation, version_id = std::string(version_id), policy_snapshot_hash = std::string(policy_snapshot_hash)]() mutable -> ProgramActivationResult {
        return compare_activate_sync(owner_scope, expected_generation, version_id, policy_snapshot_hash);
    });
}

asio::awaitable<std::vector<ProgramVersion>> SyncProgramStore::list_versions(
    std::string_view owner_scope) const {
    detail::assert_no_pure_lock_held("ProgramStore::list_versions");
    if (!offloads()) co_return list_versions_sync(owner_scope);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::vector<ProgramVersion>>([this, owner_scope = std::string(owner_scope)]() mutable -> std::vector<ProgramVersion> {
        return list_versions_sync(owner_scope);
    });
}

asio::awaitable<ProgramRetentionReport> SyncProgramStore::collect_garbage(
    std::string_view owner_scope,
    const std::vector<std::string>& pinned_version_ids) {
    detail::assert_no_pure_lock_held("ProgramStore::collect_garbage");
    if (!offloads()) co_return collect_garbage_sync(owner_scope, pinned_version_ids);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<ProgramRetentionReport>([this, owner_scope = std::string(owner_scope), pinned_version_ids = pinned_version_ids]() mutable -> ProgramRetentionReport {
        return collect_garbage_sync(owner_scope, pinned_version_ids);
    });
}

BlockingProgramStore::BlockingProgramStore(std::shared_ptr<ProgramStore> store, BlockingStorePolicy policy)
    : store_(std::move(store)), policy_(std::move(policy)) {
    if (!store_) throw std::invalid_argument("BlockingProgramStore requires a store");
    if (!policy_.await_synchronous_stores) sync_ = dynamic_cast<SyncProgramStore*>(store_.get());
}

BlockingProgramStore::BlockingProgramStore(ProgramStore& store, BlockingStorePolicy policy)
    : BlockingProgramStore(std::shared_ptr<ProgramStore>(std::shared_ptr<ProgramStore>{}, &store), std::move(policy)) {}

void BlockingProgramStore::publish_admitted(
    const ProgramBundle& bundle,
    const ProgramVersion& version) const {
    detail::assert_no_pure_lock_held("ProgramStore::publish_admitted");
    if (sync_) return sync_->publish_admitted_sync(bundle, version);
    return detail::block_on_store(store_->publish_admitted(bundle, version), policy_, "ProgramStore::publish_admitted");
}

std::optional<ProgramBundle> BlockingProgramStore::get_bundle(std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_bundle");
    if (sync_) return sync_->get_bundle_sync(id);
    return detail::block_on_store(store_->get_bundle(id), policy_, "ProgramStore::get_bundle");
}

std::optional<ProgramVersion> BlockingProgramStore::get_version(std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_version");
    if (sync_) return sync_->get_version_sync(id);
    return detail::block_on_store(store_->get_version(id), policy_, "ProgramStore::get_version");
}

std::optional<ProgramBundle> BlockingProgramStore::get_bundle(
    std::string_view owner_scope,
    std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_bundle");
    if (sync_) return sync_->get_bundle_sync(owner_scope, id);
    return detail::block_on_store(store_->get_bundle(owner_scope, id), policy_, "ProgramStore::get_bundle");
}

std::optional<ProgramVersion> BlockingProgramStore::get_version(
    std::string_view owner_scope,
    std::string_view id) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_version");
    if (sync_) return sync_->get_version_sync(owner_scope, id);
    return detail::block_on_store(store_->get_version(owner_scope, id), policy_, "ProgramStore::get_version");
}

std::optional<ProgramActivation> BlockingProgramStore::get_activation(
    std::string_view owner_scope) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_activation");
    if (sync_) return sync_->get_activation_sync(owner_scope);
    return detail::block_on_store(store_->get_activation(owner_scope), policy_, "ProgramStore::get_activation");
}

std::optional<ProgramActivationBinding> BlockingProgramStore::get_active_binding(
    std::string_view owner_scope) const {
    detail::assert_no_pure_lock_held("ProgramStore::get_active_binding");
    if (sync_) return sync_->get_active_binding_sync(owner_scope);
    return detail::block_on_store(store_->get_active_binding(owner_scope), policy_, "ProgramStore::get_active_binding");
}

ProgramActivationResult BlockingProgramStore::compare_activate(
    std::string_view owner_scope,
    std::uint64_t expected_generation,
    std::string_view version_id,
    std::string_view policy_snapshot_hash) const {
    detail::assert_no_pure_lock_held("ProgramStore::compare_activate");
    if (sync_) return sync_->compare_activate_sync(owner_scope, expected_generation, version_id, policy_snapshot_hash);
    return detail::block_on_store(store_->compare_activate(owner_scope, expected_generation, version_id, policy_snapshot_hash), policy_, "ProgramStore::compare_activate");
}

std::vector<ProgramVersion> BlockingProgramStore::list_versions(std::string_view owner_scope) const {
    detail::assert_no_pure_lock_held("ProgramStore::list_versions");
    if (sync_) return sync_->list_versions_sync(owner_scope);
    return detail::block_on_store(store_->list_versions(owner_scope), policy_, "ProgramStore::list_versions");
}

ProgramRetentionReport BlockingProgramStore::collect_garbage(
    std::string_view owner_scope,
    const std::vector<std::string>& pinned_version_ids) const {
    detail::assert_no_pure_lock_held("ProgramStore::collect_garbage");
    if (sync_) return sync_->collect_garbage_sync(owner_scope, pinned_version_ids);
    return detail::block_on_store(store_->collect_garbage(owner_scope, pinned_version_ids), policy_, "ProgramStore::collect_garbage");
}

}  // namespace neograph::program
