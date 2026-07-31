#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_program_store.h>
#include <neograph/mcp/server.h>
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
#include <neograph/mcp/sqlite_harness_store.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

using namespace std::chrono_literals;

namespace {
using neograph::json;
std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

json request() {
    return {
        {"task", {{"objective", "Review"}, {"acceptance", json::array({"structured"})}}},
        {"harness", {{"mode", "preset"}, {"preset", "fanout_judge"}}},
        {"workers",
         json::array({{{"id", "reviewer"},
                       {"instructions", "Return findings"},
                       {"tools", json::array()},
                       {"output_schema",
                        {{"type", "object"},
                         {"required", json::array({"status", "findings"})},
                         {"properties",
                          {{"status", {{"type", "string"}}}, {"findings", {{"type", "array"}}}}},
                         {"additionalProperties", false}}}}})},
        {"tool_catalog", json::array()},
        {"budgets",
         {{"max_steps", 10},
          {"timeout_seconds", 5},
          {"max_parallel_workers", 2},
          {"max_worker_retries", 0}}},
    };
}

struct HarnessFixture {
    std::atomic<int>                       calls{0};
    neograph::mcp::HarnessServiceConfig    config;
    neograph::mcp::HarnessServiceResources resources;

    explicit HarnessFixture(neograph::mcp::HarnessWorkerExecutor executor = {})
        : resources(make_resources(executor ? std::move(executor) : success_executor())) {}

    neograph::mcp::HarnessWorkerExecutor success_executor() {
        return [this](const neograph::mcp::HarnessWorkerCall&,
                      const std::shared_ptr<neograph::graph::CancelToken>&) {
            ++calls;
            return neograph::mcp::HarnessWorkerResponse::success(
                {{"status", "ok"}, {"findings", json::array({"grounded"})}});
        };
    }

    neograph::mcp::HarnessServiceResources make_resources(
        neograph::mcp::HarnessWorkerExecutor executor) {
        neograph::mcp::HarnessProgramHostConfig host;
        host.worker_executor           = std::move(executor);
        host.compiler_build_id         = "harness-cutover-test-v1";
        host.provider_binding_identity = digest('c');
        host.snapshots.owner_scope     = "harness-cutover-test";
        neograph::program::ExecutableIdentity provider{neograph::program::ExecutableKind::Provider,
                                                       "harness.provider", "1.0.0", digest('d')};
        neograph::program::ExecutableIdentity tool{neograph::program::ExecutableKind::Tool,
                                                   "harness.lookup", "1.0.0", digest('e')};
        host.snapshots.registry.provider = neograph::mcp::HarnessProviderRegistration{
            {provider, neograph::program::EffectMode::Brokered, "test-provider", {}, {}, {}},
            {json{{"type", "object"}}, json{{"type", "object"}}}};
        host.snapshots.registry.tools.push_back(
            {{tool,
              neograph::program::EffectMode::Brokered,
              "test-tool",
              {"tool.invoke"},
              {"filesystem.read"},
              {}},
             {{{"type", "object"}, {"additionalProperties", true}},
              {{"type", "object"}, {"additionalProperties", true}}}});
        host.snapshots.allowed_capabilities = {"tool.invoke"};
        host.snapshots.allowed_effects      = {"filesystem.read"};
        host.snapshots.allowed_module_digests = {
            digest('a'), digest('b'), provider.implementation_digest,
            tool.implementation_digest};
        host.snapshots.budget_ceiling         = {10000, 1000000, 100000, 8, 1000, 100, 100, 8, 100};
        host.checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
        host.state_store = std::make_shared<neograph::graph::InMemoryStore>();
        host.capability_executor = [](const json&, const json&, const auto&) {
            return json::object();
        };
        host.tool_binding_identities.emplace(tool.name, digest('f'));
        config.translation_defaults.provider = provider;
        config.translation_defaults.read_only_effects = {"filesystem.read"};
        return neograph::mcp::make_harness_program_service_resources(std::move(host));
    }
};

json await_terminal(neograph::mcp::HarnessService& service, const std::string& run_id) {
    for (int i = 0; i != 200; ++i) {
        auto value = service.get(run_id);
        if (value.at("status") != "running" && value.at("status") != "queued") return value;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Harness run did not become terminal");
}

TEST(HarnessProgramCutover, CompileStartGetAndFinalResultUseProgramRuntime) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          compiled = service.compile(request());
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    ASSERT_TRUE(compiled.contains("bundle_id"));
    ASSERT_TRUE(compiled.contains("program_version_id"));
    EXPECT_EQ(
        service.read(compiled.at("artifacts").at("core_lockfile").at("uri").get<std::string>()),
        compiled.at("artifacts").at("core_lockfile").at("content"));

    auto started = service.start({{"artifact_id", compiled.at("artifact_id").get<std::string>()}});
    ASSERT_TRUE(started.at("started").get<bool>());
    const auto run_id = started.at("run_id").get<std::string>();
    auto       result = await_terminal(service, run_id);
    ASSERT_EQ(result.at("status"), "completed") << result.dump();
    EXPECT_EQ(result.at("result").at("outcome"), "ok");
    EXPECT_EQ(result.at("result").at("valid_workers"), 1);
    EXPECT_EQ(fixture.calls.load(), 1);
    const auto status_uri = result.at("artifacts").at("status").get<std::string>();
    EXPECT_TRUE(status_uri.starts_with("neograph://runs/"));
}

TEST(HarnessProgramCutover, InvalidCompilePerformsZeroDispatch) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          invalid = request();
    invalid["workers"][0]["id"]           = "";
    auto result                           = service.compile(invalid);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(fixture.calls.load(), 0);
}

