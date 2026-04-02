#pragma once

#include <neograph/types.h>
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <map>
#include <chrono>
#include <tuple>

namespace neograph::graph {

// =========================================================================
// Store: cross-thread shared memory (LangGraph Store equivalent)
//
// Provides namespaced key-value storage that persists across threads.
// Use cases: long-term user preferences, shared knowledge, agent memory.
//
// Namespace is a vector of strings forming a hierarchical path,
// e.g., {"users", "user123", "preferences"}.
// =========================================================================

using Namespace = std::vector<std::string>;

struct StoreItem {
    Namespace   ns;
    std::string key;
    json        value;
    int64_t     created_at;
    int64_t     updated_at;
};

class Store {
public:
    virtual ~Store() = default;

    // Put a value (create or update)
    virtual void put(const Namespace& ns, const std::string& key, const json& value) = 0;

    // Get a single item
    virtual std::optional<StoreItem> get(const Namespace& ns, const std::string& key) const = 0;

    // Search items under a namespace prefix
    virtual std::vector<StoreItem> search(const Namespace& ns_prefix, int limit = 100) const = 0;

    // Delete an item
    virtual void delete_item(const Namespace& ns, const std::string& key) = 0;

    // List namespaces under a prefix
    virtual std::vector<Namespace> list_namespaces(const Namespace& prefix = {}) const = 0;
};

// =========================================================================
// InMemoryStore: for testing and single-process use
// =========================================================================
class InMemoryStore : public Store {
public:
    void put(const Namespace& ns, const std::string& key, const json& value) override;
    std::optional<StoreItem> get(const Namespace& ns, const std::string& key) const override;
    std::vector<StoreItem> search(const Namespace& ns_prefix, int limit = 100) const override;
    void delete_item(const Namespace& ns, const std::string& key) override;
    std::vector<Namespace> list_namespaces(const Namespace& prefix = {}) const override;

    size_t size() const;

private:
    // Composite key: namespace joined by "/" + key
    static std::string make_key(const Namespace& ns, const std::string& key);
    static std::string ns_to_string(const Namespace& ns);
    static bool starts_with(const std::string& str, const std::string& prefix);

    mutable std::mutex mutex_;
    std::map<std::string, StoreItem> items_;
};

} // namespace neograph::graph
