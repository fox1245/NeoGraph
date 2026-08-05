#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/tool_dispatch.h>
#include <neograph/tool_execution.h>

#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
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
        calls_.fetch_add(1, std::memory_order_acq_rel);
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
    [[nodiscard]] int calls() const {
        return calls_.load(std::memory_order_acquire);
    }

private:
    std::chrono::milliseconds delay_;
    std::atomic<int> active_{0};
    std::atomic<int> calls_{0};
    std::atomic<int> max_active_{0};
};

class FlakyProbeTool final : public Tool {
public:
    ChatTool get_definition() const override {
        return {"flaky", "retry probe",
                json{{"type", "object"}, {"properties", json::object()}}};
    }

    std::string execute(const json&) override {
        const auto call = calls_.fetch_add(1, std::memory_order_acq_rel);
        if (call == 0) throw std::runtime_error("transient");
        return "recovered";
    }

    std::string get_name() const override { return "flaky"; }
    [[nodiscard]] int calls() const {
        return calls_.load(std::memory_order_acquire);
    }

private:
    std::atomic<int> calls_{0};
};

class SlowContextualTool final : public Tool, public ContextualAsyncTool {
public:
    ChatTool get_definition() const override {
        return {"slow", "deadline probe",
                json{{"type", "object"}, {"properties", json::object()}}};
    }

    std::string execute(const json&) override { return "sync"; }

    asio::awaitable<std::string> execute_async(
        const json&, ToolExecutionContext) override {
        ++calls_;
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor);
        timer.expires_after(100ms);
        co_await timer.async_wait(asio::use_awaitable);
        co_return "late";
    }

    std::string get_name() const override { return "slow"; }
    [[nodiscard]] int calls() const {
        return calls_.load(std::memory_order_acquire);
    }

private:
    std::atomic<int> calls_{0};
};

#if defined(__unix__) || defined(__APPLE__)
class ProcessProbeTool final : public Tool, public ProcessTool {
public:
    ChatTool get_definition() const override {
        return {"process-probe", "process bridge probe",
                json{{"type", "object"}, {"properties", json::object()}}};
    }

    std::string execute(const json&) override { return "sync-not-used"; }
    std::string get_name() const override { return "process-probe"; }

    ToolProcessSpec prepare_process(
        const json& arguments, const ToolExecutionContext&) const override {
        const auto mode = arguments.value("mode", "echo");
        if (mode == "echo") return ToolProcessSpec{{"/bin/echo", "process-ok"}};
        if (mode == "sleep") return ToolProcessSpec{{"/bin/sleep", "5"}};
        if (mode == "fail") {
            return ToolProcessSpec{{"/bin/sh", "-c", "printf partial; exit 7"}};
        }
        if (mode == "input") {
            ToolProcessSpec spec;
            spec.argv = {"/bin/cat"};
            spec.interactive = true;
            return spec;
        }
        if (mode == "flood") return ToolProcessSpec{{"/usr/bin/yes", "flood"}};
        throw std::invalid_argument("unknown process probe mode");
    }
};
#endif

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

TEST(ToolExecutionController, HostAdmissionBoundsReentrantTools) {
    constexpr auto kDelay = 100ms;
    BlockingProbeTool tool(kDelay);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.concurrency = ToolConcurrency::Reentrant;
    policy.host_resources.tool_slots = 1;
    policy.queue_timeout = 1s;
    policies->upsert("probe", policy);

    HostResourceVector capacity;
    capacity.tool_slots = 1;
    HostResourceProfileData profile_data;
    profile_data.profile_id = "tool-host-v1";
    profile_data.capacity = capacity;
    profile_data.evidence = {"test", HostResourceConfidence::Measured, 1, false};
    auto host_admission = std::make_shared<HostAdmissionController>(
        HostAdmissionControllerConfig{HostResourceProfile::create(std::move(profile_data)), 8, 2ms});
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>(), host_admission});

    std::vector<ToolCall> calls;
    for (int index = 0; index < 2; ++index) {
        calls.push_back({"host-call-" + std::to_string(index), "probe", "{}"});
    }
    ToolExecutionContext execution;
    execution.controller = controller;
    const auto results = neograph::async::run_sync(
        dispatch_tool_calls(std::move(calls), std::vector<Tool*>{&tool}, {}, {}, execution));

    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].content, "ok");
    EXPECT_EQ(results[1].content, "ok");
    EXPECT_EQ(tool.max_active(), 1)
        << "a host-installed admission controller must constrain reentrant tool calls";
}

