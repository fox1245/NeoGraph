// One classifier for every node and tool failure (#364, WP8).
//
// classify_node_failure walks the whole cause chain (NodeExecutionError causes,
// std::nested_exception, nested subgraph failures) and the most severe class
// found anywhere wins. Only ordinary failures (NG.TaskFailed, NG.SubgraphFailed,
// author codes), NG.Timeout and transient provider failures (NG.ProviderFailed)
// are routable. The node retry loop and the tool failure contract take their
// decisions from the same verdict.
//
// The first half is the classification table, one row per class. The second
// half drives the real NodeExecutor and records the retry decision for every
// exception family the retry loop used to handle on its own.

#include "../src/core/tool_failure.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/executor.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/hook_outbox.h>

#include <asio/error.hpp>
#include <asio/system_error.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

using Class = NodeFailureClass;

template <typename E>
std::exception_ptr make(E error) {
    return std::make_exception_ptr(std::move(error));
}

std::shared_ptr<const sp::Outcome> provider_outcome(sp::ErrorKind kind, int http_status = 0,
                                                    std::optional<sp::StopKind> stop = {}) {
    sp::Failure failure;
    failure.error.kind = kind;
    failure.error.safe_message = "provider failed";
    failure.error.http_status = http_status;
    if (stop) failure.partial.stop = sp::StopReason{*stop, "raw"};
    return std::make_shared<const sp::Outcome>(std::move(failure));
}

std::exception_ptr provider(sp::ErrorKind kind, int http_status = 0,
                            std::optional<sp::StopKind> stop = {}) {
    return make(ProviderFailure(provider_outcome(kind, http_status, stop)));
}

std::exception_ptr in_subgraph(std::exception_ptr cause, const std::string& node = "inner") {
    return make(NodeExecutionError(node, 1, std::move(cause)));
}