TEST(HarnessProgramCutover, ResumeUsesExactPendingCallIdentity) {
    HarnessFixture* fixture_ptr = nullptr;
    HarnessFixture  fixture([&](const neograph::mcp::HarnessWorkerCall& call,
                               const std::shared_ptr<neograph::graph::CancelToken>&) {
        ++fixture_ptr->calls;
        if (!call.resume_value)
            return neograph::mcp::HarnessWorkerResponse::input_required(
                {{"kind", "input"},
                 {"result_schema", {{"type", "object"}}},
                 {"payload", {{"question", "approve?"}}}});
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"resumed"})}});
    });
    fixture_ptr = &fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          started = service.start({{"request", request()}});
    const auto                    run_id  = started.at("run_id").get<std::string>();
    auto                          paused  = await_terminal(service, run_id);
    ASSERT_EQ(paused.at("status"), "input_required");
    ASSERT_TRUE(paused.at("pending").contains("call_id"));
    const auto call_id  = paused.at("pending").at("call_id").get<std::string>();
    auto       accepted = service.resume(
        {{"run_id", run_id}, {"call_id", call_id}, {"result", {{"approved", true}}}});
    EXPECT_TRUE(accepted.at("accepted").get<bool>());
    EXPECT_EQ(await_terminal(service, run_id).at("status"), "completed");
    EXPECT_EQ(fixture.calls.load(), 2);
}

TEST(HarnessProgramCutover, AwaitingToolsProjectsTypedPendingEffectAndExactResume) {
    HarnessFixture* fixture_ptr = nullptr;
    HarnessFixture  fixture([&](const neograph::mcp::HarnessWorkerCall& call,
                               const std::shared_ptr<neograph::graph::CancelToken>&) {
        ++fixture_ptr->calls;
        if (!call.resume_value) {
            return neograph::mcp::HarnessWorkerResponse::awaiting_tool_results(
                {{"call_id", "tool-call-1"},
                 {"result_schema",
                  {{"type", "object"},
                   {"required", json::array({"tool_results"})},
                   {"properties",
                    {{"tool_results",
                      {{"type", "array"}, {"items", {{"type", "object"}}}}}}}}},
                 {"effect",
                  {{"effect_id", "tool-effect-1"},
                   {"idempotency", "supported"},
                   {"tool_calls", json::array({{{"name", "search"},
                                                {"arguments", {{"query", "NeoGraph"}}}}})}}}});
        }
        EXPECT_EQ(call.resume_value->at("tool_results").size(), 1U);
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"tool-grounded"})}});
    });
    fixture_ptr = &fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto started = service.start({{"request", request()}});
    const auto run_id  = started.at("run_id").get<std::string>();
    const auto paused  = await_terminal(service, run_id);

    ASSERT_EQ(paused.at("status"), "awaiting_tool_results") << paused.dump();
    ASSERT_TRUE(paused.at("pending").is_object());
    EXPECT_EQ(paused.at("pending").at("call_id"), "tool-call-1");
    EXPECT_EQ(paused.at("pending").at("effect_id"), "tool-effect-1");
    EXPECT_EQ(paused.at("pending").at("state"), "awaiting");
    EXPECT_THROW(
        (void)service.resume(
            {{"run_id", run_id}, {"call_id", "wrong-call"}, {"result", json::object()}}),
        neograph::program::ProgramDiagnosticError);
    EXPECT_EQ(fixture.calls.load(), 1);

    const auto accepted = service.resume(
        {{"run_id", run_id},
         {"call_id", "tool-call-1"},
         {"result",
          {{"tool_results",
            json::array({{{"name", "search"}, {"result", {{"matches", 3}}}}})}}}});
    EXPECT_TRUE(accepted.at("accepted").get<bool>());
    EXPECT_EQ(await_terminal(service, run_id).at("status"), "completed");
    EXPECT_EQ(fixture.calls.load(), 2);
}

