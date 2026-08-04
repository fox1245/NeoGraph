#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/tool_dispatch.h>
#include <neograph/tool_execution.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace neograph;

namespace {

class BlockingProbeTool final : public Tool {
public:
    explicit BlockingProbeTool(std::chrono::milliseconds delay)
        : delay_(delay) {}

    ChatTool get_definition() const override {
        return {"probe", "bounded blocking probe",
                json{{"type", "object"}, {"properties", json::object()}}};
    }

    std::string execute(const json&) override {
        const int active = active_.fetch_add(1, std::memory_order_acq_rel) + 1;
        int observed = max_active_.load(std::memory_order_acquire);
        while (observed < active
               && !max_active_.compare_exchange_weak(observed, active,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
        }
        std::this_thread::sleep_for(delay_);
        active_.fetch_sub(1, std::memory_order_acq_rel);
        return "ok";
    }

    std::string get_name() const override { return "probe"; }
    [[nodiscard]] int max_active() const {
        return max_active_.load(std::memory_order_acquire);
    }

private:
    std::chrono::milliseconds delay_;
    std::atomic<int> active_{0};
    std::atomic<int> max_active_{0};
};

ResourceRequest exclusive_request(std::chrono::milliseconds timeout = 80ms) {
    ResourceRequest request;
    request.resource_key = "test:exclusive";
    request.owner_scope = "tenant-a";
    request.capacity = 1;
    request.max_pending = 8;
    request.queue_timeout = timeout;
    return request;
}

} // namespace

TEST(ToolExecutionPolicy, RejectsUnsafeOrAmbiguousResourceTemplates) {
    ToolExecutionPolicy policy;
    policy.resource_key_template = "workspace:{arg:../../escape}";
    EXPECT_THROW(policy.validate(), std::invalid_argument);

    policy.resource_key_template = "workspace:{arg:workspace_id}";
    EXPECT_NO_THROW(policy.validate());

    policy.concurrency = ToolConcurrency::KeyedExclusive;
    policy.capacity = 2;
    EXPECT_THROW(policy.validate(), std::invalid_argument);
}

TEST(ResourceArbiter, TimeoutRemovesWaiterAndPreservesCapacity) {
    auto arbiter = std::make_shared<ResourceArbiter>();
    auto first = neograph::async::run_sync(arbiter->acquire_async(exclusive_request()));
    ASSERT_TRUE(first.held());

    try {
        (void)neograph::async::run_sync(arbiter->acquire_async(exclusive_request(40ms)));
        FAIL() << "queued resource acquisition unexpectedly succeeded";
    } catch (const asio::system_error& error) {
        EXPECT_EQ(error.code(), asio::error::timed_out);
    }

    // The timed-out waiter must be gone: after the first lease releases, a new
    // request receives the only capacity rather than being stuck behind a ghost.
    first.release();
    auto recovered = neograph::async::run_sync(arbiter->acquire_async(exclusive_request()));
    EXPECT_TRUE(recovered.held());
}

TEST(ResourceArbiter, CancellationRemovesQueuedWaiter) {
    auto arbiter = std::make_shared<ResourceArbiter>();
    auto first = neograph::async::run_sync(arbiter->acquire_async(exclusive_request()));
    auto cancel = std::make_shared<graph::CancelToken>();
    std::promise<std::exception_ptr> completion;
    auto done = completion.get_future();

    std::thread waiter([&] {
        try {
            (void)neograph::async::run_sync(
                arbiter->acquire_async(exclusive_request(2s), cancel));
            completion.set_value(nullptr);
        } catch (...) {
            completion.set_value(std::current_exception());
        }
    });

    std::this_thread::sleep_for(15ms);
    cancel->cancel();
    ASSERT_EQ(done.wait_for(250ms), std::future_status::ready);
    const auto error = done.get();
    waiter.join();
    ASSERT_NE(error, nullptr);
    EXPECT_THROW(std::rethrow_exception(error), graph::CancelledException);

    first.release();
    auto recovered = neograph::async::run_sync(arbiter->acquire_async(exclusive_request()));
    EXPECT_TRUE(recovered.held());
}

TEST(ToolExecutionController, HostCanWidenOnlyDeclaredResourceCapacity) {
    constexpr auto kDelay = 150ms;
    BlockingProbeTool tool(kDelay);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.concurrency = ToolConcurrency::Capacity;
    policy.capacity = 2;
    policy.resource_key_template = "workspace:{arg:workspace_id}";
    policies->upsert("probe", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});

    std::vector<ToolCall> calls;
    for (int index = 0; index < 2; ++index) {
        ToolCall call;
        call.id = "call-" + std::to_string(index);
        call.name = "probe";
        call.arguments = R"({"workspace_id":"w-1"})";
        calls.push_back(std::move(call));
    }
    ToolExecutionContext execution;
    execution.controller = controller;
    const auto started = std::chrono::steady_clock::now();
    const auto results = neograph::async::run_sync(
        dispatch_tool_calls(std::move(calls), std::vector<Tool*>{&tool}, {}, {}, execution));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].content, "ok");
    EXPECT_EQ(results[1].content, "ok");
    EXPECT_EQ(tool.max_active(), 2)
        << "capacity was widened only by the host-declared policy";
    EXPECT_LT(elapsed, 2 * kDelay)
        << "declared capacity did not overlap blocking tool calls";
}
