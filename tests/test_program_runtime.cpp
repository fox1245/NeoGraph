#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/hook.h>
#include <neograph/hook_outbox.h>
#include <neograph/hook_runtime.h>
#include <neograph/program/program.h>
#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/store.h>
#include <neograph/tool_dispatch.h>
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
#include <neograph/program/postgres_store.h>
#include <neograph/program/postgres_transition_store.h>

#include <libpq-fe.h>
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES_CHECKPOINT
#include <neograph/graph/postgres_checkpoint.h>
#endif
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
#include <neograph/graph/sqlite_checkpoint.h>
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#include <neograph/program/sqlite_core_tool_grant_store.h>

#include <sqlite3.h>
#endif

#include "program_expansion_fixture.h"
#include "fixtures/unique_temp_dir.h"
#include "fixtures/coarse_clock_store.h"
#include "fixtures/fork_share.h"
#include "fixtures/vanishing_checkpoint_store.h"
#include "javascript.h"
#include "registry_access.h"
#include "run_control.h"
#include <asio/bind_cancellation_slot.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include "fixtures/store_handle.h"

using neograph::program::test_support::FragmentStoreHandle;
using neograph::program::test_support::GrantStoreHandle;
using neograph::program::test_support::OwnedProgramStore;
using neograph::program::test_support::OwnedTransitionStore;
using neograph::program::test_support::ProgramStoreHandle;
using neograph::program::test_support::TransitionStoreHandle;
neograph::json orchestration_document(neograph::json root, std::string node_type);
namespace {

using neograph::json;
using neograph::ChatTool;
using neograph::HookDefinition;
using neograph::HookDefinitionData;
using neograph::HookInputMapperKind;
using neograph::HookPhase;
using neograph::HookRegistry;
using neograph::HookRuntime;
using neograph::HookTargetContract;
using neograph::HookTargetResolver;
using neograph::InMemoryHookJournal;
using neograph::MandatoryHookRunner;
using neograph::NativeHookExecutionAdapter;
using neograph::Tool;
using neograph::ToolEffectClass;
using neograph::ToolExecutionController;
using namespace neograph::graph;
using namespace neograph::program;
using namespace neograph::program_test;

std::atomic<unsigned>               completed_calls{0};
std::atomic<unsigned>               mediated_tool_calls{0};
std::atomic<neograph::ToolExecutionController*> mediated_tool_controller{nullptr};
std::string                         synthesis_execution_marker;
std::atomic<unsigned>               interrupt_calls{0};
std::atomic<unsigned>               blocking_calls{0};
std::atomic<unsigned>               blocking_active{0};
std::atomic<unsigned>               blocking_peak{0};
std::atomic<unsigned>               followup_calls{0};
std::atomic<unsigned>               stubborn_calls{0};
std::atomic<unsigned>               scheduler_blocking_calls{0};
std::atomic<unsigned>               map_fail_first_calls{0};
std::shared_ptr<std::barrier<>>     overlap_barrier;
std::shared_ptr<std::promise<void>> overlap_ready;
std::atomic<unsigned>               map_fail_first_active{0};
std::atomic<unsigned>               window_calls{0};
std::atomic<unsigned>               window_active{0};
std::atomic<unsigned>               window_peak{0};
std::atomic<unsigned>               window_completed{0};
std::atomic<unsigned>               window_second_started_before_completion{0};
std::shared_ptr<std::barrier<>>     window_first_barrier;



class CompletedNode final : public GraphNode {
public:
    explicit CompletedNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++completed_calls;
        if (!synthesis_execution_marker.empty()) {
            std::ofstream marker(synthesis_execution_marker, std::ios::app);
            marker << "executed\n";
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", "completed"});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class MediatedProbeTool final : public Tool {
public:
    ChatTool get_definition() const override {
        return {"mediated-probe", "Program gate probe", json{{"type", "object"}}};
    }
    std::string get_name() const override { return "mediated-probe"; }
    std::string execute(const json&) override {
        ++mediated_tool_calls;
        return "ok";
    }
};

class MediatedDispatchNode final : public GraphNode {
public:
    explicit MediatedDispatchNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        MediatedProbeTool tool;
        mediated_tool_controller.store(in.ctx.tool_execution_controller.get());
        neograph::ToolGateContext gate_context;
        gate_context.thread_id = in.ctx.thread_id;
        gate_context.step = in.ctx.step;
        auto execution = neograph::graph::make_tool_execution_context(in.ctx);
        // GCC 13 cannot lower nested temporary vectors across this co_await.
        std::vector<neograph::ToolCall> calls;
        calls.push_back(neograph::ToolCall{"call-1", "mediated-probe", "{}"});
        std::vector<Tool*> tools{&tool};
        (void)co_await dispatch_tool_calls(
            std::move(calls), std::move(tools),
            in.ctx.tool_gate, std::move(gate_context), std::move(execution));
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", mediated_tool_calls.load()});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};
class MapEchoNode final : public GraphNode {
public:
    explicit MapEchoNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++completed_calls;
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.state.get("item")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class InterruptNode final : public GraphNode {
public:
    explicit InterruptNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            throw NodeInterrupt("approval required", json{{"request", "approve"}});
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->at("decision")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// #388: asks until it is answered, and writes its decision to its own
// "<name>_answer" channel. Every run is counted by node name.
std::mutex                      named_ask_mutex;
std::map<std::string, unsigned> named_ask_runs;

unsigned named_ask_runs_of(const std::string& name) {
    std::lock_guard lock(named_ask_mutex);
    const auto      found = named_ask_runs.find(name);
    return found == named_ask_runs.end() ? 0U : found->second;
}

// #394: called with the node's name each time a NamedAskNode runs with an
// answer, before it writes it; a test reads durable state there.
std::function<void(const std::string&)> named_ask_answered_hook;

void set_named_ask_answered_hook(std::function<void(const std::string&)> hook) {
    std::lock_guard lock(named_ask_mutex);
    named_ask_answered_hook = std::move(hook);
}

class NamedAskNode final : public GraphNode {
public:
    explicit NamedAskNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        {
            std::lock_guard lock(named_ask_mutex);
            ++named_ask_runs[name_];
        }
        if (!input.ctx.resume_value)
            throw NodeInterrupt("approve " + name_, json{{"asks", name_}});
        std::function<void(const std::string&)> hook;
        {
            std::lock_guard lock(named_ask_mutex);
            hook = named_ask_answered_hook;
        }
        if (hook) hook(name_);
        NodeOutput output;
        output.writes.push_back(
            ChannelWrite{name_ + "_answer", input.ctx.resume_value->at("decision")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Runs a hook (set by a test) and then pauses: the hook can arm a store fault
// so that the interrupted run's terminal publication fails.
std::function<void()> before_fault_interrupt;

class FaultInterruptNode final : public GraphNode {
public:
    explicit FaultInterruptNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        if (before_fault_interrupt) before_fault_interrupt();
        throw NodeInterrupt("approval required", json{{"request", "approve"}});
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class AlwaysInterruptNode final : public GraphNode {
public:
    explicit AlwaysInterruptNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++interrupt_calls;
        throw NodeInterrupt("second approval required", json{{"request", "approve-second"}});
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class EffectInterruptNode final : public GraphNode {
public:
    explicit EffectInterruptNode(std::string name, bool non_idempotent = false)
        : name_(std::move(name)), non_idempotent_(non_idempotent) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            throw NodeInterrupt(
                "capability result required",
                json{{"__neograph_program_pending_kind", "effect"},
                     {"call_id", "call-effect-1"},
                     {"result_schema", json{{"type", "object"}}},
                     {"effect",
                      json{{"effect_id", "effect-search-1"},
                           {"idempotency", non_idempotent_ ? "non_idempotent" : "supported"},
                           {"tool", "search"},
                           {"arguments", json{{"query", "neo"}}}}},
                     {"expires_at_unix_ms", 4102444800000ULL}});
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->at("result")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    bool        non_idempotent_ = false;
};

// #364: counts its calls and fails the way it was asked to.
std::atomic<unsigned> classified_failure_calls{0};
class ClassifiedFailureNode final : public GraphNode {
public:
    // FirstCallOrdinary fails on the first call of the process-wide counter and
    // succeeds on every later one.
    enum class Kind { Ordinary, Logic, Diagnostic, FirstCallOrdinary };
    ClassifiedFailureNode(std::string name, Kind kind) : name_(std::move(name)), kind_(kind) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        const auto call = ++classified_failure_calls;
        if (kind_ == Kind::FirstCallOrdinary) {
            if (call == 1) throw std::runtime_error("ordinary failure");
            co_return NodeOutput{};
        }
        if (kind_ == Kind::Logic) throw std::logic_error("integrity check failed");
        if (kind_ == Kind::Diagnostic) {
            neograph::program::Diagnostic diagnostic;
            diagnostic.phase    = CompilePhase::Seal;
            diagnostic.code     = "P_TEST_DIAGNOSTIC";
            diagnostic.severity = DiagnosticSeverity::Error;
            diagnostic.primary  = {"runtime", "", std::nullopt};
            diagnostic.message  = "a Program diagnostic inside a node";
            throw ProgramDiagnosticError(std::move(diagnostic));
        }
        throw std::runtime_error("ordinary failure");
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    Kind        kind_;
};

class FailingNode final : public GraphNode {
public:
    explicit FailingNode(std::string name, std::string message = "classified core failure")
        : name_(std::move(name)), message_(std::move(message)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        throw std::runtime_error(message_);
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::string message_;
};

class NonUtf8ErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "test-platform"; }
    std::string message(int) const override { return std::string("\xc7\xf6\xc0\xe7", 4); }
};

class FailingSystemErrorNode final : public GraphNode {
public:
    explicit FailingSystemErrorNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        static const NonUtf8ErrorCategory category;
        throw std::system_error(std::error_code(10053, category));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class MalformedEventThenFailNode final : public GraphNode {
public:
    explicit MalformedEventThenFailNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        if (!input.stream_cb) throw std::runtime_error("stream callback missing");
        bool rejected = false;
        try {
            (*input.stream_cb)(GraphEvent{GraphEvent::Type::LLM_TOKEN, name_,
                                          json(std::string("\xc7\xf6\xc0\xe7", 4))});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected) throw std::runtime_error("invalid event was accepted");
        throw std::runtime_error("node failed after rejected event");
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class ActiveCallGuard final {
public:
    explicit ActiveCallGuard(std::atomic<unsigned>& active) : active_(active) {}
    ~ActiveCallGuard() { leave(); }
    /// Leaves the count now, by ordinary code, instead of in the destructor.
    void leave() noexcept {
        if (left_) return;
        left_ = true;
        active_.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::atomic<unsigned>& active_;
    bool                   left_ = false;
};

class MapFailFirstNode final : public GraphNode {
public:
    explicit MapFailFirstNode(std::string name, bool complete_first = false)
        : name_(std::move(name)), complete_first_(complete_first) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto call = map_fail_first_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call == 1) {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            for (unsigned attempt = 0; map_fail_first_active.load(std::memory_order_relaxed) == 0;
                 ++attempt) {
                if (attempt == 2000)
                    throw std::runtime_error("map fail-fast sibling did not become active");
                timer.expires_after(std::chrono::milliseconds(1));
                co_await timer.async_wait(asio::use_awaitable);
            }
            if (complete_first_) co_return NodeOutput{};
            throw std::runtime_error("map fail-fast sentinel");
        }
        map_fail_first_active.fetch_add(1, std::memory_order_relaxed);
        ActiveCallGuard active_guard(map_fail_first_active);
        auto            timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::seconds(5));
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    bool complete_first_;
};

// Interrupts for a value (an input, or an effect result). Given one, it runs
// the hook a test armed, once, then writes the value it saw. A hook that
// blocks holds the attempt as if its process had stopped there.
std::mutex            resumed_value_hook_mutex;
std::function<void()> resumed_value_hook;

class ResumeHookNode final : public GraphNode {
public:
    ResumeHookNode(std::string name, bool effect) : name_(std::move(name)), effect_(effect) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            if (!effect_) throw NodeInterrupt("approval required", json{{"request", "approve"}});
            throw NodeInterrupt(
                "capability result required",
                json{{"__neograph_program_pending_kind", "effect"},
                     {"call_id", "call-effect-1"},
                     {"result_schema", json{{"type", "object"}}},
                     {"effect", json{{"effect_id", "effect-search-1"},
                                     {"idempotency", "non_idempotent"},
                                     {"tool", "search"},
                                     {"arguments", json{{"query", "neo"}}}}},
                     {"expires_at_unix_ms", 4102444800000ULL}});
        }
        std::function<void()> hook;
        {
            std::lock_guard lock(resumed_value_hook_mutex);
            hook = std::exchange(resumed_value_hook, nullptr);
        }
        if (hook) hook();
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->dump()});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    bool        effect_;
};

// Holds the next resumed attempt inside its node until released.
class HeldResumedAttempt {
public:
    HeldResumedAttempt() {
        std::lock_guard lock(resumed_value_hook_mutex);
        resumed_value_hook = [this] {
            entered_.set_value();
            released_.wait();
        };
    }
    ~HeldResumedAttempt() {
        release();
        std::lock_guard lock(resumed_value_hook_mutex);
        resumed_value_hook = nullptr;
    }
    bool entered() {
        return entered_future_.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    }
    void release() {
        if (!released_flag_.exchange(true)) release_.set_value();
    }

private:
    std::promise<void>       entered_;
    std::future<void>        entered_future_ = entered_.get_future();
    std::promise<void>       release_;
    std::shared_future<void> released_ = release_.get_future().share();
    std::atomic<bool>        released_flag_{false};
};

// Fails when its "value" channel is "fail"; otherwise waits until cancelled.
class ValueGatedNode final : public GraphNode {
public:
    explicit ValueGatedNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        if (input.state.get("value") == json("fail"))
            throw std::runtime_error("join member failure sentinel");
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::seconds(5));
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// A join member driven by its "value" channel (#380): "ask" interrupts until
// it is resumed, "fail" throws, "slow" finishes after a short cancellable wait,
// "block" holds its thread for 800 ms and ignores cancellation (#367, #368),
// and any other value finishes at once. Every run is counted by its value,
// and join_member_active counts the bodies still running.
std::mutex                      join_member_mutex;
std::map<std::string, unsigned> join_member_runs;
std::atomic<unsigned>           join_member_active{0};
std::atomic<unsigned>           join_member_peak{0};

void reset_join_member_runs() {
    std::lock_guard lock(join_member_mutex);
    join_member_runs.clear();
    join_member_active.store(0);
    join_member_peak.store(0);
}

unsigned join_member_runs_of(const std::string& value) {
    std::lock_guard lock(join_member_mutex);
    const auto      found = join_member_runs.find(value);
    return found == join_member_runs.end() ? 0U : found->second;
}

// No join member body may still be running once a run has returned. The
// member leaves the count by ordinary code before any exception leaves it, so
// the count does not depend on when a compiler finishes unwinding the frame.
void expect_join_members_stopped() { EXPECT_EQ(join_member_active.load(), 0U); }

class JoinMemberNode final : public GraphNode {
public:
    explicit JoinMemberNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto value = input.state.get("value").get<std::string>();
        {
            std::lock_guard lock(join_member_mutex);
            ++join_member_runs[value];
        }
        const auto running = join_member_active.fetch_add(1, std::memory_order_relaxed) + 1;
        for (auto peak = join_member_peak.load(std::memory_order_relaxed);
             peak < running &&
             !join_member_peak.compare_exchange_weak(peak, running, std::memory_order_relaxed,
                                                     std::memory_order_relaxed);) {}
        ActiveCallGuard active(join_member_active);
        // Every way out through an exception leaves the count first, by
        // ordinary code: a test that checks the run waited for the body must
        // not depend on when a compiler unwinds a coroutine frame.
        if (value == "fail") {
            active.leave();
            throw std::runtime_error("join member failure sentinel");
        }
        if ((value == "ask" || value == "ask-slow") && !input.ctx.resume_value) {
            active.leave();
            throw NodeInterrupt("join member approval", json{{"request", "approve"}});
        }
        // #397: the interrupt the Core raises for an effect whose outcome is
        // unknown, until the member is resumed.
        if (value == "reconcile" && !input.ctx.resume_value) {
            active.leave();
            throw NodeInterrupt("Mediated Tool effect requires reconciliation",
                                json{{"kind", "tool_effect_reconciliation"},
                                     {"thread_id", input.ctx.thread_id},
                                     {"task_id", "join-member"}});
        }
        // #397: fails after a short cancellable wait, so that a member that
        // pauses at once has paused first.
        if (value == "fail-slow") {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(300));
            asio::error_code ended;
            co_await timer.async_wait(asio::bind_cancellation_slot(
                input.ctx.cancel_token->slot(), asio::redirect_error(asio::use_awaitable, ended)));
            if (ended) {
                active.leave();
                throw asio::system_error(ended);
            }
            active.leave();
            throw std::runtime_error("join member failure sentinel");
        }
        // #391: an answered "ask-slow" takes 1.2 s before it finishes.
        if (value == "slow" || value == "ask-slow") {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(value == "slow" ? 300 : 1200));
            asio::error_code ended;
            co_await timer.async_wait(asio::bind_cancellation_slot(
                input.ctx.cancel_token->slot(), asio::redirect_error(asio::use_awaitable, ended)));
            if (ended) {
                active.leave();
                throw asio::system_error(ended);
            }
        }
        if (value == "block") std::this_thread::sleep_for(std::chrono::milliseconds(800));
        // #400: an answered "reconcile" settles its effect after a short
        // cancellable wait, and counts that it did.
        if (value == "reconcile") {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(200));
            asio::error_code ended;
            co_await timer.async_wait(asio::bind_cancellation_slot(
                input.ctx.cancel_token->slot(), asio::redirect_error(asio::use_awaitable, ended)));
            if (ended) {
                active.leave();
                throw asio::system_error(ended);
            }
            std::lock_guard lock(join_member_mutex);
            ++join_member_runs["reconcile:settled"];
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{
            "value", value == "ask" || value == "ask-slow" || value == "reconcile"
                         ? input.ctx.resume_value->at("decision")
                         : json("done:" + value)});
        active.leave();
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// A branch of a plan-level parallel (#384), driven by its "value" channel:
// "ask" interrupts until it is resumed, and "one" lets the first branch that
// runs finish while every other one asks. A resumed branch writes the decision
// it was given. Each counter counts one kind of run.
std::atomic<unsigned> parallel_branch_finished{0};
std::atomic<unsigned> parallel_branch_asked{0};
std::atomic<unsigned> parallel_branch_answered{0};
std::atomic<bool>     parallel_branch_first_taken{false};

void reset_parallel_branches() {
    parallel_branch_finished.store(0);
    parallel_branch_asked.store(0);
    parallel_branch_answered.store(0);
    parallel_branch_first_taken.store(false);
}

class ParallelBranchNode final : public GraphNode {
public:
    explicit ParallelBranchNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        NodeOutput output;
        if (input.ctx.resume_value) {
            parallel_branch_answered.fetch_add(1);
            output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->at("decision")});
            co_return output;
        }
        bool taken = false;
        if (input.state.get("value") == json("one") &&
            parallel_branch_first_taken.compare_exchange_strong(taken, true)) {
            parallel_branch_finished.fetch_add(1);
            output.writes.push_back(ChannelWrite{"value", json("done")});
            co_return output;
        }
        parallel_branch_asked.fetch_add(1);
        throw NodeInterrupt("parallel branch approval", json{{"request", "approve"}});
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// A Core subgraph whose one node is a JoinMemberNode, so a Core graph can run
// a join member one level down (#367, #368).
std::shared_ptr<GraphEngine> join_member_subgraph() {
    static const std::shared_ptr<GraphEngine> engine = [] {
        NodeFactory::instance().register_type(
            "runtime-test-join-member-child",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<JoinMemberNode>(name);
            },
            json{{"type", "object"}});
        const json definition{
            {"name", "join_member_child"},
            {"channels", json{{"value", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
            {"nodes", json{{"inner", json{{"type", "runtime-test-join-member-child"}}}}},
            {"edges", json::array({json{{"from", "__start__"}, {"to", "inner"}},
                                   json{{"from", "inner"}, {"to", "__end__"}}})}};
        return std::shared_ptr<GraphEngine>(
            GraphEngine::compile(definition, NodeContext{}).release());
    }();
    return engine;
}


// #386: the Core node of the plan resume tests. A run that resumes runs the
// armed resume hook (resumed_value_hook) and writes the decision it was
// given; every other run gets an ordinal (1, 2, ... over the runs that did not
// resume) and does what the test's script returns for it: {"write": value},
// {"ask": true}, {"fail": message} or {"effect": true}. Each run is recorded
// with its Core thread; two runs active on one thread at once set the overlap
// flag. With a delay, each run first sleeps, so concurrent runs overlap.
struct PlanStepRun {
    std::size_t ordinal = 0;
    std::string thread_id;
    bool        resumed = false;
};
std::mutex                                         plan_step_mutex;
std::vector<PlanStepRun>                           plan_step_runs;
std::set<std::string>                              plan_step_active_threads;
std::atomic<bool>                                  plan_step_overlap{false};
std::chrono::milliseconds                          plan_step_delay{0};
std::function<json(std::size_t, const NodeInput&)> plan_step_script;

void reset_plan_steps(std::function<json(std::size_t, const NodeInput&)> script,
                      std::chrono::milliseconds                          delay = {}) {
    std::lock_guard lock(plan_step_mutex);
    plan_step_runs.clear();
    plan_step_active_threads.clear();
    plan_step_overlap.store(false);
    plan_step_delay  = delay;
    plan_step_script = std::move(script);
}

std::vector<PlanStepRun> plan_steps() {
    std::lock_guard lock(plan_step_mutex);
    return plan_step_runs;
}

std::size_t plan_steps_counted(bool resumed) {
    const auto runs = plan_steps();
    return static_cast<std::size_t>(std::count_if(
        runs.begin(), runs.end(), [resumed](const PlanStepRun& run) { return run.resumed == resumed; }));
}

class PlanStepNode final : public GraphNode {
public:
    explicit PlanStepNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const bool                                         resumed = bool(input.ctx.resume_value);
        std::size_t                                        ordinal = 0;
        std::chrono::milliseconds                          delay{0};
        std::function<json(std::size_t, const NodeInput&)> script;
        {
            std::lock_guard lock(plan_step_mutex);
            if (!resumed) {
                ordinal = 1 + static_cast<std::size_t>(std::count_if(
                                  plan_step_runs.begin(), plan_step_runs.end(),
                                  [](const PlanStepRun& run) { return !run.resumed; }));
            }
            plan_step_runs.push_back(PlanStepRun{ordinal, input.ctx.thread_id, resumed});
            if (!plan_step_active_threads.insert(input.ctx.thread_id).second)
                plan_step_overlap.store(true);
            delay  = plan_step_delay;
            script = plan_step_script;
        }
        struct Leave {
            std::string thread;
            ~Leave() {
                std::lock_guard lock(plan_step_mutex);
                plan_step_active_threads.erase(thread);
            }
        } leave{input.ctx.thread_id};
        if (delay.count() > 0) {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(delay);
            co_await timer.async_wait(asio::use_awaitable);
        }
        NodeOutput output;
        if (resumed) {
            std::function<void()> hook;
            {
                std::lock_guard lock(resumed_value_hook_mutex);
                hook = std::exchange(resumed_value_hook, nullptr);
            }
            if (hook) hook();
            const auto& answer = *input.ctx.resume_value;
            output.writes.push_back(ChannelWrite{
                "value", answer.is_object() && answer.contains("decision") ? answer.at("decision")
                                                                           : answer});
            co_return output;
        }
        const auto action = script ? script(ordinal, input) : json{{"write", "done"}};
        // #397: waits first (cancellably), so that a sibling that pauses at
        // once has paused before this step acts.
        if (action.contains("delay_ms")) {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(action.at("delay_ms").get<int>()));
            co_await timer.async_wait(
                asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        }
        if (action.contains("ask"))
            throw NodeInterrupt("plan step approval", json{{"ordinal", ordinal}});
        // #397: an effect whose outcome is unknown.
        if (action.contains("reconcile"))
            throw NodeInterrupt("Mediated Tool effect requires reconciliation",
                                json{{"kind", "tool_effect_reconciliation"},
                                     {"thread_id", input.ctx.thread_id},
                                     {"task_id", "plan-step"}});
        if (action.contains("fail"))
            throw std::runtime_error(action.at("fail").get<std::string>());
        if (action.contains("effect"))
            throw NodeInterrupt(
                "capability result required",
                json{{"__neograph_program_pending_kind", "effect"},
                     {"call_id", "call-plan-effect-" + std::to_string(ordinal)},
                     {"result_schema", json{{"type", "object"}}},
                     {"effect", json{{"effect_id", "effect-plan-" + std::to_string(ordinal)},
                                     {"idempotency", "non_idempotent"},
                                     {"tool", "search"},
                                     {"arguments", json{{"query", "neo"}}}}},
                     {"expires_at_unix_ms", 4102444800000ULL}});
        if (action.contains("block")) {
            // Holds until the run cancels it, as a losing race branch.
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::seconds(5));
            co_await timer.async_wait(
                asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
            output.writes.push_back(ChannelWrite{"value", "unblocked"});
            co_return output;
        }
        output.writes.push_back(ChannelWrite{"value", action.at("write")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class BlockingNode final : public GraphNode {
public:
    explicit BlockingNode(std::string               name,
                          std::chrono::milliseconds duration = std::chrono::seconds(5))
        : name_(std::move(name)), duration_(duration) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++blocking_calls;
        const auto active = blocking_active.fetch_add(1, std::memory_order_relaxed) + 1;
        auto       peak   = blocking_peak.load(std::memory_order_relaxed);
        while (peak < active &&
               !blocking_peak.compare_exchange_weak(peak, active, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {}
        ActiveCallGuard active_guard(blocking_active);
        auto            timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(duration_);
        asio::error_code ended;
        co_await timer.async_wait(asio::bind_cancellation_slot(
            input.ctx.cancel_token->slot(), asio::redirect_error(asio::use_awaitable, ended)));
        // The body leaves the active count before it ends, cancelled or not,
        // and not in a destructor that runs while an exception leaves this
        // coroutine: a test that checks the run waited for the body must not
        // depend on when a compiler unwinds a coroutine frame.
        active_guard.leave();
        if (ended) throw asio::system_error(ended);
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string               name_;
    std::chrono::milliseconds duration_;
};
std::shared_future<void> typed_parent_release;

class TypedParentGate final {
public:
    TypedParentGate() { typed_parent_release = release_.get_future().share(); }
    ~TypedParentGate() {
        open();
        typed_parent_release = {};
    }
    void open() {
        if (!opened_) {
            release_.set_value();
            opened_ = true;
        }
    }
private:
    std::promise<void> release_;
    bool opened_ = false;
};

class TypedParentGateNode final : public GraphNode {
public:
    explicit TypedParentGateNode(std::string name)
        : name_(std::move(name)), release_(typed_parent_release) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        release_.wait();
        input.ctx.cancel_token->throw_if_cancelled("typed parent gate");
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::shared_future<void> release_;
};

class BarrierNode final : public GraphNode {
public:
    explicit BarrierNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++blocking_calls;
        const auto active = blocking_active.fetch_add(1, std::memory_order_relaxed) + 1;
        auto peak = blocking_peak.load(std::memory_order_relaxed);
        while (peak < active &&
               !blocking_peak.compare_exchange_weak(peak, active, std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {}
        if (active == 2 && overlap_ready) overlap_ready->set_value();
        ActiveCallGuard active_guard(blocking_active);
        if (overlap_barrier) overlap_barrier->arrive_and_wait();
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::seconds(5));
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};
class WindowedNode final : public GraphNode {
public:
    explicit WindowedNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto call   = window_calls.fetch_add(1) + 1;
        const auto active = window_active.fetch_add(1) + 1;
        auto       peak   = window_peak.load();
        while (peak < active && !window_peak.compare_exchange_weak(peak, active)) {}
        ActiveCallGuard active_guard(window_active);
        if (call <= 2 && window_first_barrier) window_first_barrier->arrive_and_wait();
        if (call == 3 && window_completed.load() == 0) ++window_second_started_before_completion;

        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        if (call <= 2) ++window_completed;
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class StubbornNode final : public GraphNode {
public:
    explicit StubbornNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++stubborn_calls;
        // Simulate a late sibling/provider budget signal. The enclosing
        // runtime must preserve an earlier user/timeout cancellation cause.
        if (input.ctx.budget_exhausted) {
            input.ctx.budget_exhausted->store(true, std::memory_order_release);
        }
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(100));
        co_await  timer.async_wait(asio::use_awaitable);
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};
class SchedulerBlockingNode final : public GraphNode {
public:
    explicit SchedulerBlockingNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++scheduler_blocking_calls;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class FollowupNode final : public GraphNode {
public:
    explicit FollowupNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++followup_calls;
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot runtime_registry(
    ExecutionGuarantee completed_guarantee = ExecutionGuarantee::Strict) {
    RegistrySnapshotBuilder builder;
    auto                    completed = manifest(ExecutableKind::Node, "runtime-completed", '1');
    completed.execution_guarantee     = completed_guarantee;
    builder.add_node(
        std::move(completed),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CompletedNode>(name);
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-mediated-dispatch", 'a'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MediatedDispatchNode>(name);
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-map-echo", 'e'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MapEchoNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-interrupt", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<InterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-fault-interrupt", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FaultInterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-always-interrupt", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<AlwaysInterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-effect-interrupt", 'a'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EffectInterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-effect-nonidempotent", 'c'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EffectInterruptNode>(name, true);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-failing", '4'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FailingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    for (const auto& [type, kind] :
         {std::pair{"runtime-classified-ordinary", ClassifiedFailureNode::Kind::Ordinary},
          std::pair{"runtime-classified-logic", ClassifiedFailureNode::Kind::Logic},
          std::pair{"runtime-classified-diagnostic", ClassifiedFailureNode::Kind::Diagnostic},
          std::pair{"runtime-classified-once", ClassifiedFailureNode::Kind::FirstCallOrdinary}}) {
        builder.add_node(
            manifest(ExecutableKind::Node, type, '9'),
            [kind = kind](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<ClassifiedFailureNode>(name, kind);
            },
            json{{"type", "object"}}, json::object());
    }
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-failing-non-utf8", '5'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FailingNode>(name, std::string("\xc7\xf6\xc0\xe7", 4));
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-failing-system-error", '6'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FailingSystemErrorNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-malformed-event-then-fail", '7'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MalformedEventThenFailNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-map-fail-first", 'd'),
        [](const std::string& name, const json& config, const NodeContext&) {
            return std::make_unique<MapFailFirstNode>(name, config.value("complete_first", false));
        },
        json{{"type", "object"}}, json::object());

    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-resume-hook", 'c'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ResumeHookNode>(name, false);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-resume-hook-effect", 'c'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ResumeHookNode>(name, true);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-value-gated", 'e'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ValueGatedNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-join-member", 'e'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<JoinMemberNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-parallel-branch", 'f'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ParallelBranchNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-subgraph-member", 'e'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(name, join_member_subgraph());
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-plan-step", 'a'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<PlanStepNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-blocking", '5'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BlockingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-typed-parent-gate", 'd'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<TypedParentGateNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-barrier", 'f'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BarrierNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-windowed", '0'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WindowedNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-short-blocking", 'b'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BlockingNode>(name, std::chrono::milliseconds(500));
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-stubborn", '7'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StubbornNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-scheduler-blocking", '8'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SchedulerBlockingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-named-ask", 'b'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<NamedAskNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-followup", '6'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FollowupNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-overwrite", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-alternate", '9'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}


json two_interrupt_program_document() {
    auto document                  = program_document("runtime-interrupt");
    auto definition                = document["root"]["definition"];
    definition["nodes"]["second"]  = json{{"type", "runtime-always-interrupt"}};
    definition["edges"]            = json::array({
        json{{"from", "__start__"}, {"to", "work"}},
        json{{"from", "work"}, {"to", "second"}},
        json{{"from", "second"}, {"to", "__end__"}},
    });
    document["root"]["definition"] = std::move(definition);
    return document;
}

json parallel_map_child_document(std::string node_type) {
    auto document                                          = program_document(std::move(node_type));
    document["declared_budget_requirements"][0]["maximum"] = 3333;
    document["declared_budget_requirements"][1]["maximum"] = 333;
    document["declared_budget_requirements"][2]["maximum"] = 333;
    document["declared_budget_requirements"][5]["maximum"] = 6;
    return document;
}
json parallel_map_echo_child_document() {
    auto document = parallel_map_child_document("runtime-map-echo");
    document["root"]["definition"]["channels"]["item"] =
        json{{"reducer", "runtime-overwrite"}, {"initial", json::object()}};
    return document;
}
json failing_fanout_program_document(std::string completed_name = "completed",
                                     std::string failing_name   = "failing") {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{completed_name, json{{"type", "runtime-completed"}}},
                       {failing_name, json{{"type", "runtime-failing"}}}}},
        {"edges", json::array({
                      json{{"from", "__start__"}, {"to", completed_name}},
                      json{{"from", "__start__"}, {"to", failing_name}},
                      json{{"from", completed_name}, {"to", "__end__"}},
                      json{{"from", failing_name}, {"to", "__end__"}},
                  })},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

json step_limited_program_document() {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"work", json{{"type", "runtime-completed"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                               json{{"from", "work"}, {"to", "work"}}})},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

// #443: a Core graph of two nodes, the second of which runs a subgraph. The
// subgraph's step counts toward the run's Core steps, so the run takes three
// steps although the outer graph needs two.
json subgraph_step_program_document() {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"first", json{{"type", "runtime-completed"}}},
                       {"work", json{{"type", "runtime-subgraph-member"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "first"}},
                               json{{"from", "first"}, {"to", "work"}},
                               json{{"from", "work"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

class LifecycleHookTool final : public Tool {
public:
    explicit LifecycleHookTool(std::string value, bool block = false)
        : name(std::move(value)), block_terminal(block) {}
    ChatTool get_definition() const override { return {name, "", json::object()}; }
    std::string get_name() const override { return name; }
    std::string execute(const json&) override {
        ++calls;
        if (block_terminal) throw std::runtime_error("terminal hook blocked");
        return "ok";
    }
    std::string name;
    std::atomic<unsigned> calls{0};
    bool block_terminal = false;
};

std::shared_ptr<HookRuntime> lifecycle_hooks(LifecycleHookTool& admitted,
                                             LifecycleHookTool& before_terminal,
                                             LifecycleHookTool& failed) {
    HookTargetResolver resolver = [&admitted, &before_terminal, &failed](std::string_view target)
        -> std::optional<HookTargetContract> {
        LifecycleHookTool* tool = target == admitted.name ? &admitted
                                : target == before_terminal.name ? &before_terminal
                                : target == failed.name ? &failed : nullptr;
        return tool ? std::optional<HookTargetContract>{
                          HookTargetContract{tool, {}, ToolEffectClass::ReadOnly}}
                    : std::nullopt;
    };
    auto registry = std::make_shared<HookRegistry>(resolver);
    for (const auto& [phase, target] : std::vector<std::pair<HookPhase, std::string>>{
             {HookPhase::MessageAdmitted, admitted.name},
             {HookPhase::BeforeTerminalPublication, before_terminal.name},
             {HookPhase::RunFailed, failed.name}}) {
        HookDefinitionData definition;
        definition.phase = phase;
        definition.target_id = target;
        definition.effect = ToolEffectClass::ReadOnly;
        definition.input_mapper = {HookInputMapperKind::Template, {}, json::object(), {}};
        registry->admit(HookDefinition::create(std::move(definition)));
    }
    auto adapter = std::make_shared<NativeHookExecutionAdapter>(resolver,
        std::make_shared<ToolExecutionController>());
    return std::make_shared<HookRuntime>(std::make_shared<MandatoryHookRunner>(
        std::make_shared<InMemoryHookJournal>(), std::move(registry), std::move(adapter)));
}

using AdmittedRuntime = BasicAdmittedRuntime<runtime_registry>;
class DurableInstructionContextStore final : public neograph::DurableContextStore {
public:
    neograph::ContextStoreAppendResult append_history(
        const neograph::ContextStoreFeed& feed,
        const neograph::RuntimeHistoryRecord& record,
        const std::optional<std::string>& expected) override {
        return inner.append_history(feed, record, expected);
    }
    neograph::ContextStoreHead history_head(
        const neograph::ContextStoreFeed& feed) const override {
        return inner.history_head(feed);
    }
    neograph::ContextHistoryRange snapshot_history(
        const neograph::ContextStoreFeed& feed, std::uint64_t from,
        std::uint64_t through) const override {
        return inner.snapshot_history(feed, from, through);
    }
    std::string hydrate_history(
        const neograph::ContextHistoryRange& range) const override {
        return inner.hydrate_history(range);
    }
    neograph::ContextArtifactPutResult put_artifact(
        std::string_view owner,
        const neograph::ContextArtifact& artifact) override {
        return inner.put_artifact(owner, artifact);
    }
    std::optional<neograph::ContextArtifact> get_artifact(
        std::string_view owner, std::string_view id) const override {
        return inner.get_artifact(owner, id);
    }
private:
    neograph::InMemoryContextStore inner;
};

LinkedChildAdmission make_linked_child(AdmittedRuntime& fixture,
                                       std::string      parent_node_type = "runtime-blocking",
                                       std::string      child_node_type  = "runtime-blocking") {
    auto parent_document                      = program_document(std::move(parent_node_type));
    parent_document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 1;
    return link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                               fixture.admit(std::move(child_node_type)));
}
LinkedChildAdmission make_recursive_linked_child(AdmittedRuntime& fixture) {
    auto parent_document                      = program_document("runtime-blocking");
    parent_document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    parent_document["declared_budget_requirements"][3]["maximum"] = 2;
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 4;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 4;

    auto child_document = program_document("runtime-blocking");
    child_document["declared_budget_requirements"][7]["minimum"] = 0;
    child_document["declared_budget_requirements"][7]["maximum"] = 4;
    child_document["declared_budget_requirements"][8]["minimum"] = 0;
    child_document["declared_budget_requirements"][8]["maximum"] = 4;
    auto linked = link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                                      fixture.admit_document(std::move(child_document)),
                                      BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 4, 4});
    fixture.bind_child(linked.child_version, "child",
                       ProgramRuntimeChildBinding{linked.receipt, linked.child_version});
    return linked;
}

LinkedChildAdmission make_durable_spawn_child(
    AdmittedRuntime&             fixture,
    std::string                  parent_node_type   = "runtime-completed",
    std::string                  child_node_type    = "runtime-completed",
    std::optional<std::uint64_t> timeout_ms         = std::nullopt,
    ExecutionGuarantee           accepted_guarantee = ExecutionGuarantee::Strict) {
    json await = json{{"op", "await"}, {"body", json{{"op", "spawn"}, {"child_binding", "child"}}}};
    if (timeout_ms) await["timeout_ms"] = *timeout_ms;
    auto parent_document = orchestration_document(std::move(await), std::move(parent_node_type));
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 1;
    return link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                               fixture.admit(std::move(child_node_type)),
                               BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                               accepted_guarantee);
}

json parallel_map_document(json          item_source,
                           std::uint64_t max_items,
                           std::string   failure_policy   = "fail_fast",
                           std::uint64_t max_output_bytes = 65536) {
    auto document                      = program_document("runtime-completed");
    document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V3;
    const auto definition              = document["root"]["definition"];
    document["root"]                   = json{
                          {"op", "parallel_map"},
                          {"name", "main"},
                          {"definition", definition},
                          {"item_source", std::move(item_source)},
                          {"child_binding", "child"},
                          {"input_binding", json{{"from", json{{"field", ""}}}, {"to", json{{"field", "/item"}}}}},
                          {"output_binding", json{{"from", json{{"field", "/channels/value/value"}}}}},
                          {"max_items", max_items},
                          {"max_in_flight", std::min<std::uint64_t>(2, max_items)},
                          {"max_output_bytes", max_output_bytes},
                          {"failure_policy", std::move(failure_policy)}};
    document["declared_budget_requirements"][3]["minimum"] = std::min<std::uint64_t>(2, max_items);
    document["declared_budget_requirements"][3]["maximum"] = std::min<std::uint64_t>(2, max_items);
    document["declared_budget_requirements"][7]["minimum"] = 1;
    document["declared_budget_requirements"][7]["maximum"] = 1;
    document["declared_budget_requirements"][8]["minimum"] = max_items;
    document["declared_budget_requirements"][8]["maximum"] = max_items;
    return document;
}

RunBudget grant() {
    return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0};
}
std::string pending_call_id(const ProgramResult& result) {
    const auto interrupt = result.interrupt();
    if (!interrupt) throw std::invalid_argument("Program result is not interrupted");
    if (interrupt->pending_input) return interrupt->pending_input->call_id();
    if (interrupt->pending_effect) return interrupt->pending_effect->call_id();
    throw std::invalid_argument("Interrupted Program result has no pending call");
}

ProgramResume resume_for(const ProgramResult&              result,
                         json                              value,
                         std::string                       trace_id,
                         std::shared_ptr<ProgramEventSink> events = {}) {
    return ProgramResume{std::move(value), std::move(trace_id), std::move(events),
                         pending_call_id(result)};
}

class ThrowingSink final : public ProgramEventSink {
public:
    explicit ThrowingSink(ProgramEventKind fail_on) : fail_on_(fail_on) {}

    void on_event(const ProgramEvent& event) override {
        seen.push_back(event.kind);
        if (event.kind == fail_on_) {
            throw std::runtime_error("sink rejected event");
        }
    }

    std::vector<ProgramEventKind> seen;

private:
    ProgramEventKind fail_on_;
};
class CountingSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent&) override { calls.fetch_add(1); }

    std::atomic<unsigned> calls{0};
};

class CallbackSink final : public ProgramEventSink {
public:
    void set_callback(std::function<void(const ProgramEvent&)> callback) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(callback);
    }

    void on_event(const ProgramEvent& event) override {
        std::function<void(const ProgramEvent&)> callback;
        {
            std::lock_guard lock(mutex_);
            callback = callback_;
        }
        if (callback) callback(event);
    }

private:
    std::mutex                               mutex_;
    std::function<void(const ProgramEvent&)> callback_;
};

class JournalObservingSink final : public ProgramEventSink {
public:
    explicit JournalObservingSink(std::shared_ptr<ProgramTransitionStore> journal)
        : journal_(std::move(journal)) {}

    void on_event(const ProgramEvent& event) override {
        if (event.kind != ProgramEventKind::CheckpointPublished &&
            event.kind != ProgramEventKind::Terminal) {
            return;
        }
        const auto latest    = journal_->latest("tenant:runtime", event.run_id);
        const bool committed = latest &&
                               latest->continuation.state == ContinuationState::Completed &&
                               latest->core_checkpoint.has_value();
        if (event.kind == ProgramEventKind::CheckpointPublished) {
            checkpoint_observed_committed.store(committed);
        } else {
            terminal_observed_committed.store(committed);
        }
    }

    std::atomic<bool> checkpoint_observed_committed{false};
    std::atomic<bool> terminal_observed_committed{false};

private:
    TransitionStoreHandle journal_;
};

class ConflictOnceJournal final : public SyncProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load_sync(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load_sync(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest_sync(owner, run_id);
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events_sync(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects_sync(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands_sync(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_.load_context_publications_sync(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_hook_outbox_entries_sync(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_sync(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_.load_lineage_sync(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage_sync(std::string_view owner,
                                                       std::string_view run_id) const override {
        return inner_.load_run_lineage_sync(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head_sync(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_.load_lineage_head_sync(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation_sync(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return inner_.load_generation_sync(owner, lineage_id, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule_sync(
        std::string_view owner,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override {
        return inner_.load_graph_migration_capsule_sync(
            owner, source_run_id, source_lineage_head_id);
    }
    std::optional<ProgramExecutionLease> load_execution_lease_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_execution_lease_sync(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish_sync(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        if (publication.journal_record.continuation.state != ContinuationState::Running &&
            !injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return inner_.compare_publish_sync(owner, expected, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        if (publication.journal_record.continuation.state != ContinuationState::Running &&
            !injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return inner_.compare_publish_execution_sync(
            owner, expected, std::move(publication), expected_lease, next_lease);
    }
    bool injected() const noexcept { return injected_.load(); }

private:
    InMemoryProgramTransitionStore inner_;
    std::atomic<bool>              injected_{false};
};

class IsolatedProcessTransitionStore final : public SyncProgramTransitionStore {
public:
    explicit IsolatedProcessTransitionStore(
        std::shared_ptr<ProgramTransitionStore> target)
        : target_(std::make_shared<BlockingProgramTransitionStore>(std::move(target))) {}

    std::optional<ProgramRunRecord> load_sync(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest_sync(
        std::string_view owner, std::string_view run_id) const override {
        return target_->latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return target_->load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return target_->load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return target_->load_javascript_commands(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        if (recovery_run_id_ == run_id) {
            std::vector<ProgramContextPublication> result;
            for (const auto& context : recovery_contexts_) {
                if (context.epoch.sequence() > sequence) result.push_back(context);
            }
            return result;
        }
        return target_->load_context_publications(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner, std::string_view run_id) const override {
        if (recovery_run_id_ == run_id) return recovery_hooks_;
        return target_->load_hook_outbox_entries(owner, run_id);
    }
    std::optional<MigrationPlan> load_migration_plan_sync(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load_migration_plan(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_sync(
        std::string_view owner, std::string_view lineage_id) const override {
        return target_->load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage_sync(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load_run_lineage(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head_sync(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return target_->load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation_sync(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return target_->load_generation(owner, lineage_id, generation);
    }
    std::optional<ProgramTransitionPublication> load_generation_initial_publication_sync(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return target_->load_generation_initial_publication(
            owner, lineage_id, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule_sync(
        std::string_view owner, std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override {
        return target_->load_graph_migration_capsule(
            owner, source_run_id, source_lineage_head_id);
    }
    std::optional<ProgramExecutionLease> load_execution_lease_sync(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load_execution_lease(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish_sync(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication) override {
        return target_->compare_publish(owner, expected, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        const bool terminal_release = expected_lease && !next_lease &&
            publication.run_record.continuation().state != ContinuationState::Running;
        if (terminal_release) {
            std::unique_lock lock(execution_release_mutex_);
            if (block_execution_release_) {
                execution_release_observed_ = true;
                execution_release_condition_.notify_all();
                execution_release_condition_.wait_for(
                    lock, std::chrono::seconds(5),
                    [this] { return execution_release_released_; });
            }
        }
        return target_->compare_publish_execution(
            owner, expected, std::move(publication), expected_lease, next_lease);
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point_sync(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) override {
        return target_->compare_publish_graph_safe_point(
            owner, expected, std::move(publication), evidence, capsule,
            execution_lease);
    }
    void block_execution_release() {
        std::lock_guard lock(execution_release_mutex_);
        block_execution_release_ = true;
        execution_release_observed_ = false;
        execution_release_released_ = false;
    }
    bool wait_for_execution_release(std::chrono::milliseconds timeout) {
        std::unique_lock lock(execution_release_mutex_);
        return execution_release_condition_.wait_for(
            lock, timeout, [this] { return execution_release_observed_; });
    }
    void release_execution_release() {
        std::lock_guard lock(execution_release_mutex_);
        execution_release_released_ = true;
        execution_release_condition_.notify_all();
    }
    void set_runtime_recovery_state(
        std::string run_id, std::vector<ProgramContextPublication> contexts,
        std::vector<neograph::HookOutboxEntry> hooks) {
        recovery_run_id_ = std::move(run_id);
        recovery_contexts_ = std::move(contexts);
        recovery_hooks_ = std::move(hooks);
    }

private:
    std::shared_ptr<BlockingProgramTransitionStore> target_;
    std::mutex execution_release_mutex_;
    std::condition_variable execution_release_condition_;
    bool block_execution_release_ = false;
    bool execution_release_observed_ = false;
    bool execution_release_released_ = false;
    std::string recovery_run_id_;
    std::vector<ProgramContextPublication> recovery_contexts_;
    std::vector<neograph::HookOutboxEntry> recovery_hooks_;
};

ProgramContextPublication recovery_context(std::string run_id) {
    neograph::ContextEpochData epoch_data;
    epoch_data.run_id = std::move(run_id);
    epoch_data.sequence = 1;
    epoch_data.raw_window_digest = digest('9');
    epoch_data.guarantee_profile = neograph::RuntimeGuaranteeProfile::Recorded;
    auto epoch = neograph::ContextEpoch::create(std::move(epoch_data));
    neograph::ContextAssemblyReceiptData receipt_data;
    receipt_data.context_epoch_id = epoch.id();
    receipt_data.normalized_request_digest = digest('a');
    receipt_data.message_window_digest = digest('b');
    auto receipt = neograph::ContextAssemblyReceipt::create(
        std::move(receipt_data), epoch, {});
    return {std::move(epoch), {}, std::move(receipt)};
}

neograph::HookOutboxEntry recovery_hook(std::string run_id) {
    const auto event = neograph::RuntimeEvent::create(
        {{}, 1, HookPhase::BeforeToolExecution, "tool_execution",
         "tenant:runtime", std::move(run_id), json::object()});
    const auto invocation = neograph::HookInvocation::create(
        {{}, digest('f'), event.id(), "audit", HookPhase::BeforeToolExecution,
         neograph::HookDelivery::BlockingMandatory, neograph::HookFailureMode::FailClosed,
         neograph::HookIdempotency::Idempotent, ToolEffectClass::ReadOnly,
         {}, {}, json::object()});
    return neograph::HookOutboxEntry::create(
        {invocation, event, neograph::HookExecutionState::Pending, 0, 1,
         std::chrono::system_clock::now() + std::chrono::minutes(1)});
}

class BlockAfterJavaScriptResultJournal final : public SyncProgramTransitionStore {
public:
    explicit BlockAfterJavaScriptResultJournal(std::shared_ptr<ProgramTransitionStore> inner = {})
        : inner_(std::make_shared<BlockingProgramTransitionStore>(
              inner ? std::move(inner) : std::make_shared<InMemoryProgramTransitionStore>())) {}
    void reopen_backend_for_testing(std::shared_ptr<ProgramTransitionStore> backend) {
        inner_ = std::make_shared<BlockingProgramTransitionStore>(std::move(backend));
    }
    std::vector<ProgramChildSynthesisRecord> load_child_syntheses_sync(
        std::string_view owner, std::string_view run) const override {
        auto remaining = synthesis_read_failures.load();
        while (remaining &&
               !synthesis_read_failures.compare_exchange_weak(remaining, remaining - 1)) {}
        if (remaining) throw std::runtime_error("simulated synthesis journal outage");
        return inner_->load_child_syntheses(owner, run);
    }
    std::vector<ProgramTransitionPublication> synthesis_publications;
    std::mutex                                               synthesis_observation_mutex;
    std::optional<ProgramChildSynthesisState> fail_synthesis_after;
    std::function<void(const ProgramTransitionPublication&)> after_publication;
    std::function<void(const ProgramTransitionPublication&)> before_publication;
    std::function<std::optional<ProgramRunRecord>(std::string_view,
                                                 std::optional<ProgramRunRecord>)> filter_run_read;
    std::function<std::optional<ProgramRunLineage>(
        std::string_view, const std::function<std::optional<ProgramRunLineage>()>&)>
        filter_run_lineage_read;
    std::function<std::vector<ProgramJavaScriptCommandJournalEntry>(
        std::string_view, std::vector<ProgramJavaScriptCommandJournalEntry>)> filter_command_read;
    mutable std::atomic<unsigned>                            synthesis_read_failures{0};
    mutable std::atomic<unsigned>                            command_head_misses{0};
    bool javascript_reads_unavailable = false;
    mutable std::atomic<unsigned> javascript_read_attempts{0};
    std::optional<ProgramRunPublicationHead> load_run_publication_head_sync(
        std::string_view owner, std::string_view run) const override {
        return filter_run_read
            ? SyncProgramTransitionStore::load_run_publication_head_sync(owner, run)
            : inner_->load_run_publication_head(owner, run);
    }
    std::optional<ProgramCommandPublicationHead> load_command_publication_head_sync(
        std::string_view owner, std::string_view run) const override {
        auto remaining = command_head_misses.load();
        while (remaining &&
               !command_head_misses.compare_exchange_weak(remaining, remaining - 1)) {}
        if (remaining) return std::nullopt;
        // Forward native snapshots except when an adversarial read filter needs
        // to observe the individual reads of the compatibility implementation.
        return filter_run_read || filter_command_read || javascript_reads_unavailable
            ? SyncProgramTransitionStore::load_command_publication_head_sync(owner, run)
            : inner_->load_command_publication_head(owner, run);
    }
    std::optional<ProgramRunRecord> load_sync(std::string_view owner,
                                         std::string_view run_id) const override {
        if (throw_next_load_.exchange(false)) {
            throw std::runtime_error("simulated Program run read failure");
        }
        auto record = inner_->load(owner, run_id);
        return filter_run_read ? filter_run_read(run_id, std::move(record)) : std::move(record);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_->latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_->load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_->load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        if (javascript_reads_unavailable) {
            javascript_read_attempts.fetch_add(1);
            return SyncProgramTransitionStore::load_javascript_commands_sync(owner, run_id, sequence);
        }
        auto commands = inner_->load_javascript_commands(owner, run_id, sequence);
        return filter_command_read ? filter_command_read(run_id, std::move(commands))
                                   : std::move(commands);
    }
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_->load_context_publications(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_->load_hook_outbox_entries(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_sync(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_->load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage_sync(std::string_view owner,
                                                       std::string_view run_id) const override {
        if (consume_replacement_readback_failure()) {
            throw std::runtime_error("simulated replacement lineage readback failure");
        }
        const auto read = [&] { return inner_->load_run_lineage(owner, run_id); };
        return filter_run_lineage_read ? filter_run_lineage_read(run_id, read) : read();
    }
    std::optional<ProgramRunLineage> load_lineage_head_sync(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_->load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation_sync(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        if (consume_replacement_readback_failure()) {
            throw std::runtime_error("simulated replacement generation readback failure");
        }
        return inner_->load_generation(owner, lineage_id, generation);
    }
    std::optional<ProgramTransitionPublication> load_generation_initial_publication_sync(
        std::string_view owner, std::string_view lineage, std::uint64_t generation) const override {
        return inner_->load_generation_initial_publication(owner, lineage, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule_sync(
        std::string_view owner,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override {
        return inner_->load_graph_migration_capsule(owner, source_run_id, source_lineage_head_id);
    }
    std::optional<ProgramExecutionLease> load_execution_lease_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_->load_execution_lease(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish_sync(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        if (before_publication) before_publication(publication);
        const bool command_result =
            !publication.commands.empty() && publication.commands.back().completed();
        const bool replacement =
            publication.run_generation && publication.run_generation->replacement_receipt();
        if (replacement && throw_before_replacement_.exchange(false)) {
            std::function<void()> publish_collision;
            {
                std::lock_guard lock(mutex_);
                publish_collision = std::move(publish_collision_);
            }
            if (publish_collision) publish_collision();
            throw std::runtime_error("simulated replacement failure before publication");
        }
        const auto synthesis = publication.child_synthesis_records;
        if (!synthesis.empty()) {
            std::lock_guard lock(synthesis_observation_mutex);
            synthesis_publications.push_back(publication);
        }
        const auto observed  = after_publication
                                   ? std::optional<ProgramTransitionPublication>(publication)
                                   : std::nullopt;
        const auto published = inner_->compare_publish(owner, expected, std::move(publication));
        if (observed && published == ProgramTransitionPublishResult::Published)
            after_publication(*observed);
        if (!synthesis.empty() && fail_synthesis_after == synthesis.front().data().state &&
            published == ProgramTransitionPublishResult::Published) {
            fail_synthesis_after.reset();
            throw std::runtime_error("simulated loss after durable synthesis stage");
        }
        if (replacement && published == ProgramTransitionPublishResult::Published &&
            crash_after_replacement_.exchange(false)) {
            if (unreadable_replacement_commit_.exchange(false)) {
                replacement_readback_failures_.store(2);
            }
            throw std::runtime_error("simulated crash after replacement publication");
        }
        if (!command_result || published != ProgramTransitionPublishResult::Published) {
            return published;
        }
        std::unique_lock lock(mutex_);
        observed_ = true;
        condition_.notify_all();
        condition_.wait_for(lock, std::chrono::seconds(5), [this] { return released_; });
        return published;
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        if (before_publication) before_publication(publication);
        const bool replacement =
            publication.run_generation && publication.run_generation->replacement_receipt();
        if (replacement && throw_before_replacement_.exchange(false)) {
            throw std::runtime_error("simulated replacement failure before publication");
        }
        const auto observed  = after_publication
                                   ? std::optional<ProgramTransitionPublication>(publication)
                                   : std::nullopt;
        const auto published = inner_->compare_publish_execution(
            owner, expected, std::move(publication), expected_lease, next_lease);
        if (observed && published == ProgramTransitionPublishResult::Published)
            after_publication(*observed);
        if (replacement && published == ProgramTransitionPublishResult::Published &&
            crash_after_replacement_.exchange(false)) {
            if (unreadable_replacement_commit_.exchange(false)) {
                replacement_readback_failures_.store(2);
            }
            throw std::runtime_error("simulated crash after replacement publication");
        }
        return published;
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point_sync(
        std::string_view owner,
        std::string_view expected,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) override {
        const auto published = inner_->compare_publish_graph_safe_point(
            owner, expected, std::move(publication), evidence, capsule, execution_lease);
        if (published == ProgramTransitionPublishResult::Published &&
            crash_after_graph_safe_point_.exchange(false)) {
            if (unreadable_graph_safe_point_commit_.exchange(false)) {
                throw_next_load_.store(true);
            }
            throw std::runtime_error("simulated crash after graph safe-point publication");
        }
        return published;
    }
    bool wait_for_result(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return observed_; });
    }
    void release_result() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }
    void crash_after_next_replacement() { crash_after_replacement_.store(true); }
    void crash_after_next_replacement_with_unreadable_commit() {
        unreadable_replacement_commit_.store(true);
        crash_after_replacement_.store(true);
    }
    void crash_after_next_graph_safe_point_with_unreadable_commit() {
        unreadable_graph_safe_point_commit_.store(true);
        crash_after_graph_safe_point_.store(true);
    }
    void throw_on_next_load() { throw_next_load_.store(true); }
    void throw_before_next_replacement_with_occupied_target(
        std::function<void()> publish_collision) {
        {
            std::lock_guard lock(mutex_);
            publish_collision_ = std::move(publish_collision);
        }
        throw_before_replacement_.store(true);
    }

private:
    bool consume_replacement_readback_failure() const noexcept {
        auto remaining = replacement_readback_failures_.load();
        while (remaining > 0) {
            if (replacement_readback_failures_.compare_exchange_weak(remaining, remaining - 1)) {
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<BlockingProgramTransitionStore> inner_;
    mutable std::mutex             mutex_;
    std::condition_variable        condition_;
    bool                           observed_ = false;
    bool                           released_ = false;
    std::atomic<bool>              crash_after_replacement_{false};
    std::atomic<bool>              unreadable_replacement_commit_{false};
    std::atomic<bool>              crash_after_graph_safe_point_{false};
    std::atomic<bool>              unreadable_graph_safe_point_commit_{false};
    mutable std::atomic<int>       replacement_readback_failures_{0};
    std::atomic<bool>              throw_before_replacement_{false};
    mutable std::atomic<bool>      throw_next_load_{false};
    std::function<void()>          publish_collision_;
};

class FailChildDispatchOnceJournal final : public SyncProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load_sync(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load_sync(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest_sync(owner, run_id);
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events_sync(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects_sync(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands_sync(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_.load_context_publications_sync(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_hook_outbox_entries_sync(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_sync(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_.load_lineage_sync(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage_sync(std::string_view owner,
                                                       std::string_view run_id) const override {
        return inner_.load_run_lineage_sync(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head_sync(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_.load_lineage_head_sync(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation_sync(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return inner_.load_generation_sync(owner, lineage_id, generation);
    }
    std::optional<ProgramExecutionLease> load_execution_lease_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_execution_lease_sync(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish_sync(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        const auto children = publication.run_record.children();
        if (children.empty() || children.back().state != ProgramChildState::Dispatched ||
            !block_dispatch_.load()) {
            return inner_.compare_publish_sync(owner, expected, std::move(publication));
        }
        if (!injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return ProgramTransitionPublishResult::Conflict;
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        return inner_.compare_publish_execution_sync(
            owner, expected, std::move(publication), std::move(expected_lease),
            std::move(next_lease));
    }
    void allow_dispatch() noexcept { block_dispatch_.store(false); }

private:
    InMemoryProgramTransitionStore inner_;
    std::atomic<bool>              injected_{false};
    std::atomic<bool>              block_dispatch_{true};
};
class ThrowOnLatestJournal final : public SyncProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load_sync(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load_sync(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view, std::string_view) const override {
        throw std::runtime_error("journal latest failed");
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events_sync(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects_sync(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands_sync(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_.load_context_publications_sync(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_hook_outbox_entries_sync(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_sync(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_.load_lineage_sync(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage_sync(std::string_view owner,
                                                       std::string_view run_id) const override {
        return inner_.load_run_lineage_sync(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head_sync(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_.load_lineage_head_sync(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation_sync(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return inner_.load_generation_sync(owner, lineage_id, generation);
    }
    std::optional<ProgramExecutionLease> load_execution_lease_sync(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_execution_lease_sync(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish_sync(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        return inner_.compare_publish_sync(owner, expected, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        return inner_.compare_publish_execution_sync(
            owner, expected, std::move(publication), std::move(expected_lease),
            std::move(next_lease));
    }
    std::optional<ProgramJournalRecord> stored_latest(std::string_view run_id) const {
        return inner_.latest_sync("tenant:runtime", run_id);
    }

private:
    InMemoryProgramTransitionStore inner_;
};

class AdversarialCheckpointStore final : public CheckpointStore {
public:
    enum class Mode {
        Normal,
        WrongThread,
        WrongThreadAfterPrecheck,
        WrongSchemaAfterPrecheck,
        WrongSchemaAtCoreLoad,
        WrongIdAtCoreLoad,
        MissingAfterFirstExactLoad,
        MissingAfterCoreLoad,
        FailMigrationTargetSave,
        WrongMigrationContentAtConsumingLoad,
        DelayMigrationTargetSave,
        FailTerminalSave
    };

    void arm(Mode mode) {
        mode_.store(mode);
        exact_loads_.store(0);
        migration_target_save_failures_.store(0);
        migration_target_loads_.store(0);
        migration_target_save_started_.store(false);
    }
    std::uint32_t exact_loads() const noexcept { return exact_loads_.load(); }
    std::uint32_t migration_target_save_failures() const noexcept {
        return migration_target_save_failures_.load();
    }
    std::uint32_t terminal_save_failures() const noexcept {
        return terminal_save_failures_.load();
    }
    bool migration_target_save_started() const noexcept {
        return migration_target_save_started_.load();
    }

    void save(const Checkpoint& checkpoint) override {
        if (mode_.load() == Mode::FailTerminalSave && checkpoint.metadata.is_object() &&
            checkpoint.metadata.contains("neograph_program")) {
            terminal_save_failures_.fetch_add(1);
            throw 7;  // not derived from std::exception
        }
        if (mode_.load() == Mode::DelayMigrationTargetSave &&
            checkpoint.metadata.is_object() &&
            checkpoint.metadata.contains("migrated_from")) {
            migration_target_save_started_.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (mode_.load() == Mode::FailMigrationTargetSave &&
            checkpoint.metadata.is_object() &&
            checkpoint.metadata.contains("migrated_from")) {
            migration_target_save_failures_.fetch_add(1);
            throw std::runtime_error("injected migration target save failure");
        }
        inner_->save(checkpoint);
    }

    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        return inner_->load_latest(thread_id);
    }

    bool requires_managed_budget(const std::string& thread_id) override {
        return inner_->requires_managed_budget(thread_id);
    }
    asio::awaitable<bool> requires_managed_budget_async(std::string thread_id) override {
        co_return co_await inner_->requires_managed_budget_async(std::move(thread_id));
    }

    bool retains_native_checkpoint() const noexcept override {
        return inner_->retains_native_checkpoint();
    }

    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        auto       checkpoint = inner_->load_by_id(id);
        const auto call       = exact_loads_.fetch_add(1);
        const auto mode       = mode_.load();
        if (checkpoint && checkpoint->metadata.is_object() &&
            checkpoint->metadata.contains("migrated_from")) {
            const auto migration_call = migration_target_loads_.fetch_add(1);
            if (mode == Mode::WrongMigrationContentAtConsumingLoad && migration_call >= 3) {
                checkpoint->channel_values["channels"]["value"]["value"] = "tampered";
            }
        }
        if (mode == Mode::MissingAfterFirstExactLoad && call > 0) {
            return std::nullopt;
        }
        if (mode == Mode::MissingAfterCoreLoad && call > 2) {
            return std::nullopt;
        }
        if (checkpoint &&
            (mode == Mode::WrongThread || (mode == Mode::WrongThreadAfterPrecheck && call > 0))) {
            checkpoint->thread_id += "-tampered";
        }
        if (checkpoint && mode == Mode::WrongSchemaAfterPrecheck && call > 0) {
            ++checkpoint->schema_version;
        }
        if (checkpoint && mode == Mode::WrongSchemaAtCoreLoad && call > 1) {
            ++checkpoint->schema_version;
        }
        if (checkpoint && mode == Mode::WrongIdAtCoreLoad && call > 1) {
            checkpoint->id += "-tampered";
        }
        return checkpoint;
    }

    std::vector<Checkpoint> list(const std::string& thread_id, int limit) override {
        return inner_->list(thread_id, limit);
    }

    void delete_thread(const std::string& thread_id) override { inner_->delete_thread(thread_id); }

    void put_writes(const std::string&  thread_id,
                    const std::string&  parent_checkpoint_id,
                    const PendingWrite& write) override {
        inner_->put_writes(thread_id, parent_checkpoint_id, write);
    }

    std::vector<PendingWrite> get_writes(const std::string& thread_id,
                                         const std::string& parent_checkpoint_id) override {
        return inner_->get_writes(thread_id, parent_checkpoint_id);
    }

    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override {
        inner_->clear_writes(thread_id, parent_checkpoint_id);
    }

private:
    std::shared_ptr<InMemoryCheckpointStore> inner_ = std::make_shared<InMemoryCheckpointStore>();
    std::atomic<Mode>                        mode_{Mode::Normal};
    std::atomic<std::uint32_t>               exact_loads_{0};
    std::atomic<std::uint32_t>               migration_target_save_failures_{0};
    std::atomic<std::uint32_t> terminal_save_failures_{0};
    std::atomic<std::uint32_t>               migration_target_loads_{0};
    std::atomic<bool>                        migration_target_save_started_{false};
};
json typed_event_value(const TypedGraphEvent& event) {
    json value{{"index", event.index()}};
    switch (event.index()) {
        case 0: {
            const auto& typed  = std::get<NodeStartEvent>(event);
            value["node_name"] = typed.node_name;
            value["retry_attempt"] =
                typed.retry_attempt ? json(*typed.retry_attempt) : json(nullptr);
            value["data"] = typed.data;
            break;
        }
        case 1: {
            const auto& typed     = std::get<NodeEndEvent>(event);
            value["node_name"]    = typed.node_name;
            value["command_goto"] = typed.command_goto ? json(*typed.command_goto) : json(nullptr);
            value["send_count"]   = typed.send_count;
            value["data"]         = typed.data;
            break;
        }
        case 2: {
            const auto& typed  = std::get<LlmTokenEvent>(event);
            value["node_name"] = typed.node_name;
            value["token"]     = typed.token;
            break;
        }
        case 3: {
            const auto& typed  = std::get<ChannelWriteEvent>(event);
            value["node_name"] = typed.node_name;
            value["channel"]   = typed.channel;
            value["value"]     = typed.value;
            break;
        }
        case 4:
            value["state"] = std::get<StateSnapshotEvent>(event).state;
            break;
        case 5: {
            const auto& typed     = std::get<RoutingEvent>(event);
            value["command_goto"] = typed.command_goto ? json(*typed.command_goto) : json(nullptr);
            value["next_nodes"]   = typed.next_nodes;
            value["step"]         = typed.step ? json(*typed.step) : json(nullptr);
            value["data"]         = typed.data;
            break;
        }
        case 6: {
            value["sends"] = json::array();
            for (const auto& send : std::get<SendDispatchEvent>(event).sends) {
                value["sends"].push_back({{"target_node", send.target_node},
                                          {"input", send.input},
                                          {"source_node", send.source_node}});
            }
            break;
        }
        case 7: {
            const auto& typed  = std::get<InterruptEvent>(event);
            value["node_name"] = typed.node_name;
            value["phase"]     = typed.phase ? json(*typed.phase) : json(nullptr);
            value["checkpoint_id"] =
                typed.checkpoint_id ? json(*typed.checkpoint_id) : json(nullptr);
            value["data"] = typed.data;
            break;
        }
        case 8: {
            const auto& typed  = std::get<ErrorEvent>(event);
            value["node_name"] = typed.node_name;
            value["message"]   = typed.message;
            value["data"]      = typed.data;
            break;
        }
        case 9: {
            const auto& typed  = std::get<RawGraphEvent>(event).event;
            value["type"]      = static_cast<int>(typed.type);
            value["node_name"] = typed.node_name;
            value["data"]      = typed.data;
            break;
        }
        default:
            throw std::logic_error("Unknown TypedGraphEvent alternative");
    }
    return value;
}
}  // namespace

namespace {
class ProgramToolEffectProbe final : public neograph::ToolEffectBroker {
public:
    asio::awaitable<neograph::ToolExecutionResult> execute(
        neograph::ToolEffectIdentity identity, neograph::Tool& tool,
        neograph::json arguments, neograph::ToolExecutionContext context) override {
        seen.push_back(std::move(identity));
        co_return co_await context.controller->execute_result_async(
            tool, std::move(arguments), context);
    }

    std::vector<neograph::ToolEffectIdentity> seen;
};
}  // namespace

TEST(ProgramRuntimeTest, MediatedCoreToolRequiresExactRunGrant) {
    mediated_tool_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-mediated-dispatch");

    auto invoke = [&](std::string run_id) {
        ProgramInvocation invocation{json::object(), grant(), "trace-tool-grant", {}};
        invocation.requested_run_id = std::move(run_id);
        return fixture.runtime->start("tenant:runtime", version, std::move(invocation)).wait();
    };

    EXPECT_EQ(invoke("no-grant").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 0U);

    const auto allowed_controller = std::make_shared<neograph::ToolExecutionController>();
    const auto effect_broker = std::make_shared<ProgramToolEffectProbe>();
    fixture.core_tool_grant_resolver = [version_id = version.id(), allowed_controller,
                                        effect_broker](
        const ProgramCoreToolGrantContext& context) -> std::optional<ProgramCoreToolGrant> {
        if (context.owner_scope != "tenant:runtime" ||
            context.program_version_id != version_id ||
            (context.run_id != "allowed" && context.run_id != "no-broker") ||
            context.operation_id != "root" || context.attempt != 1)
            return std::nullopt;
        ProgramCoreToolGrant grant;
        grant.owner_scope = std::string(context.owner_scope);
        grant.program_version_id = std::string(context.program_version_id);
        grant.run_id = std::string(context.run_id);
        grant.operation_id = std::string(context.operation_id);
        grant.attempt = context.attempt;
        grant.grant_id = "grant-allowed";
        grant.binding_fingerprint = std::string(context.binding_fingerprint);
        grant.gate = [](neograph::ToolCall, neograph::ToolGateContext)
            -> asio::awaitable<neograph::ToolDecision> {
            co_return neograph::ToolDecision::allow();
        };
        grant.controller = allowed_controller;
        if (context.run_id == "allowed") grant.effect_broker = effect_broker;
        return grant;
    };
    fixture.runtime = fixture.make_runtime();
    EXPECT_EQ(invoke("no-broker").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 0U);
    EXPECT_EQ(invoke("allowed").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 1U);
    EXPECT_EQ(mediated_tool_controller.load(), allowed_controller.get());
    ASSERT_EQ(effect_broker->seen.size(), 1U);
    EXPECT_EQ(effect_broker->seen[0].owner_scope, "tenant:runtime");
    EXPECT_EQ(effect_broker->seen[0].run_id, "allowed");
    EXPECT_EQ(effect_broker->seen[0].program_version_id, version.id());
    EXPECT_EQ(effect_broker->seen[0].operation_id, "root");
    EXPECT_EQ(effect_broker->seen[0].grant_id, "grant-allowed");
    EXPECT_EQ(effect_broker->seen[0].attempt, 1U);
    EXPECT_FALSE(effect_broker->seen[0].thread_id.empty());
    EXPECT_FALSE(effect_broker->seen[0].task_id.empty());
    EXPECT_EQ(effect_broker->seen[0].call_ordinal, 0U);

    fixture.core_tool_grant_resolver = [version_id = version.id()](
        const ProgramCoreToolGrantContext& context) -> std::optional<ProgramCoreToolGrant> {
        ProgramCoreToolGrant stale;
        stale.owner_scope = std::string(context.owner_scope);
        stale.program_version_id = version_id;
        stale.run_id = context.run_id == "stale-run"
            ? "a-different-run" : std::string(context.run_id);
        stale.operation_id = std::string(context.operation_id);
        stale.attempt = context.attempt;
        if (context.run_id == "stale-operation")
            stale.operation_id = "a-different-operation";
        if (context.run_id == "stale-attempt") ++stale.attempt;
        stale.grant_id = "grant-stale";
        stale.binding_fingerprint = context.run_id == "stale-binding"
            ? "another-binding" : std::string(context.binding_fingerprint);
        stale.gate = [](neograph::ToolCall, neograph::ToolGateContext)
            -> asio::awaitable<neograph::ToolDecision> {
            co_return neograph::ToolDecision::allow();
        };
        stale.controller = std::make_shared<neograph::ToolExecutionController>();
        return stale;
    };
    fixture.runtime = fixture.make_runtime();
    EXPECT_EQ(invoke("stale-run").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(invoke("stale-operation").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(invoke("stale-attempt").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(invoke("stale-binding").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 1U);
}

namespace {
class ProgramBrokerProbe final : public neograph::graph::ProviderCallBroker {
public:
    asio::awaitable<sp::runtime::Result> invoke(
        neograph::graph::ProviderCallIdentity,
        std::shared_ptr<neograph::Provider>,
        neograph::ProviderRequest) override {
        sp::Completion completion;
        completion.messages.push_back(sp::Message{
            "broker-message", sp::Role::Assistant, {sp::Text{"brokered"}}});
        co_return std::make_shared<const sp::Outcome>(std::move(completion));
    }
};
}  // namespace

TEST(ProgramRuntimeTest, CoreProviderResolverChecksExactOperationBinding) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    const auto broker = std::make_shared<ProgramBrokerProbe>();
    std::vector<std::string> seen_runs;
    fixture.core_provider_call_resolver =
        [&, version_id = version.id()](const ProgramCoreProviderCallContext& context)
            -> std::optional<ProgramCoreProviderCallBinding> {
        EXPECT_EQ(context.owner_scope, "tenant:runtime");
        EXPECT_EQ(context.program_version_id, version_id);
        EXPECT_EQ(context.operation_id, "root");
        EXPECT_EQ(context.attempt, 1U);
        seen_runs.emplace_back(context.run_id);
        ProgramCoreProviderCallBinding binding;
        binding.owner_scope = std::string(context.owner_scope);
        binding.program_version_id = std::string(context.program_version_id);
        binding.run_id = context.run_id == "stale" ? "other-run" : std::string(context.run_id);
        binding.operation_id = std::string(context.operation_id);
        binding.attempt = context.attempt;
        binding.broker = broker;
        return binding;
    };
    fixture.require_core_provider_call_broker = true;
    fixture.runtime = fixture.make_runtime();

    const auto invoke = [&](std::string run_id) {
        ProgramInvocation invocation{json::object(), grant(), "trace-provider-broker", {}};
        invocation.requested_run_id = std::move(run_id);
        return fixture.runtime->start("tenant:runtime", version, std::move(invocation)).wait();
    };
    EXPECT_EQ(invoke("allowed").status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(invoke("stale").status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(completed_calls.load(), 1U);
    ASSERT_EQ(seen_runs.size(), 2U);
    EXPECT_EQ(seen_runs[0], "allowed");
    EXPECT_EQ(seen_runs[1], "stale");

    fixture.core_provider_call_resolver = {};
    fixture.runtime = fixture.make_runtime();
    EXPECT_EQ(invoke("missing-resolver").status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RequiredCoreProviderBrokerMustBeReboundOnReconnect) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    auto document = program_document("runtime-completed");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version = fixture.admit_document(std::move(document));
    const auto broker = std::make_shared<ProgramBrokerProbe>();
    fixture.require_core_provider_call_broker = true;
    fixture.core_provider_call_resolver = [broker](
        const ProgramCoreProviderCallContext& context)
        -> std::optional<ProgramCoreProviderCallBinding> {
        return ProgramCoreProviderCallBinding{
            std::string(context.owner_scope), std::string(context.program_version_id),
            std::string(context.run_id), std::string(context.operation_id),
            context.attempt, broker};
    };
    fixture.runtime = fixture.make_runtime();
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-provider-reconnect", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(completed_calls.load(), 0U);

    fixture.core_provider_call_resolver = {};
    fixture.runtime = fixture.make_runtime();
    const auto reconnected = fixture.runtime->reconnect(
        "tenant:runtime", interrupted.run_id()).wait();
    ASSERT_EQ(reconnected.status(), ProgramTerminalStatus::Interrupted);
    const auto resumed = fixture.runtime
        ->resume("tenant:runtime", interrupted.run_id(),
                 resume_for(interrupted, json::object(), "trace-provider-resume"))
        .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ReconnectedCoreToolRequiresReboundGrant) {
    mediated_tool_calls.store(0);
    AdmittedRuntime fixture;
    auto document = program_document("runtime-mediated-dispatch");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version = fixture.admit_document(std::move(document));

    fixture.core_tool_grant_resolver = [](const ProgramCoreToolGrantContext& context)
        -> std::optional<ProgramCoreToolGrant> {
        ProgramCoreToolGrant grant;
        grant.owner_scope = std::string(context.owner_scope);
        grant.program_version_id = std::string(context.program_version_id);
        grant.run_id = std::string(context.run_id);
        grant.operation_id = std::string(context.operation_id);
        grant.attempt = context.attempt;
        grant.grant_id = "grant-before-restart";
        grant.binding_fingerprint = std::string(context.binding_fingerprint);
        grant.gate = [](neograph::ToolCall, neograph::ToolGateContext)
            -> asio::awaitable<neograph::ToolDecision> {
            co_return neograph::ToolDecision::allow();
        };
        grant.controller = std::make_shared<neograph::ToolExecutionController>();
        grant.effect_broker = std::make_shared<ProgramToolEffectProbe>();
        return grant;
    };
    fixture.runtime = fixture.make_runtime();
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-tool-grant-before-restart", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(mediated_tool_calls.load(), 0U);

    const auto durable_resolver = fixture.core_tool_grant_resolver;
    fixture.core_tool_grant_resolver = {};
    fixture.runtime = fixture.make_runtime();
    const auto reconnected =
        fixture.runtime->reconnect("tenant:runtime", interrupted.run_id()).wait();
    ASSERT_EQ(reconnected.status(), ProgramTerminalStatus::Interrupted);
    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json::object(),
                                                 "trace-tool-grant-after-restart"))
                             .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 0U);

    fixture.core_tool_grant_resolver = durable_resolver;
    fixture.runtime = fixture.make_runtime();
    const auto second_interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-tool-grant-rebound", {}});
    ASSERT_EQ(second_interrupted.status(), ProgramTerminalStatus::Interrupted);
    fixture.runtime = fixture.make_runtime();
    const auto rebound = fixture.runtime
                             ->reconnect("tenant:runtime", second_interrupted.run_id())
                             .wait();
    ASSERT_EQ(rebound.status(), ProgramTerminalStatus::Interrupted);
    const auto allowed = fixture.runtime
                             ->resume("tenant:runtime", second_interrupted.run_id(),
                                      resume_for(second_interrupted, json::object(),
                                                 "trace-tool-grant-rebound-resume"))
                             .wait();
    EXPECT_EQ(allowed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 1U);
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramRuntimeTest, DurableCoreToolGrantRebindsExactAuthorityAcrossRestart) {
    mediated_tool_calls.store(0);
    AdmittedRuntime fixture;
    auto document = program_document("runtime-mediated-dispatch");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version = fixture.admit_document(std::move(document));
    const auto binding = capability_binding_receipt_root(
        version.core_materialization_receipt().capability_bindings);
    const auto path = std::filesystem::temp_directory_path() /
        ("neograph-core-tool-grant-" + Checkpoint::generate_id() + ".db");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    GrantStoreHandle store = std::make_shared<SQLiteProgramCoreToolGrantStore>(path.string());
    const auto effect_broker = std::make_shared<ProgramToolEffectProbe>();
    const auto make_policy = [effect_broker](const ProgramCoreToolGrantRecord& record)
        -> std::optional<ProgramCoreToolGrant> {
        ProgramCoreToolGrant authorized;
        authorized.owner_scope = record.owner_scope;
        authorized.program_version_id = record.program_version_id;
        authorized.run_id = record.run_id;
        authorized.operation_id = record.operation_id;
        authorized.attempt = record.attempt;
        authorized.binding_fingerprint = record.binding_fingerprint;
        authorized.grant_id = record.grant_id;
        authorized.gate = [](neograph::ToolCall, neograph::ToolGateContext)
            -> asio::awaitable<neograph::ToolDecision> {
            co_return neograph::ToolDecision::allow();
        };
        authorized.controller = std::make_shared<neograph::ToolExecutionController>();
        authorized.effect_broker = effect_broker;
        return authorized;
    };
    fixture.core_tool_grant_resolver =
        make_durable_core_tool_grant_resolver(store, make_policy);
    fixture.runtime = fixture.make_runtime();

    const auto context = ProgramCoreToolGrantContext{
        "tenant:runtime", version.id(), "durable-grant-run", "root", 1, binding};
    const auto record = ProgramCoreToolGrantRecord{
        std::string(context.owner_scope), std::string(context.program_version_id),
        std::string(context.run_id), std::string(context.operation_id),
        context.attempt, binding, "host-grant-1"};
    ASSERT_EQ(store->admit(record), ProgramCoreToolGrantAdmission::Admitted);
    EXPECT_EQ(store->admit(record), ProgramCoreToolGrantAdmission::AlreadyPresent);
    auto conflicting = record;
    conflicting.grant_id = "substituted-grant";
    EXPECT_EQ(store->admit(conflicting), ProgramCoreToolGrantAdmission::Conflict);
    EXPECT_FALSE(store->load(ProgramCoreToolGrantContext{
        "other-tenant", version.id(), context.run_id, "root", 1, binding}));

    ProgramInvocation request{json::object(), grant(), "trace-durable-tool-grant", {}};
    request.requested_run_id = std::string(context.run_id);
    const auto interrupted = fixture.runtime->start(
        "tenant:runtime", version, std::move(request)).wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(mediated_tool_calls.load(), 0U);

    fixture.runtime.reset();
    fixture.core_tool_grant_resolver = {};
    store.reset();
    store = std::make_shared<SQLiteProgramCoreToolGrantStore>(path.string());
    fixture.core_tool_grant_resolver =
        make_durable_core_tool_grant_resolver(store, make_policy);
    fixture.runtime = fixture.make_runtime();
    const auto reconnected = fixture.runtime->reconnect(
        "tenant:runtime", interrupted.run_id()).wait();
    ASSERT_EQ(reconnected.status(), ProgramTerminalStatus::Interrupted);
    // Resume advances the Program attempt; the trusted host explicitly
    // re-admits its existing authority for that attempt before dispatch.
    auto next_attempt = record;
    next_attempt.attempt = 2;
    ASSERT_EQ(store->admit(next_attempt), ProgramCoreToolGrantAdmission::Admitted);
    const auto resumed = fixture.runtime->resume(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json::object(), "trace-durable-tool-resume")).wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 1U);
    EXPECT_TRUE(store->revoke(context));
    EXPECT_FALSE(store->load(context));
    EXPECT_FALSE(store->load(ProgramCoreToolGrantContext{
        context.owner_scope, context.program_version_id, context.run_id,
        context.operation_id, 2, context.binding_fingerprint}));
    EXPECT_EQ(store->admit(record), ProgramCoreToolGrantAdmission::Conflict);
}

TEST(ProgramRuntimeTest, DurableCoreToolGrantRejectsChangedBindingAndHostPolicy) {
    mediated_tool_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-mediated-dispatch");
    const auto binding = capability_binding_receipt_root(
        version.core_materialization_receipt().capability_bindings);
    const auto path = std::filesystem::temp_directory_path() /
        ("neograph-core-tool-grant-negative-" + Checkpoint::generate_id() + ".db");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    GrantStoreHandle store = std::make_shared<SQLiteProgramCoreToolGrantStore>(path.string());
    const ProgramCoreToolGrantRecord record{
        "tenant:runtime", version.id(), "different-binding", "root", 1,
        "different-executable-binding", "host-grant-negative"};
    ASSERT_EQ(store->admit(record), ProgramCoreToolGrantAdmission::Admitted);
    fixture.core_tool_grant_resolver =
        make_durable_core_tool_grant_resolver(store,
            [](const ProgramCoreToolGrantRecord& persisted)
                -> std::optional<ProgramCoreToolGrant> {
                ProgramCoreToolGrant unauthorized;
                unauthorized.owner_scope = persisted.owner_scope;
                unauthorized.program_version_id = persisted.program_version_id;
                unauthorized.run_id = persisted.run_id;
                unauthorized.operation_id = persisted.operation_id;
                unauthorized.attempt = persisted.attempt;
                unauthorized.binding_fingerprint = persisted.binding_fingerprint;
                unauthorized.grant_id = "another-grant";
                unauthorized.gate = [](neograph::ToolCall, neograph::ToolGateContext)
                    -> asio::awaitable<neograph::ToolDecision> {
                    co_return neograph::ToolDecision::allow();
                };
                unauthorized.controller =
                    std::make_shared<neograph::ToolExecutionController>();
                return unauthorized;
            });
    fixture.runtime = fixture.make_runtime();
    ProgramInvocation request{json::object(), grant(), "trace-wrong-binding", {}};
    request.requested_run_id = record.run_id;
    EXPECT_EQ(fixture.runtime->start("tenant:runtime", version, std::move(request))
                  .wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 0U);
    EXPECT_FALSE(store->load(ProgramCoreToolGrantContext{
        "tenant:runtime", version.id(), record.run_id, "root", 1, binding}));
    auto policy_mismatch = record;
    policy_mismatch.run_id = "wrong-host-policy";
    policy_mismatch.binding_fingerprint = binding;
    policy_mismatch.grant_id = "recorded-grant";
    ASSERT_EQ(store->admit(policy_mismatch), ProgramCoreToolGrantAdmission::Admitted);
    ProgramInvocation next{json::object(), grant(), "trace-wrong-policy", {}};
    next.requested_run_id = policy_mismatch.run_id;
    EXPECT_EQ(fixture.runtime->start("tenant:runtime", version, std::move(next))
                  .wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(mediated_tool_calls.load(), 0U);
}
#endif

TEST(ProgramRuntimeTest, CompletedRunPinsAdmittedIdentitiesAndPublishesOrderedEvents) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-completed", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.program_version_id(), version.id());
    EXPECT_EQ(result.bundle_id(), version.bundle_id());
    EXPECT_EQ(result.operation_id(), "root");
    EXPECT_EQ(result.attempt(), 1U);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().program_operations, 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_program_operations, 0U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_EQ(result.remaining_budget().max_concurrency, 1U);
    ASSERT_TRUE(result.checkpoint().has_value());
    EXPECT_EQ(result.checkpoint()->core_name, "main");
    EXPECT_EQ(result.checkpoint()->core_generation_id,
              version.core_materialization_receipt().plans.front().compiled_plan_identity);

    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(journal->program_version_id, version.id());
    ASSERT_TRUE(journal->core_checkpoint.has_value());
    EXPECT_EQ(journal->core_checkpoint->core_name, result.checkpoint()->core_name);
    EXPECT_EQ(journal->core_checkpoint->core_thread_id, result.checkpoint()->core_thread_id);
    EXPECT_EQ(journal->core_checkpoint->checkpoint_id, result.checkpoint()->checkpoint_id);
    const auto lineage_id = program_run_lineage_id("tenant:runtime", result.run_id());
    const auto lineage    = fixture.journal->load_lineage("tenant:runtime", lineage_id);
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_EQ(lineage->remaining_budget(), journal->remaining_budget);
    EXPECT_EQ(lineage->inflight_reservation(), journal->inflight_reservation);
    const auto generation = fixture.journal->load_generation("tenant:runtime", lineage_id, 1);
    ASSERT_TRUE(generation.has_value());
    EXPECT_EQ(generation->program_version_id(), version.id());
    EXPECT_EQ(generation->bundle_id(), version.bundle_id());
    EXPECT_EQ(generation->run_id(), result.run_id());

    const auto events = handle.events_after(0);
    ASSERT_GE(events.size(), 4U);
    EXPECT_EQ(events.front().kind, ProgramEventKind::Started);
    EXPECT_EQ(events.back().kind, ProgramEventKind::Terminal);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const ProgramEvent& event) {
                                return event.kind == ProgramEventKind::CheckpointPublished;
                            }),
              1);
    for (std::size_t index = 0; index < events.size(); ++index) {
        EXPECT_EQ(events[index].sequence, index + 1);

        EXPECT_EQ(events[index].run_id, result.run_id());
        EXPECT_EQ(events[index].program_version_id, version.id());
        EXPECT_EQ(events[index].attempt, 1U);
        EXPECT_EQ(events[index].trace_id, "trace-completed");
    }
}

TEST(ProgramRuntimeTest, DeveloperInstructionIsDurableBeforeActiveGenerationDecision) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "instruction-source", {}});
    const auto result = handle.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);

    auto contexts = std::make_shared<DurableInstructionContextStore>();
    RuntimeInstructionController controller({
        contexts,
        fixture.journal,
        fixture.catalog,
        [](const RuntimeDeveloperInstruction& instruction,
           const ProgramRunLineage& lineage,
           const ProgramRunGeneration&) {
            RuntimeInstructionDecisionData decision;
            decision.instruction_id = instruction.id();
            decision.source_run_id = instruction.data().source_run_id;
            decision.expected_lineage_head_id = lineage.id();
            decision.policy_identity = digest('9');
            decision.action = RuntimeInstructionAction::SatisfiedInPlace;
            decision.reason = "current topology already satisfies the instruction";
            return decision;
        }});
    RuntimeDeveloperInstructionData data;
    data.owner_scope = "tenant:runtime";
    data.source_run_id = result.run_id();
    data.feed_id = "developer-instructions";
    data.sequence = 1;
    data.submitted_at_ms = 1;
    data.text = "Keep the current topology";
    const auto instruction = RuntimeDeveloperInstruction::create(std::move(data));
    const auto plan = controller.submit_and_plan(instruction, std::nullopt);
    EXPECT_EQ(plan.decision.data().action,
              RuntimeInstructionAction::SatisfiedInPlace);
    EXPECT_EQ(plan.decision.data().instruction_id, instruction.id());
    const auto head = contexts->history_head(
        {"tenant:runtime", "developer-instructions"});
    ASSERT_TRUE(head.record_id);
    EXPECT_EQ(*head.record_id, plan.history_record.id());
    ASSERT_TRUE(contexts->get_artifact("tenant:runtime",
                                       plan.decision_artifact.id()));
    EXPECT_TRUE(plan.decision_artifact.required());
}

TEST(ProgramRuntimeTest, ReconnectFailsClosedWithoutDurableRuntimeRecoveryBoundary) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-recovery-required", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);

    auto overlay = std::make_shared<IsolatedProcessTransitionStore>(fixture.journal);
    overlay->set_runtime_recovery_state(
        result.run_id(), {recovery_context(result.run_id())},
        {recovery_hook(result.run_id())});
    auto runtime = fixture.make_runtime_with_transitions(overlay);

    try {
        (void)runtime->reconnect("tenant:runtime", result.run_id());
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_RUNTIME_RECOVERY_UNAVAILABLE");
    }
}

TEST(ProgramRuntimeTest, ReconnectRestoresValidatedContextAndHookHeadsBeforeReturning) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-runtime-recovery", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);

    auto context = recovery_context(result.run_id());
    auto hook = recovery_hook(result.run_id());
    const auto context_id = context.epoch.id();
    const auto hook_id = hook.id();
    auto overlay = std::make_shared<IsolatedProcessTransitionStore>(fixture.journal);
    overlay->set_runtime_recovery_state(result.run_id(), {context}, {hook});
    unsigned restores = 0;
    auto runtime = fixture.make_runtime_with_transitions(
        overlay, [&](const ProgramRuntimeRecoveryState& state) {
            ++restores;
            EXPECT_EQ(state.owner_scope, "tenant:runtime");
            EXPECT_EQ(state.run_id, result.run_id());
            ASSERT_EQ(state.context_publications.size(), 1u);
            ASSERT_EQ(state.hook_outbox_entries.size(), 1u);
            EXPECT_EQ(state.context_publications.front().epoch.id(), context_id);
            EXPECT_EQ(state.hook_outbox_entries.front().id(), hook_id);
        });

    const auto reconnected = runtime->reconnect("tenant:runtime", result.run_id()).wait();
    EXPECT_EQ(reconnected.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(restores, 1u);
}

TEST(ProgramRuntimeTest, LifecycleHooksPublishBeforeSpawnAndFailClosedBeforeTerminalCas) {
    LifecycleHookTool admitted{"program-admitted"};
    LifecycleHookTool terminal{"program-before-terminal", true};
    LifecycleHookTool failed{"program-failed"};
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Strict, false,
                            lifecycle_hooks(admitted, terminal, failed));
    const auto version = fixture.admit("runtime-completed");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-hook-lifecycle", {}});

    EXPECT_EQ(admitted.calls.load(), 1U);
    // The blocked completed terminal is replaced by P_HOOK_BLOCKED and that
    // replacement event receives its own deterministic terminal-hook pass.
    EXPECT_EQ(terminal.calls.load(), 2U);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_HOOK_BLOCKED");
    // RunFailed is emitted only after the terminal compare-publish wins.
    EXPECT_EQ(failed.calls.load(), 1U);
    const auto durable = fixture.journal->load("tenant:runtime", result.run_id());
    ASSERT_TRUE(durable.has_value());
    EXPECT_EQ(durable->continuation().state, ContinuationState::Failed);
}

TEST(ProgramRuntimeTest, LifecycleHookIdentityIsReusedAcrossTerminalCasRetry) {
    LifecycleHookTool admitted{"program-admitted-retry"};
    LifecycleHookTool terminal{"program-before-terminal-retry"};
    LifecycleHookTool failed{"program-failed-retry"};
    auto journal = std::make_shared<ConflictOnceJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Strict, false,
                            lifecycle_hooks(admitted, terminal, failed));
    const auto version = fixture.admit("runtime-completed");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-hook-retry", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(journal->injected());
    EXPECT_EQ(admitted.calls.load(), 1U);
    // The terminal publication was retried, but its Program event sequence and
    // therefore its durable hook event identity did not change.
    EXPECT_EQ(terminal.calls.load(), 1U);
    EXPECT_EQ(failed.calls.load(), 0U);
}

TEST(ProgramRuntimeTest, BlockedAdmissionHookReturnsRecoverableTerminalHandleAfterReconnect) {
    completed_calls.store(0);
    LifecycleHookTool admitted{"program-admitted-blocked", true};
    LifecycleHookTool terminal{"program-before-terminal-after-admission-block"};
    LifecycleHookTool failed{"program-failed-after-admission-block"};
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Strict, false,
                            lifecycle_hooks(admitted, terminal, failed));
    const auto version = fixture.admit("runtime-completed");
    ProgramInvocation invocation{json::object(), grant(), "trace-admission-block", {},
                                 "admission-hook-blocked"};

    // The Started transition is durable before the hook executes. A failure
    // must return a handle whose terminal state is already recoverable.
    const auto result = fixture.runtime->start("tenant:runtime", version, invocation).wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_HOOK_BLOCKED");
    EXPECT_EQ(completed_calls.load(), 0U);

    fixture.recreate_catalog_and_runtime();
    const auto recovered = fixture.runtime->reconnect("tenant:runtime", "admission-hook-blocked").wait();
    EXPECT_EQ(recovered.id(), result.id());
    EXPECT_EQ(recovered.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(recovered.failure());
    EXPECT_EQ(recovered.failure()->code, "P_HOOK_BLOCKED");
    const auto durable = fixture.journal->load("tenant:runtime", "admission-hook-blocked");
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->continuation().state, ContinuationState::Failed);
}

TEST(ProgramRuntimeTest, InterruptedCancellationHookBlockPersistsFailureAfterReconnect) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-interrupt");
    const auto interrupted = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-interrupted-cancel-hook", {},
                          "interrupted-cancel-hook"}).wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    LifecycleHookTool admitted{"program-admitted-interrupted-cancel"};
    LifecycleHookTool terminal{"program-terminal-interrupted-cancel", true};
    LifecycleHookTool failed{"program-failed-interrupted-cancel"};
    fixture.hook_runtime = lifecycle_hooks(admitted, terminal, failed);
    fixture.recreate_catalog_and_runtime();
    auto handle = fixture.runtime->reconnect("tenant:runtime", interrupted.run_id());

    EXPECT_TRUE(handle.cancel());
    const auto blocked = handle.wait();
    EXPECT_EQ(blocked.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(blocked.failure());
    EXPECT_EQ(blocked.failure()->code, "P_HOOK_BLOCKED");
    const auto durable = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->continuation().state, ContinuationState::Failed);
}
TEST(ProgramRuntimeTest, RejectsCrossScopeStartBeforePublishingRun) {
    completed_calls.store(0);
    AdmittedRuntime   fixture;
    const auto        version = fixture.admit("runtime-completed");
    ProgramInvocation invocation{json::object(), grant(), "trace-cross-scope", {}};
    invocation.requested_run_id = "cross-scope-run";

    EXPECT_THROW((void)fixture.runtime->start("tenant:other", version, std::move(invocation)),
                 ProgramDiagnosticError);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_FALSE(fixture.journal->load("tenant:other", "cross-scope-run").has_value());
    EXPECT_FALSE(fixture.journal->load("tenant:runtime", "cross-scope-run").has_value());
}

TEST(ProgramRuntimeTest, RequestedRunIdIsUsedExactlyAndCollisionNeverDispatches) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto invocation             = ProgramInvocation{json::object(), grant(), "trace-requested", {}};
    invocation.requested_run_id = "harness-run-exact";
    const auto first = fixture.runtime->start("tenant:runtime", version, invocation).wait();
    EXPECT_EQ(first.run_id(), "harness-run-exact");
    EXPECT_EQ(completed_calls.load(), 1U);

    EXPECT_THROW((void)fixture.runtime->start("tenant:runtime", version, std::move(invocation)),
                 ProgramDiagnosticError);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, CanonicalRunInvocationIsRetainedExactlyAndAcceptsRuntimeEventSink) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-canonical-adapter";
    invocation.program_version_id = version.id();
    invocation.run_id             = "canonical-runtime-run";
    invocation.budget             = grant();
    invocation.input              = json{{"request", "canonical"}};
    invocation.message_sequence   = 17;
    invocation.idempotency_key    = "test-canonical-adapter:17";
    invocation.correlation_id     = "trace-canonical-runtime";
    invocation.validate();
    auto sink = std::make_shared<CountingSink>();

    auto       handle = fixture.runtime->start(invocation, sink);
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.run_id(), invocation.run_id);
    EXPECT_EQ(handle.snapshot().invocation(), invocation);
    EXPECT_EQ(handle.snapshot().child_depth(), 0U);
    EXPECT_GT(sink->calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ActiveStartMaterializesPersistedVersionInColdCatalog) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    ASSERT_EQ(fixture.catalog->activate("tenant:runtime", version.id(), 0),
              ProgramActivationResult::Activated);
    fixture.runtime.reset();
    fixture.catalog.reset();
    fixture.engines = std::make_shared<EngineGenerationCache>();
    fixture.catalog = std::make_shared<ProgramCatalog>(
        CatalogConfig{fixture.store, fixture.registry, fixture.engines, "program-runtime-test/v1"});
    fixture.runtime = fixture.make_runtime();

    RunInvocation request;
    request.owner_scope = "tenant:runtime";
    request.agent_id = "cold-active";
    request.budget = grant();
    request.message_sequence = 1;
    request.idempotency_key = "cold-active:1";
    request.correlation_id = "trace-cold-active";
    request.run_id = "cold-active-run";
    auto active = fixture.runtime->start_active(request);
    EXPECT_EQ(active.activation.active_version_id(), version.id());
    EXPECT_EQ(active.activation.generation(), 1U);
    EXPECT_EQ(active.handle.snapshot().program_version_id(), version.id());
    ASSERT_TRUE(active.handle.snapshot().invocation().selected_activation);
    EXPECT_EQ(active.handle.snapshot().invocation().selected_activation->id(),
              active.activation.id());
    EXPECT_EQ(active.handle.wait().status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, ActiveStartPinsAdmittedVersionAcrossRollback) {
    completed_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto first = fixture.admit("runtime-completed");
    const auto second = fixture.admit("runtime-followup");

    RunInvocation request;
    request.owner_scope = "tenant:runtime";
    request.agent_id = "active-start";
    request.budget = grant();
    request.message_sequence = 1;
    request.idempotency_key = "active-start:1";
    request.correlation_id = "trace-active-start";
    request.run_id = "active-start-first";
    EXPECT_THROW((void)fixture.runtime->start_active(request), ProgramDiagnosticError);
    EXPECT_FALSE(fixture.journal->load("tenant:runtime", request.run_id));
    request.program_version_id = first.id();
    EXPECT_THROW((void)fixture.runtime->start_active(request), std::invalid_argument);
    request.program_version_id.clear();

    ASSERT_EQ(fixture.catalog->activate("tenant:runtime", first.id(), 0),
              ProgramActivationResult::Activated);
    auto original = fixture.runtime->start_active(request);
    EXPECT_EQ(original.activation.generation(), 1U);
    EXPECT_EQ(original.activation.active_version_id(), first.id());
    EXPECT_EQ(original.handle.snapshot().program_version_id(), first.id());
    ASSERT_TRUE(original.handle.snapshot().invocation().selected_activation);
    EXPECT_EQ(original.handle.snapshot().invocation().selected_activation->id(),
              original.activation.id());
    EXPECT_EQ(RunInvocation::parse(
                  original.handle.snapshot().invocation().serialize_canonical())
                  .selected_activation->id(),
              original.activation.id());
    auto forged = request;
    forged.run_id = "active-start-forged";
    forged.program_version_id = first.id();
    forged.selected_activation = original.activation;
    EXPECT_THROW((void)fixture.runtime->start(forged), ProgramDiagnosticError);
    EXPECT_FALSE(fixture.journal->load("tenant:runtime", forged.run_id));

    ASSERT_EQ(fixture.catalog->activate("tenant:runtime", second.id(), 1),
              ProgramActivationResult::Activated);
    request.run_id = "active-start-second";
    request.message_sequence = 2;
    request.idempotency_key = "active-start:2";
    auto next = fixture.runtime->start_active(request);
    EXPECT_EQ(next.activation.generation(), 2U);
    EXPECT_EQ(next.handle.snapshot().program_version_id(), second.id());
    ASSERT_TRUE(next.handle.snapshot().invocation().selected_activation);
    EXPECT_EQ(next.handle.snapshot().invocation().selected_activation->id(),
              next.activation.id());

    ASSERT_EQ(fixture.catalog->rollback("tenant:runtime", first.id(), 2),
              ProgramActivationResult::Activated);
    EXPECT_EQ(original.handle.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(original.handle.snapshot().program_version_id(), first.id());
    EXPECT_EQ(next.handle.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(next.handle.snapshot().program_version_id(), second.id());
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ConcurrentActiveStartSelectsWholeActivationTuples) {
    completed_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto first = fixture.admit("runtime-completed");
    const auto second = fixture.admit("runtime-followup");
    ASSERT_EQ(fixture.catalog->activate("tenant:runtime", first.id(), 0),
              ProgramActivationResult::Activated);

    std::barrier gate(2);
    std::exception_ptr publisher_error;
    std::thread publisher([&] {
        try {
            gate.arrive_and_wait();
            for (std::uint64_t generation = 1; generation <= 40; ++generation) {
                const auto& selected = generation % 2 ? second : first;
                if (fixture.catalog->activate("tenant:runtime", selected.id(), generation) !=
                    ProgramActivationResult::Activated) {
                    throw std::runtime_error("activation CAS failed without competing publishers");
                }
            }
        } catch (...) {
            publisher_error = std::current_exception();
        }
    });

    std::vector<ProgramActiveRun> runs;
    runs.reserve(40);
    std::exception_ptr admission_error;
    try {
        gate.arrive_and_wait();
        for (int index = 0; index < 40; ++index) {
            RunInvocation request;
            request.owner_scope = "tenant:runtime";
            request.agent_id = "active-race";
            request.run_id = "active-race-" + std::to_string(index);
            request.budget = grant();
            request.message_sequence = static_cast<std::uint64_t>(index + 1);
            request.idempotency_key = request.run_id;
            request.correlation_id = request.run_id;
            runs.push_back(fixture.runtime->start_active(std::move(request)));
        }
    } catch (...) {
        admission_error = std::current_exception();
    }
    publisher.join();
    if (publisher_error) std::rethrow_exception(publisher_error);
    if (admission_error) std::rethrow_exception(admission_error);
    for (auto& run : runs) {
        const auto expected = run.activation.generation() % 2 ? first.id() : second.id();
        EXPECT_EQ(run.activation.active_version_id(), expected);
        EXPECT_EQ(run.handle.snapshot().program_version_id(), expected);
        ASSERT_TRUE(run.handle.snapshot().invocation().selected_activation);
        EXPECT_EQ(run.handle.snapshot().invocation().selected_activation->id(),
                  run.activation.id());
        EXPECT_EQ(run.handle.wait().status(), ProgramTerminalStatus::Completed);
    }
    EXPECT_EQ(completed_calls.load() + followup_calls.load(), runs.size());
}

TEST(ProgramRuntimeTest, ActiveRunGraphForkDropsSourceActivationButRetainsLineage) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto version = fixture.admit("runtime-short-blocking");
    ASSERT_EQ(fixture.catalog->activate("tenant:runtime", version.id(), 0),
              ProgramActivationResult::Activated);

    RunInvocation request;
    request.owner_scope = "tenant:runtime";
    request.agent_id = "active-graph-source";
    request.run_id = "active-graph-source";
    request.budget = grant();
    request.message_sequence = 1;
    request.idempotency_key = request.run_id;
    request.correlation_id = request.run_id;
    auto source = fixture.runtime->start_active(request);
    auto target = fixture.runtime->migrate_graph(
        source.handle,
        ProgramGraphMigrationTarget{version.id(), "active-graph-successor", {}});
    const auto result = target.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.run_id(), "active-graph-successor");
    EXPECT_FALSE(target.snapshot().invocation().selected_activation);
    EXPECT_EQ(source.handle.snapshot().invocation().selected_activation->id(),
              source.activation.id());
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.handle.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    EXPECT_EQ(fixture.runtime->reconnect("tenant:runtime", request.run_id).wait().id(),
              result.id());
    (void)source.handle.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ReconnectTerminalAfterCatalogRecreationIsByteExactAndNonMutating) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version   = fixture.admit("runtime-completed");
    const auto      completed = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-terminal-reconnect", {}});
    const auto run_id = completed.run_id();
    const auto record_bytes =
        fixture.journal->load("tenant:runtime", run_id)->serialize_canonical();
    const auto journal_before = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_before.has_value());

    fixture.recreate_catalog_and_runtime();
    auto reconnected = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_EQ(reconnected.snapshot().serialize_canonical(), record_bytes);
    EXPECT_EQ(reconnected.wait().serialize_canonical(), completed.serialize_canonical());
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto journal_after = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_after.has_value());
    EXPECT_EQ(journal_after->id, journal_before->id);
    EXPECT_EQ(journal_after->sequence, journal_before->sequence);

    const auto diagnostic_code = [&](std::string_view owner, std::string_view candidate) {
        try {
            (void)fixture.runtime->reconnect(owner, candidate);
        } catch (const ProgramDiagnosticError& error) {
            return error.diagnostic().code;
        }
        return std::string{};
    };
    EXPECT_EQ(diagnostic_code("tenant:other", run_id),
              diagnostic_code("tenant:other", "absent-run"));
}

TEST(ProgramRuntimeTest, ReconnectInterruptedResumesExactCheckpointOnceWithoutInputReplay) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"original", "must-not-replay"}}, grant(), "trace-interrupted-reconnect", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    const auto run_id = interrupted.run_id();
    const auto record_bytes =
        fixture.journal->load("tenant:runtime", run_id)->serialize_canonical();
    const auto journal_before = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_before.has_value());

    fixture.recreate_catalog_and_runtime();
    auto reconnected = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_EQ(reconnected.wait().serialize_canonical(), interrupted.serialize_canonical());
    EXPECT_EQ(reconnected.snapshot().serialize_canonical(), record_bytes);
    const auto journal_after_reconnect = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_after_reconnect.has_value());
    EXPECT_EQ(journal_after_reconnect->id, journal_before->id);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", run_id,
                     resume_for(interrupted, json{{"decision", "approved"}}, "trace-exact-resume"))
            .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "approved");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

// #377: crash recovery of an attempt started by resume, reconcile or fork
// resumes the attempt's checkpoint with the value that attempt was started
// with, never with the run input. Child budgets make a define()-only run
// recover from its checkpoint instead of fencing on a root execution lease.
json recoverable_document(std::string node_type) {
    auto document = program_document(std::move(node_type));
    document["declared_budget_requirements"][7]["maximum"] = 1;
    document["declared_budget_requirements"][8]["maximum"] = 1;
    return document;
}

RunBudget recoverable_grant() {
    return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1};
}

std::string failure_detail(const ProgramResult& result) {
    return result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                  result.failure()->witness.dump()
                            : "no failure detail";
}

TEST(ProgramRuntimeTest, RecoveryOfAResumedAttemptResumesWithItsValueNotTheRunInput) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto      version = fixture.admit_document(recoverable_document("runtime-resume-hook"));
    const auto interrupted  = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"decision", "run input"}}, recoverable_grant(),
                           "trace-resumed-crash", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);

    HeldResumedAttempt held;
    auto original = fixture.runtime->resume(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-resumed-crash-resume"));
    ASSERT_TRUE(held.entered());

    // The resumed attempt stops inside its node; a fresh runtime over the
    // same stores recovers it.
    auto       fresh     = fixture.make_runtime();
    const auto recovered = fresh->reconnect("tenant:runtime", interrupted.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_detail(recovered);
    EXPECT_EQ(recovered.attempt(), 2U);
    EXPECT_EQ(recovered.output()["channels"]["value"]["value"],
              (json{{"decision", "approved"}}.dump()));
    EXPECT_EQ(interrupt_calls.load(), 3U);
    held.release();
    (void)original.wait();
}

TEST(ProgramRuntimeTest, RecoveryOfAReconciledAttemptResumesWithTheReconciledResult) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto      version =
        fixture.admit_document(recoverable_document("runtime-resume-hook-effect"));
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"result", "run input"}}, recoverable_grant(),
                           "trace-reconciled-crash", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);
    ASSERT_TRUE(interrupted.interrupt() && interrupted.interrupt()->pending_effect);
    const auto pending = *interrupted.interrupt()->pending_effect;
    ASSERT_EQ(fixture.runtime
                  ->reconcile("tenant:runtime", interrupted.run_id(),
                              ProgramEffectResolution{pending.call_id(),
                                                      ProgramEffectReconciliation::Unknown,
                                                      std::nullopt,
                                                      "trace-reconciled-crash-unknown",
                                                      {}})
                  .wait()
                  .status(),
              ProgramTerminalStatus::AmbiguousEffect);

    HeldResumedAttempt held;
    auto original = fixture.runtime->reconcile(
        "tenant:runtime", interrupted.run_id(),
        ProgramEffectResolution{pending.call_id(), ProgramEffectReconciliation::Completed,
                                json{{"result", "reconciled"}}, "trace-reconciled-crash-resume",
                                {}});
    ASSERT_TRUE(held.entered());

    auto       fresh     = fixture.make_runtime();
    const auto recovered = fresh->reconnect("tenant:runtime", interrupted.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_detail(recovered);
    EXPECT_EQ(recovered.attempt(), 2U);
    EXPECT_EQ(recovered.output()["channels"]["value"]["value"],
              (json{{"result", "reconciled"}}.dump()));
    held.release();
    (void)original.wait();
}

TEST(ProgramRuntimeTest, RecoveryOfAForkedAttemptResumesWithTheForkResumeValue) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto      version = fixture.admit_document(recoverable_document("runtime-resume-hook"));
    const auto      source  = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), recoverable_grant(), "trace-forked-crash-source", {}});
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted) << failure_detail(source);
    ASSERT_TRUE(source.checkpoint());

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-forked-crash";
    invocation.program_version_id = version.id();
    invocation.run_id             = "forked-crash-target";
    invocation.budget             = neograph::test::fork_share(source.remaining_budget());
    // The fork is recovered from its checkpoint by a fresh runtime, which a run
    // with child budgets allows at once (#421): it takes the source's.
    invocation.budget.max_child_depth    = source.remaining_budget().max_child_depth;
    invocation.budget.max_total_children = source.remaining_budget().max_total_children;
    invocation.input              = json{{"decision", "fork input"}};
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "test-forked-crash:1";
    invocation.correlation_id     = "trace-forked-crash-target";
    invocation.validate();

    HeldResumedAttempt held;
    auto               original = fixture.runtime->fork(
        ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
        invocation,
        ProgramResume{json{{"decision", "forked"}}, "trace-forked-crash-resume", {},
                      pending_call_id(source)});
    ASSERT_TRUE(held.entered());

    auto       fresh     = fixture.make_runtime();
    const auto recovered = fresh->reconnect("tenant:runtime", "forked-crash-target").wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_detail(recovered);
    EXPECT_EQ(recovered.output()["channels"]["value"]["value"],
              (json{{"decision", "forked"}}.dump()));
    held.release();
    (void)original.wait();
}

TEST(ProgramRuntimeTest, TypedPendingEffectPublishesOnceAndResumesByExactCallIdentity) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-effect-interrupt");

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-effect-interrupt", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    EXPECT_FALSE(interrupted.interrupt()->pending_input.has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    EXPECT_EQ(pending.call_id(), "call-effect-1");
    EXPECT_EQ(pending.payload()["effect"]["tool"], "search");
    EXPECT_EQ(pending.state(), ProgramPendingState::Awaiting);

    const auto effects = fixture.journal->load_effects("tenant:runtime", interrupted.run_id());
    ASSERT_EQ(effects.size(), 1U);
    EXPECT_EQ(effects.front().sequence(), 1U);
    EXPECT_EQ(effects.front().effect(), pending);

    fixture.recreate_catalog_and_runtime();
    const auto reconnected =
        fixture.runtime->reconnect("tenant:runtime", interrupted.run_id()).wait();
    EXPECT_EQ(reconnected.serialize_canonical(), interrupted.serialize_canonical());
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", interrupted.run_id()).size(), 1U);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted.run_id(),
            ProgramResume{json{{"result", "unused"}}, "trace-wrong-effect", {}, "wrong-call"});
        FAIL() << "wrong pending call identity was accepted";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_PENDING_ID_MISMATCH");
    }
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"result", "recorded"}}, "trace-effect-resume"))
            .wait();
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message + " " +
                                    resumed.failure()->witness.dump()
                              : "no failure detail");
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "recorded");
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto persisted = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(persisted.has_value());
    ASSERT_TRUE(persisted->pending_effect().has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Consumed);
    EXPECT_EQ(persisted->effect_sequence(), 1U);
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", interrupted.run_id()).size(), 1U);
}

TEST(ProgramRuntimeTest, UnknownNonIdempotentEffectBlocksCancelAndRedispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-effect-nonidempotent");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-effect-ambiguous", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    const auto run_id  = interrupted.run_id();

    auto ambiguous = fixture.runtime
                         ->reconcile("tenant:runtime", run_id,
                                     ProgramEffectResolution{pending.call_id(),
                                                             ProgramEffectReconciliation::Unknown,
                                                             std::nullopt,
                                                             "trace-effect-unknown",
                                                             {}})
                         .wait();
    EXPECT_EQ(ambiguous.status(), ProgramTerminalStatus::AmbiguousEffect);
    EXPECT_EQ(interrupt_calls.load(), 1U);
    auto persisted = fixture.journal->load("tenant:runtime", run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->continuation().state, ContinuationState::AmbiguousEffect);
    ASSERT_TRUE(persisted->pending_effect().has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Ambiguous);
    const auto ambiguous_journal = persisted->journal_head();
    ASSERT_EQ(fixture.journal->load_effects("tenant:runtime", run_id).size(), 1U);

    fixture.recreate_catalog_and_runtime();
    auto recovered = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_FALSE(recovered.cancel());
    EXPECT_EQ(fixture.journal->latest("tenant:runtime", run_id)->id, ambiguous_journal);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    auto completed = fixture.runtime
                         ->reconcile("tenant:runtime", run_id,
                                     ProgramEffectResolution{pending.call_id(),
                                                             ProgramEffectReconciliation::Completed,
                                                             json{{"result", "reconciled"}},
                                                             "trace-effect-reconciled",
                                                             {}})
                         .wait();
    EXPECT_EQ(completed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed.output()["channels"]["value"]["value"], "reconciled");
    EXPECT_EQ(interrupt_calls.load(), 2U);
    persisted = fixture.journal->load("tenant:runtime", run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Consumed);
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", run_id).size(), 1U);
}

TEST(ProgramRuntimeTest, CrossOwnerResumeIsNoOracleAndNeverReadsCheckpoint) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-owner-scope", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    checkpoint_store->arm(AdversarialCheckpointStore::Mode::Normal);

    const auto diagnostic_code = [&](std::string_view run_id) {
        try {
            (void)fixture.runtime->resume(
                "tenant:other", run_id,
                resume_for(interrupted, json{{"decision", "unused"}}, "trace-cross-owner"));
        } catch (const ProgramDiagnosticError& error) {
            return error.diagnostic().code;
        }
        return std::string{};
    };
    EXPECT_EQ(diagnostic_code(interrupted.run_id()), diagnostic_code("absent-run"));
    EXPECT_EQ(checkpoint_store->exact_loads(), 0U);
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RetriesOneSpuriousTerminalJournalConflict) {
    auto            journal = std::make_shared<ConflictOnceJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto      version = fixture.admit("runtime-completed");

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-cas-retry", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(journal->injected());
    const auto latest = journal->latest_sync("tenant:runtime", result.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
}

TEST(ProgramRuntimeTest, JournalCommitPrecedesCheckpointAndTerminalEventDelivery) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            sink    = std::make_shared<JournalObservingSink>(fixture.journal);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-journal-order", sink});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(sink->checkpoint_observed_committed.load());
    EXPECT_TRUE(sink->terminal_observed_committed.load());
}

TEST(ProgramRuntimeTest, ProgramEnvelopePreservesDirectCoreBehavior) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            program_handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-equivalence", {}});
    const auto program_result = program_handle.wait();

    const auto definition = program_document("runtime-completed")["root"]["definition"];
    auto topology = neograph::program::detail::RegistrySnapshotAccess::parse_local(fixture.registry,
                                                                                   definition);
    auto linked   = neograph::program::detail::RegistrySnapshotAccess::link_local(
        fixture.registry, std::move(topology), NodeContext{});
    auto core_registry =
        neograph::program::detail::RegistrySnapshotAccess::runtime_registry(fixture.registry);
    auto direct_checkpoints = std::make_shared<InMemoryCheckpointStore>();

    EngineResources engine_resources;
    engine_resources.registry = core_registry;
    auto direct_engine =
        GraphEngine::link(std::move(linked), EngineConfig{}, std::move(engine_resources));
    direct_engine->set_checkpoint_store(direct_checkpoints);
    RunConfig direct_config;
    direct_config.thread_id = "direct-equivalence";
    direct_config.input     = json::object();
    direct_config.max_steps = 20;
    std::vector<json> direct_events;
    const auto        direct_result =
        direct_engine->run_stream(direct_config, [&](const GraphEvent& event) {
            direct_events.push_back(typed_event_value(to_typed_event(event)));
        });

    std::vector<json> program_events;
    for (const auto& event : program_handle.events_after(0)) {
        if (event.kind == ProgramEventKind::Core) {
            program_events.push_back(typed_event_value(std::get<TypedGraphEvent>(event.payload)));
        }
    }
    ASSERT_TRUE(program_result.checkpoint().has_value());
    const auto direct_checkpoint = direct_checkpoints->load_by_id(direct_result.checkpoint_id);
    const auto program_checkpoint =
        fixture.checkpoints->load_by_id(program_result.checkpoint()->checkpoint_id);

    EXPECT_EQ(program_result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(direct_result.status(), RunStatus::Completed);
    EXPECT_EQ(program_result.output(), direct_result.output);
    EXPECT_EQ(program_result.execution_trace(), direct_result.execution_trace);
    EXPECT_EQ(program_events, direct_events);
    // This topology never dispatches a provider: reports stay missing, while
    // Program's separate consumed model-token authority remains known zero.
    EXPECT_FALSE(direct_result.usage.total.has_value());
    EXPECT_TRUE(direct_result.provider_outcomes.empty());
    EXPECT_EQ(program_result.usage().model_tokens, 0U);
    ASSERT_TRUE(program_checkpoint.has_value());
    ASSERT_TRUE(direct_checkpoint.has_value());
    EXPECT_EQ(program_result.checkpoint()->checkpoint_schema_version,
              direct_checkpoint->schema_version);
    EXPECT_EQ(program_checkpoint->channel_values, direct_checkpoint->channel_values);
    EXPECT_EQ(program_checkpoint->channel_versions, direct_checkpoint->channel_versions);
    EXPECT_EQ(program_checkpoint->current_node, direct_checkpoint->current_node);
    EXPECT_EQ(program_checkpoint->next_nodes, direct_checkpoint->next_nodes);
    EXPECT_EQ(program_checkpoint->interrupt_phase, direct_checkpoint->interrupt_phase);
    EXPECT_EQ(program_checkpoint->barrier_state, direct_checkpoint->barrier_state);
    EXPECT_EQ(program_checkpoint->step, direct_checkpoint->step);
    EXPECT_EQ(completed_calls.load(), 2U);
}

// ---------------------------------------------------------------------------
// Stores shared by the join (#380) and plan parallel/quorum (#384, #386, #387)
// pause tests. They do not need QuickJS, so they live outside its block.
// ---------------------------------------------------------------------------

namespace {
// The run and checkpoint stores of a join test: in memory, or SQLite files
// that are removed afterwards.
struct JoinTestStores {
    explicit JoinTestStores(bool durable) {
        if (!durable) return;
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE) && \
    defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT)
        directory = neograph::graph::test_support::unique_temp_path("neograph-runtime-paused-join-");
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("cannot reserve test directory");
        transitions =
            std::make_shared<SQLiteProgramTransitionStore>((directory / "program.db").string());
        checkpoints =
            std::make_shared<SqliteCheckpointStore>((directory / "checkpoints.db").string());
#endif
    }
    ~JoinTestStores() {
        transitions.reset();
        checkpoints.reset();
        std::error_code ignored;
        if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
    }
    JoinTestStores(const JoinTestStores&)            = delete;
    JoinTestStores& operator=(const JoinTestStores&) = delete;

    std::filesystem::path                   directory;
    TransitionStoreHandle transitions;
    std::shared_ptr<CheckpointStore>        checkpoints;
};

std::vector<bool> join_test_store_kinds() {
    std::vector<bool> durable{false};
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE) && \
    defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT)
    durable.push_back(true);
#endif
    return durable;
}
}  // namespace

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
std::string javascript_runtime_source(std::string node_type, std::string body);
RunBudget javascript_budget(std::uint64_t max_concurrency        = 2,
                            std::uint64_t max_program_operations = 32,
                            std::uint64_t max_child_depth        = 0,
                            std::uint64_t max_total_children     = 0);

TEST(ProgramRuntimeTest, JavaScriptGeneratorPreservesLocalYieldValue) {
    const auto source = ProgramSource::from_javascript("test:generator-lifetime.js",
                                                       R"JS(
            export function* main(input) {
                const command = {protocol_version: 1, op: "call_core", name: "main",
                                 input: {requested: input.requested}};
                yield command;
            }
        )JS");

    const auto step = std::async(std::launch::async, [source] {
                          auto generator = neograph::program::detail::JavaScriptGenerator::open(
                              source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
                          if (!generator)
                              throw std::runtime_error("JavaScript generator was not exported");
                          std::optional<json> response;
                          return generator->next(std::move(response));
                      }).get();
    EXPECT_FALSE(step.done);
    EXPECT_EQ(step.value, (json{{"protocol_version", 1},
                                {"op", "call_core"},
                                {"name", "main"},
                                {"input", json{{"requested", "draft"}}}}));
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorLoadsReceiptBoundSealedModule) {
    const std::string module_name   = "sealed:command-input@1.0.0";
    const std::string module_source = R"JS(
        export function commandInput(input) {
            return {requested: input.requested + "-sealed"};
        }
    )JS";
    const auto        source        = ProgramSource::from_javascript(
        "test:generator-sealed-module.js",
        "import { commandInput } from \"sealed:command-input@1.0.0\";\n"
                      "export function* main(input) {\n"
                      "  yield ng.callCore(\"main\", commandInput(input), \"sealed:1\");\n"
                      "}\n",
        {{module_name, digest('a')}}, {}, {{module_name, digest('a'), module_source}});

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();

    ASSERT_TRUE(step.command.has_value());
    EXPECT_EQ(step.command->source_site(), "sealed:1");
    EXPECT_EQ(step.command->to_json().at("arguments").at("input"),
              (json{{"requested", "draft-sealed"}}));
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorExposesOnlyControlCommandBinding) {
    const auto source = ProgramSource::from_javascript("test:generator-host-surface.js",
                                                       R"JS(
            export function* main() {
                return {
                    graph: typeof ng.graph,
                    callCore: typeof ng.callCore,
                    frozen: Object.isFrozen(ng),
                };
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    EXPECT_TRUE(step.done);
    EXPECT_EQ(step.value,
              (json{{"graph", "undefined"}, {"callCore", "function"}, {"frozen", true}}));
}

TEST(ProgramRuntimeTest, JavaScriptCapabilityManifestMatchesTheInstalledCommandKernel) {
    const auto source = ProgramSource::from_javascript(
        "test:command-capability-manifest.js",
        "export function* main() { return Object.keys(ng).sort(); }");
    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    ASSERT_TRUE(step.done);

    std::vector<std::string> installed;
    for (const auto& name : step.value) installed.push_back(name.get<std::string>());
    std::vector<std::string> declared;
    const auto manifest = javascript_authoring_capability_manifest();
    for (const auto& method : manifest.at("main").at("command_methods"))
        declared.push_back(method.at("name").get<std::string>());
    for (const auto& property : manifest.at("main").at("ng_properties"))
        declared.push_back(property.at("name").get<std::string>());
    std::sort(declared.begin(), declared.end());
    EXPECT_EQ(installed, declared);
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorProducesSealedTypedCommandEnvelope) {
    const auto source = ProgramSource::from_javascript("test:typed-command.js",
                                                       R"JS(
            export function* main() {
                const call = ng.callCore("main", {requested: "draft"}, "main:12");
                if (!Object.isFrozen(call)) {
                    throw new Error("command was not sealed");
                }
                yield call;
                return {};
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    ASSERT_FALSE(step.done);
    ASSERT_TRUE(step.command.has_value());
    EXPECT_EQ(step.command->kind(), JavaScriptCommandKind::CallCore);
    EXPECT_EQ(step.command->import_slot(), JAVASCRIPT_IMPORT_SLOT_CALL_CORE);
    EXPECT_EQ(step.command->source_site(), "main:12");
    EXPECT_EQ(step.value, step.command->to_json());
}

TEST(ProgramRuntimeTest, JavaScriptRetainedCommandsShareTheQuickJsMemoryCeiling) {
    const auto              source = ProgramSource::from_javascript("test:native-live-commands.js",
                                                                    R"JS(
            export function* main() {
                const retained = [];
                for (let index = 0; index < 100000; ++index) {
                    retained.push(ng.callCore(
                        "main", {index, padding: "x".repeat(64)}, "retained:" + index));
                }
                yield retained[0];
            }
        )JS");
    JavaScriptCompileLimits limits;
    limits.memory_limit_bytes  = 2u * 1024u * 1024u;
    limits.max_stack_bytes     = 128u * 1024u;
    limits.max_interrupt_polls = 100'000'000u;
    auto generator =
        neograph::program::detail::JavaScriptGenerator::open(source, json::object(), limits);
    ASSERT_TRUE(generator.has_value());
    try {
        (void)generator->next();
        FAIL() << "expected cumulative native-memory rejection";
    } catch (const neograph::program::detail::JavaScriptCompileError& error) {
        EXPECT_EQ(error.code(), "P_JS_RESOURCE_LIMIT");
        ASSERT_TRUE(error.witness().contains("allocator_limit_denied_count"));
        EXPECT_GT(error.witness().at("allocator_limit_denied_count").get<std::size_t>(), 0U);
    }
}

TEST(ProgramRuntimeTest, JavaScriptCommandFinalizersReleaseNativeMemoryCharges) {
    const auto source = ProgramSource::from_javascript("test:native-command-release.js",
                                                       R"JS(
            export function* main() {
                for (let batch = 0; batch < 2000; ++batch) {
                    let retained = [];
                    for (let index = 0; index < 16; ++index) {
                        retained.push(ng.callCore(
                            "main", {index, padding: "x".repeat(64)}, "temporary:" + index));
                    }
                    retained = null;
                }
                yield ng.callCore("main", {}, "survived");
            }
        )JS");
    JavaScriptCompileLimits limits;
    limits.memory_limit_bytes  = 2u * 1024u * 1024u;
    limits.max_stack_bytes     = 128u * 1024u;
    limits.max_interrupt_polls = 100'000'000u;
    // This is a finalizer/accounting stress test, not a wall-time test. Keep
    // the 32,000 temporary native commands and tight 2 MiB memory ceiling,
    // while allowing for sanitizer allocator overhead on Windows runners.
    limits.max_wall_time_ms    = 180'000u;
    auto generator =
        neograph::program::detail::JavaScriptGenerator::open(source, json::object(), limits);
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    ASSERT_TRUE(step.command.has_value());
    EXPECT_EQ(step.command->source_site(), "survived");
}

TEST(ProgramRuntimeTest, JavaScriptRuntimeRejectsImportsBeforeQuickJsEvaluation) {
    const auto source = ProgramSource::from_javascript(
        "test:runtime-import.js",
        "import \"sealed:approved@1.0.0\";\n"
        "export function* main() { yield {}; }\n",
        {{"sealed:approved@1.0.0", std::string("sha256:") + std::string(64, 'a')}});
    try {
        (void)neograph::program::detail::JavaScriptGenerator::open(source, json::object(),
                                                                   JavaScriptCompileLimits{});
        FAIL() << "expected pre-evaluation import rejection";
    } catch (const neograph::program::detail::JavaScriptCompileError& error) {
        EXPECT_EQ(error.code(), "P_JS_IMPORT_UNAVAILABLE");
        ASSERT_TRUE(error.source_span().has_value());
        EXPECT_EQ(error.source_span()->line_begin, 1U);
        EXPECT_EQ(error.witness().at("receipt_bound_source_available"), false);
    }
}

TEST(ProgramRuntimeTest, JavaScriptRuntimeRejectsDynamicImportsBeforeQuickJsEvaluation) {
    const auto source = ProgramSource::from_javascript("test:runtime-dynamic-import.js",
                                                       "export function* main() {\n"
                                                       "  yield import(\"sealed:unknown@1.0.0\");\n"
                                                       "}\n");
    try {
        (void)neograph::program::detail::JavaScriptGenerator::open(source, json::object(),
                                                                   JavaScriptCompileLimits{});
        FAIL() << "expected pre-evaluation dynamic import rejection";
    } catch (const neograph::program::detail::JavaScriptCompileError& error) {
        EXPECT_EQ(error.code(), "P_JS_IMPORT_UNAVAILABLE");
        ASSERT_TRUE(error.source_span().has_value());
        EXPECT_EQ(error.source_span()->line_begin, 2U);
    }
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorRejectsForgedCommandEnvelope) {
    const auto source = ProgramSource::from_javascript("test:forged-command.js",
                                                       R"JS(
            export function* main() {
                yield {
                    protocol_version: 1,
                    kind: "call_core",
                    import_slot: 0,
                    source_site: "main:1",
                    arguments: {name: "main", input: {}}
                };
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    EXPECT_FALSE(step.done);
    EXPECT_FALSE(step.command.has_value());
}

TEST(ProgramRuntimeTest, JavaScriptNgExposesClosedConstructorSet) {
    const auto source = ProgramSource::from_javascript("test:command-constructors.js",
                                                       R"JS(
            export function* main() {
                const call = ng.callCore("main", {x: 1}, "call:1");
                const spawn = ng.spawn("child", {x: 2}, "spawn:1");
                yield call;
                yield spawn;
                yield ng.await(spawn, 50, "await:1");
                yield ng.join([call, spawn], "all", undefined, "join:1");
                yield ng.all([call, spawn], "all:1");
                yield ng.race([call, spawn], "race:1");
                yield ng.quorum([call, spawn], 1, "quorum:1");
                yield ng.emit({kind: "event"}, "emit:1");
                yield ng.checkpoint({checkpoint: true}, "checkpoint:1");
                yield ng.cancelScope("run", "stop", "cancel:1");
                yield ng.hostCapability(42, {request: "approved"}, "host:1");
                return {};
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const std::vector<JavaScriptCommandKind> expected = {
        JavaScriptCommandKind::CallCore,      JavaScriptCommandKind::Spawn,
        JavaScriptCommandKind::Await,         JavaScriptCommandKind::Join,
        JavaScriptCommandKind::Join,          JavaScriptCommandKind::Join,
        JavaScriptCommandKind::Join,          JavaScriptCommandKind::Emit,
        JavaScriptCommandKind::Checkpoint,    JavaScriptCommandKind::CancelScope,
        JavaScriptCommandKind::HostCapability};
    for (const auto kind : expected) {
        const auto step = generator->next(json::object());
        ASSERT_FALSE(step.done);
        ASSERT_TRUE(step.command.has_value());
        EXPECT_EQ(step.command->kind(), kind);
        EXPECT_EQ(step.value, step.command->to_json());
    }
    const auto done = generator->next(json::object());
    EXPECT_TRUE(done.done);
    EXPECT_EQ(done.value, json::object());
}

TEST(ProgramRuntimeTest, JavaScriptControlCancellationInterruptsBeforeAnyLateDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main() {
                while (true) {}
            }
        )JS");

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-javascript-cancel", {}});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Cancelled)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                             : "no failure detail");
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorExecutesYieldedCoreCommand) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const core = yield ng.callCore("main", {requested: input.requested});
                return {requested: input.requested, value: core.value};
            }
        )JS");
    const auto bundle = fixture.store->get_bundle("tenant:runtime", version.bundle_id());
    ASSERT_TRUE(bundle.has_value());
    const auto control_source = bundle->control_source();
    ASSERT_TRUE(control_source.has_value());
    auto restored_generator = neograph::program::detail::JavaScriptGenerator::open(
        *control_source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
    ASSERT_TRUE(restored_generator.has_value());
    const auto restored_step = restored_generator->next();
    ASSERT_FALSE(restored_step.done);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "draft"}}, grant(), "trace-javascript-control", {}});

    EXPECT_EQ(version.execution_guarantee(), ExecutionGuarantee::Unmanaged);
    const auto failure = result.failure();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (failure ? failure->code + ": " + failure->message + " " + failure->witness.dump()
                    : "no failure detail");
    EXPECT_EQ(result.output(), (json{{"requested", "draft"}, {"value", "completed"}}));
    EXPECT_EQ(result.usage().program_operations, 1U);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptCommandJournalPersistsCoordinateAndResult) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "journal:1");
                return {value: output.value};
            }
        )JS");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"requested", "journal"}}, grant(), "trace-javascript-command-journal", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(completed_calls.load(), 1U);

    const auto entries =
        fixture.journal->load_javascript_commands("tenant:runtime", result.run_id());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].command_ordinal(), 1U);
    EXPECT_TRUE(entries[0].pending());
    EXPECT_EQ(entries[0].coordinate_id(), entries[1].coordinate_id());
    EXPECT_TRUE(entries[1].completed());
    EXPECT_EQ(entries[1].terminal_result()->at("status"), "completed");
    EXPECT_EQ(entries[1].command().source_site(), "journal:1");
}

TEST(ProgramRuntimeTest, JavaScriptCompletedHeadFreshRuntimeReplaysRecordedResult) {
    completed_calls.store(0);
    auto            journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "crash:result");
                return {value: output.value};
            }
        )JS");

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"requested", "recorded"}}, grant(), "trace-javascript-result-crash", {}});
    if (!journal->wait_for_result(std::chrono::seconds(2))) {
        const auto failed = original.wait();
        FAIL() << (failed.failure() ? failed.failure()->code + ": " + failed.failure()->message +
                                          " " + failed.failure()->witness.dump()
                                    : "command result was not journaled");
    }
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto durable_commands =
        journal->load_javascript_commands_sync("tenant:runtime", original.run_id(), 0);
    ASSERT_EQ(durable_commands.size(), 2U);
    EXPECT_TRUE(durable_commands.back().completed());
    EXPECT_FALSE(journal->load_execution_lease_sync(
        "tenant:runtime", original.run_id()));
    const auto durable_snapshot = journal->load_sync("tenant:runtime", original.run_id());
    ASSERT_TRUE(durable_snapshot.has_value());
    ASSERT_TRUE(durable_snapshot->exact_checkpoint().has_value());
    const auto durable_events = journal->load_events_sync("tenant:runtime", original.run_id(), 0);
    ASSERT_FALSE(durable_events.empty());
    EXPECT_EQ(durable_snapshot->event_sequence(), durable_events.back().sequence);
    EXPECT_TRUE(std::any_of(durable_events.begin(), durable_events.end(), [&](const auto& event) {
        return event.kind == ProgramEventKind::CheckpointPublished &&
               std::get<ProgramCheckpointEvent>(event.payload).checkpoint ==
                   *durable_snapshot->exact_checkpoint();
    }));

    auto       fresh_runtime = fixture.make_runtime();
    const auto replayed      = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(replayed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(replayed.output(), (json{{"value", "completed"}}));
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto replayed_events = journal->load_events_sync("tenant:runtime", original.run_id(), 0);
    for (const auto& durable_event : durable_events) {
        EXPECT_EQ(std::count_if(replayed_events.begin(), replayed_events.end(),
                                [&](const auto& replayed_event) {
                                    return replayed_event.id == durable_event.id;
                                }),
                  1);
    }

    journal->release_result();
    (void)original.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, GraphMigrationFencesSourceAndExactResumesGenerationTwo) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto version = fixture.admit("runtime-short-blocking");

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-source", {}});
    auto target = fixture.runtime->migrate_graph(
        source, ProgramGraphMigrationTarget{version.id(), "graph-migration-target", {}});
    const auto result = target.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                             : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    EXPECT_EQ(lineage->active_run_record_id(), target.snapshot().id());
    EXPECT_EQ(lineage->remaining_budget(), result.remaining_budget());
    const auto generation = fixture.journal->load_generation(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->graph_migration_receipt());
    EXPECT_FALSE(generation->replacement_receipt());
    EXPECT_EQ(generation->graph_migration_receipt()->capsule().source_run_id(),
              source.run_id());
    const auto durable_capsule = fixture.journal->load_graph_migration_capsule(
        "tenant:runtime", source.run_id(),
        generation->graph_migration_receipt()->capsule().source_lineage_head_id());
    ASSERT_TRUE(durable_capsule);
    EXPECT_EQ(durable_capsule->id(),
              generation->graph_migration_receipt()->capsule().id());
    const auto initial = fixture.journal->load_generation_initial_publication(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(initial->run_record.exact_checkpoint());
    ASSERT_TRUE(initial->run_record.exact_checkpoint_content_id());
    const auto target_checkpoint = fixture.checkpoints->load_by_id(
        initial->run_record.exact_checkpoint()->checkpoint_id);
    ASSERT_TRUE(target_checkpoint);
    EXPECT_EQ(*initial->run_record.exact_checkpoint_content_id(),
              graph_migration_checkpoint_content_id(*target_checkpoint));

    const auto reconnected = fixture.runtime->reconnect(
        "tenant:runtime", source.run_id()).wait();
    EXPECT_EQ(reconnected.id(), result.id());
    EXPECT_EQ(reconnected.run_id(), target.run_id());
    const auto retried = fixture.runtime
                             ->migrate_graph(
                                 source,
                                 ProgramGraphMigrationTarget{
                                     version.id(), "graph-migration-target", {}})
                             .wait();
    EXPECT_EQ(retried.id(), result.id());
    EXPECT_EQ(retried.run_id(), target.run_id());
    (void)source.wait();
    EXPECT_EQ(followup_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, GraphSemanticMigrationAdapterAdmitsShapePreservingSuccessor) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(1);

    const auto source_version = fixture.admit("runtime-short-blocking");
    auto       target_document = program_document("runtime-short-blocking");
    // The target has a different immutable Core definition/plan identity, but
    // preserves every checkpointed channel, node id, edge, and barrier member.
    target_document["root"]["definition"]["nodes"]["work"]["migration_epoch"] = 2;
    const auto target_version = fixture.admit_document(std::move(target_document));
    ASSERT_NE(source_version.id(), target_version.id());
    ASSERT_NE(source_version.core_materialization_receipt().plans.front().compiled_plan_identity,
              target_version.core_materialization_receipt().plans.front().compiled_plan_identity);

    const auto source_bundle = fixture.store->get_bundle("tenant:runtime", source_version.bundle_id());
    const auto target_bundle = fixture.store->get_bundle("tenant:runtime", target_version.bundle_id());
    ASSERT_TRUE(source_bundle);
    ASSERT_TRUE(target_bundle);
    EXPECT_FALSE(MigrationPlan::between(source_version, *source_bundle, target_version, *target_bundle)
                     .is_compatible());
    const auto adapter = GraphSemanticMigrationAdapter::prepare(
        source_version, *source_bundle, target_version, *target_bundle);
    EXPECT_TRUE(adapter.binds(source_version, *source_bundle, target_version, *target_bundle));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), grant(), "trace-semantic-graph-migration-source", {}});
    EXPECT_THROW(
        (void)fixture.runtime->migrate_graph(
            source, ProgramGraphMigrationTarget{target_version.id(),
                                                "semantic-graph-migration-without-adapter", {}}),
        ProgramDiagnosticError);
    auto target = fixture.runtime->migrate_graph(
        source, ProgramGraphMigrationTarget{target_version.id(), "semantic-graph-migration-target",
                                             {}, adapter});
    const auto result = target.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                             : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);

    const auto lineage = fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    const auto generation = fixture.journal->load_generation(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    const auto receipt = generation->graph_migration_receipt();
    ASSERT_TRUE(receipt);
    ASSERT_TRUE(receipt->semantic_adapter());
    EXPECT_EQ(receipt->semantic_adapter()->id(), adapter.id());
    EXPECT_EQ(receipt->target_version().id(), target_version.id());
    const auto parsed = ProgramGraphMigrationReceipt::parse(receipt->serialize_canonical());
    ASSERT_TRUE(parsed.semantic_adapter());
    EXPECT_EQ(parsed.semantic_adapter()->id(), adapter.id());
    EXPECT_EQ(parsed.target_checkpoint().metadata.at("semantic_migration_adapter_id"), adapter.id());

    auto recovered = fixture.make_runtime();
    EXPECT_EQ(recovered->reconnect("tenant:runtime", source.run_id()).wait().status(),
              ProgramTerminalStatus::Completed);
    (void)source.wait();
}

// #362: metadata is outside the migration shape, and the approving host sees
// every metadata change through metadata_changes().
TEST(ProgramRuntimeTest, GraphSemanticMigrationAdapterListsMetadataChanges) {
    AdmittedRuntime fixture(1);
    auto            source_document = program_document("runtime-short-blocking");
    source_document["root"]["definition"]["metadata"] =
        json{{"owner", "agent:planner"}, {"limits", json{{"depth", 2}, {"total", 4}}},
             {"retired", true}};
    auto target_document = program_document("runtime-short-blocking");
    target_document["root"]["definition"]["metadata"] =
        json{{"owner", "agent:reviewer"}, {"limits", json{{"depth", 2}, {"total", 8}}},
             {"requirements", json::array({"REQ-12"})}};
    const auto source_version = fixture.admit_document(std::move(source_document));
    const auto target_version = fixture.admit_document(std::move(target_document));
    const auto source_bundle =
        fixture.store->get_bundle("tenant:runtime", source_version.bundle_id());
    const auto target_bundle =
        fixture.store->get_bundle("tenant:runtime", target_version.bundle_id());
    ASSERT_TRUE(source_bundle);
    ASSERT_TRUE(target_bundle);
    const auto adapter = GraphSemanticMigrationAdapter::prepare(
        source_version, *source_bundle, target_version, *target_bundle);
    EXPECT_TRUE(adapter.binds(source_version, *source_bundle, target_version, *target_bundle));

    std::vector<std::pair<std::string, std::string>> changes;
    for (const auto& change : adapter.metadata_changes())
        changes.emplace_back(change.kind, change.json_pointer);
    EXPECT_EQ(changes, (std::vector<std::pair<std::string, std::string>>{
                           {"changed", "/metadata/limits/total"},
                           {"changed", "/metadata/owner"},
                           {"added", "/metadata/requirements"},
                           {"removed", "/metadata/retired"}}));
    // The list is evidence for the approving host, not part of the adapter.
    const auto parsed = GraphSemanticMigrationAdapter::parse(adapter.serialize_canonical());
    EXPECT_EQ(parsed.id(), adapter.id());
    EXPECT_TRUE(parsed.metadata_changes().empty());

    // Same topology without metadata: the shape identity does not change.
    const auto plain_version = fixture.admit("runtime-short-blocking");
    const auto plain_bundle =
        fixture.store->get_bundle("tenant:runtime", plain_version.bundle_id());
    ASSERT_TRUE(plain_bundle);
    const auto to_plain = GraphSemanticMigrationAdapter::prepare(
        source_version, *source_bundle, plain_version, *plain_bundle);
    EXPECT_EQ(to_plain.topology_shape_id(), adapter.topology_shape_id());
    EXPECT_EQ(to_plain.metadata_changes().size(), 3U);
}

// #363: an any_of join holds barrier state like a legacy barrier, so the
// adapter lists it among the barriers its projection may carry.
TEST(ProgramRuntimeTest, GraphSemanticMigrationAdapterListsSlotJoinsAsBarriers) {
    AdmittedRuntime fixture(1);
    const auto      slot_document = [](int epoch) {
        auto document   = program_document("runtime-short-blocking");
        auto definition = document["root"]["definition"];
        definition["nodes"] =
            json{{"work", json{{"type", "runtime-short-blocking"}, {"epoch", epoch}}},
                 {"left", json{{"type", "runtime-followup"}}},
                 {"right", json{{"type", "runtime-followup"}}},
                 {"join",
                  json{{"type", "runtime-followup"},
                       {"barrier", json{{"wait_for",
                                         json::array({json{{"any_of",
                                                            json::array({"left", "right"})}}})}}}}}};
        definition["edges"] = json::array({
            json{{"from", "__start__"}, {"to", "work"}},
            json{{"from", "work"}, {"to", "left"}},
            json{{"from", "work"}, {"to", "right"}},
            json{{"from", "left"}, {"to", "join"}},
            json{{"from", "right"}, {"to", "join"}},
            json{{"from", "join"}, {"to", "__end__"}},
        });
        document["root"]["definition"] = std::move(definition);
        return document;
    };
    const auto source_version = fixture.admit_document(slot_document(1));
    const auto target_version = fixture.admit_document(slot_document(2));
    const auto source_bundle =
        fixture.store->get_bundle("tenant:runtime", source_version.bundle_id());
    const auto target_bundle =
        fixture.store->get_bundle("tenant:runtime", target_version.bundle_id());
    ASSERT_TRUE(source_bundle);
    ASSERT_TRUE(target_bundle);
    const auto adapter = GraphSemanticMigrationAdapter::prepare(
        source_version, *source_bundle, target_version, *target_bundle);
    const auto stored = json::parse(adapter.serialize_canonical());
    EXPECT_EQ(stored.at("barrier_names"), json::array({"join"}));
    EXPECT_EQ(GraphSemanticMigrationAdapter::parse(adapter.serialize_canonical()).id(), adapter.id());
}

TEST(ProgramRuntimeTest, GraphSemanticMigrationAdapterRejectsFrontierRename) {
    AdmittedRuntime fixture(1);
    const auto source_version = fixture.admit("runtime-short-blocking");
    auto       target_document = program_document("runtime-short-blocking");
    auto       definition = target_document["root"]["definition"];
    definition["nodes"] = json{{"work", json{{"type", "runtime-short-blocking"}}},
                               {"after", json{{"type", "runtime-followup"}}}};
    definition["edges"] = json::array({
        json{{"from", "__start__"}, {"to", "work"}},
        json{{"from", "work"}, {"to", "after"}},
        json{{"from", "after"}, {"to", "__end__"}},
    });
    target_document["root"]["definition"] = std::move(definition);
    const auto target_version = fixture.admit_document(std::move(target_document));
    const auto source_bundle = fixture.store->get_bundle("tenant:runtime", source_version.bundle_id());
    const auto target_bundle = fixture.store->get_bundle("tenant:runtime", target_version.bundle_id());
    ASSERT_TRUE(source_bundle);
    ASSERT_TRUE(target_bundle);

    EXPECT_THROW((void)GraphSemanticMigrationAdapter::prepare(
                     source_version, *source_bundle, target_version, *target_bundle),
                 std::invalid_argument);
}

TEST(ProgramRuntimeTest, HeldSourceWallTimeUsesWholeAttemptElapsed) {
    using Clock = std::chrono::steady_clock;
    using neograph::program::detail::remaining_attempt_wall_time;
    const auto started = Clock::time_point(std::chrono::microseconds(900));
    // The journal clock crosses a millisecond boundary during this 100us hold.
    // Adding its one-ms delta to the rounded attempt debit would charge 501ms,
    // although both release and terminal settlement see only 500ms elapsed.
    const auto held = started + std::chrono::microseconds(500050);
    const auto released = held + std::chrono::microseconds(100);
    const auto terminal = released + std::chrono::microseconds(100);
    const auto at_hold = remaining_attempt_wall_time(1000, started, held);
    const auto at_release = remaining_attempt_wall_time(1000, started, released);
    const auto at_terminal = remaining_attempt_wall_time(1000, started, terminal);
    EXPECT_EQ(at_hold, 500U);
    EXPECT_EQ(at_release, 500U);
    EXPECT_LE(at_terminal, at_release);
    EXPECT_EQ(remaining_attempt_wall_time(
                  1000, started, started + std::chrono::milliseconds(1000)), 0U);
    EXPECT_EQ(remaining_attempt_wall_time(
                  1000, started, started + std::chrono::milliseconds(1001)), 0U);
}

TEST(ProgramRuntimeTest, GraphMigrationTargetSaveFailureResumesHeldSource) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(AdversarialCheckpointStore::Mode::FailMigrationTargetSave);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    std::optional<ProgramTransitionPublication> release;
    journal->after_publication = [&](const ProgramTransitionPublication& publication) {
        if (publication.run_record.continuation().state == ContinuationState::Running &&
            publication.journal_record.sequence > 1)
            release = publication;
    };
    AdmittedRuntime fixture(4, checkpoints, journal);
    const auto version = fixture.admit("runtime-short-blocking");

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-failed", {}});
    EXPECT_THROW(
        (void)fixture.runtime->migrate_graph(
            source,
            ProgramGraphMigrationTarget{version.id(), "graph-migration-failed-target", {}}),
        std::runtime_error);

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message +
                                   " @" + result.failure()->operation_id
                             : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    EXPECT_EQ(checkpoints->migration_target_save_failures(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_EQ(lineage->active_run_record_id(), source.snapshot().id());
    ASSERT_TRUE(release);
    EXPECT_EQ(result.remaining_budget(), lineage->remaining_budget());
    EXPECT_LE(result.remaining_budget().wall_time_ms,
              release->run_record.remaining_budget().wall_time_ms);
    EXPECT_LT(release->run_record.remaining_budget().wall_time_ms, grant().wall_time_ms);
    EXPECT_LE(result.remaining_budget().max_core_steps,
              release->run_record.remaining_budget().max_core_steps);
    EXPECT_EQ(result.usage().wall_time_ms + result.remaining_budget().wall_time_ms,
              grant().wall_time_ms);
    EXPECT_FALSE(journal->load_execution_lease_sync("tenant:runtime", source.run_id()));
    EXPECT_FALSE(fixture.journal->load_graph_migration_capsule(
        "tenant:runtime", source.run_id(), lineage->id()));
    EXPECT_FALSE(fixture.journal->load(
        "tenant:runtime", "graph-migration-failed-target"));
}

TEST(ProgramRuntimeTest, GraphMigrationRejectsContentChangedAtConsumingLoad) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(
        AdversarialCheckpointStore::Mode::WrongMigrationContentAtConsumingLoad);
    AdmittedRuntime fixture(1, checkpoints);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-content-race", {}});

    const auto result = fixture.runtime
                            ->migrate_graph(
                                source,
                                ProgramGraphMigrationTarget{
                                    version.id(), "graph-migration-content-race-target", {}})
                            .wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    (void)source.wait();
}

TEST(ProgramRuntimeTest, UnreadableSafePointCommitFencesSourceDurably) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-uncertain-source", {}});
    journal->crash_after_next_graph_safe_point_with_unreadable_commit();

    EXPECT_THROW(
        (void)fixture.runtime->migrate_graph(
            source,
            ProgramGraphMigrationTarget{
                version.id(), "graph-migration-uncertain-target", {}}),
        std::runtime_error);
    const auto source_result = source.wait();
    EXPECT_EQ(source_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
    const auto durable = journal->load_sync("tenant:runtime", source.run_id());
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->continuation().state, ContinuationState::Cancelled);
    ASSERT_TRUE(durable->exact_checkpoint());
    ASSERT_TRUE(durable->exact_checkpoint_content_id());
    EXPECT_FALSE(journal->load_sync(
        "tenant:runtime", "graph-migration-uncertain-target"));

    auto fresh_runtime = fixture.make_runtime();
    EXPECT_EQ(fresh_runtime->reconnect("tenant:runtime", source.run_id()).wait().status(),
              ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, CancellationDuringGraphMigrationFencePublishesNoSuccessor) {
    blocking_calls.store(0);
    blocking_active.store(0);
    AdmittedRuntime fixture(2);
    const auto version = fixture.admit("runtime-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-cancel", {}});
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);

    auto migration = std::async(std::launch::async, [&] {
        return fixture.runtime->migrate_graph(
            source,
            ProgramGraphMigrationTarget{version.id(), "graph-migration-cancelled-target", {}});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(source.cancel());
    EXPECT_THROW((void)migration.get(), std::runtime_error);
    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_FALSE(fixture.journal->load(
        "tenant:runtime", "graph-migration-cancelled-target"));
}

TEST(ProgramRuntimeTest, CancellationAfterHeldSourceLinearizesBehindMigrationCommit) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(AdversarialCheckpointStore::Mode::DelayMigrationTargetSave);
    AdmittedRuntime fixture(1, checkpoints);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-linearized-cancel", {}});
    auto migration = std::async(std::launch::async, [&] {
        return fixture.runtime
            ->migrate_graph(
                source,
                ProgramGraphMigrationTarget{
                    version.id(), "graph-migration-linearized-target", {}})
            .wait();
    });
    for (unsigned attempt = 0;
         !checkpoints->migration_target_save_started() && attempt < 3000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(checkpoints->migration_target_save_started());

    EXPECT_FALSE(source.cancel());
    const auto target = migration.get();
    EXPECT_EQ(target.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    const auto generation = fixture.journal->load_generation(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    const auto receipt = generation->graph_migration_receipt();
    ASSERT_TRUE(receipt);
    const auto source_record = fixture.journal->load(
        "tenant:runtime", source.run_id());
    const auto source_head = fixture.journal->load_lineage_head(
        "tenant:runtime", lineage->lineage_id(),
        receipt->capsule().source_lineage_head_id());
    const auto initial = fixture.journal->load_generation_initial_publication(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(source_record);
    ASSERT_TRUE(source_head);
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->run_record.invocation().budget,
              program_replacement_remaining_budget(
                  *source_record, *source_head,
                  initial->run_record.created_at_ms()));
    EXPECT_LE(initial->run_record.invocation().budget.wall_time_ms + 150,
              source_record->remaining_budget().wall_time_ms);
    (void)source.wait();
}

TEST(ProgramRuntimeTest, RemoteReconnectCannotStealLiveRootExecutionLease) {
    blocking_calls.store(0);
    blocking_active.store(0);
    TransitionStoreHandle backend = std::make_shared<InMemoryProgramTransitionStore>();
    auto local_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    AdmittedRuntime fixture(2, {}, local_view);
    const auto version = fixture.admit("runtime-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-live-root-execution-lease", {}});
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);
    const auto lease = backend->load_execution_lease(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lease);

    auto remote_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto remote_runtime = fixture.make_runtime_with_transitions(remote_view);
    try {
        (void)remote_runtime->reconnect("tenant:runtime", source.run_id());
        FAIL() << "remote reconnect unexpectedly stole a live root execution lease";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_RUN_LEASE_HELD");
        // #421: the error names the lease, what happens when it expires, and
        // the child budgets that take no lease.
        const auto& message = error.diagnostic().message;
        EXPECT_NE(message.find("execution lease"), std::string::npos) << message;
        EXPECT_NE(message.find("TimedOut"), std::string::npos) << message;
        EXPECT_NE(message.find("max_child_depth"), std::string::npos) << message;
        EXPECT_NE(message.find("max_total_children"), std::string::npos) << message;
        const auto& witness = error.diagnostic().witness;
        EXPECT_EQ(witness.value("run_id", std::string()), source.run_id());
        EXPECT_EQ(witness.value("lease_id", std::string()), lease->id());
        EXPECT_EQ(witness.value("expires_at_ms", std::int64_t{0}), lease->expires_at_ms());
        const auto retry_after = witness.value("retry_after_ms", std::int64_t{0});
        EXPECT_GT(retry_after, 0);
        EXPECT_LE(retry_after, lease->expires_at_ms() - lease->acquired_at_ms());
    }
    EXPECT_EQ(blocking_calls.load(), 1U);

    EXPECT_TRUE(source.cancel());
    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_FALSE(backend->load_execution_lease(
        "tenant:runtime", source.run_id()));
}

TEST(ProgramRuntimeTest, ExpiredRootExecutionLeaseFencesWithoutRedispatch) {
    blocking_calls.store(0);
    blocking_active.store(0);
    TransitionStoreHandle backend = std::make_shared<InMemoryProgramTransitionStore>();
    auto local_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    local_view->block_execution_release();
    AdmittedRuntime fixture(2, {}, local_view);
    const auto version = fixture.admit("runtime-blocking");
    auto short_budget = grant();
    short_budget.wall_time_ms = 100;
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), short_budget,
                          "trace-expired-root-execution-lease", {}});
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);
    if (!local_view->wait_for_execution_release(std::chrono::seconds(2))) {
        local_view->release_execution_release();
        FAIL() << "root execution did not reach its blocked lease release";
    }

    auto first_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto second_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto first_runtime = fixture.make_runtime_with_transitions(first_view);
    auto second_runtime = fixture.make_runtime_with_transitions(second_view);
    std::barrier ready(3);
    const auto recover = [&](ProgramRuntime& runtime) {
        ready.arrive_and_wait();
        try {
            return runtime.reconnect("tenant:runtime", source.run_id()).wait().status();
        } catch (const ProgramDiagnosticError&) {
            return ProgramTerminalStatus::Failed;
        }
    };
    auto first = std::async(std::launch::async, [&] { return recover(*first_runtime); });
    auto second = std::async(std::launch::async, [&] { return recover(*second_runtime); });
    ready.arrive_and_wait();
    const auto first_status = first.get();
    const auto second_status = second.get();

    const auto durable = backend->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(durable);
    ASSERT_TRUE(durable->terminal_result());
    EXPECT_EQ(durable->terminal_result()->status(), ProgramTerminalStatus::TimedOut);
    EXPECT_TRUE(first_status == ProgramTerminalStatus::TimedOut ||
                second_status == ProgramTerminalStatus::TimedOut);
    EXPECT_FALSE(backend->load_execution_lease("tenant:runtime", source.run_id()));
    EXPECT_EQ(blocking_calls.load(), 1U);

    local_view->release_execution_release();
    (void)source.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RemoteReconnectCannotDispatchDurablyHeldGraphMigrationSource) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(AdversarialCheckpointStore::Mode::DelayMigrationTargetSave);
    TransitionStoreHandle backend = std::make_shared<InMemoryProgramTransitionStore>();
    auto local_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    AdmittedRuntime fixture(1, checkpoints, local_view);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-remote-fence", {}});
    auto migration = std::async(std::launch::async, [&] {
        return fixture.runtime
            ->migrate_graph(
                source,
                ProgramGraphMigrationTarget{
                    version.id(), "graph-migration-remote-fence-target", {}})
            .wait();
    });
    for (unsigned attempt = 0;
         !checkpoints->migration_target_save_started() && attempt < 3000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(checkpoints->migration_target_save_started());
    const auto held_lineage = backend->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(held_lineage);
    ASSERT_TRUE(backend->load_graph_migration_capsule(
        "tenant:runtime", source.run_id(), held_lineage->id()));

    auto remote_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto remote_runtime = fixture.make_runtime_with_transitions(remote_view);
    EXPECT_THROW(
        (void)remote_runtime->reconnect("tenant:runtime", source.run_id()),
        ProgramDiagnosticError);
    EXPECT_EQ(followup_calls.load(), 0U);

    const auto target = migration.get();
    EXPECT_EQ(target.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    (void)source.wait();
}

TEST(ProgramRuntimeTest, ConcurrentGraphMigrationRequestsPublishOneSuccessor) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-race", {}});
    auto second_runtime = fixture.make_runtime();
    std::barrier ready(3);
    const auto migrate = [&](ProgramRuntime& runtime, std::string run_id) {
        ready.arrive_and_wait();
        try {
            const auto result = runtime
                                    .migrate_graph(
                                        source,
                                        ProgramGraphMigrationTarget{
                                            version.id(), std::move(run_id), {}})
                                    .wait();
            return result.status() == ProgramTerminalStatus::Completed ? 1 : -1;
        } catch (const std::exception&) {
            return 0;
        }
    };
    auto first = std::async(std::launch::async, [&] {
        return migrate(*fixture.runtime, "graph-migration-race-first");
    });
    auto second = std::async(std::launch::async, [&] {
        return migrate(*second_runtime, "graph-migration-race-second");
    });
    ready.arrive_and_wait();

    EXPECT_EQ(first.get() + second.get(), 1);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    (void)source.wait();
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
TEST(ProgramRuntimeTest, GraphMigrationSurvivesSqliteRuntimeReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto id = sequence.fetch_add(1);
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-runtime-graph-migration-" +
          std::to_string(id) + ".db"))
            .string();
    const auto checkpoint_path =
        (std::filesystem::temp_directory_path() /
         ("neograph-runtime-graph-migration-checkpoints-" +
          std::to_string(id) + ".db"))
            .string();
    std::filesystem::remove(path);
    std::filesystem::remove(checkpoint_path);
    blocking_calls.store(0);
    followup_calls.store(0);

    {
        TransitionStoreHandle transitions = std::make_shared<SQLiteProgramTransitionStore>(path);
        auto checkpoints = std::make_shared<SqliteCheckpointStore>(checkpoint_path);
        AdmittedRuntime fixture(1, checkpoints, transitions);
        const auto version = fixture.admit("runtime-short-blocking");
        std::string source_run_id;
        std::string target_run_id;
        std::string target_result_id;
        {
            auto source = fixture.runtime->start(
                "tenant:runtime", version,
                ProgramInvocation{json::object(), grant(),
                                  "trace-sqlite-graph-migration", {}});
            const auto target = fixture.runtime
                                    ->migrate_graph(
                                        source,
                                        ProgramGraphMigrationTarget{
                                            version.id(), "sqlite-graph-migration-target", {}})
                                    .wait();
            ASSERT_EQ(target.status(), ProgramTerminalStatus::Completed);
            (void)source.wait();
            source_run_id = source.run_id();
            target_run_id = target.run_id();
            target_result_id = target.id();
        }
        EXPECT_EQ(blocking_calls.load(), 1U);
        EXPECT_EQ(followup_calls.load(), 1U);

        fixture.runtime.reset();
        fixture.journal.reset();
        fixture.checkpoints.reset();
        transitions.reset();
        checkpoints.reset();
        fixture.journal = std::make_shared<SQLiteProgramTransitionStore>(path);
        fixture.checkpoints = std::make_shared<SqliteCheckpointStore>(checkpoint_path);
        fixture.runtime = fixture.make_runtime();
        const auto lineage = fixture.journal->load_run_lineage(
            "tenant:runtime", source_run_id);
        ASSERT_TRUE(lineage);
        EXPECT_EQ(lineage->active_generation(), 2U);
        const auto generation = fixture.journal->load_generation(
            "tenant:runtime", lineage->lineage_id(), 2);
        ASSERT_TRUE(generation);
        ASSERT_TRUE(generation->graph_migration_receipt());
        const auto capsule = fixture.journal->load_graph_migration_capsule(
            "tenant:runtime", source_run_id,
            generation->graph_migration_receipt()->capsule().source_lineage_head_id());
        ASSERT_TRUE(capsule);
        EXPECT_EQ(capsule->id(),
                  generation->graph_migration_receipt()->capsule().id());
        const auto recovered = fixture.runtime->reconnect(
            "tenant:runtime", source_run_id).wait();
        EXPECT_EQ(recovered.id(), target_result_id);
        EXPECT_EQ(recovered.run_id(), target_run_id);
        EXPECT_EQ(followup_calls.load(), 1U);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(checkpoint_path);
}
#endif
#endif

TEST(ProgramRuntimeTest, JavaScriptCheckpointReplacementPublishesAndReconnectsGenerationTwo) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    const handoff = {cursor: 7, state: "ready"};
    yield ng.checkpoint(handoff, "replacement:boundary");
    yield ng.callCore("main", {}, "replacement:old-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {
        generation: "target",
        handoff: input.handoff,
        previous_run_id: input.previous_run_id
    };
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-replacement-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 7}, {"state", "ready"}}));
    const auto handoff_reference = handoff.reference();
    const auto source_boundary = source.snapshot();
    const auto source_lineage =
        journal->load_run_lineage_sync("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_lineage);
    EXPECT_EQ(source_lineage->active_generation(), 1U);

    const auto target_input =
        json{{"handoff", handoff.value()}, {"previous_run_id", source.run_id()}};
    auto replacement_runtime = fixture.make_runtime();
    // Wall time is counted in whole milliseconds from the source run's creation, so a replacement
    // that lands in the same millisecond leaves the budget unchanged. Let a few pass.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto target = replacement_runtime->replace(
        "tenant:runtime", std::move(handoff), target_version,
        ProgramInvocation{target_input, source_boundary.remaining_budget(),
                          "trace-replacement-target", {}});
    const auto result = target.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(result.output(),
              (json{{"generation", "target"},
                    {"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                    {"previous_run_id", source.run_id()}}));
    EXPECT_EQ(completed_calls.load(), 0U);

    const auto lineage = journal->load_run_lineage_sync("tenant:runtime", target.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->lineage_id(), source_lineage->lineage_id());
    EXPECT_EQ(lineage->active_generation(), 2U);
    const auto target_record = journal->load_sync("tenant:runtime", target.run_id());
    ASSERT_TRUE(target_record);
    EXPECT_EQ(target_record->invocation().budget,
              program_replacement_remaining_budget(source_boundary, *source_lineage,
                                                   target_record->created_at_ms()));
    EXPECT_LT(target_record->invocation().budget.wall_time_ms,
              source_boundary.remaining_budget().wall_time_ms);
    EXPECT_EQ(lineage->remaining_budget(), result.remaining_budget());
    const auto generation =
        journal->load_generation_sync("tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->replacement_receipt());
    EXPECT_EQ(generation->replacement_receipt()->source_run_id(), source.run_id());
    EXPECT_EQ(generation->replacement_receipt()->checkpoint_entry_id(),
              handoff_reference.command_entry_id);
    EXPECT_EQ(generation->replacement_receipt()->target_run_id(), target.run_id());

    const auto reconnected =
        fixture.runtime->reconnect("tenant:runtime", source.run_id()).wait();
    EXPECT_EQ(reconnected.id(), result.id());
    EXPECT_EQ(reconnected.run_id(), target.run_id());
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ReplacementCommitBeforeDispatchRecoversExactSuccessor) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 11}, "replacement:crash-boundary");
    yield ng.callCore("main", {}, "replacement:stale-source");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    const output = yield ng.callCore("main", input.handoff, "replacement:target");
    return {generation: "target", value: output.value, handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-replacement-crash-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    const auto source_boundary = source.snapshot();
    const auto target_input =
        json{{"handoff", handoff.value()}, {"previous_run_id", source.run_id()}};

    journal->crash_after_next_replacement();
    EXPECT_THROW(
        (void)fixture.runtime->replace(
            "tenant:runtime", std::move(handoff), target_version,
            ProgramInvocation{target_input, source_boundary.remaining_budget(),
                              "trace-replacement-crash-target", {}}),
        std::runtime_error);
    const auto committed_lineage =
        journal->load_run_lineage_sync("tenant:runtime", source.run_id());
    ASSERT_TRUE(committed_lineage);
    EXPECT_EQ(committed_lineage->active_generation(), 2U);
    EXPECT_EQ(completed_calls.load(), 0U);

    auto first_runtime  = fixture.make_runtime();
    auto second_runtime = fixture.make_runtime();
    std::barrier reconnect_ready(3);
    const auto reconnect = [&](ProgramRuntime& runtime) {
        reconnect_ready.arrive_and_wait();
        return runtime.reconnect("tenant:runtime", source.run_id());
    };
    auto first_reconnect =
        std::async(std::launch::async, [&] { return reconnect(*first_runtime); });
    auto second_reconnect =
        std::async(std::launch::async, [&] { return reconnect(*second_runtime); });
    reconnect_ready.arrive_and_wait();
    auto first_handle = first_reconnect.get();
    auto second_handle = second_reconnect.get();
    const auto recovered = first_handle.wait();
    const auto duplicate = second_handle.wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed)
        << (recovered.failure()
                ? recovered.failure()->code + ": " + recovered.failure()->message + " " +
                      recovered.failure()->witness.dump()
                : "no failure detail");
    EXPECT_EQ(recovered.output(),
              (json{{"generation", "target"},
                    {"value", "completed"},
                    {"handoff", json{{"cursor", 11}}}}));
    EXPECT_EQ(duplicate.id(), recovered.id());
    EXPECT_EQ(completed_calls.load(), 1U);
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, DifferentTransitionStoreCannotConsumeHandoffLease) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 23}, "handoff:wrong-store");
    yield ng.callCore("main", {}, "handoff:source-continues");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-wrong-store-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    {
        auto handoff = source.next_handoff();
        journal->release_result();
        const auto source_boundary = source.snapshot();
        const auto input =
            json{{"handoff", handoff.value()}, {"previous_run_id", source.run_id()}};
        AdmittedRuntime other_store(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
        EXPECT_THROW(
            (void)other_store.runtime->replace(
                "tenant:runtime", std::move(handoff), target_version,
                ProgramInvocation{input, source_boundary.remaining_budget(),
                                  "trace-handoff-wrong-store-target", {}}),
            std::invalid_argument);
        EXPECT_TRUE(handoff);
    }

    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, UnreadableCommittedReplacementNeverReleasesHeldSource) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 19}, "handoff:ambiguous-commit");
    yield ng.callCore("main", {}, "handoff:ambiguous-source-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    const output = yield ng.callCore("main", input.handoff, "handoff:ambiguous-target");
    return {generation: "target", value: output.value};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-ambiguous-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 19}}));
    const auto source_boundary = source.snapshot();

    journal->crash_after_next_replacement_with_unreadable_commit();
    EXPECT_THROW(
        (void)fixture.runtime->replace(
            "tenant:runtime", std::move(handoff), target_version,
            ProgramInvocation{
                json{{"handoff", json{{"cursor", 19}}},
                     {"previous_run_id", source.run_id()}},
                source_boundary.remaining_budget(), "trace-handoff-ambiguous-target", {}}),
        std::runtime_error);
    EXPECT_FALSE(handoff);
    const auto committed = journal->load_run_lineage_sync("tenant:runtime", source.run_id());
    ASSERT_TRUE(committed);
    EXPECT_EQ(committed->active_generation(), 2U);

    auto recovery_runtime = fixture.make_runtime();
    const auto recovered = recovery_runtime->reconnect("tenant:runtime", source.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(recovered.output(), (json{{"generation", "target"}, {"value", "completed"}}));
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ReplacementFailureBeforeCommitLeavesSourceExecutable) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 12}, "replacement:failed-boundary");
    yield ng.callCore("main", {}, "replacement:source-continues");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-replacement-failed-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));

    const std::string target_run_id = "replacement-occupied-target";
    auto              collider_runtime = fixture.make_runtime();
    std::optional<ProgramHandle> collider;
    {
        auto handoff = source.next_handoff();
        journal->release_result();
        EXPECT_EQ(handoff.value(), (json{{"cursor", 12}}));
        EXPECT_EQ(completed_calls.load(), 0U);
        journal->throw_before_next_replacement_with_occupied_target([&] {
            collider.emplace(collider_runtime->start(
                "tenant:runtime", target_version,
                ProgramInvocation{json{{"handoff", handoff.value()},
                                       {"previous_run_id", source.run_id()},
                                       {"collision", true}},
                                  javascript_budget(1, 4),
                                  "trace-replacement-collision", {}, target_run_id}));
        });
        EXPECT_THROW(
            (void)fixture.runtime->replace(
                "tenant:runtime", std::move(handoff), target_version,
                ProgramInvocation{
                    json{{"handoff", json{{"cursor", 12}}},
                         {"previous_run_id", source.run_id()}},
                    source.snapshot().remaining_budget(), "trace-replacement-failed-target", {},
                    target_run_id}),
            std::runtime_error);
        ASSERT_TRUE(collider);
        EXPECT_EQ(collider->wait().status(), ProgramTerminalStatus::Completed);
        const auto lineage = journal->load_run_lineage_sync("tenant:runtime", source.run_id());
        ASSERT_TRUE(lineage);
        EXPECT_EQ(lineage->active_generation(), 1U);
    }

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"generation", "source"}}));
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, AbandonedHandoffLeaseResumesSourceExactlyOnce) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 14}, "handoff:abandon");
    yield ng.callCore("main", {}, "handoff:continued");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-abandon", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    {
        auto handoff = source.next_handoff();
        EXPECT_THROW((void)source.next_handoff(), std::logic_error);
        auto reached = std::async(std::launch::async, [&] {
            return neograph::async::run_sync(handoff.wait_async());
        });
        journal->release_result();
        EXPECT_EQ(reached.get().value, (json{{"cursor", 14}}));
        EXPECT_FALSE(source.try_result());
        EXPECT_EQ(completed_calls.load(), 0U);
    }

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"status", "continued"}}));
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, DetachedAsyncHandoffWaitRemainsSafeAfterLeaseRelease) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 20}, "handoff:detached-async");
    yield ng.callCore("main", {}, "handoff:detached-continued");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-detached-async", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    std::optional<asio::awaitable<ExactProgramHandoff>> pending;
    {
        auto handoff = source.next_handoff();
        pending.emplace(handoff.wait_async());
    }
    journal->release_result();
    EXPECT_THROW((void)neograph::async::run_sync(std::move(*pending)), std::runtime_error);

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, HeldHandoffCannotBeBypassedBySecondRuntime) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 21}, "handoff:process-dedupe");
    yield ng.callCore("main", {}, "handoff:one-continuation");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-process-dedupe", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    {
        auto handoff = source.next_handoff();
        journal->release_result();
        EXPECT_EQ(handoff.value(), (json{{"cursor", 21}}));
        auto second_runtime = fixture.make_runtime();
        auto duplicate = second_runtime->reconnect("tenant:runtime", source.run_id());
        EXPECT_EQ(duplicate.run_id(), source.run_id());
        EXPECT_FALSE(duplicate.try_result());
        EXPECT_EQ(completed_calls.load(), 0U);
    }

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, HandoffRejectsAnAlreadyDuplicatedProcessControl) {
    blocking_calls.store(0);
    blocking_active.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-blocking", R"JS(
    yield ng.checkpoint({cursor: 22}, "handoff:duplicate-first");
    yield ng.callCore("main", {}, "handoff:blocking-continuation");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-duplicate-first", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto second_runtime = fixture.make_runtime();
    auto duplicate = second_runtime->reconnect("tenant:runtime", source.run_id());
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);
    EXPECT_THROW((void)source.next_handoff(), std::runtime_error);

    duplicate.cancel();
    source.cancel();
    journal->release_result();
    EXPECT_EQ(duplicate.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_NE(source.wait().status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, CancellationWakesHeldHandoffWithoutResumingJavaScript) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 15}, "handoff:cancel");
    yield ng.callCore("main", {}, "handoff:must-not-run");
    return {status: "unexpected"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-cancel", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 15}}));
    EXPECT_TRUE(source.cancel());

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, StoreReadFailureStillCancelsHeldHandoffLocally) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 16}, "handoff:store-failure");
    yield ng.callCore("main", {}, "handoff:must-not-run-after-store-failure");
    return {status: "unexpected"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-store-failure", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 16}}));
    journal->throw_on_next_load();
    EXPECT_TRUE(source.cancel());

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, RuntimeShutdownWakesHeldHandoff) {
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 17}, "handoff:shutdown");
    return {status: "unexpected"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 3),
                          "trace-handoff-shutdown", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 17}}));

    fixture.runtime.reset();
    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, CheckpointEventCanSynchronouslyCommitArmedHandoff) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    auto sink    = std::make_shared<CallbackSink>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.callCore("main", {}, "handoff:event-prime");
    yield ng.checkpoint({cursor: 18}, "handoff:event-boundary");
    yield ng.callCore("main", {}, "handoff:event-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 6),
                          "trace-handoff-event-source", sink});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto replacement_runtime = fixture.make_runtime();
    auto handoff = std::make_shared<std::optional<ProgramHandoff>>(source.next_handoff());
    std::promise<ProgramHandle> replacement_promise;
    auto replacement_future = replacement_promise.get_future();
    auto invoked = std::make_shared<std::atomic_bool>(false);
    sink->set_callback([&, handoff, invoked](const ProgramEvent& event) {
        if (event.kind != ProgramEventKind::CheckpointPublished ||
            event.operation_id != "root.javascript.2" || invoked->exchange(true)) {
            return;
        }
        try {
            auto target = replacement_runtime->replace(
                "tenant:runtime", std::move(**handoff), target_version,
                ProgramInvocation{
                    json{{"handoff", json{{"cursor", 18}}},
                         {"previous_run_id", source.run_id()}},
                    source.snapshot().remaining_budget(), "trace-handoff-event-target", {}});
            handoff->reset();
            replacement_promise.set_value(std::move(target));
        } catch (...) {
            replacement_promise.set_exception(std::current_exception());
        }
    });

    journal->release_result();
    ASSERT_EQ(replacement_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto result = replacement_future.get().wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(),
              (json{{"generation", "target"}, {"handoff", json{{"cursor", 18}}}}));
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

void configure_captured_control_catalog(AdmittedRuntime& fixture, bool unmanaged_core = false) {
    fixture.runtime.reset();
    fixture.registry = runtime_registry(
        unmanaged_core ? ExecutionGuarantee::Unmanaged : ExecutionGuarantee::Strict);
    fixture.profile = AdmittedRuntime::make_profile(
        fixture.registry, ExecutionGuarantee::Unmanaged, true);
    fixture.policy = AdmittedRuntime::make_policy(
        fixture.profile, ExecutionGuarantee::Unmanaged, true);
    fixture.engines = std::make_shared<EngineGenerationCache>();
    CatalogConfig config{fixture.store, fixture.registry, fixture.engines,
                         "program-runtime-test/v1"};
    config.recorded_capability_binder =
        [](const ProgramVersion& version, const std::vector<ProgramEvent>&) {
            if (!version.core_materialization_receipt().capability_bindings.empty())
                throw std::invalid_argument("Pure control fixture has no captured external capabilities");
            return RecordedCapabilityMaterialization{
                RecordedBindingSet({}, {}, {}), CatalogCapabilityBinding{}};
        };
    fixture.catalog = std::make_shared<ProgramCatalog>(std::move(config));
    fixture.runtime = fixture.make_runtime();
}

TEST(ProgramRuntimeTest, RecordedReplayCheckpointCannotAuthorizeLiveReplacement) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    journal->release_result();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    configure_captured_control_catalog(fixture);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 13}, "replacement:recorded-boundary");
    yield ng.callCore("main", {}, "replacement:recorded-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));
    auto original = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(2, 4),
                          "trace-authenticated-recorded-source", {}});
    ASSERT_EQ(original.wait().status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(completed_calls.load(), 1U);
    auto invocation = original.snapshot().invocation();
    invocation.run_id = "recorded-replacement-guard";
    invocation.correlation_id = "trace-recorded-replacement-source";
    std::optional<ProgramHandoff> held;
    auto sink = std::make_shared<CallbackSink>();
    sink->set_callback([&](const ProgramEvent& event) {
        if (event.kind == ProgramEventKind::Started)
            held.emplace(fixture.runtime->reconnect("tenant:runtime", invocation.run_id).next_handoff());
    });
    auto source = fixture.runtime->replay_recorded(
        original.run_id(), invocation, RecordedBindingSet({}, {}, {}), sink);
    ASSERT_TRUE(held);
    EXPECT_EQ(held->value(), (json{{"cursor", 13}}));
    EXPECT_FALSE(source.try_result());
    EXPECT_EQ(completed_calls.load(), 1U);
    try {
        (void)fixture.runtime->replace(
            "tenant:runtime", held->reference(), target_version,
            ProgramInvocation{
                json{{"handoff", held->value()}, {"previous_run_id", source.run_id()}},
                source.snapshot().remaining_budget(), "trace-recorded-replacement-target", {}});
        FAIL() << "recorded replay checkpoint authorized a live replacement";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_REPLACEMENT_RECORDED_SOURCE");
    }
    EXPECT_EQ(completed_calls.load(), 1U);
    source.cancel();
    held.reset();
    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RecordedControlChargesRealCoreWorkBeforeItsHeldCheckpoint) {
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    journal->release_result();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    configure_captured_control_catalog(fixture);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        "const observed = yield ng.callCore('main', {}, 'captured:core');"
        "yield ng.checkpoint({observed}, 'captured:checkpoint');return observed;"));
    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4), "real-core-source", {}});
    const auto terminal = original.wait();
    ASSERT_EQ(terminal.status(), ProgramTerminalStatus::Completed);
    const auto source_budget = journal->load_run_lineage_sync(
        "tenant:runtime", original.run_id())->remaining_budget();
    auto invocation = original.snapshot().invocation();
    invocation.run_id = "real-core-recorded-target";
    std::optional<ProgramHandoff> held;
    auto sink = std::make_shared<CallbackSink>();
    sink->set_callback([&](const ProgramEvent& event) {
        if (event.kind == ProgramEventKind::Started)
            held.emplace(fixture.runtime->reconnect("tenant:runtime", invocation.run_id).next_handoff());
    });
    auto replay = fixture.runtime->replay_recorded(
        original.run_id(), invocation, RecordedBindingSet({}, {}, {}), sink);
    ASSERT_TRUE(held);
    EXPECT_EQ(held->value(), (json{{"observed", terminal.output()}}));
    const auto at_checkpoint = replay.snapshot().remaining_budget();
    EXPECT_LT(at_checkpoint.max_core_steps, source_budget.max_core_steps);
    EXPECT_EQ(at_checkpoint.model_tokens, source_budget.model_tokens);
    EXPECT_EQ(at_checkpoint.max_program_operations, source_budget.max_program_operations);
    held.reset();
    const auto replay_result = replay.wait();
    ASSERT_EQ(replay_result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(replay_result.output(), terminal.output());
    EXPECT_GT(replay_result.usage().core_steps, 0U);
    ASSERT_LE(replay_result.usage().core_steps, source_budget.max_core_steps);
    EXPECT_EQ(replay_result.remaining_budget().max_core_steps,
              source_budget.max_core_steps - replay_result.usage().core_steps);
    EXPECT_EQ(replay_result.usage().program_operations, 0U);
    EXPECT_EQ(replay_result.remaining_budget().max_program_operations,
              source_budget.max_program_operations);
    EXPECT_EQ(replay_result.remaining_budget().model_tokens, source_budget.model_tokens);
}

TEST(ProgramRuntimeTest, RecordedControlCannotHideUnmanagedCoreBehindSandboxedJavaScript) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    journal->release_result();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    configure_captured_control_catalog(fixture, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", "return yield ng.callCore('main', {}, 'unmanaged:core');"));
    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4), "unmanaged-source", {}});
    ASSERT_EQ(original.wait().status(), ProgramTerminalStatus::Completed);
    const auto head = journal->load_run_lineage_sync("tenant:runtime", original.run_id());
    ASSERT_TRUE(head);
    auto invocation = original.snapshot().invocation();
    invocation.run_id = "unmanaged-hidden-target";
    try {
        (void)fixture.runtime->replay_recorded(
            original.run_id(), invocation, RecordedBindingSet({}, {}, {}));
        FAIL() << "Unmanaged Core received captured replay dispatch";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_REPLAY_GUARANTEE");
    }
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_FALSE(journal->load_sync("tenant:runtime", invocation.run_id));
    EXPECT_EQ(journal->load_run_lineage_sync("tenant:runtime", original.run_id())->id(), head->id());
}

TEST(ProgramRuntimeTest, RecordedControlCannotDispatchAnUncapturedCoreCommand) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    journal->release_result();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    configure_captured_control_catalog(fixture);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        "yield ng.checkpoint({cursor:1}, 'known:checkpoint');"
        "return yield ng.callCore('main', {}, 'missing:core');"));
    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2, 4), "uncaptured-source", {}});
    ASSERT_EQ(original.wait().status(), ProgramTerminalStatus::Completed);
    const auto source_id = original.run_id();
    journal->filter_command_read = [source_id](std::string_view run, auto commands) {
        if (run == source_id)
            std::erase_if(commands, [](const auto& entry) {
                return entry.command().kind() == JavaScriptCommandKind::CallCore;
            });
        return commands;
    };
    auto invocation = original.snapshot().invocation();
    invocation.run_id = "uncaptured-command-target";
    const auto replay = fixture.runtime->replay_recorded(
        source_id, invocation, RecordedBindingSet({}, {}, {})).wait();
    ASSERT_EQ(replay.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(replay.failure());
    EXPECT_EQ(replay.failure()->code, "P_REPLAY_EVIDENCE_REQUIRED");
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(journal->load_run_lineage_sync("tenant:runtime", source_id)->remaining_budget(), RunBudget{});
}

TEST(ProgramRuntimeTest, RecordedControlRejectsChangedCapturedCommandBeforeCoreWork) {
    for (const bool changed_effect : {false, true}) {
        completed_calls.store(0);
        auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
        journal->release_result();
        AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
        configure_captured_control_catalog(fixture);
        const auto version = fixture.admit_javascript(javascript_runtime_source(
            "runtime-completed",
            "yield ng.checkpoint({cursor:1}, 'known:checkpoint');"
            "return yield ng.callCore('main', {}, 'captured:core');"));
        auto original = fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), javascript_budget(2, 4), "changed-source", {}});
        ASSERT_EQ(original.wait().status(), ProgramTerminalStatus::Completed);
        const auto source_id = original.run_id();
        journal->filter_command_read = [source_id, changed_effect](std::string_view run, auto commands) {
            if (run != source_id) return commands;
            for (auto& entry : commands) {
                if (entry.command().kind() != JavaScriptCommandKind::CallCore) continue;
                auto command = entry.command().to_json();
                auto effect = entry.effect_identity();
                if (changed_effect)
                    effect->back() = effect->back() == '0' ? '1' : '0';
                else
                    command["arguments"]["input"] = json{{"tampered", true}};
                entry = ProgramJavaScriptCommandJournalEntry(
                    ProgramJavaScriptCommandJournalEntryData{
                        entry.sequence(), entry.bundle_id(), entry.command_ordinal(),
                        JavaScriptCommand::from_json(command), std::move(effect),
                        entry.terminal_result()});
            }
            return commands;
        };
        auto invocation = original.snapshot().invocation();
        invocation.run_id = "changed-command-target";
        const auto replay = fixture.runtime->replay_recorded(
            source_id, invocation, RecordedBindingSet({}, {}, {})).wait();
        ASSERT_EQ(replay.status(), ProgramTerminalStatus::Failed);
        EXPECT_EQ(replay.usage().core_steps, 0U);
        EXPECT_EQ(replay.usage().program_operations, 0U);
        EXPECT_EQ(completed_calls.load(), 1U);
        EXPECT_EQ(journal->load_run_lineage_sync("tenant:runtime", source_id)->remaining_budget(), RunBudget{});
    }
}

TEST(ProgramRuntimeTest, JavaScriptPendingHeadFreshRuntimeResumesWithoutRedispatch) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-blocking"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "crash:1");
                return {value: output.value};
            }
        )JS");

    // Keep the first Core dispatch in flight at the crash boundary.  A second
    // runtime is intentionally attached to the same durable transition store,
    // as a fresh process would be after the first one stopped after publishing
    // the yielded-command head.
    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"requested", "crash"}}, grant(), "trace-javascript-crash-boundary", {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blocking_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blocking_calls.load(), 1U);
    ASSERT_FALSE(original.try_result().has_value());

    const auto pending_entries =
        fixture.journal->load_javascript_commands("tenant:runtime", original.run_id());
    ASSERT_EQ(pending_entries.size(), 1U);
    ASSERT_TRUE(pending_entries.front().pending());

    auto       fresh_runtime = fixture.make_runtime();
    auto       recovered     = fresh_runtime->reconnect("tenant:runtime", original.run_id());
    const auto interrupted   = recovered.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    EXPECT_EQ(pending.idempotency(), ProgramEffectIdempotency::NonIdempotent);
    EXPECT_EQ(blocking_calls.load(), 1U);

    const auto resumed = fresh_runtime
                             ->resume("tenant:runtime", original.run_id(),
                                      resume_for(interrupted, json{{"value", "resumed"}},
                                                 "trace-javascript-crash-resume"))
                             .wait();
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message + " " +
                                    resumed.failure()->witness.dump()
                              : "no failure detail");
    EXPECT_EQ(resumed.output(), (json{{"value", "resumed"}}));
    EXPECT_EQ(blocking_calls.load(), 1U);

    const auto entries =
        fixture.journal->load_javascript_commands("tenant:runtime", original.run_id());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].coordinate_id(), entries[1].coordinate_id());
    EXPECT_TRUE(entries[1].completed());
    EXPECT_EQ(entries[1].terminal_result()->at("output"), (json{{"value", "resumed"}}));

    // Stop the original process simulation after the fresh runtime has
    // durably completed.  Its result loses the transition race and must not
    // cause a second Core dispatch.
    EXPECT_TRUE(original.cancel());
    (void)original.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptControlRejectsUnknownCommandFieldsBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                yield {
                    protocol_version: 1,
                    op: "call_core",
                    name: "main",
                    input: {requested: input.requested},
                    unexpected: true,
                };
                return {};
            }
        )JS");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "draft"}}, grant(), "trace-javascript-invalid", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JS_CONTROL_COMMAND");
    EXPECT_EQ(completed_calls.load(), 0U);
}

std::string javascript_runtime_source(std::string node_type, std::string body) {
    return "export function define() {\n"
           "  const graph = ng.graph(\"main\");\n"
           "  graph.channel(\"value\", {reducer: \"runtime-overwrite\", initial: \"\"});\n"
           "  graph.node(\"work\", {type: \"" +
           std::move(node_type) +
           "\"});\n"
           "  graph.entry(\"work\");\n"
           "  graph.exit(\"work\");\n"
           "  return graph;\n"
           "}\n\n"
           "export function* main(input) {\n" +
           std::move(body) + "\n}\n";
}

RunBudget javascript_budget(std::uint64_t max_concurrency,
                            std::uint64_t max_program_operations,
                            std::uint64_t max_child_depth,
                            std::uint64_t max_total_children) {
    return RunBudget{10000,
                     1000,
                     1000,
                     static_cast<std::uint32_t>(max_concurrency),
                     max_program_operations,
                     20,
                     0,
                     static_cast<std::uint32_t>(max_child_depth),
                     max_total_children};
}

TEST(ProgramRuntimeTest, JavaScriptBlockedCommandHonorsExternalCancellation) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-blocking",
                                                                                 R"JS(
    return yield ng.callCore("main", {}, "cancel:blocked");
)JS"));

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 1), "trace-js-cancel-blocked", {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blocking_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blocking_calls.load(), 1U);

    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_EQ(result.usage().program_operations, 1U);
}

TEST(ProgramRuntimeTest, JavaScriptCrashRecoveryDoesNotRecoverReservedResourceBudget) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-blocking",
                                                                                 R"JS(
    const first = yield ng.callCore("main", {}, "budget:first");
    yield ng.emit({unreachable: first.value}, "budget:second");
    return first;
)JS"));

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 1), "trace-js-budget-recovery", {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blocking_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto       fresh_runtime = fixture.make_runtime();
    const auto interrupted   = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");
    EXPECT_EQ(interrupted.remaining_budget().wall_time_ms, 0U);
    EXPECT_EQ(interrupted.remaining_budget().model_tokens, 0U);
    EXPECT_EQ(interrupted.remaining_budget().monetary_microunits, 0U);
    EXPECT_EQ(interrupted.remaining_budget().max_core_steps, 0U);
    EXPECT_EQ(interrupted.remaining_budget().max_program_operations, 0U);
    const auto durable = fixture.journal->latest("tenant:runtime", original.run_id());
    ASSERT_TRUE(durable.has_value());
    EXPECT_GT(durable->inflight_reservation.wall_time_ms, 0U);
    EXPECT_GT(durable->inflight_reservation.model_tokens, 0U);
    EXPECT_GT(durable->inflight_reservation.max_core_steps, 0U);
    EXPECT_TRUE(original.cancel());
    (void)original.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptAllJoinsActuallyOverlapCoreCommands) {
    blocking_calls.store(0);
    blocking_peak.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-short-blocking",
                                                           R"JS(
    const result = yield ng.all([
        ng.callCore("main", {}, "first"),
        ng.callCore("main", {}, "second")
    ], {max_in_flight: 2}, "all");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-overlap", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 2U);
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
}

TEST(ProgramRuntimeTest, JavaScriptAllKeepsFastInitialMembersAliveUntilJoinSetupFinishes) {
    completed_calls.store(0);
    AdmittedRuntime fixture(4, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    const results = yield ng.all([
        ng.callCore("main", {}, "fast:first"),
        ng.callCore("main", {}, "fast:second")
    ], {max_in_flight: 2}, "fast:all");
    return results;
)JS"));

    for (std::size_t iteration = 0; iteration < 256; ++iteration) {
        const auto result =
            fixture.runtime->run("tenant:runtime", version,
                                 ProgramInvocation{json::object(),
                                                   javascript_budget(2),
                                                   "trace-js-fast-all-" + std::to_string(iteration),
                                                   {}});
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
            << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                                 : "no failure detail");
    }
    EXPECT_EQ(completed_calls.load(), 512U);
}

TEST(ProgramRuntimeTest, JavaScriptAllKeepsFastReplacementMembersAliveUntilLaunchesRegister) {
    completed_calls.store(0);
    AdmittedRuntime fixture(4, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    const results = yield ng.all([
        ng.callCore("main", {}, "fast:first"),
        ng.callCore("main", {}, "fast:second"),
        ng.callCore("main", {}, "fast:third")
    ], {max_in_flight: 1}, "fast:all");
    return results;
)JS"));

    for (std::size_t iteration = 0; iteration < 256; ++iteration) {
        const auto result = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(),
                              javascript_budget(2),
                              "trace-js-fast-replacement-" + std::to_string(iteration),
                              {}});
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
            << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                                 : "no failure detail");
    }
    EXPECT_EQ(completed_calls.load(), 768U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinEnforcesMaxInFlightCap) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-short-blocking",
                                                           R"JS(
    const result = yield ng.all([
        ng.callCore("main", {}, "first"),
        ng.callCore("main", {}, "second"),
        ng.callCore("main", {}, "third")
    ], {max_in_flight: 2}, "all");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(3), "trace-js-cap", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 3U);
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinValidatesEveryMemberBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    yield ng.all([
        ng.callCore("main", {}, "valid:first"),
        ng.callCore("not-admitted", {}, "invalid:second")
    ], {max_in_flight: 2}, "validate");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-validate", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JS_CONTROL_COMMAND");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinReservesAggregateOperationBudgetBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    yield ng.all([
        ng.callCore("main", {}, "budget:first"),
        ng.callCore("main", {}, "budget:second")
    ], {max_in_flight: 2}, "budget");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2, 2), "trace-js-budget", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PROGRAM_OPERATION_BUDGET");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinUsesDeclarationOrderForResultsAndReadyRaceTies) {
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    const all_result = yield ng.all([
        ng.emit({id: 1}, "all:first"),
        ng.emit({id: 2}, "all:second")
    ], {max_in_flight: 2, collect: true}, "all");
    const race_result = yield ng.race([
        ng.emit({id: 1}, "race:first"),
        ng.emit({id: 2}, "race:second")
    ], "race");
    return {all: all_result, race: race_result};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-order", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(result.output(), (json{{"all", json::array({json{{"id", 1}}, json{{"id", 2}}})},
                                     {"race", json{{"id", 1}}}}));
}

TEST(ProgramRuntimeTest, JavaScriptStructuredCommandReplaysAfterFreshRuntimeWithoutRedispatch) {
    completed_calls.store(0);
    auto            journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    const results = yield ng.all([
        ng.callCore("main", {member: 1}, "replay:first"),
        ng.callCore("main", {member: 2}, "replay:second")
    ], {max_in_flight: 2}, "replay:join");
    return {values: results.map((result) => result.value)};
)JS"));

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{
            json::object(), javascript_budget(2), "trace-js-structured-result-crash", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    EXPECT_EQ(completed_calls.load(), 2U);

    auto       fresh_runtime = fixture.make_runtime();
    const auto replayed      = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(replayed.status(), ProgramTerminalStatus::Completed)
        << (replayed.failure() ? replayed.failure()->code + ": " + replayed.failure()->message
                               : "no failure detail");
    EXPECT_EQ(replayed.output(), (json{{"values", json::array({"completed", "completed"})}}));
    EXPECT_EQ(completed_calls.load(), 2U);

    journal->release_result();
    (void)original.wait();
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinCollectsFailuresInDeclarationOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-failing",
                                                                                 R"JS(
    const result = yield ng.all([
        ng.emit({id: "ok"}, "collect:ok"),
        ng.callCore("main", {}, "collect:failure")
    ], {max_in_flight: 2, collect: true}, "collect");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-collect", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.output(), json::array({json{{"id", "ok"}}, nullptr}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, RecoveryOfAResumedJavaScriptCallCoreResumesWithItsValue) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-resume-hook", R"JS(
    const reply = yield ng.callCore("main", {decision: input.decision}, "approval");
    return {value: reply.value};
)JS"));
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"decision", "run input"}}, javascript_budget(1),
                          "trace-js-resumed-crash", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);

    HeldResumedAttempt held;
    auto original = fixture.runtime->resume(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-js-resumed-crash-resume"));
    ASSERT_TRUE(held.entered());

    auto       fresh     = fixture.make_runtime();
    const auto recovered = fresh->reconnect("tenant:runtime", interrupted.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_detail(recovered);
    EXPECT_EQ(recovered.output(), (json{{"value", json{{"decision", "approved"}}.dump()}}));
    held.release();
    (void)original.wait();
}

TEST(ProgramRuntimeTest, JavaScriptFailFastJoinReportsTheFailureThatStoppedIt) {
    // #379: the member declared first is cancelled because its sibling
    // failed. The join reports the sibling's failure, not that cancellation.
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-value-gated", R"JS(
    yield ng.all([
        ng.callCore("main", {value: "wait"}, "fail-fast:waiting"),
        ng.callCore("main", {value: "fail"}, "fail-fast:failing")
    ], {max_in_flight: 2}, "fail-fast");
    return {unreachable: true};
)JS"));

    const auto started = std::chrono::steady_clock::now();
    const auto result  = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-fail-fast", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed") << result.failure()->message;
    EXPECT_NE(result.failure()->message.find("join member failure sentinel"), std::string::npos)
        << result.failure()->message;
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4))
        << "the waiting member was cancelled";
}

// ---------------------------------------------------------------------------
// A member's interrupt pauses its join (#380)
// ---------------------------------------------------------------------------

namespace {
struct EmitCountingSink final : public ProgramEventSink {
    void on_event(const ProgramEvent& event) override {
        if (event.kind == ProgramEventKind::Emit) emits.fetch_add(1);
    }
    std::atomic<unsigned> emits{0};
};

ProgramResult run_join(AdmittedRuntime& fixture, const std::string& body,
                       std::shared_ptr<ProgramEventSink> events = {}) {
    const auto version =
        fixture.admit_javascript(javascript_runtime_source("runtime-join-member", body));
    return fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(3), "trace-js-paused-join",
                          std::move(events)});
}

ProgramResult approve(AdmittedRuntime& fixture, const ProgramResult& interrupted,
                      std::shared_ptr<ProgramEventSink> events = {}) {
    return fixture.runtime
        ->resume("tenant:runtime", interrupted.run_id(),
                 resume_for(interrupted, json{{"decision", "approved"}},
                            "trace-js-paused-join-resume", std::move(events)))
        .wait();
}

std::vector<std::string> settled_members_of(AdmittedRuntime& fixture, const ProgramResult& run) {
    std::vector<std::string> members;
    const auto commands = fixture.journal->load_javascript_commands("tenant:runtime", run.run_id());
    if (commands.empty() || !commands.back().settled_members()) return members;
    for (const auto& [member, record] : commands.back().settled_members()->items())
        members.push_back(member);
    return members;
}
}  // namespace

TEST(ProgramRuntimeTest, JavaScriptAllPausedByAMemberResumesItAndReplaysTheSettledMember) {
    // #380: one member completes and the other interrupts. The join is
    // Interrupted on the paused member, the run resumes from that member's
    // checkpoint, and the command journal records the completed member, which
    // resume replays instead of running again. In memory and on SQLite alike.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        reset_join_member_runs();
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        const auto interrupted = run_join(fixture, R"JS(
    const replies = yield ng.all([
        ng.callCore("main", {value: "done"}, "pause:done"),
        ng.callCore("main", {value: "ask"}, "pause:ask")
    ], {max_in_flight: 2}, "pause:all");
    return {values: replies.map((reply) => reply.value)};
)JS");

        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
            << failure_detail(interrupted);
        ASSERT_TRUE(interrupted.interrupt() && interrupted.interrupt()->pending_input);
        EXPECT_EQ(interrupted.interrupt()->pending_input->operation_id(),
                  "root.javascript.1/member/1");
        ASSERT_TRUE(interrupted.checkpoint().has_value());
        const auto paused_at =
            fixture.checkpoints->load_by_id(interrupted.checkpoint()->checkpoint_id);
        ASSERT_TRUE(paused_at.has_value());
        EXPECT_EQ(paused_at->interrupt_phase, CheckpointPhase::NodeInterrupt)
            << "the run resumes the paused member, whichever member wrote last";
        EXPECT_EQ(interrupted.execution_trace(), std::vector<std::string>{"work"});
        EXPECT_EQ(settled_members_of(fixture, interrupted),
                  std::vector<std::string>{"root.javascript.1/member/0"});

        const auto resumed = approve(fixture, interrupted);

        ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
        EXPECT_EQ(resumed.output(), (json{{"values", json::array({"done:done", "approved"})}}));
        EXPECT_EQ(join_member_runs_of("done"), 1U) << "the completed member is replayed";
        EXPECT_EQ(join_member_runs_of("ask"), 2U);
        EXPECT_EQ(resumed.execution_trace(), std::vector<std::string>{"work"});
        const auto commands =
            fixture.journal->load_javascript_commands("tenant:runtime", resumed.run_id());
        ASSERT_EQ(commands.size(), 3U);
        EXPECT_EQ(commands.front().coordinate_id(), commands.back().coordinate_id());
        EXPECT_TRUE(commands.back().completed());
    }
}

TEST(ProgramRuntimeTest, JavaScriptPausedJoinKeepsASettledMemberOfAnyKind) {
    // #380: an emit member that settled before the pause is replayed from the
    // journal too: the resumed join does not emit it again.
    reset_join_member_runs();
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    auto            first_events = std::make_shared<EmitCountingSink>();
    const auto      interrupted  = run_join(fixture, R"JS(
    const replies = yield ng.all([
        ng.emit({id: "kept"}, "pause:emit"),
        ng.callCore("main", {value: "ask"}, "pause:ask")
    ], {max_in_flight: 2}, "pause:all");
    return {emitted: replies[0], decision: replies[1].value};
)JS",
                                     first_events);
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);
    EXPECT_EQ(first_events->emits.load(), 1U);

    auto       resumed_events = std::make_shared<EmitCountingSink>();
    const auto resumed        = approve(fixture, interrupted, resumed_events);

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
    EXPECT_EQ(resumed.output(),
              (json{{"emitted", json{{"id", "kept"}}}, {"decision", "approved"}}));
    EXPECT_EQ(resumed_events->emits.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptPausedJoinLaunchesNoFurtherMember) {
    // #380: the pause stops the join from launching members that have not
    // started; they run once the paused member resumes.
    reset_join_member_runs();
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      interrupted = run_join(fixture, R"JS(
    const replies = yield ng.all([
        ng.callCore("main", {value: "ask"}, "pause:ask"),
        ng.callCore("main", {value: "done"}, "pause:later")
    ], {max_in_flight: 1}, "pause:all");
    return {values: replies.map((reply) => reply.value)};
)JS");
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);
    EXPECT_EQ(join_member_runs_of("done"), 0U);
    EXPECT_TRUE(settled_members_of(fixture, interrupted).empty());

    const auto resumed = approve(fixture, interrupted);

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
    EXPECT_EQ(resumed.output(), (json{{"values", json::array({"approved", "done:done"})}}));
    EXPECT_EQ(join_member_runs_of("done"), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptPausedNestedJoinReplaysItsSettledMembers) {
    // #380: a nested join that pauses records its own settled members with
    // the outer join's, and resume replays both levels.
    reset_join_member_runs();
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      interrupted = run_join(fixture, R"JS(
    const replies = yield ng.all([
        ng.all([
            ng.callCore("main", {value: "inner"}, "nested:inner"),
            ng.callCore("main", {value: "ask"}, "nested:ask")
        ], {max_in_flight: 2}, "nested:inner-join"),
        ng.callCore("main", {value: "outer"}, "nested:outer")
    ], {max_in_flight: 2}, "nested:outer-join");
    return {inner: replies[0].map((reply) => reply.value), outer: replies[1].value};
)JS");
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);
    EXPECT_EQ(settled_members_of(fixture, interrupted),
              (std::vector<std::string>{"root.javascript.1/member/0/member/0",
                                        "root.javascript.1/member/1"}));

    const auto resumed = approve(fixture, interrupted);

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
    EXPECT_EQ(resumed.output(), (json{{"inner", json::array({"done:inner", "approved"})},
                                      {"outer", "done:outer"}}));
    EXPECT_EQ(join_member_runs_of("inner"), 1U);
    EXPECT_EQ(join_member_runs_of("outer"), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptFailFastAllFailsOnAFailureWhileAMemberIsPaused) {
    // #380: a pause does not hold back a fail-fast failure.
    reset_join_member_runs();
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      result = run_join(fixture, R"JS(
    yield ng.all([
        ng.callCore("main", {value: "ask"}, "pause:ask"),
        ng.callCore("main", {value: "fail"}, "pause:fail")
    ], {max_in_flight: 2}, "pause:all");
    return {unreachable: true};
)JS");

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed) << failure_detail(result);
    EXPECT_NE(result.failure()->message.find("join member failure sentinel"), std::string::npos)
        << failure_detail(result);
}

TEST(ProgramRuntimeTest, JavaScriptCollectingAllKeepsASettledFailureAcrossThePause) {
    // #380: a collecting join keeps the failure a member reached before the
    // pause; resume reports it without running that member again.
    reset_join_member_runs();
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      interrupted = run_join(fixture, R"JS(
    yield ng.all([
        ng.callCore("main", {value: "ask"}, "collect:ask"),
        ng.callCore("main", {value: "fail"}, "collect:fail"),
        ng.callCore("main", {value: "done"}, "collect:done")
    ], {max_in_flight: 3, collect: true}, "collect:all");
    return {unreachable: true};
)JS");
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);
    EXPECT_EQ(settled_members_of(fixture, interrupted),
              (std::vector<std::string>{"root.javascript.1/member/1",
                                        "root.javascript.1/member/2"}));

    const auto resumed = approve(fixture, interrupted);

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Failed) << failure_detail(resumed);
    EXPECT_NE(resumed.failure()->message.find("join member failure sentinel"), std::string::npos)
        << failure_detail(resumed);
    ASSERT_TRUE(resumed.output().is_array());
    ASSERT_EQ(resumed.output().size(), 3U);
    EXPECT_EQ(resumed.output()[0]["value"], "approved");
    EXPECT_TRUE(resumed.output()[1].is_null());
    EXPECT_EQ(resumed.output()[2]["value"], "done:done");
    EXPECT_EQ(join_member_runs_of("fail"), 1U);
    EXPECT_EQ(join_member_runs_of("done"), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptRaceIsWonByAMemberThatFinishesWhileAnotherIsPaused) {
    // #380: a paused member has not finished, so it cannot win a race; the
    // member that finishes does, and the question is never asked again.
    reset_join_member_runs();
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      result = run_join(fixture, R"JS(
    const winner = yield ng.race([
        ng.callCore("main", {value: "ask"}, "race:ask"),
        ng.callCore("main", {value: "slow"}, "race:slow")
    ], {max_in_flight: 2}, "race:pause");
    return {winner: winner.value};
)JS");

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
    EXPECT_EQ(result.output(), (json{{"winner", "done:slow"}}));
    // The paused member may be cancelled before its node starts (#367,
    // #368); it never asks twice, and no member is running once the race ends.
    EXPECT_LE(join_member_runs_of("ask"), 1U);
    EXPECT_EQ(join_member_runs_of("slow"), 1U);
    expect_join_members_stopped();
}

// #367, #368: a C++ node body never holds the Program scheduler. With one
// scheduler thread, a member whose node blocks its thread still lets the other
// member run and win; the blocked member's late completion is discarded.
TEST(ProgramRuntimeTest, JavaScriptRaceIsWonByTheFastMemberWhileACppNodeBlocksItsThread) {
    reset_join_member_runs();
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      result = run_join(fixture, R"JS(
    const winner = yield ng.race([
        ng.callCore("main", {value: "block"}, "race:block"),
        ng.callCore("main", {value: "quick"}, "race:quick")
    ], {max_in_flight: 2}, "race:blocking");
    return {winner: winner.value};
)JS");

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
    EXPECT_EQ(result.output(), (json{{"winner", "done:quick"}}));
    // The loser's node may be cancelled before it starts (#367, #368). If it
    // started, the run waited for it: no member is running once the race ends.
    EXPECT_LE(join_member_runs_of("block"), 1U);
    EXPECT_EQ(join_member_runs_of("quick"), 1U);
    expect_join_members_stopped();
    EXPECT_EQ(result.output().dump().find("done:block"), std::string::npos);
}

TEST(ProgramRuntimeTest, JavaScriptAllOfBlockingCppNodesRunsTheirBodiesSideBySide) {
    reset_join_member_runs();
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      result  = run_join(fixture, R"JS(
    const values = yield ng.all([
        ng.callCore("main", {value: "block"}, "all:first"),
        ng.callCore("main", {value: "block"}, "all:second")
    ], {max_in_flight: 2}, "all:blocking");
    return {values: values.map((member) => member.value)};
)JS");

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
    EXPECT_EQ(result.output(), (json{{"values", json::array({"done:block", "done:block"})}}));
    EXPECT_EQ(join_member_runs_of("block"), 2U);
    // Two 800 ms bodies on one scheduler thread: side by side, not in turn. The
    // peak number of bodies running at once says so without a wall-clock bound,
    // which a loaded or sanitized machine can exceed.
    EXPECT_EQ(join_member_peak.load(), 2U);
}

TEST(ProgramRuntimeTest, JavaScriptRaceIsWonByTheFastMemberWhileASubgraphNodeBlocks) {
    // The same through a Core subgraph: the blocking node runs one level down.
    reset_join_member_runs();
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-subgraph-member", R"JS(
    const winner = yield ng.race([
        ng.callCore("main", {value: "block"}, "race:block"),
        ng.callCore("main", {value: "quick"}, "race:quick")
    ], {max_in_flight: 2}, "race:subgraph");
    return {winner: winner.value};
)JS"));
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(3), "trace-js-subgraph-race", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
    EXPECT_EQ(result.output(), (json{{"winner", "done:quick"}}));
    // As above: the loser may never start, and none is running at the end.
    EXPECT_LE(join_member_runs_of("block"), 1U);
    EXPECT_EQ(join_member_runs_of("quick"), 1U);
    expect_join_members_stopped();
}

TEST(ProgramRuntimeTest, JavaScriptRaceOfPausedMembersResumesTheFirstDeclared) {
    // #380: when every member pauses, the race is Interrupted on the first
    // one declared; once it resumes and finishes it wins the race.
    reset_join_member_runs();
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      interrupted = run_join(fixture, R"JS(
    const winner = yield ng.race([
        ng.callCore("main", {value: "ask"}, "race:first"),
        ng.callCore("main", {value: "ask"}, "race:second")
    ], {max_in_flight: 2}, "race:pause");
    return {winner: winner.value};
)JS");
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);
    ASSERT_TRUE(interrupted.interrupt() && interrupted.interrupt()->pending_input);
    EXPECT_EQ(interrupted.interrupt()->pending_input->operation_id(),
              "root.javascript.1/member/0");
    EXPECT_TRUE(settled_members_of(fixture, interrupted).empty());

    const auto resumed = approve(fixture, interrupted);

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
    EXPECT_EQ(resumed.output(), (json{{"winner", "approved"}}));
}

TEST(ProgramRuntimeTest, JavaScriptQuorumMetWithoutItsPausedMemberCompletes) {
    // #380: the members that finish meet the quorum; the paused member is
    // not needed and the question is never asked again.
    reset_join_member_runs();
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      result = run_join(fixture, R"JS(
    const replies = yield ng.quorum([
        ng.callCore("main", {value: "ask"}, "quorum:ask"),
        ng.callCore("main", {value: "done"}, "quorum:done"),
        ng.callCore("main", {value: "slow"}, "quorum:slow")
    ], {required_successes: 2, max_in_flight: 3}, "quorum:pause");
    return {values: replies.map((reply) => reply.value)};
)JS");

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
    EXPECT_EQ(result.output(), (json{{"values", json::array({"done:done", "done:slow"})}}));
    // The paused member may be cancelled before its node starts (#367, #368).
    EXPECT_LE(join_member_runs_of("ask"), 1U);
    expect_join_members_stopped();
}

TEST(ProgramRuntimeTest, JavaScriptQuorumThatNeedsItsPausedMemberWaitsForIt) {
    // #380: the quorum cannot be met without the paused member, which may
    // still succeed, so it is neither met nor out of reach: the join pauses,
    // and resume replays the member that succeeded.
    reset_join_member_runs();
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      interrupted = run_join(fixture, R"JS(
    const replies = yield ng.quorum([
        ng.callCore("main", {value: "ask"}, "quorum:ask"),
        ng.callCore("main", {value: "done"}, "quorum:done")
    ], {required_successes: 2, max_in_flight: 2}, "quorum:pause");
    return {values: replies.map((reply) => reply.value)};
)JS");
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << failure_detail(interrupted);
    EXPECT_EQ(settled_members_of(fixture, interrupted),
              std::vector<std::string>{"root.javascript.1/member/1"});

    const auto resumed = approve(fixture, interrupted);

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
    EXPECT_EQ(resumed.output(), (json{{"values", json::array({"approved", "done:done"})}}));
    EXPECT_EQ(join_member_runs_of("done"), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptQuorumOutOfReachFailsWhileAMemberIsPaused) {
    // #380: two failures put the quorum out of reach even if the paused
    // member succeeds, so the join fails instead of pausing.
    reset_join_member_runs();
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      result = run_join(fixture, R"JS(
    yield ng.quorum([
        ng.callCore("main", {value: "ask"}, "quorum:ask"),
        ng.callCore("main", {value: "fail"}, "quorum:fail-1"),
        ng.callCore("main", {value: "fail"}, "quorum:fail-2")
    ], {required_successes: 2, max_in_flight: 3}, "quorum:pause");
    return {unreachable: true};
)JS");

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed) << failure_detail(result);
    EXPECT_EQ(result.failure()->code, "P_QUORUM_UNREACHABLE");
}

// ---------------------------------------------------------------------------
// An await keeps its deadline across a resume (#391)
// ---------------------------------------------------------------------------

namespace {
// The start of each await of the run's latest command, as its command journal
// recorded it.
json recorded_await_starts(AdmittedRuntime& fixture, const ProgramResult& run) {
    const auto commands = fixture.journal->load_javascript_commands("tenant:runtime", run.run_id());
    for (auto entry = commands.rbegin(); entry != commands.rend(); ++entry)
        if (entry->await_started_at()) return *entry->await_started_at();
    return json::object();
}
}  // namespace

TEST(ProgramRuntimeTest, JavaScriptAwaitThatPausedPastItsDeadlineTimesOutOnResume) {
    // #391: the awaited call asks, and is answered after the await's 800 ms
    // have passed. The deadline was recorded when the await started, so the
    // resumed await times out at once instead of starting a new 800 ms; the
    // answered node does not run again.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        reset_join_member_runs();
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        const auto paused = run_join(fixture, R"JS(
    const reply = yield ng.await(ng.callCore("main", {value: "ask"}, "await:ask"), 800, "await");
    return {value: reply.value};
)JS");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const auto resumed = approve(fixture, paused);
        ASSERT_EQ(resumed.status(), ProgramTerminalStatus::TimedOut) << failure_detail(resumed);
        EXPECT_EQ(resumed.failure()->code, "P_AWAIT_TIMEOUT");
        EXPECT_EQ(join_member_runs_of("ask"), 1U) << "a timed-out await dispatches nothing";
        EXPECT_TRUE(recorded_await_starts(fixture, paused).contains("root.javascript.1"))
            << "the await's start is in the command journal";
    }
}

TEST(ProgramRuntimeTest, JavaScriptAwaitResumedBeforeItsDeadlineHasOnlyTheRemainingTime) {
    // #391: 600 ms pass before the answer, and the answered node then takes
    // 1.2 s. Of the await's 1.5 s only 0.9 s remained, so it times out; a timer
    // that restarted on resume would have let the node finish.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        reset_join_member_runs();
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        const auto paused = run_join(fixture, R"JS(
    const reply = yield ng.await(ng.callCore("main", {value: "ask-slow"}, "await:slow"), 1500, "await");
    return {value: reply.value};
)JS");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);

        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const auto started = std::chrono::steady_clock::now();
        const auto resumed = approve(fixture, paused);
        const auto waited  = std::chrono::steady_clock::now() - started;
        ASSERT_EQ(resumed.status(), ProgramTerminalStatus::TimedOut) << failure_detail(resumed);
        EXPECT_EQ(resumed.failure()->code, "P_AWAIT_TIMEOUT");
        EXPECT_LT(waited, std::chrono::milliseconds(1150)) << "only the remaining time was waited";
    }
}

TEST(ProgramRuntimeTest, JavaScriptAwaitResumedWithinItsDeadlineCompletes) {
    // #391: a resume well inside the deadline completes normally, with the
    // start recorded once, and a later await of the run gets its own start.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        reset_join_member_runs();
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        const auto paused = run_join(fixture, R"JS(
    const first = yield ng.await(ng.callCore("main", {value: "ask"}, "await:first"), 10000, "first");
    const second = yield ng.await(ng.callCore("main", {value: "ask"}, "await:second"), 10000, "second");
    return {values: [first.value, second.value]};
)JS");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_TRUE(recorded_await_starts(fixture, paused).contains("root.javascript.1"));

        const auto again = approve(fixture, paused);
        ASSERT_EQ(again.status(), ProgramTerminalStatus::Interrupted) << failure_detail(again);
        const auto second_start = recorded_await_starts(fixture, again);
        EXPECT_TRUE(second_start.contains("root.javascript.2")) << second_start.dump();
        EXPECT_FALSE(second_start.contains("root.javascript.1"))
            << "each command records the awaits it started";

        const auto done = approve(fixture, again);
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(done.output(), (json{{"values", json::array({"approved", "approved"})}}));
    }
}

TEST(ProgramRuntimeTest, JavaScriptJoinMemberAwaitKeepsItsDeadlineAcrossAResume) {
    // #391: an await inside a join member records its own start too.
    reset_join_member_runs();
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      paused = run_join(fixture, R"JS(
    const replies = yield ng.all([
        ng.await(ng.callCore("main", {value: "ask"}, "member:ask"), 800, "member:await"),
        ng.callCore("main", {value: "done"}, "member:done")
    ], {max_in_flight: 2}, "join");
    return {values: replies.map((reply) => reply.value)};
)JS");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const auto resumed = approve(fixture, paused);
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::TimedOut) << failure_detail(resumed);
    EXPECT_EQ(resumed.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_EQ(join_member_runs_of("done"), 1U) << "the settled member replays";
    EXPECT_TRUE(recorded_await_starts(fixture, paused).contains("root.javascript.1/member/0"));
}

TEST(ProgramRuntimeTest, ProgramPathKeepsLintAndFanOutNoticesOffStderr) {
    // Program admission reports lint through diagnostics, and Program hosts
    // cannot change EngineConfig::worker_count, so neither belongs on stderr.
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    testing::internal::CaptureStderr();
    const auto version = fixture.admit_javascript(
        "export function define() {\n"
        "  const graph = ng.graph(\"main\");\n"
        "  graph.channel(\"value\", {reducer: \"runtime-overwrite\", initial: \"\"});\n"
        "  for (const name of [\"split\", \"left\", \"right\", \"join\"])\n"
        "    graph.node(name, {type: \"runtime-completed\"});\n"
        "  graph.entry(\"split\").edge(\"split\", \"left\").edge(\"split\", \"right\");\n"
        "  graph.edge(\"left\", \"join\").edge(\"right\", \"join\").exit(\"join\");\n"
        "  return graph;\n"
        "}\n");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(), "trace-quiet-stderr", {}});
    const auto output = testing::internal::GetCapturedStderr();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(output.find("[neograph] lint"), std::string::npos) << output;
    EXPECT_EQ(output.find("executing serially"), std::string::npos) << output;
}

TEST(ProgramRuntimeTest, JavaScriptCoreFailureKeepsTheTraceOfCompletedNodes) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        "export function define() {\n"
        "  const graph = ng.graph(\"main\");\n"
        "  graph.channel(\"value\", {reducer: \"runtime-overwrite\", initial: \"\"});\n"
        "  graph.node(\"first\", {type: \"runtime-completed\"});\n"
        "  graph.node(\"second\", {type: \"runtime-completed\"});\n"
        "  graph.node(\"work\", {type: \"runtime-failing\"});\n"
        "  graph.entry(\"first\");\n"
        "  graph.edge(\"first\", \"second\");\n"
        "  graph.edge(\"second\", \"work\");\n"
        "  graph.exit(\"work\");\n"
        "  return graph;\n"
        "}\n");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(), "trace-failed-trace", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->core_node, "work");
    EXPECT_EQ(result.execution_trace(), std::vector<std::string>({"first", "second"}));
    EXPECT_EQ(ProgramResult::parse(result.serialize_canonical()).execution_trace(),
              result.execution_trace());
}

TEST(ProgramRuntimeTest, JavaScriptAwaitTimeoutCancelsDirectCoreWork) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-blocking",
                                                                                 R"JS(
    const result = yield ng.await(ng.callCore("main", {}, "await:core"), 10, "await");
    return result;
)JS"));

    const auto started = std::chrono::steady_clock::now();
    const auto result  = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-await-timeout", {}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    // The node body runs on a worker (#367, #368). It is cancelled while it
    // blocks, or, when the 10 ms timeout fires before a worker starts it, it
    // never starts. Either way no Core work outlives the await, and the run
    // does not wait out the body's 5 s.
    EXPECT_LE(blocking_calls.load(), 1U);
    EXPECT_EQ(blocking_active.load(), 0U);
    EXPECT_LT(elapsed.count(), 2);
}

TEST(ProgramRuntimeTest, JavaScriptQuorumOrdersSuccessfulMembers) {
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-short-blocking",
                                                           R"JS(
    const result = yield ng.quorum([
        ng.callCore("main", {}, "quorum:blocked"),
        ng.emit({id: 1}, "quorum:first"),
        ng.emit({id: 2}, "quorum:second")
    ], {required_successes: 2, max_in_flight: 2}, "quorum");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-quorum", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"id", 1}}, json{{"id", 2}}}));
}

TEST(ProgramRuntimeTest, JavaScriptCancelScopeCancelsTheOwningRun) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    yield ng.cancelScope("current", "stop", "scope stopped");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-cancel-scope", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
}

TEST(ProgramRuntimeTest, JavaScriptRaceCancelsLoserChildrenWithoutOrphans) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      parent_version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                           R"JS(
    const winner = yield ng.race([
        ng.await(ng.spawn("child", {}, "spawn"), 5000, "await"),
        ng.emit({winner: true}, "winner")
    ], {max_in_flight: 2}, "race");
    return winner;
)JS"));
    const auto child_version = fixture.admit("runtime-blocking");
    const auto linked        = link_child_versions(fixture, parent_version, child_version,
                                                   BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1});

    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{
            json::object(), javascript_budget(2, 32, 1, 1), "trace-js-cancel-child", {}});
    const auto result = parent.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(result.output(), (json{{"winner", true}}));
    // The losing member may be cancelled before it spawns its child (#367,
    // #368); a child it did spawn is cancelled, not left running.
    const auto children = parent.snapshot().children();
    EXPECT_LE(children.size(), 1U);
    for (const auto& child : children) EXPECT_EQ(child.state, ProgramChildState::Cancelled);
}

TEST(ProgramRuntimeTest, JavaScriptInterruptedCallCoreResumesItsExactPendingCoordinate) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:call-core");
    return {value: output.value};
)JS"));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-exact-interrupt", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_input.has_value());

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "approved"}},
                                                 "trace-js-exact-resume"))
                             .wait();

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message
                              : "no failure detail");
    EXPECT_EQ(resumed.output(), (json{{"value", "approved"}}));
    EXPECT_EQ(interrupt_calls.load(), 2U);
    const auto commands =
        fixture.journal->load_javascript_commands("tenant:runtime", interrupted.run_id());
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands.front().coordinate_id(), commands.back().coordinate_id());
    EXPECT_TRUE(commands.back().completed());
}

namespace {

// Holds the attempt of a resumed run on the first event of the Core call it resumes until the test lets it go: the
// call has started and none of its nodes has run. The held event can also be rejected, as a host callback that throws
// rejects it.
class HoldFirstCoreEventSink final : public ProgramEventSink {
public:
    explicit HoldFirstCoreEventSink(bool reject_it) : reject_it_(reject_it) {}

    void on_event(const ProgramEvent& event) override {
        if (event.kind != ProgramEventKind::Core || held_.exchange(true)) return;
        nodes_run_when_held = interrupt_calls.load();
        entered_.set_value();
        released_.wait();
        if (reject_it_) throw std::runtime_error("the handler rejected the first Core event");
    }
    bool entered() { return entered_future_.wait_for(std::chrono::seconds(10)) == std::future_status::ready; }
    void release() {
        if (!released_flag_.exchange(true)) release_.set_value();
    }

    unsigned nodes_run_when_held = 0;

private:
    bool                     reject_it_;
    std::atomic<bool>        held_{false};
    std::promise<void>       entered_;
    std::future<void>        entered_future_ = entered_.get_future();
    std::promise<void>       release_;
    std::shared_future<void> released_ = release_.get_future().share();
    std::atomic<bool>        released_flag_{false};
};

// An in-memory checkpoint store that holds the first load by id made after it is armed, until the test lets it go. The
// attempt of a resumed run loads its checkpoint before it does anything else, so a test that arms the store when the
// resume starts can end the run before the attempt has begun.
class HoldingCheckpointStore final : public InMemoryCheckpointStore {
public:
    void arm() {
        std::lock_guard lock(mutex_);
        armed_ = true;
    }
    bool held() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(10), [this] { return held_; });
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }
    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        {
            std::unique_lock lock(mutex_);
            if (armed_ && !held_) {
                held_ = true;
                cv_.notify_all();
                cv_.wait(lock, [this] { return released_; });
            }
        }
        return InMemoryCheckpointStore::load_by_id(id);
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    armed_ = false, held_ = false, released_ = false;
};

// Arms a HoldingCheckpointStore on the first event of a resume, which is delivered once the resume is published and
// before its attempt exists.
class ArmOnStartedSink final : public ProgramEventSink {
public:
    explicit ArmOnStartedSink(HoldingCheckpointStore& store) : store_(store) {}
    void on_event(const ProgramEvent& event) override {
        if (event.kind == ProgramEventKind::Started) store_.arm();
    }

private:
    HoldingCheckpointStore& store_;
};

}  // namespace

// A resume of a run with a JavaScript main() withholds the capacity of the interrupted callCore as a reservation in the
// journal, and the call settles it with the usage it measured. A run that ends before the call has started returned
// nothing of it, and the journal cannot show that no node ran: it ends in the state it was asked to (#557) with the
// remaining budget the resume left it, and gives none of the reservation back.
TEST(ProgramRuntimeTest, AResumedJavaScriptRunCancelledBeforeItsCoreCallStartsEndsCancelled) {
    // In memory and on SQLite alike: the stores share the check of the transition.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite transition store" : "in-memory transition store");
        interrupt_calls.store(0);
        JoinTestStores  stores(durable);
        auto            gate = std::make_shared<HoldingCheckpointStore>();
        AdmittedRuntime fixture(1, gate, stores.transitions, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      version =
            fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:cancelled-first");
    return {value: output.value};
)JS"));
        const auto interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), javascript_budget(1), "trace-js-cancel-first", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted) << failure_detail(interrupted);

        // The attempt of the resume waits at its first load of the checkpoint, and the cancellation arrives before it has
        // done anything.
        auto handle = fixture.runtime->resume(
            "tenant:runtime", interrupted.run_id(),
            resume_for(interrupted, json{{"decision", "approved"}}, "trace-js-cancel-first-resume",
                       std::make_shared<ArmOnStartedSink>(*gate)));
        ASSERT_TRUE(gate->held()) << "the attempt never loaded its checkpoint";
        const auto held = fixture.journal->latest("tenant:runtime", interrupted.run_id());
        ASSERT_TRUE(held.has_value());
        ASSERT_EQ(held->continuation.state, ContinuationState::Running);
        ASSERT_EQ(held->continuation.attempt, 2U);
        ASSERT_NE(held->inflight_reservation, RunBudget{}) << "the resume holds the reservation of the call";

        EXPECT_TRUE(handle.cancel());
        gate->release();
        const auto ended = handle.wait();

        EXPECT_EQ(ended.status(), ProgramTerminalStatus::Cancelled) << failure_detail(ended);
        EXPECT_EQ(interrupt_calls.load(), 1U) << "no node ran after the resume";
        const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::Cancelled);
        EXPECT_EQ(latest->sequence, held->sequence + 1) << "the cancellation is the one publication after the resume";
        EXPECT_EQ(latest->remaining_budget, held->remaining_budget) << "nothing of the reservation was returned";
        EXPECT_EQ(latest->inflight_reservation, RunBudget{});
        EXPECT_EQ(ended.remaining_budget(), held->remaining_budget);
        const auto record = fixture.journal->load("tenant:runtime", interrupted.run_id());
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->continuation().state, ContinuationState::Cancelled);
        ASSERT_TRUE(record->terminal_result().has_value());
        EXPECT_EQ(record->terminal_result()->status(), ProgramTerminalStatus::Cancelled);

        // A runtime that starts over the same stores finds a run that ended, and does not run it again.
        auto       fresh = fixture.make_runtime();
        const auto found = fresh->reconnect("tenant:runtime", interrupted.run_id()).wait();
        EXPECT_EQ(found.status(), ProgramTerminalStatus::Cancelled) << failure_detail(found);
        EXPECT_EQ(interrupt_calls.load(), 1U);
    }
}

// Once the Core call has started, the call settles the reservation with the usage it measured, and the part it did not
// use is returned, whether the run is cancelled or its handler throws. These two stay as they were.
TEST(ProgramRuntimeTest, AResumedJavaScriptRunCancelledAtTheStartOfItsCoreCallSettlesTheReservation) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:cancelled-at-start");
    return {value: output.value};
)JS"));
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-cancel-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted) << failure_detail(interrupted);

    auto sink   = std::make_shared<HoldFirstCoreEventSink>(false);
    auto handle = fixture.runtime->resume(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-js-cancel-start-resume", sink));
    ASSERT_TRUE(sink->entered());
    ASSERT_EQ(sink->nodes_run_when_held, 1U) << "the node had not run again when the attempt was held";
    const auto held = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(held.has_value());
    ASSERT_NE(held->inflight_reservation, RunBudget{});

    EXPECT_TRUE(handle.cancel());
    sink->release();
    const auto ended = handle.wait();

    EXPECT_EQ(ended.status(), ProgramTerminalStatus::Cancelled) << failure_detail(ended);
    EXPECT_EQ(interrupt_calls.load(), 1U) << "no node ran after the resume";
    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Cancelled);
    EXPECT_EQ(latest->sequence, held->sequence + 2) << "the call settled its reservation, then the run ended";
    EXPECT_GT(latest->remaining_budget.model_tokens, held->remaining_budget.model_tokens)
        << "the unused part of the reservation came back";
    EXPECT_EQ(latest->inflight_reservation, RunBudget{});
}

TEST(ProgramRuntimeTest, AResumedJavaScriptRunWhoseHandlerThrowsAtTheStartOfItsCoreCallEndsFailedWithTheHandlersCode) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:handler-throws");
    return {value: output.value};
)JS"));
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-throw-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted) << failure_detail(interrupted);

    auto sink   = std::make_shared<HoldFirstCoreEventSink>(true);
    auto handle = fixture.runtime->resume(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-js-throw-start-resume", sink));
    ASSERT_TRUE(sink->entered());
    ASSERT_EQ(sink->nodes_run_when_held, 1U) << "the node had not run again when the attempt was held";
    const auto held = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(held.has_value());
    ASSERT_NE(held->inflight_reservation, RunBudget{});
    sink->release();
    const auto ended = handle.wait();

    EXPECT_EQ(ended.status(), ProgramTerminalStatus::Failed) << failure_detail(ended);
    ASSERT_TRUE(ended.failure().has_value());
    EXPECT_EQ(ended.failure()->code, "P_EVENT_SINK");
    EXPECT_EQ(interrupt_calls.load(), 1U) << "no node ran after the resume";
    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
    EXPECT_EQ(latest->sequence, held->sequence + 2);
    EXPECT_GT(latest->remaining_budget.model_tokens, held->remaining_budget.model_tokens);
    auto       fresh = fixture.make_runtime();
    const auto found = fresh->reconnect("tenant:runtime", interrupted.run_id()).wait();
    EXPECT_EQ(found.status(), ProgramTerminalStatus::Failed) << failure_detail(found);
}

namespace {

// Rejects a resume on its first event, Started, which is delivered once the resume is published and before its attempt
// exists. `before_throwing` runs first, as a host's callback does.
class RejectResumeSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent& event) override {
        if (event.kind != ProgramEventKind::Started) return;
        if (before_throwing) before_throwing();
        throw std::runtime_error("the handler rejected the resume");
    }
    std::function<void()> before_throwing;
};

}  // namespace

// The resumed call has not run when its resume is rejected, so the run ends Failed with the code of the handler's error,
// the remaining budget the resume left it, and none of the reservation of the call returned.
TEST(ProgramRuntimeTest, AResumedJavaScriptRunWhoseHandlerThrowsAtItsStartedEventEndsFailedWithTheHandlersCode) {
    // In memory and on SQLite alike.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        interrupt_calls.store(0);
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      version =
            fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:handler-throws-at-started");
    return {value: output.value};
)JS"));
        const auto interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), javascript_budget(1), "trace-js-throw-started", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted) << failure_detail(interrupted);

        std::optional<ProgramJournalRecord> held;
        auto                                sink = std::make_shared<RejectResumeSink>();
        sink->before_throwing = [&] { held = fixture.journal->latest("tenant:runtime", interrupted.run_id()); };
        const auto ended = fixture.runtime
                               ->resume("tenant:runtime", interrupted.run_id(),
                                        resume_for(interrupted, json{{"decision", "approved"}},
                                                   "trace-js-throw-started-resume", sink))
                               .wait();

        ASSERT_TRUE(held.has_value());
        ASSERT_EQ(held->continuation.state, ContinuationState::Running);
        ASSERT_NE(held->inflight_reservation, RunBudget{}) << "the resume holds the reservation of the call";
        EXPECT_EQ(ended.status(), ProgramTerminalStatus::Failed) << failure_detail(ended);
        ASSERT_TRUE(ended.failure().has_value());
        EXPECT_EQ(ended.failure()->code, "P_EVENT_SINK");
        EXPECT_EQ(interrupt_calls.load(), 1U) << "no node ran after the resume";
        const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
        EXPECT_EQ(latest->sequence, held->sequence + 1);
        EXPECT_EQ(latest->remaining_budget, held->remaining_budget);
        EXPECT_EQ(latest->inflight_reservation, RunBudget{});
        EXPECT_EQ(ended.remaining_budget(), held->remaining_budget);
        auto       fresh = fixture.make_runtime();
        const auto found = fresh->reconnect("tenant:runtime", interrupted.run_id()).wait();
        EXPECT_EQ(found.status(), ProgramTerminalStatus::Failed) << failure_detail(found);
        EXPECT_EQ(interrupt_calls.load(), 1U);
    }
}

namespace {

// What a resumed JavaScript run leaves behind when it ends before the Core call it resumes has started: the terminal
// state, as the one publication after the resume, the remaining budget the resume left it, none of the reservation of
// the call returned, and a record that a runtime starting over the stores finds ended.
void expect_a_resumed_javascript_run_ended_without_returning_its_reservation(
    AdmittedRuntime& fixture, const std::string& run_id, const ProgramJournalRecord& held,
    const ProgramResult& ended, ProgramTerminalStatus status, ContinuationState state) {
    EXPECT_EQ(held.continuation.state, ContinuationState::Running);
    EXPECT_NE(held.inflight_reservation, RunBudget{}) << "the resume holds the reservation of the call";
    EXPECT_EQ(ended.status(), status) << failure_detail(ended);
    const auto latest = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, state);
    EXPECT_EQ(latest->sequence, held.sequence + 1) << "the ending is the one publication after the resume";
    EXPECT_EQ(latest->remaining_budget, held.remaining_budget) << "nothing of the reservation was returned";
    EXPECT_EQ(latest->inflight_reservation, RunBudget{});
    EXPECT_EQ(ended.remaining_budget(), held.remaining_budget);
    const auto record = fixture.journal->load("tenant:runtime", run_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->continuation().state, state);
    ASSERT_TRUE(record->terminal_result().has_value());
    EXPECT_EQ(record->terminal_result()->status(), status);
    auto       fresh = fixture.make_runtime();
    const auto found = fresh->reconnect("tenant:runtime", run_id).wait();
    EXPECT_EQ(found.status(), status) << failure_detail(found);
}

// Makes the checkpoints vanish on the first event of a resume, which is delivered once the resume is published and before
// its attempt exists. `before_vanishing` runs first, as a host's callback does.
class VanishOnStartedSink final : public ProgramEventSink {
public:
    explicit VanishOnStartedSink(neograph::test::VanishingCheckpointStore& store) : store_(store) {}
    void on_event(const ProgramEvent& event) override {
        if (event.kind != ProgramEventKind::Started) return;
        if (before_vanishing) before_vanishing();
        store_.vanish();
    }
    std::function<void()> before_vanishing;

private:
    neograph::test::VanishingCheckpointStore& store_;
};

}  // namespace

// The checkpoint a resume continues from can be gone, or changed, when the attempt of the resume reads it: the stores of
// a restarted process do not have to agree. The attempt then ends the run CheckpointIncompatible before the call it
// resumes has started, with the remaining budget the resume left it and none of the reservation of the call returned.
TEST(ProgramRuntimeTest, AResumedJavaScriptRunWhoseCheckpointIsGoneEndsCheckpointIncompatible) {
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite transition store" : "in-memory transition store");
        interrupt_calls.store(0);
        JoinTestStores  stores(durable);
        auto            checkpoints = std::make_shared<neograph::test::VanishingCheckpointStore>();
        AdmittedRuntime fixture(1, checkpoints, stores.transitions, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      version =
            fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:checkpoint-gone");
    return {value: output.value};
)JS"));
        const auto interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), javascript_budget(1), "trace-js-checkpoint-gone", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted) << failure_detail(interrupted);

        std::optional<ProgramJournalRecord> held;
        auto                                sink = std::make_shared<VanishOnStartedSink>(*checkpoints);
        sink->before_vanishing = [&] { held = fixture.journal->latest("tenant:runtime", interrupted.run_id()); };
        const auto ended = fixture.runtime
                               ->resume("tenant:runtime", interrupted.run_id(),
                                        resume_for(interrupted, json{{"decision", "approved"}},
                                                   "trace-js-checkpoint-gone-resume", sink))
                               .wait();

        ASSERT_TRUE(held.has_value());
        expect_a_resumed_javascript_run_ended_without_returning_its_reservation(
            fixture, interrupted.run_id(), *held, ended, ProgramTerminalStatus::CheckpointIncompatible,
            ContinuationState::CheckpointIncompatible);
        ASSERT_TRUE(ended.failure().has_value());
        EXPECT_EQ(ended.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
        EXPECT_EQ(interrupt_calls.load(), 1U) << "no node ran after the resume";
    }
}

namespace {

enum class EndAtHostAdmission { CancelledWhileWaiting, CapacityExceeded, ResolverThrows, CancelledAfterAdmission };

// Resumes a paused run of a JavaScript Program under a runtime with host admission, and ends the resumed attempt at
// the point `how` names. The first attempt of the run is admitted at once; only the attempt of the resume is held,
// refused, made to throw, or admitted and then cancelled when its Core call has just started.
void end_a_resumed_javascript_run_at_host_admission(EndAtHostAdmission how, bool durable) {
    interrupt_calls.store(0);
    JoinTestStores  stores(durable);
    AdmittedRuntime fixture(1, {}, stores.transitions, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:host-admission");
    return {value: output.value};
)JS"));

    const auto host_profile =
        neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
            "runtime-host-resumed-v1",
            neograph::HostResourceVector{.cpu_millis = 1},
            {},
            neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1, false}});
    auto host = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});

    std::optional<ProgramJournalRecord> held;
    std::promise<void>                  asked;
    std::promise<void>                  proceed;
    auto                                proceed_future = proceed.get_future().share();
    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 1};
    config.host_admission          = host;
    config.host_admission_resolver = [&](const ProgramHostAdmissionContext& context) {
        neograph::HostAdmissionRequest request;
        request.resources.cpu_millis = 1;
        if (context.attempt < 2) return request;
        held = fixture.journal->latest(std::string(context.owner_scope), std::string(context.run_id));
        switch (how) {
            case EndAtHostAdmission::CancelledWhileWaiting:
                asked.set_value();
                proceed_future.wait();
                break;
            case EndAtHostAdmission::CapacityExceeded:
                request.resources.cpu_millis = 1000;
                break;
            case EndAtHostAdmission::ResolverThrows:
                throw std::runtime_error("the resolver failed");
            case EndAtHostAdmission::CancelledAfterAdmission:
                break;
        }
        return request;
    };
    fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-host-admission", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted) << failure_detail(interrupted);

    auto sink = std::make_shared<HoldFirstCoreEventSink>(false);
    auto handle = fixture.runtime->resume(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-js-host-admission-resume",
                   how == EndAtHostAdmission::CancelledAfterAdmission ? sink : nullptr));
    if (how == EndAtHostAdmission::CancelledWhileWaiting) {
        ASSERT_EQ(asked.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready)
            << "the attempt never asked for host capacity";
        EXPECT_TRUE(handle.cancel());
        proceed.set_value();
    }
    if (how == EndAtHostAdmission::CancelledAfterAdmission) {
        ASSERT_TRUE(sink->entered());
        EXPECT_TRUE(handle.cancel());
        sink->release();
    }
    const auto ended = handle.wait();
    ASSERT_TRUE(held.has_value());

    if (how == EndAtHostAdmission::CancelledAfterAdmission) {
        // Once the admitted attempt has started the Core call, the call settles the reservation with the usage it
        // measured, and the part it did not use comes back: the end is not the one of an attempt that never started.
        const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(ended.status(), ProgramTerminalStatus::Cancelled) << failure_detail(ended);
        EXPECT_EQ(latest->continuation.state, ContinuationState::Cancelled);
        EXPECT_EQ(latest->sequence, held->sequence + 2) << "the call settled its reservation, then the run ended";
        EXPECT_EQ(latest->inflight_reservation, RunBudget{});
        EXPECT_GT(latest->remaining_budget.model_tokens, held->remaining_budget.model_tokens)
            << "the unused part of the reservation came back";
        return;
    }
    const bool cancelled = how == EndAtHostAdmission::CancelledWhileWaiting;
    expect_a_resumed_javascript_run_ended_without_returning_its_reservation(
        fixture, interrupted.run_id(), *held, ended,
        cancelled ? ProgramTerminalStatus::Cancelled : ProgramTerminalStatus::Failed,
        cancelled ? ContinuationState::Cancelled : ContinuationState::Failed);
    EXPECT_EQ(interrupt_calls.load(), 1U) << "no node ran after the resume";
    if (how == EndAtHostAdmission::CapacityExceeded) {
        ASSERT_TRUE(ended.failure().has_value());
        EXPECT_EQ(ended.failure()->code, "P_HOST_ADMISSION");
    }
    if (how == EndAtHostAdmission::ResolverThrows) {
        ASSERT_TRUE(ended.failure().has_value());
        EXPECT_EQ(ended.failure()->code, "P_RUNTIME_CORE_FAILURE");
    }
}

}  // namespace

// The attempt of a resume that waits for host capacity has started nothing, and the reservation of the call it resumes
// is held in the journal: a cancellation while it waits ends the run Cancelled, with the remaining budget the resume
// left it and none of the reservation returned.
TEST(ProgramRuntimeTest, AResumedJavaScriptRunCancelledWhileItWaitsForHostCapacityEndsCancelled) {
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite transition store" : "in-memory transition store");
        end_a_resumed_javascript_run_at_host_admission(EndAtHostAdmission::CancelledWhileWaiting, durable);
    }
}

TEST(ProgramRuntimeTest, AResumedJavaScriptRunRefusedByHostAdmissionEndsFailedWithItsCode) {
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite transition store" : "in-memory transition store");
        end_a_resumed_javascript_run_at_host_admission(EndAtHostAdmission::CapacityExceeded, durable);
    }
}

// The resolver of the host is host code: when it throws for the attempt of a resume, the attempt cannot be launched, and
// the run ends Failed the same way.
TEST(ProgramRuntimeTest, AResumedJavaScriptRunWhoseHostAdmissionResolverThrowsEndsFailed) {
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite transition store" : "in-memory transition store");
        end_a_resumed_javascript_run_at_host_admission(EndAtHostAdmission::ResolverThrows, durable);
    }
}

// Admitted by the host and cancelled once its Core call has started, the run settles the reservation as it did before.
TEST(ProgramRuntimeTest, AResumedJavaScriptRunCancelledAfterHostAdmissionSettlesTheReservation) {
    end_a_resumed_javascript_run_at_host_admission(EndAtHostAdmission::CancelledAfterAdmission, false);
}

namespace {

// Holds the only scheduler thread of a runtime on its first Core event until the test lets it go, so that the attempt of
// a run started or recovered afterwards waits in the queue and a cancellation can arrive before it has begun.
class HoldSchedulerSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent& event) override {
        if (event.kind != ProgramEventKind::Core || held_.exchange(true)) return;
        entered_.set_value();
        released_.wait();
    }
    bool entered() { return entered_future_.wait_for(std::chrono::seconds(10)) == std::future_status::ready; }
    void release() {
        if (!released_flag_.exchange(true)) release_.set_value();
    }

private:
    std::atomic<bool>        held_{false};
    std::promise<void>       entered_;
    std::future<void>        entered_future_ = entered_.get_future();
    std::promise<void>       release_;
    std::shared_future<void> released_ = release_.get_future().share();
    std::atomic<bool>        released_flag_{false};
};

}  // namespace

// A run recovered after its process stopped inside a command holds the reservation of the command in the journal, and is
// no resume: nothing carries it a value to continue a Core call with. One that is cancelled before the recovered attempt
// has reached the command ends Cancelled with the remaining budget the journal holds, and the reservation is not
// returned: the command may have been sent, and nothing has measured what it used.
TEST(ProgramRuntimeTest, ARecoveredJavaScriptCommandCancelledBeforeItIsReachedEndsCancelled) {
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite transition store" : "in-memory transition store");
        interrupt_calls.store(0);
        blocking_calls.store(0);
        blocking_active.store(0);
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, {}, stores.transitions, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-blocking", R"JS(
    yield ng.callCore("main", {}, "recover:cancelled-first");
    return {status: "unexpected"};
)JS"));
        const auto blocker_version = fixture.admit("runtime-completed");

        auto original = fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), javascript_budget(1, 4), "trace-js-recover-cancel", {}});
        for (unsigned i = 0; blocking_active.load() == 0 && i < 5000; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ASSERT_EQ(blocking_active.load(), 1U) << "the process stopped inside the command";
        const auto held = fixture.journal->latest("tenant:runtime", original.run_id());
        ASSERT_TRUE(held.has_value());

        // A runtime of the restarted process, whose only scheduler thread another run holds.
        auto fresh    = fixture.make_runtime();
        auto blocker  = std::make_shared<HoldSchedulerSink>();
        auto blocking = fresh->start("tenant:runtime", blocker_version,
                                     ProgramInvocation{json::object(), grant(), "trace-js-recover-holder", blocker});
        ASSERT_TRUE(blocker->entered());
        auto recovered = fresh->reconnect("tenant:runtime", original.run_id());
        EXPECT_TRUE(recovered.cancel());
        blocker->release();
        const auto ended = recovered.wait();
        (void)blocking.wait();

        expect_a_resumed_javascript_run_ended_without_returning_its_reservation(
            fixture, original.run_id(), *held, ended, ProgramTerminalStatus::Cancelled, ContinuationState::Cancelled);
        EXPECT_EQ(blocking_calls.load(), 1U) << "the recovered attempt never reached the command";
        original.cancel();
        (void)original.wait();
    }
}

// A reconciliation that completes a paused JavaScript callCore publishes the continuation of the call and delivers its first
// event, Started, before its attempt exists. When the handler throws on that event, no node of the call has run: the run
// ends Failed with the code of the handler's error and the remaining budget the reconciliation left it, as the resume of a
// paused callCore does. The reconciliation used to answer with an ambiguous effect that was not ambiguous, which no result
// may carry (P_RUNTIME_INTERNAL), and the run stayed running.
TEST(ProgramRuntimeTest, AReconciledJavaScriptCallCoreWhoseHandlerThrowsAtItsStartedEventEndsFailedWithTheHandlersCode) {
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        interrupt_calls.store(0);
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      version =
            fixture.admit_javascript(javascript_runtime_source("runtime-effect-nonidempotent", R"JS(
    const output = yield ng.callCore("main", {}, "reconcile:handler-throws-at-started");
    return {value: output.value};
)JS"));
        const auto interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), javascript_budget(1), "trace-js-reconcile-throw", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted) << failure_detail(interrupted);
        ASSERT_TRUE(interrupted.interrupt() && interrupted.interrupt()->pending_effect);
        const auto pending = *interrupted.interrupt()->pending_effect;
        ASSERT_EQ(fixture.runtime
                      ->reconcile("tenant:runtime", interrupted.run_id(),
                                  ProgramEffectResolution{pending.call_id(), ProgramEffectReconciliation::Unknown,
                                                          std::nullopt, "trace-js-reconcile-unknown", {}})
                      .wait()
                      .status(),
                  ProgramTerminalStatus::AmbiguousEffect);

        std::optional<ProgramJournalRecord> held;
        auto                                sink = std::make_shared<RejectResumeSink>();
        sink->before_throwing = [&] { held = fixture.journal->latest("tenant:runtime", interrupted.run_id()); };
        const auto ended = fixture.runtime
                               ->reconcile("tenant:runtime", interrupted.run_id(),
                                           ProgramEffectResolution{pending.call_id(),
                                                                   ProgramEffectReconciliation::Completed,
                                                                   json{{"result", "reconciled"}},
                                                                   "trace-js-reconcile-throw-completed", sink})
                               .wait();

        ASSERT_TRUE(held.has_value());
        ASSERT_EQ(held->continuation.state, ContinuationState::Running);
        EXPECT_EQ(ended.status(), ProgramTerminalStatus::Failed) << failure_detail(ended);
        ASSERT_TRUE(ended.failure().has_value());
        EXPECT_EQ(ended.failure()->code, "P_EVENT_SINK");
        EXPECT_EQ(interrupt_calls.load(), 1U) << "no node ran after the reconciliation";
        const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
        EXPECT_EQ(latest->sequence, held->sequence + 1);
        EXPECT_EQ(latest->remaining_budget, held->remaining_budget);
        EXPECT_EQ(latest->inflight_reservation, RunBudget{});
        EXPECT_EQ(ended.remaining_budget(), held->remaining_budget);
        auto       fresh = fixture.make_runtime();
        const auto found = fresh->reconnect("tenant:runtime", interrupted.run_id()).wait();
        EXPECT_EQ(found.status(), ProgramTerminalStatus::Failed) << failure_detail(found);
        EXPECT_EQ(interrupt_calls.load(), 1U);
    }
}

TEST(ProgramRuntimeTest, JavaScriptExactResumeStillValidatesMalformedTerminalOutput) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    yield ng.callCore("main", {}, "resume:malformed-output");
    return "not-an-object";
)JS"),
                                 ContractRecord{1, json{{"type", "object"}}});

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-malformed-first", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "approved"}},
                                                 "trace-js-malformed-resume"))
                             .wait();

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_OUTPUT_CONTRACT");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JavaScriptRaceSelectionDoesNotReplenishLateMembers) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    const winner = yield ng.race([
        ng.emit({winner: 0}, "race:first"),
        ng.emit({winner: 1}, "race:second"),
        ng.callCore("main", {}, "race:must-not-launch")
    ], {max_in_flight: 2}, "race:no-replenish");
    return winner;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-race-fence", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"winner", 0}}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptMissingSpawnBindingPreflightBlocksEverySibling) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    yield ng.all([
        ng.callCore("main", {}, "spawn-preflight:valid"),
        ng.spawn("missing-child", {}, "spawn-preflight:missing")
    ], {max_in_flight: 2}, "spawn-preflight:join");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{
            json::object(), javascript_budget(2, 32, 1, 1), "trace-js-spawn-preflight", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptAwaitedCallCoreResumesItsNestedExactCoordinate) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.await(
        ng.callCore("main", {}, "resume:nested-call-core"),
        undefined,
        "resume:nested-await");
    return {value: output.value};
)JS"));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-nested-interrupt", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_input.has_value());

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "nested-approved"}},
                                                 "trace-js-nested-resume"))
                             .wait();

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message
                              : "no failure detail");
    EXPECT_EQ(resumed.output(), (json{{"value", "nested-approved"}}));
    EXPECT_EQ(interrupt_calls.load(), 2U);
    const auto events = fixture.journal->load_events("tenant:runtime", interrupted.run_id(), 0);
    const auto checkpoint_events = std::count_if(
        events.begin(), events.end(),
        [](const auto& event) { return event.kind == ProgramEventKind::CheckpointPublished; });
    EXPECT_EQ(checkpoint_events, 2);
}

TEST(ProgramRuntimeTest, JavaScriptEventSinkFailureSettlesPublishedCommandReservation) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    yield ng.emit({published: true}, "sink:emit");
    return {unreachable: true};
)JS"));
    auto sink = std::make_shared<ThrowingSink>(ProgramEventKind::Emit);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-sink", sink});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_EVENT_SINK");
    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Failed);
    EXPECT_EQ(journal->inflight_reservation, RunBudget{});
    const auto commands =
        fixture.journal->load_javascript_commands("tenant:runtime", result.run_id());
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_TRUE(commands.back().completed());
    EXPECT_LE(result.remaining_budget().wall_time_ms + result.usage().wall_time_ms,
              javascript_budget(1).wall_time_ms);
}
#endif

TEST(ProgramRuntimeTest, ProgramInterruptAndExactResumePreserveDirectCoreBehavior) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");

    const auto program_interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-program-interrupt", {}});
    const auto program_resumed =
        fixture.runtime
            ->resume("tenant:runtime", program_interrupted.run_id(),
                     resume_for(program_interrupted, json{{"decision", "approved"}},
                                "trace-program-resume"))
            .wait();

    const auto definition = program_document("runtime-interrupt")["root"]["definition"];
    auto topology = neograph::program::detail::RegistrySnapshotAccess::parse_local(fixture.registry,
                                                                                   definition);
    auto linked   = neograph::program::detail::RegistrySnapshotAccess::link_local(
        fixture.registry, std::move(topology), NodeContext{});
    EngineResources engine_resources;
    engine_resources.registry =
        neograph::program::detail::RegistrySnapshotAccess::runtime_registry(fixture.registry);
    auto direct_engine =
        GraphEngine::link(std::move(linked), EngineConfig{}, std::move(engine_resources));
    auto direct_checkpoints = std::make_shared<InMemoryCheckpointStore>();
    direct_engine->set_checkpoint_store(direct_checkpoints);

    RunConfig direct_config;
    direct_config.thread_id       = "direct-interrupt-equivalence";
    direct_config.input           = json::object();
    direct_config.max_steps       = 20;
    const auto direct_interrupted = direct_engine->run(direct_config);
    const auto direct_resumed     = direct_engine->resume_from(
        direct_config, direct_interrupted.checkpoint_id, json{{"decision", "approved"}});

    ASSERT_EQ(program_interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_EQ(direct_interrupted.status(), RunStatus::Interrupted);
    ASSERT_TRUE(program_interrupted.interrupt().has_value());
    EXPECT_EQ(program_interrupted.interrupt()->core_node, direct_interrupted.interrupt_node);
    EXPECT_EQ(program_interrupted.interrupt()->value, direct_interrupted.interrupt_value);
    EXPECT_EQ(program_interrupted.execution_trace(), direct_interrupted.execution_trace);
    EXPECT_EQ(program_resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(direct_resumed.status(), RunStatus::Completed);
    EXPECT_EQ(program_resumed.output(), direct_resumed.output);
    EXPECT_EQ(program_resumed.execution_trace(), direct_resumed.execution_trace);

    ASSERT_TRUE(program_resumed.checkpoint().has_value());
    const auto program_checkpoint =
        fixture.checkpoints->load_by_id(program_resumed.checkpoint()->checkpoint_id);
    const auto direct_checkpoint = direct_checkpoints->load_by_id(direct_resumed.checkpoint_id);
    ASSERT_TRUE(program_checkpoint.has_value());
    ASSERT_TRUE(direct_checkpoint.has_value());
    EXPECT_EQ(program_checkpoint->channel_values, direct_checkpoint->channel_values);
    EXPECT_EQ(program_checkpoint->channel_versions, direct_checkpoint->channel_versions);
    EXPECT_EQ(program_checkpoint->current_node, direct_checkpoint->current_node);
    EXPECT_EQ(program_checkpoint->next_nodes, direct_checkpoint->next_nodes);
    EXPECT_EQ(program_checkpoint->interrupt_phase, direct_checkpoint->interrupt_phase);
    EXPECT_EQ(program_checkpoint->barrier_state, direct_checkpoint->barrier_state);
    EXPECT_EQ(program_checkpoint->step, direct_checkpoint->step);
    EXPECT_EQ(interrupt_calls.load(), 4U);
}

TEST(ProgramRuntimeTest, ExactResumeAtEndChargesNoAdditionalCoreStep) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    auto            document                          = program_document("runtime-completed");
    document["root"]["definition"]["interrupt_after"] = json::array({"work"});
    const auto version                                = fixture.admit_document(std::move(document));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-after", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    ASSERT_EQ(completed_calls.load(), 1U);

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json::object(), "trace-after-resume"))
                             .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.usage().core_steps, 0U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ExactResumeToInterruptBeforeChargesOnlyExecutedSteps) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    auto            document       = program_document("runtime-completed");
    auto            core           = document["root"]["definition"];
    core["nodes"]                  = json{{"a", json{{"type", "runtime-completed"}}},
                                          {"b", json{{"type", "runtime-completed"}}},
                                          {"c", json{{"type", "runtime-completed"}}}};
    core["edges"]                  = json::array({
        json{{"from", "__start__"}, {"to", "a"}},
        json{{"from", "a"}, {"to", "b"}},
        json{{"from", "b"}, {"to", "c"}},
        json{{"from", "c"}, {"to", "__end__"}},
    });
    core["interrupt_after"]        = json::array({"a"});
    core["interrupt_before"]       = json::array({"c"});
    document["root"]["definition"] = std::move(core);
    const auto version             = fixture.admit_document(std::move(document));
    auto       budget              = grant();
    budget.max_core_steps          = 3;

    const auto first =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), budget, "trace-sequence-1", {}});
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(first.usage().core_steps, 1U);
    EXPECT_EQ(first.remaining_budget().max_core_steps, 2U);

    const auto second = fixture.runtime
                            ->resume("tenant:runtime", first.run_id(),
                                     resume_for(first, json::object(), "trace-sequence-2"))
                            .wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(second.usage().core_steps, 1U);
    EXPECT_EQ(second.remaining_budget().max_core_steps, 1U);

    const auto third = fixture.runtime
                           ->resume("tenant:runtime", second.run_id(),
                                    resume_for(second, json::object(), "trace-sequence-3"))
                           .wait();
    EXPECT_EQ(third.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(third.usage().core_steps, 1U);
    EXPECT_EQ(third.remaining_budget().max_core_steps, 0U);
    EXPECT_EQ(completed_calls.load(), 3U);
}

TEST(ProgramRuntimeTest, InterruptAndAsyncResumeUseOnePublishedCheckpointAndOneDispatchEach) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");

    auto interrupted_handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-interrupt", {}});
    const auto interrupted = interrupted_handle.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed = neograph::async::run_sync(fixture.runtime->resume_async(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-resume")));

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.run_id(), interrupted.run_id());
    EXPECT_EQ(resumed.program_version_id(), version.id());
    EXPECT_EQ(resumed.attempt(), 2U);
    EXPECT_EQ(interrupt_calls.load(), 2U);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "approved");
    ASSERT_TRUE(resumed.checkpoint().has_value());
    EXPECT_EQ(resumed.checkpoint()->core_thread_id, interrupted.checkpoint()->core_thread_id);
    const auto interrupted_budget = interrupted.remaining_budget();
    const auto resumed_budget     = resumed.remaining_budget();
    EXPECT_LE(resumed_budget.wall_time_ms, interrupted_budget.wall_time_ms);
    EXPECT_LE(resumed_budget.model_tokens, interrupted_budget.model_tokens);
    EXPECT_EQ(interrupted_budget.max_program_operations, 0U);
    EXPECT_EQ(resumed_budget.max_program_operations, 0U);
    EXPECT_EQ(interrupted_budget.max_concurrency, 1U);
    EXPECT_EQ(resumed_budget.max_concurrency, 1U);
    EXPECT_LE(resumed_budget.monetary_microunits, interrupted_budget.monetary_microunits);
    EXPECT_LE(resumed_budget.max_concurrency, interrupted_budget.max_concurrency);
    EXPECT_LE(resumed_budget.max_program_operations, interrupted_budget.max_program_operations);
    EXPECT_LE(resumed_budget.max_core_steps, interrupted_budget.max_core_steps);
    EXPECT_LE(resumed_budget.max_dynamic_compiles, interrupted_budget.max_dynamic_compiles);
    EXPECT_LE(resumed_budget.max_child_depth, interrupted_budget.max_child_depth);
    EXPECT_LE(resumed_budget.max_total_children, interrupted_budget.max_total_children);

    const auto journal = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(journal->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, AsyncWaitFromTemporaryHandleRetainsRunControl) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto pending =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-temporary-handle", {}})
            .wait_async();
    const auto result = neograph::async::run_sync(std::move(pending));

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.program_version_id(), version.id());
}

TEST(ProgramRuntimeTest, RepeatedAsyncWaitNeverMissesConcurrentCompletion) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    for (int iteration = 0; iteration < 2000; ++iteration) {
        auto pending =
            fixture.runtime
                ->start("tenant:runtime", version,
                        ProgramInvocation{json::object(), grant(), "trace-wait-race", {}})
                .wait_async();
        const auto result = neograph::async::run_sync(std::move(pending));
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << iteration;
    }
}

TEST(ProgramRuntimeTest, ConcurrentResumeHasOneWinnerAndOneCoreDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-race-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    std::barrier ready(3);
    const auto   resume_once = [&](std::string decision) {
        ready.arrive_and_wait();
        try {
            auto handle = fixture.runtime->resume(
                "tenant:runtime", interrupted.run_id(),
                resume_for(interrupted, json{{"decision", std::move(decision)}}, "trace-race"));
            return handle.wait().status() == ProgramTerminalStatus::Completed ? 1 : -1;
        } catch (const ProgramDiagnosticError& error) {
            const auto& code = error.diagnostic().code;
            return code == "P_RESUME_CONFLICT" || code == "P_RESUME_STATE" ? 0 : -2;
        } catch (...) {
            return -3;
        }
    };

    auto first  = std::async(std::launch::async, [&] { return resume_once("first"); });
    auto second = std::async(std::launch::async, [&] { return resume_once("second"); });
    ready.arrive_and_wait();
    const auto first_result  = first.get();
    const auto second_result = second.get();

    EXPECT_EQ(first_result + second_result, 1);
    EXPECT_NE(first_result, second_result);
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(latest->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, EventSinkFailureCannotRewriteAnAtomicallyCommittedTerminalTransition) {
    for (const auto fail_on : {ProgramEventKind::Started, ProgramEventKind::Core,
                               ProgramEventKind::CheckpointPublished, ProgramEventKind::Terminal}) {
        AdmittedRuntime fixture;
        const auto      version = fixture.admit("runtime-completed");
        auto            sink    = std::make_shared<ThrowingSink>(fail_on);

        auto handle =
            fixture.runtime->start("tenant:runtime", version,
                                   ProgramInvocation{json::object(), grant(), "trace-sink", sink});
        const auto result = handle.wait();
        const bool pre_commit =
            fail_on == ProgramEventKind::Started || fail_on == ProgramEventKind::Core;

        EXPECT_EQ(result.status(),
                  pre_commit ? ProgramTerminalStatus::Failed : ProgramTerminalStatus::Completed);
        EXPECT_EQ(result.failure().has_value(), pre_commit);
        if (pre_commit) EXPECT_EQ(result.failure()->code, "P_EVENT_SINK");
        EXPECT_EQ(result.usage().program_operations, 1U);
        EXPECT_EQ(result.usage().core_steps, fail_on == ProgramEventKind::Started ? 0U : 1U);
        EXPECT_EQ(result.remaining_budget().max_program_operations, 0U);
        EXPECT_EQ(result.remaining_budget().max_core_steps,
                  fail_on == ProgramEventKind::Started ? 20U : 19U);

        const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state,
                  pre_commit ? ContinuationState::Failed : ContinuationState::Completed);

        const auto events   = handle.events_after(0);
        const auto terminal = std::find_if(
            events.begin(), events.end(),
            [](const ProgramEvent& event) { return event.kind == ProgramEventKind::Terminal; });
        ASSERT_NE(terminal, events.end());
        EXPECT_EQ(std::get<ProgramTerminalEvent>(terminal->payload).status, result.status());
        EXPECT_EQ(std::count_if(events.begin(), events.end(),
                                [](const ProgramEvent& event) {
                                    return event.kind == ProgramEventKind::Terminal;
                                }),
                  1);

        const auto terminal_count =
            std::count(sink->seen.begin(), sink->seen.end(), ProgramEventKind::Terminal);
        EXPECT_LE(terminal_count, 1);
    }
}

TEST(ProgramRuntimeTest, ResumeStartedSinkFailurePreservesPublishedCheckpointAndJournal) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-resume-sink-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    auto sink = std::make_shared<ThrowingSink>(ProgramEventKind::Started);

    const auto failed = fixture.runtime
                            ->resume("tenant:runtime", interrupted.run_id(),
                                     resume_for(interrupted, json{{"decision", "unused"}},
                                                "trace-resume-sink-failed", sink))
                            .wait();

    EXPECT_EQ(failed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(failed.failure().has_value());
    EXPECT_EQ(failed.failure()->code, "P_EVENT_SINK");
    ASSERT_TRUE(failed.checkpoint().has_value());
    EXPECT_EQ(failed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", failed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
    EXPECT_EQ(latest->continuation.attempt, 2U);
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
}

TEST(ProgramRuntimeTest, InterruptedCheckpointSinkFailurePreservesCommittedResumeState) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    auto            sink    = std::make_shared<ThrowingSink>(ProgramEventKind::CheckpointPublished);

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-interrupt-sink", sink});

    EXPECT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_FALSE(interrupted.failure().has_value());
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Interrupted);
    EXPECT_EQ(latest->continuation.attempt, 1U);
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "approved"}},
                                                 "trace-resume-after-observer-failure"))
                             .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JournalLatestFailureIsContainedAsTerminalPublicationFailure) {
    auto            journal = std::make_shared<ThrowOnLatestJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto      version = fixture.admit("runtime-completed");

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-journal-read-failure", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JOURNAL_CONFLICT");
    const auto stored = journal->stored_latest(result.run_id());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->continuation.state, ContinuationState::Running);
    const auto events = handle.events_after(0);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const ProgramEvent& event) {
                                return event.kind == ProgramEventKind::CheckpointPublished ||
                                       event.kind == ProgramEventKind::Terminal;
                            }),
              0);
}

TEST(ProgramRuntimeTest, ResumeUsesRequestedCheckpointWhenANewerOrphanExists) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-orphan", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    auto orphan = fixture.checkpoints->load_by_id(interrupted.checkpoint()->checkpoint_id);
    ASSERT_TRUE(orphan.has_value());
    orphan->id         = Checkpoint::generate_id();
    orphan->parent_id  = interrupted.checkpoint()->checkpoint_id;
    orphan->next_nodes = {"__end__"};
    orphan->step += 1;
    fixture.checkpoints->save(*orphan);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "exact"}}, "trace-orphan-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "exact");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, MissingPublishedCheckpointIsTypedIncompatibleWithoutCoreDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-missing", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    fixture.checkpoints->delete_thread(interrupted.checkpoint()->core_thread_id);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-missing-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
    EXPECT_EQ(latest->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, MismatchedPublishedCheckpointIsTypedWithoutCoreDispatch) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-mismatch", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::WrongThread);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-mismatch-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, CheckpointDeletedAfterPrecheckIsTypedWithoutCoreDispatch) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-race", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::MissingAfterFirstExactLoad);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-race-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
}

TEST(ProgramRuntimeTest, PostPrecheckIdentityChangesPreservePublishedCheckpoint) {
    for (const auto mode : {AdversarialCheckpointStore::Mode::WrongThreadAfterPrecheck,
                            AdversarialCheckpointStore::Mode::WrongSchemaAfterPrecheck}) {
        interrupt_calls.store(0);
        auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
        AdmittedRuntime fixture(1, checkpoint_store);
        const auto      version     = fixture.admit("runtime-interrupt");
        const auto      interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-post-precheck", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(interrupted.checkpoint().has_value());

        checkpoint_store->arm(mode);
        const auto resumed = fixture.runtime
                                 ->resume("tenant:runtime", interrupted.run_id(),
                                          resume_for(interrupted, json{{"decision", "unused"}},
                                                     "trace-post-precheck-resume"))
                                 .wait();

        EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
        ASSERT_TRUE(resumed.failure().has_value());
        EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
        ASSERT_TRUE(resumed.checkpoint().has_value());
        EXPECT_EQ(resumed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
        EXPECT_EQ(resumed.checkpoint()->core_thread_id, interrupted.checkpoint()->core_thread_id);
        EXPECT_EQ(resumed.checkpoint()->checkpoint_schema_version,
                  interrupted.checkpoint()->checkpoint_schema_version);
        EXPECT_EQ(interrupt_calls.load(), 1U);
        const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
        ASSERT_TRUE(latest->core_checkpoint.has_value());
        EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
    }
}

TEST(ProgramRuntimeTest, CoreExactResumeRejectsIdentityChangedAtConsumingLoad) {
    for (const auto mode : {AdversarialCheckpointStore::Mode::WrongIdAtCoreLoad,
                            AdversarialCheckpointStore::Mode::WrongSchemaAtCoreLoad}) {
        interrupt_calls.store(0);
        auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
        AdmittedRuntime fixture(1, checkpoint_store);
        const auto      version     = fixture.admit("runtime-interrupt");
        const auto      interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-core-identity", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(interrupted.checkpoint().has_value());

        checkpoint_store->arm(mode);
        const auto resumed = fixture.runtime
                                 ->resume("tenant:runtime", interrupted.run_id(),
                                          resume_for(interrupted, json{{"decision", "unused"}},
                                                     "trace-core-identity-resume"))
                                 .wait();

        EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
        ASSERT_TRUE(resumed.failure().has_value());
        EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
        ASSERT_TRUE(resumed.checkpoint().has_value());
        EXPECT_EQ(resumed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
        EXPECT_EQ(interrupt_calls.load(), 1U);
        const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
    }
}

TEST(ProgramRuntimeTest, FailedResumeUsesAttemptLocalCoreStepAccounting) {
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    auto            document                           = program_document("runtime-failing");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version     = fixture.admit_document(std::move(document));
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-failure-before", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::MissingAfterCoreLoad);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json::object(), "trace-failure-after"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(resumed.usage().core_steps, 1U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedFanoutChargesOneCoreSuperstep) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit_document(failing_fanout_program_document());

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-failing-fanout", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, grant().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedFanoutCountsLegalDoubleUnderscoreNodeNames) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version =
        fixture.admit_document(failing_fanout_program_document("__completed", "__failing"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-prefixed-fanout", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, grant().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedExactResumeIgnoresNewerOrphanForUsage) {
    AdmittedRuntime fixture;
    auto            document                           = program_document("runtime-failing");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version = fixture.admit_document(std::move(document));
    const auto interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-orphan-source", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    auto orphan = fixture.checkpoints->load_by_id(interrupted.checkpoint()->checkpoint_id);
    ASSERT_TRUE(orphan.has_value());
    orphan->id = "unpublished-newer-orphan";
    orphan->step += 10;
    orphan->timestamp += 10;
    fixture.checkpoints->save(*orphan);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json::object(), "trace-orphan-failure"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(resumed.usage().core_steps, 1U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps - 1);
}

// Issue #371: a failed terminal publication of an interrupted run built a
// Failed result that still carried the interrupt, which threw inside the
// scheduler and terminated the process.
TEST(ProgramRuntimeTest, FailedInterruptPublicationFailsTheRunCleanly) {
    AdmittedRuntime fixture;
    auto store = fixture.journal.as<InMemoryProgramTransitionStore>();
    ASSERT_TRUE(store);
    before_fault_interrupt = [store] {
        store->fail_next_publication_for_testing(ProgramTransitionFaultPoint::BeforeCommit);
    };
    const auto version = fixture.admit("runtime-fault-interrupt");
    const auto result  = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-interrupt-publication", {}});
    before_fault_interrupt = nullptr;

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JOURNAL_CONFLICT");
    EXPECT_FALSE(result.interrupt().has_value());
}

// Issue #371: an exception escaping terminalization inside the scheduler
// fails the run with a typed result instead of terminating the process.
TEST(ProgramRuntimeTest, ExceptionEscapingTerminalizationFailsTheRunCleanly) {
    auto            checkpoints = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoints);
    const auto      version = fixture.admit("runtime-failing");
    checkpoints->arm(AdversarialCheckpointStore::Mode::FailTerminalSave);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-terminal-escape", {}});

    EXPECT_GE(checkpoints->terminal_save_failures(), 1U);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_INTERNAL");
    EXPECT_FALSE(result.interrupt().has_value());
    // The runtime stays usable after the failed terminalization.
    checkpoints->arm(AdversarialCheckpointStore::Mode::Normal);
    const auto next = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-terminal-escape-next", {}});
    EXPECT_NE(next.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(checkpoints->terminal_save_failures(), 1U);
}

// These two need QuickJS: their Programs are JavaScript main() sources.
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
// Issue #371: each interrupted callCore of one JavaScript main() is a
// resumable Interrupted result, on every transition store.
void expect_two_interrupted_core_calls_resume(TransitionStoreHandle store) {
    AdmittedRuntime fixture(1, {}, std::move(store), {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("ask", {type: "runtime-interrupt"});
                graph.entry("ask");
                graph.exit("ask");
                return graph;
            }

            export function* main(input) {
                const first = yield ng.callCore("main", {}, "first");
                const second = yield ng.callCore("main", {}, "second");
                return {first: first.value, second: second.value};
            }
        )JS");

    auto budget                   = grant();
    budget.max_program_operations = 2;  // one per callCore
    const auto first              = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-two-interrupts", {}});
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted)
        << (first.failure() ? first.failure()->code : std::string{});
    const auto second = fixture.runtime->resume(
        "tenant:runtime", first.run_id(),
        resume_for(first, json{{"decision", "one"}}, "trace-two-interrupts-1")).wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted)
        << (second.failure() ? second.failure()->code : std::string{});
    ASSERT_TRUE(second.interrupt().has_value());
    const auto done = fixture.runtime->resume(
        "tenant:runtime", second.run_id(),
        resume_for(second, json{{"decision", "two"}}, "trace-two-interrupts-2")).wait();
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed)
        << (done.failure() ? done.failure()->code : std::string{});
    EXPECT_EQ(done.output(), (json{{"first", "one"}, {"second", "two"}}));
    EXPECT_LE(done.remaining_budget().wall_time_ms, budget.wall_time_ms);
    EXPECT_LE(done.remaining_budget().max_core_steps, budget.max_core_steps);
}

TEST(ProgramRuntimeTest, TwoInterruptedCoreCallsEachResumeInMemory) {
    expect_two_interrupted_core_calls_resume({});
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramRuntimeTest, TwoInterruptedCoreCallsEachResumeOnSqlite) {
    const auto path = std::filesystem::temp_directory_path() /
        ("neograph-two-interrupts-" + Checkpoint::generate_id() + ".db");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    expect_two_interrupted_core_calls_resume(
        std::make_shared<SQLiteProgramTransitionStore>(path.string()));
}
#endif

// Issue #374: with two interrupting callCores in one main(), each result's
// execution_trace was one attempt late: ["draft"], ["draft"],
// ["deploy", "deploy"]. Each result lists exactly the Core nodes that ran
// since the previous result, in order.
void expect_each_result_lists_the_nodes_it_ran(TransitionStoreHandle store) {
    using Trace = std::vector<std::string>;
    AdmittedRuntime fixture(1, {}, std::move(store), {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("release");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("draft", {type: "runtime-completed"});
                graph.node("deploy", {type: "runtime-completed"});
                graph.entry("draft");
                graph.edge("draft", "deploy");
                graph.interruptBefore("deploy");
                graph.exit("deploy");
                return graph;
            }

            export function* main(input) {
                const first = yield ng.callCore("release", {}, "first");
                yield ng.checkpoint({between: true}, "between");
                const second = yield ng.callCore("release", {}, "second");
                return {first: first.value, second: second.value};
            }
        )JS");

    auto budget                   = grant();
    budget.max_program_operations = 3;  // two callCores and a checkpoint
    const auto first              = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-attribution", {}});
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted)
        << (first.failure() ? first.failure()->code : std::string{});
    EXPECT_EQ(first.execution_trace(), (Trace{"draft"}));

    // The resume finishes the first callCore and the second one pauses.
    const auto second = fixture.runtime->resume(
        "tenant:runtime", first.run_id(),
        resume_for(first, json{{"decision", "one"}}, "trace-attribution-1")).wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted)
        << (second.failure() ? second.failure()->code : std::string{});
    EXPECT_EQ(second.execution_trace(), (Trace{"deploy", "draft"}));

    // The completed first callCore is replayed from the journal, not re-run.
    const auto done = fixture.runtime->resume(
        "tenant:runtime", second.run_id(),
        resume_for(second, json{{"decision", "two"}}, "trace-attribution-2")).wait();
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed)
        << (done.failure() ? done.failure()->code : std::string{});
    EXPECT_EQ(done.execution_trace(), (Trace{"deploy"}));
    EXPECT_EQ(ProgramResult::parse(done.serialize_canonical()).execution_trace(),
              done.execution_trace());
    EXPECT_EQ(done.output(), (json{{"first", "completed"}, {"second", "completed"}}));
}

TEST(ProgramRuntimeTest, EachResumedResultListsTheNodesItRanInMemory) {
    expect_each_result_lists_the_nodes_it_ran({});
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramRuntimeTest, EachResumedResultListsTheNodesItRanOnSqlite) {
    const auto path = std::filesystem::temp_directory_path() /
        ("neograph-trace-attribution-" + Checkpoint::generate_id() + ".db");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    expect_each_result_lists_the_nodes_it_ran(
        std::make_shared<SQLiteProgramTransitionStore>(path.string()));
}
#endif
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

TEST(ProgramRuntimeTest, CoreFailureIsClassifiedWithNodeAndAttempt) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-failing");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-failure", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    // #364: the classified node failure code, and its record as the witness.
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.failure()->witness["node_failure"],
              (json{{"node", "work"}, {"code", "NG.TaskFailed"}, {"kind", "exhausted"},
                    {"attempts", 1}, {"retryable", true}, {"routable", true}}));
    EXPECT_EQ(result.failure()->core_node, "work");
    EXPECT_EQ(result.failure()->attempts, 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
}

TEST(ProgramRuntimeTest, NonUtf8NodeErrorPreservesJournaledCoreFailure) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-failing-non-utf8");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-non-utf8-error", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_NE(result.failure()->message.find("\\xC7"), std::string::npos);
    const auto events = fixture.journal->load_events("tenant:runtime", result.run_id(), 0);
    ASSERT_FALSE(events.empty());
    bool saw_error = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].sequence, i + 1);
        EXPECT_NO_THROW((void)events[i].serialize_canonical());
        const auto* core = std::get_if<neograph::graph::TypedGraphEvent>(&events[i].payload);
        if (core) {
            const auto* error = std::get_if<neograph::graph::ErrorEvent>(core);
            if (error) {
                saw_error = true;
                EXPECT_NE(error->message.find("non-UTF-8"), std::string::npos);
            }
        }
    }
    EXPECT_TRUE(saw_error);
    EXPECT_EQ(events.back().kind, ProgramEventKind::Terminal);
}

TEST(ProgramRuntimeTest, NonUtf8SystemErrorPreservesPlatformCode) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-failing-system-error");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-non-utf8-system-error", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    const auto events = fixture.journal->load_events("tenant:runtime", result.run_id(), 0);
    bool saw_system_error = false;
    for (const auto& event : events) {
        const auto* core = std::get_if<neograph::graph::TypedGraphEvent>(&event.payload);
        if (!core) continue;
        const auto* error = std::get_if<neograph::graph::ErrorEvent>(core);
        if (!error) continue;
        saw_system_error = true;
        EXPECT_NE(error->message.find("system error code 10053"), std::string::npos);
        EXPECT_NO_THROW((void)event.serialize_canonical());
    }
    EXPECT_TRUE(saw_system_error);
    EXPECT_EQ(events.back().kind, ProgramEventKind::Terminal);
}

TEST(ProgramRuntimeTest, RejectedCoreEventDoesNotConsumeSequence) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-malformed-event-then-fail");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-rejected-core-event", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_NE(result.failure()->message.find("node failed after rejected event"),
              std::string::npos);
    const auto events = fixture.journal->load_events("tenant:runtime", result.run_id(), 0);
    ASSERT_FALSE(events.empty());
    for (std::size_t i = 0; i < events.size(); ++i) EXPECT_EQ(events[i].sequence, i + 1);
    EXPECT_EQ(events.back().kind, ProgramEventKind::Terminal);
}

TEST(ProgramRuntimeTest, CoreStepLimitMapsToBudgetExhausted) {
    completed_calls.store(0);

    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(step_limited_program_document());
    auto            budget  = grant();
    budget.max_core_steps   = 1;

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), budget, "trace-step-budget", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 0U);
    // #443: the engine stopped the run at its grant, and the record says so.
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CORE_STEP_BUDGET");
    EXPECT_EQ(result.failure()->witness["core_steps"],
              (json{{"granted", 1}, {"used", 1}, {"over_by", 0}}));
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::BudgetExhausted);
}

TEST(ProgramRuntimeTest, ARunThatCompletesOverItsCoreStepGrantNamesTheBudget) {
    completed_calls.store(0);

    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(subgraph_step_program_document());
    auto            budget  = grant();
    budget.max_core_steps   = 2;  // the outer graph needs two; its subgraph takes a third

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-step-overrun", {}});

    // Both nodes ran, the second through its subgraph: the plan completed, and
    // the grant was passed on the way.
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_detail(result);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CORE_STEP_BUDGET");
    EXPECT_EQ(result.failure()->witness["core_steps"],
              (json{{"granted", 2}, {"used", 3}, {"over_by", 1}}));
    EXPECT_EQ(result.usage().core_steps, 3U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 0U);
    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::BudgetExhausted);

    // A grant that covers the three steps lets the same run complete.
    budget.max_core_steps = 3;
    const auto within = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-step-within", {}});
    EXPECT_EQ(within.status(), ProgramTerminalStatus::Completed) << failure_detail(within);
    EXPECT_FALSE(within.failure());
}

TEST(ProgramRuntimeTest, RejectsCoreStepBudgetThatCannotFitCoreRunConfig) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            budget  = grant();
    budget.max_core_steps   = static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1;

    try {
        (void)fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), budget, "trace-overflow", {}});
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_START_BUDGET");
    }
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, RejectsWallTimeBudgetThatCannotFitTimerDuration) {
    completed_calls.store(0);
    AdmittedRuntime       fixture;
    const auto            maximum = std::numeric_limits<std::uint64_t>::max();
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("runtime-policy-large-wall")
        .semantic_version("1.0.0")
        .owner_scope("tenant:runtime")
        .admission_profile(fixture.profile)
        .budget_ceiling(BudgetLimits{maximum, 1000, 1000, 1, 1, 20, 1, 1, 1});
    fixture.policy                                         = std::move(policy_builder).build();
    auto document                                          = program_document("runtime-completed");
    document["declared_budget_requirements"][0]["maximum"] = maximum;
    const auto version  = fixture.admit_document(std::move(document));
    auto       budget   = grant();
    budget.wall_time_ms = maximum;

    try {
        (void)fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), budget, "trace-wall-overflow", {}});
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_START_BUDGET");
    }
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, WallTimeBudgetCancelsCoreAndSkipsLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;
    auto handle             = fixture.runtime->start(
        "tenant:runtime", version, ProgramInvocation{json::object(), budget, "trace-timeout", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_FALSE(handle.cancel());
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ImmediateCancellationSuppressesAllLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-cancel-immediate", {}});

    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_LE(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, UserCancellationWinsAndSkipsLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-cancel", {}});

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);
    handle.cancel();
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, HostAdmissionQueuesProgramAttemptsUntilLeaseRelease) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit("runtime-short-blocking");

    const auto host_profile =
        neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
            "runtime-host-v1",
            neograph::HostResourceVector{.cpu_millis = 1},
            {},
            neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1,
                                           false}});
    auto host = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});

    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 2};
    config.host_admission          = host;
    config.host_admission_resolver = [](const ProgramHostAdmissionContext& context) {
        EXPECT_EQ(context.owner_scope, "tenant:runtime");
        EXPECT_EQ(context.operation_id, "root");
        neograph::HostAdmissionRequest request;
        request.resources.cpu_millis = 1;
        request.priority             = 17;
        return request;
    };
    fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));

    auto first =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-host-first", {}});
    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto second =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-host-second", {}});
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(host->snapshot().queued, 1U);

    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(second.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(followup_calls.load(), 2U);
    EXPECT_TRUE(host->snapshot().reserved.empty());
}

TEST(ProgramRuntimeTest, HostAdmissionDeadlineCancelsQueuedAttemptBeforeCoreDispatch) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit("runtime-blocking");

    const auto host_profile =
        neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
            "runtime-host-timeout-v1",
            neograph::HostResourceVector{.cpu_millis = 1},
            {},
            neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1,
                                           false}});
    auto host = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});
    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 2};
    config.host_admission          = host;
    config.host_admission_resolver = [](const ProgramHostAdmissionContext&) {
        neograph::HostAdmissionRequest request;
        request.resources.cpu_millis = 1;
        return request;
    };
    fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));

    auto first = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-host-timeout-holder", {}});
    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto budget         = grant();
    budget.wall_time_ms = 30;
    auto queued         = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-host-timeout-queued", {}});
    const auto queued_result = queued.wait();

    EXPECT_EQ(queued_result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(queued_result.failure().has_value());
    EXPECT_EQ(queued_result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(queued_result.usage().core_steps, 0U);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_TRUE(host->snapshot().reserved.cpu_millis == 1);
    EXPECT_EQ(host->snapshot().queued, 0U);

    EXPECT_TRUE(first.cancel());
    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_TRUE(host->snapshot().reserved.empty());
}

TEST(ProgramRuntimeTest, HostAdmissionRequiresControllerAndResolverTogether) {
    AdmittedRuntime fixture;
    auto            host = std::make_shared<neograph::HostAdmissionController>();

    RuntimeConfig controller_only{
        fixture.catalog, fixture.checkpoints, {}, fixture.journal, fixture.scheduler_thread_count};
    controller_only.host_admission = host;
    EXPECT_THROW({ ProgramRuntime runtime(std::move(controller_only)); }, std::invalid_argument);

    RuntimeConfig resolver_only{
        fixture.catalog, fixture.checkpoints, {}, fixture.journal, fixture.scheduler_thread_count};
    resolver_only.host_admission_resolver = [](const ProgramHostAdmissionContext&) {
        return neograph::HostAdmissionRequest{};
    };
    EXPECT_THROW({ ProgramRuntime runtime(std::move(resolver_only)); }, std::invalid_argument);
}

TEST(ProgramRuntimeTest, QueueWaitCountsAgainstWallTimeBudget) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto      blocker_version = fixture.admit("runtime-completed");
    const auto      queued_version  = fixture.admit("runtime-completed");

    // Node bodies no longer run on the Program scheduler (#367, #368), but
    // event delivery does: the blocker's sink holds the only scheduler thread
    // on its first Core event, so the second run waits in the queue.
    std::atomic<bool> scheduler_held{false};
    auto              blocker_sink = std::make_shared<CallbackSink>();
    blocker_sink->set_callback([&scheduler_held](const ProgramEvent& event) {
        if (event.kind != ProgramEventKind::Core || scheduler_held.exchange(true)) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    });
    auto blocker_budget         = grant();
    blocker_budget.wall_time_ms = 5000;
    auto blocker                = fixture.runtime->start(
        "tenant:runtime", blocker_version,
        ProgramInvocation{json::object(), blocker_budget, "trace-queue-blocker", blocker_sink});
    for (int i = 0; i < 1000 && !scheduler_held.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(scheduler_held.load());

    auto queued_budget         = grant();
    queued_budget.wall_time_ms = 20;
    const auto queued =
        fixture.runtime->run("tenant:runtime", queued_version,
                             ProgramInvocation{json::object(), queued_budget, "trace-queued", {}});

    EXPECT_EQ(queued.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(queued.failure().has_value());
    EXPECT_EQ(queued.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(queued.usage().core_steps, 0U);
    EXPECT_EQ(queued.remaining_budget().wall_time_ms, 0U);
    EXPECT_EQ(blocker.wait().status(), ProgramTerminalStatus::Completed);
    // Only the blocker's node ran.
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, WallTimeExpiresWhenSingleSchedulerThreadIsBlocked) {
    scheduler_blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-scheduler-blocking");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-scheduler-blocked", {}});

    EXPECT_EQ(scheduler_blocking_calls.load(), 1U);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, budget.max_core_steps - 1);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, UserCancellationRemainsFirstCauseAfterTimeoutAttempts) {
    stubborn_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-stubborn");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;
    auto handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), budget, "trace-user-first", {}});

    for (int i = 0; i < 100 && stubborn_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(stubborn_calls.load(), 1U);
    ASSERT_TRUE(handle.cancel());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_FALSE(handle.cancel());
}

TEST(ProgramRuntimeTest, TimeoutRemainsFirstCauseAfterUserCancellationAttempts) {
    stubborn_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-stubborn");
    auto            budget  = grant();
    budget.wall_time_ms     = 30;
    auto handle             = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-timeout-first", {}});

    for (int i = 0; i < 100 && stubborn_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A 30ms budget may expire before the scheduler reaches the first node
    // on a cold or sanitizer-instrumented process. That is still the
    // timeout-first path under test; the user-first companion above covers
    // the case where the stubborn node has entered.
    EXPECT_LE(stubborn_calls.load(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(handle.cancel());
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_FALSE(handle.cancel());
}

TEST(ProgramRuntimeTest, HandleCopiesShareOneResultAndRuntimeTeardownDrainsWork) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto            sink = std::make_shared<CountingSink>();
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            first =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-teardown", sink});
    auto second = first;

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);
    fixture.runtime.reset();

    const auto first_result  = first.wait();
    const auto second_result = second.wait();
    EXPECT_EQ(first_result.run_id(), second_result.run_id());
    EXPECT_EQ(first_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(second_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
    const auto callbacks_after_teardown = sink->calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(sink->calls.load(), callbacks_after_teardown);
}

TEST(ProgramRuntimeTest, MoveAssignmentDrainsTheReplacedRuntime) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-move-assign", {}});

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    ProgramRuntime replacement(
        RuntimeConfig{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 1});
    *fixture.runtime = std::move(replacement);

    const auto result = handle.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ConcurrentAttemptsShareConfiguredSchedulerWithoutCrossTalk) {
    completed_calls.store(0);
    AdmittedRuntime            fixture(4);
    const auto                 version = fixture.admit("runtime-completed");
    std::vector<ProgramHandle> handles;
    handles.reserve(24);
    for (int i = 0; i < 24; ++i) {
        handles.push_back(fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{
                json::object(), grant(), "trace-concurrent-" + std::to_string(i), {}}));
    }

    for (auto& handle : handles) {
        const auto result = handle.wait();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
        const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
    }
    EXPECT_EQ(completed_calls.load(), 24U);
}

TEST(ProgramRuntimeTest, RuntimeShutdownPreservesDeliveredInterruptedRun) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-shutdown-interrupt", {}});
    const auto interrupted = handle.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    fixture.recreate_catalog_and_runtime();

    const auto persisted = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->continuation().state, ContinuationState::Interrupted);
    ASSERT_TRUE(persisted->terminal_result().has_value());
    EXPECT_EQ(persisted->terminal_result()->status(), ProgramTerminalStatus::Interrupted);
    EXPECT_TRUE(persisted->pending_input().has_value());
}

TEST(ProgramRuntimeTest, ExactForkAfterRestartResumesPublishedCheckpointAndPersistsLineage) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    ASSERT_EQ(interrupt_calls.load(), 1U);
    const auto source_record = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_record.has_value());
    ASSERT_EQ(source_record->continuation().state, ContinuationState::Interrupted);
    ASSERT_TRUE(source_record->pending_input().has_value());
    const auto source_pending_id = source_record->pending_input()->call_id();
    const auto source_lineage_before =
        fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_lineage_before);
    ASSERT_EQ(source_lineage_before->active_generation(), 1U);

    fixture.recreate_catalog_and_runtime();
    RunInvocation fork_invocation;
    fork_invocation.owner_scope        = "tenant:runtime";
    fork_invocation.agent_id           = "test-fork";
    fork_invocation.program_version_id = version.id();
    fork_invocation.run_id             = "fork-target-run";
    fork_invocation.budget             = neograph::test::fork_share(source.remaining_budget());
    fork_invocation.input              = json::object();
    fork_invocation.message_sequence   = 19;
    fork_invocation.idempotency_key    = "test-fork:19";
    fork_invocation.correlation_id     = "trace-fork-target";
    fork_invocation.validate();
    const auto forked =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   fork_invocation,
                   ProgramResume{
                       json{{"decision", "forked"}}, "trace-fork-resume", {}, source_pending_id})
            .wait();

    EXPECT_EQ(forked.status(), ProgramTerminalStatus::Completed)
        << (forked.failure() ? forked.failure()->message : "");
    EXPECT_EQ(forked.run_id(), "fork-target-run");
    EXPECT_NE(forked.run_id(), source.run_id());
    EXPECT_EQ(forked.output()["channels"]["value"]["value"], "forked");
    ASSERT_TRUE(forked.checkpoint().has_value());
    EXPECT_NE(forked.checkpoint()->checkpoint_id, source.checkpoint()->checkpoint_id);
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto retried =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   fork_invocation,
                   ProgramResume{
                       json{{"decision", "forked"}}, "trace-fork-resume", {}, source_pending_id})
            .wait();
    EXPECT_EQ(retried.run_id(), forked.run_id());
    EXPECT_EQ(retried.status(), forked.status());
    EXPECT_EQ(interrupt_calls.load(), 2U);
    EXPECT_THROW((void)fixture.runtime->fork(
                     ExactProgramCheckpointReference{source.run_id(),
                                                     source.checkpoint()->checkpoint_id},
                     fork_invocation,
                     ProgramResume{json{{"decision", "different"}}, "trace-fork-conflict", {},
                                   source_pending_id}),
                 ProgramDiagnosticError);

    const auto target_record = fixture.journal->load("tenant:runtime", forked.run_id());
    ASSERT_TRUE(target_record.has_value());
    ASSERT_TRUE(target_record->fork_receipt().has_value());
    EXPECT_TRUE(target_record->fork_receipt()->compatible());
    ASSERT_TRUE(target_record->fork_receipt()->initial_resume_binding().has_value());
    EXPECT_EQ(target_record->fork_receipt()->initial_resume_binding()->target_pending_id,
              source_pending_id);
    EXPECT_TRUE(target_record->fork_receipt()->matches_initial_resume(
        source_pending_id, json{{"decision", "forked"}}));
    EXPECT_EQ(target_record->fork_source_run_id(), source.run_id());
    EXPECT_EQ(target_record->fork_source_program_version_id(), version.id());
    EXPECT_EQ(target_record->fork_source_checkpoint_id(), source.checkpoint()->checkpoint_id);
    EXPECT_EQ(target_record->invocation(), fork_invocation);

    const auto source_lineage_after =
        fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    const auto target_lineage =
        fixture.journal->load_run_lineage("tenant:runtime", forked.run_id());
    ASSERT_TRUE(source_lineage_after);
    ASSERT_TRUE(target_lineage);
    EXPECT_EQ(source_lineage_after->lineage_id(), source_lineage_before->lineage_id());
    EXPECT_NE(target_lineage->lineage_id(), source_lineage_after->lineage_id());
    EXPECT_EQ(source_lineage_after->active_generation(), 1U);
    EXPECT_EQ(target_lineage->active_generation(), 1U);
    const auto source_generation = fixture.journal->load_generation(
        "tenant:runtime", source_lineage_after->lineage_id(), 1);
    const auto target_generation = fixture.journal->load_generation(
        "tenant:runtime", target_lineage->lineage_id(), 1);
    ASSERT_TRUE(source_generation);
    ASSERT_TRUE(target_generation);
    EXPECT_EQ(source_generation->run_id(), source.run_id());
    EXPECT_EQ(target_generation->run_id(), forked.run_id());
    const auto expected_source_budget =
        neograph::test::fork_remainder(source.remaining_budget(), fork_invocation.budget);
    EXPECT_EQ(source_lineage_after->remaining_budget(), expected_source_budget);
    EXPECT_EQ(source_lineage_after->remaining_budget().max_concurrency,
              source.remaining_budget().max_concurrency);
    EXPECT_EQ(target_lineage->remaining_budget(), target_record->remaining_budget());

    const auto source_after = fixture.runtime->reconnect("tenant:runtime", source.run_id()).wait();
    EXPECT_EQ(source_after.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(source_after.checkpoint()->checkpoint_id, source.checkpoint()->checkpoint_id);
    const auto resumed_source =
        fixture.runtime
            ->resume("tenant:runtime", source.run_id(),
                     ProgramResume{json{{"decision", "source"}}, "trace-source-after-fork", {},
                                   source_pending_id})
            .wait();
    EXPECT_EQ(resumed_source.status(), ProgramTerminalStatus::Completed)
        << failure_detail(resumed_source);
    EXPECT_EQ(interrupt_calls.load(), 3U);
    const auto retried_after_source_progress =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   fork_invocation,
                   ProgramResume{
                       json{{"decision", "forked"}}, "trace-fork-resume", {}, source_pending_id})
            .wait();
    EXPECT_EQ(retried_after_source_progress.id(), forked.id());
    EXPECT_EQ(interrupt_calls.load(), 3U);
}

TEST(ProgramRuntimeTest, ForkRetryAfterLaterInterruptUsesImmutableInitialResumeBinding) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(two_interrupt_program_document());
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-later-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());
    const auto source_pending_id = pending_call_id(source);

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-fork-later-interrupt";
    invocation.program_version_id = version.id();
    invocation.run_id             = "fork-later-interrupt-target";
    invocation.budget             = neograph::test::fork_share(source.remaining_budget());
    invocation.input              = json::object();
    invocation.message_sequence   = 23;
    invocation.idempotency_key    = "test-fork-later-interrupt:23";
    invocation.correlation_id     = "trace-fork-later-target";
    invocation.validate();
    const ExactProgramCheckpointReference exact_source{source.run_id(),
                                                       source.checkpoint()->checkpoint_id};
    const ProgramResume                   initial_resume{
        json{{"decision", "first"}}, "trace-fork-later-resume", {}, source_pending_id};

    const auto forked = fixture.runtime->fork(exact_source, invocation, initial_resume).wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Interrupted);
    const auto later_pending_id = pending_call_id(forked);
    EXPECT_NE(later_pending_id, source_pending_id);
    EXPECT_EQ(interrupt_calls.load(), 3U);

    fixture.recreate_catalog_and_runtime();
    const auto retried = fixture.runtime->fork(exact_source, invocation, initial_resume).wait();
    EXPECT_EQ(retried.id(), forked.id());
    EXPECT_EQ(retried.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(pending_call_id(retried), later_pending_id);
    EXPECT_EQ(interrupt_calls.load(), 3U);

    const auto expect_conflict = [&](ProgramResume resume) {
        try {
            (void)fixture.runtime->fork(exact_source, invocation, std::move(resume));
            FAIL() << "Conflicting fork retry unexpectedly reconnected";
        } catch (const ProgramDiagnosticError& error) {
            EXPECT_EQ(error.diagnostic().code, "P_RUN_CONFLICT");
        }
    };
    expect_conflict(ProgramResume{
        json{{"decision", "changed"}}, "trace-fork-later-value", {}, source_pending_id});
    expect_conflict(
        ProgramResume{json{{"decision", "first"}}, "trace-fork-later-id", {}, later_pending_id});
}

TEST(ProgramRuntimeTest, ConcurrentExactForksAtomicallyDebitSourceOnce) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-race-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());
    const auto source_record = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_record);
    ASSERT_TRUE(source_record->pending_input());
    const auto pending_id = source_record->pending_input()->call_id();

    fixture.recreate_catalog_and_runtime();
    // Each fork asks for three fifths of the wall time and the Core steps, so
    // the source's remainder fits one of them and not both.
    auto racing_share            = neograph::test::fork_share(source.remaining_budget());
    racing_share.wall_time_ms    = source.remaining_budget().wall_time_ms * 3 / 5;
    racing_share.max_core_steps  = source.remaining_budget().max_core_steps * 3 / 5;
    const auto make_invocation = [&](std::string run_id) {
        RunInvocation invocation;
        invocation.owner_scope        = "tenant:runtime";
        invocation.agent_id           = "test-fork-race";
        invocation.program_version_id = version.id();
        invocation.run_id             = std::move(run_id);
        invocation.budget             = racing_share;
        invocation.input              = json::object();
        invocation.message_sequence   = 20;
        invocation.idempotency_key    = "test-fork-race:" + invocation.run_id;
        invocation.correlation_id     = "trace-" + invocation.run_id;
        invocation.validate();
        return invocation;
    };
    std::barrier ready(3);
    auto fork = [&](std::string run_id) {
        ready.arrive_and_wait();
        try {
            const auto result = fixture.runtime
                                    ->fork(ExactProgramCheckpointReference{
                                               source.run_id(),
                                               source.checkpoint()->checkpoint_id},
                                           make_invocation(run_id),
                                           ProgramResume{json{{"decision", run_id}},
                                                         "trace-fork-race-resume", {}, pending_id})
                                    .wait();
            return result.status() == ProgramTerminalStatus::Completed;
        } catch (const ProgramDiagnosticError&) {
            return false;
        }
    };
    auto first  = std::async(std::launch::async, [&] { return fork("fork-race-first"); });
    auto second = std::async(std::launch::async, [&] { return fork("fork-race-second"); });
    ready.arrive_and_wait();
    const auto first_won  = first.get();
    const auto second_won = second.get();

    EXPECT_NE(first_won, second_won);
    const auto first_record = fixture.journal->load("tenant:runtime", "fork-race-first");
    const auto second_record = fixture.journal->load("tenant:runtime", "fork-race-second");
    EXPECT_NE(first_record.has_value(), second_record.has_value());
    const auto winner_id = first_record ? "fork-race-first" : "fork-race-second";
    const auto source_lineage =
        fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    const auto target_lineage = fixture.journal->load_run_lineage("tenant:runtime", winner_id);
    ASSERT_TRUE(source_lineage);
    ASSERT_TRUE(target_lineage);
    EXPECT_EQ(source_lineage->active_generation(), 1U);
    EXPECT_EQ(source_lineage->remaining_budget(),
              neograph::test::fork_remainder(source.remaining_budget(), racing_share));
    EXPECT_EQ(target_lineage->active_generation(), 1U);
    EXPECT_NE(target_lineage->lineage_id(), source_lineage->lineage_id());
    EXPECT_EQ(fixture.journal
                  ->load_generation("tenant:runtime", target_lineage->lineage_id(), 1)
                  ->run_id(),
              winner_id);
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, ConcurrentIdenticalForksReconnectOneTargetDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-identical-fork-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());
    const auto pending_id = pending_call_id(source);

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-identical-fork";
    invocation.program_version_id = version.id();
    invocation.run_id             = "identical-fork-target";
    invocation.budget             = neograph::test::fork_share(source.remaining_budget());
    invocation.input              = json::object();
    invocation.message_sequence   = 22;
    invocation.idempotency_key    = "test-identical-fork:22";
    invocation.correlation_id     = "trace-identical-fork-target";
    invocation.validate();

    std::barrier ready(3);
    auto fork = [&] {
        ready.arrive_and_wait();
        return fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   invocation,
                   ProgramResume{json{{"decision", "same"}}, "trace-identical-fork-resume", {},
                                 pending_id})
            .wait();
    };
    auto first  = std::async(std::launch::async, fork);
    auto second = std::async(std::launch::async, fork);
    ready.arrive_and_wait();
    const auto first_result  = first.get();
    const auto second_result = second.get();
    EXPECT_EQ(first_result.run_id(), "identical-fork-target");
    EXPECT_EQ(second_result.run_id(), "identical-fork-target");
    EXPECT_EQ(first_result.id(), second_result.id());
    EXPECT_EQ(interrupt_calls.load(), 2U);
    const auto events = fixture.journal->load_events("tenant:runtime", "identical-fork-target");
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const ProgramEvent& event) {
                  return event.kind == ProgramEventKind::Started;
              }),
              1);
}

// #414: a fork's budget. Amounts are split: the fork's share leaves the source
// the rest. max_concurrency is a limit: the fork carries its own and the
// source keeps its own. A fork that would leave the source without the wall
// time or the core steps every resume needs is refused at fork().
namespace {

struct ForkBudgetSource {
    ProgramResult source;
    std::string   pending_id;
    ProgramVersion version;
};

ForkBudgetSource pause_for_fork_budget(AdmittedRuntime& fixture, const std::string& trace) {
    interrupt_calls.store(0);
    auto version = fixture.admit("runtime-interrupt");
    auto source  = fixture.runtime
                       ->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), trace, {}})
                       .wait();
    EXPECT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    auto pending_id = pending_call_id(source);
    return ForkBudgetSource{std::move(source), std::move(pending_id), std::move(version)};
}

RunInvocation fork_invocation_with(const ForkBudgetSource& paused, std::string run_id,
                                   const RunBudget& budget) {
    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-fork-budget";
    invocation.program_version_id = paused.version.id();
    invocation.run_id             = std::move(run_id);
    invocation.budget             = budget;
    invocation.input              = json::object();
    invocation.message_sequence   = 24;
    invocation.idempotency_key    = "test-fork-budget:" + invocation.run_id;
    invocation.correlation_id     = "trace-" + invocation.run_id;
    invocation.validate();
    return invocation;
}

}  // namespace

TEST(ProgramRuntimeTest, ForkTakesItsShareOfTheAmountsAndLeavesTheSourceItsConcurrency) {
    AdmittedRuntime fixture;
    const auto      paused = pause_for_fork_budget(fixture, "trace-fork-share-source");
    const auto      before = fixture.journal->load_run_lineage("tenant:runtime", paused.source.run_id());
    ASSERT_TRUE(before);
    const auto left  = before->remaining_budget();
    const auto share = neograph::test::fork_share(left);
    ASSERT_EQ(share.max_concurrency, left.max_concurrency);
    ASSERT_GE(left.max_concurrency, 1U);

    const auto forked =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{paused.source.run_id(),
                                                   paused.source.checkpoint()->checkpoint_id},
                   fork_invocation_with(paused, "fork-budget-share", share),
                   ProgramResume{json{{"decision", "forked"}}, "trace-fork-share-resume", {},
                                 paused.pending_id})
            .wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << failure_detail(forked);

    const auto expected = neograph::test::fork_remainder(left, share);
    const auto after = fixture.journal->load_run_lineage("tenant:runtime", paused.source.run_id());
    ASSERT_TRUE(after);
    EXPECT_EQ(after->remaining_budget(), expected);
    EXPECT_EQ(after->remaining_budget().max_concurrency, left.max_concurrency);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", paused.source.run_id(),
                     ProgramResume{json{{"decision", "source"}}, "trace-fork-share-source-resume", {},
                                   paused.pending_id})
            .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
    EXPECT_EQ(resumed.remaining_budget().max_concurrency, left.max_concurrency);
}

TEST(ProgramRuntimeTest, ForkThatWouldLeaveTheSourceWithoutAResumeResourceIsRefused) {
    for (const std::string field : {"wall_time_ms", "max_core_steps"}) {
        SCOPED_TRACE(field);
        AdmittedRuntime fixture;
        const auto      paused = pause_for_fork_budget(fixture, "trace-fork-refused-source-" + field);
        const auto before =
            fixture.journal->load_run_lineage("tenant:runtime", paused.source.run_id());
        ASSERT_TRUE(before);
        auto whole = neograph::test::fork_share(before->remaining_budget());
        if (field == "wall_time_ms") whole.wall_time_ms = before->remaining_budget().wall_time_ms;
        if (field == "max_core_steps") whole.max_core_steps = before->remaining_budget().max_core_steps;

        const std::string target_id = "fork-budget-refused-" + field;
        try {
            (void)fixture.runtime->fork(
                ExactProgramCheckpointReference{paused.source.run_id(),
                                                paused.source.checkpoint()->checkpoint_id},
                fork_invocation_with(paused, target_id, whole),
                ProgramResume{json{{"decision", "forked"}}, "trace-fork-refused-resume", {},
                              paused.pending_id});
            FAIL() << "a fork that takes all of " << field << " was accepted";
        } catch (const ProgramDiagnosticError& error) {
            EXPECT_EQ(error.diagnostic().code, "P_FORK_SOURCE_BUDGET");
            EXPECT_NE(error.diagnostic().message.find(field), std::string::npos)
                << error.diagnostic().message;
            EXPECT_EQ(error.diagnostic().witness.value("resource", std::string()), field);
        }

        // Nothing changed: no target run, the source's lineage as it was, and the
        // source resumes with everything it had.
        EXPECT_FALSE(fixture.journal->load("tenant:runtime", target_id));
        const auto after =
            fixture.journal->load_run_lineage("tenant:runtime", paused.source.run_id());
        ASSERT_TRUE(after);
        EXPECT_EQ(after->id(), before->id());
        EXPECT_EQ(interrupt_calls.load(), 1U);
        const auto resumed =
            fixture.runtime
                ->resume("tenant:runtime", paused.source.run_id(),
                         ProgramResume{json{{"decision", "source"}}, "trace-fork-refused-source-resume",
                                       {}, paused.pending_id})
                .wait();
        EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
    }
}

TEST(ProgramRuntimeTest, ForkLargerThanTheSourceRemainderIsStillRefusedAsBefore) {
    AdmittedRuntime fixture;
    const auto      paused = pause_for_fork_budget(fixture, "trace-fork-too-much-source");
    auto            too_much = neograph::test::fork_share(paused.source.remaining_budget());
    too_much.wall_time_ms    = paused.source.remaining_budget().wall_time_ms + 1;
    try {
        (void)fixture.runtime->fork(
            ExactProgramCheckpointReference{paused.source.run_id(),
                                            paused.source.checkpoint()->checkpoint_id},
            fork_invocation_with(paused, "fork-budget-too-much", too_much),
            ProgramResume{json{{"decision", "forked"}}, "trace-fork-too-much-resume", {},
                          paused.pending_id});
        FAIL() << "a fork larger than the source's remainder was accepted";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_FORK_BUDGET");
    }
}

// The forks of one pause that several threads make at the same time. Each is published against the lineage the one before
// it left (the runtime takes its fork lock before it reads the lineage), so the budgets they take add up to at most what the
// paused run had, whatever the order, and the paused run keeps the rest.
namespace {

struct ConcurrentFork {
    std::optional<ProgramResult> result;
    std::string                  refusal;  // the code of the runtime's refusal, or the text of another failure
};

// Runs a pause of the interrupting Program under a budget that can be shared, and makes `count` forks of it from `count`
// threads released together, each asking for `each`. `check` sees the fixture and the outcomes.
void fork_from_threads(int count, std::uint64_t numerator, std::uint64_t denominator,
                       const std::function<void(AdmittedRuntime&, const ProgramResult&, const RunBudget& left,
                                                const RunBudget& each, const std::vector<ConcurrentFork>&)>& check) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(4);
    auto            document = program_document("runtime-interrupt");
    document["program_schema_version"]                     = PROGRAM_SCHEMA_VERSION_V2;
    document["declared_budget_requirements"][4]["maximum"] = 16;
    const auto version = fixture.admit_document(std::move(document));
    const auto source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 1, 16, 20, 0, 0, 0},
                                      "trace-concurrent-fork-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    const auto pending_id = pending_call_id(source);
    const auto lineage    = fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    const auto left = lineage->remaining_budget();
    const auto each = neograph::test::fork_fraction(left, numerator, denominator);

    std::vector<ConcurrentFork> outcomes(static_cast<std::size_t>(count));
    std::barrier                start(count);
    std::vector<std::thread>    threads;
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&, i] {
            RunInvocation invocation;
            invocation.owner_scope        = "tenant:runtime";
            invocation.agent_id           = "test-concurrent-fork";
            invocation.program_version_id = version.id();
            invocation.run_id             = "concurrent-fork-" + std::to_string(i);
            invocation.budget             = each;
            invocation.input              = json::object();
            invocation.message_sequence   = 25;
            invocation.idempotency_key    = "test-concurrent-fork:" + invocation.run_id;
            invocation.correlation_id     = "trace-" + invocation.run_id;
            invocation.validate();
            start.arrive_and_wait();
            try {
                outcomes[static_cast<std::size_t>(i)].result =
                    fixture.runtime
                        ->fork(ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
                               invocation,
                               ProgramResume{json{{"decision", "forked"}}, "trace-concurrent-fork-resume", {}, pending_id})
                        .wait();
            } catch (const ProgramDiagnosticError& error) {
                outcomes[static_cast<std::size_t>(i)].refusal = error.diagnostic().code;
            } catch (const std::exception& error) {
                outcomes[static_cast<std::size_t>(i)].refusal = std::string("exception: ") + error.what();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(fixture, source, left, each, outcomes);
}

}  // namespace

TEST(ProgramRuntimeTest, ForksOfOnePauseMadeByThreadsAtTheSameTimeAllFitAndTakeTheirBudgetOnce) {
    for (int round = 0; round < 15; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        fork_from_threads(4, 1, 5, [](AdmittedRuntime& fixture, const ProgramResult& source, const RunBudget& left,
                                      const RunBudget& each, const std::vector<ConcurrentFork>& outcomes) {
            RunBudget expected = left;
            for (std::size_t i = 0; i < outcomes.size(); ++i) {
                ASSERT_TRUE(outcomes[i].result) << "fork " << i << ": " << outcomes[i].refusal;
                EXPECT_EQ(outcomes[i].result->status(), ProgramTerminalStatus::Completed) << failure_detail(*outcomes[i].result);
                const auto record = fixture.journal->load("tenant:runtime", "concurrent-fork-" + std::to_string(i));
                ASSERT_TRUE(record);
                // The child's record holds the amounts it was given.
                EXPECT_EQ(record->invocation().budget, each);
                expected = neograph::test::fork_remainder(expected, each);
            }
            const auto after = fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
            ASSERT_TRUE(after);
            EXPECT_EQ(after->remaining_budget(), expected);

            // A fork made again is that fork: it is not debited a second time and runs nothing.
            const auto calls = interrupt_calls.load();
            for (std::size_t i = 0; i < outcomes.size(); ++i) {
                RunInvocation invocation;
                invocation.owner_scope        = "tenant:runtime";
                invocation.agent_id           = "test-concurrent-fork";
                invocation.program_version_id = source.program_version_id();
                invocation.run_id             = "concurrent-fork-" + std::to_string(i);
                invocation.budget             = each;
                invocation.input              = json::object();
                invocation.message_sequence   = 25;
                invocation.idempotency_key    = "test-concurrent-fork:" + invocation.run_id;
                invocation.correlation_id     = "trace-" + invocation.run_id;
                invocation.validate();
                const auto again =
                    fixture.runtime
                        ->fork(ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
                               invocation,
                               ProgramResume{json{{"decision", "forked"}}, "trace-concurrent-fork-resume", {},
                                             pending_call_id(source)})
                        .wait();
                EXPECT_EQ(again.run_id(), outcomes[i].result->run_id());
            }
            EXPECT_EQ(interrupt_calls.load(), calls);
            const auto replayed = fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
            ASSERT_TRUE(replayed);
            EXPECT_EQ(replayed->remaining_budget(), expected);

            // The paused run is still resumable with what is left.
            const auto resumed =
                fixture.runtime
                    ->resume("tenant:runtime", source.run_id(),
                             ProgramResume{json{{"decision", "source"}}, "trace-concurrent-fork-source-resume", {},
                                           pending_call_id(source)})
                    .wait();
            EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_detail(resumed);
        });
    }
}

TEST(ProgramRuntimeTest, ForksOfOnePauseMadeByThreadsAtTheSameTimeNeverTakeMoreThanItHasLeft) {
    for (int round = 0; round < 15; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        // Two fifths each: two fit and the others ask for more than those two left, whichever threads get there first.
        fork_from_threads(4, 2, 5, [](AdmittedRuntime& fixture, const ProgramResult& source, const RunBudget& left,
                                      const RunBudget& each, const std::vector<ConcurrentFork>& outcomes) {
            int completed = 0, refused = 0;
            for (std::size_t i = 0; i < outcomes.size(); ++i) {
                const auto id = "concurrent-fork-" + std::to_string(i);
                if (outcomes[i].result) {
                    ++completed;
                    EXPECT_EQ(outcomes[i].result->status(), ProgramTerminalStatus::Completed) << failure_detail(*outcomes[i].result);
                    EXPECT_TRUE(fixture.journal->load("tenant:runtime", id));
                } else {
                    ++refused;
                    EXPECT_EQ(outcomes[i].refusal, "P_FORK_BUDGET") << "fork " << i;
                    EXPECT_FALSE(fixture.journal->load("tenant:runtime", id));
                }
            }
            EXPECT_EQ(completed, 2);
            EXPECT_EQ(refused, 2);
            const auto after = fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
            ASSERT_TRUE(after);
            EXPECT_EQ(after->remaining_budget(),
                      neograph::test::fork_remainder(neograph::test::fork_remainder(left, each), each));
        });
    }
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramRuntimeTest, ExactForkLineageSurvivesSqliteRuntimeReconnect) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-runtime-fork-lineage-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    interrupt_calls.store(0);
    {
        TransitionStoreHandle transitions = std::make_shared<SQLiteProgramTransitionStore>(path);
        AdmittedRuntime fixture(1, {}, transitions);
        const auto      version = fixture.admit("runtime-interrupt");
        const auto      source =
            fixture.runtime
                ->start("tenant:runtime", version,
                        ProgramInvocation{json::object(), grant(), "trace-sqlite-fork-source", {}})
                .wait();
        ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(source.checkpoint());

        RunInvocation invocation;
        invocation.owner_scope        = "tenant:runtime";
        invocation.agent_id           = "test-sqlite-fork";
        invocation.program_version_id = version.id();
        invocation.run_id             = "sqlite-fork-target";
        invocation.budget             = neograph::test::fork_share(source.remaining_budget());
        invocation.input              = json::object();
        invocation.message_sequence   = 21;
        invocation.idempotency_key    = "test-sqlite-fork:21";
        invocation.correlation_id     = "trace-sqlite-fork-target";
        invocation.validate();
        const auto target =
            fixture.runtime
                ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                       source.checkpoint()->checkpoint_id},
                       invocation,
                       ProgramResume{json{{"decision", "sqlite"}}, "trace-sqlite-fork-resume", {},
                                     pending_call_id(source)})
                .wait();
        ASSERT_EQ(target.status(), ProgramTerminalStatus::Completed);

        fixture.runtime.reset();
        fixture.journal.reset();
        fixture.journal = std::make_shared<SQLiteProgramTransitionStore>(path);
        fixture.runtime = fixture.make_runtime();
        const auto lineage =
            fixture.journal->load_run_lineage("tenant:runtime", target.run_id());
        ASSERT_TRUE(lineage);
        const auto source_lineage =
            fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
        ASSERT_TRUE(source_lineage);
        EXPECT_EQ(lineage->active_generation(), 1U);
        EXPECT_EQ(source_lineage->active_generation(), 1U);
        EXPECT_NE(source_lineage->lineage_id(), lineage->lineage_id());
        EXPECT_EQ(source_lineage->remaining_budget(),
                  neograph::test::fork_remainder(source.remaining_budget(), invocation.budget));
        EXPECT_EQ(fixture.journal
                      ->load_generation("tenant:runtime", lineage->lineage_id(), 1)
                      ->run_id(),
                  target.run_id());
    }
    std::filesystem::remove(path);
}
#endif

TEST(ProgramRuntimeTest, ForkAllowsBudgetOnlyMigrationButEnforcesTargetDeclaredBounds) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      source_version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", source_version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-budget-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto source_before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_before.has_value());

    auto target_document = program_document("runtime-interrupt");
    target_document["declared_budget_requirements"][1]["maximum"] = 10;
    const auto target_version = fixture.admit_document(std::move(target_document));
    const auto plan =
        fixture.catalog->plan_migration("tenant:runtime", source_version.id(), target_version.id());
    EXPECT_TRUE(plan.is_compatible());

    const std::string rejected_run_id = "fork-budget-rejected";
    try {
        (void)fixture.runtime->fork(
            "tenant:runtime",
            ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
            target_version,
            ProgramInvocation{json::object(),
                              source.remaining_budget(),
                              "trace-fork-budget-target",
                              {},
                              rejected_run_id},
            ProgramResume{json{{"decision", "must-not-dispatch"}},
                          "trace-fork-budget-resume",
                          {},
                          pending_call_id(source)});
        FAIL() << "budget outside target admitted bounds unexpectedly forked";
    } catch (const std::exception& error) {
        EXPECT_NE(std::string(error.what()).find("target admitted bounds"), std::string::npos);
    }

    EXPECT_FALSE(fixture.journal->load("tenant:runtime", rejected_run_id).has_value());
    const auto source_after = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(source_after->id(), source_before->id());
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ForkRejectsSourceRunIdBeforeCloningCheckpoint) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-same-run", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(before.has_value());
    const auto source_thread             = before->exact_checkpoint()->core_thread_id;
    const auto source_checkpoints_before = fixture.checkpoints->list(source_thread, 100).size();
    ProgramInvocation invocation{json::object(),
                                 source.remaining_budget(),
                                 "trace-fork-same-run-target",
                                 {},
                                 source.run_id()};
    EXPECT_THROW(
        (void)fixture.runtime->fork(
            "tenant:runtime",
            ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
            version, std::move(invocation),
            ProgramResume{json{{"decision", "must-not-clone"}},
                          "trace-fork-same-run-resume",
                          {},
                          before->pending_input()->call_id()}),
        std::exception);

    const auto after = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->id(), before->id());
    EXPECT_EQ(interrupt_calls.load(), 1U);
    EXPECT_EQ(fixture.checkpoints->list(source_thread, 100).size(), source_checkpoints_before);
}

TEST(ProgramRuntimeTest, DirectForkOverloadRejectsUntrackedChildTopology) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-child-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());

    ProgramInvocation invocation{json::object(),
                                 source.remaining_budget(),
                                 "trace-fork-child-target",
                                 {},
                                 "fork-child-target",
                                 "untracked-parent",
                                 1};
    EXPECT_THROW((void)fixture.runtime->fork(
                     "tenant:runtime",
                     ExactProgramCheckpointReference{source.run_id(),
                                                     source.checkpoint()->checkpoint_id},
                     version, std::move(invocation),
                     ProgramResume{json{{"decision", "must-not-fork"}},
                                   "trace-fork-child-resume", {}, pending_call_id(source)}),
                 std::invalid_argument);
    EXPECT_FALSE(fixture.journal->load("tenant:runtime", "fork-child-target"));
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ForkMismatchesRejectBeforeTargetRunAndLeaveSourceUnchanged) {
    interrupt_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      source_version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", source_version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-reject-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto source_before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_before.has_value());

    struct RejectionCase {
        std::string            run_id;
        ForkCompatibilityField field;
        json                   document;
    };
    std::vector<RejectionCase> cases;
    {
        auto document = program_document("runtime-interrupt");
        document["root"]["definition"]["channels"]["extra"] =
            json{{"reducer", "runtime-overwrite"}, {"initial", nullptr}};
        cases.push_back(
            {"fork-reject-channel", ForkCompatibilityField::Channel, std::move(document)});
    }
    {
        auto document = program_document("runtime-interrupt");
        document["root"]["definition"]["channels"]["value"]["reducer"] = "runtime-alternate";
        cases.push_back(
            {"fork-reject-reducer", ForkCompatibilityField::Reducer, std::move(document)});
    }
    {
        auto document                                       = program_document("runtime-interrupt");
        document["root"]["definition"]["nodes"]["followup"] = json{{"type", "runtime-followup"}};
        document["root"]["definition"]["edges"] =
            json::array({json{{"from", "__start__"}, {"to", "work"}},
                         json{{"from", "work"}, {"to", "followup"}},
                         json{{"from", "followup"}, {"to", "__end__"}}});
        cases.push_back({"fork-reject-continuation", ForkCompatibilityField::Continuation,
                         std::move(document)});
    }

    for (auto& item : cases) {
        const auto target = fixture.admit_document(std::move(item.document));
        try {
            (void)fixture.runtime->fork(
                "tenant:runtime",
                ExactProgramCheckpointReference{source.run_id(),
                                                source.checkpoint()->checkpoint_id},
                target,
                ProgramInvocation{json::object(),
                                  source.remaining_budget(),
                                  "trace-fork-reject",
                                  {},
                                  item.run_id},
                ProgramResume{
                    json{{"decision", "must-not-dispatch"}}, "trace-fork-reject-resume", {}});
            FAIL() << "incompatible fork unexpectedly created a target run";
        } catch (const ProgramForkCompatibilityError& error) {
            EXPECT_TRUE(
                std::any_of(error.receipt().witnesses().begin(), error.receipt().witnesses().end(),
                            [&](const auto& witness) { return witness.field == item.field; }));
        }
        EXPECT_FALSE(fixture.journal->load("tenant:runtime", item.run_id).has_value());
        const auto source_now = fixture.journal->load("tenant:runtime", source.run_id());
        ASSERT_TRUE(source_now.has_value());
        EXPECT_EQ(source_now->id(), source_before->id());
    }

    EXPECT_EQ(interrupt_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

json orchestration_document(json root, std::string node_type = "runtime-completed") {
    auto document                      = program_document(std::move(node_type));
    document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    root["name"]                       = "main";
    root["definition"]                 = std::move(document["root"]["definition"]);
    document["declared_budget_requirements"][3]["maximum"] = 4;
    document["declared_budget_requirements"][4]["maximum"] = 32;
    document["root"]                                       = std::move(root);
    return document;
}

json orchestration_document() {
    return orchestration_document(
        json{{"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "emit"}, {"value", json{{"kind", "core_done"}}}},
                                       json{{"op", "return"}, {"value", json{{"ok", true}}}}})}});
}

ProgramResult run_orchestration(AdmittedRuntime& fixture,
                                json             document,
                                json             input    = json::object(),
                                std::string      trace_id = "trace-orchestration") {
    const auto version = fixture.admit_document(std::move(document));
    return fixture.runtime->run("tenant:runtime", version,
                                ProgramInvocation{std::move(input),
                                                  RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                                                  std::move(trace_id),
                                                  {}});
}

TEST(ProgramRuntimeTest, BranchSelectsWithoutDispatchingTheUnselectedCore) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "branch"},
                      {"condition", json{{"path", "/route"}, {"equals", "then"}}},
                      {"then", json{{"op", "call_core"}}},
                      {"else", json{{"op", "return"}, {"value", json{{"selected", "else"}}}}}}),
        json{{"route", "else"}}, "trace-branch");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"selected", "else"}}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, BoundedLoopReportsBudgetExhaustionAfterTheLastAllowedIteration) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "loop"},
                                         {"condition", json{{"path", "/never"}, {"exists", false}}},
                                         {"max_iterations", 2},
                                         {"body", json{{"op", "call_core"}}}}),
        json::object(), "trace-loop");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_LOOP_BOUND");
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, RetryRetriesCoreFailuresAndPreservesFailureClassification) {
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "retry"}, {"max_attempts", 2}, {"body", json{{"op", "call_core"}}}},
            "runtime-failing"),
        json::object(), "trace-retry");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/body");
    EXPECT_EQ(result.failure()->attempts, 2U);
}

// #364: the Program Retry operation asks the node failure classifier. A
// failure it does not retry fails the run as if there were no Retry.
TEST(ProgramRuntimeTest, RetryLeavesANonRetryableCoreFailureToTheClassifier) {
    classified_failure_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "retry"}, {"max_attempts", 3}, {"body", json{{"op", "call_core"}}}},
            "runtime-classified-logic"),
        json::object(), "trace-retry-logic");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(classified_failure_calls.load(), 1U);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.LogicError");
    EXPECT_EQ(result.failure()->witness["node_failure"]["routable"], json(false));

    classified_failure_calls.store(0);
    const auto ordinary = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "retry"}, {"max_attempts", 3}, {"body", json{{"op", "call_core"}}}},
            "runtime-classified-ordinary"),
        json::object(), "trace-retry-ordinary");
    EXPECT_EQ(ordinary.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(classified_failure_calls.load(), 3U);
    EXPECT_EQ(ordinary.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(ordinary.failure()->attempts, 3U);
}

namespace {

// program_document with a graph-level Core retry policy.
json retrying_program_document(std::string node_type, int max_retries) {
    auto document = program_document(std::move(node_type));
    document["root"]["definition"]["retry_policy"] =
        json{{"max_retries", max_retries}, {"initial_delay_ms", 1}, {"backoff_multiplier", 1.0},
             {"max_delay_ms", 1}};
    return document;
}

}  // namespace

// #364: a Program diagnostic raised inside a node is a configuration verdict
// (NG.ConfigError): the node retry loop does not run it again.
TEST(ProgramRuntimeTest, AProgramDiagnosticInsideANodeIsNeverRetried) {
    classified_failure_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit_document(retrying_program_document("runtime-classified-diagnostic", 2));
    const auto result  = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-diagnostic", {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(classified_failure_calls.load(), 1U);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.ConfigError");
    EXPECT_EQ(result.failure()->witness["node_failure"]["kind"], json("non_retryable"));
}

// #364: RunBudget::max_node_retries caps node retries across the run, is
// reported as ProgramUsage::node_retries, and both are serialized only when set.
TEST(ProgramRuntimeTest, NodeRetryCeilingIsARunBudgetField) {
    classified_failure_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit_document(retrying_program_document("runtime-classified-ordinary", 3));
    auto       budget  = grant();
    budget.max_node_retries = 1;
    const auto result = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), budget, "trace-ceiling", {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(classified_failure_calls.load(), 2U);
    EXPECT_EQ(result.usage().node_retries, 1U);
    EXPECT_EQ(result.remaining_budget().max_node_retries, std::optional<std::uint64_t>(0));
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->witness["node_failure"]["kind"], json("ceiling"));
    const auto bytes = result.serialize_canonical();
    EXPECT_NE(bytes.find("\"node_retries\":1"), std::string::npos);
    EXPECT_NE(bytes.find("\"max_node_retries\":0"), std::string::npos);
    EXPECT_EQ(ProgramResult::parse(bytes).id(), result.id());
    EXPECT_EQ(ProgramResult::parse(bytes).remaining_budget(), result.remaining_budget());

    // Without a ceiling nothing is counted and nothing new is serialized.
    classified_failure_calls.store(0);
    const auto unbounded = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-no-ceiling", {}});
    EXPECT_EQ(classified_failure_calls.load(), 4U);
    EXPECT_EQ(unbounded.usage().node_retries, 0U);
    EXPECT_FALSE(unbounded.remaining_budget().max_node_retries);
    const auto plain = unbounded.serialize_canonical();
    EXPECT_EQ(plain.find("node_retries"), std::string::npos);
}

// #419: the retry ceiling's remainder is the ceiling less the retries taken,
// whether the Program is a typed plan or has a JavaScript main().
TEST(ProgramRuntimeTest, RemainingNodeRetriesCountTheRetriesTakenOfATypedPlan) {
    classified_failure_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit_document(retrying_program_document("runtime-classified-once", 2));
    auto       budget  = grant();
    budget.max_node_retries = 5;
    const auto result = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), budget, "trace-retries-plan", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
    EXPECT_EQ(classified_failure_calls.load(), 2U);
    EXPECT_EQ(result.usage().node_retries, 1U);
    EXPECT_EQ(result.remaining_budget().max_node_retries, std::optional<std::uint64_t>(4));
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, RemainingNodeRetriesCountTheRetriesTakenWithAJavaScriptMain) {
    classified_failure_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-classified-once"});
                graph.entry("work");
                graph.exit("work");
                graph.retryPolicy("work", {max_retries: 2, initial_delay_ms: 10,
                                           backoff_multiplier: 1, max_delay_ms: 10});
                return graph;
            }

            export function* main(input) {
                return yield ng.callCore("main", input, "run");
            }
        )JS");
    auto budget = grant();
    budget.max_node_retries = 5;
    const auto result = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), budget, "trace-retries-main", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
    EXPECT_EQ(classified_failure_calls.load(), 2U);
    EXPECT_EQ(result.usage().node_retries, 1U);
    EXPECT_EQ(result.remaining_budget().max_node_retries, std::optional<std::uint64_t>(4));
}
#endif

// #364 (WP12): a Core backoff that would cross the run's wall-time deadline
// stops retrying with kind "deadline" instead of sleeping until the budget
// ends the run, and the Program retry operation does not run the body again
// after such a stop.
TEST(ProgramRuntimeTest, ABackoffPastTheWallTimeDeadlineStopsWithKindDeadline) {
    classified_failure_calls.store(0);
    AdmittedRuntime fixture;
    auto document = orchestration_document(
        json{{"op", "retry"}, {"max_attempts", 3}, {"body", json{{"op", "call_core"}}}},
        "runtime-classified-ordinary");
    document["root"]["definition"]["retry_policy"] =
        json{{"max_retries", 3}, {"initial_delay_ms", 5000}, {"backoff_multiplier", 1.0},
             {"max_delay_ms", 5000}};
    const auto version = fixture.admit_document(std::move(document));
    const auto started = std::chrono::steady_clock::now();
    const auto result  = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), RunBudget{1500, 1000, 1000, 4, 32, 20, 0, 0, 0},
                          "trace-deadline-backoff", {}});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500)) << "the backoff slept toward the deadline";
    EXPECT_EQ(classified_failure_calls.load(), 1U);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.failure()->witness["node_failure"]["kind"], json("deadline"));
    EXPECT_EQ(result.failure()->witness["node_failure"]["attempts"], json(1));
}

// #364: a failure-exit node fails the Program with its declared code.
TEST(ProgramRuntimeTest, AFailureExitFailsTheProgramWithItsCode) {
    AdmittedRuntime fixture;
    auto            document = program_document("runtime-completed");
    auto            core     = document["root"]["definition"];
    core["nodes"]["ABORT"]   = json{{"type", "runtime-completed"}};
    core["edges"] = json::array({json{{"from", "__start__"}, {"to", "work"}},
                                 json{{"from", "work"}, {"to", "ABORT"}}});
    core["failure_exits"] = json{{"ABORT", json{{"code", "plan.user_abort"}}}};
    document["root"]["definition"] = std::move(core);
    const auto version = fixture.admit_document(std::move(document));
    const auto result  = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-exit", {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "plan.user_abort");
    EXPECT_EQ(result.failure()->core_node, "ABORT");
    EXPECT_EQ(result.failure()->witness["failure_exit"],
              (json{{"node", "ABORT"}, {"code", "plan.user_abort"}}));
}

TEST(ProgramRuntimeTest, DeepNestedCoreFailurePreservesOperationAndSourceCoordinate) {
    AdmittedRuntime fixture;
    const auto      nested_root = json{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "branch"},
                                            {"condition", json{{"path", "/route"}, {"equals", "then"}}},
                                            {"then", json{{"op", "retry"},
                                                          {"max_attempts", 1},
                                                          {"body", json{{"op", "call_core"}}}}}}})}};
    const auto result =
        run_orchestration(fixture, orchestration_document(nested_root, "runtime-failing"),
                          json{{"route", "then"}}, "trace-deep-nested-failure");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.failure()->operation_id, "root.0.0.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/children/0/then/body");
}

TEST(ProgramRuntimeTest, ParallelJoinsAllBranchesInPlanOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const json      first{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "return"}, {"value", json{{"branch", 1}}}}})}};
    const json second{{"op", "return"}, {"value", json{{"branch", 2}}}};
    const auto result =
        run_orchestration(fixture,
                          orchestration_document(
                              json{{"op", "parallel"}, {"branches", json::array({first, second})}}),
                          json::object(), "trace-parallel");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"branch", 1}}, json{{"branch", 2}}}));
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ParallelReportsTheBranchFailureNotTheSiblingItCancelled) {
    // #379, the plan-level sibling of the JavaScript join: the branch that
    // fails cancels the other one, whose cancellation never replaces the
    // failure.
    map_fail_first_calls.store(0);
    map_fail_first_active.store(0);
    AdmittedRuntime fixture(2);
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "parallel"},
                                    {"branches", json::array({json{{"op", "call_core"}},
                                                              json{{"op", "call_core"}}})}},
                               "runtime-map-fail-first"),
        json::object(), "trace-parallel-failure");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed") << result.failure()->message;
    EXPECT_NE(result.failure()->message.find("map fail-fast sentinel"), std::string::npos)
        << result.failure()->message;
    EXPECT_EQ(map_fail_first_calls.load(), 2U);
    EXPECT_EQ(map_fail_first_active.load(), 0U);
}

TEST(ProgramRuntimeTest, ParallelImmediateBranchesRetainTheirCompletionSignal) {
    AdmittedRuntime fixture(2);
    const json      unreachable_core{{"op", "branch"},
                                     {"condition", json{{"path", "/unused"}, {"exists", true}}},
                                     {"then", json{{"op", "call_core"}}},
                                     {"else", json{{"op", "return"}, {"value", 3}}}};
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{
                 {"op", "parallel"},
                 {"branches", json::array({json{{"op", "return"}, {"value", 1}},
                                           json{{"op", "return"}, {"value", 2}}, unreachable_core})}}),
        json::object(), "trace-parallel-immediate");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({1, 2, 3}));
}

// ---------------------------------------------------------------------------
// A branch's interrupt pauses a plan-level parallel (#384)
// ---------------------------------------------------------------------------

namespace {
ProgramResult run_parallel_plan(AdmittedRuntime& fixture, json input) {
    const auto version = fixture.admit_document(orchestration_document(
        json{{"op", "parallel"},
             {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}},
        "runtime-parallel-branch"));
    return fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{std::move(input), RunBudget{10000, 1000, 1000, 2, 32, 20, 0, 0, 0},
                          "trace-plan-parallel-pause", {}});
}

ProgramResult answer(AdmittedRuntime& fixture, const ProgramResult& interrupted,
                     const std::string& decision) {
    return fixture.runtime
        ->resume("tenant:runtime", interrupted.run_id(),
                 resume_for(interrupted, json{{"decision", decision}},
                            "trace-plan-parallel-answer"))
        .wait();
}

json branch_value(const ProgramResult& result, std::size_t index) {
    return result.output()[index]["channels"]["value"]["value"];
}

CheckpointPhase paused_phase(AdmittedRuntime& fixture, const ProgramResult& result) {
    const auto stored = fixture.checkpoints->load_by_id(result.checkpoint()->checkpoint_id);
    return stored ? stored->interrupt_phase : CheckpointPhase::Completed;
}
}  // namespace

TEST(ProgramRuntimeTest, PlanParallelPausedByBothBranchesAsksEachOfThemInTurn) {
    // #384: both branches ask. Resume answers the first; the second asks its
    // own question again instead of running on the first one's answer, and
    // resume then answers it. Nothing runs twice. In memory and on SQLite.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        reset_parallel_branches();
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);

        const auto first = run_parallel_plan(fixture, json{{"value", "ask"}});
        ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_detail(first);
        ASSERT_TRUE(first.interrupt() && first.interrupt()->pending_input);
        EXPECT_EQ(first.interrupt()->pending_input->operation_id(), "root.0");
        ASSERT_TRUE(first.checkpoint().has_value());
        EXPECT_EQ(paused_phase(fixture, first), CheckpointPhase::NodeInterrupt);
        EXPECT_EQ(parallel_branch_asked.load(), 2U);

        const auto second = answer(fixture, first, "first");
        ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted) << failure_detail(second);
        ASSERT_TRUE(second.interrupt() && second.interrupt()->pending_input);
        EXPECT_EQ(second.interrupt()->pending_input->operation_id(), "root.1")
            << "the other paused branch asks its own question";
        EXPECT_EQ(second.interrupt()->core_node, "work");
        ASSERT_TRUE(second.checkpoint().has_value());
        EXPECT_EQ(paused_phase(fixture, second), CheckpointPhase::NodeInterrupt);
        EXPECT_EQ(parallel_branch_answered.load(), 1U);
        EXPECT_EQ(parallel_branch_asked.load(), 2U) << "a paused branch is not run again to ask";

        const auto done = answer(fixture, second, "second");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(branch_value(done, 0), json("first"));
        EXPECT_EQ(branch_value(done, 1), json("second"));
        EXPECT_EQ(parallel_branch_answered.load(), 2U) << "the answered branch is replayed";
        EXPECT_EQ(parallel_branch_asked.load(), 2U);
    }
}

TEST(ProgramRuntimeTest, PlanParallelReplaysTheBranchThatFinishedBeforeThePause) {
    // #384: one branch finishes and the other asks. Resume answers the paused
    // branch and replays the finished one from its record: it does not run,
    // and is not charged, again.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        reset_parallel_branches();
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);

        const auto paused = run_parallel_plan(fixture, json{{"value", "one"}});
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_EQ(paused_phase(fixture, paused), CheckpointPhase::NodeInterrupt)
            << "the run resumes the paused branch, whichever branch wrote last";
        const std::size_t finished_index =
            paused.interrupt()->pending_input->operation_id() == "root.0" ? 1 : 0;
        EXPECT_TRUE(paused.output()[1 - finished_index].is_null());
        EXPECT_EQ(paused.output()[finished_index]["channels"]["value"]["value"], json("done"));

        const auto done = answer(fixture, paused, "approved");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(branch_value(done, finished_index), json("done"));
        EXPECT_EQ(branch_value(done, 1 - finished_index), json("approved"));
        EXPECT_EQ(parallel_branch_finished.load(), 1U) << "the finished branch is replayed";
        EXPECT_EQ(parallel_branch_asked.load(), 1U);
        EXPECT_EQ(parallel_branch_answered.load(), 1U);
        EXPECT_EQ(done.usage().core_steps, 1U) << "only the answered branch ran a Core step";
    }
}

TEST(ProgramRuntimeTest, NestedPlanParallelsAskEveryPausedCallInTurnAcrossRestarts) {
    // #384: root.0 and the two branches of the nested parallel root.1 all ask.
    // Each resume answers the call it names and the others ask again, in
    // declaration order; the record of each paused or finished branch survives
    // the next pause and a restart of the runtime over the same stores.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        reset_parallel_branches();
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(3, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        const json nested{
            {"op", "parallel"},
            {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}};
        const auto version = fixture.admit_document(orchestration_document(
            json{{"op", "parallel"}, {"branches", json::array({json{{"op", "call_core"}}, nested})}},
            "runtime-parallel-branch"));
        auto result = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json{{"value", "ask"}}, RunBudget{10000, 1000, 1000, 3, 32, 20, 0, 0, 0},
                              "trace-nested-plan-parallel-pause", {}});
        EXPECT_EQ(parallel_branch_asked.load(), 3U);

        for (const auto* named : {"root.0", "root.1.0", "root.1.1"}) {
            SCOPED_TRACE(named);
            ASSERT_EQ(result.status(), ProgramTerminalStatus::Interrupted) << failure_detail(result);
            ASSERT_TRUE(result.interrupt() && result.interrupt()->pending_input);
            EXPECT_EQ(result.interrupt()->pending_input->operation_id(), named);
            EXPECT_EQ(paused_phase(fixture, result), CheckpointPhase::NodeInterrupt);
            fixture.recreate_catalog_and_runtime();
            (void)fixture.admit_document(orchestration_document(
                json{{"op", "parallel"},
                     {"branches", json::array({json{{"op", "call_core"}}, nested})}},
                "runtime-parallel-branch"));
            result = answer(fixture, result, named);
        }
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_detail(result);
        EXPECT_EQ(branch_value(result, 0), json("root.0"));
        EXPECT_EQ(result.output()[1][0]["channels"]["value"]["value"], json("root.1.0"));
        EXPECT_EQ(result.output()[1][1]["channels"]["value"]["value"], json("root.1.1"));
        EXPECT_EQ(parallel_branch_asked.load(), 3U) << "a paused call is never run again to ask";
        EXPECT_EQ(parallel_branch_answered.load(), 3U) << "an answered call is replayed";
    }
}

// ---------------------------------------------------------------------------
// A plan resume replays what the paused run finished (#386)
// ---------------------------------------------------------------------------

namespace {
json plan_call() { return json{{"op", "call_core"}}; }
json plan_return(json value) { return json{{"op", "return"}, {"value", std::move(value)}}; }
json plan_sequence(json children) {
    return json{{"op", "sequence"}, {"children", std::move(children)}};
}
json plan_parallel(json branches) {
    return json{{"op", "parallel"}, {"branches", std::move(branches)}};
}
json plan_quorum(json branches, std::uint64_t min_success) {
    return json{{"op", "quorum"}, {"branches", std::move(branches)}, {"min_success", min_success}};
}
// #387: a quorum branch that fails without failing the run: an await whose
// call blocks until the await's timeout cancels it.
json plan_timed_out_call() {
    return json{{"op", "await"}, {"timeout_ms", 50}, {"body", plan_call()}};
}

RunBudget plan_budget() { return RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0}; }

ProgramVersion admit_plan(AdmittedRuntime& fixture, json root) {
    return fixture.admit_document(orchestration_document(std::move(root), "runtime-plan-step"));
}

ProgramResult run_plan(AdmittedRuntime& fixture, const ProgramVersion& version,
                       const std::string& run_id, json input = json{{"value", "start"}}) {
    ProgramInvocation invocation{std::move(input), plan_budget(), "trace-plan-resume-" + run_id,
                                 {}};
    invocation.requested_run_id = run_id;
    return fixture.runtime->run("tenant:runtime", version, std::move(invocation));
}

ProgramResult decide(AdmittedRuntime& fixture, const ProgramResult& interrupted,
                     const std::string& decision) {
    return fixture.runtime
        ->resume("tenant:runtime", interrupted.run_id(),
                 resume_for(interrupted, json{{"decision", decision}}, "trace-plan-decide"))
        .wait();
}

std::string paused_operation(const ProgramResult& result) {
    if (!result.interrupt()) return {};
    if (result.interrupt()->pending_input) return result.interrupt()->pending_input->operation_id();
    if (result.interrupt()->pending_effect) return result.interrupt()->pending_effect->operation_id();
    return {};
}

std::string plan_root_thread(const ProgramVersion& version, const std::string& run_id) {
    return program_root_core_thread_id(
        run_id, version.core_materialization_receipt().plans.front().compiled_plan_identity);
}

json channel_value(const json& output) { return output["channels"]["value"]["value"]; }
}  // namespace

TEST(ProgramRuntimeTest, TwoCoreInterruptsOfOneStepAreAnsweredOneAtATime) {
    // #388: left and right ask in the same Core super-step. The run pauses on
    // left, and its pending input names left's Core interrupt, because the
    // step paused on two. Resume answers left only: right asks its own
    // question again, with a pending input that names no Core interrupt, as
    // a step that paused on one keeps its bytes. In memory and on SQLite.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        {
            std::lock_guard lock(named_ask_mutex);
            named_ask_runs.clear();
        }
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        auto document   = program_document("runtime-completed");
        auto definition = document["root"]["definition"];
        definition["channels"]["left_answer"]  = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
        definition["channels"]["right_answer"] = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
        definition["nodes"]["left"]  = json{{"type", "runtime-named-ask"}};
        definition["nodes"]["right"] = json{{"type", "runtime-named-ask"}};
        definition["edges"] = json::array({json{{"from", "__start__"}, {"to", "work"}},
                                           json{{"from", "work"}, {"to", "left"}},
                                           json{{"from", "work"}, {"to", "right"}},
                                           json{{"from", "left"}, {"to", "__end__"}},
                                           json{{"from", "right"}, {"to", "__end__"}}});
        document["root"]["definition"] = std::move(definition);
        const auto version = fixture.admit_document(std::move(document));

        const auto first = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-two-core-interrupts", {}});
        ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_detail(first);
        ASSERT_TRUE(first.interrupt() && first.interrupt()->pending_input);
        EXPECT_EQ(first.interrupt()->core_node, "left");
        EXPECT_EQ(first.interrupt()->pending_input->core_interrupt_id(), "s1:left");
        EXPECT_NE(first.interrupt()->pending_input->serialize_canonical().find(
                      "\"storage_schema_version\":2"),
                  std::string::npos);

        const auto second = fixture.runtime
                                ->resume("tenant:runtime", first.run_id(),
                                         resume_for(first, json{{"decision", "L"}},
                                                    "trace-two-core-interrupts-left"))
                                .wait();
        ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted) << failure_detail(second);
        ASSERT_TRUE(second.interrupt() && second.interrupt()->pending_input);
        EXPECT_EQ(second.interrupt()->core_node, "right") << "right asks its own question again";
        EXPECT_EQ(second.interrupt()->pending_input->core_interrupt_id(), "");
        EXPECT_NE(second.interrupt()->pending_input->serialize_canonical().find(
                      "\"storage_schema_version\":1"),
                  std::string::npos)
            << "one pending Core interrupt keeps the pending value's bytes";

        const auto done = fixture.runtime
                              ->resume("tenant:runtime", second.run_id(),
                                       resume_for(second, json{{"decision", "R"}},
                                                  "trace-two-core-interrupts-right"))
                              .wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(done.output()["channels"]["left_answer"]["value"], json("L"));
        EXPECT_EQ(done.output()["channels"]["right_answer"]["value"], json("R"))
            << "right gets its own answer, not left's";
        EXPECT_EQ(named_ask_runs_of("left"), 2U);
        EXPECT_EQ(named_ask_runs_of("right"), 3U);
    }
}

TEST(ProgramRuntimeTest, AnAnswerToOneCoreStepDoesNotAnswerALaterOne) {
    // #393: first asks, then second asks in the next Core super-step. The
    // answer to first used to stay visible to second, which took it as its
    // own and never asked. Now second asks, and gets its own answer. In
    // memory and on SQLite.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        {
            std::lock_guard lock(named_ask_mutex);
            named_ask_runs.clear();
        }
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        auto document   = program_document("runtime-completed");
        auto definition = document["root"]["definition"];
        definition["channels"]["first_answer"]  = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
        definition["channels"]["second_answer"] = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
        definition["nodes"]["first"]  = json{{"type", "runtime-named-ask"}};
        definition["nodes"]["second"] = json{{"type", "runtime-named-ask"}};
        definition["edges"] = json::array({json{{"from", "__start__"}, {"to", "work"}},
                                           json{{"from", "work"}, {"to", "first"}},
                                           json{{"from", "first"}, {"to", "second"}},
                                           json{{"from", "second"}, {"to", "__end__"}}});
        document["root"]["definition"] = std::move(definition);
        const auto version = fixture.admit_document(std::move(document));

        const auto first = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-later-core-step", {}});
        ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_detail(first);
        ASSERT_TRUE(first.interrupt() && first.interrupt()->pending_input);
        EXPECT_EQ(first.interrupt()->core_node, "first");

        const auto second = fixture.runtime
                                ->resume("tenant:runtime", first.run_id(),
                                         resume_for(first, json{{"decision", "F"}},
                                                    "trace-later-core-step-first"))
                                .wait();
        ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted)
            << "second took first's answer: " << failure_detail(second);
        ASSERT_TRUE(second.interrupt() && second.interrupt()->pending_input);
        EXPECT_EQ(second.interrupt()->core_node, "second");
        EXPECT_EQ(second.interrupt()->value["value"], json({{"asks", "second"}}));

        const auto done = fixture.runtime
                              ->resume("tenant:runtime", second.run_id(),
                                       resume_for(second, json{{"decision", "S"}},
                                                  "trace-later-core-step-second"))
                              .wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(done.output()["channels"]["first_answer"]["value"], json("F"));
        EXPECT_EQ(done.output()["channels"]["second_answer"]["value"], json("S"));
        EXPECT_EQ(named_ask_runs_of("first"), 2U);
        EXPECT_EQ(named_ask_runs_of("second"), 2U);
    }
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, AJavaScriptCallCoreAnswerDoesNotAnswerALaterCoreStep) {
    // #393 through a JavaScript callCore command: the resume continues the
    // command's Core call with the answer, which reaches first only.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        {
            std::lock_guard lock(named_ask_mutex);
            named_ask_runs.clear();
        }
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        const auto      version = fixture.admit_javascript(R"JS(
export function define() {
  const graph = ng.graph("main");
  graph.channel("first_answer", {reducer: "runtime-overwrite", initial: ""});
  graph.channel("second_answer", {reducer: "runtime-overwrite", initial: ""});
  graph.node("first", {type: "runtime-named-ask"});
  graph.node("second", {type: "runtime-named-ask"});
  graph.entry("first");
  graph.edge("first", "second");
  graph.exit("second");
  return graph;
}

export function* main(input) {
  const output = yield ng.callCore("main", {}, "resume:two-core-steps");
  return {first: output.first_answer, second: output.second_answer};
}
)JS");

        const auto first = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), javascript_budget(1), "trace-js-later-step", {}});
        ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_detail(first);
        ASSERT_TRUE(first.interrupt() && first.interrupt()->pending_input);
        EXPECT_EQ(first.interrupt()->core_node, "first");

        const auto second = fixture.runtime
                                ->resume("tenant:runtime", first.run_id(),
                                         resume_for(first, json{{"decision", "F"}},
                                                    "trace-js-later-step-first"))
                                .wait();
        ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted)
            << "second took first's answer: " << failure_detail(second);
        ASSERT_TRUE(second.interrupt() && second.interrupt()->pending_input);
        EXPECT_EQ(second.interrupt()->core_node, "second");

        const auto done = fixture.runtime
                              ->resume("tenant:runtime", second.run_id(),
                                       resume_for(second, json{{"decision", "S"}},
                                                  "trace-js-later-step-second"))
                              .wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(done.output(), (json{{"first", "F"}, {"second", "S"}}));
        EXPECT_EQ(named_ask_runs_of("first"), 2U);
        EXPECT_EQ(named_ask_runs_of("second"), 2U);
    }
}
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

TEST(ProgramRuntimeTest, ACoreStepWithAFailureAndAnInterruptFailsWhateverTheOrder) {
    // #392: one Core super-step runs a node that asks next to one that fails.
    // The Program used to pause when the asking node came first and fail when
    // the failing one did. It now fails either way, with the failing node's
    // classified failure, and leaves no question pending. In memory and on
    // SQLite.
    for (const bool durable : join_test_store_kinds()) {
        for (const bool ask_first : {true, false}) {
            SCOPED_TRACE(std::string(durable ? "SQLite stores" : "in-memory stores") +
                         (ask_first ? ", asking node first" : ", failing node first"));
            JoinTestStores  stores(durable);
            AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {},
                                    ExecutionGuarantee::Unmanaged, true);
            auto document   = program_document("runtime-completed");
            auto definition = document["root"]["definition"];
            definition["channels"]["a_first_answer"]  = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
            definition["channels"]["b_second_answer"] = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
            definition["nodes"]["a_first"] =
                json{{"type", ask_first ? "runtime-named-ask" : "runtime-failing"}};
            definition["nodes"]["b_second"] =
                json{{"type", ask_first ? "runtime-failing" : "runtime-named-ask"}};
            definition["edges"] = json::array({json{{"from", "__start__"}, {"to", "work"}},
                                               json{{"from", "work"}, {"to", "a_first"}},
                                               json{{"from", "work"}, {"to", "b_second"}},
                                               json{{"from", "a_first"}, {"to", "__end__"}},
                                               json{{"from", "b_second"}, {"to", "__end__"}}});
            document["root"]["definition"] = std::move(definition);
            const auto version = fixture.admit_document(std::move(document));

            const auto result = fixture.runtime->run(
                "tenant:runtime", version,
                ProgramInvocation{json::object(), grant(), "trace-step-failure-and-interrupt", {}});
            ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed)
                << "the step paused on the interrupt and dropped the failure: "
                << failure_detail(result);
            ASSERT_TRUE(result.failure());
            EXPECT_EQ(result.failure()->core_node, ask_first ? "b_second" : "a_first");
            EXPECT_FALSE(result.interrupt().has_value());
        }
    }
}

TEST(ProgramRuntimeTest, AForkOfTwoCoreInterruptsAnswersTheOneItsPendingInputNames) {
    // #394: left and right ask in one Core super-step and the run pauses with
    // a pending input that names left's Core interrupt. A fork of that run
    // answers exactly the interrupt the source's pending input names, as a
    // resume does: left gets the fork's answer and right asks its own
    // question again, in the fork. In memory and on SQLite.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        {
            std::lock_guard lock(named_ask_mutex);
            named_ask_runs.clear();
        }
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        auto document   = program_document("runtime-completed");
        auto definition = document["root"]["definition"];
        definition["channels"]["left_answer"]  = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
        definition["channels"]["right_answer"] = json{{"reducer", "runtime-overwrite"}, {"initial", ""}};
        definition["nodes"]["left"]  = json{{"type", "runtime-named-ask"}};
        definition["nodes"]["right"] = json{{"type", "runtime-named-ask"}};
        definition["edges"] = json::array({json{{"from", "__start__"}, {"to", "work"}},
                                           json{{"from", "work"}, {"to", "left"}},
                                           json{{"from", "work"}, {"to", "right"}},
                                           json{{"from", "left"}, {"to", "__end__"}},
                                           json{{"from", "right"}, {"to", "__end__"}}});
        document["root"]["definition"] = std::move(definition);
        const auto version = fixture.admit_document(std::move(document));

        const auto source = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-fork-two-interrupts", {}});
        ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted) << failure_detail(source);
        ASSERT_TRUE(source.checkpoint() && source.interrupt() && source.interrupt()->pending_input);
        ASSERT_EQ(source.interrupt()->pending_input->core_interrupt_id(), "s1:left");

        RunInvocation invocation;
        invocation.owner_scope        = "tenant:runtime";
        invocation.agent_id           = "test-fork-two-interrupts";
        invocation.program_version_id = version.id();
        invocation.run_id             = durable ? "fork-two-interrupts-sqlite" : "fork-two-interrupts";
        invocation.budget             = neograph::test::fork_share(source.remaining_budget());
        invocation.input              = json::object();
        invocation.message_sequence   = 1;
        invocation.idempotency_key    = "test-fork-two-interrupts:1";
        invocation.correlation_id     = "trace-fork-two-interrupts-target";
        invocation.validate();

        // Read the fork's run record while its first attempt runs left with
        // the answer.
        struct AnsweredRecord {
            std::string pending_input;
            std::string checkpoint;
            std::string attempt_resume;
        };
        std::optional<AnsweredRecord> answered_record;
        set_named_ask_answered_hook([&](const std::string& name) {
            if (name != "left" || answered_record) return;
            const auto record = fixture.journal->load("tenant:runtime", invocation.run_id);
            if (!record) return;
            AnsweredRecord seen;
            if (const auto pending = record->pending_input())
                seen.pending_input = pending->core_interrupt_id();
            if (record->exact_checkpoint()) seen.checkpoint = record->exact_checkpoint()->checkpoint_id;
            if (record->attempt_resume()) seen.attempt_resume = record->attempt_resume()->checkpoint_id;
            answered_record = std::move(seen);
        });
        const auto forked =
            fixture.runtime
                ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                       source.checkpoint()->checkpoint_id},
                       invocation,
                       ProgramResume{json{{"decision", "L"}}, "trace-fork-two-interrupts-resume",
                                     {}, pending_call_id(source)})
                .wait();
        set_named_ask_answered_hook(nullptr);
        ASSERT_EQ(forked.status(), ProgramTerminalStatus::Interrupted) << failure_detail(forked);
        ASSERT_TRUE(forked.interrupt() && forked.interrupt()->pending_input);
        EXPECT_EQ(forked.interrupt()->core_node, "right") << "right asks its own question again";

        // What the fork's first attempt resumed with, read while it ran: the
        // cloned checkpoint and the source's pending input, which names
        // left's Core interrupt. The attempt answers the interrupt that
        // pending input names, as an attempt that resumes the source does.
        ASSERT_TRUE(answered_record.has_value()) << "left was never answered in the fork";
        EXPECT_EQ(answered_record->pending_input, "s1:left");
        EXPECT_FALSE(answered_record->checkpoint.empty());
        EXPECT_NE(answered_record->checkpoint, source.checkpoint()->checkpoint_id)
            << "the fork resumes its own copy of the checkpoint";
        EXPECT_EQ(answered_record->attempt_resume, answered_record->checkpoint);

        const auto done = fixture.runtime
                              ->resume("tenant:runtime", forked.run_id(),
                                       resume_for(forked, json{{"decision", "R"}},
                                                  "trace-fork-two-interrupts-right"))
                              .wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(done.output()["channels"]["left_answer"]["value"], json("L"));
        EXPECT_EQ(done.output()["channels"]["right_answer"]["value"], json("R"));
    }
}

TEST(ProgramRuntimeTest, TwoInterruptBeforeGatesOfOneCoreStepAreAnsweredOneAtATime) {
    // #396: a and b are ready in one Core super-step and both have
    // interruptBefore. The run pauses before a. One resume used to pass both
    // gates; it now answers a's, and the run pauses before b, with its own
    // pending input, until the next resume answers it. In memory and on
    // SQLite.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(1, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        auto document   = program_document("runtime-completed");
        auto definition = document["root"]["definition"];
        definition["nodes"]["a"] = json{{"type", "runtime-followup"}};
        definition["nodes"]["b"] = json{{"type", "runtime-followup"}};
        definition["edges"] = json::array({json{{"from", "__start__"}, {"to", "work"}},
                                           json{{"from", "work"}, {"to", "a"}},
                                           json{{"from", "work"}, {"to", "b"}},
                                           json{{"from", "a"}, {"to", "__end__"}},
                                           json{{"from", "b"}, {"to", "__end__"}}});
        definition["interrupt_before"] = json::array({"a", "b"});
        document["root"]["definition"] = std::move(definition);
        const auto version = fixture.admit_document(std::move(document));

        const auto first = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-two-gates", {}});
        ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_detail(first);
        ASSERT_TRUE(first.interrupt() && first.interrupt()->pending_input);
        EXPECT_EQ(first.interrupt()->core_node, "a");

        const auto second =
            fixture.runtime
                ->resume("tenant:runtime", first.run_id(),
                         resume_for(first, json{{"approved", "a"}}, "trace-two-gates-a"))
                .wait();
        ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted)
            << "one resume passed both gates: " << failure_detail(second);
        ASSERT_TRUE(second.interrupt() && second.interrupt()->pending_input);
        EXPECT_EQ(second.interrupt()->core_node, "b");
        EXPECT_NE(pending_call_id(second), pending_call_id(first));

        const auto done =
            fixture.runtime
                ->resume("tenant:runtime", second.run_id(),
                         resume_for(second, json{{"approved", "b"}}, "trace-two-gates-b"))
                .wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    }
}

// ---------------------------------------------------------------------------
// #397: a Program join ranks a reconciliation pause above a member's failure
// ---------------------------------------------------------------------------

namespace {
bool asks_to_reconcile(const ProgramResult& result) {
    return result.interrupt() && result.interrupt()->value.is_object() &&
           result.interrupt()->value.contains("value") &&
           result.interrupt()->value["value"].value("kind", std::string()) ==
               "tool_effect_reconciliation";
}
}  // namespace

TEST(ProgramRuntimeTest, PlanParallelPausesForReconciliationBesideAFailedBranch) {
    // #397: one branch's Core call leaves an effect uncertain and the other
    // fails. The parallel used to fail, abandoning the effect; it now pauses
    // for reconciliation, as a Core step does (#392), whichever branch comes
    // first, and records the failed branch, which fails the run on resume
    // without running again. In memory and on SQLite.
    for (const bool durable : join_test_store_kinds()) {
        for (const bool reconcile_first : {true, false}) {
            SCOPED_TRACE(std::string(durable ? "SQLite stores" : "in-memory stores") +
                         (reconcile_first ? ", reconciling branch first" : ", failing branch first"));
            JoinTestStores  stores(durable);
            AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                    ExecutionGuarantee::Unmanaged, true);
            const auto version = admit_plan(fixture, plan_parallel(json::array({plan_call(), plan_call()})));
            const std::string run_id = std::string("plan-parallel-reconcile-") +
                                       (durable ? "sqlite-" : "memory-") +
                                       (reconcile_first ? "first" : "second");
            // Branch 0 runs on the parallel's own Core thread, the root's.
            const auto root_thread = plan_root_thread(version, run_id);
            reset_plan_steps([root_thread, reconcile_first](std::size_t, const NodeInput& input) -> json {
                const bool first = input.ctx.thread_id == root_thread;
                return first == reconcile_first
                           ? json{{"reconcile", true}}
                           : json{{"fail", "branch failure sentinel"}, {"delay_ms", 300}};
            });

            const auto paused = run_plan(fixture, version, run_id);
            ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted)
                << "the failure abandoned the uncertain effect: " << failure_detail(paused);
            EXPECT_TRUE(asks_to_reconcile(paused));
            EXPECT_EQ(paused_operation(paused), reconcile_first ? "root.0" : "root.1");

            const auto done = decide(fixture, paused, "reconciled");
            ASSERT_EQ(done.status(), ProgramTerminalStatus::Failed) << failure_detail(done);
            EXPECT_NE(done.failure()->message.find("branch failure sentinel"), std::string::npos)
                << failure_detail(done);
            EXPECT_EQ(plan_steps_counted(false), 2U) << "the failed branch ran once";
            EXPECT_EQ(plan_steps_counted(true), 1U) << "the reconciled branch ran on resume";
        }
    }
}

TEST(ProgramRuntimeTest, PlanParallelPausesForReconciliationBeforeAnOrdinaryQuestion) {
    // #401: with no failure, a parallel whose branches pause on an ordinary
    // question and on an uncertain effect asks for reconciliation first,
    // whatever the branches' order, as a Core step (#392) and a JavaScript
    // join do; the ordinary question is asked again once it is answered.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        const auto version =
            admit_plan(fixture, plan_parallel(json::array({plan_call(), plan_call()})));
        const std::string run_id =
            std::string("plan-parallel-reconcile-ordinary-") + (durable ? "sqlite" : "memory");
        const auto root_thread = plan_root_thread(version, run_id);
        reset_plan_steps([root_thread](std::size_t, const NodeInput& input) -> json {
            if (input.ctx.thread_id == root_thread) return json{{"ask", true}};
            return json{{"reconcile", true}, {"delay_ms", 100}};
        });

        const auto paused = run_plan(fixture, version, run_id);
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_TRUE(asks_to_reconcile(paused))
            << "the ordinary question outranked the uncertain effect";
        EXPECT_EQ(paused_operation(paused), "root.1");

        const auto asked = decide(fixture, paused, "reconciled");
        ASSERT_EQ(asked.status(), ProgramTerminalStatus::Interrupted) << failure_detail(asked);
        EXPECT_FALSE(asks_to_reconcile(asked));
        EXPECT_EQ(paused_operation(asked), "root.0");

        const auto done = decide(fixture, asked, "approved");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    }
}

TEST(ProgramRuntimeTest, PlanQuorumPausesForReconciliationAfterAFailedBranch) {
    // #397: the quorum's branches run in order; A fails without failing the
    // run (its await times out) and B leaves an effect uncertain. B pauses
    // the quorum, which still can reach two successes, and resume replays A's
    // failure, answers B and runs C.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
            if (ordinal == 1) return json{{"block", true}};
            if (ordinal == 2) return json{{"reconcile", true}};
            return json{{"write", "C"}};
        });
        const auto version = admit_plan(
            fixture,
            plan_quorum(json::array({plan_timed_out_call(), plan_call(), plan_call()}), 2));
        const auto paused = run_plan(fixture, version, durable ? "plan-quorum-reconcile-sqlite"
                                                               : "plan-quorum-reconcile-memory");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_TRUE(asks_to_reconcile(paused));
        EXPECT_EQ(paused_operation(paused), "root.1");

        const auto done = decide(fixture, paused, "B");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(plan_steps_counted(false), 3U) << "A's failure replays";
    }
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, JavaScriptJoinsPauseForReconciliationBesideAFailedMember) {
    // #397: in ng.all (fail-fast), ng.race and ng.quorum a member's Core call
    // leaves an effect uncertain while another member fails. Each join used to
    // end with the failure, abandoning the effect; each now pauses for
    // reconciliation, and records the failed member, which ends the join on
    // resume without running again.
    struct Case {
        const char* label;
        const char* body;
        const char* failure;
        unsigned    failed_runs;
    };
    const Case cases[] = {
        {"all, reconciling member first", R"JS(
    yield ng.all([
        ng.callCore("main", {value: "reconcile"}, "all:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "all:fail")
    ], {max_in_flight: 2}, "reconcile:all");
    return {unreachable: true};
)JS",
         "join member failure sentinel", 1},
        {"all, failing member first", R"JS(
    yield ng.all([
        ng.callCore("main", {value: "fail-slow"}, "all:fail"),
        ng.callCore("main", {value: "reconcile"}, "all:reconcile")
    ], {max_in_flight: 2}, "reconcile:all");
    return {unreachable: true};
)JS",
         "join member failure sentinel", 1},
        {"race", R"JS(
    yield ng.race([
        ng.callCore("main", {value: "reconcile"}, "race:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "race:fail")
    ], {max_in_flight: 2}, "reconcile:race");
    return {unreachable: true};
)JS",
         "join member failure sentinel", 1},
        {"quorum", R"JS(
    yield ng.quorum([
        ng.callCore("main", {value: "reconcile"}, "quorum:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "quorum:fail-a"),
        ng.callCore("main", {value: "fail-slow"}, "quorum:fail-b")
    ], {required_successes: 2, max_in_flight: 3}, "reconcile:quorum");
    return {unreachable: true};
)JS",
         "quorum", 2},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.label);
        reset_join_member_runs();
        AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      paused = run_join(fixture, item.body);
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted)
            << "the failure abandoned the uncertain effect: " << failure_detail(paused);
        EXPECT_TRUE(asks_to_reconcile(paused));

        const auto done = approve(fixture, paused);
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Failed) << failure_detail(done);
        EXPECT_NE(failure_detail(done).find(item.failure), std::string::npos) << failure_detail(done);
        EXPECT_EQ(join_member_runs_of("fail-slow"), item.failed_runs)
            << "a failed member replays from its record";
    }
}
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
namespace {
std::atomic<bool> answered_member_launched{false};

// Holds the launch of a replayed member that comes after the member the resume
// answers until that member has settled its effect and its completion has
// been handled: the launch order in which the replayed failure is registered
// last. A replayed member launched first is not held.
void hold_replay_until_the_answered_member_settles(std::size_t, bool replayed) {
    if (!replayed) {
        answered_member_launched.store(true);
        return;
    }
    if (!answered_member_launched.load()) return;
    for (int waited = 0; waited < 5000 && join_member_runs_of("reconcile:settled") == 0; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}
}  // namespace

TEST(ProgramRuntimeTest, AResumedJavaScriptJoinEndsWithItsReplayedFailureInEveryLaunchOrder) {
    // A join paused for reconciliation beside a member that failed (#397).
    // On resume the failure replays and the answered member settles its
    // effect first (#400); the failure finished before the pause, so it ends
    // the join, and wins a race. That must not depend on whether the replayed
    // member is registered before or after the answered member finishes: the
    // launch of a replay that comes after the answered member is held here
    // until that member has finished.
    struct Case {
        const char* label;
        const char* body;
        const char* failure;
        unsigned    failed_runs;
    };
    const Case cases[] = {
        {"race", R"JS(
    yield ng.race([
        ng.callCore("main", {value: "reconcile"}, "race:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "race:fail")
    ], {max_in_flight: 2}, "reconcile:race");
    return {unreachable: true};
)JS",
         "join member failure sentinel", 1},
        {"all", R"JS(
    yield ng.all([
        ng.callCore("main", {value: "reconcile"}, "all:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "all:fail")
    ], {max_in_flight: 2}, "reconcile:all");
    return {unreachable: true};
)JS",
         "join member failure sentinel", 1},
        {"quorum", R"JS(
    yield ng.quorum([
        ng.callCore("main", {value: "reconcile"}, "quorum:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "quorum:fail-a"),
        ng.callCore("main", {value: "fail-slow"}, "quorum:fail-b")
    ], {required_successes: 2, max_in_flight: 3}, "reconcile:quorum");
    return {unreachable: true};
)JS",
         "quorum", 2},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.label);
        reset_join_member_runs();
        answered_member_launched.store(false);
        AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      paused = run_join(fixture, item.body);
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        ASSERT_TRUE(asks_to_reconcile(paused));

        neograph::program::detail::set_join_member_launch_observer_for_testing(
            &hold_replay_until_the_answered_member_settles);
        struct ResetObserver {
            ~ResetObserver() {
                neograph::program::detail::set_join_member_launch_observer_for_testing(nullptr);
            }
        } reset_observer;
        const auto done = approve(fixture, paused);

        ASSERT_EQ(done.status(), ProgramTerminalStatus::Failed) << failure_detail(done);
        EXPECT_NE(failure_detail(done).find(item.failure), std::string::npos)
            << failure_detail(done);
        EXPECT_EQ(join_member_runs_of("fail-slow"), item.failed_runs) << "the failures replay";
        EXPECT_EQ(join_member_runs_of("reconcile:settled"), 1U)
            << "the answered member settles once";
    }
}
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, AForkOfAJavaScriptMainContinuesItsPausedCallCore) {
    // #395: a fork of a JavaScript main() paused in a callCore used to start
    // main() over: the commands that finished ran again, the paused call
    // asked its question again, and the fork's answer was lost. The fork now
    // continues like a resume of the source: it carries the source's command
    // journal, so the finished commands replay without running, and resumes
    // the paused call from its checkpoint with the fork's answer. For a
    // command and for a join member, in memory and on SQLite.
    struct Case {
        const char* label;
        const char* body;
        json        expected;
    };
    const Case cases[] = {
        {"a later command", R"JS(
    const first = yield ng.callCore("main", {value: "first"}, "fork:first");
    const second = yield ng.callCore("main", {value: "ask"}, "fork:second");
    return {first: first.value, second: second.value};
)JS",
         json{{"first", "done:first"}, {"second", "forked"}}},
        {"a join member", R"JS(
    const results = yield ng.all([
        ng.callCore("main", {value: "first"}, "fork:a"),
        ng.callCore("main", {value: "ask"}, "fork:b")
    ], {max_in_flight: 2}, "fork:all");
    return {first: results[0].value, second: results[1].value};
)JS",
         json{{"first", "done:first"}, {"second", "forked"}}},
    };
    for (const bool durable : join_test_store_kinds()) {
        for (const auto& item : cases) {
            SCOPED_TRACE(std::string(durable ? "SQLite stores, " : "in-memory stores, ") +
                         item.label);
            reset_join_member_runs();
            JoinTestStores  stores(durable);
            AdmittedRuntime fixture(3, stores.checkpoints, stores.transitions, {},
                                    ExecutionGuarantee::Unmanaged, true);
            const auto version =
                fixture.admit_javascript(javascript_runtime_source("runtime-join-member", item.body));
            const auto source = fixture.runtime->run(
                "tenant:runtime", version,
                ProgramInvocation{json{{"tag", "source input"}}, javascript_budget(3),
                                  "trace-js-fork-source", {}});
            ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted) << failure_detail(source);
            ASSERT_TRUE(source.checkpoint());
            EXPECT_EQ(join_member_runs_of("first"), 1U);
            EXPECT_EQ(join_member_runs_of("ask"), 1U);

            RunInvocation invocation;
            invocation.owner_scope        = "tenant:runtime";
            invocation.agent_id           = "test-js-fork";
            invocation.program_version_id = version.id();
            invocation.run_id             = std::string("js-fork-") + (durable ? "sqlite-" : "memory-") +
                                std::to_string(&item - cases);
            invocation.budget             = neograph::test::fork_share(source.remaining_budget());
            invocation.input              = json::object();
            invocation.message_sequence   = 1;
            invocation.idempotency_key    = "test-js-fork:" + invocation.run_id;
            invocation.correlation_id     = "trace-js-fork-target";
            invocation.validate();

            const auto forked =
                fixture.runtime
                    ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                           source.checkpoint()->checkpoint_id},
                           invocation,
                           ProgramResume{json{{"decision", "forked"}}, "trace-js-fork-resume", {},
                                         pending_call_id(source)})
                    .wait();
            ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed)
                << "the fork started main() over: " << failure_detail(forked);
            EXPECT_EQ(forked.output(), item.expected);
            EXPECT_EQ(join_member_runs_of("first"), 1U) << "the finished call replays";
            EXPECT_EQ(join_member_runs_of("ask"), 2U) << "the paused call resumes once, answered";
            const auto record = fixture.journal->load("tenant:runtime", forked.run_id());
            ASSERT_TRUE(record);
            EXPECT_EQ(record->invocation().input, (json{{"tag", "source input"}}))
                << "main() runs again with the source's input";
        }
    }
}
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, RecoveryOfAForkedJavaScriptAttemptResumesItsCallCoreWithTheForkValue) {
    // #395: the first attempt of a JavaScript fork stops inside the answered
    // node; a fresh runtime recovers it from the fork's run record, and the
    // recovered attempt resumes the paused callCore with the fork's value,
    // with main() run on the source's input.
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-resume-hook", R"JS(
    const reply = yield ng.callCore("main", {decision: input.decision}, "approval");
    return {value: reply.value, input: input.decision};
)JS"));
    const auto source = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"decision", "source input"}}, javascript_budget(1),
                          "trace-js-forked-crash-source", {}});
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted) << failure_detail(source);
    ASSERT_TRUE(source.checkpoint());

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-js-forked-crash";
    invocation.program_version_id = version.id();
    invocation.run_id             = "js-forked-crash-target";
    invocation.budget             = neograph::test::fork_share(source.remaining_budget());
    invocation.input              = json::object();
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "test-js-forked-crash:1";
    invocation.correlation_id     = "trace-js-forked-crash-target";
    invocation.validate();

    HeldResumedAttempt held;
    auto               original = fixture.runtime->fork(
        ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
        invocation,
        ProgramResume{json{{"decision", "forked"}}, "trace-js-forked-crash-resume", {},
                      pending_call_id(source)});
    ASSERT_TRUE(held.entered());

    auto       fresh     = fixture.make_runtime();
    const auto recovered = fresh->reconnect("tenant:runtime", "js-forked-crash-target").wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_detail(recovered);
    EXPECT_EQ(recovered.output(), (json{{"value", json{{"decision", "forked"}}.dump()},
                                        {"input", "source input"}}));
    held.release();
    (void)original.wait();
}
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

TEST(ProgramRuntimeTest, PlanQuorumCountsABranchNodeFailureAsAFailedBranch) {
    // #399: a branch whose Core node fails is a failed branch of the quorum,
    // which then ends by its own rule. It used to fail the whole run at once.
    // In memory and on SQLite.
    for (const bool durable : join_test_store_kinds()) {
        const std::string suffix = durable ? "-sqlite" : "-memory";
        {
            SCOPED_TRACE(std::string(durable ? "SQLite stores" : "in-memory stores") +
                         ", still reachable");
            JoinTestStores  stores(durable);
            AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                    ExecutionGuarantee::Unmanaged, true);
            reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
                if (ordinal == 1) return json{{"fail", "quorum branch node failure"}};
                return json{{"write", ordinal == 2 ? "B" : "C"}};
            });
            const auto version = admit_plan(
                fixture, plan_quorum(json::array({plan_call(), plan_call(), plan_call()}), 2));
            const auto done = run_plan(fixture, version, "plan-quorum-node-failure" + suffix);
            ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed)
                << "a branch's node failure failed the run: " << failure_detail(done);
            ASSERT_EQ(done.output().size(), 2U);
            EXPECT_EQ(channel_value(done.output()[0]), json("B"));
            EXPECT_EQ(channel_value(done.output()[1]), json("C"));
        }
        {
            SCOPED_TRACE(std::string(durable ? "SQLite stores" : "in-memory stores") +
                         ", out of reach");
            JoinTestStores  stores(durable);
            AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                    ExecutionGuarantee::Unmanaged, true);
            reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
                if (ordinal <= 2) return json{{"fail", "quorum branch node failure"}};
                return json{{"write", "C"}};
            });
            const auto version = admit_plan(
                fixture, plan_quorum(json::array({plan_call(), plan_call(), plan_call()}), 2));
            const auto failed = run_plan(fixture, version, "plan-quorum-node-unreachable" + suffix);
            ASSERT_EQ(failed.status(), ProgramTerminalStatus::Failed) << failure_detail(failed);
            EXPECT_EQ(failed.failure()->code, "P_QUORUM_UNREACHABLE") << failure_detail(failed);
            EXPECT_EQ(plan_steps_counted(false), 2U) << "the third branch never starts";
        }
        {
            SCOPED_TRACE(std::string(durable ? "SQLite stores" : "in-memory stores") +
                         ", paused after the failure");
            JoinTestStores  stores(durable);
            AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                    ExecutionGuarantee::Unmanaged, true);
            reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
                if (ordinal == 1) return json{{"fail", "quorum branch node failure"}};
                if (ordinal == 2) return json{{"ask", true}};
                return json{{"write", "C"}};
            });
            const auto version = admit_plan(
                fixture, plan_quorum(json::array({plan_call(), plan_call(), plan_call()}), 2));
            const auto paused = run_plan(fixture, version, "plan-quorum-node-paused" + suffix);
            ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
            EXPECT_EQ(paused_operation(paused), "root.1");
            const auto record = fixture.journal->load("tenant:runtime", paused.run_id());
            ASSERT_TRUE(record && record->settled_branches());
            EXPECT_EQ((*record->settled_branches())["root.0"]["kind"], json("settled"))
                << "the failed branch finished, and is recorded";

            const auto done = decide(fixture, paused, "B");
            ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
            EXPECT_EQ(plan_steps_counted(false), 3U) << "the failed branch replays";
        }
    }
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, ACollectingAllWaitsForItsPausedMembersAndPausesForReconciliationFirst) {
    // #401: a collecting ng.all is the deliberate exception to the rule of a
    // Core step (#392, #397): it waits for every member, paused ones
    // included, before it reports, so a member's failure does not end it
    // while another member is paused on an ordinary question. Within it, as
    // in every join, a member paused on an uncertain effect outranks a member
    // paused on an ordinary question: the join pauses on it first, whatever
    // the declaration order.
    {
        SCOPED_TRACE("an ordinary pause beside a failure");
        reset_join_member_runs();
        AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      paused = run_join(fixture, R"JS(
    return yield ng.all([
        ng.callCore("main", {value: "ask"}, "collect:ask"),
        ng.callCore("main", {value: "fail"}, "collect:fail")
    ], {max_in_flight: 2, collect: true}, "collect:all");
)JS");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted)
            << "the collecting join reported before its paused member settled: "
            << failure_detail(paused);
        EXPECT_FALSE(asks_to_reconcile(paused));

        const auto done = approve(fixture, paused);
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Failed) << failure_detail(done);
        EXPECT_NE(failure_detail(done).find("join member failure sentinel"), std::string::npos)
            << failure_detail(done);
        EXPECT_EQ(join_member_runs_of("ask"), 2U);
        EXPECT_EQ(join_member_runs_of("fail"), 1U) << "the failed member replays";
    }
    for (const bool with_failure : {false, true}) {
        SCOPED_TRACE(with_failure ? "an ordinary and an uncertain pause beside a failure"
                                  : "an ordinary and an uncertain pause");
        reset_join_member_runs();
        AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      paused = run_join(fixture, with_failure ? R"JS(
    return yield ng.all([
        ng.callCore("main", {value: "ask"}, "collect:ask"),
        ng.callCore("main", {value: "reconcile"}, "collect:reconcile"),
        ng.callCore("main", {value: "fail"}, "collect:fail")
    ], {max_in_flight: 3, collect: true}, "collect:all");
)JS"
                                                                : R"JS(
    return yield ng.all([
        ng.callCore("main", {value: "ask"}, "collect:ask"),
        ng.callCore("main", {value: "reconcile"}, "collect:reconcile")
    ], {max_in_flight: 2, collect: true}, "collect:all");
)JS");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_TRUE(asks_to_reconcile(paused))
            << "the ordinary question outranked the uncertain effect";

        // The reconciled member settles; the ordinary question is asked again.
        const auto asked = approve(fixture, paused);
        ASSERT_EQ(asked.status(), ProgramTerminalStatus::Interrupted) << failure_detail(asked);
        EXPECT_FALSE(asks_to_reconcile(asked));
        EXPECT_EQ(join_member_runs_of("reconcile:settled"), 1U);

        const auto done = approve(fixture, asked);
        if (with_failure) {
            ASSERT_EQ(done.status(), ProgramTerminalStatus::Failed) << failure_detail(done);
            EXPECT_NE(failure_detail(done).find("join member failure sentinel"),
                      std::string::npos)
                << failure_detail(done);
            EXPECT_EQ(join_member_runs_of("fail"), 1U) << "the failed member replays";
        } else {
            ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
            ASSERT_TRUE(done.output().is_array());
            ASSERT_EQ(done.output().size(), 2U);
            EXPECT_EQ(done.output()[0]["value"], "approved");
            EXPECT_EQ(done.output()[1]["value"], "approved");
        }
        EXPECT_EQ(join_member_runs_of("ask"), 3U);
        EXPECT_EQ(join_member_runs_of("reconcile"), 2U) << "the settled member replays";
        EXPECT_EQ(join_member_runs_of("reconcile:settled"), 1U);
    }
}

TEST(ProgramRuntimeTest, AFailFastJoinPausesForReconciliationBeforeAnOrdinaryQuestion) {
    // #401: the precedence of a Core step (#392) holds in every join: with no
    // failure, a member paused on an uncertain effect is asked before one
    // paused on an ordinary question, whatever the declaration order.
    reset_join_member_runs();
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      paused = run_join(fixture, R"JS(
    return yield ng.all([
        ng.callCore("main", {value: "ask"}, "all:ask"),
        ng.callCore("main", {value: "reconcile"}, "all:reconcile")
    ], {max_in_flight: 2}, "reconcile:all");
)JS");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
    EXPECT_TRUE(asks_to_reconcile(paused))
        << "the ordinary question outranked the uncertain effect";

    const auto asked = approve(fixture, paused);
    ASSERT_EQ(asked.status(), ProgramTerminalStatus::Interrupted) << failure_detail(asked);
    EXPECT_FALSE(asks_to_reconcile(asked));
    const auto done = approve(fixture, asked);
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    EXPECT_EQ(join_member_runs_of("reconcile:settled"), 1U);
}
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, AResumedJoinSettlesTheReconciledMemberBeforeTheReplayedFailureEndsIt) {
    // #400: a fail-fast ng.all, an ng.race and an ng.quorum pause for
    // reconciliation beside a failed member (#397). On resume the failed
    // member's record replays at once; it used to end the join then and
    // cancel the reconciled member before its node consumed the answer. The
    // answered member now settles its effect first, and only then does the
    // replayed failure end the join.
    struct Case {
        const char* label;
        const char* body;
    };
    const Case cases[] = {
        {"all", R"JS(
    yield ng.all([
        ng.callCore("main", {value: "reconcile"}, "all:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "all:fail")
    ], {max_in_flight: 2}, "settle:all");
    return {unreachable: true};
)JS"},
        {"race", R"JS(
    yield ng.race([
        ng.callCore("main", {value: "reconcile"}, "race:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "race:fail")
    ], {max_in_flight: 2}, "settle:race");
    return {unreachable: true};
)JS"},
        {"quorum", R"JS(
    yield ng.quorum([
        ng.callCore("main", {value: "reconcile"}, "quorum:reconcile"),
        ng.callCore("main", {value: "fail-slow"}, "quorum:fail-a"),
        ng.callCore("main", {value: "fail-slow"}, "quorum:fail-b")
    ], {required_successes: 2, max_in_flight: 3}, "settle:quorum");
    return {unreachable: true};
)JS"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.label);
        reset_join_member_runs();
        AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
        const auto      paused = run_join(fixture, item.body);
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);

        const auto done = approve(fixture, paused);
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Failed) << failure_detail(done);
        EXPECT_EQ(join_member_runs_of("reconcile:settled"), 1U)
            << "the reconciled member was cancelled before it consumed the answer";
        EXPECT_EQ(join_member_runs_of("reconcile"), 2U);
    }
}
#endif  // NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS

TEST(ProgramRuntimeTest, PlanQuorumPausedByABranchAsksAndResumeAnswersIt) {
    // #387: A finishes and B asks. The quorum needs two successes and B may
    // still be one, so the quorum pauses on B instead of counting it as a
    // failure and moving on to C. Resume answers B; A replays from its record
    // and is not run or charged again, and C never runs. In memory and SQLite.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
            if (ordinal == 1) return json{{"write", "A"}};
            if (ordinal == 2) return json{{"ask", true}};
            return json{{"write", "C"}};
        });
        const auto version =
            admit_plan(fixture, plan_quorum(json::array({plan_call(), plan_call(), plan_call()}), 2));

        const auto paused = run_plan(fixture, version, "plan-quorum-ask");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_EQ(paused_operation(paused), "root.1") << "the quorum pauses on B";
        EXPECT_EQ(plan_steps_counted(false), 2U) << "C does not start while B is paused";
        const auto record = fixture.journal->load("tenant:runtime", paused.run_id());
        ASSERT_TRUE(record && record->settled_branches());
        EXPECT_EQ((*record->settled_branches())["root.0"]["kind"], json("settled"));

        const auto done = decide(fixture, paused, "B");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        ASSERT_TRUE(done.output().is_array());
        ASSERT_EQ(done.output().size(), 2U);
        EXPECT_EQ(channel_value(done.output()[0]), json("A"));
        EXPECT_EQ(channel_value(done.output()[1]), json("B"));
        EXPECT_EQ(plan_steps_counted(false), 2U) << "A is replayed and C never runs";
        EXPECT_EQ(plan_steps_counted(true), 1U);
        EXPECT_EQ(done.usage().core_steps, 1U) << "only B's resumed step ran";
    }
}

TEST(ProgramRuntimeTest, PlanQuorumAsksEachPausedBranchInTurnAndReplaysWhatFinished) {
    // #387: three of three. A asks first; once answered, B asks; once B is
    // answered, C runs. Each resume answers one branch, and the branches that
    // finished replay from their records instead of running again.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
            if (ordinal <= 2) return json{{"ask", true}};
            return json{{"write", "C"}};
        });
        const auto version =
            admit_plan(fixture, plan_quorum(json::array({plan_call(), plan_call(), plan_call()}), 3));

        const auto first = run_plan(fixture, version, "plan-quorum-turns");
        ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_detail(first);
        EXPECT_EQ(paused_operation(first), "root.0");
        EXPECT_EQ(plan_steps_counted(false), 1U);

        const auto second = decide(fixture, first, "A");
        ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted) << failure_detail(second);
        EXPECT_EQ(paused_operation(second), "root.1") << "B asks its own question";
        EXPECT_EQ(plan_steps_counted(false), 2U);
        EXPECT_EQ(plan_steps_counted(true), 1U);

        const auto done = decide(fixture, second, "B");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        ASSERT_EQ(done.output().size(), 3U);
        EXPECT_EQ(channel_value(done.output()[0]), json("A"));
        EXPECT_EQ(channel_value(done.output()[1]), json("B"));
        EXPECT_EQ(channel_value(done.output()[2]), json("C"));
        EXPECT_EQ(plan_steps_counted(false), 3U) << "nothing ran twice";
        EXPECT_EQ(plan_steps_counted(true), 2U) << "A was answered once and replayed after";
    }
}

TEST(ProgramRuntimeTest, PlanQuorumReplaysAFailedBranchWithoutRunningItAgain) {
    // #387: A fails without failing the run (its await times out), B asks and
    // C would write. Two of three are still reachable with B, so the quorum
    // pauses; resume answers B, A's failure replays from its record, and C
    // runs.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
            if (ordinal == 1) return json{{"block", true}};
            if (ordinal == 2) return json{{"ask", true}};
            return json{{"write", "C"}};
        });
        const auto version = admit_plan(
            fixture, plan_quorum(json::array({plan_timed_out_call(), plan_call(), plan_call()}), 2));

        const auto paused = run_plan(fixture, version, "plan-quorum-failed");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_EQ(paused_operation(paused), "root.1");
        const auto record = fixture.journal->load("tenant:runtime", paused.run_id());
        ASSERT_TRUE(record && record->settled_branches());
        EXPECT_EQ((*record->settled_branches())["root.0"]["kind"], json("settled"))
            << "the failed branch finished, and is recorded";

        const auto done = decide(fixture, paused, "B");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        ASSERT_EQ(done.output().size(), 2U);
        EXPECT_EQ(channel_value(done.output()[0]), json("B"));
        EXPECT_EQ(channel_value(done.output()[1]), json("C"));
        EXPECT_EQ(plan_steps_counted(false), 3U) << "A's call ran once";
    }
}

TEST(ProgramRuntimeTest, PlanQuorumOutOfReachFailsInsteadOfCompleting) {
    // #387: once the remaining branches cannot reach the quorum, it fails with
    // P_QUORUM_UNREACHABLE, as a JavaScript quorum does. It returned a
    // Completed result holding fewer successes than it required.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
        if (ordinal == 1) return json{{"block", true}};
        return json{{"write", "B"}};
    });
    const auto version =
        admit_plan(fixture, plan_quorum(json::array({plan_timed_out_call(), plan_call()}), 2));
    const auto result = run_plan(fixture, version, "plan-quorum-unreachable");
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed) << failure_detail(result);
    EXPECT_EQ(result.failure()->code, "P_QUORUM_UNREACHABLE");
    EXPECT_EQ(plan_steps_counted(false), 1U) << "B cannot change the outcome and does not run";
}

TEST(ProgramRuntimeTest, PlanAwaitThatPausedPastItsDeadlineTimesOutOnResume) {
    // #391: a plan-level await keeps its deadline across a resume. The call it
    // awaits asks and is answered after the 800 ms passed: the resumed await
    // times out without dispatching the answered call. In memory and SQLite.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t, const NodeInput&) { return json{{"ask", true}}; });
        const auto version = admit_plan(
            fixture, json{{"op", "await"}, {"timeout_ms", 800}, {"body", plan_call()}});
        const auto paused = run_plan(fixture, version, "plan-await-late");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        const auto record = fixture.journal->load("tenant:runtime", paused.run_id());
        ASSERT_TRUE(record && record->settled_branches());
        EXPECT_TRUE((*record->settled_branches())["root"].contains("started_at_unix_ms"))
            << "the paused await records its start: " << record->settled_branches()->dump();

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const auto resumed = decide(fixture, paused, "approved");
        ASSERT_EQ(resumed.status(), ProgramTerminalStatus::TimedOut) << failure_detail(resumed);
        EXPECT_EQ(resumed.failure()->code, "P_AWAIT_TIMEOUT");
        EXPECT_EQ(plan_steps_counted(true), 0U) << "the answered call is not dispatched";
    }
}

TEST(ProgramRuntimeTest, PlanAwaitResumedWithinItsDeadlineCompletes) {
    // #391: a resume inside the deadline continues the awaited call.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    reset_plan_steps([](std::size_t, const NodeInput&) { return json{{"ask", true}}; });
    const auto version = admit_plan(
        fixture, json{{"op", "await"}, {"timeout_ms", 10000}, {"body", plan_call()}});
    const auto paused = run_plan(fixture, version, "plan-await-in-time");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
    const auto done = decide(fixture, paused, "approved");
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    EXPECT_EQ(channel_value(done.output()), json("approved"));
}

TEST(ProgramRuntimeTest, PlanResumeReplaysTheOperationsBeforeThePausedCallAndKeepsTheBranchArm) {
    // #386: A finishes, a branch on A's output selects a sequence, and B in it
    // asks. Resume answers B. A is replayed: it does not run, and is not
    // charged, again, and the branch sees A's output, not the resume value.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
            if (ordinal == 1) return json{{"write", "A1"}};
            if (ordinal == 2) return json{{"ask", true}};
            return json{{"write", "A" + std::to_string(ordinal)}};
        });
        const auto version = admit_plan(
            fixture,
            plan_sequence(json::array(
                {plan_call(),
                 json{{"op", "branch"},
                      {"condition", json{{"path", "/channels/value/value"}, {"equals", "A1"}}},
                      {"then", plan_sequence(json::array(
                                   {plan_call(), plan_return(json{{"arm", "then"}})}))},
                      {"else", plan_return(json{{"arm", "else"}})}}})));

        const auto paused = run_plan(fixture, version, "plan-prefix");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_EQ(paused_operation(paused), "root.1.0.0");
        EXPECT_EQ(paused.usage().program_operations, 5U);

        const auto done = decide(fixture, paused, "approved");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(done.output(), json({{"arm", "then"}}))
            << "the branch sees A's output, not the resume value";
        EXPECT_EQ(plan_steps_counted(false), 2U) << "A is replayed, not run again";
        EXPECT_EQ(plan_steps_counted(true), 1U);
        EXPECT_EQ(done.usage().core_steps, 1U) << "only B's resumed step ran";
        EXPECT_EQ(done.usage().program_operations, 1U) << "only the return after B is new";
    }
}

TEST(ProgramRuntimeTest, PlanResumeContinuesALoopAtItsPausedIteration) {
    // #386: iteration 0 of the loop runs X and Y; iteration 1 runs X and Y
    // asks. Resume answers Y with "stop" and the loop ends. Neither
    // iteration 0 nor X of iteration 1 runs again.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
            if (ordinal == 4) return json{{"ask", true}};
            return json{{"write", "step" + std::to_string(ordinal)}};
        });
        const auto version = admit_plan(
            fixture, json{{"op", "loop"},
                          {"condition", json{{"path", "/channels/value/value"},
                                             {"not_equals", "stop"}}},
                          {"max_iterations", 3},
                          {"body", plan_sequence(json::array({plan_call(), plan_call()}))}});

        const auto paused = run_plan(fixture, version, "plan-loop");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_EQ(paused_operation(paused), "root.0.1");

        const auto done = decide(fixture, paused, "stop");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        EXPECT_EQ(channel_value(done.output()), json("stop"));
        EXPECT_EQ(plan_steps_counted(false), 4U) << "no earlier call runs again";
        EXPECT_EQ(plan_steps_counted(true), 1U);
        EXPECT_EQ(done.usage().program_operations, 0U) << "the loop and its body continue";
    }
}

TEST(ProgramRuntimeTest, PlanResumeContinuesARetryAtItsPausedAttempt) {
    // #386: A finishes, then the retry's first attempt fails and its second
    // asks. Resume answers the second attempt; A and the failed attempt do not
    // run again.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
        if (ordinal == 2) return json{{"fail", "transient plan step failure"}};
        if (ordinal == 3) return json{{"ask", true}};
        return json{{"write", "A" + std::to_string(ordinal)}};
    });
    const auto version = admit_plan(
        fixture, plan_sequence(json::array(
                     {plan_call(), json{{"op", "retry"}, {"max_attempts", 3}, {"body", plan_call()}}})));

    const auto paused = run_plan(fixture, version, "plan-retry");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
    EXPECT_EQ(paused_operation(paused), "root.1.0");

    const auto done = decide(fixture, paused, "retried");
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    EXPECT_EQ(channel_value(done.output()), json("retried"));
    EXPECT_EQ(plan_steps_counted(false), 3U) << "A and the failed attempt are not run again";
    EXPECT_EQ(plan_steps_counted(true), 1U);
}

TEST(ProgramRuntimeTest, PlanResumeContinuesAMapAtItsPausedItem) {
    // #386: the map's second item asks. Resume answers it, keeps the first
    // item's result without running it again, then runs the third.
    for (const bool durable : join_test_store_kinds()) {
        SCOPED_TRACE(durable ? "SQLite stores" : "in-memory stores");
        JoinTestStores  stores(durable);
        AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                                ExecutionGuarantee::Unmanaged, true);
        reset_plan_steps([](std::size_t, const NodeInput& input) -> json {
            const auto item = input.state.get("value");
            if (item == json("i1")) return json{{"ask", true}};
            return json{{"write", item.get<std::string>() + "-done"}};
        });
        const auto version = admit_plan(
            fixture, plan_sequence(json::array(
                         {plan_call(),
                          json{{"op", "map"},
                               {"items", json::array({json{{"value", "i0"}}, json{{"value", "i1"}},
                                                      json{{"value", "i2"}}})},
                               {"body", plan_call()}}})));

        const auto paused = run_plan(fixture, version, "plan-map");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
        EXPECT_EQ(paused_operation(paused), "root.1.0");

        const auto done = decide(fixture, paused, "answered");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
        ASSERT_TRUE(done.output().is_array());
        ASSERT_EQ(done.output().size(), 3U);
        EXPECT_EQ(channel_value(done.output()[0]), json("i0-done"));
        EXPECT_EQ(channel_value(done.output()[1]), json("answered"));
        EXPECT_EQ(channel_value(done.output()[2]), json("i2-done"));
        EXPECT_EQ(plan_steps_counted(false), 4U) << "the first call and item 0 run once";
        EXPECT_EQ(plan_steps_counted(true), 1U);
    }
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE) && \
    defined(NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT)
TEST(ProgramRuntimeTest, PlanResumeAnswersThePausedCallWhoseCheckpointTiesAnEarlierCall) {
    // #386: A and then B run on the plan's root Core thread, and B asks. A's
    // last checkpoint and B's question are saved in the same millisecond, as
    // SQLite does on a host whose fsync returns at once, and A's has the
    // higher step. B's question, saved last, is the thread's latest; the run
    // pauses on it, and resume runs B with the answer instead of replaying
    // A's completed state.
    JoinTestStores stores(true);
    stores.checkpoints =
        std::make_shared<neograph::graph::test_support::CoarseClockStore<SqliteCheckpointStore>>(
            (stores.directory / "coarse-checkpoints.db").string());
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
        if (ordinal == 2) return json{{"ask", true}};
        return json{{"write", "A" + std::to_string(ordinal)}};
    });
    const auto version =
        admit_plan(fixture, plan_sequence(json::array({plan_call(), plan_call()})));

    const auto paused = run_plan(fixture, version, "plan-coarse-clock");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
    EXPECT_EQ(paused_operation(paused), "root.1");
    ASSERT_TRUE(paused.checkpoint().has_value());
    const auto latest =
        stores.checkpoints->load_latest(plan_root_thread(version, "plan-coarse-clock"));
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->id, paused.checkpoint()->checkpoint_id)
        << "the checkpoint saved last, B's question, is the thread's latest";
    EXPECT_EQ(latest->interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(latest->timestamp, 1);
    EXPECT_EQ(paused_phase(fixture, paused), CheckpointPhase::NodeInterrupt);

    const auto done = decide(fixture, paused, "approved");
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    EXPECT_EQ(channel_value(done.output()), json("approved"));
    EXPECT_EQ(plan_steps_counted(false), 2U);
    EXPECT_EQ(plan_steps_counted(true), 1U) << "B runs with the answer";
    EXPECT_EQ(done.usage().core_steps, 1U);
}
#endif

TEST(ProgramRuntimeTest, PlanResumeContinuesTheRaceBranchThatWon) {
    // #386: a race's first branch asks while the second blocks; the question
    // wins and the race pauses on it. Resume continues the winner alone: the
    // cancelled loser does not run again.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    const auto version = admit_plan(
        fixture, json{{"op", "race"}, {"branches", json::array({plan_call(), plan_call()})}});
    const auto root_thread = plan_root_thread(version, "plan-race");
    reset_plan_steps([root_thread](std::size_t, const NodeInput& input) -> json {
        if (input.ctx.thread_id == root_thread) return json{{"ask", true}};
        return json{{"block", true}};
    });

    const auto paused = run_plan(fixture, version, "plan-race");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
    EXPECT_EQ(paused_operation(paused), "root.0");
    // The loser's node body runs on a worker (#367, #368). It blocks until the
    // question wins and cancels it, or, when that happens before a worker
    // starts it, it never starts.
    const auto loser_runs = [&root_thread] {
        const auto runs = plan_steps();
        return static_cast<std::size_t>(
            std::count_if(runs.begin(), runs.end(), [&](const PlanStepRun& run) {
                return !run.resumed && run.thread_id != root_thread;
            }));
    };
    const auto loser_runs_before = loser_runs();
    EXPECT_LE(loser_runs_before, 1U);

    const auto done = decide(fixture, paused, "won");
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    EXPECT_EQ(channel_value(done.output()), json("won"));
    EXPECT_EQ(loser_runs(), loser_runs_before) << "the cancelled loser is not run again";
    EXPECT_EQ(plan_steps_counted(false), 1U + loser_runs_before);
    EXPECT_EQ(plan_steps_counted(true), 1U);
}

TEST(ProgramRuntimeTest, EachConcurrentPlanBranchRunsOnItsOwnCoreThread) {
    // #386: branch 0 of a parallel continues on the parallel's own Core
    // thread. Branch 0 of a nested parallel therefore no longer shares the
    // root thread with the root's branch 0 while both run; the root's branch
    // 0 keeps the root thread, as before.
    AdmittedRuntime fixture(3);
    reset_plan_steps([](std::size_t, const NodeInput&) { return json{{"write", "ok"}}; },
                     std::chrono::milliseconds(150));
    const auto version = admit_plan(
        fixture, plan_parallel(json::array(
                     {plan_call(), plan_parallel(json::array({plan_call(), plan_call()}))})));
    ProgramInvocation invocation{json{{"value", "start"}}, RunBudget{10000, 1000, 1000, 3, 32, 20, 0, 0, 0},
                                 "trace-plan-threads", {}};
    invocation.requested_run_id = "plan-threads";
    const auto done = fixture.runtime->run("tenant:runtime", version, std::move(invocation));
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);

    const auto runs = plan_steps();
    ASSERT_EQ(runs.size(), 3U);
    std::set<std::string> threads;
    for (const auto& run : runs) threads.insert(run.thread_id);
    EXPECT_EQ(threads.size(), 3U) << "three concurrent Core calls, three Core threads";
    EXPECT_FALSE(plan_step_overlap.load()) << "no Core thread ran two calls at once";
    EXPECT_EQ(threads.count(plan_root_thread(version, "plan-threads")), 1U)
        << "the root's branch 0 keeps the root thread";
}

TEST(ProgramRuntimeTest, ForkOfAPausedPlanReplaysWhatTheSourceFinished) {
    // #386: A finishes, then both branches of a parallel ask. The fork answers
    // the first: A is replayed, not run again, and the other branch asks its
    // own question again from a checkpoint the fork cloned into its own run.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
        if (ordinal == 1) return json{{"write", "A"}};
        return json{{"ask", true}};
    });
    const auto version = admit_plan(
        fixture, plan_sequence(json::array(
                     {plan_call(), plan_parallel(json::array({plan_call(), plan_call()}))})));
    const auto source = run_plan(fixture, version, "plan-fork-source");
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted) << failure_detail(source);
    EXPECT_EQ(paused_operation(source), "root.1.0");
    const auto source_record = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_record && source_record->settled_branches());
    const auto source_branch = (*source_record->settled_branches())["root.1.1"];
    ASSERT_EQ(source_branch["kind"], json("paused"));

    const auto forked = fixture.runtime
                            ->fork("tenant:runtime",
                                   ExactProgramCheckpointReference{
                                       source.run_id(), source.checkpoint()->checkpoint_id},
                                   version,
                                   ProgramInvocation{json{{"value", "start"}},
                                                     neograph::test::fork_share(source.remaining_budget()),
                                                     "trace-plan-fork", {}, "plan-fork-target"},
                                   resume_for(source, json{{"decision", "first"}},
                                              "trace-plan-fork-resume"))
                            .wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Interrupted) << failure_detail(forked);
    EXPECT_EQ(paused_operation(forked), "root.1.1") << "the other branch asks again";
    ASSERT_TRUE(forked.checkpoint().has_value());
    EXPECT_NE(forked.checkpoint()->core_thread_id,
              source_branch["checkpoint"]["core_thread_id"].get<std::string>())
        << "the fork continues from its own copy of the branch's checkpoint";
    EXPECT_EQ(plan_steps_counted(false), 3U) << "A and the branches asked once each";

    const auto done = decide(fixture, forked, "second");
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    ASSERT_TRUE(done.output().is_array());
    EXPECT_EQ(channel_value(done.output()[0]), json("first"));
    EXPECT_EQ(channel_value(done.output()[1]), json("second"));
    EXPECT_EQ(plan_steps_counted(false), 3U) << "A never runs again";
    EXPECT_EQ(plan_steps_counted(true), 2U);
}

TEST(ProgramRuntimeTest, ForkOfAPlanPausedInALaterBranchContinuesThatBranch) {
    // #386: A finishes and B, the second branch, asks. A fork copies B's
    // checkpoint into its own run's root thread; the answered call continues
    // on the thread of its checkpoint, whatever thread the plan gives B, and A
    // replays from its record.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    const auto version =
        admit_plan(fixture, plan_parallel(json::array({plan_call(), plan_call()})));
    const auto source_root = plan_root_thread(version, "plan-fork-later");
    reset_plan_steps([source_root](std::size_t, const NodeInput& input) -> json {
        if (input.ctx.thread_id == source_root) return json{{"write", "A"}};
        return json{{"ask", true}};
    });
    const auto source = run_plan(fixture, version, "plan-fork-later");
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted) << failure_detail(source);
    EXPECT_EQ(paused_operation(source), "root.1");
    EXPECT_NE(source.checkpoint()->core_thread_id, source_root) << "B runs on its own thread";

    const auto forked = fixture.runtime
                            ->fork("tenant:runtime",
                                   ExactProgramCheckpointReference{
                                       source.run_id(), source.checkpoint()->checkpoint_id},
                                   version,
                                   ProgramInvocation{json{{"value", "start"}},
                                                     neograph::test::fork_share(source.remaining_budget()),
                                                     "trace-plan-fork-later", {},
                                                     "plan-fork-later-target"},
                                   resume_for(source, json{{"decision", "B"}},
                                              "trace-plan-fork-later-resume"))
                            .wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << failure_detail(forked);
    ASSERT_TRUE(forked.output().is_array());
    EXPECT_EQ(channel_value(forked.output()[0]), json("A"));
    EXPECT_EQ(channel_value(forked.output()[1]), json("B"));
    EXPECT_EQ(plan_steps_counted(false), 2U) << "A is replayed in the fork, not run again";
    EXPECT_EQ(plan_steps_counted(true), 1U);
}

TEST(ProgramRuntimeTest, ReconcileOfAPausedPlanReplaysWhatTheRunFinished) {
    // #386: A finishes, then E leaves an effect to reconcile. The completed
    // reconciliation continues E with its result; A does not run again.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
        if (ordinal == 1) return json{{"write", "A"}};
        return json{{"effect", true}};
    });
    const auto version =
        admit_plan(fixture, plan_sequence(json::array({plan_call(), plan_call()})));
    const auto paused = run_plan(fixture, version, "plan-reconcile");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
    ASSERT_TRUE(paused.interrupt() && paused.interrupt()->pending_effect);
    const auto pending = *paused.interrupt()->pending_effect;
    ASSERT_EQ(fixture.runtime
                  ->reconcile("tenant:runtime", paused.run_id(),
                              ProgramEffectResolution{pending.call_id(),
                                                      ProgramEffectReconciliation::Unknown,
                                                      std::nullopt, "trace-plan-unknown", {}})
                  .wait()
                  .status(),
              ProgramTerminalStatus::AmbiguousEffect);

    const auto done =
        fixture.runtime
            ->reconcile("tenant:runtime", paused.run_id(),
                        ProgramEffectResolution{pending.call_id(),
                                                ProgramEffectReconciliation::Completed,
                                                json{{"decision", "reconciled"}},
                                                "trace-plan-reconciled", {}})
            .wait();
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_detail(done);
    EXPECT_EQ(channel_value(done.output()), json("reconciled"));
    EXPECT_EQ(plan_steps_counted(false), 2U) << "A is replayed, not run again";
    EXPECT_EQ(plan_steps_counted(true), 1U);
}

TEST(ProgramRuntimeTest, RecoveryOfAResumedPlanAttemptReplaysWhatTheRunFinished) {
    // #386: the attempt that resumes B stops inside B; a fresh runtime over
    // the same stores recovers it, and replays A from the record too.
    AdmittedRuntime fixture(1);
    reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
        if (ordinal == 1) return json{{"write", "A"}};
        return json{{"ask", true}};
    });
    const auto version =
        admit_plan(fixture, plan_sequence(json::array({plan_call(), plan_call()})));
    const auto paused = run_plan(fixture, version, "plan-recovery");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);

    HeldResumedAttempt held;
    auto original = fixture.runtime->resume(
        "tenant:runtime", paused.run_id(),
        resume_for(paused, json{{"decision", "recovered"}}, "trace-plan-recovery-resume"));
    ASSERT_TRUE(held.entered());

    auto       fresh     = fixture.make_runtime();
    const auto recovered = fresh->reconnect("tenant:runtime", paused.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_detail(recovered);
    EXPECT_EQ(channel_value(recovered.output()), json("recovered"));
    EXPECT_EQ(plan_steps_counted(false), 2U) << "A is replayed by the recovered attempt too";
    held.release();
    (void)original.wait();
}

TEST(ProgramRuntimeTest, AReplacementOrAGraphMigrationNeverContinuesAPausedPlan) {
    // #386: resume, reconcile, recovery and fork carry the plan's replay
    // records. Replacement and graph migration cannot continue a paused plan
    // at all, so they have none to carry: replacement needs a quiescent
    // JavaScript source, and graph migration a single root CallCore.
    JoinTestStores  stores(false);
    AdmittedRuntime fixture(2, stores.checkpoints, stores.transitions, {},
                            ExecutionGuarantee::Unmanaged, true);
    reset_plan_steps([](std::size_t ordinal, const NodeInput&) -> json {
        if (ordinal == 1) return json{{"write", "A"}};
        return json{{"ask", true}};
    });
    const auto version =
        admit_plan(fixture, plan_sequence(json::array({plan_call(), plan_call()})));
    const auto paused = run_plan(fixture, version, "plan-no-handoff");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_detail(paused);
    const auto record = fixture.journal->load("tenant:runtime", paused.run_id());
    ASSERT_TRUE(record && record->settled_branches());

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
    // The replacement target is a JavaScript source.
    const auto target =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", "return 1;"));
    try {
        (void)fixture.runtime->replace(
            "tenant:runtime",
            ExactProgramHandoffReference{paused.run_id(), record->journal_head(), "coordinate",
                                         "entry"},
            target,
            ProgramInvocation{json{{"handoff", json::object()}, {"previous_run_id", paused.run_id()}},
                              paused.remaining_budget(), "trace-plan-replace", {}});
        FAIL() << "a paused plan was replaced";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_REPLACEMENT_STATE");
    }

    EXPECT_EQ(fixture.journal->load("tenant:runtime", paused.run_id())->id(), record->id())
        << "the paused run is untouched";
#endif

    // Graph migration takes a live run, and refuses any plan but one root
    // CallCore before it holds the run anywhere.
    reset_plan_steps([](std::size_t, const NodeInput&) { return json{{"block", true}}; });
    ProgramInvocation invocation{json{{"value", "start"}}, plan_budget(), "trace-plan-live", {}};
    invocation.requested_run_id = "plan-live";
    auto live = fixture.runtime->start("tenant:runtime", version, std::move(invocation));
    try {
        (void)fixture.runtime->migrate_graph(live, ProgramGraphMigrationTarget{version.id()});
        FAIL() << "a plan was migrated";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_GRAPH_MIGRATION_INCOMPATIBLE");
    }
    (void)live.cancel();
    EXPECT_EQ(live.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, NestedParallelCannotExceedRunConcurrencyGrant) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(4);
    const json      nested{
             {"op", "parallel"},
             {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}};
    const auto version = fixture.admit_document(orchestration_document(
        json{{"op", "parallel"}, {"branches", json::array({nested, nested})}},
        "runtime-short-blocking"));

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 2, 32, 20, 0, 0, 0},
                                               "trace-nested-concurrency",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CONCURRENCY_BUDGET");
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
    EXPECT_EQ(blocking_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, RaceReturnsTheFirstCompletedBranchAndCancelsTheLoser) {
    AdmittedRuntime fixture(2);
    const json      first{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "return"}, {"value", json{{"branch", 1}}}}})}};
    const json second{{"op", "return"}, {"value", json{{"branch", 2}}}};
    const auto result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "race"}, {"branches", json::array({first, second})}},
                               "runtime-short-blocking"),
        json::object(), "trace-race");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"branch", 2}}));
}

TEST(ProgramRuntimeTest, RaceImmediateBranchesRetainTheirCompletionSignal) {
    AdmittedRuntime fixture(1);
    const json      unreachable_core{{"op", "branch"},
                                     {"condition", json{{"path", "/unused"}, {"exists", true}}},
                                     {"then", json{{"op", "call_core"}}},
                                     {"else", json{{"op", "return"}, {"value", 2}}}};
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{
                 {"op", "race"},
                 {"branches", json::array({json{{"op", "return"}, {"value", 1}}, unreachable_core})}}),
        json::object(), "trace-race-immediate");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), 1);
}

TEST(ProgramRuntimeTest, ImmediateParallelAndRaceJoinsWakeOnAMultiThreadedScheduler) {
    // A plan parallel or race whose branches finish at once signals its join
    // on the strand its branch completions run on, often while the parent is
    // starting to wait. The asio channel that carries the signal is not
    // thread-safe; a wait started on another scheduler thread could miss it
    // and the run never finished (ParallelImmediateBranchesRetainTheir-
    // CompletionSignal hung on Windows). Many runs at once on four scheduler
    // threads; a run that misses its deadline fails the test, and the runtime
    // is left alive so that its shutdown does not wait for that run.
    const auto immediate = [](int value) { return json{{"op", "return"}, {"value", value}}; };
    // A plan needs a Core call; this one is never dispatched.
    const auto unreachable_core = [&](int value) {
        return json{{"op", "branch"},
                    {"condition", json{{"path", "/unused"}, {"exists", true}}},
                    {"then", json{{"op", "call_core"}}},
                    {"else", immediate(value)}};
    };
    const std::vector<std::pair<std::string, json>> joins{
        {"parallel",
         json{{"op", "parallel"},
              {"branches",
               json::array({immediate(1), immediate(2), immediate(3), unreachable_core(4)})}}},
        {"race",
         json{{"op", "race"}, {"branches", json::array({immediate(1), unreachable_core(2)})}}}};
    constexpr std::size_t kRounds       = 40;
    constexpr std::size_t kRunsPerRound = 32;
    constexpr auto        kDeadline     = std::chrono::seconds(30);
    for (const auto& [name, root] : joins) {
        SCOPED_TRACE(name);
        auto       fixture = std::make_unique<AdmittedRuntime>(4);
        const auto version = fixture->admit_document(orchestration_document(root));
        bool       hung    = false;
        for (std::size_t round = 0; round < kRounds && !hung; ++round) {
            std::vector<ProgramHandle> handles;
            handles.reserve(kRunsPerRound);
            for (std::size_t run = 0; run < kRunsPerRound; ++run) {
                handles.push_back(fixture->runtime->start(
                    "tenant:runtime", version,
                    ProgramInvocation{json::object(),
                                      RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                                      "trace-immediate-join-" + name,
                                      {}}));
            }
            const auto deadline = std::chrono::steady_clock::now() + kDeadline;
            for (const auto& handle : handles) {
                std::optional<ProgramResult> result;
                while (!(result = handle.try_result()) &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!result) {
                    ADD_FAILURE() << "a " << name << " join of immediate branches lost its "
                                  << "completion signal in round " << round;
                    hung = true;
                    break;
                }
                EXPECT_EQ(result->status(), ProgramTerminalStatus::Completed)
                    << failure_detail(*result);
            }
        }
        // A run that is still waiting would block the runtime's shutdown.
        if (hung) (void)fixture->runtime.release();
    }
}

TEST(ProgramRuntimeTest, RaceUsesDeclarationOrderForAReadyTie) {
    // A ready tie is a single scheduler-turn condition. Use one scheduler
    // thread so this verifies declaration-order arbitration rather than
    // treating cross-worker completion timing as a logical tie. The branches
    // emit instead of calling Core: a Core node body runs on a worker, off the
    // scheduler (#367, #368), so two Core calls finish in worker order. The
    // plan's Core call runs before the race.
    AdmittedRuntime fixture(1);
    const auto      branch = [](int value) {
        return json{{"op", "sequence"},
                    {"children", json::array({json{{"op", "emit"}, {"value", json{{"branch", value}}}},
                                              json{{"op", "return"}, {"value", value}}})}};
    };
    const auto result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "sequence"},
                 {"children",
                  json::array({json{{"op", "call_core"}},
                               json{{"op", "race"},
                                    {"branches", json::array({branch(1), branch(2)})}}})}}),
        json::object(), "trace-race-tie");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), 1);
}

TEST(ProgramRuntimeTest, QuorumStopsAfterRequiredSuccesses) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "quorum"},
                                         {"branches", json::array({json{{"op", "return"}, {"value", 1}},
                                                                   json{{"op", "return"}, {"value", 2}},
                                                                   json{{"op", "call_core"}}})},
                                         {"min_success", 2}}),
        json::object(), "trace-quorum");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({1, 2}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, MapEvaluatesEveryItemAndCollectsOutputs) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{
                 {"op", "map"},
                 {"items", json::array({1, 2})},
                 {"body", json{{"op", "sequence"},
                               {"children", json::array({json{{"op", "call_core"}},
                                                         json{{"op", "return"},
                                                              {"value", json{{"mapped", true}}}}})}}}}),
        json::object(), "trace-map");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"mapped", true}}, json{{"mapped", true}}}));
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, ExpandTaskGraphPublishesAndBindsDependentChildTasks) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = link_child_versions(
        fixture, fixture.admit_document(expand_task_graph_document(expansion_proposal())),
        fixture.admit_document(parallel_map_echo_child_document()),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json{{"item", "seed"}},
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 1, 2, 2},
                                                 "trace-expand-task-graph",
                                                 {}});
    const auto result = parent.wait();
    if (result.status() != ProgramTerminalStatus::Completed) {
        std::cerr << "expand failure=" << (result.failure() ? result.failure()->code : "<none>")
                  << " message=" << (result.failure() ? result.failure()->message : "<none>")
                  << " output=" << result.output().dump() << '\n';
    }

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 2U);
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 0U);
    const auto lineage_id = program_run_lineage_id("tenant:runtime", result.run_id());
    const auto lineage    = fixture.journal->load_lineage("tenant:runtime", lineage_id);
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(lineage->remaining_budget().max_dynamic_compiles, 0U);
    EXPECT_GT(lineage->committed_descendant_budget().max_total_children, 0U);
    const auto output = result.output();
    ASSERT_TRUE(output.is_object());
    ASSERT_TRUE(output.contains("fragment_id"));
    ASSERT_TRUE(output.contains("tasks"));
    ASSERT_TRUE(output["tasks"].is_array());
    ASSERT_EQ(output["tasks"].size(), 2U);
    EXPECT_EQ(output["tasks"][0]["task_id"], "seed");
    EXPECT_EQ(output["tasks"][0]["state"], "completed");
    EXPECT_EQ(output["tasks"][1]["task_id"], "dependent");
    EXPECT_EQ(output["tasks"][1]["state"], "completed");
    EXPECT_EQ(output["tasks"][0]["output"]["channels"]["value"]["value"], "seed");
    EXPECT_EQ(output["tasks"][1]["output"]["channels"]["value"]["value"], "seed");

    const auto fragment_id = output["fragment_id"].get<std::string>();
    const auto record      = fixture.task_graph_fragments->load(fragment_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->published);
    EXPECT_TRUE(record->terminal);
    EXPECT_EQ(record->revision, 6U);
    ASSERT_EQ(record->tasks.size(), 2U);
    for (const auto& task : record->tasks) {
        EXPECT_EQ(task.state, TaskGraphTaskState::Completed);
        EXPECT_EQ(task.attempt, 1U);
    }
}

// #412. A task graph aggregate must count what each launched task receives. Schema-1
// fragments keep their behaviour (an unset dimension is launched with the link ceiling);
// schema-2 fragments launch every task with exactly what its reservation counted.
namespace {

json expansion_task_json(const char* id, std::vector<std::string> dependencies, json budget) {
    json bindings = json::array();
    for (const auto& dependency : dependencies) {
        bindings.push_back(json{{"from", json{{"task", dependency},
                                              {"artifact", "result"},
                                              {"field", "/channels/value/value"}}},
                                {"to", json{{"field", "/item"}}}});
    }
    return json{{"id", id},
                {"template", "runtime-map/v1"},
                {"input_bindings", std::move(bindings)},
                {"depends_on", std::move(dependencies)},
                {"budget", std::move(budget)}};
}

json two_task_expansion_proposal(std::uint32_t schema_version, json seed_budget,
                                 json dependent_budget) {
    return json{{"schema_version", schema_version},
                {"tasks", json::array({expansion_task_json("seed", {}, std::move(seed_budget)),
                                       expansion_task_json("dependent", {"seed"},
                                                           std::move(dependent_budget))})},
                {"join", json{{"kind", "all"}}}};
}

struct ExpansionBudgetRun {
    ProgramHandle                 parent;
    ProgramResult                 result;
    TaskGraphFragmentRecord       record;
    std::map<std::string, RunBudget> granted;  // task id -> budget the child was launched with
};

ExpansionBudgetRun run_budgeted_expansion(AdmittedRuntime& fixture, json proposal) {
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(expand_task_graph_document(std::move(proposal))),
        fixture.admit_document(parallel_map_echo_child_document()),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});
    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json{{"item", "seed"}}, RunBudget{10000, 1000, 1000, 1, 2, 20, 1, 2, 2},
                          "trace-expansion-budget", {}});
    auto result = parent.wait();
    const auto fragment_id = result.output().contains("fragment_id")
                                 ? result.output()["fragment_id"].get<std::string>()
                                 : std::string();
    auto record = fixture.task_graph_fragments->load(fragment_id);
    if (!record) throw std::runtime_error("expansion published no fragment");
    std::map<std::string, RunBudget> granted;
    for (const auto& task : record->tasks) {
        if (!task.child_run_id) continue;
        for (const auto& child : parent.snapshot().children()) {
            if (child.child_run_id == *task.child_run_id)
                granted[task.task_id] = child.invocation.granted_budget;
        }
    }
    return ExpansionBudgetRun{std::move(parent), std::move(result), std::move(*record),
                              std::move(granted)};
}

}  // namespace

TEST(ProgramRuntimeTest, ExpandTaskGraphSchemaTwoLaunchesEachTaskWithWhatTheAggregateCounted) {
    AdmittedRuntime fixture(2);
    const auto run = run_budgeted_expansion(
        fixture, two_task_expansion_proposal(
                     2, json{{"wall_time_ms", 700}, {"model_tokens", 7},
                             {"monetary_microunits", 9}, {"output_bytes", 2048}},
                     json{{"wall_time_ms", 400}, {"model_tokens", 4},
                          {"monetary_microunits", 5}, {"output_bytes", 1024}}));

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    EXPECT_EQ(run.record.fragment.schema_version(), 2U);
    ASSERT_EQ(run.granted.size(), 2U);
    EXPECT_EQ(run.granted.at("seed").wall_time_ms, 700U);
    EXPECT_EQ(run.granted.at("seed").model_tokens, 7U);
    EXPECT_EQ(run.granted.at("seed").monetary_microunits, 9U);
    EXPECT_EQ(run.granted.at("dependent").wall_time_ms, 400U);
    EXPECT_EQ(run.granted.at("dependent").model_tokens, 4U);
    EXPECT_EQ(run.granted.at("dependent").monetary_microunits, 5U);
    // What the aggregate counted is what the children were granted.
    TaskGraphBudget granted_total;
    for (const auto& [id, granted] : run.granted) {
        granted_total.wall_time_ms += granted.wall_time_ms;
        granted_total.model_tokens += granted.model_tokens;
        granted_total.monetary_microunits += granted.monetary_microunits;
    }
    const auto& aggregate = run.record.fragment.aggregate_budget();
    EXPECT_EQ(granted_total.wall_time_ms, aggregate.wall_time_ms);
    EXPECT_EQ(granted_total.model_tokens, aggregate.model_tokens);
    EXPECT_EQ(granted_total.monetary_microunits, aggregate.monetary_microunits);
    EXPECT_EQ(aggregate.output_bytes, 3072U);
}

TEST(ProgramRuntimeTest, ExpandTaskGraphSchemaOneStillLaunchesAnUnsetDimensionWithTheLinkCeiling) {
    // Published schema-1 data keeps its behaviour: the monetary dimension is unset, counted as
    // 0, and the child is launched with the link's 333. Hosts close this by setting
    // TaskGraphProposalLimits::min_schema_version to 2.
    AdmittedRuntime fixture(2);
    const auto run = run_budgeted_expansion(fixture, expansion_proposal());

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(run.record.fragment.schema_version(), 1U);
    EXPECT_EQ(run.record.fragment.aggregate_budget().monetary_microunits, 0U);
    ASSERT_EQ(run.granted.size(), 2U);
    for (const auto& [id, granted] : run.granted) {
        EXPECT_EQ(granted.wall_time_ms, 100U);
        EXPECT_EQ(granted.model_tokens, 1U);
        EXPECT_EQ(granted.monetary_microunits, 333U);
    }
}

TEST(ProgramRuntimeTest, ExpandTaskGraphSchemaTwoRefusesUnsetDimensionsBeforeAnythingRuns) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto linked = link_child_versions(
        fixture,
        fixture.admit_document(expand_task_graph_document(two_task_expansion_proposal(
            2, json{{"wall_time_ms", 100}, {"model_tokens", 1}},
            json{{"wall_time_ms", 100}, {"model_tokens", 1}}))),
        fixture.admit_document(parallel_map_echo_child_document()),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json{{"item", "seed"}}, RunBudget{10000, 1000, 1000, 1, 2, 20, 1, 2, 2},
                          "trace-expansion-budget-refused", {}});
    const auto result = parent.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_EXPAND_REJECTED");
    const auto& diagnostics = result.failure()->witness["diagnostics"];
    ASSERT_EQ(diagnostics.size(), 4U);  // monetary_microunits and output_bytes of both tasks
    for (std::size_t index = 0; index < diagnostics.size(); ++index)
        EXPECT_EQ(diagnostics[index]["code"], "P_PROPOSAL_BUDGET_ZERO");
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_TRUE(parent.snapshot().children().empty());
}

TEST(ProgramRuntimeTest, ExpandTaskGraphSchemaTwoZeroGrantsNothingWhereTheTemplateCannotSpend) {
    // A template whose monetary ceiling is 0 is stated as 0 and the child is launched with 0,
    // not with the link's 333 (schema 1 would launch it with 333).
    AdmittedRuntime fixture(2);
    fixture.expansion->template_allowlist.front().budget_ceiling.monetary_microunits = 0;
    fixture.expansion->limits.per_task_budget_ceiling.monetary_microunits = 0;
    fixture.expansion->limits.total_budget_ceiling.monetary_microunits = 0;
    const auto run = run_budgeted_expansion(
        fixture, two_task_expansion_proposal(
                     2, json{{"wall_time_ms", 700}, {"model_tokens", 7}, {"output_bytes", 2048}},
                     json{{"wall_time_ms", 400}, {"model_tokens", 4}, {"output_bytes", 1024}}));

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    ASSERT_EQ(run.granted.size(), 2U);
    for (const auto& [id, granted] : run.granted) EXPECT_EQ(granted.monetary_microunits, 0U);
}

TEST(ProgramRuntimeTest, ExpandTaskGraphSchemaTwoHoldsAnOutputToItsReservedBytes) {
    // Schema 1 reads an output_bytes of 0 as unbounded. Schema 2 reads it as 0 bytes.
    for (const std::uint32_t schema : {1U, 2U}) {
        SCOPED_TRACE(schema);
        AdmittedRuntime fixture(2);
        fixture.expansion->template_allowlist.front().budget_ceiling.output_bytes = 0;
        fixture.expansion->limits.per_task_budget_ceiling.output_bytes = 0;
        fixture.expansion->limits.total_budget_ceiling.output_bytes = 0;
        json budget{{"wall_time_ms", 700}, {"model_tokens", 7}, {"monetary_microunits", 9}};
        const auto run = run_budgeted_expansion(
            fixture, two_task_expansion_proposal(schema, budget, budget));
        if (schema == 1U) {
            EXPECT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
        } else {
            EXPECT_EQ(run.result.status(), ProgramTerminalStatus::Failed);
            ASSERT_FALSE(run.record.tasks.empty());
            ASSERT_TRUE(run.record.tasks[0].failure.has_value());
            EXPECT_EQ((*run.record.tasks[0].failure)["code"], "P_TASK_OUTPUT_BUDGET");
        }
    }
}

TEST(ProgramRuntimeTest, MapExecutesItemsSerially) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    AdmittedRuntime fixture(2);
    const auto      result =
        run_orchestration(fixture,
                          orchestration_document(json{{"op", "map"},
                                                      {"items", json::array({1, 2})},
                                                      {"body", json{{"op", "call_core"}}}},
                                                 "runtime-short-blocking"),
                          json::object(), "trace-map-serial");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 1U);
    EXPECT_EQ(blocking_active.load(), 0U);
}

TEST(ProgramRuntimeTest, AwaitTimeoutCancelsTheChildOperation) {
    for (const auto threads : {1U, 2U, 4U}) {
        SCOPED_TRACE(threads);
        AdmittedRuntime fixture(threads);
        const auto      started = std::chrono::steady_clock::now();
        const auto      result  = run_orchestration(
            fixture,
            orchestration_document(
                json{{"op", "await"}, {"timeout_ms", 10}, {"body", json{{"op", "call_core"}}}},
                "runtime-blocking"),
            json::object(), "trace-await");

        EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
        ASSERT_TRUE(result.failure().has_value());
        EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
        EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - started).count(), 2);
    }
}

TEST(ProgramRuntimeTest, AwaitPropagatesChildFailureBeforeTimeout) {
    AdmittedRuntime fixture;
    const auto      started = std::chrono::steady_clock::now();
    const auto      result  = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "await"}, {"timeout_ms", 1000}, {"body", json{{"op", "call_core"}}}},
            "runtime-failing"),
        json::object(), "trace-await-failure");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count(),
              500);
}

TEST(ProgramRuntimeTest, ExplicitCancelStopsTheFollowingOperation) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "sequence"},
                      {"children", json::array({json{{"op", "cancel"}}, json{{"op", "call_core"}}})}}),
        json::object(), "trace-cancel");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ExplicitCheckpointPublishesCheckpointEventAfterCore) {
    auto            sink = std::make_shared<CountingSink>();
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(
        orchestration_document(json{{"op", "checkpoint"}, {"body", json{{"op", "call_core"}}}}));
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                          "trace-checkpoint", sink});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(result.checkpoint().has_value());
    EXPECT_GE(sink->calls.load(), 1U);
}
TEST(ProgramRuntimeTest, SequenceEmitAndReturnHaveDeterministicTraceAndOutput) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(orchestration_document());

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                                               "trace-orchestration",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"ok", true}}));
    EXPECT_EQ(result.execution_trace(), std::vector<std::string>({"work"}));

    EXPECT_EQ(result.usage().program_operations, 4U);
}
TEST(ProgramRuntimeTest, ChildStartPinsReceiptAndPropagatesParentCancellation) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{
            json::object(), RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1}, "trace-parent", {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{
            json::object(), RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0}, "trace-child", {}});

    const auto child_record = child.snapshot();
    EXPECT_EQ(child_record.invocation().parent_run_id, parent.run_id());
    EXPECT_EQ(child_record.child_depth(), 1U);
    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, TypedChildStoreDoesNotRequireJavaScriptHistory) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    journal->javascript_reads_unavailable = true;
    journal->release_result();
    AdmittedRuntime fixture(2, {}, journal);
    TypedParentGate gate;
    const auto linked = make_linked_child(fixture, "runtime-typed-parent-gate", "runtime-completed");
    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                          "typed-parent-without-js", {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(), RunBudget{3333, 333, 333, 1, 1, 6, 0, 0, 0},
                          "typed-child-without-js", {}});
    const auto result = child.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    const auto head = journal->load_run_publication_head_sync("tenant:runtime", parent.run_id());
    ASSERT_TRUE(head);
    const auto children = head->run_record.children();
    ASSERT_EQ(children.size(), 1U);
    ASSERT_TRUE(children.front().terminal_result);
    EXPECT_EQ(children.front().terminal_result->id(), result.id());
    EXPECT_EQ(head->run_record.journal_head(), head->journal_record.id);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(journal->javascript_read_attempts.load(), 0U);
    EXPECT_TRUE(parent.cancel());
    gate.open();
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, TypedChildFallbackRetriesSiblingPublicationWithoutRedispatch) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    journal->javascript_reads_unavailable = true;
    journal->release_result();
    AdmittedRuntime fixture(2, {}, journal);
    TypedParentGate gate;
    auto document = program_document("runtime-typed-parent-gate");
    document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    document["declared_budget_requirements"][4]["minimum"] = 2;
    document["declared_budget_requirements"][4]["maximum"] = 2;
    document["declared_budget_requirements"][7]["minimum"] = 1;
    document["declared_budget_requirements"][7]["maximum"] = 1;
    document["declared_budget_requirements"][8]["minimum"] = 2;
    document["declared_budget_requirements"][8]["maximum"] = 2;
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(document)), fixture.admit("runtime-completed"));
    const std::string parent_id = "typed-fallback-parent";
    auto armed = std::make_shared<std::atomic<bool>>(false);
    auto sibling_completion = std::make_shared<std::optional<ProgramResult>>();
    auto allow_completion = std::make_shared<std::atomic<bool>>(false);
    journal->before_publication =
        [parent_id, allow_completion](const ProgramTransitionPublication& value) {
            if (value.run_record.run_id() != parent_id || allow_completion->load()) return;
            for (const auto& child : value.run_record.children())
                if (child.terminal_result)
                    throw std::runtime_error("defer sibling completion until acquisition");
        };
    const RunBudget child_budget{3333, 333, 333, 1, 1, 6, 0, 0, 0};
    auto advances = std::make_shared<std::atomic<unsigned>>(0);
    const auto weak_journal = std::weak_ptr<BlockAfterJavaScriptResultJournal>(journal);
    // Simulate another publisher committing the real sibling result after the
    // fallback has captured its first parent record, before it reads the journal.
    journal->filter_run_read =
        [parent_id, sibling_completion, allow_completion, armed, advances, weak_journal](
            std::string_view id, std::optional<ProgramRunRecord> record) {
            if (id != parent_id || !record || !armed->load() || advances->exchange(1) != 0)
                return record;
            const auto store = weak_journal.lock();
            const auto previous_journal = store->latest_sync("tenant:runtime", parent_id);
            const auto lineage = store->load_run_lineage_sync("tenant:runtime", parent_id);
            if (!previous_journal || !lineage)
                throw std::runtime_error("Sibling publication has no durable parent head");
            auto next_journal = ProgramJournalRecord::create(ProgramJournalRecordData{
                record->journal_head(), parent_id, record->program_version_id(), record->bundle_id(),
                previous_journal->sequence + 1, record->continuation(), record->remaining_budget(),
                previous_journal->inflight_reservation, record->exact_checkpoint(),
                record->updated_at_ms()});
            ProgramRunRecordData data;
            data.owner_scope = record->owner_scope();
            data.run_id = record->run_id();
            data.logical_run_id = record->logical_run_id();
            data.program_version_id = record->program_version_id();
            data.bundle_id = record->bundle_id();
            data.binding_fingerprint = record->binding_fingerprint();
            data.invocation = record->invocation();
            data.child_depth = record->child_depth();
            data.continuation = record->continuation();
            data.remaining_budget = record->remaining_budget();
            data.exact_checkpoint = record->exact_checkpoint();
            data.exact_checkpoint_content_id = record->exact_checkpoint_content_id();
            data.pending_input = record->pending_input();
            data.pending_effect = record->pending_effect();
            data.terminal_result = record->terminal_result();
            data.fork_receipt = record->fork_receipt();
            data.children = record->children();
            data.children.front().state = ProgramChildState::Completed;
            data.children.front().terminal_result = **sibling_completion;
            data.journal_head = next_journal.id;
            data.fork_source_run_id = record->fork_source_run_id();
            data.fork_source_program_version_id = record->fork_source_program_version_id();
            data.fork_source_checkpoint_id = record->fork_source_checkpoint_id();
            data.recorded_binding_set_fingerprint = record->recorded_binding_set_fingerprint();
            data.event_sequence = record->event_sequence();
            data.effect_sequence = record->effect_sequence();
            data.created_at_ms = record->created_at_ms();
            data.updated_at_ms = record->updated_at_ms();
            auto next_run = ProgramRunRecord::create(std::move(data));
            auto next_lineage = ProgramRunLineage::create(ProgramRunLineageData{
                lineage->owner_scope(), lineage->lineage_id(), lineage->root_run_id(),
                lineage->active_generation(), lineage->active_generation_id(), next_run.id(),
                next_run.journal_head(), lineage->remaining_budget(),
                lineage->inflight_reservation(), lineage->id(), lineage->created_at_ms(),
                next_run.updated_at_ms(), lineage->committed_descendant_budget()});
            ProgramTransitionPublication publication{next_run, std::move(next_journal), {}, {}};
            publication.run_lineage = std::move(next_lineage);
            allow_completion->store(true);
            if (store->compare_publish_sync("tenant:runtime", record->journal_head(),
                                            std::move(publication)) !=
                ProgramTransitionPublishResult::Published)
                throw std::runtime_error("Sibling completion publication was rejected");
            return record;
        };
    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 2},
                          "typed-fallback-parent", {}, parent_id});
    auto sibling = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(), child_budget, "typed-fallback-sibling", {}});
    const auto sibling_result = sibling.wait();
    ASSERT_EQ(sibling_result.status(), ProgramTerminalStatus::Completed)
        << sibling_result.serialize_canonical();
    ASSERT_EQ(parent.snapshot().children().front().state, ProgramChildState::Dispatched);
    *sibling_completion = sibling_result;
    armed->store(true);
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(), child_budget, "typed-fallback-new-child", {}});
    const auto result = child.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(advances->load(), 1U);
    EXPECT_EQ(completed_calls.load(), 2U);
    EXPECT_EQ(journal->javascript_read_attempts.load(), 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    ASSERT_TRUE(children.front().terminal_result);
    ASSERT_TRUE(children.back().terminal_result);
    EXPECT_EQ(children.front().terminal_result->id(), sibling_result.id());
    EXPECT_EQ(children.back().terminal_result->id(), result.id());
    EXPECT_TRUE(parent.cancel());
    gate.open();
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, PlannerCannotOverrideResolvedChildBudget) {
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-planner-budget-parent",
                                                 {}});

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 21, 0, 0, 0},
                              "trace-planner-budget-override",
                              {}});
        FAIL() << "Expected the planner-supplied child budget to be rejected";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_BUDGET");
    }

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, RecursiveChildGrantsAttenuateToZeroAtTheConfiguredBoundary) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(4);
    const auto      linked = make_recursive_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 4, 4},
                                                 "trace-recursive-parent",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 3, 3},
                          "trace-recursive-child",
                          {}});
    auto grandchild = fixture.runtime->start_child(
        "tenant:runtime", child, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 2, 2},
                          "trace-recursive-grandchild",
                          {}});
    auto great_grandchild = fixture.runtime->start_child(
        "tenant:runtime", grandchild, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                          "trace-recursive-great-grandchild",
                          {}});
    auto leaf = fixture.runtime->start_child(
        "tenant:runtime", great_grandchild, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-recursive-leaf",
                          {}});

    EXPECT_EQ(child.snapshot().child_depth(), 1U);
    EXPECT_EQ(grandchild.snapshot().child_depth(), 2U);
    EXPECT_EQ(great_grandchild.snapshot().child_depth(), 3U);
    EXPECT_EQ(leaf.snapshot().child_depth(), 4U);

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 3, 0},
                              "trace-recursive-sibling-after-subtree-reservation",
                              {}});
        FAIL() << "Expected the reserved descendant quota to reject a sibling";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_COUNT");
    }

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", leaf, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                              "trace-recursive-overflow",
                              {}});
        FAIL() << "Expected recursive child depth to fail closed";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_DEPTH");
    }

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(grandchild.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(great_grandchild.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(leaf.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, GlobalChildQuotaRejectsFragmentationAndReleasesOnTerminal) {
    ProgramChildQuotaConfig quota;
    quota.max_active_children                  = 1;
    quota.max_active_children_per_owner        = 1;
    quota.max_pending_spawn_requests           = 4;
    quota.max_pending_spawn_requests_per_owner = 4;

    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, quota);
    const auto      linked = make_recursive_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 2, 2, 20, 0, 4, 4},
                                                 "trace-global-quota-parent",
                                                 {}});
    auto child =
        fixture.runtime->start_child("tenant:runtime", parent, linked.receipt, linked.child_version,
                                     ProgramInvocation{json::object(),
                                                       RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                                                       "trace-global-quota-child-a",
                                                       {}});

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                              "trace-global-quota-child-b",
                              {}});
        FAIL() << "Expected the global active-child quota to reject a fragmented sibling";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_QUOTA");
    }

    EXPECT_TRUE(child.cancel());
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);

    auto replacement =
        fixture.runtime->start_child("tenant:runtime", parent, linked.receipt, linked.child_version,
                                     ProgramInvocation{json::object(),
                                                       RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                                                       "trace-global-quota-child-c",
                                                       {}});
    EXPECT_EQ(replacement.snapshot().child_depth(), 1U);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(replacement.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, ChildResumeReclaimsGlobalQuotaAfterACompetingChildStops) {
    ProgramChildQuotaConfig quota;
    quota.max_active_children           = 1;
    quota.max_active_children_per_owner = 1;

    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, quota);
    const auto      interrupted_link =
        make_linked_child(fixture, "runtime-blocking", "runtime-interrupt");
    const auto active_link = make_linked_child(fixture, "runtime-blocking", "runtime-blocking");

    auto interrupted_parent =
        fixture.runtime->start("tenant:runtime", interrupted_link.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-quota-resume-parent",
                                                 {}});
    auto interrupted_child = fixture.runtime->start_child(
        "tenant:runtime", interrupted_parent, interrupted_link.receipt,
        interrupted_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-quota-resume-child",
                          {}});
    const auto interrupted = interrupted_child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    auto active_parent =
        fixture.runtime->start("tenant:runtime", active_link.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-quota-active-parent",
                                                 {}});
    auto active_child = fixture.runtime->start_child(
        "tenant:runtime", active_parent, active_link.receipt, active_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-quota-active-child",
                          {}});

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted_child.run_id(),
            resume_for(interrupted, json{{"decision", "approved"}}, "trace-quota-retry"));
        FAIL() << "Expected child resume to honor the active global quota";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_QUOTA");
    }

    EXPECT_TRUE(active_child.cancel());
    EXPECT_EQ(active_child.wait().status(), ProgramTerminalStatus::Cancelled);

    auto resumed = fixture.runtime->resume(
        "tenant:runtime", interrupted_child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-quota-retry"));
    EXPECT_EQ(resumed.wait().status(), ProgramTerminalStatus::Completed);

    EXPECT_TRUE(interrupted_parent.cancel());
    EXPECT_EQ(interrupted_parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_TRUE(active_parent.cancel());
    EXPECT_EQ(active_parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, InterruptedChildReleasesParentConcurrencyUntilItsResumeIsActive) {
    interrupt_calls.store(0);
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);

    auto parent_document                      = program_document("runtime-blocking");
    parent_document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 2;
    parent_document["declared_budget_requirements"][8]["maximum"] = 2;
    const auto parent_version = fixture.admit_document(std::move(parent_document));
    const auto interrupted_version =
        fixture.admit_document(parallel_map_child_document("runtime-interrupt"));
    const auto blocking_version =
        fixture.admit_document(parallel_map_child_document("runtime-blocking"));
    const auto child_budget = BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0};
    const auto interrupted_link =
        link_child_versions(fixture, parent_version, interrupted_version, child_budget);
    const auto blocking_link =
        link_child_versions(fixture, parent_version, blocking_version, child_budget);

    auto parent =
        fixture.runtime->start("tenant:runtime", parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 2},
                                                 "trace-parent-concurrency-reclaim",
                                                 {}});
    auto interrupted_child = fixture.runtime->start_child(
        "tenant:runtime", parent, interrupted_link.receipt, interrupted_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{3333, 333, 333, 1, 1, 6, 0, 0, 0},
                          "trace-interrupted-concurrency-child",
                          {}});
    const auto interrupted = interrupted_child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    auto active_child = fixture.runtime->start_child(
        "tenant:runtime", parent, blocking_link.receipt, blocking_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{3333, 333, 333, 1, 1, 6, 0, 0, 0},
                          "trace-active-concurrency-child",
                          {}});

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted_child.run_id(),
            resume_for(interrupted, json{{"decision", "approved"}}, "trace-concurrency-resume"));
        FAIL() << "Expected active sibling concurrency to block child resume";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_CONCURRENCY");
    }

    EXPECT_TRUE(active_child.cancel());
    EXPECT_EQ(active_child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto resumed = fixture.runtime->resume(
        "tenant:runtime", interrupted_child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-concurrency-resume"));
    EXPECT_EQ(resumed.wait().status(), ProgramTerminalStatus::Completed);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, DuplicateChildRecoveryReturnsTheExistingRun) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-duplicate",
                                                 {}});
    ProgramInvocation invocation{json::object(),
                                 RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                 "trace-child-duplicate",
                                 {}};
    auto              first = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                           linked.child_version, invocation);
    auto duplicate          = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                           linked.child_version, invocation);

    EXPECT_EQ(duplicate.run_id(), first.run_id());
    EXPECT_EQ(duplicate.snapshot().id(), first.snapshot().id());
    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(duplicate.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, InterruptedChildRemainsInFlightAndResumeJoinsDurably) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-interrupt");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-interrupted-child",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-interrupted",
                          {}});

    const auto interrupted = child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    const auto parent_after_interrupt = fixture.journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(parent_after_interrupt.has_value());
    ASSERT_EQ(parent_after_interrupt->children().size(), 1U);
    EXPECT_EQ(parent_after_interrupt->children().front().state, ProgramChildState::Dispatched);
    ASSERT_TRUE(parent_after_interrupt->children().front().terminal_result.has_value());
    EXPECT_EQ(parent_after_interrupt->children().front().terminal_result->status(),
              ProgramTerminalStatus::Interrupted);

    auto resumed = fixture.runtime->resume(
        "tenant:runtime", child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-child-resumed"));
    const auto result = resumed.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.snapshot().child_depth(), 1U);
    EXPECT_EQ(result.output()["channels"]["value"]["value"], "approved");
    const auto parent_after_resume = fixture.journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(parent_after_resume.has_value());
    ASSERT_EQ(parent_after_resume->children().size(), 1U);
    EXPECT_EQ(parent_after_resume->children().front().state, ProgramChildState::Completed);
    ASSERT_TRUE(parent_after_resume->children().front().terminal_result.has_value());
    EXPECT_EQ(parent_after_resume->children().front().terminal_result->status(),
              ProgramTerminalStatus::Completed);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}
// The parent's cancellation holds the process-wide transition lock while it cancels its children, and the cancellation of an
// interrupted child takes that lock to publish its Cancelled state: the lock must be handed down, not taken twice.
TEST(ProgramRuntimeTest, CancellingARunningParentCancelsItsInterruptedChildDurably) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-interrupt");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-cancels-interrupted-child",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-cancelled-with-parent",
                          {}});
    ASSERT_EQ(child.wait().status(), ProgramTerminalStatus::Interrupted);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto stored_child = fixture.journal->load("tenant:runtime", child.run_id());
    ASSERT_TRUE(stored_child.has_value());
    EXPECT_EQ(stored_child->continuation().state, ContinuationState::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, FailedDispatchIsRetriedFromPublishingBoundary) {
    auto            journal = std::make_shared<FailChildDispatchOnceJournal>();
    AdmittedRuntime fixture(2, {}, journal);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-completed");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-dispatch-recovery",
                                                 {}});
    ProgramInvocation child_invocation{json::object(),
                                       RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                       "trace-child-dispatch-recovery",
                                       {}};
    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, child_invocation),
                 ProgramDiagnosticError);

    const auto publishing = journal->load_sync("tenant:runtime", parent.run_id());
    ASSERT_TRUE(publishing.has_value());
    ASSERT_EQ(publishing->children().size(), 1U);
    EXPECT_EQ(publishing->children().front().state, ProgramChildState::Publishing);

    journal->allow_dispatch();
    auto recovered = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                  linked.child_version, child_invocation);
    EXPECT_EQ(recovered.wait().status(), ProgramTerminalStatus::Completed);
    const auto completed = journal->load_sync("tenant:runtime", parent.run_id());
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->children().size(), 1U);
    EXPECT_EQ(completed->children().front().state, ProgramChildState::Completed);
    ASSERT_TRUE(completed->children().front().terminal_result.has_value());

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, RecoverChildrenRetriesPublishingBoundaryWithoutDuplicateDispatch) {
    completed_calls.store(0);
    auto            journal = std::make_shared<FailChildDispatchOnceJournal>();
    AdmittedRuntime fixture(2, {}, journal);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-completed");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-recover-children",
                                                 {}});
    ProgramInvocation child_invocation{json::object(),
                                       RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                       "trace-child-recover-children",
                                       {}};

    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, child_invocation),
                 ProgramDiagnosticError);
    const auto publishing = journal->load_sync("tenant:runtime", parent.run_id());
    ASSERT_TRUE(publishing.has_value());
    ASSERT_EQ(publishing->children().size(), 1U);
    EXPECT_EQ(publishing->children().front().state, ProgramChildState::Publishing);

    journal->allow_dispatch();
    auto recovered = fixture.runtime->recover_children("tenant:runtime", parent.run_id());
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front().wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);

    const auto completed = journal->load_sync("tenant:runtime", parent.run_id());
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->children().size(), 1U);
    EXPECT_EQ(completed->children().front().state, ProgramChildState::Completed);
    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, DurableDslSpawnDispatchesTheLinkedChildAndJoinsItsResult) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_durable_spawn_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-durable-spawn",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output()["channels"]["value"]["value"], "completed");
    EXPECT_EQ(completed_calls.load(), 1U);

    const auto record   = parent.snapshot();
    const auto children = record.children();
    ASSERT_EQ(children.size(), 1U);
    const auto& child = children.front();
    EXPECT_EQ(child.link_id, linked.receipt.id());
    EXPECT_EQ(child.invocation.parent_run_id, parent.run_id());
    EXPECT_EQ(child.invocation.child_depth, 1U);
    EXPECT_EQ(child.state, ProgramChildState::Completed);
    ASSERT_TRUE(child.terminal_result.has_value());
    EXPECT_EQ(child.terminal_result->status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, DurableDslSpawnFailsClosedWhenItsBindingCannotBeResolved) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_durable_spawn_child(fixture);
    fixture.clear_child_bindings();

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-missing-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/body");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslSpawnRejectsAResolverReceiptForAnotherBinding) {
    completed_calls.store(0);
    AdmittedRuntime         fixture(2);
    const auto              linked     = make_durable_spawn_child(fixture);
    const ModuleLinkReceipt mismatched = ModuleLinkReceipt::create(ModuleLinkReceiptData{
        linked.receipt.owner_scope(), linked.receipt.parent_module_id(),
        linked.receipt.dependency_merkle_root(), "different-child",
        linked.receipt.child_program_version_id(), linked.receipt.child_bundle_id(),
        linked.receipt.child_input_contract_fingerprint(),
        linked.receipt.child_output_contract_fingerprint(), linked.receipt.granted_capabilities(),
        linked.receipt.granted_effects(), linked.receipt.budget()});
    fixture.bind_child(linked.parent_version, "child",
                       ProgramRuntimeChildBinding{mismatched, linked.child_version});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-mismatched-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslSpawnRejectsReceiptWhoseGuaranteeExceedsChildVersion) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Recorded);
    const auto linked = make_durable_spawn_child(fixture, "runtime-completed", "runtime-completed",
                                                 std::nullopt, ExecutionGuarantee::Recorded);
    ASSERT_EQ(linked.child_version.execution_guarantee(), ExecutionGuarantee::Recorded);

    const ModuleLinkReceipt forged = ModuleLinkReceipt::create(ModuleLinkReceiptData{
        linked.receipt.owner_scope(), linked.receipt.parent_module_id(),
        linked.receipt.dependency_merkle_root(), linked.receipt.child_name(),
        linked.receipt.child_program_version_id(), linked.receipt.child_bundle_id(),
        linked.receipt.child_input_contract_fingerprint(),
        linked.receipt.child_output_contract_fingerprint(), linked.receipt.granted_capabilities(),
        linked.receipt.granted_effects(), linked.receipt.budget(), ExecutionGuarantee::Strict});
    fixture.bind_child(linked.parent_version, "child",
                       ProgramRuntimeChildBinding{forged, linked.child_version});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-guarantee-mismatched-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_GUARANTEE");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslAwaitTimeoutCancelsAndRecordsItsChild) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked =
        make_durable_spawn_child(fixture, "runtime-completed", "runtime-blocking", 10);
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-durable-spawn-timeout",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");

    const auto pending          = parent.snapshot();
    const auto pending_children = pending.children();
    ASSERT_EQ(pending_children.size(), 1U);
    auto child =
        fixture.runtime->reconnect("tenant:runtime", pending_children.front().child_run_id);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto settled          = parent.snapshot();
    const auto settled_children = settled.children();
    ASSERT_EQ(settled_children.size(), 1U);
    EXPECT_EQ(settled_children.front().state, ProgramChildState::Cancelled);
    // Parent timeout and child cancellation publish through independent
    // scheduler paths.  The parent may snapshot the cancelled relation before
    // or after the child's matching terminal result is durably attached.
    if (const auto& terminal = settled_children.front().terminal_result) {
        EXPECT_EQ(terminal->status(), ProgramTerminalStatus::Cancelled);
        EXPECT_EQ(terminal->run_id(), pending_children.front().child_run_id);
    }
}
TEST(ProgramRuntimeTest, ParentCompletionCancelsAttachedChild) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto linked = make_linked_child(fixture, "runtime-short-blocking", "runtime-blocking");
    auto       parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-complete",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-complete",
                          {}});

    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, ParentTerminalReconnectRetainsDurableChildRecords) {
    AdmittedRuntime fixture(2);
    const auto linked = make_linked_child(fixture, "runtime-short-blocking", "runtime-blocking");
    auto       parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-reopen",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-reopen",
                          {}});

    ASSERT_EQ(parent.wait().status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto parent_id = parent.run_id();
    const auto child_id  = child.run_id();
    const auto before    = parent.snapshot();
    ASSERT_EQ(before.children().size(), 1U);
    EXPECT_EQ(before.children().front().child_run_id, child_id);

    fixture.recreate_catalog_and_runtime();
    auto       reconnected = fixture.runtime->reconnect("tenant:runtime", parent_id);
    const auto after       = reconnected.snapshot();
    ASSERT_EQ(after.children().size(), 1U);
    EXPECT_EQ(after.children().front().child_run_id, child_id);
    EXPECT_EQ(after.children().front().invocation.parent_run_id, parent_id);
    EXPECT_EQ(reconnected.wait().status(), ProgramTerminalStatus::Completed);
}
TEST(ProgramRuntimeTest, FailedChildPublicationReleasesParentReservation) {
    blocking_calls.store(0);
    AdmittedRuntime   fixture(2);
    const auto        linked = make_linked_child(fixture);
    ProgramInvocation occupied_invocation{json::object(),
                                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                          "trace-child-conflict",
                                          {}};
    occupied_invocation.requested_run_id = "run-child-conflict";
    auto occupied =
        fixture.runtime->start("tenant:runtime", linked.child_version, occupied_invocation);
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-reservation",
                                                 {}});

    ProgramInvocation conflict_invocation{json::object(),
                                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                          "trace-child-conflict",
                                          {}};
    conflict_invocation.requested_run_id = occupied.run_id();
    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, conflict_invocation),
                 std::exception);
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-after-conflict",
                          {}});

    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(occupied.cancel(), true);
    EXPECT_EQ(occupied.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, ParallelMapBindsInputAndReturnsOrderedBoundedChildOutputs) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      items = json::array({json{{"id", 1}}, json{{"id", 2}}, json{{"id", 3}}});
    auto            parent_document = parallel_map_document(
        json{{"literal", json::array({json{{"id", 1}}, json{{"id", 2}}, json{{"id", 3}}})}}, 3);
    parent_document["root"]["item_source"] = json{{"artifact", "input"}, {"field", "/items"}};
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 4;
    const auto linked                                             = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json{{"items", items}},
                                                 RunBudget{10000, 1000, 1000, 2, 4, 20, 0, 1, 3},
                                                 "trace-parallel-map-input-output",
                                                 {}});
    const auto result = parent.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    const auto output = result.output();
    ASSERT_TRUE(output.is_array());
    ASSERT_EQ(output.size(), 3U);
    EXPECT_EQ(output.at(0).get<std::string>(), "completed");
    EXPECT_EQ(output.at(1).get<std::string>(), "completed");
    EXPECT_EQ(output.at(2).get<std::string>(), "completed");
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
    EXPECT_EQ(completed_calls.load(), 3U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 3U);
    // Two workers claim items in ordinal order but register each child when it reaches
    // launch_child, so the record lists children in launch order: a worker that finishes item 0
    // can launch item 2 before the other worker has launched item 1. Output order is fixed by
    // the ordinal (asserted above); what this loop checks is that each child was bound to its own
    // item, so visit the children by the item they were bound to, not by their position.
    std::vector<std::size_t> by_item;
    for (std::size_t index = 0; index < children.size(); ++index) by_item.push_back(index);
    std::sort(by_item.begin(), by_item.end(), [&children](std::size_t lhs, std::size_t rhs) {
        return children[lhs].invocation.input.dump() < children[rhs].invocation.input.dump();
    });
    for (std::size_t index = 0; index < by_item.size(); ++index) {
        const auto& child = children[by_item[index]];
        EXPECT_EQ(child.state, ProgramChildState::Completed);
        const json expected_input{{"item", items.at(index)}};
        EXPECT_EQ(child.invocation.input, expected_input);
    }
}
TEST(ProgramRuntimeTest, ParallelMapCollectPreservesInputOrdinalOutputOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      items = json::array({json{{"id", 1}}, json{{"id", 2}}, json{{"id", 3}}});
    auto            parent_document = parallel_map_document(json{{"literal", items}}, 3, "collect");
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 4;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_echo_child_document()),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 2, 4, 20, 0, 1, 3},
                                                 "trace-parallel-map-collect-order",
                                                 {}});
    const auto result = parent.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message : "");
    ASSERT_TRUE(result.output().is_array());
    ASSERT_EQ(result.output().size(), items.size());
    for (std::size_t index = 0; index < items.size(); ++index)
        EXPECT_EQ(result.output().at(index), items.at(index));
    EXPECT_EQ(completed_calls.load(), 3U);
}

TEST(ProgramRuntimeTest, ParallelMapRejectsDynamicCollectionBeforeLaunchingChildren) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"artifact", "input"}, {"field", "/items"}}, 2);
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json{{"items", json::array({1, 2, 3})}},
                                               RunBudget{10000, 1000, 1000, 2, 1, 20, 0, 1, 2},
                                               "trace-parallel-map-oversized-input",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_BOUND");
    EXPECT_EQ(result.failure()->operation_id, "root");
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_TRUE(fixture.runtime->reconnect("tenant:runtime", result.run_id())
                    .snapshot()
                    .children()
                    .empty());
}
TEST(ProgramRuntimeTest, ParallelMapRejectsMissingInputArtifactFieldBeforeChildLaunch) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"artifact", "input"}, {"field", "/missing"}}, 1);
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-missing-input",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_SOURCE");
    EXPECT_TRUE(fixture.runtime->reconnect("tenant:runtime", result.run_id())
                    .snapshot()
                    .children()
                    .empty());
}

TEST(ProgramRuntimeTest, ParallelMapRejectsMissingChildOutputField) {
    AdmittedRuntime fixture(2);
    auto            parent_document = parallel_map_document(json{{"literal", json::array({1})}}, 1);
    parent_document["root"]["output_binding"]["from"]["field"] = "/missing";
    const auto linked                                          = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-missing-output",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_OUTPUT");
    ASSERT_EQ(
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children().size(),
        1U);
}
TEST(ProgramRuntimeTest, ParallelMapRejectsChildOutputContractMismatch) {
    AdmittedRuntime fixture(2);
    auto            parent_document = parallel_map_document(json{{"literal", json::array({1})}}, 1);
    auto            child_document  = parallel_map_child_document("runtime-completed");
    child_document["output_contract"]["schema"] =
        json{{"type", "object"}, {"required", json::array({"must"})}};
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(std::move(child_document)),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-output-contract",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_OUTPUT_CONTRACT");
    ASSERT_EQ(
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children().size(),
        1U);
}

TEST(ProgramRuntimeTest, ParallelMapBuildsNestedArrayInputBindings) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({json{{"id", 7}}})}}, 1);
    parent_document["root"]["input_binding"]["to"]["field"] = "/payload/0";
    const auto linked                                       = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-array-input",
                                               {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    const auto children =
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children();
    ASSERT_EQ(children.size(), 1U);
    const json expected_input{{"payload", json::array({json{{"id", 7}}})}};
    EXPECT_EQ(children.front().invocation.input, expected_input);
}

TEST(ProgramRuntimeTest, ParallelMapCollectWaitsForEveryChildFailure) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3, "collect");
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 4;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_child_document("runtime-failing")),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 3, 4, 20, 0, 1, 3},
                                                 "trace-parallel-map-collect",
                                                 {}});
    const auto result = parent.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.failure()->operation_id, "root");
    EXPECT_EQ(result.failure()->witness["item_index"], 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 3U);
    for (const auto& child : children)
        EXPECT_EQ(child.state, ProgramChildState::Failed);
}

TEST(ProgramRuntimeTest, ParallelMapFailFastStopsLaunchingAfterFirstFailedItem) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3, "fail_fast");
    parent_document["root"]["max_in_flight"]                      = 1;
    parent_document["declared_budget_requirements"][3]["minimum"] = 1;
    parent_document["declared_budget_requirements"][3]["maximum"] = 1;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_child_document("runtime-failing")),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 3},
                                               "trace-parallel-map-fail-fast",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(result.failure()->witness["item_index"], 0U);
    const auto children =
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children();
    ASSERT_EQ(children.size(), 1U);
    EXPECT_EQ(children.front().state, ProgramChildState::Failed);
}

TEST(ProgramRuntimeTest, ParallelMapFailFastCancelsActiveSiblingAfterFailure) {
    map_fail_first_calls.store(0);
    map_fail_first_active.store(0);
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3, "fail_fast");
    parent_document["declared_budget_requirements"][3]["maximum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 3;
    const auto linked                                             = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-map-fail-first")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 2, 3, 20, 0, 1, 3},
                                                 "trace-parallel-map-fail-fast-active",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "NG.TaskFailed");
    EXPECT_EQ(map_fail_first_calls.load(), 2U);
    EXPECT_EQ(map_fail_first_active.load(), 0U);

    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    std::size_t failed    = 0;
    std::size_t cancelled = 0;
    for (const auto& child : children) {
        failed += child.state == ProgramChildState::Failed;
        cancelled += child.state == ProgramChildState::Cancelled;
    }
    EXPECT_EQ(failed, 1U);
    EXPECT_EQ(cancelled, 1U);
}

TEST(ProgramRuntimeTest, ParallelMapRejectsOutputBeyondItsImmutableByteBound) {
    AdmittedRuntime fixture(2);
    const auto      linked = link_child_versions(
        fixture,
        fixture.admit_document(
            parallel_map_document(json{{"literal", json::array({1})}}, 1, "fail_fast", 2)),
        fixture.admit("runtime-completed"), BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-output-cap",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_OUTPUT");
    EXPECT_EQ(result.failure()->operation_id, "root");
    EXPECT_TRUE(result.output().is_array());
    EXPECT_TRUE(result.output().empty());
    const auto record = fixture.journal->load("tenant:runtime", result.run_id());
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->terminal_result().has_value());
    EXPECT_TRUE(record->terminal_result()->output().is_array());
    EXPECT_TRUE(record->terminal_result()->output().empty());
}

TEST(ProgramRuntimeTest, ParallelMapProvidesBoundedChildOverlapAndCancelsActiveChildren) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    overlap_barrier                = std::make_shared<std::barrier<>>(2);
    overlap_ready                  = std::make_shared<std::promise<void>>();
    auto            overlap_future = overlap_ready->get_future();
    AdmittedRuntime fixture(4);
    auto parent_document = parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3);
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 3;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_child_document("runtime-barrier")),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 3, 3, 20, 0, 1, 3},
                                                 "trace-parallel-map-overlap-cancel",
                                                 {}});
    ASSERT_EQ(overlap_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(blocking_calls.load(), 2U);
    ASSERT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_active.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 2U);
    ASSERT_TRUE(parent.cancel());

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(blocking_active.load(), 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    for (const auto& child : children)
        EXPECT_EQ(child.state, ProgramChildState::Cancelled);
    overlap_barrier.reset();
    overlap_ready.reset();
}
TEST(ProgramRuntimeTest, ParallelMapLaunchesNextWindowAfterPriorCompletion) {
    window_calls.store(0);
    window_active.store(0);
    window_peak.store(0);
    window_completed.store(0);
    window_second_started_before_completion.store(0);
    window_first_barrier = std::make_shared<std::barrier<>>(2);
    AdmittedRuntime fixture(4);
    auto parent_document = parallel_map_document(json{{"literal", json::array({1, 2, 3, 4})}}, 4);
    parent_document["declared_budget_requirements"][3]["maximum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 5;
    parent_document["declared_budget_requirements"][5]["maximum"] = 20;
    auto child_document = parallel_map_child_document("runtime-windowed");
    child_document["declared_budget_requirements"][0]["maximum"] = 2000;
    child_document["declared_budget_requirements"][1]["maximum"] = 200;
    child_document["declared_budget_requirements"][2]["maximum"] = 200;
    child_document["declared_budget_requirements"][5]["maximum"] = 4;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(std::move(child_document)),
                            BudgetLimits{2000, 200, 200, 1, 1, 4, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 2, 5, 20, 0, 1, 4},
                                               "trace-parallel-map-windowed",
                                               {}});

    ASSERT_FALSE(result.failure().has_value())
        << result.failure()->code << ": " << result.failure()->message;
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(window_calls.load(), 4U);
    EXPECT_EQ(window_completed.load(), 2U);
    EXPECT_EQ(window_active.load(), 0U);
    EXPECT_EQ(window_peak.load(), 2U);
    EXPECT_EQ(window_second_started_before_completion.load(), 0U);
    window_first_barrier.reset();
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
namespace {
struct ChildSynthesisFixture {
    std::shared_ptr<BlockAfterJavaScriptResultJournal> journal =
        std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime                  fixture{2, {}, journal, {}, ExecutionGuarantee::Strict, true};
    std::shared_ptr<ProgramCompiler> compiler = std::make_shared<ProgramCompiler>(
        fixture.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    RunBudget     child_budget{1000, 10, 10, 1, 1, 2, 0, 0, 0};
    ProgramSource child_source = [] {
        auto text = javascript_runtime_source("runtime-completed", "");
        text.resize(text.find("export function* main"));
        return ProgramSource::from_javascript("reviewed-child.js", std::move(text));
    }();
    std::optional<ProgramVersion>                         version;
    std::optional<ProgramHandle>                          handle;
    std::optional<ProgramHandoff>                         hold;
    std::optional<ProgramChildSynthesisParent>            parent;
    std::string                                           reservation_head;
    RunBudget                                             reservation_remaining;
    unsigned                                              reservations       = 0;
    unsigned                                              semantic_calls     = 0;
    unsigned                                              admissions         = 0;
    bool                                                  semantic_accept    = true;
    std::function<void()>                                 semantic_hook;
    std::string                                           semantic_validator = digest('b');
    std::string                                           semantic_contract  = digest('c');
    std::function<void(ProgramSynthesisReservationData&)> alter_reservation;
    std::function<void(ProgramAdmission&)>                alter_admission;

    std::optional<ProgramChildSynthesisGrant> trusted_grant;
    ChildSynthesisFixture(std::shared_ptr<ProgramTransitionStore> backend          = {},
                          bool                                    durable          = false,
                          std::shared_ptr<ProgramStore>           program_store    = {},
                          std::shared_ptr<CheckpointStore>        checkpoint_store = {})
        : journal(std::make_shared<BlockAfterJavaScriptResultJournal>(std::move(backend))) {
        completed_calls.store(0);
        if (program_store)
            fixture.catalog = std::make_shared<ProgramCatalog>(
                CatalogConfig{std::move(program_store), fixture.registry, fixture.engines,
                              "program-runtime-test/v1"});
        if (checkpoint_store) fixture.checkpoints = std::move(checkpoint_store);
        // The current compiler conservatively labels arbitrary generator
        // control Unmanaged. The reviewed declaration-only child stays Strict.
        fixture.profile =
            AdmittedRuntime::make_profile(fixture.registry, ExecutionGuarantee::Unmanaged, true);
        fixture.policy =
            AdmittedRuntime::make_policy(fixture.profile, ExecutionGuarantee::Unmanaged, true);
        // Synthesis/recovery is bounded by the parent run budget, not an incidental
        // per-operation latency cap; dedicated await tests exercise deadlines.
        const auto source = ProgramSource::from_javascript(
            "synthesis-parent.js", javascript_runtime_source("runtime-completed", durable ? R"JS(
                yield ng.checkpoint({ready: true}, "synthesis:ready");
                return yield ng.await(ng.spawn("generated-child", {}, "generated:spawn"), undefined, "generated:await");
            )JS"
                                                                                          : R"JS(
                yield ng.checkpoint({ready: true}, "synthesis:ready");
                return {done: true};
            )JS"));
        const RunBudget ceiling{60000, 1000, 1000, 2, 16, 20, 3, 2, 8};
        const RunBudget floor{1, 0, 0, 1, 1, 1, 0, 0, 0};
        auto            bundle = compiler->compile(source, ProgramBudgetBounds{floor, ceiling});
        try {
            version = fixture.catalog->admit(
                bundle, ProgramAdmission{"tenant:runtime", fixture.profile, fixture.policy, {}});
        } catch (const ProgramAdmissionError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + ": " + diagnostic.message;
            }
            throw std::runtime_error(message);
        }
        if (durable) {
            RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, journal, 2};
            config.child_synthesis_gateway = std::make_shared<ProgramSynthesisGateway>(gateway());
            config.child_synthesis_grant_resolver =
                [this](std::string_view owner, std::string_view run,
                       std::string_view id) -> std::optional<ProgramChildSynthesisGrant> {
                if (trusted_grant && owner == trusted_grant->data().owner_scope &&
                    run == trusted_grant->data().parent_run_id && id == trusted_grant->id())
                    return trusted_grant;
                return std::nullopt;
            };
            fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));
        }
        handle = fixture.runtime->start(
            "tenant:runtime", *version,
            ProgramInvocation{json::object(), ceiling, "child-synthesis-parent", {}});
        if (!journal->wait_for_result(std::chrono::seconds(2))) {
            journal->release_result();
            throw std::runtime_error("Parent did not reach synthesis checkpoint");
        }
        hold.emplace(handle->next_handoff());
        journal->release_result();
        (void)hold->value();
        auto run     = handle->snapshot();
        auto lineage = journal->load_run_lineage_sync("tenant:runtime", handle->run_id());
        if (!lineage) throw std::runtime_error("Missing parent lineage");
        auto generation = journal->load_generation_sync("tenant:runtime", lineage->lineage_id(),
                                                   lineage->active_generation());
        if (!generation) throw std::runtime_error("Missing parent generation");
        parent.emplace(
            ProgramChildSynthesisParent{*version, std::move(run), *lineage, *generation});
        if (durable) trusted_grant = ProgramChildSynthesisGrant::create(grant_data());
        reservation_head      = lineage->id();
        reservation_remaining = lineage->remaining_budget();
    }
    ~ChildSynthesisFixture() {
        if (handle) handle->cancel();
        journal->release_result();
        hold.reset();
        if (handle) (void)handle->wait();
    }
    ProgramSynthesisProposalData proposal_data() const {
        ProgramSynthesisProposalData data;
        data.owner_scope      = "tenant:runtime";
        data.lineage_id       = parent->lineage.lineage_id();
        data.parent_run_id    = parent->run.run_id();
        data.source           = child_source;
        data.requested_budget = child_budget;
        data.created_at_ms    = parent->run.updated_at_ms();
        return data;
    }
    ProgramChildSynthesisGrantData grant_data() const {
        ProgramChildSynthesisGrantData data;
        data.owner_scope                 = "tenant:runtime";
        data.parent_run_id               = parent->run.run_id();
        data.parent_program_version_id   = version->id();
        data.parent_policy_fingerprint   = version->policy_snapshot().fingerprint();
        data.lineage_id                  = parent->lineage.lineage_id();
        data.expected_lineage_head_id    = parent->lineage.id();
        data.template_identity           = digest('a');
        data.reviewed_source_identity    = program_synthesis_source_identity(child_source);
        data.semantic_validator_identity = digest('b');
        data.semantic_contract_identity  = digest('c');
        data.child_budget_ceiling        = child_budget;
        return data;
    }
    ProgramSynthesisGateway gateway() {
        ProgramSynthesisGatewayConfig config{
            compiler,
            fixture.catalog,
            {},
            [this](const auto&, const auto&, const auto&) {
                ++admissions;
                auto admission =
                    ProgramAdmission{"tenant:runtime", fixture.profile, fixture.policy, {}};
                if (alter_admission) alter_admission(admission);
                return admission;
            },
            1024 * 1024,
            [this](const auto&, const auto&, const auto&) {
                ++semantic_calls;
                if (semantic_hook) semantic_hook();
                return ProgramSynthesisSemanticDecision{semantic_validator, semantic_contract,
                                                        semantic_accept,
                                                        json{{"accepted", semantic_accept}}};
            }};
        config.reserve_child = [this](const ProgramSynthesisProposal&           proposal,
                                      const ProgramChildSynthesisAuthorization& authorization) {
            // Model the host's compare-and-swap boundary. SQLite publication
            // and its crash recovery belong to the subsequent integration slice.
            if (reservation_head != authorization.data().source_lineage_head_id) {
                throw ProgramChildSynthesisError("P_CHILD_SYNTHESIS_RESERVATION", "stale host CAS");
            }
            auto before = reservation_remaining;
            auto after  = before;
            --after.max_dynamic_compiles;
            ProgramSynthesisReservationData data{proposal.id(),
                                                 parent->lineage.lineage_id(),
                                                 parent->lineage.id(),
                                                 digest('f'),
                                                 before,
                                                 after};
            if (alter_reservation) alter_reservation(data);
            auto result = ProgramSynthesisReservation::create(std::move(data));
            ++reservations;
            reservation_head      = result.data().reserved_lineage_head_id;
            reservation_remaining = result.data().remaining_after_reservation;
            return result;
        };
        return ProgramSynthesisGateway(std::move(config));
    }
};

void expect_child_synthesis_error(const std::function<void()>& call, std::string_view code) {
    try {
        call();
        FAIL() << "Expected " << code;
    } catch (const ProgramChildSynthesisError& error) {
        EXPECT_EQ(error.code(), code);
    }
}
}  // namespace

TEST(ProgramChildSynthesis, AuthorizesExactHostGrantWithoutChangingParent) {
    ChildSynthesisFixture test;
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    const auto            grant    = ProgramChildSynthesisGrant::create(test.grant_data());
    const auto authorization = authorize_program_child_synthesis(proposal, grant, *test.parent);
    EXPECT_EQ(authorization.data().proposal_id, proposal.id());
    EXPECT_EQ(authorization.data().grant_id, grant.id());
    EXPECT_EQ(authorization.data().source_lineage_head_id, test.parent->lineage.id());
    EXPECT_EQ(authorization.data().child_budget, test.child_budget);
    EXPECT_EQ(authorize_program_child_synthesis(proposal, grant, *test.parent).id(),
              authorization.id());
    EXPECT_EQ(test.handle->snapshot().id(), test.parent->run.id());
    EXPECT_EQ(test.journal->load_run_lineage_sync("tenant:runtime", test.handle->run_id())->id(),
              test.parent->lineage.id());
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, AdmitsChildAtRequestedCeilingWithoutDispatch) {
    ChildSynthesisFixture test;
    const auto            result = test.gateway().synthesize_child(
        ProgramSynthesisProposal::create(test.proposal_data()),
        ProgramChildSynthesisGrant::create(test.grant_data()), *test.parent);
    EXPECT_EQ(test.reservations, 1U);
    EXPECT_EQ(test.semantic_calls, 1U);
    EXPECT_EQ(test.admissions, 1U);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_EQ(result.synthesis.reservation.data().remaining_after_reservation.max_dynamic_compiles,
              result.authorization.data().parent_remaining.max_dynamic_compiles - 1);
    const std::map<std::string, std::uint64_t> ceilings{
        {"wall_time_ms", 1000},      {"model_tokens", 10},          {"monetary_microunits", 10},
        {"max_concurrency", 1},      {"max_program_operations", 1}, {"max_core_steps", 2},
        {"max_dynamic_compiles", 0}, {"max_child_depth", 0},        {"max_total_children", 0}};
    for (const auto& budget : result.synthesis.bundle.declared_budget_requirements()) {
        EXPECT_EQ(budget.maximum, ceilings.at(budget.resource));
    }
}

TEST(ProgramChildSynthesis, RejectsForeignOwnerAndRunBeforeReservation) {
    ChildSynthesisFixture test;
    const auto            grant   = ProgramChildSynthesisGrant::create(test.grant_data());
    auto                  request = test.proposal_data();
    request.owner_scope           = "tenant:foreign";
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(request), grant,
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_OWNER");
    request               = test.proposal_data();
    request.parent_run_id = "another-run";
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(request), grant,
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_GENERATION");
    EXPECT_EQ(test.reservations, 0U);
    EXPECT_EQ(test.admissions, 0U);
}

TEST(ProgramChildSynthesis, RejectsStaleHeadPolicyAndGeneration) {
    ChildSynthesisFixture test;
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    for (const auto field : {0, 1, 2}) {
        auto data = test.grant_data();
        if (field == 0) data.expected_lineage_head_id = digest('d');
        if (field == 1) data.parent_policy_fingerprint = digest('d');
        if (field == 2) data.parent_program_version_id = digest('d');
        expect_child_synthesis_error(
            [&] {
                test.gateway().synthesize_child(proposal, ProgramChildSynthesisGrant::create(data),
                                                *test.parent);
            },
            "P_CHILD_SYNTHESIS_GENERATION");
    }
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, GrantCannotMoveBetweenRunsOfTheSameVersion) {
    ChildSynthesisFixture first;
    ChildSynthesisFixture second;
    ASSERT_EQ(first.version->id(), second.version->id());
    const auto proposal = ProgramSynthesisProposal::create(second.proposal_data());
    const auto grant    = ProgramChildSynthesisGrant::create(first.grant_data());
    expect_child_synthesis_error(
        [&] { second.gateway().synthesize_child(proposal, grant, *second.parent); },
        "P_CHILD_SYNTHESIS_GENERATION");
    EXPECT_EQ(second.reservations, 0U);
}

TEST(ProgramChildSynthesis, RejectsUnreviewedSourceAndSourceByteLimit) {
    ChildSynthesisFixture test;
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    auto                  data     = test.grant_data();
    data.reviewed_source_identity  = digest('d');
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(proposal, ProgramChildSynthesisGrant::create(data),
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_SOURCE");
    data                  = test.grant_data();
    data.max_source_bytes = proposal.source().serialize_canonical().size() - 1;
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(proposal, ProgramChildSynthesisGrant::create(data),
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_SOURCE");
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, SourceReviewIncludesSealedModulesAndCountLimit) {
    ChildSynthesisFixture test;
    const auto text   = javascript_runtime_source("runtime-completed", "return {ok: true};");
    const auto source = ProgramSource::from_javascript(
        "reviewed-child.js", text, {{"reviewed:helper", digest('1')}}, {},
        {{"reviewed:helper", digest('1'), "export const v=1;"}});
    EXPECT_NE(program_synthesis_source_identity(source),
              program_synthesis_source_identity(test.child_source));
    auto request                  = test.proposal_data();
    request.source                = source;
    auto data                     = test.grant_data();
    data.reviewed_source_identity = program_synthesis_source_identity(source);
    data.max_sealed_modules       = 0;
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(request),
                                            ProgramChildSynthesisGrant::create(data), *test.parent);
        },
        "P_CHILD_SYNTHESIS_SOURCE");
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, RequestAndHostGrantCannotWidenParentAuthority) {
    ChildSynthesisFixture test;
    for (const bool capability : {false, true}) {
        auto request = test.proposal_data();
        auto grant   = test.grant_data();
        if (capability) {
            request.requested_capabilities = {"foreign.capability"};
            grant.allowed_capabilities     = request.requested_capabilities;
        } else {
            request.requested_effects = {"foreign.effect"};
            grant.allowed_effects     = request.requested_effects;
        }
        expect_child_synthesis_error(
            [&] {
                test.gateway().synthesize_child(ProgramSynthesisProposal::create(request),
                                                ProgramChildSynthesisGrant::create(grant),
                                                *test.parent);
            },
            "P_CHILD_SYNTHESIS_AUTHORITY");
    }
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, ChecksEveryHostBudgetDimensionBeforeReservation) {
    ChildSynthesisFixture test;
    const auto            grant = ProgramChildSynthesisGrant::create(test.grant_data());
    const std::vector<std::function<void(RunBudget&)>> increases{
        [](auto& b) { ++b.wall_time_ms; },           [](auto& b) { ++b.model_tokens; },
        [](auto& b) { ++b.monetary_microunits; },    [](auto& b) { ++b.max_concurrency; },
        [](auto& b) { ++b.max_program_operations; }, [](auto& b) { ++b.max_core_steps; },
        [](auto& b) { ++b.max_dynamic_compiles; },   [](auto& b) { ++b.max_child_depth; },
        [](auto& b) { ++b.max_total_children; }};
    for (std::size_t i = 0; i < increases.size(); ++i) {
        SCOPED_TRACE(i);
        auto request = test.proposal_data();
        increases[i](request.requested_budget);
        expect_child_synthesis_error(
            [&] {
                test.gateway().synthesize_child(ProgramSynthesisProposal::create(request), grant,
                                                *test.parent);
            },
            "P_CHILD_SYNTHESIS_BUDGET");
    }
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, AdmissionCannotGiveTheChildUnrequestedAuthority) {
    for (const auto field : {0, 1, 2}) {
        ChildSynthesisFixture test;
        const auto versions_before = test.fixture.store->list_versions("tenant:runtime").size();
        test.alter_admission       = [&](ProgramAdmission& admission) {
            PolicySnapshotBuilder builder;
            builder.id("overbroad-child-policy")
                .semantic_version("1.0.0")
                .owner_scope("tenant:runtime")
                .admission_profile(test.fixture.profile)
                .budget_ceiling(test.fixture.policy.budget_ceiling())
                .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
            if (field == 0) builder.allow_capability("unused.capability");
            if (field == 1) builder.allow_effect("unused.effect");
            if (field == 2) builder.allow_module_digest(digest('e'));
            admission.policy = std::move(builder).build();
        };
        expect_child_synthesis_error(
            [&] {
                test.gateway().synthesize_child(
                    ProgramSynthesisProposal::create(test.proposal_data()),
                    ProgramChildSynthesisGrant::create(test.grant_data()), *test.parent);
            },
            "P_CHILD_SYNTHESIS_AUTHORITY");
        EXPECT_EQ(test.fixture.store->list_versions("tenant:runtime").size(), versions_before);
    }
}

TEST(ProgramChildSynthesis, ReservesRoomForCompileChildAndDescendantDepth) {
    ChildSynthesisFixture test;
    auto                  grant = test.grant_data();
    grant.child_budget_ceiling  = test.parent->lineage.remaining_budget();
    for (const auto field : {0, 1, 2}) {
        auto       request   = test.proposal_data();
        const auto remaining = test.parent->lineage.remaining_budget();
        if (field == 0)
            request.requested_budget.max_dynamic_compiles = remaining.max_dynamic_compiles;
        if (field == 1) request.requested_budget.max_child_depth = remaining.max_child_depth;
        if (field == 2) request.requested_budget.max_total_children = remaining.max_total_children;
        expect_child_synthesis_error(
            [&] {
                test.gateway().synthesize_child(ProgramSynthesisProposal::create(request),
                                                ProgramChildSynthesisGrant::create(grant),
                                                *test.parent);
            },
            "P_CHILD_SYNTHESIS_BUDGET");
    }
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, RejectsZeroExecutionBudgetAndCompiledGuaranteeDowngrade) {
    ChildSynthesisFixture test;
    test.child_source = ProgramSource::from_javascript(
        "reviewed-generator.js",
        javascript_runtime_source("runtime-completed", "return {ok: true};"));
    auto grant = test.grant_data();
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(test.proposal_data()),
                                            ProgramChildSynthesisGrant::create(grant),
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_GUARANTEE");
    auto request                                    = test.proposal_data();
    request.requested_budget.max_program_operations = 0;
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(request),
                                            ProgramChildSynthesisGrant::create(test.grant_data()),
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_BUDGET");
    EXPECT_EQ(test.reservations, 1U);
    EXPECT_EQ(test.semantic_calls, 0U);
    EXPECT_EQ(test.admissions, 0U);
}

TEST(ProgramChildSynthesis, RejectsMismatchedReservationBeforeEvaluatingInvalidSource) {
    ChildSynthesisFixture test;
    test.child_source = ProgramSource::from_javascript("reviewed-invalid.js", "not JavaScript {");
    test.alter_reservation = [](auto& data) { data.source_lineage_head_id = digest('e'); };
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(test.proposal_data()),
                                            ProgramChildSynthesisGrant::create(test.grant_data()),
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_RESERVATION");
    EXPECT_EQ(test.reservations, 1U);
    EXPECT_EQ(test.semantic_calls, 0U);
    EXPECT_EQ(test.admissions, 0U);
}

TEST(ProgramChildSynthesis, RejectsReservationThatConsumesRequiredChildCapacity) {
    for (const bool depth : {false, true}) {
        ChildSynthesisFixture test;
        test.alter_reservation = [depth](auto& data) {
            if (depth)
                data.remaining_after_reservation.max_child_depth = 0;
            else
                data.remaining_after_reservation.max_total_children = 0;
        };
        expect_child_synthesis_error(
            [&] {
                test.gateway().synthesize_child(
                    ProgramSynthesisProposal::create(test.proposal_data()),
                    ProgramChildSynthesisGrant::create(test.grant_data()), *test.parent);
            },
            "P_CHILD_SYNTHESIS_RESERVATION");
        EXPECT_EQ(test.admissions, 0U);
    }
}

TEST(ProgramChildSynthesis, SemanticRejectionOccursAfterReservationAndNeverAdmits) {
    ChildSynthesisFixture test;
    test.semantic_accept = false;
    EXPECT_THROW(test.gateway().synthesize_child(
                     ProgramSynthesisProposal::create(test.proposal_data()),
                     ProgramChildSynthesisGrant::create(test.grant_data()), *test.parent),
                 ProgramSynthesisValidationError);
    EXPECT_EQ(test.reservations, 1U);
    EXPECT_EQ(test.semantic_calls, 1U);
    EXPECT_EQ(test.admissions, 0U);
    EXPECT_EQ(completed_calls.load(), 0U);
    const auto after_rejection = test.reservation_remaining;
    EXPECT_EQ(after_rejection.max_dynamic_compiles,
              test.parent->lineage.remaining_budget().max_dynamic_compiles - 1);
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(test.proposal_data()),
                                            ProgramChildSynthesisGrant::create(test.grant_data()),
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_RESERVATION");
    EXPECT_EQ(test.reservation_remaining, after_rejection);
    EXPECT_EQ(test.reservations, 1U);
}

TEST(ProgramChildSynthesis, ChildOnlyGatewayHasNoUncheckedSuccessorFallback) {
    ChildSynthesisFixture test;
    EXPECT_THROW(test.gateway().synthesize(ProgramSynthesisProposal::create(test.proposal_data())),
                 std::invalid_argument);
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, SameSourceCannotSwitchItsRegistryBeforeSemanticValidation) {
    ChildSynthesisFixture test;
    test.compiler =
        std::make_shared<ProgramCompiler>(runtime_registry(ExecutionGuarantee::Unmanaged),
                                          ProgramCompilerConfig{"program-runtime-test/v1"});
    expect_child_synthesis_error(
        [&] {
            test.gateway().synthesize_child(ProgramSynthesisProposal::create(test.proposal_data()),
                                            ProgramChildSynthesisGrant::create(test.grant_data()),
                                            *test.parent);
        },
        "P_CHILD_SYNTHESIS_REGISTRY");
    EXPECT_EQ(test.reservations, 1U);
    EXPECT_EQ(test.semantic_calls, 0U);
    EXPECT_EQ(test.admissions, 0U);
}

TEST(ProgramChildSynthesis, SemanticAcceptanceCannotSwitchValidatorOrContract) {
    for (const bool validator : {false, true}) {
        ChildSynthesisFixture test;
        if (validator)
            test.semantic_validator = digest('e');
        else
            test.semantic_contract = digest('e');
        expect_child_synthesis_error(
            [&] {
                test.gateway().synthesize_child(
                    ProgramSynthesisProposal::create(test.proposal_data()),
                    ProgramChildSynthesisGrant::create(test.grant_data()), *test.parent);
            },
            "P_CHILD_SYNTHESIS_SEMANTICS");
        EXPECT_EQ(test.reservations, 1U);
        EXPECT_EQ(test.semantic_calls, 1U);
        EXPECT_EQ(test.admissions, 0U);
    }
}

TEST(ProgramChildSynthesis, ANewGrantCannotAuthorizeATerminalParent) {
    ChildSynthesisFixture test;
    test.hold.reset();
    ASSERT_EQ(test.handle->wait().status(), ProgramTerminalStatus::Completed);
    test.parent->run     = test.handle->snapshot();
    test.parent->lineage = *test.journal->load_run_lineage_sync("tenant:runtime", test.handle->run_id());
    const auto proposal  = ProgramSynthesisProposal::create(test.proposal_data());
    const auto grant     = ProgramChildSynthesisGrant::create(test.grant_data());
    expect_child_synthesis_error(
        [&] { test.gateway().synthesize_child(proposal, grant, *test.parent); },
        "P_CHILD_SYNTHESIS_GENERATION");
    EXPECT_EQ(test.reservations, 0U);
}

TEST(ProgramChildSynthesis, DynamicCompileBudgetRequiresGeneratorAndExplicitHostBounds) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    ProgramCompiler compiler(fixture.registry, {"program-runtime-test/v1"});
    const auto      body   = javascript_runtime_source("runtime-completed", "return {ok: true};");
    const auto      source = ProgramSource::from_javascript("compile-grant.js", body);
    auto            budget = javascript_budget(1, 4, 1, 1);
    budget.max_dynamic_compiles = 2;
    const auto bundle           = compiler.compile(source, budget);
    const auto version          = fixture.catalog->admit(
        bundle, ProgramAdmission{"tenant:runtime", fixture.profile, fixture.policy, {}});
    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), budget, "unused-compile-grant", {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    const auto define_only = ProgramSource::from_javascript(
        "define-only.js", body.substr(0, body.find("export function* main")));
    EXPECT_THROW(compiler.compile(define_only, budget), ProgramCompileError);
    const auto default_version = fixture.admit_javascript(body);
    EXPECT_THROW(
        fixture.runtime->start("tenant:runtime", default_version,
                               ProgramInvocation{json::object(), budget, "ungranted-compile", {}}),
        ProgramDiagnosticError);
}

TEST(ProgramSynthesisGateway, RequiresSemanticValidatorBeforeAcceptingProposals) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    ProgramSynthesisGatewayConfig config;
    config.compiler = std::make_shared<ProgramCompiler>(
        fixture.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    config.catalog = fixture.catalog;
    config.reserve = [](const ProgramSynthesisProposal&) -> ProgramSynthesisReservation {
        throw std::logic_error("reservation must not be called");
    };
    config.admission = [](const ProgramSynthesisProposal&, const ProgramBundle&,
                          const ProgramSynthesisReservation&) -> ProgramAdmission {
        throw std::logic_error("admission must not be called");
    };
    EXPECT_THROW(ProgramSynthesisGateway(std::move(config)), std::invalid_argument);
}

TEST(ProgramSynthesisGateway, ReservesBeforeCompilingAndAdmitsExactSuccessor) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    auto compiler = std::make_shared<ProgramCompiler>(
        fixture.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    const auto source = ProgramSource::from_javascript(
        "generated-successor.js",
        javascript_runtime_source("runtime-completed", "return {ok: true};"));
    ProgramSynthesisProposalData proposal_data;
    proposal_data.owner_scope = "tenant:runtime";
    proposal_data.lineage_id = digest('1');
    proposal_data.parent_run_id = "parent-run";
    proposal_data.source = source;
    proposal_data.requested_budget = javascript_budget(1, 1, 0, 0);
    proposal_data.created_at_ms = 1;
    const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));

    bool reserved = false;
    ProgramSynthesisGateway gateway({
        compiler,
        fixture.catalog,
        [&](const ProgramSynthesisProposal& value) {
            reserved = true;
            auto before = javascript_budget(1, 1, 0, 0);
            before.max_dynamic_compiles = 1;
            auto after = before;
            after.max_dynamic_compiles = 0;
            return ProgramSynthesisReservation::create(
                {value.id(), value.data().lineage_id, digest('2'), digest('3'),
                 before, after});
        },
        [&](const ProgramSynthesisProposal&, const ProgramBundle&,
            const ProgramSynthesisReservation&) {
            EXPECT_TRUE(reserved);
            return ProgramAdmission{"tenant:runtime", fixture.profile,
                                    fixture.policy, {}};
        },
        1024 * 1024,
        [](const ProgramSynthesisProposal&, const ProgramBundle& bundle,
           const ProgramSynthesisReservation&) {
            const auto definitions = bundle.sealed_core_definitions();
            return ProgramSynthesisSemanticDecision{
                digest('7'), digest('8'),
                definitions.size() == 1 && definitions.front().name == "main",
                json{{"core_count", definitions.size()}}};
        },
        1024 * 1024});
    const auto result = gateway.synthesize(proposal);
    EXPECT_TRUE(reserved);
    EXPECT_EQ(result.receipt.data().proposal_id, proposal.id());
    EXPECT_EQ(result.receipt.data().bundle_id, result.bundle.id());
    EXPECT_EQ(result.receipt.data().program_version_id, result.version.id());
    EXPECT_TRUE(result.validation.data().accepted);
    EXPECT_EQ(result.validation.data().proposal_id, proposal.id());
    EXPECT_EQ(result.validation.data().reservation_id, result.reservation.id());
    EXPECT_EQ(result.validation.data().bundle_id, result.bundle.id());
    EXPECT_EQ(ProgramSynthesisValidationReceipt::parse(
                  result.validation.serialize_canonical()).id(),
              result.validation.id());
    EXPECT_NO_THROW(validate_program_synthesis_validation_evidence(
        result.validation, result.validation_evidence));
    auto tampered_evidence = result.validation_evidence;
    tampered_evidence["core_count"] = 99;
    EXPECT_THROW(validate_program_synthesis_validation_evidence(
                     result.validation, tampered_evidence),
                 std::invalid_argument);
    ASSERT_TRUE(fixture.catalog->find_version("tenant:runtime", result.version.id()));

    const auto& remaining = result.reservation.data().remaining_after_reservation;
    const std::map<std::string, std::uint64_t> expected_maximum = {
        {"wall_time_ms", remaining.wall_time_ms},
        {"model_tokens", remaining.model_tokens},
        {"monetary_microunits", remaining.monetary_microunits},
        {"max_concurrency", remaining.max_concurrency},
        {"max_program_operations", remaining.max_program_operations},
        {"max_core_steps", remaining.max_core_steps},
        {"max_dynamic_compiles", remaining.max_dynamic_compiles},
        {"max_child_depth", remaining.max_child_depth},
        {"max_total_children", remaining.max_total_children},
    };
    const std::map<std::string, std::uint64_t> expected_minimum = {
        {"wall_time_ms", 1},
        {"model_tokens", 0},
        {"monetary_microunits", 0},
        {"max_concurrency", 1},
        {"max_program_operations", 1},
        {"max_core_steps", 1},
        {"max_dynamic_compiles", 0},
        {"max_child_depth", 0},
        {"max_total_children", 0},
    };
    ASSERT_EQ(result.bundle.declared_budget_requirements().size(), expected_maximum.size());
    for (const auto& requirement : result.bundle.declared_budget_requirements()) {
        ASSERT_TRUE(expected_minimum.contains(requirement.resource));
        EXPECT_EQ(requirement.minimum, expected_minimum.at(requirement.resource));
        EXPECT_EQ(requirement.maximum, expected_maximum.at(requirement.resource));
    }
}

TEST(ProgramSynthesisGateway, SemanticRejectionConsumesReservationAndPreventsAdmission) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    auto compiler = std::make_shared<ProgramCompiler>(
        fixture.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    const auto source = ProgramSource::from_javascript(
        "generated-wrong-successor.js",
        javascript_runtime_source(
            "runtime-completed",
            "return yield ng.callCore(\"wrong-core\", input, \"semantic:wrong-core\");"));
    ProgramSynthesisProposalData proposal_data;
    proposal_data.owner_scope = "tenant:runtime";
    proposal_data.lineage_id = digest('1');
    proposal_data.parent_run_id = "parent-run";
    proposal_data.source = source;
    proposal_data.requested_budget = javascript_budget(1, 1, 0, 0);
    proposal_data.created_at_ms = 1;
    const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));

    bool reserved = false;
    bool admission_called = false;
    ProgramSynthesisGateway gateway({
        compiler,
        fixture.catalog,
        [&](const ProgramSynthesisProposal& value) {
            reserved = true;
            auto before = javascript_budget(1, 1, 0, 0);
            before.max_dynamic_compiles = 1;
            auto after = before;
            after.max_dynamic_compiles = 0;
            return ProgramSynthesisReservation::create(
                {value.id(), value.data().lineage_id, digest('2'), digest('3'), before, after});
        },
        [&](const ProgramSynthesisProposal&, const ProgramBundle&,
            const ProgramSynthesisReservation&) {
            admission_called = true;
            return ProgramAdmission{"tenant:runtime", fixture.profile, fixture.policy, {}};
        },
        1024 * 1024,
        [](const ProgramSynthesisProposal&, const ProgramBundle& bundle,
           const ProgramSynthesisReservation&) {
            std::string observed;
            bool accepted = false;
            if (const auto control = bundle.control_source()) {
                auto generator = neograph::program::detail::JavaScriptGenerator::open(
                    *control, json::object(), JavaScriptCompileLimits{});
                if (generator) {
                    const auto step = generator->next();
                    if (step.command && step.command->kind() == JavaScriptCommandKind::CallCore) {
                        observed = step.command->arguments().value("name", "");
                        accepted = observed == "main";
                    }
                }
            }
            return ProgramSynthesisSemanticDecision{
                digest('7'), digest('8'), accepted,
                json{{"code", "P_SYNTHESIS_SEMANTIC_CORE_BINDING"},
                     {"expected", "main"},
                     {"observed", observed},
                     {"observed_bundle", bundle.id()}}};
        },
        1024 * 1024});

    try {
        (void)gateway.synthesize(proposal);
        FAIL() << "expected semantic rejection";
    } catch (const ProgramSynthesisValidationError& error) {
        EXPECT_TRUE(reserved);
        EXPECT_FALSE(admission_called);
        EXPECT_FALSE(error.receipt().data().accepted);
        EXPECT_EQ(error.receipt().data().proposal_id, proposal.id());
        EXPECT_EQ(error.evidence().at("code"), "P_SYNTHESIS_SEMANTIC_CORE_BINDING");
        EXPECT_EQ(error.evidence().at("observed"), "wrong-core");
        EXPECT_FALSE(fixture.store->get_bundle(
            "tenant:runtime", error.receipt().data().bundle_id));
        EXPECT_EQ(ProgramSynthesisValidationReceipt::parse(
                      error.receipt().serialize_canonical()).id(),
                  error.receipt().id());
    }
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
namespace {
class ProgramChildSynthesisPersistence : public ::testing::TestWithParam<std::string> {
protected:
    void check_static_child_recovery(bool replace, bool recover = true,
                                     std::string_view fault = {});
    std::string database;
    void        SetUp() override {
        if (GetParam() == "PostgreSQL" && !std::getenv("NEOGRAPH_TEST_POSTGRES_URL"))
            GTEST_SKIP() << "PostgreSQL test URL is required";
        database = (std::filesystem::temp_directory_path() /
                    ("neograph-synthesis-" + Checkpoint::generate_id() + ".db"))
                       .string();
    }
    void TearDown() override {
        if (GetParam() == "SQLite") {
            std::filesystem::remove(database);
            std::filesystem::remove(database + "-wal");
            std::filesystem::remove(database + "-shm");
        }
    }
    std::shared_ptr<CheckpointStore> checkpoint_backend() {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
        if (GetParam() == "SQLite")
            return std::make_shared<neograph::graph::SqliteCheckpointStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES_CHECKPOINT
        if (GetParam() == "PostgreSQL")
            return std::make_shared<neograph::graph::PostgresCheckpointStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"), 2);
#endif
        return std::make_shared<InMemoryCheckpointStore>();
    }
    ProgramStoreHandle program_backend() {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (GetParam() == "SQLite") return std::make_shared<SQLiteProgramStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (GetParam() == "PostgreSQL")
            return std::make_shared<PostgreSQLProgramStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"));
#endif
        return std::make_shared<InMemoryProgramStore>();
    }
    TransitionStoreHandle backend() {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (GetParam() == "SQLite") return std::make_shared<SQLiteProgramTransitionStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (GetParam() == "PostgreSQL")
            return std::make_shared<PostgreSQLProgramTransitionStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"));
#endif
        return std::make_shared<InMemoryProgramTransitionStore>();
    }
};
std::vector<std::string> synthesis_backends() {
    std::vector<std::string> result{"Memory"};
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
    result.push_back("SQLite");
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
    result.push_back("PostgreSQL");
#endif
    return result;
}
INSTANTIATE_TEST_SUITE_P(Backends,
                         ProgramChildSynthesisPersistence,
                         ::testing::ValuesIn(synthesis_backends()),
                         [](const auto& info) { return info.param; });
}  // namespace
TEST_P(ProgramChildSynthesisPersistence, BindsSpawnsAndJoinsWithoutRefundOrDuplicateValidation) {
    auto                  storage = backend();
    ChildSynthesisFixture test(storage, true);
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    auto                  bound    = test.fixture.runtime->prepare_child_synthesis(
        "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
        "generated-child");
    ASSERT_EQ(bound.data().state, ProgramChildSynthesisState::Bound);
    EXPECT_EQ(test.fixture.runtime
                  ->prepare_child_synthesis("tenant:runtime", *test.handle, *test.hold, proposal,
                                            *test.trusted_grant, "generated-child")
                  .id(),
              bound.id());
    EXPECT_EQ(test.semantic_calls, 1U);
    EXPECT_EQ(test.admissions, 1U);
    EXPECT_EQ(test.reservations,
              0U);  // The runtime transaction, never the N1 fixture callback, debited.
    EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_EQ(ProgramChildSynthesisRecord::parse(bound.serialize_canonical()).id(), bound.id());
    if (GetParam() != "Memory") {
        auto reopened = backend();
        ASSERT_EQ(reopened->load_child_syntheses("tenant:runtime", test.handle->run_id()).size(),
                  1U);
        EXPECT_EQ(
            reopened->load_child_syntheses("tenant:runtime", test.handle->run_id()).front().id(),
            bound.id());
        EXPECT_TRUE(
            reopened->load_child_syntheses("tenant:foreign", test.handle->run_id()).empty());
        EXPECT_TRUE(reopened->load_child_syntheses("tenant:runtime", "another-parent").empty());
    }
    test.hold.reset();
    const auto result = test.handle->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    const auto records = storage->load_child_syntheses("tenant:runtime", test.handle->run_id());
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records.front().data().state, ProgramChildSynthesisState::Spawned);
    EXPECT_EQ(test.handle->snapshot().children().size(), 1U);
}
TEST_P(ProgramChildSynthesisPersistence, ReusesCommittedCompileValidationAndFrozenAdmission) {
    for (auto state : {ProgramChildSynthesisState::Compiled, ProgramChildSynthesisState::Validated,
                       ProgramChildSynthesisState::Admitting}) {
        ChildSynthesisFixture test(backend(), true);
        test.journal->fail_synthesis_after = state;
        const auto proposal                = ProgramSynthesisProposal::create(test.proposal_data());
        EXPECT_THROW(test.fixture.runtime->prepare_child_synthesis(
                         "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
                         "generated-child"),
                     std::runtime_error);
        ASSERT_EQ(test.journal->load_child_syntheses_sync("tenant:runtime", test.handle->run_id())
                      .front()
                      .data()
                      .state,
                  state);
        auto bound = test.fixture.runtime->prepare_child_synthesis(
            "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
            "generated-child");
        EXPECT_EQ(bound.data().state, ProgramChildSynthesisState::Bound);
        EXPECT_EQ(test.semantic_calls, 1U);
        EXPECT_EQ(test.admissions, 1U);
        EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    }
}
TEST_P(ProgramChildSynthesisPersistence, AmbiguousClaimsBlockReplayAndNeverRerunForFree) {
    for (auto state :
         {ProgramChildSynthesisState::Compiling, ProgramChildSynthesisState::Validating}) {
        ChildSynthesisFixture test(backend(), true);
        test.journal->fail_synthesis_after = state;
        const auto proposal                = ProgramSynthesisProposal::create(test.proposal_data());
        EXPECT_THROW(test.fixture.runtime->prepare_child_synthesis(
                         "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
                         "generated-child"),
                     std::runtime_error);
        EXPECT_THROW(test.fixture.runtime->reconnect("tenant:runtime", test.handle->run_id()),
                     std::runtime_error);
        auto uncertain = test.fixture.runtime->prepare_child_synthesis(
            "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
            "generated-child");
        EXPECT_EQ(uncertain.data().state, ProgramChildSynthesisState::ReconciliationRequired);
        EXPECT_EQ(test.semantic_calls, 0U);
        EXPECT_EQ(test.admissions, 0U);
        EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    }
}
TEST_P(ProgramChildSynthesisPersistence, ReservationIsAtomicAndCannotFundAnotherBinding) {
    auto                  storage = backend();
    ChildSynthesisFixture test(storage, true);
    test.journal->fail_synthesis_after = ProgramChildSynthesisState::Reserved;
    const auto proposal                = ProgramSynthesisProposal::create(test.proposal_data());
    EXPECT_THROW(test.fixture.runtime->prepare_child_synthesis(
                     "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
                     "generated-child"),
                 std::runtime_error);
    auto       publication = test.journal->synthesis_publications.front();
    const auto expected    = publication.journal_record.previous_id;
    EXPECT_EQ(storage->compare_publish("tenant:runtime", expected, publication),
              ProgramTransitionPublishResult::AlreadyPresent);
    auto data                           = publication.child_synthesis_records.front().data();
    data.binding_name                   = "another-binding";
    publication.child_synthesis_records = {ProgramChildSynthesisRecord::create(std::move(data))};
    EXPECT_EQ(storage->compare_publish("tenant:runtime", expected, publication),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(storage->load_child_syntheses("tenant:runtime", test.handle->run_id()).size(), 1U);
    EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    // A revoked host grant cannot be resurrected by deserializing its valid content id.
    const auto trusted = test.trusted_grant;
    test.trusted_grant.reset();
    EXPECT_THROW(
        test.fixture.runtime->prepare_child_synthesis("tenant:runtime", *test.handle, *test.hold,
                                                      proposal, *trusted, "generated-child"),
        std::runtime_error);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS) && !defined(_WIN32)
TEST_P(ProgramChildSynthesisPersistence,
       ProcessExitReopensCatalogCheckpointAndSynthesisBeforeJoin) {
    if (GetParam() == "Memory") GTEST_SKIP() << "Process recovery requires a durable backend";
    if (const auto* inherited = std::getenv("NEOGRAPH_SYNTHESIS_CRASH_DB")) database = inherited;
    setenv("NEOGRAPH_SYNTHESIS_CRASH_DB", database.c_str(), 1);
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    const auto programs                     = [&]() -> ProgramStoreHandle {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (GetParam() == "SQLite") return std::make_shared<SQLiteProgramStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (GetParam() == "PostgreSQL")
            return std::make_shared<PostgreSQLProgramStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"));
#endif
        throw std::runtime_error("Durable Program store unavailable");
    };
    const auto checkpoints = [&]() -> std::shared_ptr<CheckpointStore> {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
        if (GetParam() == "SQLite")
            return std::make_shared<neograph::graph::SqliteCheckpointStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES_CHECKPOINT
        if (GetParam() == "PostgreSQL")
            return std::make_shared<neograph::graph::PostgresCheckpointStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"), 2);
#endif
        throw std::runtime_error("Durable checkpoint store unavailable");
    };
    ASSERT_EXIT(
        ([&] {
            ChildSynthesisFixture child(backend(), true, programs(), checkpoints());
            const auto proposal = ProgramSynthesisProposal::create(child.proposal_data());
            child.journal->fail_synthesis_after = ProgramChildSynthesisState::Admitting;
            try {
                child.fixture.runtime->prepare_child_synthesis(
                    "tenant:runtime", *child.handle, *child.hold, proposal, *child.trusted_grant,
                    "generated-child");
                std::_Exit(78);
            } catch (const std::runtime_error&) {
                const auto stored =
                    child.journal->load_child_syntheses_sync("tenant:runtime", child.handle->run_id());
                if (stored.size() != 1 ||
                    stored.front().data().state != ProgramChildSynthesisState::Admitting)
                    std::_Exit(79);
                std::ofstream metadata(database + ".recovery");
                metadata << json{{"run_id", child.handle->run_id()},
                                 {"proposal_id", proposal.id()},
                                 {"grant", child.trusted_grant->serialize_canonical()}}
                                .dump();
                metadata.close();
                std::_Exit(77);  // No destructors, cancellation, or orderly parent termination.
            }
        }()),
        ::testing::ExitedWithCode(77), "");
    unsetenv("NEOGRAPH_SYNTHESIS_CRASH_DB");
    std::ifstream     metadata(database + ".recovery");
    const std::string bytes((std::istreambuf_iterator<char>(metadata)),
                            std::istreambuf_iterator<char>());
    const auto        saved       = json::parse(bytes);
    const auto        run_id      = saved.at("run_id").get<std::string>();
    const auto        proposal_id = saved.at("proposal_id").get<std::string>();
    const auto      grant = ProgramChildSynthesisGrant::parse(saved.at("grant").get<std::string>());
    AdmittedRuntime host(2, checkpoints(), backend(), {}, ExecutionGuarantee::Strict, true);
    host.catalog = std::make_shared<ProgramCatalog>(
        CatalogConfig{programs(), host.registry, host.engines, "program-runtime-test/v1"});
    unsigned                      semantic_calls = 0, admission_calls = 0;
    ProgramSynthesisGatewayConfig gateway;
    gateway.compiler = std::make_shared<ProgramCompiler>(
        host.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    gateway.catalog       = host.catalog;
    gateway.reserve_child = [](const auto&, const auto&) -> ProgramSynthesisReservation {
        throw std::runtime_error("Must not reserve twice");
    };
    gateway.validate_semantics = [&](const auto&, const auto&,
                                     const auto&) -> ProgramSynthesisSemanticDecision {
        ++semantic_calls;
        throw std::runtime_error("Must reuse semantic result");
    };
    gateway.admission = [&](const auto&, const auto&, const auto&) -> ProgramAdmission {
        ++admission_calls;
        throw std::runtime_error("Must reuse frozen admission");
    };
    RuntimeConfig config{host.catalog, host.checkpoints, {}, host.journal, 2};
    config.child_synthesis_gateway = std::make_shared<ProgramSynthesisGateway>(std::move(gateway));
    config.child_synthesis_grant_resolver =
        [grant](std::string_view owner, std::string_view run,
                std::string_view id) -> std::optional<ProgramChildSynthesisGrant> {
        if (owner == grant.data().owner_scope && run == grant.data().parent_run_id &&
            id == grant.id())
            return grant;
        return std::nullopt;
    };
    host.runtime = std::make_unique<ProgramRuntime>(std::move(config));
    EXPECT_THROW(host.runtime->reconnect("tenant:runtime", run_id), std::runtime_error);
    const auto bound = host.runtime->recover_child_synthesis("tenant:runtime", run_id, proposal_id);
    ASSERT_EQ(bound.data().state, ProgramChildSynthesisState::Bound);
    EXPECT_EQ(semantic_calls, 0U);
    EXPECT_EQ(admission_calls, 0U);
    auto       parent = host.runtime->reconnect("tenant:runtime", run_id);
    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_EQ(parent.snapshot().children().size(), 1U);
    const auto head = host.journal->load_child_syntheses("tenant:runtime", run_id).front();
    EXPECT_EQ(head.data().state, ProgramChildSynthesisState::Spawned);
    metadata.close();
    std::filesystem::remove(database + ".recovery");
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence, SemanticRejectionPersistsEvidenceWithoutAdmission) {
    ChildSynthesisFixture test(backend(), true);
    test.semantic_accept = false;
    const auto proposal  = ProgramSynthesisProposal::create(test.proposal_data());
    auto       rejected  = test.fixture.runtime->prepare_child_synthesis(
        "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
        "generated-child");
    EXPECT_EQ(rejected.data().state, ProgramChildSynthesisState::Failed);
    EXPECT_EQ(rejected.data().error_code, "P_CHILD_SYNTHESIS_REJECTED");
    EXPECT_FALSE(rejected.data().artifacts.at("evidence").at("accepted").get<bool>());
    EXPECT_EQ(test.admissions, 0U);
    EXPECT_EQ(test.semantic_calls, 1U);
    EXPECT_EQ(test.fixture.runtime
                  ->prepare_child_synthesis("tenant:runtime", *test.handle, *test.hold, proposal,
                                            *test.trusted_grant, "generated-child")
                  .id(),
              rejected.id());
    EXPECT_EQ(test.semantic_calls, 1U);
    EXPECT_TRUE(test.handle->snapshot().children().empty());
}
TEST_P(ProgramChildSynthesisPersistence, ConcurrentRetriesHaveOneReservationAndOneSemanticCall) {
    ChildSynthesisFixture test(backend(), true);
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    const auto            prepare  = [&] {
        return test.fixture.runtime->prepare_child_synthesis(
            "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
            "generated-child");
    };
    auto first  = std::async(std::launch::async, prepare);
    auto second = std::async(std::launch::async, prepare);
    EXPECT_EQ(first.get().id(), second.get().id());
    EXPECT_EQ(test.semantic_calls, 1U);
    EXPECT_EQ(test.admissions, 1U);
    EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    ASSERT_EQ(test.journal->load_child_syntheses_sync("tenant:runtime", test.handle->run_id()).size(),
              1U);
}
TEST_P(ProgramChildSynthesisPersistence, RejectsMutatedStageArtifactsAndRecordIdentity) {
    ChildSynthesisFixture test(backend(), true);
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    const auto            bound    = test.fixture.runtime->prepare_child_synthesis(
        "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
        "generated-child");
    auto value        = json::parse(bound.serialize_canonical());
    value["revision"] = 8.5;
    EXPECT_THROW(ProgramChildSynthesisRecord::parse(value.dump()), std::invalid_argument);
    value                           = json::parse(bound.serialize_canonical());
    value["unrecognized_authority"] = true;
    EXPECT_THROW(ProgramChildSynthesisRecord::parse(value.dump()), std::invalid_argument);
    auto data                  = bound.data();
    data.artifacts["evidence"] = json{{"accepted", false}};
    EXPECT_THROW(ProgramChildSynthesisRecord::create(data), std::invalid_argument);
    data              = bound.data();
    data.binding_name = "foreign-child";
    EXPECT_THROW(ProgramChildSynthesisRecord::create(data), std::invalid_argument);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence,
       BackendWriteFailureRollsBackReservationAndParentHeadTogether) {
    auto                  storage = backend();
    ChildSynthesisFixture test(storage, true);
    const auto            before = test.handle->snapshot();
    const auto lineage = storage->load_run_lineage("tenant:runtime", test.handle->run_id());
    const auto execute = [&](const std::string& sql) {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (GetParam() == "SQLite") {
            sqlite3* connection = nullptr;
            if (sqlite3_open(database.c_str(), &connection) != SQLITE_OK)
                throw std::runtime_error("Test SQLite connection failed");
            const auto code = sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, nullptr);
            const std::string error = sqlite3_errmsg(connection);
            sqlite3_close(connection);
            if (code != SQLITE_OK) throw std::runtime_error(error);
            return;
        }
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (GetParam() == "PostgreSQL") {
            auto* connection = PQconnectdb(std::getenv("NEOGRAPH_TEST_POSTGRES_URL"));
            if (!connection || PQstatus(connection) != CONNECTION_OK ||
                std::string(PQdb(connection)) != "neograph_test") {
                if (connection) PQfinish(connection);
                throw std::runtime_error(
                    "Failure injection requires disposable neograph_test database");
            }
            auto*             result = PQexec(connection, sql.c_str());
            const bool        ok     = result && PQresultStatus(result) == PGRES_COMMAND_OK;
            const std::string error  = PQerrorMessage(connection);
            if (result) PQclear(result);
            PQfinish(connection);
            if (!ok) throw std::runtime_error(error);
        }
#endif
    };
    if (GetParam() == "Memory")
        storage.as<InMemoryProgramTransitionStore>()
            ->fail_next_publication_for_testing(ProgramTransitionFaultPoint::BeforeCommit);
    else if (GetParam() == "SQLite")
        execute(
            "CREATE TRIGGER synthesis_test_abort BEFORE INSERT ON "
            "program_transition_synthesis_log_v1 BEGIN SELECT RAISE(ABORT, 'injected synthesis "
            "failure'); END;");
    else
        execute(
            "CREATE FUNCTION synthesis_test_abort_fn() RETURNS trigger LANGUAGE plpgsql AS $$ "
            "BEGIN RAISE EXCEPTION 'injected synthesis failure'; END $$; CREATE TRIGGER "
            "synthesis_test_abort BEFORE INSERT ON neograph_program_transition_synthesis_log_v1 "
            "FOR EACH ROW EXECUTE FUNCTION synthesis_test_abort_fn();");
    const auto proposal = ProgramSynthesisProposal::create(test.proposal_data());
    EXPECT_THROW(test.fixture.runtime->prepare_child_synthesis(
                     "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
                     "generated-child"),
                 std::runtime_error);
    if (GetParam() == "SQLite")
        execute("DROP TRIGGER synthesis_test_abort;");
    else if (GetParam() == "PostgreSQL")
        execute(
            "DROP TRIGGER synthesis_test_abort ON neograph_program_transition_synthesis_log_v1; "
            "DROP FUNCTION synthesis_test_abort_fn();");
    EXPECT_EQ(test.handle->snapshot().id(), before.id());
    EXPECT_EQ(storage->load_run_lineage("tenant:runtime", test.handle->run_id())->id(),
              lineage->id());
    EXPECT_TRUE(storage->load_child_syntheses("tenant:runtime", test.handle->run_id()).empty());
    EXPECT_EQ(test.semantic_calls, 0U);
    const auto bound = test.fixture.runtime->prepare_child_synthesis(
        "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
        "generated-child");
    EXPECT_EQ(bound.data().state, ProgramChildSynthesisState::Bound);
    EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS) && !defined(_WIN32)
namespace {
class ProgramChildSynthesisCrash : public ::testing::TestWithParam<std::string> {
protected:
    std::string database;
    std::string backend_name() const { return GetParam().substr(0, GetParam().find('_')); }
    std::string boundary() const { return GetParam().substr(GetParam().find('_') + 1); }
    void        SetUp() override {
        if (backend_name() == "PostgreSQL" && !std::getenv("NEOGRAPH_TEST_POSTGRES_URL"))
            GTEST_SKIP();
        const auto* inherited = std::getenv("NEOGRAPH_N3_CRASH_DB");
        database              = inherited ? inherited
                                                 : (std::filesystem::temp_directory_path() /
                                ("neograph-n3-" + Checkpoint::generate_id() + ".db"))
                                   .string();
    }
    void TearDown() override {
        if (std::getenv("NEOGRAPH_N3_WORKER")) return;
        synthesis_execution_marker.clear();
        unsetenv("NEOGRAPH_N3_CRASH_DB");
        for (const auto* suffix : {"", "-wal", "-shm", ".recovery", ".executions"})
            std::filesystem::remove(database + suffix);
    }
    TransitionStoreHandle backend() {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (backend_name() == "SQLite")
            return std::make_shared<SQLiteProgramTransitionStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (backend_name() == "PostgreSQL")
            return std::make_shared<PostgreSQLProgramTransitionStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"));
#endif
        throw std::runtime_error("Durable transition backend unavailable");
    }
    ProgramStoreHandle programs() {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        if (backend_name() == "SQLite") return std::make_shared<SQLiteProgramStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        if (backend_name() == "PostgreSQL")
            return std::make_shared<PostgreSQLProgramStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"));
#endif
        throw std::runtime_error("Durable Program backend unavailable");
    }
    std::shared_ptr<CheckpointStore> checkpoints() {
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
        if (backend_name() == "SQLite")
            return std::make_shared<neograph::graph::SqliteCheckpointStore>(database);
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES_CHECKPOINT
        if (backend_name() == "PostgreSQL")
            return std::make_shared<neograph::graph::PostgresCheckpointStore>(
                std::getenv("NEOGRAPH_TEST_POSTGRES_URL"), 2);
#endif
        throw std::runtime_error("Durable checkpoint backend unavailable");
    }
};
std::vector<std::string> synthesis_crash_boundaries() {
    std::vector<std::string> result;
    for (const auto& backend : synthesis_backends()) {
        if (backend == "Memory") continue;
        for (const auto* boundary :
             {"Reserved", "Compiling", "Compiled", "Validating", "Validated", "Admitting",
              "Admitted", "Bound", "Dispatching", "Spawned", "ChildPublishing", "ChildDispatched",
              "ChildTerminal", "ChildResult", "CommandResult", "ParentTerminal",
              "ConcurrentRecovery"})
            result.push_back(backend + "_" + boundary);
    }
    return result;
}
INSTANTIATE_TEST_SUITE_P(Boundaries,
                         ProgramChildSynthesisCrash,
                         ::testing::ValuesIn(synthesis_crash_boundaries()),
                         [](const auto& info) { return info.param; });
}  // namespace
TEST_P(ProgramChildSynthesisCrash, ProcessLossPreservesBudgetIdentityAndRecordedOutcomes) {
    const auto* worker = std::getenv("NEOGRAPH_N3_WORKER");
    if (!worker) {
        setenv("NEOGRAPH_N3_CRASH_DB", database.c_str(), 1);
        ::testing::FLAGS_gtest_death_test_style = "threadsafe";
        ASSERT_EXIT(
            ([&] {
                ChildSynthesisFixture child(backend(), true, programs(), checkpoints());
                const auto proposal  = ProgramSynthesisProposal::create(child.proposal_data());
                const auto parent_id = child.handle->run_id();
                synthesis_execution_marker = database + ".executions";
                {
                    std::ofstream metadata(database + ".recovery");
                    metadata << json{{"run_id", parent_id},
                                     {"proposal_id", proposal.id()},
                                     {"grant", child.trusted_grant->serialize_canonical()}}
                                    .dump();
                }
                const std::vector<std::string> states{
                    "Reserved",  "Compiling", "Compiled", "Validating",  "Validated",
                    "Admitting", "Admitted",  "Bound",    "Dispatching", "Spawned"};
                child.journal->after_publication =
                    [&](const ProgramTransitionPublication& publication) {
                        bool stop = false;
                        if (!publication.child_synthesis_records.empty()) {
                            auto state = static_cast<std::size_t>(
                                publication.child_synthesis_records.front().data().state);
                            stop = state < states.size() &&
                                   (boundary() == states[state] ||
                                    (boundary() == "ConcurrentRecovery" && state == 0));
                        }
                        const auto& run = publication.run_record;
                        if (boundary() == "ChildTerminal" &&
                            run.invocation().parent_run_id == parent_id && run.terminal_result())
                            stop = true;
                        if (run.run_id() == parent_id) {
                            if (boundary() == "ParentTerminal" && run.terminal_result())
                                stop = true;
                            for (const auto& command : publication.commands)
                                if (boundary() == "CommandResult" &&
                                    command.command_ordinal() == 2 && command.completed())
                                    stop = true;
                            for (const auto& relation : run.children()) {
                                if (boundary() == "ChildPublishing" &&
                                    relation.state == ProgramChildState::Publishing)
                                    stop = true;
                                if (boundary() == "ChildDispatched" &&
                                    relation.state == ProgramChildState::Dispatched)
                                    stop = true;
                                if (boundary() == "ChildResult" && relation.terminal_result)
                                    stop = true;
                            }
                        }
                        if (stop) std::_Exit(77);
                    };
                child.fixture.runtime->prepare_child_synthesis(
                    "tenant:runtime", *child.handle, *child.hold, proposal, *child.trusted_grant,
                    "generated-child");
                child.hold.reset();
                (void)child.handle->wait();
                std::_Exit(78);  // The requested boundary was never observed.
            }()),
            ::testing::ExitedWithCode(77), "");
        unsetenv("NEOGRAPH_N3_CRASH_DB");
    }
    std::ifstream     metadata(database + ".recovery");
    const std::string bytes((std::istreambuf_iterator<char>(metadata)),
                            std::istreambuf_iterator<char>());
    const auto        saved       = json::parse(bytes);
    const auto        run_id      = saved.at("run_id").get<std::string>();
    const auto        proposal_id = saved.at("proposal_id").get<std::string>();
    const auto grant   = ProgramChildSynthesisGrant::parse(saved.at("grant").get<std::string>());
    auto       storage = backend();
    const auto before  = storage->load_child_syntheses("tenant:runtime", run_id).front();
    const auto previous_parent  = storage->load("tenant:runtime", run_id);
    const auto previous_journal = storage->latest("tenant:runtime", run_id);
    EXPECT_EQ(previous_parent->remaining_budget().max_dynamic_compiles +
                  previous_journal->inflight_reservation.max_dynamic_compiles,
              2U);
    const auto      original_child = previous_parent->children().empty()
                                         ? std::string{}
                                         : previous_parent->children().front().child_run_id;
    AdmittedRuntime host(2, checkpoints(), storage, {}, ExecutionGuarantee::Strict, true);
    host.catalog = std::make_shared<ProgramCatalog>(
        CatalogConfig{programs(), host.registry, host.engines, "program-runtime-test/v1"});
    const auto                    profile        = before.data().parent.version.admission_profile();
    const auto                    policy         = before.data().parent.version.policy_snapshot();
    unsigned                      semantic_calls = 0, admission_calls = 0;
    ProgramSynthesisGatewayConfig gateway;
    gateway.compiler = std::make_shared<ProgramCompiler>(
        host.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    gateway.catalog            = host.catalog;
    gateway.validate_semantics = [&](const auto&, const auto&, const auto&) {
        ++semantic_calls;
        if (worker) {
            std::ofstream marker(database + ".semantic-" + worker);
            marker << "validated";
        }
        return ProgramSynthesisSemanticDecision{digest('b'), digest('c'), true,
                                                json{{"accepted", true}}};
    };
    gateway.admission = [&](const auto&, const auto&, const auto&) {
        ++admission_calls;
        return ProgramAdmission{"tenant:runtime", profile, policy, {}};
    };
    RuntimeConfig config{host.catalog, host.checkpoints, {}, storage, 2};
    config.child_synthesis_gateway = std::make_shared<ProgramSynthesisGateway>(std::move(gateway));
    unsigned grant_reads           = 0;
    config.child_synthesis_grant_resolver =
        [&, grant](std::string_view owner, std::string_view run,
                   std::string_view id) -> std::optional<ProgramChildSynthesisGrant> {
        if (worker && ++grant_reads == 2) {
            {
                std::ofstream ready(database + ".ready-" + worker);
                ready << "ready";
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!std::filesystem::exists(database + ".release")) {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("Concurrent recovery barrier timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (owner == grant.data().owner_scope && run == grant.data().parent_run_id &&
            id == grant.id())
            return grant;
        return std::nullopt;
    };
    host.runtime               = std::make_unique<ProgramRuntime>(std::move(config));
    synthesis_execution_marker = database + ".executions";
    using State                = ProgramChildSynthesisState;
    if (worker) {
        try {
            const auto value =
                host.runtime->recover_child_synthesis("tenant:runtime", run_id, proposal_id);
            std::_Exit(value.data().state == State::Bound ? 0 : 4);
        } catch (const std::exception&) {
            std::_Exit(3);
        }
    }
    if (boundary() == "ConcurrentRecovery") {
        setenv("NEOGRAPH_N3_CRASH_DB", database.c_str(), 1);
#ifdef __APPLE__
        std::vector<char> executable_buffer(1024);
        auto buffer_size = static_cast<std::uint32_t>(executable_buffer.size());
        if (_NSGetExecutablePath(executable_buffer.data(), &buffer_size) != 0) {
            executable_buffer.resize(buffer_size);
            ASSERT_EQ(_NSGetExecutablePath(executable_buffer.data(), &buffer_size), 0);
        }
        const auto executable = std::filesystem::canonical(executable_buffer.data()).string();
#else
        const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
#endif
        const auto filter =
            "--gtest_filter=Boundaries/"
            "ProgramChildSynthesisCrash.ProcessLossPreservesBudgetIdentityAndRecordedOutcomes/" +
            GetParam();
        std::vector<pid_t> workers;
        for (const auto* identity : {"1", "2"}) {
            setenv("NEOGRAPH_N3_WORKER", identity, 1);
            char* arguments[] = {const_cast<char*>(executable.c_str()),
                                 const_cast<char*>(filter.c_str()), nullptr};
            pid_t process     = 0;
            ASSERT_EQ(
                posix_spawn(&process, executable.c_str(), nullptr, nullptr, arguments, environ), 0);
            workers.push_back(process);
        }
        unsetenv("NEOGRAPH_N3_WORKER");
        unsetenv("NEOGRAPH_N3_CRASH_DB");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while ((!std::filesystem::exists(database + ".ready-1") ||
                !std::filesystem::exists(database + ".ready-2")) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        EXPECT_TRUE(std::filesystem::exists(database + ".ready-1"));
        EXPECT_TRUE(std::filesystem::exists(database + ".ready-2"));
        {
            std::ofstream release(database + ".release");
            release << "go";
        }
        std::vector<int> outcomes;
        for (const auto process : workers) {
            int status = 0;
            ASSERT_EQ(waitpid(process, &status, 0), process);
            ASSERT_TRUE(WIFEXITED(status));
            outcomes.push_back(WEXITSTATUS(status));
        }
        std::sort(outcomes.begin(), outcomes.end());
        EXPECT_EQ(outcomes, (std::vector<int>{0, 3}));
        EXPECT_EQ(static_cast<unsigned>(std::filesystem::exists(database + ".semantic-1")) +
                      static_cast<unsigned>(std::filesystem::exists(database + ".semantic-2")),
                  1U);
        EXPECT_EQ(storage->load_child_syntheses("tenant:runtime", run_id).front().data().state,
                  State::Bound);
        for (const auto* suffix :
             {".ready-1", ".ready-2", ".release", ".semantic-1", ".semantic-2"})
            std::filesystem::remove(database + suffix);
    }
    const auto recovered =
        host.runtime->recover_child_synthesis("tenant:runtime", run_id, proposal_id);
    if (recovered.data().state == State::ReconciliationRequired &&
        recovered.data().error_code == "P_CHILD_SYNTHESIS_CHILD_UNCERTAIN") {
        EXPECT_TRUE(boundary() == "ChildDispatched" || boundary() == "Spawned");
        EXPECT_THROW(host.runtime->reconnect("tenant:runtime", run_id), std::runtime_error);
        ASSERT_EQ(storage->load("tenant:runtime", run_id)->children().size(), 1U);
        EXPECT_EQ(storage->load("tenant:runtime", run_id)->children().front().child_run_id,
                  original_child);
        EXPECT_EQ(semantic_calls, 0U);
        EXPECT_EQ(admission_calls, 0U);
        return;
    }
    if (before.data().state == State::Compiling || before.data().state == State::Validating) {
        EXPECT_EQ(recovered.data().state, State::ReconciliationRequired);
        EXPECT_EQ(semantic_calls, 0U);
        EXPECT_EQ(admission_calls, 0U);
        EXPECT_THROW(host.runtime->reconnect("tenant:runtime", run_id), std::runtime_error);
        EXPECT_TRUE(storage->load("tenant:runtime", run_id)->children().empty());
        EXPECT_EQ(storage->load("tenant:runtime", run_id)->remaining_budget().max_dynamic_compiles,
                  2U);
        return;
    }
    EXPECT_EQ(
        semantic_calls,
        before.data().state < State::Validated && boundary() != "ConcurrentRecovery" ? 1U : 0U);
    EXPECT_EQ(
        admission_calls,
        before.data().state < State::Admitting && boundary() != "ConcurrentRecovery" ? 1U : 0U);
    auto       parent = host.runtime->reconnect("tenant:runtime", run_id);
    const auto result = parent.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_EQ(result.remaining_budget().max_program_operations, 13U);
    const auto snapshot = parent.snapshot();
    ASSERT_EQ(snapshot.children().size(), 1U);
    if (!original_child.empty())
        EXPECT_EQ(snapshot.children().front().child_run_id, original_child);
    EXPECT_EQ(snapshot.children().front().state, ProgramChildState::Completed);
    EXPECT_EQ(host.runtime->reconnect("tenant:runtime", run_id).wait().id(), result.id());
    const auto head = storage->load_child_syntheses("tenant:runtime", run_id).front();
    EXPECT_EQ(head.data().state, State::Spawned);
    EXPECT_EQ(head.data().artifacts.at("child_run_id"), snapshot.children().front().child_run_id);
    if (boundary() == "ChildTerminal" || boundary() == "ChildResult" ||
        boundary() == "CommandResult" || boundary() == "ParentTerminal") {
        std::ifstream executions(database + ".executions");
        std::string   line;
        unsigned      count = 0;
        while (std::getline(executions, line))
            ++count;
        EXPECT_EQ(count, 1U) << "Completed child work must not be repeated after recovery";
    }
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence, LostCommitAcknowledgementAndReadOutageNeverRefundCompile) {
    ChildSynthesisFixture test(backend(), true);
    const auto            proposal  = ProgramSynthesisProposal::create(test.proposal_data());
    test.journal->after_publication = [&](const auto& publication) {
        if (!publication.child_synthesis_records.empty() &&
            publication.child_synthesis_records.front().data().state ==
                ProgramChildSynthesisState::Reserved) {
            test.journal->synthesis_read_failures.store(2);
            throw std::runtime_error("lost commit acknowledgement");
        }
    };
    const auto prepare = [&] {
        return test.fixture.runtime->prepare_child_synthesis(
            "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
            "generated-child");
    };
    EXPECT_THROW(prepare(), std::runtime_error);
    EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_THROW(prepare(), std::runtime_error);
    const auto bound = prepare();
    EXPECT_EQ(bound.data().state, ProgramChildSynthesisState::Bound);
    EXPECT_EQ(test.semantic_calls, 1U);
    test.hold.reset();
    const auto result = test.handle->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
}
TEST_P(ProgramChildSynthesisPersistence, CancellationDuringValidationCannotAdmitOrDispatch) {
    ChildSynthesisFixture test(backend(), true);
    test.semantic_hook  = [&] { test.handle->cancel(); };
    const auto proposal = ProgramSynthesisProposal::create(test.proposal_data());
    EXPECT_THROW(test.fixture.runtime->prepare_child_synthesis(
                     "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
                     "generated-child"),
                 std::runtime_error);
    test.hold.reset();
    const auto result = test.handle->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled) << result.serialize_canonical();
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_EQ(test.admissions, 0U);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_TRUE(test.handle->snapshot().children().empty());
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS) && defined(NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES)
TEST(ProgramChildSynthesisPostgreSQL, ConnectionLossInsideReservationRollsBackBeforeHostRecovery) {
    const auto* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    if (!url) GTEST_SKIP();
    auto* admin = PQconnectdb(url);
    ASSERT_TRUE(admin && PQstatus(admin) == CONNECTION_OK);
    ASSERT_EQ(std::string(PQdb(admin)), "neograph_test");
    const auto execute = [&](const char* sql) {
        auto*             result = PQexec(admin, sql);
        const bool        ok     = result && PQresultStatus(result) == PGRES_COMMAND_OK;
        const std::string error  = PQerrorMessage(admin);
        if (result) PQclear(result);
        if (!ok) throw std::runtime_error(error);
    };
    ChildSynthesisFixture test(std::make_shared<PostgreSQLProgramTransitionStore>(url), true);
    const auto            before = test.handle->snapshot();
    execute(
        "CREATE FUNCTION synthesis_n3_disconnect_fn() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
        "PERFORM pg_terminate_backend(pg_backend_pid()); RETURN NEW; END $$; CREATE TRIGGER "
        "synthesis_n3_disconnect BEFORE INSERT ON neograph_program_transition_synthesis_log_v1 FOR "
        "EACH ROW EXECUTE FUNCTION synthesis_n3_disconnect_fn();");
    const auto proposal = ProgramSynthesisProposal::create(test.proposal_data());
    EXPECT_THROW(test.fixture.runtime->prepare_child_synthesis(
                     "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
                     "generated-child"),
                 std::runtime_error);
    execute(
        "DROP TRIGGER synthesis_n3_disconnect ON neograph_program_transition_synthesis_log_v1; "
        "DROP FUNCTION synthesis_n3_disconnect_fn();");
    PQfinish(admin);
    TransitionStoreHandle reopened = std::make_shared<PostgreSQLProgramTransitionStore>(url);
    EXPECT_EQ(reopened->load("tenant:runtime", test.handle->run_id())->id(), before.id());
    EXPECT_TRUE(reopened->load_child_syntheses("tenant:runtime", test.handle->run_id()).empty());
    test.journal->reopen_backend_for_testing(reopened);
    EXPECT_EQ(test.fixture.runtime
                  ->prepare_child_synthesis("tenant:runtime", *test.handle, *test.hold, proposal,
                                            *test.trusted_grant, "generated-child")
                  .data()
                  .state,
              ProgramChildSynthesisState::Bound);
    test.hold.reset();
    const auto result = test.handle->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_EQ(test.semantic_calls, 1U);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence, ActivationCannotRedirectTheAdmittedChild) {
    ChildSynthesisFixture test(backend(), true, program_backend());
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    const auto            bound    = test.fixture.runtime->prepare_child_synthesis(
        "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
        "generated-child");
    const auto alternative = test.fixture.admit("runtime-failing");
    (void)test.fixture.catalog->activate("tenant:runtime", alternative.id(), 0);
    test.hold.reset();
    const auto result = test.handle->wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    const auto relation = test.handle->snapshot().children().front();
    const auto link     = ModuleLinkReceipt::parse(relation.link_receipt);
    EXPECT_EQ(link.child_program_version_id(),
              bound.data().artifacts.at("version").at("id").get<std::string>());
    EXPECT_NE(link.child_program_version_id(), alternative.id());
}
TEST_P(ProgramChildSynthesisPersistence,
       RemovedChildVersionCannotExecuteFromCachedSynthesisEvidence) {
    ChildSynthesisFixture test(backend(), true, program_backend());
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    const auto            bound    = test.fixture.runtime->prepare_child_synthesis(
        "tenant:runtime", *test.handle, *test.hold, proposal, *test.trusted_grant,
        "generated-child");
    const auto child_id = bound.data().artifacts.at("version").at("id").get<std::string>();
    (void)test.fixture.catalog->collect_retention("tenant:runtime", {test.version->id(), child_id});
    ASSERT_TRUE(test.fixture.catalog->resolve_version("tenant:runtime", child_id));
    (void)test.fixture.catalog->collect_retention("tenant:runtime", {test.version->id()});
    ASSERT_FALSE(test.fixture.catalog->resolve_version("tenant:runtime", child_id));
    test.hold.reset();
    const auto result = test.handle->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_TRUE(test.handle->snapshot().children().empty());
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence,
       ReplacementKeepsCompileDebitAndCannotInheritGeneratedBinding) {
    ChildSynthesisFixture test(backend(), true, program_backend());
    const auto            proposal = ProgramSynthesisProposal::create(test.proposal_data());
    (void)test.fixture.runtime->prepare_child_synthesis("tenant:runtime", *test.handle, *test.hold,
                                                        proposal, *test.trusted_grant,
                                                        "generated-child");
    const auto handoff = test.handle->latest_handoff();
    ASSERT_TRUE(handoff);
    auto source = ProgramSource::from_javascript(
        "n3-successor.js", javascript_runtime_source("runtime-completed", R"JS(
        return {generation: "successor"};
    )JS"));
    const auto      ceiling = test.parent->run.invocation().budget;
    const RunBudget floor{1, 0, 0, 1, 1, 1, 0, 0, 0};
    auto            bundle = test.compiler->compile(source, ProgramBudgetBounds{floor, ceiling});
    auto            target = test.fixture.catalog->admit(
        bundle, ProgramAdmission{"tenant:runtime", test.fixture.profile, test.fixture.policy, {}});
    auto replacement = test.fixture.runtime->replace(
        "tenant:runtime", handoff->reference, target,
        ProgramInvocation{
            json{{"handoff", handoff->value}, {"previous_run_id", test.handle->run_id()}},
            test.handle->snapshot().remaining_budget(),
            "n3-successor",
            {}});
    test.hold.reset();
    const auto result = replacement.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_EQ(test.journal->load_run_lineage_sync("tenant:runtime", test.handle->run_id())
                  ->active_generation(),
              2U);
    EXPECT_TRUE(test.journal->load_child_syntheses_sync("tenant:runtime", replacement.run_id()).empty());
    EXPECT_THROW(test.fixture.runtime->recover_child_synthesis(
                     "tenant:runtime", test.handle->run_id(), proposal.id()),
                 std::runtime_error);
    EXPECT_EQ(completed_calls.load(), 0U);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence, LiveReconnectDoesNotMisclassifyAnActiveChildAsUncertain) {
    ChildSynthesisFixture test(backend(), true);
    test.child_budget.wall_time_ms = 5000;
    test.trusted_grant             = ProgramChildSynthesisGrant::create(test.grant_data());
    const auto proposal            = ProgramSynthesisProposal::create(test.proposal_data());
    (void)test.fixture.runtime->prepare_child_synthesis("tenant:runtime", *test.handle, *test.hold,
                                                        proposal, *test.trusted_grant,
                                                        "generated-child");
    std::promise<void> reached, release;
    auto               ready  = reached.get_future();
    auto               resume = release.get_future().share();
    std::atomic<bool>  observed{false};
    test.journal->after_publication = [&](const auto& publication) {
        if (publication.run_record.run_id() != test.handle->run_id()) return;
        for (const auto& child : publication.run_record.children()) {
            if (child.state == ProgramChildState::Dispatched && !observed.exchange(true)) {
                reached.set_value();
                (void)resume.wait_for(std::chrono::seconds(10));
            }
        }
    };
    test.hold.reset();
    EXPECT_EQ(ready.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_NO_THROW({
        auto same = test.fixture.runtime->reconnect("tenant:runtime", test.handle->run_id());
        EXPECT_EQ(same.run_id(), test.handle->run_id());
    });
    release.set_value();
    const auto result = test.handle->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(test.journal->load_child_syntheses_sync("tenant:runtime", test.handle->run_id())
                  .front()
                  .data()
                  .state,
              ProgramChildSynthesisState::Spawned);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
namespace {
std::string recursive_graph_source(unsigned nodes, const std::string& body) {
    std::string source =
        "export function define() { const g=ng.graph('main'); "
        "g.channel('value',{reducer:'runtime-overwrite',initial:''});\n";
    for (unsigned i = 0; i < nodes; ++i) {
        source += "g.node('n" + std::to_string(i) + "',{type:'runtime-completed'});\n";
        if (i) source += "g.edge('n" + std::to_string(i - 1) + "','n" + std::to_string(i) + "');\n";
    }
    return source + "g.entry('n0');g.exit('n" + std::to_string(nodes - 1) +
           "');return g;}\nexport function* main(input){\n" + body + "\n}";
}
struct RecursiveCheckpointGate {
    std::mutex                         mutex;
    std::condition_variable            condition;
    std::map<std::string, std::string> reached;
    std::set<std::string>              released;
    bool                               stopped = false;
    void                               observe(const ProgramTransitionPublication& publication) {
        for (const auto& command : publication.commands) {
            if (!command.completed() ||
                command.command().kind() != JavaScriptCommandKind::Checkpoint)
                continue;
            const auto site = command.command().source_site();
            if (site != "main:propose" && site != "main:budget" && site != "child:propose" &&
                site != "child:swap" && site != "grandchild:ready")
                continue;
            std::unique_lock lock(mutex);
            reached[site] = publication.run_record.run_id();
            condition.notify_all();
            condition.wait_for(lock, std::chrono::seconds(30),
                                                             [&] { return stopped || released.contains(site); });
        }
    }
    std::string wait(std::string site) {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(15),
                                [&] { return reached.contains(site); }))
            throw std::runtime_error("Missing recursive checkpoint: " + site);
        return reached.at(site);
    }
    void release(std::string site) {
        std::lock_guard lock(mutex);
        released.insert(std::move(site));
        condition.notify_all();
    }
    void stop() {
        std::lock_guard lock(mutex);
        stopped = true;
        condition.notify_all();
    }
};
struct RecursiveHarnessFixture {
    std::shared_ptr<RecursiveCheckpointGate> gate = std::make_shared<RecursiveCheckpointGate>();
    std::shared_ptr<BlockAfterJavaScriptResultJournal> journal;
    AdmittedRuntime                                    host;
    ProgramStoreHandle                      program_store;
    std::shared_ptr<ProgramCompiler>                   compiler;
    std::mutex                                         grants_mutex;
    std::map<std::string, ProgramChildSynthesisGrant>  grants;
    std::map<std::string, std::string>                 approved_sources;
    std::optional<ProgramHandle>                       root, child, grandchild, replacement;
    std::optional<ProgramHandoff>                      root_hold, child_hold, grandchild_hold;
    std::optional<ProgramVersion>                      root_version, replacement_version;
    std::optional<ProgramChildSynthesisRecord>         child_synthesis, grandchild_synthesis;
    std::string                                        grand_source, child_source, root_source;
    RunBudget root_budget{60000, 1000, 1000, 4, 32, 100, 4, 3, 12};
    RunBudget child_budget{45000, 500, 500, 2, 16, 50, 2, 2, 4};
    RunBudget grand_budget{30000, 100, 100, 1, 8, 20, 0, 0, 0};
    RecursiveHarnessFixture(std::shared_ptr<ProgramTransitionStore> transitions,
                            std::shared_ptr<ProgramStore>           programs,
                            std::shared_ptr<CheckpointStore>        checkpoints = {})
        : journal(std::make_shared<BlockAfterJavaScriptResultJournal>(std::move(transitions))),
          host(4, std::move(checkpoints), journal, {}, ExecutionGuarantee::Strict, true),
          program_store(programs),
          compiler(std::make_shared<ProgramCompiler>(
              host.registry, ProgramCompilerConfig{"program-runtime-test/v1"})) {
        completed_calls.store(0);
        host.profile =
            AdmittedRuntime::make_profile(host.registry, ExecutionGuarantee::Unmanaged, true);
        host.policy =
            AdmittedRuntime::make_policy(host.profile, ExecutionGuarantee::Unmanaged, true);
        host.catalog = std::make_shared<ProgramCatalog>(CatalogConfig{
            std::move(programs), host.registry, host.engines, "program-runtime-test/v1"});
        journal->release_result();
        journal->after_publication = [gate = gate](const auto& publication) {
            gate->observe(publication);
        };
        ProgramSynthesisGatewayConfig gateway;
        gateway.compiler           = compiler;
        gateway.catalog            = host.catalog;
        gateway.validate_semantics = [](const auto& proposal, const auto& bundle, const auto&) {
            return ProgramSynthesisSemanticDecision{
                digest('b'), digest('c'), true,
                json{{"source", proposal.source().source_hash()}, {"bundle", bundle.id()}}};
        };
        gateway.admission = [this](const auto&, const auto&, const auto&) {
            return ProgramAdmission{"tenant:runtime", host.profile, host.policy, {}};
        };
        RuntimeConfig config{host.catalog, host.checkpoints, {}, journal, 4};
        config.child_synthesis_gateway =
            std::make_shared<ProgramSynthesisGateway>(std::move(gateway));
        config.child_synthesis_grant_resolver =
            [this](std::string_view owner, std::string_view parent,
                   std::string_view id) -> std::optional<ProgramChildSynthesisGrant> {
            std::lock_guard lock(grants_mutex);
            auto            found = grants.find(std::string(id));
            if (found != grants.end() && found->second.data().owner_scope == owner &&
                found->second.data().parent_run_id == parent)
                return found->second;
            return std::nullopt;
        };
        host.runtime = std::make_unique<ProgramRuntime>(std::move(config));
        grand_source =
            recursive_graph_source(4,
                                   "yield ng.checkpoint({ready:true},'grandchild:ready');return "
                                   "yield ng.callCore('main',{},'grandchild:work');");
        child_source = recursive_graph_source(
            2,
            "yield ng.callCore('main',{},'child:work');const source=" + json(grand_source).dump() +
                ";yield ng.checkpoint({source:source},'child:propose');const descendant=yield "
                "ng.spawn('grandchild',{},'child:spawn');yield "
                "ng.checkpoint({child:descendant.child_run_id,replacement:'child-v2.json'},'child:"
                "swap');yield ng.callCore('main',{},'retired:forbidden');throw new Error('Retired "
                "child ran');");
        root_source = recursive_graph_source(
            1,
            "yield ng.callCore('main',{},'main:work');const source=" + json(child_source).dump() +
                ";yield ng.checkpoint({source:source},'main:propose');return yield "
                "ng.await(ng.spawn('child',{},'main:spawn'),55000,'main:await');");
        approved_sources.emplace("child.js", child_source);
        approved_sources.emplace("grandchild.js", grand_source);
        // This is the already-admitted JSON swap target, compiled before the session starts.
        replacement_version =
            admit("child-v2.js",
                  recursive_graph_source(
                      3,
                      "const result=yield "
                      "ng.await(ng.spawn('grandchild',{},'replacement:join'),30000,'replacement:"
                      "await');yield ng.callCore('main',{},'replacement:work');return "
                      "{generation:2,result:result};"),
                  child_budget);
    }
    ~RecursiveHarnessFixture() {
        gate->stop();
        if (root) root->cancel();
        root_hold.reset();
        child_hold.reset();
        grandchild_hold.reset();
        if (root) (void)root->wait();
    }
    ProgramVersion admit(std::string name, std::string source, RunBudget budget) {
        const RunBudget floor{1, 0, 0, 1, 1, 1, 0, 0, 0};
        auto            bundle =
            compiler->compile(ProgramSource::from_javascript(std::move(name), std::move(source)),
                              ProgramBudgetBounds{floor, budget});
        return host.catalog->admit(
            ProgramBundle::parse(bundle.serialize_canonical()),
            ProgramAdmission{"tenant:runtime", host.profile, host.policy, {}});
    }
    ProgramHandoff hold(ProgramHandle& handle, const std::string& site) {
        EXPECT_EQ(gate->wait(site), handle.run_id());
        auto point = handle.next_handoff();
        gate->release(site);
        (void)point.value();
        return point;
    }
    ProgramChildSynthesisRecord synthesize(ProgramHandle&  parent,
                                           ProgramHandoff& point,
                                           std::string     source_name,
                                           RunBudget       budget,
                                           std::string     binding) {
        const auto source = ProgramSource::from_javascript(
            source_name, point.value().at("source").get<std::string>());
        const auto approved = approved_sources.find(source_name);
        if (approved == approved_sources.end() ||
            source.source_hash() !=
                ProgramSource::from_javascript(source_name, approved->second).source_hash())
            throw std::runtime_error("Source is not an independently reviewed template instance");
        const auto run     = parent.snapshot();
        const auto lineage = journal->load_run_lineage_sync("tenant:runtime", parent.run_id());
        const auto version =
            host.catalog->resolve_version("tenant:runtime", parent.program_version_id());
        ProgramSynthesisProposalData request;
        request.owner_scope      = "tenant:runtime";
        request.parent_run_id    = parent.run_id();
        request.lineage_id       = lineage->lineage_id();
        request.source           = source;
        request.requested_budget = budget;
        request.created_at_ms    = run.updated_at_ms();
        ProgramChildSynthesisGrantData grant;
        grant.owner_scope                 = request.owner_scope;
        grant.parent_run_id               = parent.run_id();
        grant.parent_program_version_id   = version->id();
        grant.parent_policy_fingerprint   = version->policy_snapshot().fingerprint();
        grant.lineage_id                  = lineage->lineage_id();
        grant.expected_lineage_head_id    = lineage->id();
        grant.template_identity           = digest('a');
        grant.reviewed_source_identity    = program_synthesis_source_identity(source);
        grant.semantic_validator_identity = digest('b');
        grant.semantic_contract_identity  = digest('c');
        grant.child_budget_ceiling        = budget;
        grant.minimum_execution_guarantee = ExecutionGuarantee::Unmanaged;
        auto trusted                      = ProgramChildSynthesisGrant::create(std::move(grant));
        {
            std::lock_guard lock(grants_mutex);
            grants.emplace(trusted.id(), trusted);
        }
        return host.runtime->prepare_child_synthesis(
            "tenant:runtime", parent, point, ProgramSynthesisProposal::create(std::move(request)),
            trusted, std::move(binding));
    }
    void prepare_tree() {
        root_version = admit("orchestrator.js", root_source, root_budget);
        root         = host.runtime->start(
            "tenant:runtime", *root_version,
            ProgramInvocation{json::object(), root_budget, "session:recursive", {}});
        root_hold.emplace(hold(*root, "main:propose"));
        child_synthesis = synthesize(*root, *root_hold, "child.js", child_budget, "child");
        root_hold.reset();
        child = host.runtime->reconnect("tenant:runtime", gate->wait("child:propose"));
        child_hold.emplace(hold(*child, "child:propose"));
        grandchild_synthesis =
            synthesize(*child, *child_hold, "grandchild.js", grand_budget, "grandchild");
        child_hold.reset();
        grandchild = host.runtime->reconnect("tenant:runtime", gate->wait("grandchild:ready"));
        grandchild_hold.emplace(hold(*grandchild, "grandchild:ready"));
        child_hold.emplace(hold(*child, "child:swap"));
    }
    void replace_child() {
        const auto target = *replacement_version;
        const auto state  = child_hold->value();
        replacement       = host.runtime->replace(
            "tenant:runtime", std::move(*child_hold), target,
            ProgramInvocation{json{{"handoff", state}, {"previous_run_id", child->run_id()}},
                              child->snapshot().remaining_budget(),
                              "session:recursive/replacement",
                                    {}});
        child_hold.reset();
    }
};
}  // namespace
TEST_P(ProgramChildSynthesisPersistence,
       RecursiveDifferentTopologiesRetainGrandchildAcrossChildReplacement) {
    RecursiveHarnessFixture test(backend(), program_backend());
    test.prepare_tree();
    const auto child_root = test.child->run_id(), grandchild_root = test.grandchild->run_id();
    EXPECT_EQ(test.child->snapshot().child_depth(), 1U);
    EXPECT_EQ(test.grandchild->snapshot().child_depth(), 2U);
    EXPECT_EQ(test.child->snapshot().remaining_budget().max_dynamic_compiles, 1U);
    test.replace_child();
    const auto initial = test.journal->load_sync("tenant:runtime", test.replacement->run_id());
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->logical_run_id(), child_root);
    ASSERT_EQ(initial->children().size(), 1U);
    EXPECT_EQ(initial->children().front().child_run_id, grandchild_root);
    test.grandchild_hold.reset();
    const auto result = test.root->wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.output().at("generation"), 2);
    EXPECT_EQ(test.child->wait().id(), test.replacement->wait().id());
    EXPECT_EQ(completed_calls.load(), 10U);
    const auto main = test.root->snapshot();
    ASSERT_EQ(main.children().size(), 1U);
    EXPECT_EQ(main.children().front().child_run_id, child_root);
    EXPECT_EQ(main.children().front().terminal_generation, 2U);
    EXPECT_EQ(main.children().front().terminal_result->run_id(), test.replacement->run_id());
    const auto lineage = test.journal->load_run_lineage_sync("tenant:runtime", child_root);
    const auto generation =
        test.journal->load_generation_sync("tenant:runtime", lineage->lineage_id(), 2);
    EXPECT_TRUE(does_program_child_generation_result_bind(
        main, main.children().front(), *generation, *lineage, test.replacement->snapshot()));
    auto forged                = main.children().front();
    forged.terminal_generation = 3;
    EXPECT_FALSE(does_program_child_generation_result_bind(main, forged, *generation, *lineage,
                                                           test.replacement->snapshot()));
    forged              = main.children().front();
    forged.child_run_id = grandchild_root;
    EXPECT_FALSE(does_program_child_generation_result_bind(main, forged, *generation, *lineage,
                                                           test.replacement->snapshot()));
    EXPECT_EQ(test.replacement->snapshot().children().front().child_run_id, grandchild_root);
    EXPECT_EQ(test.replacement->wait().remaining_budget().max_dynamic_compiles, 1U);
    std::set<std::string> topologies;
    for (const auto& id :
         {test.root_version->bundle_id(),
          test.child_synthesis->data().artifacts.at("bundle").at("id").get<std::string>(),
          test.grandchild_synthesis->data().artifacts.at("bundle").at("id").get<std::string>(),
          test.host.catalog
              ->resolve_version("tenant:runtime", test.replacement->program_version_id())
              ->bundle_id()}) {
        const auto bundle = test.program_store->get_bundle(id);
        ASSERT_TRUE(bundle);
        topologies.insert(bundle->core_plan_identities().front().compiled_plan_identity);
    }
    EXPECT_EQ(topologies.size(), 4U);
    if (const auto* directory = std::getenv("NEOGRAPH_RECURSIVE_ARTIFACT_DIR")) {
        std::filesystem::create_directories(directory);
        const std::vector<std::pair<std::string, std::string>> bundles{
            {"main", test.root_version->bundle_id()},
            {"child-v1",
             test.child_synthesis->data().artifacts.at("bundle").at("id").get<std::string>()},
            {"child-v2", test.replacement_version->bundle_id()},
            {"grandchild",
             test.grandchild_synthesis->data().artifacts.at("bundle").at("id").get<std::string>()}};
        for (const auto& [name, id] : bundles) {
            std::ofstream file(std::filesystem::path(directory) / (name + ".json"));
            file << test.program_store->get_bundle(id)->serialize_canonical();
        }
        std::ofstream manifest(std::filesystem::path(directory) / "session.json");
        manifest << json{{"session", "session:recursive"},
                         {"main", main.run_id()},
                         {"child", child_root},
                         {"active_child_run", test.replacement->run_id()},
                         {"grandchild", grandchild_root},
                         {"child_generation", 2},
                         {"result", json::parse(result.serialize_canonical())}}
                        .dump(2);
    }
}

namespace {
using LineageRead = std::function<std::optional<ProgramRunLineage>()>;
thread_local std::string last_loaded_run_id;

// Routes the first lineage read that directly follows a read of the same run's record on one
// non-test thread (the replacement generation's inherited-child check) through `action`.
// `action` runs between those two reads, exactly where a concurrent publisher can interleave.
std::shared_ptr<std::atomic<unsigned>> intercept_lineage_read_after_run_read(
    RecursiveHarnessFixture&                                             test,
    std::set<std::string>                                                skipped_runs,
    std::function<std::optional<ProgramRunLineage>(const LineageRead&)> action) {
    auto hits = std::make_shared<std::atomic<unsigned>>(0);
    test.journal->filter_run_read = [](std::string_view             run_id,
                                       std::optional<ProgramRunRecord> record) {
        last_loaded_run_id.assign(run_id);
        return record;
    };
    test.journal->filter_run_lineage_read =
        [hits, skipped = std::move(skipped_runs), action = std::move(action),
         test_thread = std::this_thread::get_id()](std::string_view run_id,
                                                   const LineageRead& read) {
            const bool follows_run_read = last_loaded_run_id == run_id;
            last_loaded_run_id.clear();
            if (!follows_run_read || std::this_thread::get_id() == test_thread ||
                skipped.contains(std::string(run_id)) || hits->fetch_add(1) != 0)
                return read();
            return action(read);
        };
    return hits;
}
}  // namespace
TEST_P(ProgramChildSynthesisPersistence, InheritedChildLookupSurvivesParentHeadAdvance) {
    struct Completion {
        std::mutex              mutex;
        std::condition_variable condition;
        bool                    published = false;
    };
    auto completion = std::make_shared<Completion>();
    RecursiveHarnessFixture test(backend(), program_backend());
    test.prepare_tree();
    const std::set<std::string> predecessors{test.root->run_id(), test.child->run_id(),
                                             test.grandchild->run_id()};
    test.journal->after_publication = [observe = test.journal->after_publication, completion,
                                       predecessors, grandchild = test.grandchild->run_id()](
                                          const ProgramTransitionPublication& publication) {
        observe(publication);
        const auto& record = publication.run_record;
        if (predecessors.contains(record.run_id())) return;
        for (const auto& child : record.children()) {
            if (child.child_run_id != grandchild || !child.terminal_result) continue;
            {
                std::lock_guard lock(completion->mutex);
                completion->published = true;
            }
            completion->condition.notify_all();
        }
    };
    auto release_grandchild =
        std::make_shared<std::optional<ProgramHandoff>>(std::move(test.grandchild_hold));
    test.grandchild_hold.reset();
    // The grandchild finishes, and records its result on the replacement's run, between the
    // replacement's read of its own record and its read of the lineage head.
    const auto hits = intercept_lineage_read_after_run_read(
        test, predecessors, [release_grandchild, completion](const LineageRead& read) {
            release_grandchild->reset();
            std::unique_lock lock(completion->mutex);
            if (!completion->condition.wait_for(lock, std::chrono::seconds(30),
                                                [&] { return completion->published; }))
                throw std::runtime_error("Grandchild completion was never published");
            lock.unlock();
            return read();
        });
    test.replace_child();
    const auto result = test.root->wait();
    EXPECT_GE(hits->load(), 1U) << "the inherited-child lineage read was never intercepted";
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.output().at("generation"), 2);
}


TEST_P(ProgramChildSynthesisPersistence, InheritedChildRejectsSupersededParentGeneration) {
    RecursiveHarnessFixture test(backend(), program_backend());
    test.prepare_tree();
    const std::set<std::string> predecessors{test.root->run_id(), test.child->run_id(),
                                             test.grandchild->run_id()};
    // The generation-1 head of the child lineage is what a lagging reader returns after
    // generation 2 became active; the replacement must not act on it.
    const auto superseded = test.journal->load_run_lineage_sync("tenant:runtime", test.child->run_id());
    ASSERT_TRUE(superseded);
    ASSERT_EQ(superseded->active_generation(), 1U);
    const auto hits = intercept_lineage_read_after_run_read(
        test, predecessors, [superseded](const LineageRead&) { return superseded; });
    test.replace_child();
    const auto result = test.replacement->wait();
    EXPECT_GE(hits->load(), 1U) << "the inherited-child lineage read was never intercepted";
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed) << result.serialize_canonical();
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_CHILD_GENERATION");
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS) && !defined(_WIN32)
TEST_P(ProgramChildSynthesisPersistence, RecursiveTreeReopensAfterNestedReplacementCommit) {
    if (GetParam() == "Memory") GTEST_SKIP() << "Process recovery requires durable stores";
    if (const auto* inherited = std::getenv("NEOGRAPH_RECURSIVE_DB")) database = inherited;
    setenv("NEOGRAPH_RECURSIVE_DB", database.c_str(), 1);
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT(([&] {
                    RecursiveHarnessFixture test(backend(), program_backend(),
                                                 checkpoint_backend());
                    test.prepare_tree();
                    {
                        json grants = json::array();
                        for (const auto& [_, grant] : test.grants)
                            grants.push_back(grant.serialize_canonical());
                        std::ofstream saved(database + ".tree");
                        saved << json{{"root", test.root->run_id()},
                                      {"child", test.child->run_id()},
                                      {"grandchild", test.grandchild->run_id()},
                                      {"grants", grants},
                                      {"executed", completed_calls.load()}}
                                     .dump();
                    }
                    test.journal->after_publication = [&](const auto& publication) {
                        if (publication.run_generation &&
                            publication.run_generation->replacement_receipt() &&
                            publication.run_record.child_depth() == 1)
                            std::_Exit(77);
                        test.gate->observe(publication);
                    };
                    test.replace_child();
                    std::_Exit(78);
                }()),
                ::testing::ExitedWithCode(77), "");
    unsetenv("NEOGRAPH_RECURSIVE_DB");
    std::ifstream input(database + ".tree");
    const auto    saved   = json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
    const auto    root    = saved.at("root").get<std::string>(),
               child      = saved.at("child").get<std::string>(),
               grandchild = saved.at("grandchild").get<std::string>();
    RecursiveHarnessFixture recovered(backend(), program_backend(), checkpoint_backend());
    for (const auto& encoded : saved.at("grants")) {
        auto grant = ProgramChildSynthesisGrant::parse(encoded.get<std::string>());
        recovered.grants.emplace(grant.id(), grant);
    }
    const auto child_before_recovery =
        recovered.journal->load_run_lineage_sync("tenant:runtime", child);
    ASSERT_TRUE(child_before_recovery);
    recovered.root    = recovered.host.runtime->reconnect("tenant:runtime", root);
    const auto result = recovered.root->wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.output().at("generation"), 2);
    EXPECT_EQ(completed_calls.load() + saved.at("executed").get<unsigned>(), 10U);
    const auto parent = recovered.root->snapshot();
    ASSERT_EQ(parent.children().size(), 1U);
    EXPECT_EQ(parent.children().front().child_run_id, child);
    EXPECT_EQ(parent.children().front().terminal_generation, 2U);
    const auto active = recovered.host.runtime->reconnect("tenant:runtime", child);
    EXPECT_EQ(active.snapshot().logical_run_id(), child);
    ASSERT_EQ(active.snapshot().children().size(), 1U);
    EXPECT_EQ(active.snapshot().children().front().child_run_id, grandchild);
    const auto child_remaining = active.wait().remaining_budget();
    EXPECT_EQ(child_remaining.max_dynamic_compiles, 1U);
    EXPECT_LE(child_remaining.wall_time_ms,
              child_before_recovery->remaining_budget().wall_time_ms);
    EXPECT_LE(child_remaining.model_tokens,
              child_before_recovery->remaining_budget().model_tokens);
    EXPECT_LE(child_remaining.monetary_microunits,
              child_before_recovery->remaining_budget().monetary_microunits);
    input.close();
    std::filesystem::remove(database + ".tree");
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence, RecursiveCancellationFollowsTheReplacementAndGrandchild) {
    RecursiveHarnessFixture test(backend(), program_backend(), checkpoint_backend());
    test.prepare_tree();
    test.replace_child();
    EXPECT_TRUE(test.root->cancel());
    test.grandchild_hold.reset();
    EXPECT_EQ(test.root->wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(test.child->wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(test.replacement->wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(test.grandchild->wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(test.replacement->wait().remaining_budget().max_dynamic_compiles, 1U);
}

namespace {
struct StaticChildRecoveryFixture {
    std::shared_ptr<BlockAfterJavaScriptResultJournal> journal;
    AdmittedRuntime                                    host;
    std::optional<ProgramVersion> root_version, child_version, grand_version, successor_version;
    std::optional<ProgramHandle>  root;
    std::mutex                    mutex;
    std::condition_variable       ready;
    std::optional<ProgramHandoff> root_hold, grand_hold;
    RunBudget                     root_budget{60000, 1000, 1000, 4, 128, 100, 1, 3, 8};
    RunBudget                     child_budget{45000, 500, 500, 2, 64, 50, 0, 2, 4};
    RunBudget                     grand_budget{30000, 100, 100, 1, 32, 20, 0, 0, 0};
    std::function<void()>         binding_observer;
    std::atomic<bool>             hold_checkpoints{true};

    StaticChildRecoveryFixture(std::shared_ptr<ProgramTransitionStore> transitions,
                               std::shared_ptr<CheckpointStore>        checkpoints,
                               bool                                    recover,
                               bool                                    hold,
                               bool                                    replace,
                               bool                                    external_effect = false)
        : journal(std::make_shared<BlockAfterJavaScriptResultJournal>(std::move(transitions))),
          host(4, std::move(checkpoints), journal, {}, ExecutionGuarantee::Unmanaged, true) {
        journal->release_result();
        PolicySnapshotBuilder policy;
        policy.id("static-recovery-policy")
            .semantic_version("1.0.0")
            .owner_scope("tenant:runtime")
            .admission_profile(host.profile)
            .budget_ceiling(BudgetLimits{60000, 10000, 1000000, 4, 256, 100, 4, 4, 32})
            .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
        host.policy      = std::move(policy).build();
        const auto admit = [&](int topology, std::string body, RunBudget budget) {
            ProgramCompiler compiler(host.registry, {"program-runtime-test/v1"});
            auto            bundle = compiler.compile(
                ProgramSource::from_javascript("static-child-recovery.js",
                                                          recursive_graph_source(topology, std::move(body))),
                ProgramBudgetBounds{RunBudget{1, 0, 0, 1, 1, 1, 0, 0, 0}, budget});
            return host.catalog->admit(
                bundle, ProgramAdmission{"tenant:runtime", host.profile, host.policy, {}});
        };
        grand_version = admit(3, external_effect
            ? "for(let i=0;i<11;i++)yield ng.checkpoint({cursor:i},'auditor:checkpoint');"
              "yield ng.emit({unknown:true},'auditor:external');return {answer:42};"
            : "for(let i=0;i<32;i++)yield ng.checkpoint({cursor:i},'auditor:checkpoint');"
              "return {answer:42};", grand_budget);
        child_version = admit(2,
                              "return yield ng.await(ng.spawn('child',{marker:2},'child:spawn'),"
                              "40000,'child:await');",
                              child_budget);
        root_version =
            admit(1,
                  replace ? "yield ng.spawn('child',{marker:1},'root:spawn');"
                            "yield ng.checkpoint({cursor:1},'root:swap');return {};"
                          : "return yield ng.await(ng.spawn('child',{marker:1},'root:spawn'),"
                            "55000,'root:await');",
                  root_budget);
        successor_version =
            admit(4,
                  "return yield ng.await(ng.spawn('child',{marker:1},'successor:spawn'),"
                  "55000,'successor:await');",
                  root_budget);
        const auto limits = [](const RunBudget& b) {
            return BudgetLimits{b.wall_time_ms,
                                b.model_tokens,
                                b.monetary_microunits,
                                b.max_concurrency,
                                b.max_program_operations,
                                b.max_core_steps,
                                static_cast<std::uint32_t>(b.max_dynamic_compiles),
                                b.max_child_depth,
                                b.max_total_children};
        };
        link_child_versions(host, *root_version, *child_version, limits(child_budget),
                            ExecutionGuarantee::Unmanaged);
        link_child_versions(host, *child_version, *grand_version, limits(grand_budget),
                            ExecutionGuarantee::Unmanaged);
        RuntimeConfig config{
            host.catalog,
            host.checkpoints,
            {},
            journal,
            4,
            [this, bindings = host.child_bindings](std::string_view owner, std::string_view version,
                                                   std::string_view name) {
                if (binding_observer) binding_observer();
                return bindings->resolve(owner, version, name);
            }};
        config.recover_existing_child_commands = recover;
        if (hold)
            config.checkpoint_handler = [this](ProgramHandle handle, ProgramHandoff point) {
                if (!hold_checkpoints.load()) return;
                std::lock_guard lock(mutex);
                if (handle.program_version_id() == grand_version->id())
                    grand_hold.emplace(std::move(point));
                else
                    root_hold.emplace(std::move(point));
                ready.notify_all();
            };
        host.runtime = std::make_unique<ProgramRuntime>(std::move(config));
    }
    ~StaticChildRecoveryFixture() {
        journal->after_publication = {};
        if (root) root->cancel();
        root_hold.reset();
        grand_hold.reset();
        host.runtime.reset();
    }
    void start(bool replace) {
        root = host.runtime->start(
            "tenant:runtime", *root_version,
            ProgramInvocation{json::object(), root_budget, "static-family", {}});
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(10),
                            [&] { return grand_hold && (!replace || root_hold); }))
            throw std::runtime_error("Static family did not reach its held checkpoints");
    }
};
}  // namespace
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS) && !defined(_WIN32)
void ProgramChildSynthesisPersistence::check_static_child_recovery(bool             replace,
                                                                   bool             recover,
                                                                   std::string_view fault) {
    if (GetParam() == "Memory") GTEST_SKIP() << "Process recovery requires durable stores";
    const bool checkpoint_crash = fault == "pending_checkpoint";
    const bool effect_crash = fault == "unknown_effect";
    if (const auto* inherited = std::getenv("NEOGRAPH_STATIC_CHILD_DB")) database = inherited;
    setenv("NEOGRAPH_STATIC_CHILD_DB", database.c_str(), 1);
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT(
        ([&] {
            StaticChildRecoveryFixture test(backend(), checkpoint_backend(), true, true, replace,
                                            effect_crash);
            test.start(replace);
            const auto save = [&](const ProgramRunRecord& active) {
                const auto    child      = active.children().front();
                const auto    descendant = test.journal->load_sync("tenant:runtime", child.child_run_id);
                std::ofstream saved(database + ".static-family");
                saved << json{{"root", test.root->run_id()},
                              {"active", active.run_id()},
                              {"child", child.child_run_id},
                              {"child_record_id", descendant->id()},
                              {"grandchild", descendant->children().front().child_run_id},
                              {"root_children", active.children().size()},
                              {"child_record", json::parse(child.link_receipt)},
                              {"dynamic_compiles", active.remaining_budget().max_dynamic_compiles},
                              {"root_budget",
                               active.remaining_budget().model_tokens +
                                   test.journal->latest_sync("tenant:runtime", active.run_id())
                                       ->inflight_reservation.model_tokens}}
                             .dump();
                saved.close();
                std::_Exit(77);
            };
            if (!replace) save(test.root->snapshot());
            std::optional<ProgramHandle> successor;
            test.journal->after_publication = [&](const auto& publication) {
                if (checkpoint_crash || effect_crash) {
                    if (publication.run_record.program_version_id() == test.grand_version->id() &&
                        !publication.commands.empty() && !publication.commands.back().completed() &&
                        publication.commands.back().command_ordinal() == 12)
                        save(*test.journal->load_sync("tenant:runtime", successor->run_id()));
                    return;
                }
                if (publication.run_record.program_version_id() == test.successor_version->id() &&
                    !publication.commands.empty() && !publication.commands.back().completed())
                    save(publication.run_record);
            };
            const auto state     = test.root_hold->value();
            successor = test.host.runtime->replace(
                "tenant:runtime", std::move(*test.root_hold), *test.successor_version,
                ProgramInvocation{
                    json{{"handoff", state}, {"previous_run_id", test.root->run_id()}},
                    test.root->snapshot().remaining_budget(),
                    "static-family:replacement",
                          {}});
            if (checkpoint_crash || effect_crash) {
                test.hold_checkpoints.store(false);
                test.grand_hold.reset();
            }
            (void)successor->wait();
            std::_Exit(78);
        }()),
        ::testing::ExitedWithCode(77), "");
    unsetenv("NEOGRAPH_STATIC_CHILD_DB");
    std::ifstream saved_file(database + ".static-family");
    const auto    saved = json::parse(std::string(std::istreambuf_iterator<char>(saved_file), {}));
    saved_file.close();
    StaticChildRecoveryFixture recovered(backend(), checkpoint_backend(), recover, false, replace,
                                          effect_crash);
    const auto                 child_id = saved.at("child").get<std::string>();
    if (fault == "missing_run" || fault == "disappearing_run") {
        auto hidden = std::make_shared<std::atomic<bool>>(fault == "missing_run");
        recovered.journal->filter_run_read = [child_id, hidden](auto id, auto record) {
            return id == child_id && hidden->load() ? std::nullopt : record;
        };
        if (fault == "disappearing_run") {
            auto resolutions           = std::make_shared<unsigned>(0);
            recovered.binding_observer = [hidden, resolutions] {
                // The second binding read is command preflight, after the
                // existing-child recovery decision but before dispatch.
                if (++*resolutions == 2) hidden->store(true);
            };
        }
    } else if (fault == "changed_link") {
        recovered.host.clear_child_bindings();
    } else if (fault == "changed_receipt") {
        auto binding = recovered.host.child_bindings->resolve(
            "tenant:runtime", recovered.root_version->id(), "child");
        const auto& old  = binding->receipt;
        binding->receipt = ModuleLinkReceipt::create(ModuleLinkReceiptData{
            old.owner_scope(), digest('9'), old.dependency_merkle_root(), old.child_name(),
            old.child_program_version_id(), old.child_bundle_id(),
            old.child_input_contract_fingerprint(), old.child_output_contract_fingerprint(),
            old.granted_capabilities(), old.granted_effects(), old.budget(),
            old.minimum_execution_guarantee()});
        recovered.host.bind_child(*recovered.root_version, "child", std::move(*binding));
    } else if (fault == "changed_input") {
        recovered.journal->filter_run_read = [child_id](auto id, auto record) {
            if (id != child_id || !record) return record;
            ProgramRunRecordData d;
            d.owner_scope         = record->owner_scope();
            d.run_id              = record->run_id();
            d.logical_run_id      = record->logical_run_id();
            d.program_version_id  = record->program_version_id();
            d.bundle_id           = record->bundle_id();
            d.binding_fingerprint = record->binding_fingerprint();
            d.invocation          = record->invocation();
            d.invocation.input    = json{{"marker", "changed"}};
            d.child_depth         = record->child_depth();
            d.continuation        = record->continuation();
            d.remaining_budget    = record->remaining_budget();
            d.children            = record->children();
            d.journal_head        = record->journal_head();
            d.event_sequence      = record->event_sequence();
            d.effect_sequence     = record->effect_sequence();
            d.created_at_ms       = record->created_at_ms();
            d.updated_at_ms       = record->updated_at_ms();
            return std::optional<ProgramRunRecord>(ProgramRunRecord::create(std::move(d)));
        };
    }
    recovered.root =
        recovered.host.runtime->reconnect("tenant:runtime", saved.at("root").get<std::string>());
    const auto result = recovered.root->wait();
    if (effect_crash) {
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Interrupted) << result.serialize_canonical();
        const auto child = recovered.journal->load_sync("tenant:runtime", child_id);
        const auto grand = recovered.journal->load_sync("tenant:runtime",
                                                   saved.at("grandchild").get<std::string>());
        ASSERT_TRUE(child);
        ASSERT_TRUE(grand);
        EXPECT_EQ(child->continuation().state, ContinuationState::Interrupted);
        EXPECT_EQ(grand->continuation().state, ContinuationState::Interrupted);
        ASSERT_EQ(child->children().size(), 1U);
        EXPECT_EQ(child->children().front().state, ProgramChildState::Dispatched);
        ASSERT_EQ(recovered.root->snapshot().children().size(), 1U);
        EXPECT_EQ(recovered.root->snapshot().children().front().state, ProgramChildState::Dispatched);
        EXPECT_EQ(recovered.journal->load_javascript_commands_sync("tenant:runtime", grand->run_id(), 0)
                      .size(), 23U);
        const auto events = recovered.journal->load_events_sync("tenant:runtime", grand->run_id(), 0);
        EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == ProgramEventKind::Emit;
        }));
        std::filesystem::remove(database + ".static-family");
        return;
    }
    if (!recover || (!fault.empty() && !checkpoint_crash)) {
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Interrupted)
            << result.serialize_canonical();
        ASSERT_TRUE(result.interrupt());
        EXPECT_TRUE(result.interrupt()->pending_effect);
        const auto storage = backend();
        ASSERT_TRUE(storage->load("tenant:runtime", child_id));
        EXPECT_EQ(storage->load("tenant:runtime", child_id)->id(),
                  saved.at("child_record_id").get<std::string>());
        EXPECT_EQ(storage
                      ->load_javascript_commands("tenant:runtime",
                                                 saved.at("grandchild").get<std::string>())
                      .size(),
                  2U);
        EXPECT_EQ(recovered.root->snapshot().children().size(), 1U);
        std::filesystem::remove(database + ".static-family");
        return;
    }
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.output(), (json{{"answer", 42}}));
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles,
              saved.at("dynamic_compiles").get<std::uint64_t>());
    EXPECT_EQ(recovered.root->run_id(), saved.at("active").get<std::string>());
    const auto active = recovered.root->snapshot();
    ASSERT_EQ(active.children().size(), 1U);
    EXPECT_EQ(active.children().front().child_run_id, saved.at("child").get<std::string>());
    EXPECT_EQ(json::parse(active.children().front().link_receipt), saved.at("child_record"));
    EXPECT_EQ(active.remaining_budget().model_tokens, saved.at("root_budget").get<std::uint64_t>());
    const auto child =
        recovered.journal->load_sync("tenant:runtime", saved.at("child").get<std::string>());
    ASSERT_TRUE(child);
    ASSERT_EQ(child->children().size(), 1U);
    EXPECT_EQ(child->children().front().child_run_id, saved.at("grandchild").get<std::string>());
    EXPECT_EQ(recovered.journal
                  ->load_javascript_commands_sync("tenant:runtime",
                                             saved.at("grandchild").get<std::string>(), 0)
                  .size(),
              64U);
    std::filesystem::remove(database + ".static-family");
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildCommandsRecoverAfterProcessLoss) {
    check_static_child_recovery(false);
}
TEST_P(ProgramChildSynthesisPersistence, InheritedStaticChildCommandsRecoverAfterReplacement) {
    check_static_child_recovery(true);
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryRequiresHostOptIn) {
    EXPECT_FALSE(RuntimeConfig{}.recover_existing_child_commands);
    check_static_child_recovery(false, false);
}
TEST_P(ProgramChildSynthesisPersistence, InheritedStaticChildRecoveryRequiresHostOptIn) {
    check_static_child_recovery(true, false);
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryRejectsMissingRun) {
    check_static_child_recovery(false, true, "missing_run");
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryRejectsChangedInput) {
    check_static_child_recovery(false, true, "changed_input");
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryRejectsMissingBinding) {
    check_static_child_recovery(false, true, "changed_link");
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryRejectsChangedReceipt) {
    check_static_child_recovery(false, true, "changed_receipt");
}
TEST_P(ProgramChildSynthesisPersistence, InheritedStaticChildRecoveryRejectsMissingRun) {
    check_static_child_recovery(true, true, "missing_run");
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryCannotCreateAfterRunDisappears) {
    check_static_child_recovery(false, true, "disappearing_run");
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryCompletesPendingCheckpoint) {
    check_static_child_recovery(true, true, "pending_checkpoint");
}
TEST_P(ProgramChildSynthesisPersistence, StaticChildRecoveryPreservesUnknownDescendantEffect) {
    check_static_child_recovery(true, true, "unknown_effect");
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence, CommandResultRetriesAnOptimisticSnapshotMiss) {
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>(backend());
    journal->release_result();
    journal->after_publication = [weak = std::weak_ptr(journal)](const auto& publication) {
        if (!publication.commands.empty() && !publication.commands.back().completed())
            if (const auto store = weak.lock()) store->command_head_misses.store(1);
    };
    AdmittedRuntime host(2, checkpoint_backend(), journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = host.admit_javascript(javascript_runtime_source("runtime-completed",
        "return yield ng.checkpoint({done:true},'snapshot:checkpoint');"));
    const auto result = host.runtime->run("tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "snapshot-miss", {}});
    journal->after_publication = {};
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(result.output(), (json{{"done", true}}));
    EXPECT_EQ(journal->load_javascript_commands_sync("tenant:runtime", result.run_id(), 0).size(), 2U);
}

TEST_P(ProgramChildSynthesisPersistence, FailedResultPublicationPersistsAmbiguousEffect) {
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>(backend());
    journal->release_result();
    std::atomic<bool> failed{false};
    journal->before_publication = [&](const auto& publication) {
        if (!publication.commands.empty() && publication.commands.back().completed() &&
            !failed.exchange(true))
            throw std::runtime_error("injected command result publication failure");
    };
    AdmittedRuntime host(2, checkpoint_backend(), journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = host.admit_javascript(javascript_runtime_source("runtime-completed",
        "yield ng.emit({effect:'once'},'ambiguous:emit');return {};"));
    const auto result = host.runtime->run("tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "ambiguous-publication", {}});
    journal->before_publication = {};
    EXPECT_TRUE(failed.load());
    ASSERT_EQ(result.status(), ProgramTerminalStatus::AmbiguousEffect) << result.serialize_canonical();
    ASSERT_TRUE(result.interrupt());
    ASSERT_TRUE(result.interrupt()->pending_effect);
    EXPECT_EQ(result.interrupt()->pending_effect->state(), ProgramPendingState::Ambiguous);
    const auto stored = journal->load_sync("tenant:runtime", result.run_id());
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->continuation().state, ContinuationState::AmbiguousEffect);
    EXPECT_EQ(journal->load_effects_sync("tenant:runtime", result.run_id(), 0).size(), 1U);
    EXPECT_EQ(journal->load_javascript_commands_sync("tenant:runtime", result.run_id(), 0).size(), 1U);
    auto recovered = host.runtime->reconnect("tenant:runtime", result.run_id());
    EXPECT_FALSE(recovered.cancel());
    EXPECT_EQ(recovered.wait().id(), result.id());
}

TEST_P(ProgramChildSynthesisPersistence, AmbiguousDescendantDoesNotStrandFamilyCompletion) {
    StaticChildRecoveryFixture test(backend(), checkpoint_backend(), true, false, false, true);
    std::atomic<bool> failed{false};
    test.journal->before_publication = [&](const auto& publication) {
        if (publication.run_record.program_version_id() == test.grand_version->id() &&
            !publication.commands.empty() && publication.commands.back().completed() &&
            publication.commands.back().command().kind() == JavaScriptCommandKind::Emit &&
            !failed.exchange(true))
            throw std::runtime_error("injected descendant result publication failure");
    };
    test.root = test.host.runtime->start("tenant:runtime", *test.root_version,
        ProgramInvocation{json::object(), test.root_budget, "ambiguous-family", {}});
    const auto result = test.root->wait();
    test.journal->before_publication = {};
    EXPECT_TRUE(failed.load());
    ASSERT_EQ(result.status(), ProgramTerminalStatus::AmbiguousEffect) << result.serialize_canonical();
    const auto root = test.root->snapshot();
    ASSERT_EQ(root.children().size(), 1U);
    EXPECT_EQ(root.children().front().state, ProgramChildState::Dispatched);
    const auto child = test.journal->load_sync("tenant:runtime", root.children().front().child_run_id);
    ASSERT_TRUE(child);
    ASSERT_EQ(child->children().size(), 1U);
    EXPECT_EQ(child->continuation().state, ContinuationState::AmbiguousEffect);
    EXPECT_EQ(child->children().front().state, ProgramChildState::Dispatched);
    const auto grand = test.journal->load_sync("tenant:runtime", child->children().front().child_run_id);
    ASSERT_TRUE(grand);
    EXPECT_EQ(grand->continuation().state, ContinuationState::AmbiguousEffect);
    EXPECT_FALSE(test.root->cancel());
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence,
       RecursiveReplacementRejectsParentAndOutputContractChanges) {
    RecursiveHarnessFixture test(backend(), program_backend());
    test.prepare_tree();
    const auto before = test.child->snapshot();
    auto       target =
        test.admit("valid-target.js", recursive_graph_source(3, "return {};"), test.child_budget);
    const auto call = [&](const ProgramVersion& version, std::string parent) {
        return test.host.runtime->replace(
            "tenant:runtime", test.child_hold->reference(), version,
            ProgramInvocation{json{{"handoff", test.child_hold->value()},
                                   {"previous_run_id", test.child->run_id()}},
                              before.remaining_budget(),
                              "recursive-negative",
                              {},
                              "",
                              std::move(parent),
                              1});
    };
    EXPECT_THROW(call(target, "foreign-parent"), ProgramDiagnosticError);
    auto bundle = test.compiler->compile(
        ProgramSource::from_javascript("wrong-output.js",
                                       recursive_graph_source(3, "return 'wrong';")),
        before.remaining_budget(), ContractRecord{}, ContractRecord{1, json{{"type", "string"}}});
    auto incompatible = test.host.catalog->admit(
        bundle, ProgramAdmission{"tenant:runtime", test.host.profile, test.host.policy, {}});
    try {
        (void)call(incompatible, test.root->run_id());
        FAIL() << "Expected output contract rejection";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_REPLACEMENT_CHILD_OUTPUT");
    }
    EXPECT_EQ(test.child->snapshot().id(), before.id());
    EXPECT_FALSE(test.grandchild->try_result());
}
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramCatalogTest, SQLiteCatalogWaitsForShortSharedDatabaseWriter) {
    AdmittedRuntime host;
    const auto      version = host.admit("runtime-completed");
    const auto      bundle  = host.store->get_bundle(version.bundle_id());
    const auto      path    = (std::filesystem::temp_directory_path() /
                       ("neograph-catalog-busy-" + Checkpoint::generate_id() + ".db"))
                          .string();
    {
        OwnedProgramStore<SQLiteProgramStore> store(path);
        store.publish_admitted(*bundle, version);
        sqlite3* writer = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &writer), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(writer, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr), SQLITE_OK);
        auto read = std::async(std::launch::async,
                               [&] { return store.get_version("tenant:runtime", version.id()); });
        EXPECT_EQ(read.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
        EXPECT_EQ(sqlite3_exec(writer, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(writer);
        const auto result = read.get();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->id(), version.id());
    }
    std::filesystem::remove(path);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence, HostSuccessorReservationBindsOwnerAndExactHead) {
    ChildSynthesisFixture test(backend(), true, program_backend(), checkpoint_backend());
    auto                  proposal = ProgramSynthesisProposal::create(test.proposal_data());
    const auto            before   = test.handle->snapshot();
    const auto head = test.journal->load_run_lineage_sync("tenant:runtime", test.handle->run_id());
    EXPECT_THROW(test.fixture.runtime->reserve_synthesis("tenant:other", *test.handle, *test.hold,
                                                         proposal, head->id()),
                 ProgramDiagnosticError);
    EXPECT_EQ(test.handle->snapshot().id(), before.id());
    ProgramRuntime other(
        RuntimeConfig{test.fixture.catalog, test.fixture.checkpoints, {}, test.journal, 1});
    EXPECT_THROW(
        other.reserve_synthesis("tenant:runtime", *test.handle, *test.hold, proposal, head->id()),
        ProgramDiagnosticError);
    auto reserved = test.fixture.runtime->reserve_synthesis("tenant:runtime", *test.handle,
                                                            *test.hold, proposal, head->id());
    EXPECT_EQ(reserved.data().remaining_after_reservation.max_dynamic_compiles, 2U);
    EXPECT_EQ(test.hold->reference().source_journal_head, test.handle->snapshot().journal_head());
    EXPECT_THROW(test.fixture.runtime->reserve_synthesis("tenant:runtime", *test.handle, *test.hold,
                                                         proposal, head->id()),
                 ProgramDiagnosticError);
    EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    auto reopened = GetParam() == "Memory" ? test.journal : backend();
    EXPECT_EQ(reopened->load_run_lineage("tenant:runtime", test.handle->run_id())
                  ->remaining_budget()
                  .max_dynamic_compiles,
              2U);
}

TEST_P(ProgramChildSynthesisPersistence, SuccessorReservationLostAcknowledgementDoesNotRefund) {
    ChildSynthesisFixture test(backend(), true, program_backend(), checkpoint_backend());
    auto                  proposal = ProgramSynthesisProposal::create(test.proposal_data());
    const auto head = test.journal->load_run_lineage_sync("tenant:runtime", test.handle->run_id());
    bool       lost = false;
    test.journal->after_publication = [&](const auto& publication) {
        if (!lost && publication.run_record.run_id() == test.handle->run_id() &&
            publication.run_record.remaining_budget().max_dynamic_compiles == 2) {
            lost = true;
            throw std::runtime_error("lost successor reservation acknowledgement");
        }
    };
    EXPECT_THROW(test.fixture.runtime->reserve_synthesis("tenant:runtime", *test.handle, *test.hold,
                                                         proposal, head->id()),
                 std::runtime_error);
    EXPECT_TRUE(lost);
    EXPECT_EQ(test.handle->snapshot().remaining_budget().max_dynamic_compiles, 2U);
    EXPECT_THROW(test.fixture.runtime->reserve_synthesis("tenant:runtime", *test.handle, *test.hold,
                                                         proposal, head->id()),
                 ProgramDiagnosticError);
    test.journal->after_publication = {};
}

TEST(ProgramRuntimeTest, HostCheckpointHandlerHoldsEachDurableBoundaryBeforeContinuation) {
    AdmittedRuntime             fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    std::mutex                  mutex;
    std::condition_variable     condition;
    std::vector<ProgramHandoff> holds;
    unsigned                    observed = 0;
    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 2};
    config.checkpoint_handler = [&](ProgramHandle, ProgramHandoff lease) {
        std::lock_guard lock(mutex);
        holds.push_back(std::move(lease));
        ++observed;
        condition.notify_all();
    };
    ProgramRuntime  runtime(std::move(config));
    ProgramCompiler compiler(fixture.registry, {"program-runtime-test/v1"});
    const auto      bundle = compiler.compile(
        ProgramSource::from_javascript(
            "host-checkpoint.js",
            javascript_runtime_source("runtime-completed",
                                           "yield ng.checkpoint({turn:1},'one');yield "
                                                "ng.checkpoint({turn:2},'two');return {done:true};")),
        ProgramBudgetBounds{RunBudget{1, 0, 0, 1, 1, 1, 0, 0, 0},
                            RunBudget{10000, 0, 0, 1, 8, 8, 0, 0, 0}});
    const auto version = fixture.catalog->admit(
        bundle, ProgramAdmission{"tenant:runtime", fixture.profile, fixture.policy, {}});
    auto handle = runtime.start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), RunBudget{10000, 0, 0, 1, 8, 8, 0, 0, 0}, {}, {}});
    for (unsigned turn = 1; turn <= 2; ++turn) {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(
            condition.wait_for(lock, std::chrono::seconds(3), [&] { return observed >= turn; }));
        EXPECT_EQ(observed, turn);
        EXPECT_EQ(holds.back().value().at("turn"), turn);
        EXPECT_FALSE(handle.try_result());
        holds.clear();
    }
    EXPECT_EQ(handle.wait().status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, PersistedChildInvocationComparesNestedInputCanonically) {
    ProgramPersistedInvocation original;
    original.input = json{
        {"payload", {{"phase", "critique"}, {"task", {{"z", 2}, {"a", 1}}}, {"draft", "text"}}}};
    auto reopened = original;
    reopened.input =
        json::parse(R"({"payload":{"draft":"text","phase":"critique","task":{"a":1,"z":2}}})");
    EXPECT_EQ(original, reopened);
    reopened.input["payload"]["draft"] = "changed";
    EXPECT_NE(original, reopened);
}

TEST_P(ProgramChildSynthesisPersistence, RecursiveParentCannotReuseDelegatedCompileBudget) {
    RecursiveHarnessFixture test(backend(), program_backend());
    const auto              leaf = recursive_graph_source(1, "return {}; ");
    test.approved_sources.emplace("reused-budget.js", leaf);
    test.root_source = recursive_graph_source(
        1, "yield ng.checkpoint({source:" + json(test.child_source).dump() +
               "},'main:propose');yield ng.spawn('child',{},'main:spawn');yield "
               "ng.checkpoint({source:" +
               json(leaf).dump() + "},'main:budget');return {}; ");
    test.root_version = test.admit("budget-parent.js", test.root_source, test.root_budget);
    test.root         = test.host.runtime->start(
        "tenant:runtime", *test.root_version,
        ProgramInvocation{json::object(), test.root_budget, "session:budget", {}});
    test.root_hold.emplace(test.hold(*test.root, "main:propose"));
    test.child_synthesis =
        test.synthesize(*test.root, *test.root_hold, "child.js", test.child_budget, "child");
    test.root_hold.reset();
    test.root_hold.emplace(test.hold(*test.root, "main:budget"));
    const auto before  = test.root->snapshot();
    const auto lineage = test.journal->load_run_lineage_sync("tenant:runtime", test.root->run_id());
    EXPECT_EQ(before.remaining_budget().max_dynamic_compiles, 3U);
    EXPECT_EQ(lineage->committed_descendant_budget().max_dynamic_compiles, 2U);
    const RunBudget attempted{1000, 10, 10, 1, 4, 4, 1, 1, 1};
    EXPECT_THROW(
        test.synthesize(*test.root, *test.root_hold, "reused-budget.js", attempted, "second-child"),
        ProgramChildSynthesisError);
    EXPECT_EQ(test.root->snapshot().id(), before.id());
    EXPECT_EQ(test.journal->load_child_syntheses_sync("tenant:runtime", test.root->run_id()).size(), 1U);
    ProgramSynthesisProposalData request;
    request.owner_scope      = "tenant:runtime";
    request.parent_run_id    = test.root->run_id();
    request.lineage_id       = lineage->lineage_id();
    request.source           = ProgramSource::from_javascript("reused-budget.js", leaf);
    request.requested_budget = RunBudget{1, 0, 0, 1, 1, 1, 0, 0, 0};
    auto       proposal      = ProgramSynthesisProposal::create(request);
    const auto reserved      = test.host.runtime->reserve_synthesis(
        "tenant:runtime", *test.root, *test.root_hold, proposal, lineage->id());
    EXPECT_EQ(reserved.data().remaining_after_reservation.max_dynamic_compiles, 2U);
    const auto next_head = test.journal->load_run_lineage_sync("tenant:runtime", test.root->run_id());
    EXPECT_THROW(test.host.runtime->reserve_synthesis("tenant:runtime", *test.root, *test.root_hold,
                                                      proposal, next_head->id()),
                 ProgramDiagnosticError);
    EXPECT_EQ(test.root->snapshot().remaining_budget().max_dynamic_compiles, 2U);
}
#endif
#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST_P(ProgramChildSynthesisPersistence,
       RecursiveReplacementLostAcknowledgementKeepsLogicalWaiters) {
    std::atomic<bool>       lost{false};
    RecursiveHarnessFixture test(backend(), program_backend(), checkpoint_backend());
    test.prepare_tree();
    test.journal->after_publication = [&](const auto& publication) {
        if (publication.run_generation && publication.run_generation->replacement_receipt() &&
            publication.run_record.child_depth() == 1 && !lost.exchange(true))
            throw std::runtime_error("lost family replacement acknowledgement");
        test.gate->observe(publication);
    };
    test.replace_child();
    test.grandchild_hold.reset();
    const auto result = test.root->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_TRUE(lost.load());
    EXPECT_EQ(completed_calls.load(), 10U);
    EXPECT_EQ(test.child->wait().id(), test.replacement->wait().id());
}
TEST_P(ProgramChildSynthesisPersistence, RecursiveConcurrentReconnectUsesTheRegisteredSuccessor) {
    std::promise<void>      committed, release;
    auto                    reached = committed.get_future();
    auto                    proceed = release.get_future().share();
    std::atomic<bool>       observed{false};
    RecursiveHarnessFixture test(backend(), program_backend(), checkpoint_backend());
    test.prepare_tree();
    const auto identity             = test.child->logical_run_id();
    test.journal->after_publication = [&](const auto& publication) {
        if (publication.run_generation && publication.run_generation->replacement_receipt() &&
            publication.run_record.child_depth() == 1 && !observed.exchange(true)) {
            committed.set_value();
            (void)proceed.wait_for(std::chrono::seconds(10));
        }
        test.gate->observe(publication);
    };
    auto replacing = std::async(std::launch::async, [&] { test.replace_child(); });
    EXPECT_EQ(reached.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    auto observer = std::async(std::launch::async, [&] {
        return test.host.runtime->reconnect("tenant:runtime", identity);
    });
    release.set_value();
    replacing.get();
    auto attached = observer.get();
    EXPECT_EQ(attached.run_id(), test.replacement->run_id());
    test.grandchild_hold.reset();
    const auto result = test.root->wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << result.serialize_canonical();
    EXPECT_EQ(attached.wait().id(), test.replacement->wait().id());
    EXPECT_EQ(completed_calls.load(), 10U);
}
#endif

namespace {

json join_scope_test_await_document(json document) {
    // Only the root owns the sealed Core name/definition; the Await body is an operation.
    auto body = std::move(document["root"]);
    json root{{"op", "await"},
              {"name", std::move(body["name"])},
              {"definition", std::move(body["definition"])},
              {"timeout_ms", 1000}};
    auto operation = json::object();
    for (const auto& [key, value] : body.items()) {
        if (key != "name" && key != "definition") operation[key] = value;
    }
    root["body"] = std::move(operation);
    document["root"] = std::move(root);
    return document;
}

json join_scope_test_child_document(std::string node_type) {
    auto document = parallel_map_child_document(std::move(node_type));
    // The immutable 3s link must cover the child's declared maximum, not just its minimum.
    document["declared_budget_requirements"][0]["maximum"] = 3000;
    return document;
}

std::unique_ptr<ProgramRuntime> join_scope_test_runtime(
    AdmittedRuntime& fixture, std::shared_ptr<TaskGraphFragmentStore> fragments = {}) {
    RuntimeConfig config{
        fixture.catalog, fixture.checkpoints, {}, fixture.journal,
        fixture.scheduler_thread_count,
        [bindings = fixture.child_bindings](std::string_view owner,
                                            std::string_view version,
                                            std::string_view binding) {
            return bindings->resolve(owner, version, binding);
        }};
    config.task_graph_fragments = fragments ? std::move(fragments) : fixture.task_graph_fragments.shared();
    config.task_graph_policy_resolver =
        [](std::string_view owner, std::string_view,
           std::string_view) -> std::optional<TaskGraphExpansionPolicy> {
            if (owner != "tenant:runtime") return std::nullopt;
            auto policy = expansion_policy();
            policy.limits.per_task_budget_ceiling.wall_time_ms = 3000;
            policy.limits.total_budget_ceiling.wall_time_ms = 9000;
            policy.limits.total_budget_ceiling.output_bytes = 12288;
            policy.template_allowlist.front().budget_ceiling.wall_time_ms = 3000;
            return policy;
        };
    config.child_quota = fixture.child_quota;
    config.hook_runtime = fixture.hook_runtime;
    config.core_tool_grant_resolver = fixture.core_tool_grant_resolver;
    config.core_provider_call_resolver = fixture.core_provider_call_resolver;
    config.require_core_provider_call_broker = fixture.require_core_provider_call_broker;
    return std::make_unique<ProgramRuntime>(std::move(config));
}

json join_scope_test_proposal(bool dependent_last) {
    json tasks = json::array();
    for (const auto id : {"first", "second", "third"}) {
        json dependencies = json::array();
        if (dependent_last && std::string_view(id) == "third")
            dependencies = json::array({"first", "second"});
        tasks.push_back(json{{"id", id},
                             {"template", "runtime-map/v1"},
                             {"input_bindings", json::array()},
                             {"depends_on", std::move(dependencies)},
                             {"budget", json{{"wall_time_ms", 3000}, {"model_tokens", 1}}}});
    }
    return json{{"schema_version", 1}, {"tasks", std::move(tasks)},
                {"join", json{{"kind", "all"}}}};
}

// This wraps the real in-memory durable store and fails a real dispatch publication
// only after the first two child nodes have entered. It exposes a partial-launch
// exception while their expansion task coroutines still borrow the launcher's locals.
class JoinPartialLaunchStore final : public SyncTaskGraphFragmentStore {
public:
    JoinPartialLaunchStore(std::shared_ptr<InMemoryTaskGraphFragmentStore> inner,
                          std::shared_future<void> children_entered)
        : inner_(std::move(inner)), children_entered_(std::move(children_entered)) {}

    TaskGraphPublishResult publish_sync(const TaskGraphFragmentRecord& record) override {
        return inner_->publish_sync(record);
    }
    std::optional<TaskGraphFragmentRecord> load_sync(std::string_view id) const override {
        return inner_->load_sync(id);
    }
    std::optional<TaskGraphFragmentRecord> find_published_lineage_sync(
        std::string_view owner, std::string_view parent, std::string_view operation) const override {
        return inner_->find_published_lineage_sync(owner, parent, operation);
    }
    std::optional<TaskGraphFragmentRecord> find_published_sync(
        std::string_view owner, std::string_view parent, std::string_view operation,
        std::string_view proposal) const override {
        return inner_->find_published_sync(owner, parent, operation, proposal);
    }
    TaskGraphPublishResult compare_update_sync(std::string_view id, std::uint64_t revision,
                                         const TaskGraphFragmentRecord& record) override {
        const auto active = std::count_if(record.tasks.begin(), record.tasks.end(),
                                         [](const auto& task) {
                                             return task.state == TaskGraphTaskState::Active;
                                         });
        if (active == 3 && !failed.exchange(true)) {
            if (children_entered_.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
                throw std::runtime_error("join partial launch children did not enter");
            throw std::runtime_error("join partial launch sentinel");
        }
        return inner_->compare_update_sync(id, revision, record);
    }

    std::atomic<bool> failed{false};

private:
    std::shared_ptr<InMemoryTaskGraphFragmentStore> inner_;
    std::shared_future<void> children_entered_;
};

struct JoinLateAttachmentGate {
    JoinLateAttachmentGate() : cancellation(cancelled.get_future().share()) {}

    void observe(const ProgramTransitionPublication& publication) {
        const auto& record = publication.run_record;
        if (record.child_depth() != 1) return;
        if (record.terminal_result()) {
            if (record.terminal_result()->status() == ProgramTerminalStatus::Cancelled &&
                !cancellation_observed.exchange(true))
                cancelled.set_value();
            return;
        }
        bool block = false;
        {
            std::lock_guard lock(mutex);
            if (std::find(children.begin(), children.end(), record.run_id()) != children.end())
                return;
            children.push_back(record.run_id());
            block = children.size() == 2;
        }
        if (!block) return;
        blocked.set_value();  // Second child's publication exists, but launch_child has not returned.
        if (cancellation.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            throw std::runtime_error("late attachment did not observe scoped child cancellation");
        released_after_cancellation.store(true);
    }

    std::mutex mutex;
    std::vector<std::string> children;
    std::promise<void> blocked;
    std::promise<void> cancelled;
    std::shared_future<void> cancellation;
    std::atomic<bool> cancellation_observed{false};
    std::atomic<bool> released_after_cancellation{false};
};

}  // namespace

TEST(ProgramRuntimeTest, AwaitScopedParallelMapTimeoutCancelsAndDrainsChildren) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    // BarrierNode's promise reports two real node entries; its synchronous barrier is
    // deliberately disabled so a startup/cancellation failure cannot strand a pool thread.
    overlap_barrier.reset();
    overlap_ready = std::make_shared<std::promise<void>>();
    auto entered = overlap_ready->get_future();
    AdmittedRuntime fixture(4);
    auto document = parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3);
    document = join_scope_test_await_document(std::move(document));
    document["declared_budget_requirements"][4]["maximum"] = 8;
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(document)),
        fixture.admit_document(join_scope_test_child_document("runtime-barrier")),
        BudgetLimits{3000, 333, 333, 1, 1, 6, 0, 0, 0});
    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 2, 8, 20, 0, 1, 3},
                          "trace-await-scoped-parallel-map", {}});
    auto done = std::async(std::launch::async, [parent] { return parent.wait(); });
    const auto entered_status = entered.wait_for(std::chrono::seconds(10));
    EXPECT_EQ(entered_status, std::future_status::ready);
    if (entered_status != std::future_status::ready) (void)parent.cancel();
    const auto settled = done.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(settled, std::future_status::ready)
        << "scoped Await timeout must cancel children rather than await their 3s run deadlines";
    if (settled != std::future_status::ready) (void)parent.cancel();
    const auto result = done.get();  // Root cancellation above also drains the failing-before case.
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut) << result.serialize_canonical();
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_EQ(blocking_calls.load(), 2U);  // No dispatch of the third item after scoped cancellation.
    EXPECT_EQ(blocking_peak.load(), 2U);
    EXPECT_EQ(blocking_active.load(), 0U); // No borrowed child work survives parent completion.
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    for (const auto& child : children) {
        EXPECT_EQ(child.state, ProgramChildState::Cancelled);
        EXPECT_EQ(fixture.runtime->reconnect("tenant:runtime", child.child_run_id).wait().status(),
                  ProgramTerminalStatus::Cancelled);
    }
    overlap_ready.reset();
}

TEST(ProgramRuntimeTest, AwaitScopedExpansionTimeoutCancelsActiveBatchAndStopsPendingTasks) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    overlap_barrier.reset();
    overlap_ready = std::make_shared<std::promise<void>>();
    auto entered = overlap_ready->get_future();
    AdmittedRuntime fixture(4);
    fixture.runtime = join_scope_test_runtime(fixture);
    auto document = expand_task_graph_document(join_scope_test_proposal(true), 3, 4, 2, 2);
    document = join_scope_test_await_document(std::move(document));
    document["declared_budget_requirements"][4]["maximum"] = 8;
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(document)),
        fixture.admit_document(join_scope_test_child_document("runtime-barrier")),
        BudgetLimits{3000, 333, 333, 1, 1, 6, 0, 0, 0});
    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json{{"item", "seed"}},
                          RunBudget{10000, 1000, 1000, 2, 8, 20, 1, 2, 3},
                          "trace-await-scoped-expansion", {}});
    auto done = std::async(std::launch::async, [parent] { return parent.wait(); });
    const auto entered_status = entered.wait_for(std::chrono::seconds(10));
    EXPECT_EQ(entered_status, std::future_status::ready);
    if (entered_status != std::future_status::ready) (void)parent.cancel();
    const auto settled = done.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(settled, std::future_status::ready)
        << "scoped expansion cancellation must settle before its 3s task budget expires";
    if (settled != std::future_status::ready) (void)parent.cancel();
    const auto result = done.get();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut) << result.serialize_canonical();
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 2U);
    EXPECT_EQ(blocking_active.load(), 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    for (const auto& child : children) {
        EXPECT_EQ(child.state, ProgramChildState::Cancelled);
        EXPECT_EQ(fixture.runtime->reconnect("tenant:runtime", child.child_run_id).wait().status(),
                  ProgramTerminalStatus::Cancelled);
    }
    const auto fragment = fixture.task_graph_fragments->find_published_lineage(
        "tenant:runtime", result.run_id(), "root.0");
    ASSERT_TRUE(fragment);
    EXPECT_TRUE(fragment->terminal);
    ASSERT_EQ(fragment->tasks.size(), 3U);
    for (const auto& task : fragment->tasks) {
        EXPECT_EQ(task.state, TaskGraphTaskState::Cancelled);
        if (task.task_id == "third") {
            EXPECT_EQ(task.attempt, 0U);
            EXPECT_FALSE(task.child_run_id);
        }
    }
    overlap_ready.reset();
}

TEST(ProgramRuntimeTest, ExpansionPartialLaunchFailureDrainsAlreadyBorrowingTasks) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    overlap_barrier.reset();
    overlap_ready = std::make_shared<std::promise<void>>();
    auto entered = overlap_ready->get_future().share();
    AdmittedRuntime fixture(4);
    auto fragments = std::make_shared<JoinPartialLaunchStore>(fixture.task_graph_fragments.as<InMemoryTaskGraphFragmentStore>(), entered);
    fixture.runtime = join_scope_test_runtime(fixture, fragments);
    auto document = expand_task_graph_document(join_scope_test_proposal(false), 3, 4, 2, 3);
    document["declared_budget_requirements"][4]["maximum"] = 8;
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(document)),
        fixture.admit_document(join_scope_test_child_document("runtime-barrier")),
        BudgetLimits{3000, 333, 333, 1, 1, 6, 0, 0, 0});
    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json{{"item", "seed"}},
                          RunBudget{10000, 1000, 1000, 3, 8, 20, 1, 2, 3},
                          "trace-expansion-partial-launch-drain", {}});
    auto done = std::async(std::launch::async, [parent] { return parent.wait(); });
    const auto settled = done.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(settled, std::future_status::ready);
    if (settled != std::future_status::ready) (void)parent.cancel();
    const auto result = done.get();
    EXPECT_TRUE(fragments->failed.load());
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed) << result.serialize_canonical();
    EXPECT_NE(result.serialize_canonical().find("join partial launch sentinel"), std::string::npos);
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 2U);
    EXPECT_EQ(blocking_active.load(), 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    for (const auto& child : children) {
        EXPECT_EQ(child.state, ProgramChildState::Cancelled);
        EXPECT_EQ(fixture.runtime->reconnect("tenant:runtime", child.child_run_id).wait().status(),
                  ProgramTerminalStatus::Cancelled);
    }
    overlap_ready.reset();
}

TEST(ProgramRuntimeTest, AwaitScopedParallelMapTimeoutCancelsLateAttachedChild) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    journal->release_result();
    auto gate = std::make_shared<JoinLateAttachmentGate>();
    auto blocked = gate->blocked.get_future();
    journal->after_publication = [gate](const auto& publication) { gate->observe(publication); };
    AdmittedRuntime fixture(4, {}, journal);
    auto document = parallel_map_document(json{{"literal", json::array({1, 2})}}, 2);
    document = join_scope_test_await_document(std::move(document));
    document["declared_budget_requirements"][4]["maximum"] = 8;
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(document)),
        fixture.admit_document(join_scope_test_child_document("runtime-blocking")),
        BudgetLimits{3000, 333, 333, 1, 1, 6, 0, 0, 0});
    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 2, 8, 20, 0, 1, 2},
                          "trace-await-scoped-late-attachment", {}});
    auto done = std::async(std::launch::async, [parent] { return parent.wait(); });
    const auto blocked_status = blocked.wait_for(std::chrono::seconds(10));
    EXPECT_EQ(blocked_status, std::future_status::ready);
    if (blocked_status != std::future_status::ready) (void)parent.cancel();
    const auto settled = done.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(settled, std::future_status::ready)
        << "the scoped timeout must cancel the first child, then its late-attached sibling";
    if (settled != std::future_status::ready) (void)parent.cancel();
    const auto result = done.get();
    journal->after_publication = {};
    EXPECT_TRUE(gate->cancellation_observed.load());
    EXPECT_TRUE(gate->released_after_cancellation.load());
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut) << result.serialize_canonical();
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_GE(blocking_calls.load(), 1U);
    EXPECT_EQ(blocking_active.load(), 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    for (const auto& child : children) {
        EXPECT_EQ(child.state, ProgramChildState::Cancelled);
        EXPECT_EQ(fixture.runtime->reconnect("tenant:runtime", child.child_run_id).wait().status(),
                  ProgramTerminalStatus::Cancelled);
    }
}

TEST(ProgramRuntimeTest, AwaitScopedExpansionPreservesSettledTasksWhenCancellingTheirBatch) {
    for (const bool complete_first : {false, true}) {
        SCOPED_TRACE(complete_first ? "completed-and-cancelled" : "failed-and-cancelled");
        map_fail_first_calls.store(0);
        map_fail_first_active.store(0);
        AdmittedRuntime fixture(4);
        fixture.runtime = join_scope_test_runtime(fixture);
        auto document = expand_task_graph_document(join_scope_test_proposal(true), 3, 4, 2, 2);
        document = join_scope_test_await_document(std::move(document));
        document["declared_budget_requirements"][4]["maximum"] = 8;
        auto child_document = join_scope_test_child_document("runtime-map-fail-first");
        child_document["root"]["definition"]["nodes"]["work"]["complete_first"] = complete_first;
        child_document["root"]["definition"]["channels"]["value"]["initial"] = "mixed-completed";
        const auto linked = link_child_versions(
            fixture, fixture.admit_document(std::move(document)),
            fixture.admit_document(std::move(child_document)),
            BudgetLimits{3000, 333, 333, 1, 1, 6, 0, 0, 0});
        auto parent = fixture.runtime->start(
            "tenant:runtime", linked.parent_version,
            ProgramInvocation{json{{"item", "seed"}},
                              RunBudget{10000, 1000, 1000, 2, 8, 20, 1, 2, 3},
                              "trace-mixed-scoped-expansion", {}});
        auto done = std::async(std::launch::async, [parent] { return parent.wait(); });
        const auto settled = done.wait_for(std::chrono::seconds(2));
        EXPECT_EQ(settled, std::future_status::ready);
        if (settled != std::future_status::ready) (void)parent.cancel();
        const auto result = done.get();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
        ASSERT_TRUE(result.failure());
        EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
        EXPECT_EQ(map_fail_first_calls.load(), 2U);
        EXPECT_EQ(map_fail_first_active.load(), 0U);
        const auto fragment = fixture.task_graph_fragments->find_published_lineage(
            "tenant:runtime", result.run_id(), "root.0");
        ASSERT_TRUE(fragment);
        EXPECT_TRUE(fragment->terminal);
        ASSERT_EQ(fragment->tasks.size(), 3U);
        unsigned preserved = 0, cancelled = 0;
        for (const auto& task : fragment->tasks) {
            EXPECT_NE(task.state, TaskGraphTaskState::Pending);
            EXPECT_NE(task.state, TaskGraphTaskState::Active);
            if (task.task_id == "third") {
                EXPECT_EQ(task.state, TaskGraphTaskState::Cancelled);
                EXPECT_EQ(task.attempt, 0U);
                EXPECT_FALSE(task.child_run_id);
                continue;
            }
            ASSERT_TRUE(task.child_run_id);
            const auto child = fixture.runtime->reconnect(
                "tenant:runtime", *task.child_run_id).wait();
            if (child.status() == ProgramTerminalStatus::Cancelled) {
                ++cancelled;
                EXPECT_EQ(task.state, TaskGraphTaskState::Cancelled);
            } else if (complete_first) {
                ++preserved;
                EXPECT_EQ(child.status(), ProgramTerminalStatus::Completed);
                EXPECT_EQ(task.state, TaskGraphTaskState::Completed);
                ASSERT_TRUE(task.output);
                EXPECT_EQ(task.output->at("channels").at("value").at("value"), "mixed-completed");
                EXPECT_FALSE(task.failure);
            } else {
                ++preserved;
                EXPECT_EQ(child.status(), ProgramTerminalStatus::Failed);
                EXPECT_EQ(task.state, TaskGraphTaskState::Failed);
                EXPECT_TRUE(task.failure);
                EXPECT_FALSE(task.output);
            }
        }
        EXPECT_EQ(preserved, 1U);
        EXPECT_EQ(cancelled, 1U);
    }
}
