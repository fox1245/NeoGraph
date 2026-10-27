#pragma once

#include <neograph/program/run_record.h>

#include <cstddef>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace neograph::program::detail {

/**
 * Bounded least-recently-used cache of parsed ProgramRunRecord values keyed by
 * (owner_scope, run_id) and validated against the exact stored bytes read from
 * the database row.
 */
class RunRecordCache final {
public:
    explicit RunRecordCache(std::size_t capacity = 256) noexcept : capacity_(capacity) {}

    std::size_t capacity() const noexcept { return capacity_; }

    ProgramRunRecord parse(std::string_view owner_scope,
                           std::string_view run_id,
                           std::string_view stored_bytes) {
        if (capacity_ == 0) {
            auto parsed = ProgramRunRecord::parse(stored_bytes);
            if (parsed.owner_scope() != owner_scope || parsed.run_id() != run_id) {
                throw std::invalid_argument("Stored Program run record key binding is corrupt");
            }
            return parsed;
        }
        if (const auto found = index_.find(KeyView{owner_scope, run_id});
            found != index_.end()) {
            if (found->second->stored_bytes == stored_bytes) {
                entries_.splice(entries_.begin(), entries_, found->second);
                return found->second->record;
            }
            entries_.erase(found->second);
            index_.erase(found);
        }
        auto parsed = ProgramRunRecord::parse(stored_bytes);
        if (parsed.owner_scope() != owner_scope || parsed.run_id() != run_id) {
            throw std::invalid_argument("Stored Program run record key binding is corrupt");
        }
        put(owner_scope, run_id, stored_bytes, parsed);
        return parsed;
    }

    void put(std::string_view        owner_scope,
             std::string_view        run_id,
             std::string_view        stored_bytes,
             const ProgramRunRecord& record) noexcept {
        if (capacity_ == 0) return;
        if (record.owner_scope() != owner_scope || record.run_id() != run_id) {
            erase(owner_scope, run_id);
            return;
        }

        try {
            std::string bytes_copy(stored_bytes);
            if (const auto found = index_.find(KeyView{owner_scope, run_id});
                found != index_.end()) {
                found->second->stored_bytes = std::move(bytes_copy);
                found->second->record       = record;
                entries_.splice(entries_.begin(), entries_, found->second);
                return;
            }

            Entry entry{std::string(owner_scope), std::string(run_id), std::move(bytes_copy),
                        record};
            Key   key{entry.owner_scope, entry.run_id};

            if (entries_.size() >= capacity_) {
                const auto lru =
                    index_.find(KeyView{entries_.back().owner_scope, entries_.back().run_id});
                if (lru != index_.end()) index_.erase(lru);
                entries_.pop_back();
            }

            entries_.push_front(std::move(entry));
            try {
                index_.emplace(std::move(key), entries_.begin());
            } catch (...) {
                entries_.pop_front();
                throw;
            }
        } catch (...) {
            erase(owner_scope, run_id);
        }
    }

    void erase(std::string_view owner_scope, std::string_view run_id) noexcept {
        const auto found = index_.find(KeyView{owner_scope, run_id});
        if (found == index_.end()) return;
        entries_.erase(found->second);
        index_.erase(found);
    }

private:
    struct Entry {
        std::string      owner_scope;
        std::string      run_id;
        std::string      stored_bytes;
        ProgramRunRecord record;
    };

    struct Key {
        std::string owner_scope;
        std::string run_id;
    };

    struct KeyView {
        std::string_view owner_scope;
        std::string_view run_id;
    };

    struct KeyLess {
        using is_transparent = void;

        static KeyView view(const Key& key) noexcept {
            return {key.owner_scope, key.run_id};
        }
        static KeyView view(KeyView key) noexcept { return key; }

        template <typename Left, typename Right>
        bool operator()(const Left& left, const Right& right) const noexcept {
            const auto l = view(left);
            const auto r = view(right);
            if (l.owner_scope != r.owner_scope) return l.owner_scope < r.owner_scope;
            return l.run_id < r.run_id;
        }
    };

    std::size_t                                               capacity_ = 0;
    std::list<Entry>                                          entries_;
    std::map<Key, typename std::list<Entry>::iterator, KeyLess> index_;
};

}  // namespace neograph::program::detail
