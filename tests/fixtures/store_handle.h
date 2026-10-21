#pragma once

// A Program store as a test holds it (#556). The runtime takes the awaitable store; a test usually also reads and
// writes it, from a thread that can wait. StoreHandle keeps both: it converts to the shared awaitable store, and its
// `->` is the blocking view, so `fixture.journal->load(owner, run)` is the synchronous call.

#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/module.h>
#include <neograph/program/store.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/transition_store.h>

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace neograph::program::test_support {

template <class Interface, class Blocking>
class StoreHandle {
public:
    StoreHandle() = default;
    StoreHandle(std::nullptr_t) {}
    template <class Store,
              class = std::enable_if_t<std::is_convertible_v<Store*, Interface*>>>
    StoreHandle(std::shared_ptr<Store> store)
        : store_(std::move(store)), view_(store_ ? std::make_shared<Blocking>(store_) : nullptr) {}

    Blocking* operator->() const { return view_.get(); }
    Blocking& operator*() const { return *view_; }
    explicit operator bool() const noexcept { return static_cast<bool>(store_); }
    template <class Other,
              class = std::enable_if_t<std::is_convertible_v<std::shared_ptr<Interface>, std::shared_ptr<Other>>>>
    operator std::shared_ptr<Other>() const {
        return store_;
    }

    Interface* get() const noexcept { return store_.get(); }
    /// The store as its concrete class, or null when it is another class.
    template <class Concrete>
    std::shared_ptr<Concrete> as() const {
        return std::dynamic_pointer_cast<Concrete>(store_);
    }
    const std::shared_ptr<Interface>& shared() const noexcept { return store_; }
    void reset() {
        store_.reset();
        view_.reset();
    }

private:
    std::shared_ptr<Interface> store_;
    std::shared_ptr<Blocking>  view_;
};

namespace detail {
template <class Concrete>
struct StoreOwner {
    template <class... Args>
    explicit StoreOwner(Args&&... args) : owned(std::make_shared<Concrete>(std::forward<Args>(args)...)) {}
    std::shared_ptr<Concrete> owned;
};
}  // namespace detail

// A store a test builds in place and then calls from its own thread. It is the blocking view of the store it owns,
// so `store.load(owner, run)` is the synchronous call; `store.store()` is the store itself, for what is not part of
// the interface, and `store.shared()` is the awaitable store a runtime takes.
template <class Concrete, class Blocking>
class OwnedStore : private detail::StoreOwner<Concrete>, public Blocking {
public:
    template <class... Args>
    explicit OwnedStore(Args&&... args)
        : detail::StoreOwner<Concrete>(std::forward<Args>(args)...), Blocking(this->owned) {}

    Concrete&                 store() noexcept { return *this->owned; }
    const Concrete&           store() const noexcept { return *this->owned; }
    std::shared_ptr<Concrete> shared() const noexcept { return this->owned; }
};

template <class Concrete> using OwnedTransitionStore = OwnedStore<Concrete, BlockingProgramTransitionStore>;
template <class Concrete> using OwnedProgramStore    = OwnedStore<Concrete, BlockingProgramStore>;
template <class Concrete> using OwnedFragmentStore   = OwnedStore<Concrete, BlockingTaskGraphFragmentStore>;
template <class Concrete> using OwnedModuleStore     = OwnedStore<Concrete, BlockingModuleStore>;
template <class Concrete> using OwnedGrantStore      = OwnedStore<Concrete, BlockingProgramCoreToolGrantStore>;

using TransitionStoreHandle = StoreHandle<ProgramTransitionStore, BlockingProgramTransitionStore>;
using ProgramStoreHandle    = StoreHandle<ProgramStore, BlockingProgramStore>;
using FragmentStoreHandle   = StoreHandle<TaskGraphFragmentStore, BlockingTaskGraphFragmentStore>;
using ModuleStoreHandle     = StoreHandle<ModuleStore, BlockingModuleStore>;
using GrantStoreHandle      = StoreHandle<ProgramCoreToolGrantStore, BlockingProgramCoreToolGrantStore>;

}  // namespace neograph::program::test_support
