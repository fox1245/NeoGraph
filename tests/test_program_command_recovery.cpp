// Program: recovery of a run that stopped inside a Core call, and a fork of
// a paused call.
//
// AStopInsideTheSecondCallAsksToReconcileIt: main() calls the Core twice. The first call finishes and its checkpoint
// becomes the run's; the process stops after the second call is recorded as
// pending, before it publishes anything of its own. Recovery treated the
// first call's checkpoint as the one to resume the second call from, found
// that it belongs to another call, and ended the run CheckpointIncompatible
// ("Exact Core resume checkpoint does not match a pending JavaScript callCore
// coordinate"). It now returns the run for reconciliation of the second
// call, as it does when the process stops inside the first. Found by the
// resume model harness (tests/test_program_resume_model.cpp).
//
// #402: a call that saved a Core checkpoint before the process stopped
// continues from it on reconnect; only a call without one is reconciled.
// #403: reconciling a stopped call as Completed continues main().
// #404: a plan that stopped before its first Core checkpoint is reconciled,
// as a JavaScript call in that state is, instead of P_RUN_RECOVERY_BLOCKED.
// #405: a fork of a paused call replays the provider call the paused
// invocation made before it asked, instead of sending and charging it again.
//
// A run resumed with resume() while a JavaScript command is pending is granted
// what its journal holds, as a reconciled one is. Before, the grant included
// the command's reservation, which the journal had already consumed, so the
// attempt's limits were wider than the journal's: a later command could send
// provider calls that the journal then refused to publish.

#include "program_resume_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

[[maybe_unused]] int sent_to(Peer& peer, const std::string& tag) {
    std::lock_guard lock(peer.mutex);
    const auto      found = peer.sent.find(tag);
    return found == peer.sent.end() ? 0 : found->second;
}

[[maybe_unused]] int body_runs(World& world, const std::string& value) {
    std::lock_guard lock(world.journal.mutex);
    return static_cast<int>(std::count_if(world.journal.runs.begin(), world.journal.runs.end(),
                                          [&](const auto& run) { return run.value == value; }));
}

[[maybe_unused]] int finished_runs(World& world, const std::string& value) {
    std::lock_guard lock(world.journal.mutex);
    return static_cast<int>(std::count_if(world.journal.runs.begin(), world.journal.runs.end(), [&](const auto& run) {
        return run.value == value && run.finished;
    }));
}

/// Runs `source` until the durable state is copied where `armed_when` says,
/// then reconnects the run on the copy in a new process. `granted` replaces the
/// fixture's run budget.
struct StoppedRun {
    TempDir                       live{"command-live"}, image{"command-image"};
    World                         world{next_world_id()};
    std::optional<ProgramVersion> version;
    std::shared_ptr<Peer>         original_peer, recovered_peer;
    std::unique_ptr<Host>         recovered;

    StoppedRun(const std::function<ProgramSource()>& source, json input,
               std::function<bool(const std::vector<std::string>&)> armed_when,
               std::optional<RunBudget>                             granted = std::nullopt) {
        world.crash.reset(live.path, image.path, -1, true);
        world.crash.armed_when = std::move(armed_when);
        {
            original_peer = std::make_shared<Peer>(&world.crash.alive);
            Host host(world, original_peer, live.path);
            host.granted = granted;
            version      = host.admit(source());
            (void)host.run(*version, world.run_id(), std::move(input));
        }
        world.crash.alive = true;
        recovered_peer    = std::make_shared<Peer>(&world.crash.alive);
        recovered         = std::make_unique<Host>(world, recovered_peer, image.path);
    }
};

[[maybe_unused]] std::function<ProgramSource()> javascript(std::string body) {
    return [body = std::move(body)] { return ProgramSource::from_javascript("calls.js", javascript_program(body)); };
}

// The copy is taken after the n-th operation named `what`.
std::function<bool(const std::vector<std::string>&)> after_nth(std::string what, long n) {
    return [what = std::move(what), n](const std::vector<std::string>& seen) {
        return seen.back() == what && std::count(seen.begin(), seen.end(), what) == n;
    };
}

}  // namespace

#if defined(NEOGRAPH_PROGRAM_RESUME_MODEL_HAVE_QUICKJS)
TEST(ProgramCommandRecovery, AStopInsideTheSecondCallAsksToReconcileIt) {
    const auto source = javascript_program(R"JS(
  const first = yield ng.callCore("main", {value: "ok:a"}, "first");
  const second = yield ng.callCore("main", {value: "ok:b"}, "second");
  return {out: [first.out, second.out]};)JS");
    TempDir live("command-live"), image("command-image");
    World   world(next_world_id());
    // Copy the durable state at the publication that records the second
    // call as pending: the second publication after the first call's
    // checkpoint.
    world.crash.reset(live.path, image.path, -1, true);
    world.crash.armed_when = [](const std::vector<std::string>& seen) {
        const auto saved = std::find(seen.begin(), seen.end(), "completed");
        if (saved == seen.end()) return false;
        return std::count_if(saved, seen.end(),
                             [](const std::string& op) { return op.rfind("publish", 0) == 0; }) == 2;
    };
    std::optional<ProgramVersion> version;
    {
        Host host(world, std::make_shared<Peer>(&world.crash.alive), live.path);
        version = host.admit(ProgramSource::from_javascript("calls.js", source));
        (void)host.run(*version, world.run_id(), json::object());
        ASSERT_FALSE(world.crash.alive) << "the process stopped inside the second call";
    }
    const auto second_runs = [&world] {
        const std::lock_guard lock(world.journal.mutex);
        return std::count_if(world.journal.runs.begin(), world.journal.runs.end(),
                             [](const auto& run) { return run.value == "ok:b"; });
    };
    const auto before = second_runs();
    world.crash.alive = true;
    Host       recovered(world, std::make_shared<Peer>(&world.crash.alive), image.path);
    const auto paused = recovered.reconnect(world.run_id());
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_TRUE(paused.interrupt()->pending_effect) << describe(paused);
    EXPECT_EQ(paused.interrupt()->pending_effect->operation_id(), "root.javascript.2");

    // The host can reconcile the second call: here it does not know whether
    // the call ran.
    const auto unknown = recovered.reconcile(world.run_id(), paused.interrupt()->pending_effect->call_id(),
                                             ProgramEffectReconciliation::Unknown, std::nullopt);
    EXPECT_EQ(unknown.status(), ProgramTerminalStatus::AmbiguousEffect) << describe(unknown);
    EXPECT_EQ(second_runs(), before) << "recovery does not run the second call again";
}


