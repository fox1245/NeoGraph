#include <neograph/program/store.h>

#include <map>
#include <mutex>
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

}  // namespace neograph::program