TEST(HarnessProgramCutover, RecordedReplayUsesCapturedCallsWithoutLiveDispatch) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto                    source_started = service.start({{"request", request()}});
    ASSERT_TRUE(source_started.at("started").get<bool>());
    const auto source_run_id = source_started.at("run_id").get<std::string>();
    const auto source_result = await_terminal(service, source_run_id);
    ASSERT_EQ(source_result.at("status"), "completed") << source_result.dump();
    ASSERT_EQ(fixture.calls.load(), 1);

    const auto replay_started =
        service.start({{"replay", {{"source_run_id", source_run_id}, {"mode", "recorded"}}}});
    ASSERT_TRUE(replay_started.at("started").get<bool>()) << replay_started.dump();
    EXPECT_EQ(replay_started.at("execution_mode"), "recorded_replay");
    const auto replay_result =
        await_terminal(service, replay_started.at("run_id").get<std::string>());

    ASSERT_EQ(replay_result.at("status"), "completed") << replay_result.dump();
    EXPECT_EQ(replay_result.at("result"), source_result.at("result"));
    EXPECT_EQ(fixture.calls.load(), 1);
}

TEST(HarnessProgramCutover, CancelDelegatesToProgramHandle) {
    HarnessFixture* fixture_ptr = nullptr;
    HarnessFixture  fixture([&](const neograph::mcp::HarnessWorkerCall&,
                               const std::shared_ptr<neograph::graph::CancelToken>& cancel) {
        ++fixture_ptr->calls;
        while (!cancel->is_cancelled())
            std::this_thread::sleep_for(1ms);
        return neograph::mcp::HarnessWorkerResponse::cancelled();
    });
    fixture_ptr = &fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          started = service.start({{"request", request()}});
    const auto                    run_id  = started.at("run_id").get<std::string>();
    EXPECT_TRUE(service.cancel(run_id));
    EXPECT_EQ(await_terminal(service, run_id).at("status"), "cancelled");
}