TEST(ProgramCommandRecovery, ReconcilingAStoppedCallContinuesMain) {
    // #403: the first call stops before its checkpoint; the host reconciles
    // it as Completed, and main() goes on to the second call.
    StoppedRun stopped(javascript(R"JS(
  const first = yield ng.callCore("main", {value: "ok:a"}, "first");
  const second = yield ng.callCore("main", {value: "ok:b"}, "second");
  return {out: [first.out, second.out]};)JS"),
                       json::object(), after_nth("publish running", 2));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto run_id = stopped.world.run_id();
    const auto paused = stopped.recovered->reconnect(run_id);
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_TRUE(paused.interrupt()->pending_effect) << describe(paused);
    EXPECT_EQ(paused.interrupt()->pending_effect->operation_id(), "root.javascript.1");
    const auto call_id = paused.interrupt()->pending_effect->call_id();
    const auto unknown =
        stopped.recovered->reconcile(run_id, call_id, ProgramEffectReconciliation::Unknown, std::nullopt);
    ASSERT_EQ(unknown.status(), ProgramTerminalStatus::AmbiguousEffect) << describe(unknown);
    const auto done = stopped.recovered->reconcile(run_id, call_id, ProgramEffectReconciliation::Completed,
                                                   json{{"out", "reconciled"}});
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << describe(done);
    EXPECT_EQ(done.output(), (json{{"out", json::array({"reconciled", "done:b"})}}));
    EXPECT_EQ(finished_runs(stopped.world, "ok:b"), 1);
}

TEST(ProgramCommandRecovery, ReconcilingALaterStoppedCallContinuesMain) {
    // #403: the run carries the first call's checkpoint when the second call
    // stops before its own; the pause for reconciliation keeps the second
    // call's reservation, and a Completed reconcile goes on to the third call.
    StoppedRun stopped(javascript(R"JS(
  const first = yield ng.callCore("main", {value: "ok:a"}, "first");
  const second = yield ng.callCore("main", {value: "ok:b"}, "second");
  const third = yield ng.callCore("main", {value: "ok:c"}, "third");
  return {out: [first.out, second.out, third.out]};)JS"),
                       json::object(), after_nth("publish running", 4));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto run_id = stopped.world.run_id();
    const auto paused = stopped.recovered->reconnect(run_id);
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_TRUE(paused.interrupt()->pending_effect) << describe(paused);
    EXPECT_EQ(paused.interrupt()->pending_effect->operation_id(), "root.javascript.2");
    const auto call_id = paused.interrupt()->pending_effect->call_id();
    const auto unknown =
        stopped.recovered->reconcile(run_id, call_id, ProgramEffectReconciliation::Unknown, std::nullopt);
    ASSERT_EQ(unknown.status(), ProgramTerminalStatus::AmbiguousEffect) << describe(unknown);
    const auto done = stopped.recovered->reconcile(run_id, call_id, ProgramEffectReconciliation::Completed,
                                                   json{{"out", "reconciled"}});
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << describe(done);
    EXPECT_EQ(done.output(), (json{{"out", json::array({"done:a", "reconciled", "done:c"})}}));
}

TEST(ProgramCommandRecovery, AStopAfterTheCallPausedAsksItsQuestion) {
    // #402: the second call's Core paused and saved its checkpoint; the
    // process stopped before the run published the pause. Reconnect continues
    // the call from that checkpoint, which asks the question again.
    StoppedRun stopped(javascript(R"JS(
  const first = yield ng.callCore("main", {value: "ok:a"}, "first");
  const second = yield ng.callCore("main", {value: "ask:b"}, "second");
  return {out: [first.out, second.out]};)JS"),
                       json::object(), after_nth("node_interrupt", 1));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto run_id = stopped.world.run_id();
    const auto asked  = stopped.recovered->reconnect(run_id);
    ASSERT_EQ(asked.status(), ProgramTerminalStatus::Interrupted) << describe(asked);
    ASSERT_TRUE(asked.interrupt()->pending_input) << describe(asked);
    EXPECT_EQ(asked.interrupt()->pending_input->operation_id(), "root.javascript.2");
    const auto done =
        stopped.recovered->resume(run_id, json{{"decision", "d1"}}, asked.interrupt()->pending_input->call_id());
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << describe(done);
    EXPECT_EQ(done.output(), (json{{"out", json::array({"done:a", "d1"})}}));
    EXPECT_EQ(finished_runs(stopped.world, "ok:a"), 1) << "the first call replays";
}

TEST(ProgramCommandRecovery, AStopAfterTheCallFinishedReplaysIt) {
    // #402: the second call finished and saved its last checkpoint; the
    // process stopped before the run published its result. Reconnect takes
    // the result from that checkpoint without running the call again.
    StoppedRun stopped(javascript(R"JS(
  const first = yield ng.callCore("main", {value: "ok:a"}, "first");
  const second = yield ng.callCore("main", {value: "ok:b"}, "second");
  return {out: [first.out, second.out]};)JS"),
                       json::object(), after_nth("completed", 2));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto done = stopped.recovered->reconnect(stopped.world.run_id());
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << describe(done);
    EXPECT_EQ(done.output(), (json{{"out", json::array({"done:a", "done:b"})}}));
    EXPECT_EQ(finished_runs(stopped.world, "ok:b"), 1) << "the second call is not run again";
}

