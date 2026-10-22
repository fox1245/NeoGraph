// Test-only (not part of the module): a reference store registered with the store bridge, for the conformance suites.
//
// It owns a set of in-memory stores (wasm/memory_stores.h) and the handle that names the JavaScript object wrapping
// them. The JavaScript driver (wasm/test/store_conformance_test.mjs) provides Module.ngTestRegisterStores(set) and
// Module.ngTestReleaseStores(handle).
#pragma once

#include <neograph/program/transition_store.h>

namespace neograph::wasm::testing {

class ReferenceStores {
public:
    ReferenceStores();
    ~ReferenceStores();
    ReferenceStores(const ReferenceStores&)            = delete;
    ReferenceStores& operator=(const ReferenceStores&) = delete;

    /// The handle to give adapt_js_stores.
    int handle() const noexcept { return handle_; }
    /// The next publication to the transition store fails at `point` (the in-memory store's one-shot fault).
    void fail_next_publication(program::ProgramTransitionFaultPoint point);

private:
    int set_    = 0;
    int handle_ = 0;
};

}  // namespace neograph::wasm::testing