// `outer` thrown while `inner` is being handled: a std::nested_exception chain.
std::exception_ptr nested(std::exception_ptr inner, const std::string& outer) {
    try {
        try {
            std::rethrow_exception(inner);
        } catch (...) {
            std::throw_with_nested(std::runtime_error(outer));
        }
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

struct UnrelatedException : std::exception {
    const char* what() const noexcept override { return "outside every known family"; }
};

struct Row {
    const char* name;
    std::function<std::exception_ptr()> error;
    Class expected;
    std::string code;
    bool retryable;
    bool routable;
};

std::vector<Row> table() {
    const auto cancelled_token = std::make_shared<CancelToken>();
    return {
        // ── Ordinary: routable, retried ───────────────────────────────────
        {"runtime_error", [] { return make(std::runtime_error("boom")); },
         Class::Ordinary, "NG.TaskFailed", true, true},
        {"system_error", [] {
             return make(std::system_error(std::make_error_code(std::errc::io_error)));
         },
         Class::Ordinary, "NG.TaskFailed", true, true},
        {"asio aborted without a cancelled run", [] {
             return make(asio::system_error(asio::error::operation_aborted));
         },
         Class::Ordinary, "NG.TaskFailed", true, true},
        {"author code", [] { return make(NodeFailureError("app.compile_error", "no build")); },
         Class::Ordinary, "app.compile_error", true, true},
        {"author code marked non-retryable", [] {
             return make(NodeFailureError("app.compile_error", "", false));
         },
         Class::Ordinary, "app.compile_error", false, true},
        {"subgraph whose root is ordinary", [] {
             return in_subgraph(make(std::runtime_error("child failed")));
         },
         Class::Ordinary, "NG.SubgraphFailed", true, true},
        {"subgraph whose root is a non-retryable author code", [] {
             return in_subgraph(make(NodeFailureError("app.flaky", "", false)));
         },
         Class::Ordinary, "NG.SubgraphFailed", false, true},
        // ── Transient provider failures: routable, not retried ───────────
        {"provider rate limited", [] { return provider(sp::ErrorKind::RateLimited); },
         Class::ProviderFailed, "NG.ProviderFailed", false, true},
        {"provider overloaded", [] { return provider(sp::ErrorKind::Overloaded); },
         Class::ProviderFailed, "NG.ProviderFailed", false, true},
        {"provider transport", [] { return provider(sp::ErrorKind::Transport); },
         Class::ProviderFailed, "NG.ProviderFailed", false, true},
        {"provider stream cut", [] { return provider(sp::ErrorKind::Truncated); },
         Class::ProviderFailed, "NG.ProviderFailed", false, true},
        {"provider timeout", [] { return provider(sp::ErrorKind::DeadlineExceeded); },
         Class::ProviderFailed, "NG.ProviderFailed", false, true},
        {"provider server error", [] { return provider(sp::ErrorKind::RemoteFailure, 503); },
         Class::ProviderFailed, "NG.ProviderFailed", false, true},
        // ── Never routable ────────────────────────────────────────────────
        {"logic_error", [] { return make(std::logic_error("broken invariant")); },
         Class::LogicError, "NG.LogicError", false, false},
        {"integrity check", [] {
             return make(std::invalid_argument(
                 "Managed provider effect does not match its host task scope"));
         },
         Class::LogicError, "NG.LogicError", false, false},
        {"provider quota exhausted", [] { return provider(sp::ErrorKind::QuotaExhausted); },
         Class::BudgetExhausted, "NG.BudgetExhausted", false, false},
        {"provider resource limit", [] { return provider(sp::ErrorKind::ResourceLimit); },
         Class::BudgetExhausted, "NG.BudgetExhausted", false, false},
        {"provider safety block", [] {
             return provider(sp::ErrorKind::RemoteFailure, 200, sp::StopKind::ContentFilter);
         },
         Class::SafetyBlocked, "NG.SafetyBlocked", false, false},
        {"provider refusal on a server error", [] {
             return provider(sp::ErrorKind::RemoteFailure, 500, sp::StopKind::Refusal);
         },
         Class::SafetyBlocked, "NG.SafetyBlocked", false, false},
        {"provider in-band remote failure", [] {
             return provider(sp::ErrorKind::RemoteFailure, 200);
         },
         Class::Unknown, "NG.UnknownFailure", false, false},
        {"provider configuration", [] { return provider(sp::ErrorKind::InvalidConfig); },
         Class::ConfigError, "NG.ConfigError", false, false},
        {"provider authentication", [] { return provider(sp::ErrorKind::Authentication); },
         Class::ConfigError, "NG.ConfigError", false, false},
        {"provider misuse", [] { return provider(sp::ErrorKind::Misuse); },
         Class::ConfigError, "NG.ConfigError", false, false},
        {"provider unknown limits", [] { return provider(sp::ErrorKind::LimitUnknown); },
         Class::ConfigError, "NG.ConfigError", false, false},
        {"provider protocol corruption", [] { return provider(sp::ErrorKind::ProtocolCorrupt); },
         Class::Unknown, "NG.UnknownFailure", false, false},
        {"provider cancelled outside a cancelled run", [] {
             return provider(sp::ErrorKind::Cancelled);
         },
         Class::Unknown, "NG.UnknownFailure", false, false},
        {"provider outcome error", [] {
             return make(ProviderOutcomeError("observer failed",
                                              provider_outcome(sp::ErrorKind::Transport), {}));
         },
         Class::EffectUncertain, "NG.EffectUncertain", false, false},
        {"bad_alloc", [] { return make(std::bad_alloc()); },
         Class::Unknown, "NG.UnknownFailure", false, false},
        {"exception outside the known families", [] { return make(UnrelatedException()); },
         Class::Unknown, "NG.UnknownFailure", false, false},
        {"non-standard exception", [] { return std::make_exception_ptr(42); },
         Class::Unknown, "NG.UnknownFailure", false, false},
        // ── Run control: never routable, never retried ───────────────────
        {"node interrupt", [] { return make(NodeInterrupt("ask a human")); },
         Class::Interrupted, "NG.Interrupted", false, false},
        {"hook block", [] {
             return make(HookBoundaryBlocked("hook-1", HookExecutionState::Failed, "blocked"));
         },
         Class::HookBlocked, "NG.HookBlocked", false, false},
        {"cancellation", [] { return make(CancelledException("stop")); },
         Class::Cancelled, "NG.Cancelled", false, false},
        // ── Cause chains: the most severe class anywhere wins ────────────
        {"quota exhaustion in a nested subgraph", [] {
             return in_subgraph(in_subgraph(provider(sp::ErrorKind::QuotaExhausted), "leaf"),
                                "middle");
         },
         Class::BudgetExhausted, "NG.BudgetExhausted", false, false},
        {"logic_error inside a subgraph", [] {
             return in_subgraph(make(std::logic_error("child invariant")));
         },
         Class::LogicError, "NG.LogicError", false, false},
        {"hook block inside a subgraph", [] {
             return in_subgraph(
                 make(HookBoundaryBlocked("hook-2", HookExecutionState::TimedOut, "late")));
         },
         Class::HookBlocked, "NG.HookBlocked", false, false},
        {"quota exhaustion behind std::nested_exception", [] {
             return nested(provider(sp::ErrorKind::QuotaExhausted), "wrapper");
         },
         Class::BudgetExhausted, "NG.BudgetExhausted", false, false},
        {"author code behind std::nested_exception", [] {
             return nested(make(NodeFailureError("app.inner", "", false)), "wrapper");
         },
         Class::Ordinary, "NG.TaskFailed", false, true},
        {"transient provider failure in a subgraph", [] {
             return in_subgraph(provider(sp::ErrorKind::Overloaded));
         },
         Class::ProviderFailed, "NG.ProviderFailed", false, true},
    };
}

}  // namespace

TEST(NodeFailureClassifier, ClassifiesEveryRow) {
    for (const auto& row : table()) {
        SCOPED_TRACE(row.name);
        const auto verdict = neograph::detail::classify_node_failure(row.error(), nullptr);
        EXPECT_EQ(verdict.failure_class, row.expected);
        EXPECT_EQ(verdict.code, row.code);
        EXPECT_EQ(verdict.retryable, row.retryable);
        EXPECT_EQ(verdict.routable, row.routable);
    }
}

TEST(NodeFailureClassifier, AbortedOperationOfACancelledRunIsCancellation) {
    CancelToken token;
    token.cancel();
    const auto verdict = neograph::detail::classify_node_failure(
        make(asio::system_error(asio::error::operation_aborted)), &token);
    EXPECT_EQ(verdict.failure_class, Class::Cancelled);
    // So is a provider call the run's cancellation stopped.
    EXPECT_EQ(neograph::detail::classify_node_failure(provider(sp::ErrorKind::Cancelled), &token)
                  .failure_class,
              Class::Cancelled);
    // Another error code is still ordinary under a cancelled run.
    EXPECT_EQ(neograph::detail::classify_node_failure(
                  make(asio::system_error(asio::error::connection_reset)), &token)
                  .failure_class,
              Class::Ordinary);
}

TEST(NodeFailureClassifier, KeepsOnlyTypedPublicText) {
    const auto authored = neograph::detail::classify_node_failure(
        make(NodeFailureError("app.compile_error", "the patch does not compile")), nullptr);
    EXPECT_EQ(authored.public_message, "the patch does not compile");
    const auto plain = neograph::detail::classify_node_failure(make(std::runtime_error("secret=1")), nullptr);
    EXPECT_TRUE(plain.public_message.empty());
}

TEST(NodeFailureClassifier, ADeepChainIsWalkedAndAnEndlessOneFailsClosed) {
    auto deep = provider(sp::ErrorKind::QuotaExhausted);
    for (int i = 0; i < 40; ++i) deep = in_subgraph(deep, "level" + std::to_string(i));
    EXPECT_EQ(neograph::detail::classify_node_failure(deep, nullptr).failure_class, Class::BudgetExhausted);

    auto endless = make(std::runtime_error("root"));
    for (int i = 0; i < 1000; ++i) endless = in_subgraph(endless, "level" + std::to_string(i));
    const auto verdict = neograph::detail::classify_node_failure(endless, nullptr);
    EXPECT_EQ(verdict.failure_class, Class::Unknown);
    EXPECT_FALSE(verdict.routable);
}

TEST(NodeFailureClassifier, AuthorCodes) {
    EXPECT_TRUE(is_author_failure_code("app.compile_error"));
    EXPECT_TRUE(is_author_failure_code("plan.user_abort"));
    EXPECT_TRUE(is_author_failure_code("a.b.c_1"));
    EXPECT_FALSE(is_author_failure_code("app"));            // needs a dot
    EXPECT_FALSE(is_author_failure_code("App.error"));      // lower case only
    EXPECT_FALSE(is_author_failure_code("1app.error"));
    EXPECT_FALSE(is_author_failure_code("app..error"));
    EXPECT_FALSE(is_author_failure_code("app.error."));
    EXPECT_FALSE(is_author_failure_code("NG.TaskFailed"));  // reserved
    EXPECT_FALSE(is_author_failure_code("ng.task_failed"));  // reserved, any case
    EXPECT_FALSE(is_author_failure_code(""));
    EXPECT_FALSE(is_author_failure_code("a." + std::string(200, 'b')));
    EXPECT_THROW(NodeFailureError("NG.TaskFailed"), std::invalid_argument);
    EXPECT_THROW(NodeFailureError("not a code"), std::invalid_argument);
}

TEST(NodeFailureClassifier, ClassTableIsConsistent) {
    for (int value = 0; value <= static_cast<int>(Class::Cancelled); ++value) {
        const auto failure_class = static_cast<Class>(value);
        SCOPED_TRACE(node_failure_code(failure_class));
        // Only Ordinary, Timeout and ProviderFailed are routable.
        EXPECT_EQ(node_failure_routable(failure_class),
                  failure_class == Class::Ordinary || failure_class == Class::Timeout ||
                      failure_class == Class::ProviderFailed);
        // Only Ordinary and Timeout are retried.
        EXPECT_EQ(node_failure_retryable(failure_class),
                  failure_class == Class::Ordinary || failure_class == Class::Timeout);
    }
    EXPECT_STREQ(node_failure_code(Class::EffectCommitted), "NG.EffectCommitted");
    EXPECT_STREQ(to_string(NodeFailureKind::EffectCommitted), "effect_committed");
    EXPECT_STREQ(to_string(NodeFailureKind::Exhausted), "exhausted");
    EXPECT_STREQ(to_string(NodeFailureKind::NonRetryable), "non_retryable");
}

// The tool failure contract is the same classifier: a run control class
// anywhere in a tool's cause chain stops the run, everything else is a tool
// message the model sees.
TEST(NodeFailureClassifier, ToolFailuresUseTheSameVerdict) {
    for (const auto& row : table()) {
        SCOPED_TRACE(row.name);
        const auto error = row.error();
        const auto tool = neograph::detail::classify_tool_failure(error, nullptr);
        switch (neograph::detail::classify_node_failure(error, nullptr).failure_class) {
        case Class::Cancelled: EXPECT_EQ(tool, neograph::detail::ToolFailureClass::Cancelled); break;
        case Class::HookBlocked: EXPECT_EQ(tool, neograph::detail::ToolFailureClass::HookBlocked); break;
        case Class::Interrupted: EXPECT_EQ(tool, neograph::detail::ToolFailureClass::Interrupted); break;
        default: EXPECT_EQ(tool, neograph::detail::ToolFailureClass::ToolError); break;
        }
    }
}

// ── The node retry loop decides with the same verdict ─────────────────────

namespace {

class ThrowingNode final : public GraphNode {
public:
    ThrowingNode(std::function<void(int)> fail, int succeed_on = -1)
        : fail_(std::move(fail)), succeed_on_(succeed_on) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        const int call = calls.fetch_add(1);
        if (call != succeed_on_) fail_(call);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"result", json("ok")});
        co_return out;
    }
    std::string get_name() const override { return "worker"; }
    std::atomic<int> calls{0};

