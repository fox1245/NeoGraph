#include <gtest/gtest.h>

#include <neograph/graph/compiler.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/server.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

using neograph::json;
using neograph::mcp::HarnessService;
using neograph::mcp::HarnessServiceConfig;
using neograph::mcp::HarnessWorkerCall;
using neograph::mcp::HarnessWorkerResponse;

json output_schema() {
    return json::parse(R"JSON({
        "type":"object",
        "required":["status","findings"],
        "properties":{
            "status":{"enum":["ok","partial","failed"]},
            "findings":{"type":"array"}
        },
        "additionalProperties":false
    })JSON");
}

json worker(std::string id, json tools = json::array()) {
    return {
        {"id", std::move(id)},
        {"instructions", "Return structured findings"},
        {"tools", std::move(tools)},
        {"output_schema", output_schema()},
    };
}

json request(json workers = json::array({worker("reviewer")})) {
    return {
        {"task", {
            {"objective", "Review the change"},
            {"acceptance", json::array({"Use structured output"})},
        }},
        {"harness", {{"mode", "preset"}, {"preset", "fanout_judge"}}},
        {"workers", std::move(workers)},
        {"tool_catalog", json::array()},
        {"budgets", {
            {"max_steps", 10},
            {"timeout_seconds", 5},
            {"max_parallel_workers", 2},
            {"max_worker_retries", 1},
        }},
    };
}

json wait_terminal(HarnessService& service, const std::string& run_id,
                   std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = service.get(run_id);
        const auto status = snapshot["status"].get<std::string>();
        if (status != "queued" && status != "running") return snapshot;
        std::this_thread::sleep_for(2ms);
    }
    return service.get(run_id);
}

TEST(HarnessServiceTest, ExposesSmallStableMcpToolSurface) {
    HarnessService service;
    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {{"name", "harness-test"}, {"version", "1"}};
    neograph::mcp::MCPServer server(server_config);
    service.register_tools(server);

    auto initialize = server.handle_message({
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-11-25"},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "test"}, {"version", "1"}}},
        }},
    });
    ASSERT_TRUE(initialize.contains("result"));
    server.handle_message({
        {"jsonrpc", "2.0"}, {"method", "notifications/initialized"},
        {"params", json::object()},
    });
    auto listed = server.handle_message({
        {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"},
        {"params", json::object()},
    });
    ASSERT_TRUE(listed.contains("result"));
    ASSERT_EQ(listed["result"]["tools"].size(), 5u);
    EXPECT_EQ(listed["result"]["tools"][0]["name"], "neograph_cancel");
    EXPECT_EQ(listed["result"]["tools"][4]["name"], "neograph_start");
    for (const auto& tool : listed["result"]["tools"]) {
        EXPECT_TRUE(tool.contains("outputSchema"));
    }
}

TEST(HarnessServiceTest, MalformedHarnessCannotProduceExecutableArtifact) {
    HarnessService service;
    auto malformed = request();
    malformed["workers"][0]["tools"] = json::array({"missing.tool"});

    auto compiled = service.compile(malformed);
    EXPECT_FALSE(compiled["ok"].get<bool>());
    EXPECT_FALSE(compiled.contains("artifact_id"));
    bool saw_binding_error = false;
    for (const auto& diagnostic : compiled["diagnostics"]) {
        EXPECT_TRUE(diagnostic.contains("phase"));
        EXPECT_TRUE(diagnostic.contains("severity"));
        EXPECT_TRUE(diagnostic.contains("path"));
        EXPECT_TRUE(diagnostic.contains("message"));
        EXPECT_TRUE(diagnostic.contains("witness"));
        EXPECT_TRUE(diagnostic.contains("source"));
        if (diagnostic["code"] == "H_UNKNOWN_TOOL") saw_binding_error = true;
    }
    EXPECT_TRUE(saw_binding_error);

    auto started = service.start({{"request", malformed}});
    EXPECT_FALSE(started["started"].get<bool>());
    EXPECT_EQ(started["status"], "compile_failed");
}