TEST(ToolExecutionController, PropagatesPolicyPriorityToHostAdmission) {
    BlockingProbeTool tool(1ms);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.concurrency = ToolConcurrency::Reentrant;
    policy.host_resources.tool_slots = 1;
    policy.host_priority = 77;
    policy.queue_timeout = 1s;
    policies->upsert("probe", policy);

    HostResourceVector capacity;
    capacity.tool_slots = 1;
    HostResourceProfileData profile_data;
    profile_data.profile_id = "tool-priority-host-v1";
    profile_data.capacity = capacity;
    profile_data.evidence = {"test", HostResourceConfidence::Measured, 1, false};
    auto host_admission = std::make_shared<HostAdmissionController>(
        HostAdmissionControllerConfig{HostResourceProfile::create(std::move(profile_data)), 8, 2ms});
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>(), host_admission});

    HostAdmissionRequest holder_request;
    holder_request.owner_scope = "tenant-a";
    holder_request.operation_id = "holder";
    holder_request.resources.tool_slots = 1;
    holder_request.priority = 3;
    holder_request.queue_timeout = 1s;
    auto holder = neograph::async::run_sync(host_admission->reserve_async(std::move(holder_request)));

    std::promise<std::exception_ptr> completion;
    auto done = completion.get_future();
    std::thread waiter([&] {
        try {
            ToolExecutionContext context;
            const auto result = neograph::async::run_sync(
                controller->execute_async(tool, json::object(), std::move(context)));
            if (result != "ok") throw std::runtime_error("unexpected tool result");
            completion.set_value(nullptr);
        } catch (...) {
            completion.set_value(std::current_exception());
        }
    });

    std::this_thread::sleep_for(20ms);
    EXPECT_GE(holder.priority_hint(), 77U)
        << "the host controller did not receive the tool policy's priority";

    holder.release();
    const auto status = done.wait_for(1s);
    if (status != std::future_status::ready) {
        waiter.join();
        FAIL() << "host-admitted tool did not complete after the holder released";
    }
    const auto error = done.get();
    waiter.join();
    EXPECT_EQ(error, nullptr);
}

TEST(ToolExecutionController, WaitingForToolResourceDoesNotHoldHostSlots) {
    constexpr auto kDelay = 200ms;
    BlockingProbeTool tool(kDelay);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.concurrency = ToolConcurrency::KeyedExclusive;
    policy.host_resources.tool_slots = 1;
    policy.queue_timeout = 1s;
    policies->upsert("probe", policy);

    HostResourceVector capacity;
    capacity.tool_slots = 2;
    HostResourceProfileData profile_data;
    profile_data.profile_id = "tool-wait-host-v1";
    profile_data.capacity = capacity;
    profile_data.evidence = {"test", HostResourceConfidence::Measured, 1, false};
    auto host_admission = std::make_shared<HostAdmissionController>(
        HostAdmissionControllerConfig{HostResourceProfile::create(std::move(profile_data)), 8, 2ms});
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>(), host_admission});

    std::vector<ToolCall> calls{
        {"waiting-host-call-1", "probe", "{}"},
        {"waiting-host-call-2", "probe", "{}"},
    };
    ToolExecutionContext execution;
    execution.controller = controller;
    std::promise<std::exception_ptr> completion;
    auto done = completion.get_future();
    std::thread worker([&] {
        try {
            const auto results = neograph::async::run_sync(
                dispatch_tool_calls(std::move(calls), std::vector<Tool*>{&tool}, {}, {}, execution));
            if (results.size() != 2U || results[0].content != "ok" || results[1].content != "ok") {
                throw std::runtime_error("unexpected tool dispatch result");
            }
            completion.set_value(nullptr);
        } catch (...) {
            completion.set_value(std::current_exception());
        }
    });

    bool started = false;
    for (int index = 0; index < 100; ++index) {
        if (tool.max_active() == 1) {
            started = true;
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    if (!started) {
        worker.join();
        FAIL() << "the first keyed tool invocation did not start";
    }

    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(host_admission->snapshot().reserved.tool_slots, 1U)
        << "a request waiting for a tool-specific resource retained a host slot";

    const auto status = done.wait_for(2s);
    if (status != std::future_status::ready) {
        worker.join();
        FAIL() << "keyed tool calls did not complete";
    }
    const auto error = done.get();
    worker.join();
    EXPECT_EQ(error, nullptr);
}


TEST(ToolExecutionController, TypedResultDistinguishesSuccessAndOutputFailure) {
    BlockingProbeTool tool(1ms);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.output_limit_bytes = 2;
    policies->upsert("probe", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});

    ToolExecutionContext context;
    const auto success = neograph::async::run_sync(
        controller->execute_result_async(tool, json::object(), context));
    ASSERT_TRUE(success.succeeded());
    EXPECT_EQ(success.status, ToolTerminalStatus::Succeeded);
    EXPECT_EQ(success.output, "ok");
    EXPECT_TRUE(success.error.empty());

    policy.output_limit_bytes = 1;
    policies->upsert("probe", policy);
    const auto failure = neograph::async::run_sync(
        controller->execute_result_async(tool, json::object(), context));
    EXPECT_EQ(failure.status, ToolTerminalStatus::Failed);
    EXPECT_FALSE(failure.succeeded());
    EXPECT_FALSE(failure.error.empty());
}

