#pragma once

#include "transition_validator.h"

#include <neograph/program/transition_store.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace neograph::program::detail {

/**
 * The head a store instance last published for each run, kept in memory (ProgramTransitionStore::own_head_hint).
 * Bounded and least-recently-used; its own mutex, never the store's connection mutex, so that a reader does not wait
 * for a publication in flight. Capacity 0 keeps nothing.
 */
class OwnHeadHints final {
public:
    explicit OwnHeadHints(std::size_t capacity) noexcept : capacity_(capacity) {}

    std::optional<ProgramOwnHeadHint> get(std::string_view owner_scope, std::string_view run_id) const {
        std::lock_guard lock(mutex_);
        const auto      found = index_.find(Key{std::string(owner_scope), std::string(run_id)});
        if (found == index_.end()) return std::nullopt;
        return found->second->hint;
    }

    void erase(std::string_view owner_scope, std::string_view run_id) noexcept {
        std::lock_guard lock(mutex_);
        erase_locked(Key{std::string(owner_scope), std::string(run_id)});
    }

    /// Remembers the head a publication wrote; every other run of the same lineage forgets its own, since the
    /// lineage head moved under it.
    void record(std::string_view owner_scope, std::string_view run_id, ProgramOwnHeadHint hint) noexcept {
        if (capacity_ == 0) return;
        try {
            std::lock_guard lock(mutex_);
            if (hint.lineage) forget_lineage_locked(owner_scope, hint.lineage->lineage_id(), run_id);
            Key key{std::string(owner_scope), std::string(run_id)};
            erase_locked(key);
            if (entries_.size() >= capacity_) {
                index_.erase(Key{entries_.back().owner_scope, entries_.back().run_id});
                entries_.pop_back();
            }
            entries_.push_front(Entry{key.owner_scope, key.run_id, std::move(hint)});
            index_.emplace(std::move(key), entries_.begin());
        } catch (...) {
            erase(owner_scope, run_id);
        }
    }

    /// A lineage head moved without a publication of the run that holds a hint of it (a fork or a replay debits its
    /// source, a successor generation replaces the run): the runs of the lineage forget theirs.
    void forget_lineage(std::string_view owner_scope, std::string_view lineage_id,
                        std::string_view except_run_id = {}) noexcept {
        std::lock_guard lock(mutex_);
        forget_lineage_locked(owner_scope, lineage_id, except_run_id);
    }

private:
    struct Entry {
        std::string        owner_scope;
        std::string        run_id;
        ProgramOwnHeadHint hint;
    };
    struct Key {
        std::string owner_scope;
        std::string run_id;
        bool operator<(const Key& other) const noexcept {
            return owner_scope != other.owner_scope ? owner_scope < other.owner_scope : run_id < other.run_id;
        }
    };

    void erase_locked(const Key& key) noexcept {
        const auto found = index_.find(key);
        if (found == index_.end()) return;
        entries_.erase(found->second);
        index_.erase(found);
    }
    void forget_lineage_locked(std::string_view owner_scope, std::string_view lineage_id,
                               std::string_view except_run_id) noexcept {
        for (auto entry = entries_.begin(); entry != entries_.end();) {
            const bool same = entry->owner_scope == owner_scope && entry->hint.lineage &&
                              entry->hint.lineage->lineage_id() == lineage_id && entry->run_id != except_run_id;
            if (!same) {
                ++entry;
                continue;
            }
            index_.erase(Key{entry->owner_scope, entry->run_id});
            entry = entries_.erase(entry);
        }
    }

    mutable std::mutex                          mutex_;
    std::size_t                                 capacity_ = 0;
    std::list<Entry>                            entries_;
    std::map<Key, std::list<Entry>::iterator>   index_;
};

/**
 * Records what a publication wrote, after it published. The command at the head is the publication's newest command, or
 * the one the previous hint already knew when this publication builds on exactly that head, or none for a new run.
 * A run that ended keeps no hint. A fork or a replay debits another lineage's head, and a publication writes its own
 * lineage head: the hints that held the old heads are dropped.
 */
