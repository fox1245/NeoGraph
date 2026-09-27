#include <neograph/tenant.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace neograph::tenant {
namespace {

std::string encode_component(std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value);
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

TenantScope::TenantScope(std::string tenant_id, std::string authorization_scope)
    : tenant_id_(std::move(tenant_id)), authorization_scope_(std::move(authorization_scope)) {
    if (tenant_id_.empty() || authorization_scope_.empty())
        throw std::invalid_argument("TenantScope requires tenant and authorization IDs");
    if (tenant_id_.find('\0') != std::string::npos ||
        authorization_scope_.find('\0') != std::string::npos)
        throw std::invalid_argument("TenantScope IDs must not contain NUL bytes");
}

bool TenantScope::authorizes(std::string_view tenant_id,
                             std::string_view authorization_scope) const noexcept {
    return tenant_id_ == tenant_id && authorization_scope_ == authorization_scope;
}

TenantQuotaExceeded::TenantQuotaExceeded(const char* resource)
    : std::runtime_error(std::string("tenant quota exhausted: ") + resource) {}

TenantQuota::TenantQuota(TenantQuotas limits) : limits_(limits) {}

std::uint64_t TenantQuota::active() const noexcept { return active_.load(std::memory_order_relaxed); }
std::uint64_t TenantQuota::queued() const noexcept { return queued_.load(std::memory_order_relaxed); }
std::uint64_t TenantQuota::model_tokens() const noexcept {
    return model_tokens_.load(std::memory_order_relaxed);
}
std::uint64_t TenantQuota::cost_microunits() const noexcept {
    return cost_microunits_.load(std::memory_order_relaxed);
}
std::uint64_t TenantQuota::artifacts() const noexcept {
    return artifacts_.load(std::memory_order_relaxed);
}
std::uint64_t TenantQuota::retained_artifacts() const noexcept {
    return retained_artifacts_.load(std::memory_order_relaxed);
}

bool TenantQuota::try_increment(std::atomic<std::uint64_t>& value,
                                std::uint64_t limit) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    for (;;) {
        if (limit != 0 && current >= limit) return false;
        if (current == std::numeric_limits<std::uint64_t>::max()) return false;
        if (value.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                        std::memory_order_relaxed))
            return true;
    }
}

void TenantQuota::decrement(std::atomic<std::uint64_t>& value,
                            std::uint64_t amount) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    for (;;) {
        if (current < amount) {
            value.store(0, std::memory_order_release);
            return;
        }
        if (value.compare_exchange_weak(current, current - amount, std::memory_order_acq_rel,
                                        std::memory_order_relaxed))
            return;
    }
}

bool TenantQuota::try_acquire_concurrency() noexcept {
    return try_increment(active_, limits_.max_concurrency);
}
void TenantQuota::release_concurrency() noexcept { decrement(active_, 1); }
bool TenantQuota::try_acquire_queue() noexcept { return try_increment(queued_, limits_.max_queue); }
void TenantQuota::release_queue() noexcept { decrement(queued_, 1); }

