// A mediated Tool call that ends without starting performed no effect (the
// owner's decision on the normalization review). The run's deadline had
// passed (`expired`), admission refused it (`rejected`), or the run was
// cancelled first (`cancelled_before_start`). Every ToolEffectBroker records
// such a call the same way: it keeps its status, it is not effect-uncertain,
// it committed nothing, and its slot is free again, so the same call can run
// later. These tests drive the durable SQLiteToolEffectBroker and the
// in-memory broker a node attempt sees (the run's broker behind its
// AttemptEffectLog) with the same cases.
#include "../src/core/run_context_runtime.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/host_admission.h>
#include <neograph/sqlite_runtime_stores.h>
#include <neograph/tool_effect_broker.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace neograph;
using namespace std::chrono_literals;
using graph::detail::RunContextRuntime;
using graph::detail::ScopedInvocationRuntime;
using graph::detail::ScopedRunContextRuntime;

namespace {

class CountingTool final : public Tool {
public:
    ChatTool get_definition() const override { return {"write", "", json::object()}; }
    std::string get_name() const override { return "write"; }
    std::string execute(const json&) override {
        ++runs;
        return "written";
    }
    std::atomic<int> runs{0};
};

// The in-memory broker: dispatches through the controller, keeps nothing.
class PassThroughBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(ToolEffectIdentity, Tool& tool, json arguments,
                                                 ToolExecutionContext context) override {
        auto controller = context.controller ? context.controller
                                             : default_tool_execution_controller();
        co_return co_await controller->execute_result_async(tool, std::move(arguments),
                                                            std::move(context));
    }
};

std::string journal_path(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() /
                ("neograph_not_started_" + name + ".sqlite");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
    return path.string();
}

std::shared_ptr<ToolExecutionController> controller_with_policy(
    ToolExecutionPolicy policy, std::shared_ptr<HostAdmissionController> host = {}) {
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    policies->upsert("write", policy);
    return std::make_shared<ToolExecutionController>(ToolExecutionControllerConfig{
        policies, std::make_shared<ResourceArbiter>(), std::move(host)});
}

ToolExecutionPolicy write_policy() {
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.effect = ToolEffectClass::ExternalWrite;
    policy.idempotency = ToolIdempotency::NonIdempotent;
    return policy;
}

// A host whose capacity can never fit the call: admission rejects it.
std::shared_ptr<ToolExecutionController> rejecting_controller() {
    auto policy = write_policy();
    policy.host_resources.tool_slots = 2;
    HostResourceVector capacity;
    capacity.tool_slots = 1;
    HostResourceProfileData profile;
    profile.profile_id = "not-started-host-v1";
    profile.capacity = capacity;
    profile.evidence = {"test", HostResourceConfidence::Measured, 1, false};
    return controller_with_policy(
        policy, std::make_shared<HostAdmissionController>(HostAdmissionControllerConfig{
                    HostResourceProfile::create(std::move(profile)), 8, 2ms}));
}

struct NotStartedCase {
    std::string name;
    ToolTerminalStatus status;
    // Turns a context that would run the call into one that does not start it.
    std::function<void(ToolExecutionContext&)> arrange;
};

std::vector<NotStartedCase> not_started_cases() {
    return {
        {"expired", ToolTerminalStatus::Expired,
         [](ToolExecutionContext& context) {
             context.deadline = std::chrono::steady_clock::now() - 1ms;
         }},
        {"rejected", ToolTerminalStatus::Rejected,
         [](ToolExecutionContext& context) { context.controller = rejecting_controller(); }},
        {"cancelled_before_start", ToolTerminalStatus::CancelledBeforeStart,
         [](ToolExecutionContext& context) {
             context.cancel_token = std::make_shared<graph::CancelToken>();
             context.cancel_token->cancel();
         }},
    };
}

ToolEffectIdentity slot(const std::string& run) {
    ToolEffectIdentity identity;
    identity.owner_scope = "tenant";
    identity.run_id = run;
    identity.thread_id = "thread";
    identity.task_id = "s1:act";
    identity.call_ordinal = 0;
    identity.operation_id = "operation";
    identity.grant_id = "grant";
    identity.tool_call_id = "call-1";
    return identity;
}

ToolExecutionResult run_broker(ToolEffectBroker& broker, const ToolEffectIdentity& identity,
                               Tool& tool, ToolExecutionContext context) {
    return async::run_sync(broker.execute(identity, tool, json::object(), std::move(context)));
}

}  // namespace

TEST(ToolEffectNotStarted, SQLiteBrokerRecordsTheCallAsNotStartedAndReleasesTheSlot) {
    for (const auto& c : not_started_cases()) {
        SCOPED_TRACE(c.name);
        CountingTool tool;
        SQLiteToolEffectBroker broker(journal_path(c.name),
                                      {{&tool, "write-executable:v1"}});
        const auto identity = slot("run-" + c.name);
        ToolExecutionContext context;
        context.controller = controller_with_policy(write_policy());
        auto refused = context;
        c.arrange(refused);

        const auto first = run_broker(broker, identity, tool, refused);
        EXPECT_EQ(first.status, c.status) << first.error;
        EXPECT_FALSE(first.effect_uncertain);
        EXPECT_EQ(tool.runs.load(), 0);

        // The slot is free: the same call, now admitted, runs once and its
        // receipt replays without running it again.
        const auto second = run_broker(broker, identity, tool, context);
        EXPECT_EQ(second.status, ToolTerminalStatus::Succeeded) << second.error;
        EXPECT_EQ(second.output, "written");
        const auto replay = run_broker(broker, identity, tool, context);
        EXPECT_EQ(replay.status, ToolTerminalStatus::Succeeded) << replay.error;
        EXPECT_EQ(tool.runs.load(), 1);
    }
}

TEST(ToolEffectNotStarted, EveryBrokerRecordsANotStartedCallAsNoEffect) {
    for (const bool durable : {false, true}) {
        for (const auto& c : not_started_cases()) {
            SCOPED_TRACE(std::string(durable ? "sqlite/" : "in-memory/") + c.name);
            CountingTool tool;
            std::shared_ptr<ToolEffectBroker> inner;
            if (durable)
                inner = std::make_shared<SQLiteToolEffectBroker>(
                    journal_path("attempt_" + c.name),
                    std::vector<SQLiteToolExecutableBinding>{{&tool, "write-executable:v1"}});
            else
                inner = std::make_shared<PassThroughBroker>();
            auto runtime = std::make_shared<RunContextRuntime>();
            runtime->tool_effect_broker = inner;
            const graph::RunContext run_context;
            const graph::RunContext node_context;
            ScopedRunContextRuntime run_scope(run_context, runtime);
            ScopedInvocationRuntime attempt(run_context, node_context, "s1:act", true);
            ASSERT_TRUE(attempt.effects());
            auto execution = graph::make_tool_execution_context(node_context);
            ASSERT_TRUE(execution.effect_broker);
            execution.controller = controller_with_policy(write_policy());
            c.arrange(execution);

            const auto result = run_broker(*execution.effect_broker, slot("attempt-" + c.name),
                                           tool, execution);
            EXPECT_EQ(result.status, c.status) << result.error;
            EXPECT_FALSE(result.effect_uncertain);
            EXPECT_FALSE(attempt.effects()->uncertain());
            EXPECT_FALSE(attempt.effects()->committed());
            const auto entries = attempt.effects()->entries();
            ASSERT_EQ(entries.size(), 1u);
            EXPECT_EQ(entries[0].status, c.status);
            EXPECT_EQ(tool.runs.load(), 0);
        }
    }
}