private:
    std::function<void(int)> fail_;
    int succeed_on_;
};

struct Harness {
    explicit Harness(std::function<void(int)> fail, int max_retries = 2, int succeed_on = -1) {
        auto owned = std::make_unique<ThrowingNode>(std::move(fail), succeed_on);
        node = owned.get();
        nodes.emplace("worker", std::move(owned));
        ChannelDef channel;
        channel.name = "result";
        channel.reducer_name = "overwrite";
        channel.type = ReducerType::OVERWRITE;
        channels.push_back(channel);
        RetryPolicy policy;
        policy.max_retries = max_retries;
        policy.initial_delay_ms = 1;
        policy.backoff_multiplier = 1.0f;
        policy.max_delay_ms = 1;
        executor = std::make_unique<NodeExecutor>(
            nodes, channels, [policy](const std::string&) { return policy; });
    }

    // Runs the node and returns what escaped the retry loop.
    std::exception_ptr run() {
        GraphState state;
        for (const auto& cd : channels)
            state.init_channel(cd.name, cd.type, ReducerRegistry::instance().get(cd.reducer_name),
                               cd.initial_value);
        RunContext ctx;
        try {
            (void)async::run_sync(executor->execute_node_with_retry_async(
                "worker", state, nullptr, StreamMode::EVENTS, ctx));
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<ChannelDef> channels;
    std::unique_ptr<NodeExecutor> executor;
    ThrowingNode* node = nullptr;
};

template <typename E>
std::function<void(int)> throws(E error) {
    return [error](int) { throw error; };
}

std::optional<NodeFailure> record_of(const std::exception_ptr& error) {
    return node_failure_of(error);
}

}  // namespace

TEST(NodeFailureRetry, OrdinaryFailureIsRetriedUntilExhausted) {
    Harness h(throws(std::runtime_error("transient")));
    const auto error = h.run();
    EXPECT_EQ(h.node->calls.load(), 3);
    const auto record = record_of(error);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->node, "worker");
    EXPECT_EQ(record->code, "NG.TaskFailed");
    EXPECT_EQ(record->kind, NodeFailureKind::Exhausted);
    EXPECT_EQ(record->attempts, 3);
    EXPECT_TRUE(record->retryable);
    EXPECT_TRUE(record->routable);
    EXPECT_TRUE(record->effects.empty());
}

