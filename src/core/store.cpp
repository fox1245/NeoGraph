#include <neograph/graph/store.h>
#include <algorithm>
#include <set>

namespace neograph::graph {

namespace {

bool namespace_has_prefix(const Namespace& ns, const Namespace& prefix) {
    return ns.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), ns.begin());
}

} // namespace

std::string InMemoryStore::make_key(const Namespace& ns, const std::string& key) {
    auto result = ns_to_string(ns);
    result += std::to_string(key.size());
    result += ":";
    result += key;
    return result;
}

std::string InMemoryStore::ns_to_string(const Namespace& ns) {
    std::string result = std::to_string(ns.size()) + ":";
    for (const auto& component : ns) {
        result += std::to_string(component.size());
        result += ":";
        result += component;
    }
    return result;
}

bool InMemoryStore::starts_with(const std::string& str, const std::string& prefix) {
    if (prefix.empty()) return true;
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
}

void InMemoryStore::put(const Namespace& ns, const std::string& key, const json& value) {
    std::lock_guard lock(mutex_);
    auto composite = make_key(ns, key);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto it = items_.find(composite);
    if (it != items_.end()) {
        it->second.value = value;
        it->second.updated_at = now;
    } else {
        items_[composite] = StoreItem{ns, key, value, now, now};
    }
}

std::optional<StoreItem> InMemoryStore::get(const Namespace& ns, const std::string& key) const {
    std::lock_guard lock(mutex_);
    auto it = items_.find(make_key(ns, key));
    if (it == items_.end()) return std::nullopt;
    return it->second;
}

std::vector<StoreItem> InMemoryStore::search(const Namespace& ns_prefix, int limit) const {
    std::lock_guard lock(mutex_);

    std::vector<StoreItem> results;
    for (const auto& [composite, item] : items_) {
        if (namespace_has_prefix(item.ns, ns_prefix)) {
            results.push_back(item);
            if (static_cast<int>(results.size()) >= limit) break;
        }
    }
    return results;
}

void InMemoryStore::delete_item(const Namespace& ns, const std::string& key) {
    std::lock_guard lock(mutex_);
    items_.erase(make_key(ns, key));
}

std::vector<Namespace> InMemoryStore::list_namespaces(const Namespace& prefix) const {
    std::lock_guard lock(mutex_);

    std::set<Namespace> seen;
    std::vector<Namespace> results;

    for (const auto& [composite, item] : items_) {
        if (namespace_has_prefix(item.ns, prefix) && seen.insert(item.ns).second) {
            results.push_back(item.ns);
        }
    }
    return results;
}

size_t InMemoryStore::size() const {
    std::lock_guard lock(mutex_);
    return items_.size();
}

} // namespace neograph::graph
