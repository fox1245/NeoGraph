// jarvis/src/memory/json_file_store.h
//
// File-backed persistent Store — wraps InMemoryStore, flushes to a JSON file on
// every mutation (put/delete), and restores from disk on startup.
//
// Rationale: JARVIS stores conversation memory (turns/prefs) in a long-term
// Store, and InMemoryStore is lost when the process exits. Without persistence,
// restarting the process loses prior-turn context; at cookbook scale, a full
// JSON file dump is sufficient without introducing SQLite.
//
// Writes use a temporary file + atomic rename so an interrupted write never corrupts the file.

#pragma once

#include <neograph/graph/store.h>

#include <mutex>
#include <string>

namespace jarvis::memory {

class JsonFileStore : public neograph::graph::Store {
  public:
    /// @param path Persistent file path. Restored on startup if present, or created on first write.
    explicit JsonFileStore(std::string path);

    void put(const neograph::graph::Namespace& ns, const std::string& key,
             const neograph::json& value) override;
    std::optional<neograph::graph::StoreItem>
    get(const neograph::graph::Namespace& ns, const std::string& key) const override;
    std::vector<neograph::graph::StoreItem>
    search(const neograph::graph::Namespace& ns_prefix, int limit = 100) const override;
    void delete_item(const neograph::graph::Namespace& ns,
                     const std::string& key) override;
    std::vector<neograph::graph::Namespace>
    list_namespaces(const neograph::graph::Namespace& prefix = {}) const override;

  private:
    void load_from_file();
    void flush_to_file() const;

    std::string                     path_;
    neograph::graph::InMemoryStore  inner_;
    mutable std::mutex              io_mutex_;  // serializes file I/O
};

}  // namespace jarvis::memory
