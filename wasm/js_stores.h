// The Program stores of a JavaScript host, as the five awaitable store interfaces.
//
// A host that runs NeoGraph in WebAssembly supplies its stores as a JavaScript object,
//   { transitions, programs, grants, fragments, modules }
// each member optional. adapt_js_stores() turns that object into the C++ interfaces the Program runtime and the
// catalog use. Each store call crosses to JavaScript as a method call: the arguments are copied into the call, a
// result that is not a Promise completes the call at once without suspending, and a Promise completes it from a later
// turn of the host's event loop (the module's exported ng_store_resolve / ng_store_reject, called by the bridge in
// wasm/stores.mjs). wasm/README.md has the contract.
//
// The stores keep their semantics in C++: the adapters move values and decisions and re-implement nothing.
#pragma once

#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/module.h>
#include <neograph/program/store.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/transition_store.h>

#include <asio/any_io_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace neograph::wasm {

/// The stores of a host's object. A member is null when the object has no store of that kind.
struct JsStoreSet {
    std::shared_ptr<program::ProgramTransitionStore>    transitions;
    std::shared_ptr<program::ProgramStore>              programs;
    std::shared_ptr<program::ProgramCoreToolGrantStore> grants;
    std::shared_ptr<program::TaskGraphFragmentStore>    fragments;
    std::shared_ptr<program::ModuleStore>               modules;
};

/// Adapts the stores object that `registerStores` (wasm/stores.mjs) returned `handle` for. Each call makes new adapters
/// over the same JavaScript object, so two sets are two writers on the same data. The handle must stay registered for
/// as long as an adapter is used; a call after `releaseStores` fails like any other failed call.
///
/// Throws std::invalid_argument when the handle names no stores object, when a member is not an object, when a
/// method is present but not a function, and when a method that the C++ interface requires is missing (the message
/// names it). A method that is absent and optional in the interface keeps the interface's default.
///
/// `lease`, when given, is held by every adapter of the set and released with the last of them. A caller that registered
/// the object for this set alone (the module's Programs object does) ends the registration from the lease's deleter, so
/// that it lasts as long as a run, or a task that a run left behind, can still make a call.
JsStoreSet adapt_js_stores(int handle, std::shared_ptr<const void> lease = {});

/// The executor that a pending call is completed on. A completion posts to it, which schedules a turn when it is the
/// executor of an async::HostLoop. Unset (the default), a completion is posted to the executor that the awaiting
/// coroutine runs on, which is the loop's executor for a coroutine the loop runs. Set it to an executor that outlives
/// every pending call and reset it (`set_js_store_executor({})`) before the executor goes away.
void set_js_store_executor(asio::any_io_executor executor);

/// What the bridge has done since the module started, for tests.
struct JsStoreStats {
    std::uint64_t calls     = 0;  ///< calls made to JavaScript
    std::uint64_t immediate = 0;  ///< calls that completed at once, with a value or a failure
    std::uint64_t pending   = 0;  ///< calls that returned a Promise
    std::uint64_t completed = 0;  ///< pending calls that were resumed with their outcome
    std::uint64_t dropped   = 0;  ///< outcomes that arrived after the awaiting coroutine was gone, and were dropped
    std::size_t   waiting   = 0;  ///< pending calls with a coroutine waiting now
};
JsStoreStats js_store_stats();

}  // namespace neograph::wasm