TEST(NodeFailureRetry, AuthorCodeIsRetriedAndReported) {
    Harness h(throws(NodeFailureError("app.flaky_check", "flaky")), 2, 1);
    EXPECT_FALSE(h.run());
    EXPECT_EQ(h.node->calls.load(), 2);

    Harness exhausted(throws(NodeFailureError("app.flaky_check", "flaky")));
    const auto record = record_of(exhausted.run());
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "app.flaky_check");
    EXPECT_EQ(record->public_message, "flaky");
    EXPECT_EQ(record->attempts, 3);
}

TEST(NodeFailureRetry, NonRetryableAuthorCodeRunsOnce) {
    Harness h(throws(NodeFailureError("app.compile_error", "", false)));
    const auto error = h.run();
    EXPECT_EQ(h.node->calls.load(), 1);
    const auto record = record_of(error);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "app.compile_error");
    EXPECT_EQ(record->kind, NodeFailureKind::NonRetryable);
    EXPECT_FALSE(record->retryable);
    EXPECT_TRUE(record->routable);
    EXPECT_EQ(record->attempts, 1);
}

// Retry audit: what the loop did on a84bced next to the classifier's decision.

TEST(NodeFailureRetry, AuditRuntimeErrorFamilyStillRetried) {
    // Unchanged: runtime_error and its family (including Program diagnostics,
    // which Core sees as runtime_error) are ordinary.
    struct DiagnosticLike : std::runtime_error { using std::runtime_error::runtime_error; };
    Harness h(throws(DiagnosticLike("P_RUNTIME_SOMETHING")));
    (void)h.run();
    EXPECT_EQ(h.node->calls.load(), 3);
}