TEST(HarnessServiceTest, PresetAndEquivalentDslCompileToCanonicalCore) {
    HarnessService service;
    auto preset_request = request(json::array({worker("a"), worker("b")}));
    auto preset = service.compile(preset_request);
    ASSERT_TRUE(preset["ok"].get<bool>()) << preset.dump();

    auto dsl_request = preset_request;
    dsl_request["harness"] = {
        {"mode", "dsl"},
        {"definition", preset["artifacts"]["core_lockfile"]["content"]},
    };
    auto dsl = service.compile(dsl_request);
    ASSERT_TRUE(dsl["ok"].get<bool>()) << dsl.dump();

    EXPECT_EQ(neograph::graph::GraphCompiler::canon(
                  preset["artifacts"]["core_lockfile"]["content"]),
              neograph::graph::GraphCompiler::canon(
                  dsl["artifacts"]["core_lockfile"]["content"]));
}

TEST(HarnessServiceTest, BindingDiagnosticCarriesElaboratorSourceCoordinate) {
    HarnessService service;
    auto authored = request();
    authored["harness"] = {
        {"mode", "dsl"},
        {"definition", {
            {"schema_version", 1},
            {"channels", {
                {"task", {{"reducer", "overwrite"}, {"initial", json::object()}}},
                {"worker_results", {{"reducer", "append"}, {"initial", json::array()}}},
                {"final_result", {{"reducer", "overwrite"}, {"initial", nullptr}}},
            }},
            {"nodes", {
                {"judge", {
                    {"type", "neograph_harness_judge"},
                    {"barrier", {{"wait_for", json::array({"panel_worker"})}}},
                }},
            }},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "panel_worker"}},
                {{"from", "panel_worker"}, {"to", "judge"}},
                {{"from", "judge"}, {"to", "__end__"}},
            })},
            {"templates", {
                {"worker", {
                    {"params", json::array()},
                    {"nodes", {
                        {"worker", {
                            {"type", "neograph_harness_worker"},
                            {"worker_id", "undeclared"},
                        }},
                    }},
                }},
            }},
            {"use", json::array({{
                {"template", "worker"}, {"prefix", "panel"},
                {"args", json::object()},
            }})},
        }},
    };

    auto compiled = service.compile(authored);
    ASSERT_FALSE(compiled["ok"].get<bool>());
    bool found = false;
    for (const auto& diagnostic : compiled["diagnostics"]) {
        if (diagnostic["code"] != "H_UNKNOWN_WORKER") continue;
        found = true;
        EXPECT_NE(diagnostic["source"].get<std::string>().find("use[0]"),
                  std::string::npos);
        EXPECT_NE(diagnostic["source"].get<std::string>().find("'worker'"),
                  std::string::npos);
    }
    EXPECT_TRUE(found) << compiled.dump();
}

TEST(HarnessServiceTest, RepairsInvalidWorkerOutputBeforeJudgeAndReportsZeroFindings) {
    std::atomic<int> calls{0};
    HarnessServiceConfig config;
    config.worker_executor = [&calls](const HarnessWorkerCall& call, const auto&) {
        ++calls;
        if (call.attempt == 1) {
            EXPECT_TRUE(call.repair_feedback.empty());
            return HarnessWorkerResponse::success({{"status", "ok"}});
        }
        EXPECT_NE(call.repair_feedback.find("missing required property findings"),
                  std::string::npos);
        return HarnessWorkerResponse::success({
            {"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));
    auto compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    ASSERT_TRUE(started["started"].get<bool>());

    auto finished = wait_terminal(service, started["run_id"].get<std::string>());
    ASSERT_EQ(finished["status"], "completed") << finished.dump();
    EXPECT_EQ(finished["result"]["outcome"], "zero_findings");
    ASSERT_EQ(finished["result"]["workers"].size(), 1u);
    EXPECT_EQ(finished["result"]["workers"][0]["attempts"], 2);
    EXPECT_TRUE(finished["result"]["workers"][0].contains("output"));
    EXPECT_EQ(calls.load(), 2);
}

TEST(HarnessServiceTest, InvalidWorkerOutputNeverReachesJudgeAsOutput) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::parse_error("invalid JSON at byte 4");
    };
    HarnessService service(std::move(config));
    auto compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    auto finished = wait_terminal(service, started["run_id"].get<std::string>());

    ASSERT_EQ(finished["status"], "completed") << finished.dump();
    EXPECT_EQ(finished["result"]["outcome"], "failed");
    const auto rejected = finished["result"]["workers"][0];
    EXPECT_EQ(rejected["failure_kind"], "parse_error");
    EXPECT_FALSE(rejected.contains("output"));
    EXPECT_EQ(rejected["attempts"], 2);
}

TEST(HarnessServiceTest, PreservesPartialWorkerOutcome) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall& call, const auto&) {
        return HarnessWorkerResponse::success({
            {"status", call.worker["id"] == "a" ? "ok" : "partial"},
            {"findings", json::array()},
        });
    };
    HarnessService service(std::move(config));
    auto compiled = service.compile(request(json::array({worker("a"), worker("b")})));
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    auto finished = wait_terminal(service, started["run_id"].get<std::string>());
    ASSERT_EQ(finished["status"], "completed") << finished.dump();
    EXPECT_EQ(finished["result"]["outcome"], "partial");
}