TEST(HarnessProgramCutover, SixCallbacksOwnAdapterPastServiceLifetime) {
    HarnessFixture                 fixture;
    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {{"name", "harness-test"}, {"version", "1"}};
    neograph::mcp::MCPServer server(server_config);
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        service.register_tools(server);
    }
    ASSERT_TRUE(server
                    .handle_message({{"jsonrpc", "2.0"},
                                     {"id", 1},
                                     {"method", "initialize"},
                                     {"params",
                                      {{"protocolVersion", "2025-11-25"},
                                       {"capabilities", json::object()},
                                       {"clientInfo", {{"name", "test"}, {"version", "1"}}}}}})
                    .contains("result"));
    server.handle_message(
        {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}, {"params", json::object()}});
    auto listed = server.handle_message(
        {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", json::object()}});
    ASSERT_EQ(listed.at("result").at("tools").size(), 6);
    auto schema = server.handle_message(
        {{"jsonrpc", "2.0"},
         {"id", 3},

         {"method", "tools/call"},
         {"params", {{"name", "neograph_schema"}, {"arguments", json::object()}}}});
    EXPECT_FALSE(schema.contains("error"));
}

TEST(HarnessProgramCutover, HostConfigurationChangesBindingAndArtifactIdentity) {
    HarnessFixture fixture;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-binding-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    auto records = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    fixture.config.record_store = records;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);

    const auto configured_request = [](std::string server_ref) {
        auto value = request();
        value["workers"][0]["tools"] = json::array({"harness.lookup"});
        value["tool_catalog"] = json::array(
            {{{"id", "harness.lookup"},
              {"description", "Look up a value"},
              {"input_schema", {{"type", "object"}, {"additionalProperties", true}}},
              {"output_schema", {{"type", "object"}, {"additionalProperties", true}}},
              {"read_only", true},
              {"executor",
               {{"kind", "mcp"},
                {"server_ref", std::move(server_ref)},
                {"tool", "lookup"}}}}});
        return value;
    };
    const auto first  = service.compile(configured_request("server-a"));
    const auto second = service.compile(configured_request("server-b"));
    ASSERT_TRUE(first.at("ok").get<bool>()) << first.dump();
    ASSERT_TRUE(second.at("ok").get<bool>()) << second.dump();
    EXPECT_EQ(first.at("bundle_id"), second.at("bundle_id"));
    EXPECT_NE(first.at("artifact_id"), second.at("artifact_id"));
    EXPECT_NE(first.at("program_version_id"), second.at("program_version_id"));

    const auto first_stored =
        records->load_artifact(first.at("artifact_id").get<std::string>());
    const auto second_stored =
        records->load_artifact(second.at("artifact_id").get<std::string>());
    ASSERT_TRUE(first_stored.has_value());
    ASSERT_TRUE(second_stored.has_value());
    const auto first_record =
        neograph::mcp::HarnessProgramArtifactRecord::parse(*first_stored);
    const auto second_record =
        neograph::mcp::HarnessProgramArtifactRecord::parse(*second_stored);
    EXPECT_NE(
        neograph::program::capability_binding_receipt_root(
            first_record.version().core_materialization_receipt().capability_bindings),
        neograph::program::capability_binding_receipt_root(
            second_record.version().core_materialization_receipt().capability_bindings));
    std::filesystem::remove_all(root);
}

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
TEST(HarnessProgramCutover, ReconnectsExactSqliteProgramRunAfterServiceDestruction) {
    HarnessFixture fixture;
    const auto     path =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-cutover-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    fixture.config.record_store =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
    std::string run_id;
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        auto                          started = service.start({{"request", request()}});
        run_id                                = started.at("run_id").get<std::string>();
        ASSERT_EQ(await_terminal(service, run_id).at("status"), "completed");
    }
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        auto                          reconnected = service.get(run_id);
        EXPECT_EQ(reconnected.at("status"), "completed");
        EXPECT_EQ(reconnected.at("run_id"), run_id);
    }
    std::filesystem::remove(path);
}
TEST(HarnessProgramCutover, ReconnectsInterruptedSqliteRunAndContinuesExactCheckpoint) {
    HarnessFixture* fixture_ptr = nullptr;
    HarnessFixture  fixture([&](const neograph::mcp::HarnessWorkerCall& call,
                               const std::shared_ptr<neograph::graph::CancelToken>&) {
        ++fixture_ptr->calls;
        if (!call.resume_value) {
            return neograph::mcp::HarnessWorkerResponse::input_required(
                {{"call_id", "restart-input-1"},
                 {"result_schema", {{"type", "object"}}},
                 {"payload", {{"question", "continue?"}}}});
        }
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"continued"})}});
    });
    fixture_ptr = &fixture;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-interrupted-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    fixture.config.record_store =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());

    std::string run_id;
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto                    started = service.start({{"request", request()}});
        run_id                                = started.at("run_id").get<std::string>();
        const auto paused                     = await_terminal(service, run_id);
        ASSERT_EQ(paused.at("status"), "input_required");
        EXPECT_EQ(paused.at("pending").at("call_id"), "restart-input-1");
    }
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto                    reconnected = service.get(run_id);
        ASSERT_EQ(reconnected.at("status"), "input_required") << reconnected.dump();
        ASSERT_EQ(reconnected.at("pending").at("call_id"), "restart-input-1");
        const auto accepted = service.resume(
            {{"run_id", run_id},
             {"call_id", "restart-input-1"},
             {"result", {{"approved", true}}}});
        EXPECT_TRUE(accepted.at("accepted").get<bool>());
        const auto completed = await_terminal(service, run_id);
        EXPECT_EQ(completed.at("status"), "completed") << completed.dump();
    }
    EXPECT_EQ(fixture.calls.load(), 2);
    std::filesystem::remove(path);
}

