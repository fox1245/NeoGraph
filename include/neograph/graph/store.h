/**
 * @file graph/store.h
 * @brief Cross-thread shared memory store (LangGraph Store equivalent).
 *
 * Provides namespaced key-value storage that persists across threads.
 * Use cases: long-term user preferences, shared knowledge, agent memory.
 */
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

/**
 * @brief Hierarchical namespace path for store items.
 *
 * A namespace is a vector of strings forming a hierarchical path,
 * e.g., {"users", "user123", "preferences"}.
 */
using Namespace = std::vector<std::string>;

/**
 * @brief A single item stored in the cross-thread Store.
 */
struct StoreItem {
    Namespace   ns;          ///< Hierarchical namespace path.
    std::string key;         ///< Item key within the namespace.
    json        value;       ///< Stored JSON value.
    int64_t     created_at;  ///< Creation timestamp (Unix epoch milliseconds).
    int64_t     updated_at;  ///< Last update timestamp (Unix epoch milliseconds).
};

/**
 * @brief Abstract interface for cross-thread shared memory.
 *
 * Provides namespaced key-value storage accessible from any graph thread.
 * Implement this to use databases or other persistence backends.
 *
 * @see InMemoryStore for a reference implementation.
 */
class Store {
public:
    virtual ~Store() = default;

    /**
     * @brief Store a value (create or update).
     * @param ns Namespace path for the item.
     * @param key Item key within the namespace.
     * @param value JSON value to store.
     */
    virtual void put(const Namespace& ns, const std::string& key, const json& value) = 0;

    /**
     * @brief Retrieve a single item.
     * @param ns Namespace path.
     * @param key Item key.
     * @return The item, or std::nullopt if not found.
     */
    virtual std::optional<StoreItem> get(const Namespace& ns, const std::string& key) const = 0;

    /**
     * @brief Search items under a namespace prefix.
     * @param ns_prefix Namespace prefix to match (e.g., {"users"} matches all user items).
     * @param limit Maximum number of items to return (default: 100).
     * @return Vector of matching StoreItem objects.
     */
    virtual std::vector<StoreItem> search(const Namespace& ns_prefix, int limit = 100) const = 0;

    /**
     * @brief Delete a single item.
     * @param ns Namespace path.
     * @param key Item key to delete.
     */
    virtual void delete_item(const Namespace& ns, const std::string& key) = 0;

    /**
     * @brief List all namespaces under a prefix.
     * @param prefix Namespace prefix to filter by (empty = list all).
     * @return Vector of unique Namespace paths.
     */
    virtual std::vector<Namespace> list_namespaces(const Namespace& prefix = {}) const = 0;
};

/**
 * @brief In-memory store implementation for testing and single-process use.
 *
 * Thread-safe via mutex. Items are stored in a std::map keyed by
 * a composite string of namespace + key.
 */
class InMemoryStore : public Store {
public:
    void put(const Namespace& ns, const std::string& key, const json& value) override;
    std::optional<StoreItem> get(const Namespace& ns, const std::string& key) const override;
    std::vector<StoreItem> search(const Namespace& ns_prefix, int limit = 100) const override;
    void delete_item(const Namespace& ns, const std::string& key) override;
    std::vector<Namespace> list_namespaces(const Namespace& prefix = {}) const override;

    /**
     * @brief Get the total number of stored items.
     * @return Total item count.
     */
    size_t size() const;

private:
    static std::string make_key(const Namespace& ns, const std::string& key);
    static std::string ns_to_string(const Namespace& ns);
    static bool starts_with(const std::string& str, const std::string& prefix);

    mutable std::mutex mutex_;
    std::map<std::string, StoreItem> items_;
};

} // namespace neograph::graph
