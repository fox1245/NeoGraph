// Test helper: the child records a parent's relation rows hold, for tests that used to read them from the run record.
#pragma once

#include <neograph/program/transition_store.h>

#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace neograph::program::test {

using RelationRows = std::vector<ProgramChildRelation>;

// A blocking store (or a test wrapper over one) answers the read itself.
template <class Store>
auto relation_rows(const Store& store, std::string_view owner, std::string_view parent, int)
    -> std::enable_if_t<std::is_same_v<decltype(store.load_child_relations(owner, parent, false)), RelationRows>,
                        RelationRows> {
    return store.load_child_relations(owner, parent, false);
}

// An awaitable store is read through a blocking view.
inline RelationRows relation_rows(const ProgramTransitionStore& store, std::string_view owner,
                                  std::string_view parent, long) {
    BlockingProgramTransitionStore blocking(const_cast<ProgramTransitionStore&>(store));
    return blocking.load_child_relations(owner, parent, false);
}

// A pointer to either.
template <class Pointer>
auto relation_rows(const Pointer& pointer, std::string_view owner, std::string_view parent, long)
    -> decltype(relation_rows(*pointer, owner, parent, 0)) {
    return relation_rows(*pointer, owner, parent, 0);
}

/** The child records of `run`'s logical run id, in attach order; the rows are read from `store`. */
template <class Store>
std::vector<ProgramChildRecord> children_of(const Store& store, std::string_view owner, const ProgramRunRecord& run) {
    std::vector<ProgramChildRecord> children;
    for (const auto& row : relation_rows(store, owner, run.logical_run_id(), 0)) children.push_back(row.child());
    return children;
}

}  // namespace neograph::program::test