TEST(HarnessServiceTest, DistinguishesEmptyResponseAndWorkerTimeout) {
    {
        HarnessServiceConfig config;
        config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
            return HarnessWorkerResponse::empty("model returned no content");
        };
        HarnessService service(std::move(config));
        auto compiled = service.compile(request());
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
        auto finished = wait_terminal(service, started["run_id"].get<std::string>());
        ASSERT_EQ(finished["status"], "completed") << finished.dump();
        EXPECT_EQ(finished["result"]["workers"][0]["failure_kind"],
                  "empty_response");
        EXPECT_EQ(finished["result"]["workers"][0]["attempts"], 2);
    }
    {
        HarnessServiceConfig config;
        config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
            return HarnessWorkerResponse::timeout("downstream worker timed out");
        };
        HarnessService service(std::move(config));
        auto compiled = service.compile(request());
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
        auto finished = wait_terminal(service, started["run_id"].get<std::string>());
        ASSERT_EQ(finished["status"], "completed") << finished.dump();
        EXPECT_EQ(finished["result"]["workers"][0]["failure_kind"], "timeout");
        EXPECT_EQ(finished["result"]["workers"][0]["attempts"], 1);
    }
}

TEST(HarnessServiceTest, GlobalDeadlineHasDistinctRunState) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto& cancel) {
        while (!cancel->is_cancelled()) std::this_thread::sleep_for(1ms);
        return HarnessWorkerResponse::cancelled("deadline cancellation");
    };
    HarnessService service(std::move(config));
    auto timed_request = request();
    timed_request["budgets"]["timeout_seconds"] = 1;
    auto compiled = service.compile(timed_request);
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    auto finished = wait_terminal(service, started["run_id"].get<std::string>(), 2s);
    EXPECT_EQ(finished["status"], "timeout") << finished.dump();
}

TEST(HarnessServiceTest, CooperativeCancellationHasDistinctRunState) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto& cancel) {
        while (!cancel->is_cancelled()) std::this_thread::sleep_for(1ms);
        return HarnessWorkerResponse::cancelled("test cancellation");
    };
    HarnessService service(std::move(config));
    auto compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto run_id = started["run_id"].get<std::string>();

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (service.get(run_id)["status"] == "queued"
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(service.cancel(run_id));
    auto finished = wait_terminal(service, run_id);
    EXPECT_EQ(finished["status"], "cancelled") << finished.dump();
}

TEST(HarnessServiceTest, ExhaustedMaxStepsHasDistinctRunState) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({
            {"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));
    auto loop_request = request();
    auto preset = service.compile(loop_request);
    ASSERT_TRUE(preset["ok"].get<bool>()) << preset.dump();
    auto core = preset["artifacts"]["core_lockfile"]["content"];
    core["edges"] = json::array({
        {{"from", "__start__"}, {"to", "worker_0"}},
        {{"from", "worker_0"}, {"to", "worker_0"}},
        {{"from", "worker_0"}, {"to", "judge"}},
        {{"from", "judge"}, {"to", "__end__"}},
    });
    loop_request["harness"] = {{"mode", "dsl"}, {"definition", core}};
    loop_request["budgets"]["max_steps"] = 2;
    auto compiled = service.compile(loop_request);
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    auto finished = wait_terminal(service, started["run_id"].get<std::string>());
    EXPECT_EQ(finished["status"], "max_steps_exhausted") << finished.dump();
}

} // namespace