inline void record_published_head(OwnHeadHints& hints, std::string_view owner_scope, std::string_view run_id,
                                  std::string_view expected_journal_head,
                                  const std::optional<ProgramOwnHeadHint>& previous,
                                  const ProgramTransitionPublication& publication, bool run_existed) noexcept {
    if (publication.fork_source_lineage)
        hints.forget_lineage(owner_scope, publication.fork_source_lineage->lineage_id());
    if (publication.recorded_replay_source_lineage)
        hints.forget_lineage(owner_scope, publication.recorded_replay_source_lineage->lineage_id());
    // A record that carries a terminal result or state its bytes do not is what a later reader must take from the
    // store.
    if (transition::is_final(publication.run_record.continuation().state) ||
        publication.run_record.terminal_result() || publication.run_record.has_runtime_overlay()) {
        if (publication.run_lineage) hints.forget_lineage(owner_scope, publication.run_lineage->lineage_id(), run_id);
        return;
    }
    try {
        ProgramOwnHeadHint hint{publication.run_record, publication.journal_record, std::nullopt, false,
                                publication.run_lineage};
        if (!publication.commands.empty()) {
            hint.latest_command = *std::max_element(
                publication.commands.begin(), publication.commands.end(),
                [](const auto& left, const auto& right) { return left.sequence() < right.sequence(); });
            hint.latest_command_known = true;
        } else if (!run_existed) {
            hint.latest_command_known = true;
        } else if (previous && previous->latest_command_known && previous->journal_record.id == expected_journal_head) {
            hint.latest_command       = previous->latest_command;
            hint.latest_command_known = true;
        }
        hints.record(owner_scope, run_id, std::move(hint));
    } catch (...) {
        hints.erase(owner_scope, run_id);
    }
}

/**
 * Bounded least-recently-used cache of stored run generations. A stored generation never changes: its id is the digest of
 * its bytes and the row is insert-only. A reader still compares the generation it takes from here with the lineage head
 * that names it. Used under the store's connection mutex. Capacity 0 keeps nothing.
 */
class GenerationCache final {
public:
    explicit GenerationCache(std::size_t capacity = 256) noexcept : capacity_(capacity) {}

    std::optional<ProgramRunGeneration> get(std::string_view owner_scope, std::string_view lineage_id,
                                            std::uint64_t ordinal) {
        const auto found = index_.find(Key{std::string(owner_scope), std::string(lineage_id), ordinal});
        if (found == index_.end()) return std::nullopt;
        entries_.splice(entries_.begin(), entries_, found->second);
        return found->second->generation;
    }

    void put(std::string_view owner_scope, std::string_view lineage_id, std::uint64_t ordinal,
             const ProgramRunGeneration& generation) noexcept {
        if (capacity_ == 0) return;
        try {
            Key key{std::string(owner_scope), std::string(lineage_id), ordinal};
            if (const auto found = index_.find(key); found != index_.end()) {
                entries_.splice(entries_.begin(), entries_, found->second);
                return;
            }
            if (entries_.size() >= capacity_) {
                const auto& last = entries_.back();
                index_.erase(Key{last.owner_scope, last.lineage_id, last.ordinal});
                entries_.pop_back();
            }
            entries_.push_front(Entry{key.owner_scope, key.lineage_id, ordinal, generation});
            index_.emplace(std::move(key), entries_.begin());
        } catch (...) {
            entries_.clear();
            index_.clear();
        }
    }

private:
    struct Entry {
        std::string          owner_scope;
        std::string          lineage_id;
        std::uint64_t        ordinal = 0;
        ProgramRunGeneration generation;
    };
    struct Key {
        std::string   owner_scope;
        std::string   lineage_id;
        std::uint64_t ordinal = 0;
        bool operator<(const Key& other) const noexcept {
            if (owner_scope != other.owner_scope) return owner_scope < other.owner_scope;
            if (lineage_id != other.lineage_id) return lineage_id < other.lineage_id;
            return ordinal < other.ordinal;
        }
    };

    std::size_t                               capacity_ = 0;
    std::list<Entry>                          entries_;
    std::map<Key, std::list<Entry>::iterator> index_;
};

}  // namespace neograph::program::detail
