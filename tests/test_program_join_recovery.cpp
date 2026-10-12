// Program: recovery of a resumed join whose answered member had finished.
//
// A JavaScript join that runs its members one at a time, both asking. The
// resume answers member 0, which finishes; member 1 asks; the join records
// member 0 as settled, and the process stops before the pause on member 1 is
// published. Recovery resumes the attempt with the answer to member 0, whose
// result is already recorded: it replays member 0, and member 1 asks its
// question. Recovery used to fail the run ("Recorded join members do not match
// the paused join", reported as P_TRANSITION_CONFLICT). Found by the resume
// model harness (tests/test_program_resume_model.cpp).

#include "program_resume_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#if defined(NEOGRAPH_PROGRAM_RESUME_MODEL_HAVE_QUICKJS)
TEST(ProgramJoinRecovery, RecoveryAfterTheAnsweredMemberSettledAsksTheNextMember) {
    const auto source = javascript_program(R"JS(
  const replies = yield ng.all([
    ng.callCore("main", {value: "ask:a"}, "first"),
    ng.callCore("main", {value: "ask:b"}, "second")
  ], {max_in_flight: 1}, "sequential");
  return {out: replies.map((reply) => reply.out)};)JS");
    TempDir live("join-live"), image("join-image");
    World   world(next_world_id());
    // Copy the durable state after the publication that follows member 1's
    // NodeInterrupt checkpoint: the one that records member 0 as settled.
    world.crash.reset(live.path, image.path, -1, true);
    world.crash.armed_when = [](const std::vector<std::string>& seen) {
        const auto interrupts = std::count(seen.begin(), seen.end(), "node_interrupt");
        return interrupts == 2 && seen.back().rfind("publish", 0) == 0;
    };
    std::optional<ProgramVersion> version;
    {
        Host host(world, std::make_shared<Peer>(&world.crash.alive), live.path);
        version           = host.admit(ProgramSource::from_javascript("join.js", source));
        const auto paused = host.run(*version, world.run_id(), json::object());
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << describe(paused);
        ASSERT_EQ(paused.interrupt()->pending_input->operation_id(), "root.javascript.1/member/0");
        (void)host.resume(world.run_id(), json{{"decision", "d1"}}, paused.interrupt()->pending_input->call_id());
        ASSERT_FALSE(world.crash.alive) << "the process stopped after the join recorded member 0";
    }
    world.crash.alive = true;
    Host       recovered(world, std::make_shared<Peer>(&world.crash.alive), image.path);
    const auto asked = recovered.reconnect(world.run_id());
    ASSERT_EQ(asked.status(), ProgramTerminalStatus::Interrupted) << describe(asked);
    ASSERT_TRUE(asked.interrupt()->pending_input);
    EXPECT_EQ(asked.interrupt()->pending_input->operation_id(), "root.javascript.1/member/1");
    const auto done =
        recovered.resume(world.run_id(), json{{"decision", "d2"}}, asked.interrupt()->pending_input->call_id());
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << describe(done);
    EXPECT_EQ(done.output(), (json{{"out", json::array({"d1", "d2"})}}));
    const std::lock_guard lock(world.journal.mutex);
    int answered = 0;
    for (const auto& run : world.journal.runs) answered += run.value == "ask:a" && !run.answer.empty();
    EXPECT_EQ(answered, 1) << "member 0 replays after the recovery";
}
#endif
