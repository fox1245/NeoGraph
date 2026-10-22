// Test-only: see js_store_deployment.h.
#include "js_store_deployment.h"

#include "../js_stores.h"
#include "../memory_stores.h"

#include <neograph/program/store.h>

#include <emscripten.h>
#include <gtest/gtest.h>

#include <stdexcept>

namespace {

EM_JS(int, ng_test_register_stores, (int set), {
  if (typeof Module.ngTestRegisterStores !== 'function') return 0;
  return Module.ngTestRegisterStores(set);
});
EM_JS(void, ng_test_release_stores, (int handle), {
  if (typeof Module.ngTestReleaseStores === 'function') Module.ngTestReleaseStores(handle);
});

}  // namespace

namespace neograph::wasm::testing {

ReferenceStores::ReferenceStores() : set_(create_memory_stores()) {
    handle_ = ng_test_register_stores(set_);
    if (handle_ == 0) {
        release_memory_stores(set_);
        throw std::runtime_error("the test driver did not register the reference stores (Module.ngTestRegisterStores)");
    }
}

ReferenceStores::~ReferenceStores() {
    ng_test_release_stores(handle_);
    release_memory_stores(set_);
}

void ReferenceStores::fail_next_publication(program::ProgramTransitionFaultPoint point) {
    find_memory_stores(set_)->transitions->fail_next_publication_for_testing(point);
}

}  // namespace neograph::wasm::testing

// The conformance cases would pass just as well against a store that never reached JavaScript. These say that the
// JavaScript backend does: every call crosses the bridge, none of them waits, and the reference store has all five stores.
TEST(JavaScriptStoreBridge, EveryCallCrossesToJavaScriptAndCompletesAtOnce) {
    neograph::wasm::testing::ReferenceStores reference;
    const auto                               stores = neograph::wasm::adapt_js_stores(reference.handle());
    ASSERT_TRUE(stores.transitions && stores.programs && stores.grants && stores.fragments && stores.modules);

    const auto before = neograph::wasm::js_store_stats();
    neograph::program::BlockingProgramTransitionStore transitions(stores.transitions);
    neograph::program::BlockingProgramStore           programs(stores.programs);
    EXPECT_FALSE(transitions.load("owner", "run"));
    EXPECT_FALSE(transitions.latest("owner", "run"));
    EXPECT_TRUE(transitions.load_events("owner", "run").empty());
    EXPECT_FALSE(programs.get_version("owner", "version"));
    EXPECT_TRUE(programs.list_versions("owner").empty());
    const auto after = neograph::wasm::js_store_stats();
    EXPECT_EQ(after.calls - before.calls, 5U);
    EXPECT_EQ(after.immediate - before.immediate, 5U);
    EXPECT_EQ(after.pending, before.pending);
    EXPECT_EQ(after.waiting, 0U);
}

TEST(JavaScriptStoreBridge, TwoSetsOverOneReferenceStoreAreTwoWritersOnTheSameData) {
    neograph::wasm::testing::ReferenceStores reference;
    const auto                               first  = neograph::wasm::adapt_js_stores(reference.handle());
    const auto                               second = neograph::wasm::adapt_js_stores(reference.handle());
    EXPECT_NE(first.transitions.get(), second.transitions.get());
    EXPECT_EQ(first.transitions->process_coordination_key(), second.transitions->process_coordination_key());
    EXPECT_FALSE(first.transitions->process_coordination_key().empty());
}
