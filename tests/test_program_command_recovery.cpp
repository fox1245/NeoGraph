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

#include "program_resume_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
/// then reconnects the run on the copy in a new process.
struct StoppedRun {
    TempDir                       live{"command-live"}, image{"command-image"};
    World                         world{next_world_id()};
    std::optional<ProgramVersion> version;
    std::unique_ptr<Host>         recovered;

    StoppedRun(const std::function<ProgramSource()>& source, json input,
               std::function<bool(const std::vector<std::string>&)> armed_when) {
        world.crash.reset(live.path, image.path, -1, true);
        world.crash.armed_when = std::move(armed_when);
        {
            Host host(world, std::make_shared<Peer>(&world.crash.alive), live.path);
            version = host.admit(source());
            (void)host.run(*version, world.run_id(), std::move(input));
        }
        world.crash.alive = true;
        recovered = std::make_unique<Host>(world, std::make_shared<Peer>(&world.crash.alive), image.path);
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
