// The in-memory Program stores, reachable from JavaScript: the reference store of the JavaScript store contract
// (wasm/README.md). A set holds one store of each kind, and an operation of one of them is executed from its method
// name and a JSON array of arguments, with the codecs the adapters use (js_store_codec.h). The semantics are the
// in-memory stores' own: nothing here decides what a store does.
#pragma once

#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/module.h>
#include <neograph/program/store.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/transition_store.h>

#include <memory>
#include <string>
#include <string_view>

namespace neograph::wasm {

struct MemoryStoreSet {
    std::shared_ptr<program::InMemoryProgramTransitionStore> transitions;
    std::shared_ptr<program::InMemoryProgramStore>           programs;
    std::shared_ptr<program::SyncProgramCoreToolGrantStore>  grants;
    std::shared_ptr<program::InMemoryTaskGraphFragmentStore> fragments;
    std::shared_ptr<program::InMemoryModuleStore>            modules;
};

/// A new set of empty stores; the returned id names it.
int create_memory_stores();
/// Drops a set. Does nothing for an id that names none.
void release_memory_stores(int set);
/// The set, or null.
MemoryStoreSet* find_memory_stores(int set);

/// The JavaScript names of the methods of one member, as a JSON array text.
std::string memory_store_methods(std::string_view member);

/// Runs `member.method(arguments)` on the set and returns the result as JSON text. `arguments` is a JSON array text.
/// Throws what the store throws, std::invalid_argument for an unknown member or method or a malformed argument list.
std::string call_memory_store(MemoryStoreSet& set, std::string_view member, std::string_view method,
                              std::string_view arguments);

}  // namespace neograph::wasm