bool TenantQuota::try_reserve_usage(std::uint64_t model_tokens,
                                    std::uint64_t cost_microunits) noexcept {
    if (model_tokens > std::numeric_limits<std::uint64_t>::max() - model_tokens_.load()) return false;
    if (cost_microunits > std::numeric_limits<std::uint64_t>::max() - cost_microunits_.load())
        return false;

    auto reserve = [](std::atomic<std::uint64_t>& value, std::uint64_t amount,
                      std::uint64_t limit) noexcept {
        if (limit != 0 && amount > limit) return false;
        auto current = value.load(std::memory_order_relaxed);
        for (;;) {
            if (amount > std::numeric_limits<std::uint64_t>::max() - current) return false;
            if (limit != 0 && current > limit - amount) return false;
            if (value.compare_exchange_weak(current, current + amount, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) return true;
        }
    };
    if (!reserve(model_tokens_, model_tokens, limits_.max_model_tokens)) return false;
    if (!reserve(cost_microunits_, cost_microunits, limits_.max_cost_microunits)) {
        decrement(model_tokens_, model_tokens);
        return false;
    }
    return true;
}

void TenantQuota::release_usage(std::uint64_t model_tokens,
                                std::uint64_t cost_microunits) noexcept {
    decrement(model_tokens_, model_tokens);
    decrement(cost_microunits_, cost_microunits);
}
bool TenantQuota::try_acquire_artifact() noexcept {
    return try_increment(artifacts_, limits_.max_artifacts);
}
void TenantQuota::release_artifact() noexcept { decrement(artifacts_, 1); }
bool TenantQuota::try_acquire_retained_artifact() noexcept {
    return try_increment(retained_artifacts_, limits_.max_retained_artifacts);
}
void TenantQuota::release_retained_artifact() noexcept { decrement(retained_artifacts_, 1); }

TenantQuotaLease TenantQuotaLease::acquire(TenantQuota& quota, bool artifact) {
    if (!quota.try_acquire_concurrency()) throw TenantQuotaExceeded("concurrency");
    if (artifact && !quota.try_acquire_artifact()) {
        quota.release_concurrency();
        throw TenantQuotaExceeded("artifacts");
    }
    return TenantQuotaLease(&quota, artifact);
}

TenantQuotaLease::TenantQuotaLease(TenantQuotaLease&& other) noexcept
    : quota_(std::exchange(other.quota_, nullptr)), artifact_(std::exchange(other.artifact_, false)) {}

TenantQuotaLease& TenantQuotaLease::operator=(TenantQuotaLease&& other) noexcept {
    if (this != &other) {
        release();
        quota_ = std::exchange(other.quota_, nullptr);
        artifact_ = std::exchange(other.artifact_, false);
    }
    return *this;
}

TenantQuotaLease::~TenantQuotaLease() { release(); }

void TenantQuotaLease::release() noexcept {
    if (!quota_) return;
    if (artifact_) quota_->release_artifact();
    quota_->release_concurrency();
    quota_ = nullptr;
    artifact_ = false;
}

ScopedStore::ScopedStore(TenantScope scope, std::shared_ptr<graph::Store> backend)
    : scope_(std::move(scope)), backend_(std::move(backend)) {
    if (!backend_) throw std::invalid_argument("ScopedStore requires a backend");
}

graph::Namespace ScopedStore::private_namespace(const graph::Namespace& ns) const {
    graph::Namespace result;
    result.reserve(ns.size() + 1);
    result.push_back("__neograph_tenant__" + encode_component(scope_.tenant_id()));
    result.insert(result.end(), ns.begin(), ns.end());
    return result;
}

graph::StoreItem ScopedStore::public_item(graph::StoreItem item) const {
    const auto prefix = std::string("__neograph_tenant__") + encode_component(scope_.tenant_id());
    if (!item.ns.empty() && item.ns.front() == prefix) item.ns.erase(item.ns.begin());
    return item;
}

void ScopedStore::put(const graph::Namespace& ns, const std::string& key, const json& value) {
    backend_->put(private_namespace(ns), key, value);
}

std::optional<graph::StoreItem> ScopedStore::get(const graph::Namespace& ns,
                                                  const std::string& key) const {
    auto item = backend_->get(private_namespace(ns), key);
    if (item) *item = public_item(std::move(*item));
    return item;
}

std::vector<graph::StoreItem> ScopedStore::search(const graph::Namespace& ns_prefix, int limit) const {
    auto items = backend_->search(private_namespace(ns_prefix), limit);
    for (auto& item : items) item = public_item(std::move(item));
    return items;
}

void ScopedStore::delete_item(const graph::Namespace& ns, const std::string& key) {
    backend_->delete_item(private_namespace(ns), key);
}

std::vector<graph::Namespace>
ScopedStore::list_namespaces(const graph::Namespace& prefix) const {
    auto values = backend_->list_namespaces(private_namespace(prefix));
    const auto tenant_prefix = std::string("__neograph_tenant__") + encode_component(scope_.tenant_id());
    std::vector<graph::Namespace> result;
    result.reserve(values.size());
    for (auto& value : values) {
        if (value.empty() || value.front() != tenant_prefix) continue;
        value.erase(value.begin());
        result.push_back(std::move(value));
    }
    return result;
}

ScopedCheckpointStore::ScopedCheckpointStore(TenantScope scope,
                                             std::shared_ptr<graph::CheckpointStore> backend)
    : scope_(std::move(scope)), backend_(std::move(backend)) {
    if (!backend_) throw std::invalid_argument("ScopedCheckpointStore requires a backend");
}

std::string ScopedCheckpointStore::private_id(std::string_view id) const {
    return "__neograph_tenant__" + encode_component(scope_.tenant_id()) + ":" +
           encode_component(id);
}

graph::Checkpoint ScopedCheckpointStore::private_checkpoint(const graph::Checkpoint& cp) const {
    auto result = cp;
    result.thread_id = private_id(cp.thread_id);
    result.id = private_id(cp.id);
    if (!cp.parent_id.empty()) result.parent_id = private_id(cp.parent_id);
    return result;
}

graph::Checkpoint ScopedCheckpointStore::public_checkpoint(graph::Checkpoint cp) const {
    const auto prefix = std::string("__neograph_tenant__") + encode_component(scope_.tenant_id()) + ":";
    auto strip = [&](std::string& id) {
        if (!starts_with(id, prefix)) return false;
        id.erase(0, prefix.size());
        const auto colon = id.find(':');
        if (colon == std::string::npos) return false;
        const auto length = id.substr(0, colon);
        std::size_t count = 0;
        try { count = static_cast<std::size_t>(std::stoull(length)); } catch (...) { return false; }
        if (colon + 1 + count != id.size()) return false;
        id.erase(0, colon + 1);
        return true;
    };
    (void)strip(cp.thread_id);
    (void)strip(cp.id);
    if (!cp.parent_id.empty()) (void)strip(cp.parent_id);
    return cp;
}

void ScopedCheckpointStore::save(const graph::Checkpoint& cp) {
    backend_->save(private_checkpoint(cp));
}

std::optional<graph::Checkpoint>
ScopedCheckpointStore::load_latest(const std::string& thread_id) {
    auto cp = backend_->load_latest(private_id(thread_id));
    if (cp) *cp = public_checkpoint(std::move(*cp));
    return cp;
}

std::optional<graph::Checkpoint>
ScopedCheckpointStore::load_by_id(const std::string& id) {
    auto cp = backend_->load_by_id(private_id(id));
    if (cp) *cp = public_checkpoint(std::move(*cp));
    return cp;
}

std::vector<graph::Checkpoint> ScopedCheckpointStore::list(const std::string& thread_id, int limit) {
    auto cps = backend_->list(private_id(thread_id), limit);
    for (auto& cp : cps) cp = public_checkpoint(std::move(cp));
    return cps;
}

void ScopedCheckpointStore::delete_thread(const std::string& thread_id) {
    backend_->delete_thread(private_id(thread_id));
}

void ScopedCheckpointStore::put_writes(const std::string& thread_id,
                                       const std::string& parent_checkpoint_id,
                                       const graph::PendingWrite& write) {
    backend_->put_writes(private_id(thread_id), private_id(parent_checkpoint_id), write);
}

std::vector<graph::PendingWrite>
ScopedCheckpointStore::get_writes(const std::string& thread_id,
                                  const std::string& parent_checkpoint_id) {
    return backend_->get_writes(private_id(thread_id), private_id(parent_checkpoint_id));
}

void ScopedCheckpointStore::clear_writes(const std::string& thread_id,
                                         const std::string& parent_checkpoint_id) {
    backend_->clear_writes(private_id(thread_id), private_id(parent_checkpoint_id));
}

}  // namespace neograph::tenant