TEST(ToolExecutionController, RejectsUnsafeSingleFlightPolicies) {
    ToolExecutionPolicy policy;
    policy.concurrency = ToolConcurrency::SingleFlight;
    policy.effect = ToolEffectClass::ExternalWrite;
    policy.idempotency = ToolIdempotency::Idempotent;
    EXPECT_THROW(policy.validate(), std::invalid_argument);

    policy.effect = ToolEffectClass::ReadOnly;
    policy.idempotency = ToolIdempotency::Idempotent;
    EXPECT_NO_THROW(policy.validate());
    EXPECT_EQ(tool_terminal_status_from_string(std::string(to_string(
                  ToolTerminalStatus::ReconciliationRequired))),
              ToolTerminalStatus::ReconciliationRequired);
}

#if defined(__unix__) || defined(__APPLE__)
TEST(ToolExecutionController, ProcessBridgePreservesTypedTerminalDetails) {
    ProcessProbeTool tool;
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingProcess;
    policy.effect = ToolEffectClass::ReadOnly;
    policy.idempotency = ToolIdempotency::Idempotent;
    policy.execution_timeout = 1s;
    policy.output_limit_bytes = 4096;
    policies->upsert("process-probe", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});

    const auto success = neograph::async::run_sync(
        controller->execute_result_async(tool, json{{"mode", "echo"}}, {}));
    EXPECT_EQ(success.output, "process-ok\n");
    ASSERT_TRUE(success.exit_code.has_value());
    EXPECT_EQ(*success.exit_code, 0);

    const auto failure = neograph::async::run_sync(
        controller->execute_result_async(tool, json{{"mode", "fail"}}, {}));
    EXPECT_EQ(failure.status, ToolTerminalStatus::Failed);
    EXPECT_EQ(failure.output, "partial");
    ASSERT_TRUE(failure.exit_code.has_value());
    EXPECT_EQ(*failure.exit_code, 7);

    const auto input = neograph::async::run_sync(
        controller->execute_result_async(tool, json{{"mode", "input"}}, {}));
    EXPECT_EQ(input.status, ToolTerminalStatus::InputRequired);

    policy.output_limit_bytes = 256;
    policies->upsert("process-probe", policy);
    const auto flood = neograph::async::run_sync(
        controller->execute_result_async(tool, json{{"mode", "flood"}}, {}));
    EXPECT_EQ(flood.status, ToolTerminalStatus::Killed);
    EXPECT_TRUE(flood.output_truncated);
}

