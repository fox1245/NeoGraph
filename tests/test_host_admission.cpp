#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/host_admission.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using namespace neograph;

namespace {

HostResourceProfile profile(HostResourceVector capacity,
                            HostResourceVector reserve = {}) {
    HostResourceProfileData data;
    data.profile_id = "test-host-v1";
    data.capacity = capacity;
    data.safety_reserve = reserve;
    data.evidence = {"test", HostResourceConfidence::Measured, 1, false};
    return HostResourceProfile::create(std::move(data));
}

HostAdmissionRequest request(std::uint64_t cpu_millis,
                             std::chrono::milliseconds timeout = 100ms,
                             std::uint8_t priority = 0) {
    HostAdmissionRequest result;
    result.owner_scope = "tenant-a";
    result.operation_id = "operation";
    result.resources.cpu_millis = cpu_millis;
    result.queue_timeout = timeout;
    result.priority = priority;
    return result;
}

} // namespace

TEST(HostResourceProfile, RejectsReserveBeyondConfiguredCapacity) {
    HostResourceVector capacity;
    capacity.memory_bytes = 16;
    HostResourceVector reserve;
    reserve.memory_bytes = 17;

    EXPECT_THROW((void)profile(capacity, reserve), std::invalid_argument);
}

TEST(HostResourceProfile, IntersectsEverySourceAtTheTightestBound) {
    HostResourceVector first;
    first.cpu_millis = 4000;
    first.memory_bytes = 8192;
    first.tool_slots = 4;
    HostResourceVector second;
    second.cpu_millis = 2000;
    second.memory_bytes = 16384;
    second.tool_slots = 0; // Explicitly unavailable must not become unlimited.

    const auto effective = HostResourceProfile::intersect(
        {profile(first), profile(second)}, "effective-host-v1");

    EXPECT_EQ(effective.capacity().cpu_millis, 2000U);
    EXPECT_EQ(effective.capacity().memory_bytes, 8192U);
    EXPECT_EQ(effective.capacity().tool_slots, 0U);
}

TEST(HostAdmissionController, HoldsAllRequestedComponentsUntilLeaseRelease) {
    HostResourceVector capacity;
    capacity.cpu_millis = 2;
    capacity.memory_bytes = 64;
    HostAdmissionController controller({profile(capacity), 8, 5ms});

    auto first = neograph::async::run_sync(controller.reserve_async(request(2)));
    EXPECT_TRUE(first.held());
    EXPECT_EQ(controller.snapshot().reserved.cpu_millis, 2U);

    EXPECT_THROW(
        (void)neograph::async::run_sync(controller.reserve_async(request(1, 20ms))),
        HostAdmissionError);

    first.release();
    auto second = neograph::async::run_sync(controller.reserve_async(request(1)));
    EXPECT_TRUE(second.held());
    EXPECT_EQ(controller.snapshot().available.cpu_millis, 1U);
}

TEST(HostAdmissionController, CancellationRemovesQueuedRequest) {
    HostResourceVector capacity;
    capacity.cpu_millis = 1;
    HostAdmissionController controller({profile(capacity), 8, 5ms});
    auto held = neograph::async::run_sync(controller.reserve_async(request(1)));
    auto cancel = std::make_shared<graph::CancelToken>();
    std::promise<std::exception_ptr> completion;
    auto done = completion.get_future();

    std::thread waiter([&] {
        try {
            (void)neograph::async::run_sync(controller.reserve_async(request(1, 2s), cancel));
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

    held.release();
    auto recovered = neograph::async::run_sync(controller.reserve_async(request(1)));
    EXPECT_TRUE(recovered.held());
}

TEST(HostAdmissionController, BlockedWorkRaisesHolderPriorityWithAging) {
    HostResourceVector capacity;
    capacity.cpu_millis = 1;
    HostAdmissionController controller({profile(capacity), 8, 2ms});
    auto held = neograph::async::run_sync(controller.reserve_async(request(1, 100ms, 3)));
    std::promise<std::exception_ptr> completion;
    auto done = completion.get_future();

    std::thread waiter([&] {
        try {
            (void)neograph::async::run_sync(controller.reserve_async(request(1, 2s, 1)));
            completion.set_value(nullptr);
        } catch (...) {
            completion.set_value(std::current_exception());
        }
    });
    std::this_thread::sleep_for(20ms);

    EXPECT_GE(held.priority_hint(), 5U)
        << "the resource holder did not inherit the blocked request's aged priority";

    held.release();
    const auto status = done.wait_for(250ms);
    if (status != std::future_status::ready) {
        waiter.join();
        FAIL() << "queued host reservation did not complete after capacity released";
    }
    const auto error = done.get();
    waiter.join();
    EXPECT_EQ(error, nullptr);
}

TEST(HostAdmissionController, CapacityShrinkBlocksNewWorkWithoutRevokingHeldLease) {
    HostResourceVector capacity;
    capacity.cpu_millis = 2;
    HostAdmissionController controller({profile(capacity), 8, 5ms});
    auto held = neograph::async::run_sync(controller.reserve_async(request(2)));

    HostResourceVector smaller;
    smaller.cpu_millis = 1;
    controller.update_profile(profile(smaller));
    EXPECT_TRUE(controller.snapshot().overcommitted);
    EXPECT_TRUE(held.held());
    EXPECT_THROW((void)neograph::async::run_sync(controller.reserve_async(request(1, 20ms))),
                 HostAdmissionError);
}
