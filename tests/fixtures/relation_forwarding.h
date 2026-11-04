// The child relation operations of a ProgramTransitionStore, forwarded to another store: for the test stores that wrap
// one and change only what they test. The relation operations are pure virtual, so every store has to answer them.
#pragma once

#include <neograph/program/transition_store.h>

// A SyncProgramTransitionStore whose inner object exposes the `..._sync` operations (a SyncProgramTransitionStore).
// Everything but the row update, for a store that decides the update itself.
#define NG_FORWARD_RELATION_SYNC_EXCEPT_UPDATE(INNER)                                                                               \
    std::optional<::neograph::program::ProgramChildRelationHead> load_child_relation_head_sync(                       \
        std::string_view o, std::string_view p) const override {                                                      \
        return (INNER).load_child_relation_head_sync(o, p);                                                           \
    }                                                                                                                 \
    std::optional<::neograph::program::ProgramChildRelation> load_child_relation_sync(                                \
        std::string_view o, std::string_view p, std::string_view c) const override {                                  \
        return (INNER).load_child_relation_sync(o, p, c);                                                             \
    }                                                                                                                 \
    std::vector<::neograph::program::ProgramChildRelation> load_child_relations_sync(                                 \
        std::string_view o, std::string_view p, bool open_only) const override {                                      \
        return (INNER).load_child_relations_sync(o, p, open_only);                                                    \
    }                                                                                                                 \
    ::neograph::program::ProgramChildRelationOutcome insert_child_relation_sync(                                      \
        std::string_view o, std::string_view p, std::uint64_t fence,                                                  \
        ::neograph::program::ProgramChildRecord child) override {                                                     \
        return (INNER).insert_child_relation_sync(o, p, fence, std::move(child));                                     \
    }

#define NG_FORWARD_RELATION_SYNC_UPDATE(INNER)                                                                        \
    ::neograph::program::ProgramChildRelationOutcome update_child_relation_sync(                                      \
        std::string_view o, std::string_view p, std::uint64_t fence, std::optional<std::uint64_t> version,           \
        ::neograph::program::ProgramChildRecord next) override {                                                      \
        return (INNER).update_child_relation_sync(o, p, fence, version, std::move(next));                             \
    }

#define NG_FORWARD_RELATION_SYNC(INNER) NG_FORWARD_RELATION_SYNC_EXCEPT_UPDATE(INNER) NG_FORWARD_RELATION_SYNC_UPDATE(INNER)

// A SyncProgramTransitionStore whose inner object is a BlockingProgramTransitionStore (no `_sync` suffix).
// Only the relation reads, for a store that decides the writes itself.
#define NG_FORWARD_RELATION_BLOCKING_READS_EXCEPT_LIST(INNER)                                                                           \
    std::optional<::neograph::program::ProgramChildRelationHead> load_child_relation_head_sync(                       \
        std::string_view o, std::string_view p) const override {                                                      \
        return (INNER).load_child_relation_head(o, p);                                                                \
    }                                                                                                                 \
    std::optional<::neograph::program::ProgramChildRelation> load_child_relation_sync(                                \
        std::string_view o, std::string_view p, std::string_view c) const override {                                  \
        return (INNER).load_child_relation(o, p, c);                                                                  \
    }

#define NG_FORWARD_RELATION_BLOCKING_LIST(INNER)                                                                      \
    std::vector<::neograph::program::ProgramChildRelation> load_child_relations_sync(                                 \
        std::string_view o, std::string_view p, bool open_only) const override {                                      \
        return (INNER).load_child_relations(o, p, open_only);                                                         \
    }

#define NG_FORWARD_RELATION_BLOCKING_READS(INNER) NG_FORWARD_RELATION_BLOCKING_READS_EXCEPT_LIST(INNER) NG_FORWARD_RELATION_BLOCKING_LIST(INNER)

#define NG_FORWARD_RELATION_BLOCKING_WRITES(INNER)                                                                     \
    ::neograph::program::ProgramChildRelationOutcome insert_child_relation_sync(                                      \
        std::string_view o, std::string_view p, std::uint64_t fence,                                                  \
        ::neograph::program::ProgramChildRecord child) override {                                                     \
        return (INNER).insert_child_relation(o, p, fence, std::move(child));                                          \
    }                                                                                                                 \
    ::neograph::program::ProgramChildRelationOutcome update_child_relation_sync(                                      \
        std::string_view o, std::string_view p, std::uint64_t fence, std::optional<std::uint64_t> version,           \
        ::neograph::program::ProgramChildRecord next) override {                                                      \
        return (INNER).update_child_relation(o, p, fence, version, std::move(next));                                  \
    }

#define NG_FORWARD_RELATION_BLOCKING(INNER) NG_FORWARD_RELATION_BLOCKING_READS(INNER) NG_FORWARD_RELATION_BLOCKING_WRITES(INNER)

// A ProgramTransitionStore (awaitable) whose inner object is a ProgramTransitionStore; BEFORE is a statement run
// before each forwarded call (for example `co_await pause();`, or nothing).
#define NG_FORWARD_RELATION_ASYNC(INNER, BEFORE)                                                                      \
    asio::awaitable<std::optional<::neograph::program::ProgramChildRelationHead>> load_child_relation_head(           \
        std::string_view o, std::string_view p) const override {                                                      \
        BEFORE                                                                                                        \
        co_return co_await (INNER).load_child_relation_head(o, p);                                                    \
    }                                                                                                                 \
    asio::awaitable<std::optional<::neograph::program::ProgramChildRelation>> load_child_relation(                    \
        std::string_view o, std::string_view p, std::string_view c) const override {                                  \
        BEFORE                                                                                                        \
        co_return co_await (INNER).load_child_relation(o, p, c);                                                      \
    }                                                                                                                 \
    asio::awaitable<std::vector<::neograph::program::ProgramChildRelation>> load_child_relations(                     \
        std::string_view o, std::string_view p, bool open_only) const override {                                      \
        BEFORE                                                                                                        \
        co_return co_await (INNER).load_child_relations(o, p, open_only);                                             \
    }                                                                                                                 \
    asio::awaitable<::neograph::program::ProgramChildRelationOutcome> insert_child_relation(                          \
        std::string_view o, std::string_view p, std::uint64_t fence,                                                  \
        ::neograph::program::ProgramChildRecord child) override {                                                     \
        BEFORE                                                                                                        \
        co_return co_await (INNER).insert_child_relation(o, p, fence, std::move(child));                              \
    }                                                                                                                 \
    asio::awaitable<::neograph::program::ProgramChildRelationOutcome> update_child_relation(                          \
        std::string_view o, std::string_view p, std::uint64_t fence, std::optional<std::uint64_t> version,           \
        ::neograph::program::ProgramChildRecord next) override {                                                      \
        BEFORE                                                                                                        \
        co_return co_await (INNER).update_child_relation(o, p, fence, version, std::move(next));                      \
    }
