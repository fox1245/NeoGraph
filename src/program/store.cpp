#include <neograph/program/store.h>

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <set>
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

void InMemoryProgramStore::publish_admitted(const ProgramBundle&  bundle,
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

std::optional<ProgramBundle> InMemoryProgramStore::get_bundle(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto      found = impl_->bundles.find(id);
    if (found == impl_->bundles.end()) {
        return std::nullopt;
    }
    return found->second.value;
}

std::optional<ProgramVersion> InMemoryProgramStore::get_version(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto      found = impl_->versions.find(id);
    if (found == impl_->versions.end()) {
        return std::nullopt;
    }
    return found->second.value;
}

std::optional<ProgramActivation>
InMemoryProgramStore::get_activation(std::string_view owner_scope) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->activations.find(owner_scope);
    if (found == impl_->activations.end()) return std::nullopt;
    return found->second.value;
}

ProgramActivationResult InMemoryProgramStore::compare_activate(
    std::string_view owner_scope,
    std::uint64_t expected_generation,
    std::string_view version_id,
    std::string_view policy_snapshot_hash) {
    std::lock_guard lock(impl_->mutex);
    const auto version = impl_->versions.find(version_id);
    if (version == impl_->versions.end())
        throw std::invalid_argument("Cannot activate an unpublished Program version");
    if (version->second.value.ownership_scope() != owner_scope)
        throw std::invalid_argument("Program activation owner scope does not match the version");

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
InMemoryProgramStore::list_versions(std::string_view owner_scope) const {
    std::lock_guard lock(impl_->mutex);
    std::vector<ProgramVersion> result;
    for (const auto& [id, stored] : impl_->versions)
        if (stored.value.ownership_scope() == owner_scope) result.push_back(stored.value);
    return result;
}

ProgramRetentionReport InMemoryProgramStore::collect_garbage(
    std::string_view owner_scope,
    const std::vector<std::string>& pinned_version_ids) {
    std::lock_guard lock(impl_->mutex);
    std::set<std::string, std::less<>> keep(pinned_version_ids.begin(), pinned_version_ids.end());
    const auto active = impl_->activations.find(owner_scope);
    if (active != impl_->activations.end()) keep.insert(active->second.value.active_version_id());

    ProgramRetentionReport report;
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
    return report;
}

}  // namespace neograph::program
