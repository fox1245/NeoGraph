// Program: recovery of a JavaScript main() that stopped inside its second
// Core call.
//
// main() calls the Core twice. The first call finishes and its checkpoint
// becomes the run's; the process stops after the second call is recorded as
// pending, before it publishes anything of its own. Recovery treated the
// first call's checkpoint as the one to resume the second call from, found
// that it belongs to another call, and ended the run CheckpointIncompatible
// ("Exact Core resume checkpoint does not match a pending JavaScript callCore
// coordinate"). It now returns the run for reconciliation of the second
// call, as it does when the process stops inside the first. Found by the
// resume model harness (tests/test_program_resume_model.cpp).

#include "program_resume_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

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
#endif