TEST(ProgramCommandRecovery, AStopInsideAJoinContinuesItsMembers) {
    // #402: a join runs its members one at a time; the first finished, the
    // second paused and saved its checkpoint, and the process stopped before
    // the pause was published. Reconnect continues the join: the first member
    // replays from its checkpoint and the second asks again.
    StoppedRun stopped(javascript(R"JS(
  const replies = yield ng.all([
    ng.callCore("main", {value: "ok:a"}, "first"),
    ng.callCore("main", {value: "ask:b"}, "second")
  ], {max_in_flight: 1}, "sequential");
  return {out: replies.map((reply) => reply.out)};)JS"),
                       json::object(), after_nth("node_interrupt", 1));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto run_id = stopped.world.run_id();
    const auto asked  = stopped.recovered->reconnect(run_id);
    ASSERT_EQ(asked.status(), ProgramTerminalStatus::Interrupted) << describe(asked);
    ASSERT_TRUE(asked.interrupt()->pending_input) << describe(asked);
    EXPECT_EQ(asked.interrupt()->pending_input->operation_id(), "root.javascript.1/member/1");
    const auto done =
        stopped.recovered->resume(run_id, json{{"decision", "d1"}}, asked.interrupt()->pending_input->call_id());
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << describe(done);
    EXPECT_EQ(done.output(), (json{{"out", json::array({"done:a", "d1"})}}));
    EXPECT_EQ(finished_runs(stopped.world, "ok:a"), 1) << "the first member is not run again";
}