TEST(NodeFailureRetry, AuditAsioErrorStillRetried) {
    Harness h(throws(asio::system_error(asio::error::connection_reset)));
    const auto record = record_of(h.run());
    EXPECT_EQ(h.node->calls.load(), 3);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "NG.TaskFailed");
}

TEST(NodeFailureRetry, AuditOrdinarySubgraphFailureStillRetried) {
    Harness h(throws(NodeExecutionError("child", 1, make(std::runtime_error("child failed")))));
    const auto error = h.run();
    EXPECT_EQ(h.node->calls.load(), 3);
    // The innermost dispatch context is kept, as before.
    try {
        std::rethrow_exception(error);
    } catch (const NodeExecutionError& failure) {
        EXPECT_EQ(failure.node_name(), "child");
        const auto* record = node_failure_of(failure);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->node, "worker");
        EXPECT_EQ(record->code, "NG.SubgraphFailed");
        EXPECT_EQ(record->attempts, 3);
    }
}

TEST(NodeFailureRetry, AuditSubgraphLogicErrorNoLongerRetried) {
    // Before: retried, because only the outer NodeExecutionError was seen.
    Harness h(throws(NodeExecutionError("child", 1, make(std::logic_error("invariant")))));
    const auto record = record_of(h.run());
    EXPECT_EQ(h.node->calls.load(), 1);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "NG.LogicError");
    EXPECT_FALSE(record->routable);
}