TEST(HarnessProgramCutover, ConcurrentSqliteReconnectResumeHasOneCasWinnerAndOneDispatch) {
    HarnessFixture* fixture_ptr = nullptr;
    HarnessFixture  fixture([&](const neograph::mcp::HarnessWorkerCall& call,
                               const std::shared_ptr<neograph::graph::CancelToken>&) {
        ++fixture_ptr->calls;
        if (!call.resume_value) {
            return neograph::mcp::HarnessWorkerResponse::input_required(
                {{"call_id", "resume-race-1"},
                 {"result_schema", {{"type", "object"}}},
                 {"payload", {{"question", "continue?"}}}});
        }
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"single-winner"})}});
    });
    fixture_ptr = &fixture;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-resume-race-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    fixture.config.record_store =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
    auto second_config = fixture.config;
    second_config.record_store =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
    neograph::mcp::HarnessService first(fixture.config, nullptr, fixture.resources);
    neograph::mcp::HarnessService second(second_config, nullptr, fixture.resources);

    const auto started = first.start({{"request", request()}});
    const auto run_id  = started.at("run_id").get<std::string>();
    ASSERT_EQ(await_terminal(first, run_id).at("status"), "input_required");
    ASSERT_EQ(second.get(run_id).at("status"), "input_required");

    std::promise<void> ready;
    auto               gate = ready.get_future().share();
    const auto resume_once = [&](neograph::mcp::HarnessService& service, bool approved) {
        gate.wait();
        try {
            const auto accepted = service.resume(
                {{"run_id", run_id},
                 {"call_id", "resume-race-1"},
                 {"result", {{"approved", approved}}}});
            return accepted.at("accepted").get<bool>() ? 1 : -1;
        } catch (const neograph::program::ProgramDiagnosticError& error) {
            return error.diagnostic().code == "P_RESUME_CONFLICT" ? 0 : -2;
        }
    };
    auto first_resume =
        std::async(std::launch::async, [&] { return resume_once(first, true); });
    auto second_resume =
        std::async(std::launch::async, [&] { return resume_once(second, false); });
    ready.set_value();
    const int first_result  = first_resume.get();
    const int second_result = second_resume.get();
    EXPECT_EQ(first_result + second_result, 1);
    ASSERT_TRUE(first_result == 1 || second_result == 1);
    auto& winner = first_result == 1 ? first : second;
    const auto completed = await_terminal(winner, run_id);
    EXPECT_EQ(completed.at("status"), "completed") << completed.dump();
    EXPECT_EQ(fixture.calls.load(), 2);
    std::filesystem::remove(path);
}

TEST(HarnessProgramCutover, CrossArtifactForkReadsSourceTransitionsAndForwardsPendingValue) {
    HarnessFixture* fixture_ptr = nullptr;
    HarnessFixture  fixture([&](const neograph::mcp::HarnessWorkerCall& call,
                               const std::shared_ptr<neograph::graph::CancelToken>&) {
        ++fixture_ptr->calls;
        if (!call.resume_value) {
            return neograph::mcp::HarnessWorkerResponse::input_required(
                {{"call_id", "fork-input-1"},
                 {"result_schema", {{"type", "object"}}},
                 {"payload", {{"question", "repair?"}}}});
        }
        EXPECT_TRUE(call.resume_value->at("approved").get<bool>());
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"forked"})}});
    });
    fixture_ptr = &fixture;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-fork-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    fixture.config.record_store =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);

    const auto source_artifact = service.compile(request());
    ASSERT_TRUE(source_artifact.at("ok").get<bool>());
    auto target_request = request();
    target_request["budgets"]["timeout_seconds"] = 1;
    const auto target_artifact = service.compile(target_request);
    ASSERT_TRUE(target_artifact.at("ok").get<bool>()) << target_artifact.dump();
    ASSERT_NE(source_artifact.at("artifact_id"), target_artifact.at("artifact_id"));

    const auto source_started =
        service.start({{"artifact_id", source_artifact.at("artifact_id")}});
    const auto source_run_id = source_started.at("run_id").get<std::string>();
    const auto source_paused = await_terminal(service, source_run_id);
    ASSERT_EQ(source_paused.at("status"), "input_required") << source_paused.dump();
    const auto checkpoint_id =
        source_paused.at("checkpoint").at("checkpoint_id").get<std::string>();

    const auto fork_started = service.start(
        {{"fork",
          {{"source_run_id", source_run_id},
           {"checkpoint_id", checkpoint_id},
           {"artifact_id", target_artifact.at("artifact_id")},
           {"call_id", "fork-input-1"},
           {"result", {{"approved", true}}}}}});
    ASSERT_TRUE(fork_started.at("started").get<bool>()) << fork_started.dump();
    EXPECT_EQ(fork_started.at("execution_mode"), "compatible_fork");
    EXPECT_EQ(fork_started.at("artifact_id"), target_artifact.at("artifact_id"));
    EXPECT_EQ(fork_started.at("source_run_id"), source_run_id);
    EXPECT_EQ(fork_started.at("source_checkpoint_id"), checkpoint_id);

    const auto forked =
        await_terminal(service, fork_started.at("run_id").get<std::string>());
    EXPECT_EQ(forked.at("status"), "completed") << forked.dump();
    EXPECT_EQ(forked.at("result").at("outcome"), "ok");
    EXPECT_EQ(fixture.calls.load(), 2);
    std::filesystem::remove(path);
}

#endif

}  // namespace
