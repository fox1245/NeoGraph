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
// #403: reconciling a stopped call as Completed continues main().

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

#endif