TEST(NodeFailureRetry, AuditQuotaBehindNestedExceptionNoLongerRetried) {
    // Before: retried; the provider outcome check did not look inside
    // std::nested_exception.
    Harness h([](int) {
        try {
            throw ProviderFailure(provider_outcome(sp::ErrorKind::QuotaExhausted));
        } catch (...) {
            std::throw_with_nested(std::runtime_error("wrapped"));
        }
    });
    const auto record = record_of(h.run());
    EXPECT_EQ(h.node->calls.load(), 1);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "NG.BudgetExhausted");
}

TEST(NodeFailureRetry, AuditUnknownExceptionsNoLongerRetried) {
    // Before: catch (...) retried anything outside runtime/logic_error.
    Harness unrelated(throws(UnrelatedException()));
    const auto record = record_of(unrelated.run());
    EXPECT_EQ(unrelated.node->calls.load(), 1);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "NG.UnknownFailure");
    EXPECT_FALSE(record->routable);

    Harness non_standard([](int) { throw 42; });
    (void)non_standard.run();
    EXPECT_EQ(non_standard.node->calls.load(), 1);
}

TEST(NodeFailureRetry, AuditUnchangedNonRetryableFamilies) {
    {
        Harness h(throws(std::logic_error("bug")));
        const auto record = record_of(h.run());
        EXPECT_EQ(h.node->calls.load(), 1);
        ASSERT_TRUE(record);
        EXPECT_EQ(record->code, "NG.LogicError");
    }
    {
        Harness h(throws(ProviderFailure(provider_outcome(sp::ErrorKind::RateLimited))));
        const auto record = record_of(h.run());
        EXPECT_EQ(h.node->calls.load(), 1);
        ASSERT_TRUE(record);
        EXPECT_EQ(record->code, "NG.ProviderFailed");
        EXPECT_TRUE(record->routable);
        EXPECT_FALSE(record->retryable);
    }
    {
        Harness h(throws(HookBoundaryBlocked("hook", HookExecutionState::Failed, "no")));
        const auto error = h.run();
        EXPECT_EQ(h.node->calls.load(), 1);
        EXPECT_THROW(std::rethrow_exception(error), NodeExecutionError);
    }
    {
        // Control flow still escapes unwrapped.
        Harness h(throws(NodeInterrupt("ask")));
        EXPECT_THROW(std::rethrow_exception(h.run()), NodeInterrupt);
        Harness c(throws(CancelledException("stop")));
        EXPECT_THROW(std::rethrow_exception(c.run()), CancelledException);
        Harness oom(throws(std::bad_alloc()));
        EXPECT_THROW(std::rethrow_exception(oom.run()), std::bad_alloc);
        EXPECT_EQ(oom.node->calls.load(), 1);
    }
}