TEST(ProgramCommandRecovery, AForkReplaysTheCallThePausedInvocationMade) {
    // #405: the call asks after it called the provider. A fork of the pause
    // replays that provider call, as a resume of the source does: it is
    // neither sent nor charged again.
    TempDir live("fork-live");
    World   world(next_world_id());
    auto    peer = std::make_shared<Peer>(&world.crash.alive);
    Host    host(world, peer, live.path);
    const auto version = host.admit(ProgramSource::from_javascript(
        "fork.js", javascript_program(R"JS(
  const reply = yield ng.callCore("main", {value: "llm-ask:a"}, "only");
  return {out: [reply.out]};)JS")));
    const auto paused = host.run(version, world.run_id(), json::object());
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_EQ(sent_to(*peer, "a"), 1);
    const auto forked = host.fork(version, paused, world.run_id("run-fork"), json{{"decision", "d1"}},
                                  paused.interrupt()->pending_input->call_id());
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << describe(forked);
    EXPECT_EQ(forked.output(), (json{{"out", json::array({"reply-a/d1"})}}));
    EXPECT_EQ(sent_to(*peer, "a"), 1) << "the fork sent the paused invocation's call again";
    EXPECT_EQ(forked.remaining_budget().model_tokens, paused.remaining_budget().model_tokens)
        << "the fork charged the call's tokens again";
    EXPECT_EQ(forked.remaining_budget().monetary_microunits, paused.remaining_budget().monetary_microunits)
        << "the fork charged the call's money again";
}

#endif

TEST(ProgramCommandRecovery, AForkOfAPausedPlanCallReplaysItsProviderCall) {
    // #405, in a typed plan: the plan's one call asks after it called the
    // provider, and a fork of the pause replays the call.
    TempDir live("plan-fork-live");
    World   world(next_world_id());
    auto    peer = std::make_shared<Peer>(&world.crash.alive);
    Host    host(world, peer, live.path);
    const auto version = host.admit(ProgramSource::from_cpp_builder(
        "fork-plan", 2, plan_program(json{{"op", "call_core"}})));
    const auto paused = host.run(version, world.run_id(), json{{"value", "llm-ask:a"}});
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_EQ(sent_to(*peer, "a"), 1);
    const auto forked = host.fork(version, paused, world.run_id("run-fork"), json{{"decision", "d1"}},
                                  paused.interrupt()->pending_input->call_id());
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << describe(forked);
    EXPECT_EQ(sent_to(*peer, "a"), 1) << "the fork sent the paused invocation's call again";
    EXPECT_EQ(forked.remaining_budget().model_tokens, paused.remaining_budget().model_tokens);
    EXPECT_EQ(forked.remaining_budget().monetary_microunits, paused.remaining_budget().monetary_microunits);
}

// #431: a fork replays the paused invocation's provider calls from a durable
// provider-call journal. A host without one sends and charges a call that the
// fork runs again, and the fork's handle says so (ProgramHandle::notices()).
namespace {

// The fixture's reply always costs 1,234,567 nano-USD, which is more than the hold of a call against its small
// input limit. A call that the fork sends again is therefore also reported as P_MONEY_OVER_HOLD (#428); the
// notice about the host is the one that is not.
std::size_t over_hold_notices(const ProgramHandle& handle) {
    std::size_t count = 0;
    for (const auto& notice : handle.notices())
        if (notice.code == "P_MONEY_OVER_HOLD") ++count;
    return count;
}

ProgramNotice only_notice(const ProgramHandle& handle) {
    std::vector<ProgramNotice> others;
    for (auto& notice : handle.notices())
        if (notice.code != "P_MONEY_OVER_HOLD") others.push_back(std::move(notice));
    EXPECT_EQ(others.size(), 1u);
    return others.empty() ? ProgramNotice{} : std::move(others.front());
}

}  // namespace

#if defined(NEOGRAPH_PROGRAM_RESUME_MODEL_HAVE_QUICKJS)
TEST(ProgramForkNotice, AForkWithoutAProviderJournalSendsTheCallAgainAndSaysSo) {
    World world(next_world_id());
    auto  peer = std::make_shared<Peer>(&world.crash.alive);
    Host  host(world, peer, std::nullopt);
    const auto version = host.admit(ProgramSource::from_javascript(
        "fork.js", javascript_program(R"JS(
  const reply = yield ng.callCore("main", {value: "llm-ask:a"}, "only");
  return {out: [reply.out]};)JS")));
    const auto paused = host.run(version, world.run_id(), json::object());
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_EQ(sent_to(*peer, "a"), 1);

    const auto handle = host.start_fork(version, paused, world.run_id("run-fork"), json{{"decision", "d1"}},
                                        paused.interrupt()->pending_input->call_id());
    const auto forked = handle.wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << describe(forked);
    EXPECT_EQ(forked.output(), (json{{"out", json::array({"reply-a/d1"})}}));
    EXPECT_EQ(sent_to(*peer, "a"), 2) << "the call is sent again; the host has nothing to replay it from";
    EXPECT_LT(forked.remaining_budget().model_tokens, paused.remaining_budget().model_tokens);
    EXPECT_LT(forked.remaining_budget().monetary_microunits, paused.remaining_budget().monetary_microunits);

    EXPECT_EQ(over_hold_notices(handle), 1u) << "the call sent again is a call of its own";
    const auto notice = only_notice(handle);
    EXPECT_EQ(notice.code, "P_FORK_PROVIDER_CALLS_NOT_JOURNALED");
    EXPECT_FALSE(notice.message.empty());
    EXPECT_EQ(notice.witness.at("source_run_id"), json(paused.run_id()));
    EXPECT_EQ(notice.witness.at("source_attempt"), json(paused.attempt()));
    EXPECT_EQ(notice.witness.at("operation_id"), json(paused.interrupt()->pending_input->operation_id()));
    ASSERT_EQ(notice.witness.at("calls").size(), 1u);
    EXPECT_EQ(notice.witness.at("calls")[0].at("node"), json("work"));
    EXPECT_EQ(notice.witness.at("microunits"), json(paused.usage().monetary_microunits));
}

TEST(ProgramForkNotice, AForkWithAProviderJournalReplaysTheCallAndSaysNothing) {
    TempDir live("fork-notice-live");
    World   world(next_world_id());
    auto    peer = std::make_shared<Peer>(&world.crash.alive);
    Host    host(world, peer, live.path);
    const auto version = host.admit(ProgramSource::from_javascript(
        "fork.js", javascript_program(R"JS(
  const reply = yield ng.callCore("main", {value: "llm-ask:a"}, "only");
  return {out: [reply.out]};)JS")));
    const auto paused = host.run(version, world.run_id(), json::object());
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);

    const auto handle = host.start_fork(version, paused, world.run_id("run-fork"), json{{"decision", "d1"}},
                                        paused.interrupt()->pending_input->call_id());
    const auto forked = handle.wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << describe(forked);
    EXPECT_EQ(forked.output(), (json{{"out", json::array({"reply-a/d1"})}}));
    EXPECT_EQ(sent_to(*peer, "a"), 1);
    EXPECT_EQ(forked.remaining_budget().model_tokens, paused.remaining_budget().model_tokens);
    EXPECT_TRUE(handle.notices().empty());
}

TEST(ProgramForkNotice, ACallOfAnEarlierCommandIsNotNamedBecauseItIsReplayed) {
    // The provider call belongs to the first command, which finished and replays
    // from the command journal; the paused command made none. Nothing is sent
    // again, so nothing is said.
    World world(next_world_id());
    auto  peer = std::make_shared<Peer>(&world.crash.alive);
    Host  host(world, peer, std::nullopt);
    const auto version = host.admit(ProgramSource::from_javascript(
        "fork.js", javascript_program(R"JS(
  const first = yield ng.callCore("main", {value: "llm:a"}, "first");
  const second = yield ng.callCore("main", {value: "ask:b"}, "second");
  return {out: [first.out, second.out]};)JS")));
    const auto paused = host.run(version, world.run_id(), json::object());
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_EQ(sent_to(*peer, "a"), 1);
    ASSERT_FALSE(paused.monetary_records().empty()) << "the paused attempt did record the first command's call";

    const auto handle = host.start_fork(version, paused, world.run_id("run-fork"), json{{"decision", "d1"}},
                                        paused.interrupt()->pending_input->call_id());
    const auto forked = handle.wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << describe(forked);
    EXPECT_EQ(sent_to(*peer, "a"), 1) << "the finished command replays";
    EXPECT_TRUE(handle.notices().empty());
}
#endif

TEST(ProgramForkNotice, AForkOfAPausedPlanCallWithoutAProviderJournalSaysSo) {
    World world(next_world_id());
    auto  peer = std::make_shared<Peer>(&world.crash.alive);
    Host  host(world, peer, std::nullopt);
    const auto version = host.admit(ProgramSource::from_cpp_builder(
        "fork-plan", 2, plan_program(json{{"op", "call_core"}})));
    const auto paused = host.run(version, world.run_id(), json{{"value", "llm-ask:a"}});
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_EQ(sent_to(*peer, "a"), 1);

    const auto handle = host.start_fork(version, paused, world.run_id("run-fork"), json{{"decision", "d1"}},
                                        paused.interrupt()->pending_input->call_id());
    const auto forked = handle.wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << describe(forked);
    EXPECT_EQ(sent_to(*peer, "a"), 2);

    EXPECT_EQ(over_hold_notices(handle), 1u) << "the call sent again is a call of its own";
    const auto notice = only_notice(handle);
    EXPECT_EQ(notice.code, "P_FORK_PROVIDER_CALLS_NOT_JOURNALED");
    EXPECT_EQ(notice.witness.at("operation_id"), json(paused.interrupt()->pending_input->operation_id()));
    ASSERT_EQ(notice.witness.at("calls").size(), 1u);
}

TEST(ProgramForkNotice, ARunThatMadeNoProviderCallGivesNoNotice) {
    World world(next_world_id());
    auto  peer = std::make_shared<Peer>(&world.crash.alive);
    Host  host(world, peer, std::nullopt);
    const auto version = host.admit(ProgramSource::from_cpp_builder(
        "fork-plan", 2, plan_program(json{{"op", "call_core"}})));
    const auto paused = host.run(version, world.run_id(), json{{"value", "ask:a"}});
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_TRUE(paused.monetary_records().empty());

    const auto handle = host.start_fork(version, paused, world.run_id("run-fork"), json{{"decision", "d1"}},
                                        paused.interrupt()->pending_input->call_id());
    ASSERT_EQ(handle.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(handle.notices().empty());
    EXPECT_EQ(sent_to(*peer, "a"), 0);
}

TEST(ProgramCommandRecovery, APlanStoppedBeforeItsFirstCheckpointAsksToReconcile) {
    // #404: a plan sequence stops after it started, before its first call
    // saved a checkpoint. Reconnect returns the run for reconciliation of that
    // call instead of P_RUN_RECOVERY_BLOCKED; a Completed reconcile goes on to
    // the next call.
    StoppedRun stopped([] {
        return ProgramSource::from_cpp_builder(
            "stopped-plan", 2,
            plan_program(json{{"op", "sequence"},
                              {"children", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}}));
    },
                       json{{"value", "ok:a"}}, after_nth("publish running", 1));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto run_id = stopped.world.run_id();
    const auto paused = stopped.recovered->reconnect(run_id);
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_TRUE(paused.interrupt()->pending_effect) << describe(paused);
    EXPECT_EQ(paused.interrupt()->pending_effect->operation_id(), "root.0");
    const auto call_id = paused.interrupt()->pending_effect->call_id();
    const auto unknown =
        stopped.recovered->reconcile(run_id, call_id, ProgramEffectReconciliation::Unknown, std::nullopt);
    ASSERT_EQ(unknown.status(), ProgramTerminalStatus::AmbiguousEffect) << describe(unknown);
    const auto next = stopped.recovered->reconcile(run_id, call_id, ProgramEffectReconciliation::Completed,
                                                   json{{"out", "reconciled"}});
    // The next call of the sequence takes the reconciled result as its input,
    // which sets no channel: it asks (StepNode).
    ASSERT_EQ(next.status(), ProgramTerminalStatus::Interrupted) << describe(next);
    ASSERT_TRUE(next.interrupt()->pending_input) << describe(next);
    EXPECT_EQ(next.interrupt()->pending_input->operation_id(), "root.1");
    EXPECT_EQ(body_runs(stopped.world, "ok:a"), 0) << "recovery does not run the stopped call";
}

// A run resumed with resume() while a JavaScript command is pending is granted what its journal holds, as a run
// reconciled with reconcile() is. The host's answer does not say what the command spent, so the journal consumes the
// provider currency (tokens and money) the command reserved; only its wall time and Core steps return to the run.
#if defined(NEOGRAPH_PROGRAM_RESUME_MODEL_HAVE_QUICKJS)
namespace {

// Money in micro-USD. A call reports kCallMoney and is held at about 525 before it is sent. A grant of kOneCall admits
// one call and, once that call settled, no second hold (1235 + 525 > 1700).
constexpr std::uint64_t kOneCall = 1700;

RunBudget granted_money(std::uint64_t money) {
    auto budget                = Host::budget();
    budget.monetary_microunits = money;
    return budget;
}

int sent_total(Peer& peer) {
    std::lock_guard lock(peer.mutex);
    return peer.total;
}

int attempted_total(Peer& peer) {
    std::lock_guard lock(peer.mutex);
    return peer.attempted_total;
}

/// A second connection to the durable file: the journal as the recovered process left it.
struct JournalView {
    explicit JournalView(const std::filesystem::path& dir)
        : store(std::make_shared<SQLiteProgramTransitionStore>((dir / "durable.sqlite").string())) {}
    std::shared_ptr<SQLiteProgramTransitionStore> store;
};

/// Records the budget the attempt reports in its Started event and the journal remainder at that moment.
class StartedGrant final : public ProgramEventSink {
public:
    StartedGrant(const JournalView& journal, std::string run_id) : store_(journal.store), run_id_(std::move(run_id)) {}
    void on_event(const ProgramEvent& event) override {
        const auto* started = std::get_if<ProgramStartedEvent>(&event.payload);
        if (!started) return;
        grant = started->budget;
        if (const auto latest = store_->latest_sync(kOwner, run_id_)) journal_remaining = latest->remaining_budget;
    }

    std::optional<RunBudget> grant;
    RunBudget                journal_remaining{};

private:
    std::shared_ptr<SQLiteProgramTransitionStore> store_;
    std::string                                   run_id_;
};

/// How the host answers the stopped command: resume() with its result, or reconcile() after saying it does not know.
enum class Driver { Resume, Reconcile };

struct Driven {
    ProgramResult                paused;
    std::optional<ProgramResult> result;
    std::optional<RunBudget>     grant;
    RunBudget                    journal_remaining{};
    int                          original_sent = 0, recovered_sent = 0;
    int                          original_attempted = 0, recovered_attempted = 0;
};

/// Reconnects the recovered run and answers its pending JavaScript command with `value`.
Driven drive(StoppedRun& stopped, Driver driver, json value) {
    const auto run_id = stopped.world.run_id();
    Driven     out{stopped.recovered->reconnect(run_id), std::nullopt};
    if (out.paused.status() != ProgramTerminalStatus::Interrupted || !out.paused.interrupt() ||
        !out.paused.interrupt()->pending_effect)
        return out;
    const auto call_id = out.paused.interrupt()->pending_effect->call_id();
    JournalView journal(stopped.image.path);
    auto        sink    = std::make_shared<StartedGrant>(journal, run_id);
    auto&       runtime = stopped.recovered->runtime();
    if (driver == Driver::Reconcile) {
        const auto unknown = runtime.reconcile(kOwner, run_id,
                                               ProgramEffectResolution{call_id, ProgramEffectReconciliation::Unknown,
                                                                       std::nullopt, "trace-unknown", {}}).wait();
        if (unknown.status() != ProgramTerminalStatus::AmbiguousEffect) {
            out.paused = unknown;
            return out;
        }
    }
    auto handle = driver == Driver::Resume
                      ? runtime.resume(kOwner, run_id, ProgramResume{std::move(value), "trace-resume", sink, call_id})
                      : runtime.reconcile(kOwner, run_id,
                                          ProgramEffectResolution{call_id, ProgramEffectReconciliation::Completed,
                                                                  std::move(value), "trace-reconcile", sink});
    out.result              = handle.wait();
    out.grant               = sink->grant;
    out.journal_remaining   = sink->journal_remaining;
    out.original_sent       = sent_total(*stopped.original_peer);
    out.recovered_sent      = sent_total(*stopped.recovered_peer);
    out.original_attempted  = attempted_total(*stopped.original_peer);
    out.recovered_attempted = attempted_total(*stopped.recovered_peer);
    return out;
}

constexpr const char kOkThenLlm[] = R"JS(
  const first = yield ng.callCore("main", {value: "ok:a"}, "first");
  const second = yield ng.callCore("main", {value: "llm:b"}, "second");
  return {out: [first.out, second.out]};)JS";

constexpr const char kOkThenOk[] = R"JS(
  const first = yield ng.callCore("main", {value: "ok:a"}, "first");
  const second = yield ng.callCore("main", {value: "ok:b"}, "second");
  return {out: [first.out, second.out]};)JS";

constexpr const char kThreeLlm[] = R"JS(
  const first = yield ng.callCore("main", {value: "llm:a"}, "first");
  const second = yield ng.callCore("main", {value: "llm:b"}, "second");
  const third = yield ng.callCore("main", {value: "llm:c"}, "third");
  return {out: [first.out, second.out, third.out]};)JS";

// The grant the attempt starts with is what the journal holds for it.
void expect_grant_equals_journal(Driver driver) {
    StoppedRun stopped(javascript(kOkThenLlm), json::object(), after_nth("publish running", 2));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto driven = drive(stopped, driver, json{{"out", "reconciled"}});
    ASSERT_TRUE(driven.result) << describe(driven.paused);
    ASSERT_TRUE(driven.grant) << "the attempt did not report Started";
    EXPECT_EQ(driven.grant->monetary_microunits, driven.journal_remaining.monetary_microunits) << "money";
    EXPECT_EQ(driven.grant->model_tokens, driven.journal_remaining.model_tokens) << "tokens";
    EXPECT_EQ(driven.grant->wall_time_ms, driven.journal_remaining.wall_time_ms) << "wall time";
    EXPECT_EQ(driven.grant->max_core_steps, driven.journal_remaining.max_core_steps) << "Core steps";
    EXPECT_GT(driven.grant->wall_time_ms, 0u) << "the stopped command's wall time returns to the run";
}

// The stopped command consumed the money it reserved, so a later priced command is refused before it is sent.
void expect_later_priced_command_refused(Driver driver) {
    StoppedRun stopped(javascript(kOkThenLlm), json::object(), after_nth("publish running", 2),
                       granted_money(kOneCall));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto driven = drive(stopped, driver, json{{"out", "reconciled"}});
    ASSERT_TRUE(driven.result) << describe(driven.paused);
    EXPECT_EQ(driven.result->status(), ProgramTerminalStatus::BudgetExhausted) << describe(*driven.result);
    ASSERT_TRUE(driven.result->failure()) << describe(*driven.result);
    EXPECT_EQ(driven.result->failure()->code, "P_MONEY_BUDGET") << describe(*driven.result);
    EXPECT_EQ(sent_to(*stopped.original_peer, "b"), 0);
    EXPECT_EQ(sent_to(*stopped.recovered_peer, "b"), 0) << "the later priced call was sent";
    EXPECT_EQ(driven.result->usage().monetary_microunits, 0u);
}

// The run stopped right after the first provider call was journaled and before its command published a result. The
// provider calls of both processes together stay inside what the run was granted.
void expect_spend_across_a_stop_inside_the_grant(Driver driver) {
    StoppedRun stopped(javascript(kThreeLlm), json::object(), after_nth("provider call journaled", 1),
                       granted_money(kOneCall));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto driven = drive(stopped, driver, json{{"out", "reconciled-a"}});
    ASSERT_TRUE(driven.result) << describe(driven.paused);
    EXPECT_EQ(driven.result->status(), ProgramTerminalStatus::BudgetExhausted) << describe(*driven.result);
    ASSERT_TRUE(driven.result->failure()) << describe(*driven.result);
    EXPECT_EQ(driven.result->failure()->code, "P_MONEY_BUDGET") << describe(*driven.result);
    EXPECT_EQ(driven.original_sent, 1) << "the stop is after the first provider call";
    EXPECT_EQ(driven.recovered_attempted, 1) << "the recovered process attempted the second provider call";
    EXPECT_EQ(driven.recovered_sent, 0) << "the second provider call was refused before dispatch";
    EXPECT_LE(kCallMoney * static_cast<std::uint64_t>(driven.original_sent + driven.recovered_sent), kOneCall)
        << "calls sent: " << driven.original_sent << " before the stop, " << driven.recovered_sent << " after it";
}

}  // namespace

TEST(ProgramCommandRecovery, AResumedExternalCommandIsGrantedWhatTheJournalHolds) {
    expect_grant_equals_journal(Driver::Resume);
}

TEST(ProgramCommandRecovery, AReconciledExternalCommandIsGrantedWhatTheJournalHolds) {
    expect_grant_equals_journal(Driver::Reconcile);
}

TEST(ProgramCommandRecovery, ALaterCommandAfterAResumeStaysInsideTheJournalsBudget) {
    expect_later_priced_command_refused(Driver::Resume);
}

TEST(ProgramCommandRecovery, ALaterCommandAfterAReconcileStaysInsideTheJournalsBudget) {
    expect_later_priced_command_refused(Driver::Reconcile);
}

TEST(ProgramCommandRecovery, AResumedRunContinuesMainWithAnUnpricedCommand) {
    // The journal accepts what a later command publishes, so main() goes on as it does after a reconcile.
    StoppedRun stopped(javascript(kOkThenOk), json::object(), after_nth("publish running", 2));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto driven = drive(stopped, Driver::Resume, json{{"out", "reconciled"}});
    ASSERT_TRUE(driven.result) << describe(driven.paused);
    ASSERT_EQ(driven.result->status(), ProgramTerminalStatus::Completed) << describe(*driven.result);
    EXPECT_EQ(driven.result->output(), (json{{"out", json::array({"reconciled", "done:b"})}}));
    EXPECT_EQ(finished_runs(stopped.world, "ok:b"), 1);
}

TEST(ProgramCommandRecovery, AResumedExternalCommandIsNotDispatchedAgain) {
    // The first command is a priced call that was not sent before the stop. The host's answer settles it.
    StoppedRun stopped(javascript(R"JS(
  const first = yield ng.callCore("main", {value: "llm:a"}, "first");
  const second = yield ng.callCore("main", {value: "ok:b"}, "second");
  return {out: [first.out, second.out]};)JS"),
                       json::object(), after_nth("publish running", 2), granted_money(kOneCall));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto driven = drive(stopped, Driver::Resume, json{{"out", "reconciled-a"}});
    ASSERT_TRUE(driven.result) << describe(driven.paused);
    ASSERT_EQ(driven.result->status(), ProgramTerminalStatus::Completed) << describe(*driven.result);
    EXPECT_EQ(driven.result->output(), (json{{"out", json::array({"reconciled-a", "done:b"})}}));
    EXPECT_EQ(finished_runs(stopped.world, "ok:b"), 1);
    EXPECT_EQ(sent_to(*stopped.original_peer, "a"), 0);
    EXPECT_EQ(sent_to(*stopped.recovered_peer, "a"), 0) << "the resumed command was sent again";
    EXPECT_EQ(body_runs(stopped.world, "llm:a"), 0) << "the resumed command ran again";
}

TEST(ProgramCommandRecovery, APausedCallCoreOnPendingInputKeepsItsMonetaryReservationOnResume) {
    // A normal callCore pause on pending_input (not a mid-command stop on pending_effect) returns the command's
    // unspent monetary reservation to the run, so a priced command after resume() still has the full grant.
    World   world(next_world_id());
    auto    peer = std::make_shared<Peer>(&world.crash.alive);
    TempDir dir("command-pending-input");
    Host    host(world, peer, dir.path);
    host.granted       = granted_money(kOneCall);
    const auto version = host.admit(ProgramSource::from_javascript(
        "calls.js", javascript_program(R"JS(
  const first = yield ng.callCore("main", {value: "ask:a"}, "first");
  const second = yield ng.callCore("main", {value: "llm:b"}, "second");
  return {out: [first.out, second.out]};)JS")));
    const auto paused = host.run(version, world.run_id(), json::object());
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_TRUE(paused.interrupt() && paused.interrupt()->pending_input) << describe(paused);
    EXPECT_FALSE(paused.interrupt()->pending_effect.has_value());
    const auto done =
        host.resume(world.run_id(), json{{"decision", "d1"}}, paused.interrupt()->pending_input->call_id());
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << describe(done);
    EXPECT_EQ(done.output(), (json{{"out", json::array({"d1", "reply-b"})}}));
    EXPECT_EQ(sent_to(*peer, "b"), 1);
}

TEST(ProgramCommandRecovery, ProviderSpendAcrossAStopAndAResumeStaysInsideTheGrant) {
    expect_spend_across_a_stop_inside_the_grant(Driver::Resume);
}

TEST(ProgramCommandRecovery, ProviderSpendAcrossAStopAndAReconcileStaysInsideTheGrant) {
    expect_spend_across_a_stop_inside_the_grant(Driver::Reconcile);
}

TEST(ProgramCommandRecovery, AJoinOfPricedCallsAfterAResumeStaysInsideTheJournalsBudget) {
    // Starting "first" reserves all 5000 micro-USD of the run's grant into inflight_reservation, leaving 0 in the
    // journal. Because "first" stopped before publishing its result, resume() keeps that 5000 micro-USD reservation
    // consumed; the first member of the join is therefore refused before dispatch.
    constexpr std::uint64_t kGrant = 5000;
    StoppedRun stopped(javascript(R"JS(
  const first = yield ng.callCore("main", {value: "llm:a"}, "first");
  const rest = yield ng.all([
    ng.callCore("main", {value: "llm:b"}, "b"),
    ng.callCore("main", {value: "llm:c"}, "c"),
    ng.callCore("main", {value: "llm:d"}, "d"),
    ng.callCore("main", {value: "llm:e"}, "e")
  ], {max_in_flight: 1}, "rest");
  return {out: [first.out, rest.length]};)JS"),
                       json::object(), after_nth("provider call journaled", 1), granted_money(kGrant));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto driven = drive(stopped, Driver::Resume, json{{"out", "reconciled-a"}});
    ASSERT_TRUE(driven.result) << describe(driven.paused);
    EXPECT_EQ(driven.result->status(), ProgramTerminalStatus::BudgetExhausted) << describe(*driven.result);
    ASSERT_TRUE(driven.result->failure()) << describe(*driven.result);
    EXPECT_EQ(driven.result->failure()->code, "P_MONEY_BUDGET") << describe(*driven.result);
    EXPECT_EQ(driven.original_sent, 1) << "the stop is after the first provider call";
    EXPECT_EQ(driven.recovered_attempted, 1) << "the recovered process attempted the first call of the join";
    EXPECT_EQ(driven.recovered_sent, 0) << "the join's call was refused before dispatch";
    EXPECT_LE(kCallMoney * static_cast<std::uint64_t>(driven.original_sent + driven.recovered_sent), kGrant)
        << "calls sent: " << driven.original_sent << " before the stop, " << driven.recovered_sent << " after it";
}

TEST(ProgramCommandRecovery, ARunWithoutAStopAdmitsOnlyWhatItsGrantAllows) {
    // The baseline of the two tests above: the grant of kOneCall admits one call and refuses the second.
    World   world(next_world_id());
    auto    peer = std::make_shared<Peer>(&world.crash.alive);
    TempDir dir("command-control");
    Host    host(world, peer, dir.path);
    host.granted       = granted_money(kOneCall);
    const auto version = host.admit(ProgramSource::from_javascript("calls.js", javascript_program(kThreeLlm)));
    const auto result  = host.run(version, world.run_id(), json::object());
    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << describe(result);
    EXPECT_EQ(sent_total(*peer), 1);
}

namespace {

/// Holds a scheduler thread of the runtime in its first event after Started, until the gate opens.
struct Gate {
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    open = false;
    std::atomic<int>        entered{0};
    void                    wait() {
        ++entered;
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return open; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

class HoldSink final : public ProgramEventSink {
public:
    explicit HoldSink(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    void on_event(const ProgramEvent& event) override {
        if (event.kind == ProgramEventKind::Started) return;
        if (!held_.exchange(true)) gate_->wait();
    }

private:
    std::shared_ptr<Gate> gate_;
    std::atomic<bool>     held_{false};
};

}  // namespace

TEST(ProgramCommandRecovery, ACancelBeforeAResumedAttemptStartsEndsCancelled) {
    // Both scheduler threads of the recovered process are busy, so the resumed attempt cannot start. The run is
    // cancelled meanwhile; when the threads are free, the attempt ends on what the journal holds. The terminal
    // transition is a valid one and the stored record is not left Running.
    StoppedRun stopped(javascript(kOkThenOk), json::object(), after_nth("publish running", 2));
    ASSERT_FALSE(stopped.world.crash.taken_at.empty());
    const auto run_id = stopped.world.run_id();
    const auto paused = stopped.recovered->reconnect(run_id);
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
    ASSERT_TRUE(paused.interrupt()->pending_effect) << describe(paused);
    const auto call_id = paused.interrupt()->pending_effect->call_id();

    auto&      runtime = stopped.recovered->runtime();
    // core_definition() has one entry node ("work"), so execute_single_async emits NodeStarted on the
    // scheduler->attempts thread itself instead of fanning out to the GraphEngine worker pool and suspending the
    // attempt coroutine.
    const auto holding =
        stopped.recovered->admit(ProgramSource::from_cpp_builder("holding", 2, plan_program(json{{"op", "call_core"}})));
    auto gate = std::make_shared<Gate>();
    std::vector<ProgramHandle> holders;
    struct ReleaseOnExit {
        std::shared_ptr<Gate>       gate;
        std::vector<ProgramHandle>& holders;
        ~ReleaseOnExit() {
            gate->release();
            for (auto& holder : holders) (void)holder.wait();
        }
    } release_on_exit{gate, holders};
    for (int i = 0; i < 2; ++i) {
        ProgramInvocation invocation{json{{"value", "ok:hold"}}, Host::budget(), "trace-hold",
                                     std::make_shared<HoldSink>(gate), stopped.world.run_id("hold" + std::to_string(i))};
        holders.push_back(runtime.start(kOwner, holding, std::move(invocation)));
    }
    for (int i = 0; i < 3000 && gate->entered.load() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(gate->entered.load(), 2) << "both scheduler threads are held";

    JournalView journal(stopped.image.path);
    auto        sink   = std::make_shared<StartedGrant>(journal, run_id);
    auto        handle = runtime.resume(kOwner, run_id,
                                        ProgramResume{json{{"out", "reconciled"}}, "trace-resume", sink, call_id});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(handle.try_result().has_value()) << "the resumed attempt ran while both scheduler threads were held";
    EXPECT_TRUE(handle.cancel());
    gate->release();

    std::promise<ProgramResult> done;
    auto                        future = done.get_future();
    std::thread                 waiter([handle, &done]() mutable {
        try {
            done.set_value(handle.wait());
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    struct JoinWaiter {
        std::thread& waiter;
        ~JoinWaiter() {
            if (waiter.joinable()) waiter.join();
        }
    } join_waiter{waiter};
    ASSERT_EQ(future.wait_for(std::chrono::seconds(60)), std::future_status::ready) << "the cancelled run finished";
    waiter.join();
    const auto result = future.get();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled) << describe(result);

    const auto record = journal.store->load_sync(kOwner, run_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->continuation().state, ContinuationState::Cancelled);
    ASSERT_TRUE(record->terminal_result()) << "the stored record has no terminal result";
    EXPECT_EQ(record->terminal_result()->status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(sink->grant) << "the attempt did not report Started";
    EXPECT_EQ(sink->grant->monetary_microunits, sink->journal_remaining.monetary_microunits);
    EXPECT_EQ(sink->grant->model_tokens, sink->journal_remaining.model_tokens);
}
#endif