TEST(ToolExecutionController, ProcessBridgeKillsTheProcessGroupOnCancellation) {
    ProcessProbeTool tool;
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingProcess;
    policy.effect = ToolEffectClass::WorkspaceWrite;
    policy.idempotency = ToolIdempotency::Unknown;
    policy.execution_timeout = 10s;
    policies->upsert("process-probe", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});
    auto cancel = std::make_shared<graph::CancelToken>();
    std::promise<ToolExecutionResult> completed;
    auto result = completed.get_future();
    std::thread worker([&] {
        completed.set_value(neograph::async::run_sync(
            controller->execute_result_async(tool, json{{"mode", "sleep"}},
                                              ToolExecutionContext{cancel})));
    });
    std::this_thread::sleep_for(40ms);
    cancel->cancel();
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    const auto terminal = result.get();
    worker.join();
    EXPECT_EQ(terminal.status, ToolTerminalStatus::CancellationRequested);
    EXPECT_TRUE(terminal.effect_uncertain);
}
#endif

TEST(ToolDispatch, EmitsTypedTerminalStatusInToolMessages) {
    BlockingProbeTool tool(1ms);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policies->upsert("probe", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});
    ToolExecutionContext context;
    context.controller = controller;
    const auto messages = neograph::async::run_sync(dispatch_tool_calls(
        {ToolCall{"call-typed", "probe", "{}"}}, {&tool}, {}, {}, context));
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages.front().tool_status, "succeeded");
    EXPECT_EQ(messages.front().content, "ok");

    json serialized;
    to_json(serialized, messages.front());
    EXPECT_EQ(serialized.at("tool_status"), "succeeded");
}

TEST(ToolExecutionController, RetriesOnlyAfterPolicyAllowsIdempotentCalls) {
    FlakyProbeTool tool;
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.effect = ToolEffectClass::ExternalRead;
    policy.idempotency = ToolIdempotency::Idempotent;
    policy.retry_max_attempts = 2;
    policy.retry_backoff = 1ms;
    policies->upsert("flaky", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});

    const auto result = neograph::async::run_sync(
        controller->execute_result_async(tool, json::object(), {}));
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.output, "recovered");
    EXPECT_EQ(tool.calls(), 2);
}

TEST(ToolExecutionController, NativeAsyncExecutionTimeoutIsTyped) {
    SlowContextualTool tool;
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.execution_timeout = 10ms;
    policy.queue_timeout = 1s;
    policies->upsert("slow", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});

    const auto started = std::chrono::steady_clock::now();
    const auto result = neograph::async::run_sync(
        controller->execute_result_async(tool, json::object(), {}));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(result.status, ToolTerminalStatus::TimedOut);
    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(tool.calls(), 1);
    EXPECT_LT(elapsed, 90ms);
}

TEST(ToolExecutionController, SingleFlightSharesOneInFlightResultPerKey) {
    BlockingProbeTool tool(40ms);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.concurrency = ToolConcurrency::SingleFlight;
    policy.effect = ToolEffectClass::ReadOnly;
    policy.idempotency = ToolIdempotency::Idempotent;
    policy.max_pending = 8;
    policy.queue_timeout = 1s;
    policies->upsert("probe", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});

    std::vector<ToolCall> calls{
        {"single-flight-1", "probe", "{}"},
        {"single-flight-2", "probe", "{}"},
    };
    ToolExecutionContext context;
    context.controller = controller;
    const auto results = neograph::async::run_sync(
        dispatch_tool_calls(std::move(calls), std::vector<Tool*>{&tool}, {}, {}, context));

    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].content, "ok");
    EXPECT_EQ(results[1].content, "ok");
    EXPECT_EQ(tool.calls(), 1)
        << "single-flight callers with the same derived key must share one invocation";
}

TEST(ToolExecutionController, CancellationBeforeAdmissionIsTyped) {
    BlockingProbeTool tool(1ms);
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policies->upsert("probe", policy);
    auto controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});
    auto cancel = std::make_shared<graph::CancelToken>();
    cancel->cancel();

    ToolExecutionContext context;
    context.cancel_token = std::move(cancel);
    const auto result = neograph::async::run_sync(
        controller->execute_result_async(tool, json::object(), std::move(context)));
    EXPECT_EQ(result.status, ToolTerminalStatus::CancelledBeforeStart);
    EXPECT_FALSE(result.retryable);
    EXPECT_FALSE(result.effect_uncertain);
}