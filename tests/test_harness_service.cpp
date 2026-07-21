#include <neograph/graph/checkpoint.h>
#include <neograph/graph/compiler.h>

#include <gtest/gtest.h>
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/mcp/sqlite_harness_store.h>
#include <sqlite3.h>
#endif
#include <neograph/mcp/harness.h>
#include <neograph/mcp/server.h>
#include <neograph/provider.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
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
        {"task",
         {
             {"objective", "Review the change"},
             {"acceptance", json::array({"Use structured output"})},
         }},
        {"harness", {{"mode", "preset"}, {"preset", "fanout_judge"}}},
        {"workers", std::move(workers)},
        {"tool_catalog", json::array()},
        {"budgets",
         {
             {"max_steps", 10},
             {"timeout_seconds", 5},
             {"max_parallel_workers", 2},
             {"max_worker_retries", 1},
         }},
    };
}

json dsl_request(std::string judge_name, std::string worker_results_reducer = "append") {
    auto       value       = request();
    const auto worker_node = "worker_0";
    value["harness"]       = {
        {"mode", "dsl"},
        {"definition",
               {
             {"schema_version", 1},
             {"name", "fork_compatibility_test"},
             {"channels",
                    {
                  {"task", {{"reducer", "overwrite"}, {"initial", json::object()}}},
                  {"worker_results",
                         {{"reducer", std::move(worker_results_reducer)}, {"initial", json::array()}}},
                  {"final_result", {{"reducer", "overwrite"}, {"initial", nullptr}}},
              }},
             {"nodes",
                    {
                  {worker_node, {{"type", "neograph_harness_worker"}, {"worker_id", "reviewer"}}},
                  {judge_name,
                         {{"type", "neograph_harness_judge"},
                          {"barrier", {{"wait_for", json::array({worker_node})}}}}},
              }},
             {"edges", json::array({
                           {{"from", "__start__"}, {"to", worker_node}},
                           {{"from", worker_node}, {"to", judge_name}},
                           {{"from", judge_name}, {"to", "__end__"}},
                       })},
         }},
    };
    return value;
}

json host_brokered_request(std::string interaction = "tool_result") {
    auto value                   = request();
    value["workers"][0]["tools"] = json::array({"host.lookup"});
    value["tool_catalog"]        = json::array({{
        {"id", "host.lookup"},
        {"description", "Look up a value through the host"},
        {"input_schema",
                {
             {"type", "object"},
             {"required", json::array({"query"})},
             {"properties", {{"query", {{"type", "string"}}}}},
             {"additionalProperties", false},
         }},
        {"output_schema",
                {
             {"type", "object"},
             {"required", json::array({"answer"})},
             {"properties", {{"answer", {{"type", "string"}}}}},
             {"additionalProperties", false},
         }},
        {"read_only", true},
        {"executor", {{"kind", "host_brokered"}, {"interaction", std::move(interaction)}}},
    }});
    return value;
}

std::filesystem::path unique_temp_path(const std::string& stem) {
    return std::filesystem::temp_directory_path() /
           (stem + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

class TempDirectoryCleanup {
public:
    explicit TempDirectoryCleanup(std::filesystem::path path) : path_(std::move(path)) {}

    ~TempDirectoryCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

private:
    std::filesystem::path path_;
};

struct ServerResponses {
    std::mutex              mutex;
    std::condition_variable cv;
    std::vector<json>       values;

    neograph::mcp::MCPServer::ResponseSink sink() {
        return [this](const json& value) {
            std::lock_guard lock(mutex);
            values.push_back(value);
            cv.notify_all();
        };
    }

    json wait(const json& id) {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 3s, [&] {
            for (const auto& value : values) {
                if (value.value("id", json(nullptr)) == id) return true;
            }
            return false;
        });
        for (const auto& value : values) {
            if (value.value("id", json(nullptr)) == id) return value;
        }
        return nullptr;
    }
};

json wait_terminal(HarnessService&           service,
                   const std::string&        run_id,
                   std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = service.get(run_id);
        const auto status = snapshot["status"].get<std::string>();
        if (status != "queued" && status != "running") {
            return service.get(run_id, "details");
        }
        std::this_thread::sleep_for(2ms);
    }
    return service.get(run_id, "details");
}

class ScriptedProvider final : public neograph::Provider {
public:
    std::vector<neograph::ChatCompletion> completions;
    std::vector<neograph::CompletionParams> calls;

    neograph::ChatCompletion complete(const neograph::CompletionParams& params) override {
        calls.push_back(params);
        if (completions.empty()) throw std::runtime_error("no scripted completion");
        auto result = completions.front();
        completions.erase(completions.begin());
        return result;
    }

    std::string get_name() const override { return "scripted-harness-provider"; }
};

class StartFailingJournal final : public neograph::mcp::HarnessJournal {
public:
    void append_event(const json& event) override {
        if (event.value("event_type", "") == "run.started") {
            throw std::runtime_error("journal unavailable");
        }
    }

    std::vector<json> list_events(const std::string&, std::size_t, std::size_t) override {
        return {};
    }
};

TEST(HarnessServiceTest, ExposesSmallStableMcpToolSurface) {
    HarnessService service;
    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {{"name", "harness-test"}, {"version", "1"}};
    neograph::mcp::MCPServer server(server_config);
    service.register_tools(server);

    auto initialize = server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params",
         {
             {"protocolVersion", "2025-11-25"},
             {"capabilities", json::object()},
             {"clientInfo", {{"name", "test"}, {"version", "1"}}},
         }},
    });
    ASSERT_TRUE(initialize.contains("result"));
    server.handle_message({
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()},
    });
    auto listed = server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/list"},
        {"params", json::object()},
    });
    ASSERT_TRUE(listed.contains("result"));
    ASSERT_EQ(listed["result"]["tools"].size(), 6u);
    EXPECT_EQ(listed["result"]["tools"][0]["name"], "neograph_cancel");
    EXPECT_EQ(listed["result"]["tools"][4]["name"], "neograph_schema");
    EXPECT_EQ(listed["result"]["tools"][5]["name"], "neograph_start");
    bool saw_debug_views = false;
    bool saw_replay      = false;
    bool saw_fork        = false;
    for (const auto& tool : listed["result"]["tools"]) {
        EXPECT_TRUE(tool.contains("outputSchema"));
        EXPECT_EQ(tool["inputSchema"].value("type", ""), "object");
        if (tool["name"] == "neograph_get") {
            const auto schema = tool["inputSchema"]["properties"];
            EXPECT_EQ(schema["limit"]["maximum"], 1000);
            EXPECT_EQ(schema["after_sequence"]["minimum"], 0);
            EXPECT_EQ(schema["view"]["enum"].size(), 7u);
            saw_debug_views = true;
        } else if (tool["name"] == "neograph_start") {
            const auto replay = tool["inputSchema"]["properties"]["replay"];
            EXPECT_EQ(replay["properties"]["mode"]["enum"].size(), 2u);
            const auto fork = tool["inputSchema"]["properties"]["fork"];
            EXPECT_EQ(fork["required"].size(), 3u);
            saw_replay = true;
            saw_fork   = true;
        }
    }
    EXPECT_TRUE(saw_debug_views);
    EXPECT_TRUE(saw_replay);
    EXPECT_TRUE(saw_fork);

    auto invalid_get = server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "neograph_get"}, {"arguments", json::object()}}},
    });
    ASSERT_TRUE(invalid_get.contains("result"));
    EXPECT_TRUE(invalid_get["result"]["isError"].get<bool>());
    EXPECT_NE(invalid_get["result"]["content"][0]["text"].get<std::string>().find(
                  "missing required property run_id"),
              std::string::npos);
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

TEST(HarnessServiceTest, JournalFailureFailsRunWithoutEscapingWorkerThread) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config), std::make_shared<StartFailingJournal>());
    const auto compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    const auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto failed = wait_terminal(service, started["run_id"].get<std::string>());
    EXPECT_EQ(failed["status"], "failed");
    EXPECT_NE(failed["error"].get<std::string>().find("journal unavailable"), std::string::npos);
}

TEST(HarnessServiceTest, DebugViewsReportUnavailableWithoutDurableStores) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));
    const auto     compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    const auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto run_id  = started["run_id"].get<std::string>();
    ASSERT_EQ(wait_terminal(service, run_id)["status"], "completed");

    const auto attempts = service.get(run_id, "attempts");
    EXPECT_FALSE(attempts["result"]["journal_available"].get<bool>());
    EXPECT_TRUE(attempts["result"]["events"].empty());
    const auto checkpoints = service.get(run_id, "checkpoints");
    EXPECT_FALSE(checkpoints["result"]["available"].get<bool>());
    EXPECT_TRUE(checkpoints["result"]["checkpoints"].empty());
    const auto diff = service.get(run_id, "diff");
    EXPECT_FALSE(diff["result"]["available"].get<bool>());
    EXPECT_TRUE(diff["result"]["diffs"].empty());

    EXPECT_THROW(service.get(run_id, "status", 1, 100), std::invalid_argument);
    EXPECT_THROW(service.get(run_id, "details", 0, 1), std::invalid_argument);
    EXPECT_THROW(service.get(run_id, "attempts", 0, 0), std::invalid_argument);
    EXPECT_THROW(service.get(run_id, "attempts", 0, 1001), std::invalid_argument);
}

TEST(HarnessServiceTest, TraceIsReadableBeforeRunDetailsExist) {
    std::atomic<bool>    release_worker{false};
    HarnessServiceConfig config;
    config.worker_executor = [&](const HarnessWorkerCall&, const auto&) {
        while (!release_worker.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));
    const auto     compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    const auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto run_id  = started["run_id"].get<std::string>();

    json trace;
    try {
        trace = service.get(run_id, "trace");
    } catch (const std::exception& error) {
        ADD_FAILURE() << error.what();
    }
    release_worker.store(true, std::memory_order_release);
    EXPECT_TRUE(trace["result"]["execution_trace"].empty());
    EXPECT_EQ(wait_terminal(service, run_id)["status"], "completed");
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

    EXPECT_EQ(
        neograph::graph::GraphCompiler::canon(preset["artifacts"]["core_lockfile"]["content"]),
        neograph::graph::GraphCompiler::canon(dsl["artifacts"]["core_lockfile"]["content"]));
}

TEST(HarnessServiceTest, ShipsReviewTriageAndResearchPresets) {
    HarnessService service;
    auto schema = service.schema();
    EXPECT_EQ(schema["preset_contracts"].size(), 4u);
    for (const auto* preset : {"pr_review_panel", "bug_triage", "research_synthesis"}) {
        auto value = request();
        value["harness"]["preset"] = preset;
        auto compiled = service.compile(value);
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        EXPECT_EQ(compiled["artifacts"]["core_lockfile"]["content"]["name"],
                  std::string("harness_") + preset);
    }
}

TEST(HarnessServiceTest, EnforcesReadOnlyWorkspaceAndEvidenceContracts) {
    HarnessService service;
    auto value = request();
    value["harness"]["preset"] = "pr_review_panel";
    value["policy"] = {
        {"read_only", true},
        {"workspace_roots", json::array({"/workspace"})},
        {"evidence_required", json::array({"file", "line", "evidence"})},
    };
    value["workers"][0]["tools"] = json::array({"repo.read"});
    value["tool_catalog"]        = json::array({{
        {"id", "repo.read"},
        {"description", "Read a workspace file"},
        {"input_schema",
                {
             {"type", "object"},
             {"properties", {{"path", {{"type", "string"}}}}},
             {"required", json::array({"path"})},
         }},
        {"read_only", true},
        {"path_arguments", json::array({"path"})},
        {"executor", {{"kind", "mcp"}, {"server_ref", "repo"}, {"tool", "read_file"}}},
    }});

    auto rejected = service.compile(value);
    EXPECT_FALSE(rejected["ok"].get<bool>());
    bool saw_evidence = false;
    for (const auto& diagnostic : rejected["diagnostics"]) {
        if (diagnostic["code"] == "H_EVIDENCE_SCHEMA") saw_evidence = true;
    }
    EXPECT_TRUE(saw_evidence) << rejected.dump();

    value["workers"][0]["output_schema"]["properties"]["findings"]["items"] = {
        {"type", "object"},
        {"required", json::array({"file", "line", "evidence"})},
        {"properties",
         {
             {"file", {{"type", "string"}}},
             {"line", {{"type", "integer"}}},
             {"evidence", {{"type", "string"}}},
         }},
    };
    auto accepted = service.compile(value);
    EXPECT_TRUE(accepted["ok"].get<bool>()) << accepted.dump();

    value["tool_catalog"][0]["read_only"] = false;
    auto write_rejected = service.compile(value);
    EXPECT_FALSE(write_rejected["ok"].get<bool>());
}

TEST(HarnessServiceTest, ProviderExecutorRunsDeclaredToolsAndValidatesPaths) {
    const auto workspace = unique_temp_path("neograph-harness-path-policy");
    std::filesystem::create_directories(workspace / "src");
    const auto expected_path =
        std::filesystem::weakly_canonical(workspace / "src/main.cpp").string();
    auto provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.role = "assistant";
    tool_request.message.tool_calls.push_back(
        {"call-1", "repo.read", R"({"path":"src/main.cpp"})"});
    neograph::ChatCompletion final;
    final.message.role = "assistant";
    final.message.content = R"({"status":"ok","findings":[]})";
    provider->completions = {tool_request, final};

    int capability_calls = 0;
    neograph::mcp::HarnessProviderExecutorConfig config;
    config.provider = provider;
    config.model = "test-model";
    config.capability_executor = [&capability_calls, expected_path](const json& tool,
                                                                    const json& arguments,
                                                                    const auto&) {
        ++capability_calls;
        EXPECT_EQ(tool["id"], "repo.read");
        EXPECT_EQ(arguments["path"], expected_path);
        return json{{"content", "int main() {}"}};
    };
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(config));

    HarnessWorkerCall call;
    call.task = {{"objective", "Review"}};
    call.worker = worker("reviewer", json::array({"repo.read"}));
    call.tool_catalog = json::array({{
        {"id", "repo.read"},
        {"description", "Read file"},
        {"input_schema",
         {
             {"type", "object"},
             {"required", json::array({"path"})},
             {"properties", {{"path", {{"type", "string"}}}}},
         }},
        {"path_arguments", json::array({"path"})},
        {"executor", {{"kind", "builtin"}}},
    }});
    call.policy = {{"workspace_roots", json::array({workspace.string()})}};
    auto response = executor(call, std::make_shared<neograph::graph::CancelToken>());

    EXPECT_EQ(response.kind, neograph::mcp::HarnessWorkerResponseKind::VALUE);
    EXPECT_EQ(response.value["status"], "ok");
    EXPECT_EQ(capability_calls, 1);
    ASSERT_EQ(provider->calls.size(), 2u);
    EXPECT_EQ(provider->calls[0].model, "test-model");
    ASSERT_EQ(provider->calls[1].messages.size(), 3u);
    EXPECT_EQ(provider->calls[1].messages.back().role, "tool");
    std::filesystem::remove_all(workspace);
}

TEST(HarnessServiceTest, ProviderExecutorRejectsWorkspaceEscapeBeforeToolCall) {
    auto provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"call-1", "repo.read", R"({"path":"../secret.txt"})"});
    provider->completions = {tool_request};

    int capability_calls = 0;
    neograph::mcp::HarnessProviderExecutorConfig config;
    config.provider = provider;
    config.capability_executor = [&capability_calls](const json&, const json&, const auto&) {
        ++capability_calls;
        return json::object();
    };
    auto              executor = neograph::mcp::make_provider_harness_executor(std::move(config));
    HarnessWorkerCall call;
    call.task = {{"objective", "Review"}};
    call.worker = worker("reviewer");
    call.tool_catalog = json::array({{
        {"id", "repo.read"},
        {"description", "Read file"},
        {"input_schema",
         {
             {"type", "object"},
             {"properties", {{"path", {{"type", "string"}}}}},
         }},
        {"path_arguments", json::array({"path"})},
        {"executor", {{"kind", "builtin"}}},
    }});
    call.policy = {{"workspace_roots", json::array({"/workspace"})}};

    auto response = executor(call, std::make_shared<neograph::graph::CancelToken>());
    EXPECT_EQ(response.kind, neograph::mcp::HarnessWorkerResponseKind::TOOL_ERROR);
    EXPECT_NE(response.message.find("escapes configured workspace roots"), std::string::npos);
    EXPECT_EQ(capability_calls, 0);
}

TEST(HarnessServiceTest, ProviderExecutorRejectsSymlinkWorkspaceEscape) {
#ifdef _WIN32
    GTEST_SKIP() << "directory symlink creation requires host privileges on Windows";
#else
    const auto base = std::filesystem::temp_directory_path() /
                      ("neograph-harness-path-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto workspace = base / "workspace";
    const auto outside = base / "outside";
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(outside);
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, workspace / "link", link_error);
    if (link_error) {
        std::filesystem::remove_all(base);
        GTEST_SKIP() << "cannot create test symlink: " << link_error.message();
    }

    auto provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"call-1", "repo.read", R"({"path":"link/secret.txt"})"});
    provider->completions = {tool_request};
    int capability_calls = 0;
    neograph::mcp::HarnessProviderExecutorConfig config;
    config.provider = provider;
    config.capability_executor = [&capability_calls](const json&, const json&, const auto&) {
        ++capability_calls;
        return json::object();
    };
    auto              executor = neograph::mcp::make_provider_harness_executor(std::move(config));
    HarnessWorkerCall call;
    call.task = {{"objective", "Review"}};
    call.worker = worker("reviewer");
    call.tool_catalog = json::array({{
        {"id", "repo.read"},
        {"description", "Read file"},
        {"input_schema",
         {
             {"type", "object"},
             {"properties", {{"path", {{"type", "string"}}}}},
         }},
        {"path_arguments", json::array({"path"})},
        {"executor", {{"kind", "builtin"}}},
    }});
    call.policy = {{"workspace_roots", json::array({workspace.string()})}};

    auto response = executor(call, std::make_shared<neograph::graph::CancelToken>());
    EXPECT_EQ(response.kind, neograph::mcp::HarnessWorkerResponseKind::TOOL_ERROR);
    EXPECT_NE(response.message.find("escapes configured workspace roots"), std::string::npos);
    EXPECT_EQ(capability_calls, 0);
    std::filesystem::remove_all(base);
#endif
}

TEST(HarnessServiceTest, ExperimentalTasksProfileNegotiatesAndResumesInput) {
    const auto           root = unique_temp_path("neograph-harness-tasks");
    TempDirectoryCleanup cleanup(root);
    auto                 provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"provider-call", "host.lookup", R"({"query":"needle"})"});
    neograph::ChatCompletion final;
    final.message.content = R"({"status":"ok","findings":[]})";
    provider->completions = {tool_request, final};

    HarnessServiceConfig config;
    config.checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    config.record_store = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    config.enable_experimental_tasks = true;
    config.poll_interval             = 25ms;
    neograph::mcp::HarnessProviderExecutorConfig provider_config;
    provider_config.provider = provider;
    config.worker_executor =
        neograph::mcp::make_provider_harness_executor(std::move(provider_config));
    HarnessService service(std::move(config));
    auto           compiled = service.compile(host_brokered_request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();

    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {{"name", "tasks-test"}, {"version", "1"}};
    neograph::mcp::MCPServer server(server_config);
    ServerResponses          responses;
    server.set_response_sink(responses.sink());
    service.register_tools(server);
    auto initialized = server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params",
         {
             {"protocolVersion", "2025-11-25"},
             {"capabilities",
              {{"extensions", {{"io.modelcontextprotocol/tasks", json::object()}}}}},
             {"clientInfo", {{"name", "test"}, {"version", "1"}}},
         }},
    });
    ASSERT_TRUE(initialized.contains("result"));
    EXPECT_TRUE(initialized["result"]["capabilities"]["extensions"].contains(
        "io.modelcontextprotocol/tasks"));
    server.handle_message({
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()},
    });

    EXPECT_TRUE(
        server
            .handle_message({
                {"jsonrpc", "2.0"},
                {"id", 2},
                {"method", "tools/call"},
                {"params",
                 {
                     {"name", "neograph_start"},
                     {"arguments", {{"artifact_id", compiled["artifact_id"]}}},
                     {"_meta",
                      {{"io.modelcontextprotocol/clientCapabilities",
                        {{"extensions", {{"io.modelcontextprotocol/tasks", json::object()}}}}}}},
                 }},
            })
            .is_null());
    auto created = responses.wait(2);
    ASSERT_TRUE(created.contains("result")) << created.dump();
    EXPECT_EQ(created["result"]["resultType"], "task");
    EXPECT_EQ(created["result"]["pollIntervalMs"], 25);
    const auto task_id = created["result"]["taskId"].get<std::string>();

    auto waiting = wait_terminal(service, task_id);
    ASSERT_EQ(waiting["status"], "awaiting_tool_results") << waiting.dump();
    auto polled = server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tasks/get"},
        {"params", {{"taskId", task_id}}},
    });
    ASSERT_EQ(polled["result"]["status"], "input_required") << polled.dump();
    ASSERT_EQ(polled["result"]["inputRequests"].size(), 1u);
    const auto call_id = polled["result"]["inputRequests"].begin().key();

    auto updated = server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tasks/update"},
        {"params",
         {
             {"taskId", task_id},
             {"inputResponses",
              {{call_id,
                {
                    {"action", "accept"},
                    {"content", {{"answer", "found"}}},
                }}}},
         }},
    });
    EXPECT_EQ(updated["result"]["resultType"], "complete") << updated.dump();
    auto finished = wait_terminal(service, task_id);
    ASSERT_EQ(finished["status"], "completed") << finished.dump();
    auto completed = server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 5},
        {"method", "tasks/get"},
        {"params", {{"taskId", task_id}}},
    });
    EXPECT_EQ(completed["result"]["status"], "completed") << completed.dump();
    EXPECT_EQ(completed["result"]["result"]["structuredContent"]["status"], "completed");
}

TEST(HarnessServiceTest, ExperimentalTasksProfileFallsBackWithoutRequestOptIn) {
    const auto           root = unique_temp_path("neograph-harness-tasks-fallback");
    TempDirectoryCleanup cleanup(root);
    HarnessServiceConfig config;
    config.checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    config.record_store = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    config.enable_experimental_tasks = true;
    config.worker_executor           = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));
    auto           compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();

    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {{"name", "tasks-fallback"}, {"version", "1"}};
    neograph::mcp::MCPServer server(server_config);
    ServerResponses          responses;
    server.set_response_sink(responses.sink());
    service.register_tools(server);
    server.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params",
         {
             {"protocolVersion", "2025-11-25"},
             {"capabilities",
              {{"extensions", {{"io.modelcontextprotocol/tasks", json::object()}}}}},
             {"clientInfo", {{"name", "test"}, {"version", "1"}}},
         }},
    });
    server.handle_message({
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()},
    });
    EXPECT_TRUE(server
                    .handle_message({
                        {"jsonrpc", "2.0"},
                        {"id", 2},
                        {"method", "tools/call"},
                        {"params",
                         {
                             {"name", "neograph_start"},
                             {"arguments", {{"artifact_id", compiled["artifact_id"]}}},
                         }},
                    })
                    .is_null());
    auto response = responses.wait(2);
    ASSERT_TRUE(response["result"].contains("structuredContent")) << response.dump();
    EXPECT_FALSE(response["result"].contains("resultType"));
    const auto run_id = response["result"]["structuredContent"]["run_id"].get<std::string>();
    EXPECT_EQ(wait_terminal(service, run_id)["status"], "completed");

    neograph::mcp::MCPServer unnegotiated(server_config);
    service.register_tools(unnegotiated);
    unnegotiated.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 10},
        {"method", "initialize"},
        {"params",
         {
             {"protocolVersion", "2025-11-25"},
             {"capabilities", json::object()},
             {"clientInfo", {{"name", "test"}, {"version", "1"}}},
         }},
    });
    unnegotiated.handle_message({
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()},
    });
    auto rejected = unnegotiated.handle_message({
        {"jsonrpc", "2.0"},
        {"id", 11},
        {"method", "tasks/get"},
        {"params", {{"taskId", run_id}}},
    });
    EXPECT_EQ(rejected["error"]["code"], -32602);
}

TEST(HarnessServiceTest, HostBrokeredToolsRequireDurableStores) {
    HarnessService service;
    auto           compiled = service.compile(host_brokered_request());
    ASSERT_FALSE(compiled["ok"].get<bool>());
    bool saw_durability_error = false;
    for (const auto& diagnostic : compiled["diagnostics"]) {
        if (diagnostic["code"] == "H_HOST_BROKER_UNAVAILABLE") {
            saw_durability_error = true;
        }
    }
    EXPECT_TRUE(saw_durability_error) << compiled.dump();
}

TEST(HarnessServiceTest, ProviderExecutorReturnsTypedHostPendingStates) {
    for (const auto& [interaction, expected] :
         std::vector<std::pair<std::string, neograph::mcp::HarnessWorkerResponseKind>>{
             {"tool_result", neograph::mcp::HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS},
             {"input", neograph::mcp::HarnessWorkerResponseKind::INPUT_REQUIRED}}) {
        auto                     provider = std::make_shared<ScriptedProvider>();
        neograph::ChatCompletion tool_request;
        tool_request.message.tool_calls.push_back(
            {"provider-call", "host.lookup", R"({"query":"needle"})"});
        provider->completions = {tool_request};
        neograph::mcp::HarnessProviderExecutorConfig config;
        config.provider = provider;
        auto executor   = neograph::mcp::make_provider_harness_executor(std::move(config));

        auto              authored = host_brokered_request(interaction);
        HarnessWorkerCall call;
        call.task         = authored["task"];
        call.worker       = authored["workers"][0];
        call.tool_catalog = authored["tool_catalog"];
        auto response     = executor(call, std::make_shared<neograph::graph::CancelToken>());
        ASSERT_EQ(response.kind, expected);
        EXPECT_EQ(response.value["tool_id"], "host.lookup");
        EXPECT_EQ(response.value["provider_call_id"], "provider-call");
        EXPECT_EQ(response.value["arguments"]["query"], "needle");
        EXPECT_FALSE(response.value.value("call_id", "").empty());
    }
}

TEST(HarnessServiceTest, WaitingHostCallCanBeCancelled) {
    const auto               root     = unique_temp_path("neograph-harness-cancel");
    auto                     provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"provider-call", "host.lookup", R"({"query":"needle"})"});
    provider->completions = {tool_request};

    HarnessServiceConfig config;
    config.checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    config.record_store = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    neograph::mcp::HarnessProviderExecutorConfig provider_config;
    provider_config.provider = provider;
    config.worker_executor =
        neograph::mcp::make_provider_harness_executor(std::move(provider_config));
    {
        HarnessService service(std::move(config));
        auto           compiled = service.compile(host_brokered_request("input"));
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        auto       started = service.start({{"artifact_id", compiled["artifact_id"]}});
        const auto run_id  = started["run_id"].get<std::string>();
        auto       waiting = wait_terminal(service, run_id);
        ASSERT_EQ(waiting["status"], "input_required") << waiting.dump();
        EXPECT_TRUE(service.cancel(run_id));
        EXPECT_EQ(service.get(run_id)["status"], "cancelled");
        EXPECT_FALSE(service.cancel(run_id));
    }
    std::filesystem::remove_all(root);
}

TEST(HarnessServiceTest, CancellationPropagatesIntoResumedWorker) {
    const auto           root = unique_temp_path("neograph-harness-resume-cancel");
    HarnessServiceConfig config;
    config.checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    config.record_store    = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    config.worker_executor = [](const HarnessWorkerCall& call, const auto& cancel) {
        if (!call.resume_value) {
            return HarnessWorkerResponse::awaiting_tool_results({
                {"call_id", "host-call"},
                {"tool_id", "host.lookup"},
                {"arguments", {{"query", "needle"}}},
                {"result_schema",
                 {
                     {"type", "object"},
                     {"required", json::array({"answer"})},
                     {"properties", {{"answer", {{"type", "string"}}}}},
                 }},
            });
        }
        while (!cancel->is_cancelled())
            std::this_thread::sleep_for(1ms);
        return HarnessWorkerResponse::cancelled("resumed worker cancelled");
    };
    {
        HarnessService service(std::move(config));
        auto           compiled = service.compile(host_brokered_request());
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        auto       started = service.start({{"artifact_id", compiled["artifact_id"]}});
        const auto run_id  = started["run_id"].get<std::string>();
        auto       waiting = wait_terminal(service, run_id);
        ASSERT_EQ(waiting["status"], "awaiting_tool_results");
        auto resumed = service.resume(
            {{"run_id", run_id}, {"call_id", "host-call"}, {"result", {{"answer", "found"}}}});
        EXPECT_TRUE(resumed["accepted"].get<bool>());
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (service.get(run_id)["status"] == "queued" &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_TRUE(service.cancel(run_id));
        auto finished = wait_terminal(service, run_id);
        EXPECT_EQ(finished["status"], "cancelled") << finished.dump();
    }
    std::filesystem::remove_all(root);
}

TEST(HarnessServiceTest, ExpiredHostResultIsRejectedAndPersisted) {
    const auto               root     = unique_temp_path("neograph-harness-expiry");
    auto                     provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"provider-call", "host.lookup", R"({"query":"needle"})"});
    provider->completions = {tool_request};

    std::string run_id;
    std::string call_id;
    {
        HarnessServiceConfig config;
        config.checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
        config.record_store =
            std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
        config.run_ttl = 500ms;
        neograph::mcp::HarnessProviderExecutorConfig provider_config;
        provider_config.provider = provider;
        config.worker_executor =
            neograph::mcp::make_provider_harness_executor(std::move(provider_config));
        HarnessService service(std::move(config));
        auto           compiled = service.compile(host_brokered_request());
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
        run_id       = started["run_id"].get<std::string>();
        auto waiting = wait_terminal(service, run_id);
        ASSERT_EQ(waiting["status"], "awaiting_tool_results");
        call_id = waiting["pending"]["call_id"].get<std::string>();
        const auto expires_at = waiting["expires_at"].get<int64_t>();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count() <= expires_at) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_THROW(
            service.resume(
                {{"run_id", run_id}, {"call_id", call_id}, {"result", {{"answer", "late"}}}}),
            std::invalid_argument);
        EXPECT_EQ(service.get(run_id)["status"], "expired");
    }
    auto records   = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    auto persisted = records->load_run(run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ((*persisted)["status"], "expired");
    std::filesystem::remove_all(root);
}

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
TEST(HarnessServiceTest, SqliteRecordStoreReopensAndKeepsArtifactsImmutable) {
    const auto root = unique_temp_path("neograph-harness-sqlite-records");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const auto database = root / "runs.db";

    const json artifact = {
        {"artifact_id", "artifact_1"},
        {"request", {{"task", "review"}}},
    };
    const json run = {
        {"run_id", "run_1"},
        {"artifact_id", "artifact_1"},
        {"status", "queued"},
    };
    {
        neograph::mcp::SqliteHarnessRecordStore records(database.string());
        records.save_artifact("artifact_1", artifact);
        records.save_artifact("artifact_1", artifact);
        records.save_run("run_1", run);

        auto changed = artifact;
        changed["request"]["task"] = "mutated";
        EXPECT_THROW(records.save_artifact("artifact_1", changed), std::invalid_argument);
    }
    {
        neograph::mcp::SqliteHarnessRecordStore records(database.string());
        ASSERT_EQ(records.load_artifact("artifact_1"), std::optional<json>(artifact));
        ASSERT_EQ(records.load_run("run_1"), std::optional<json>(run));

        auto completed = run;
        completed["status"] = "completed";
        records.save_run("run_1", completed);
        EXPECT_EQ((*records.load_run("run_1"))["status"], "completed");

        auto rebound = completed;
        rebound["artifact_id"] = "artifact_2";
        EXPECT_THROW(records.save_run("run_1", rebound), std::invalid_argument);
        EXPECT_THROW(records.save_run("run_missing", {
            {"run_id", "run_missing"},
            {"artifact_id", "artifact_missing"},
            {"status", "queued"},
        }), std::runtime_error);
    }
}

TEST(HarnessServiceTest, SqliteRecordStoreBusyTimeoutWaitsForWriter) {
    const auto root = unique_temp_path("neograph-harness-sqlite-busy");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const auto database = root / "runs.db";

    neograph::mcp::SqliteHarnessRecordStore records(database.string(), 500ms);
    records.save_artifact("artifact_1", {
        {"artifact_id", "artifact_1"},
        {"request", json::object()},
    });
    sqlite3*                                 blocker = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &blocker), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(blocker, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);

    std::exception_ptr failure;
    const auto started = std::chrono::steady_clock::now();
    std::thread writer([&] {
        try {
            records.save_run("run_waiting", {
                {"run_id", "run_waiting"},
                {"artifact_id", "artifact_1"},
                {"status", "queued"},
            });
        } catch (...) {
            failure = std::current_exception();
        }
    });
    std::this_thread::sleep_for(75ms);
    EXPECT_EQ(sqlite3_exec(blocker, "COMMIT;", nullptr, nullptr, nullptr), SQLITE_OK);
    writer.join();
    sqlite3_close(blocker);

    EXPECT_FALSE(failure);
    EXPECT_GE(std::chrono::steady_clock::now() - started, 75ms);
    EXPECT_TRUE(records.load_run("run_waiting").has_value());
}

TEST(HarnessServiceTest, SqliteJournalOrdersEventsAndRedactsPayloadsAtRest) {
    const auto root = unique_temp_path("neograph-harness-journal");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const auto database = root / "runs.db";

    neograph::mcp::SqliteHarnessRecordStore records(database.string());
    records.save_artifact("artifact_1", {
        {"artifact_id", "artifact_1"},
        {"request", json::object()},
    });
    const json run = {
        {"run_id", "run_1"},
        {"artifact_id", "artifact_1"},
        {"revision_digest", "fnv1a64:1234"},
        {"protocol_version", "2025-11-25"},
        {"profile", "harness-m4"},
        {"status", "queued"},
    };
    records.save_run("run_1", run);
    const auto event = [](std::string type, json payload) {
        return json{
            {"run_id", "run_1"},
            {"artifact_id", "artifact_1"},
            {"revision_digest", "fnv1a64:1234"},
            {"protocol_version", "2025-11-25"},
            {"profile", "harness-m4"},
            {"event_type", std::move(type)},
            {"correlation_id", "call_1"},
            {"node_id", "worker_0"},
            {"worker_id", "reviewer"},
            {"attempt", 1},
            {"payload", std::move(payload)},
        };
    };
    records.append_event(event("worker.attempt.started", {
        {"authorization", "Bearer secret"},
        {"nested", {{"safe", "visible"}, {"token", "hidden"}}},
    }));
    records.append_event(event("worker.attempt.completed", {{"result", {{"answer", "42"}}}}));

    const auto events = records.list_events("run_1");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0]["sequence"], 1);
    EXPECT_EQ(events[1]["sequence"], 2);
    EXPECT_EQ(events[0]["payload"]["authorization"], "[REDACTED]");
    EXPECT_EQ(events[0]["payload"]["nested"]["token"], "[REDACTED]");
    EXPECT_EQ(events[0]["payload"]["nested"]["safe"], "visible");
    EXPECT_EQ(events[1]["payload"]["result"], "[REDACTED]");
    ASSERT_EQ(records.list_events("run_1", 1, 1).size(), 1u);
    EXPECT_EQ(records.list_events("run_1", 1, 1)[0]["sequence"], 2);

    auto mismatched = event("tampered", json::object());
    mismatched["revision_digest"] = "fnv1a64:different";
    EXPECT_THROW(records.append_event(mismatched), std::invalid_argument);
    auto rebound = run;
    rebound["revision_digest"] = "fnv1a64:different";
    EXPECT_THROW(records.save_run("run_1", rebound), std::invalid_argument);
}

TEST(HarnessServiceTest, SqliteJournalSupportsMetadataOnlyAndFullPayloadModes) {
    const auto root = unique_temp_path("neograph-harness-journal-modes");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);

    const auto exercise = [&](const std::string& name,
                              neograph::mcp::HarnessJournalPayloadMode mode) {
        neograph::mcp::SqliteHarnessJournalConfig journal_config;
        journal_config.mode = mode;
        neograph::mcp::SqliteHarnessRecordStore records(
            (root / name).string(), 5s, std::move(journal_config));
        records.save_artifact("artifact_1", {
            {"artifact_id", "artifact_1"},
            {"request", json::object()},
        });
        records.save_run("run_1", {
            {"run_id", "run_1"},
            {"artifact_id", "artifact_1"},
            {"revision_digest", "revision"},
            {"protocol_version", "protocol"},
            {"profile", "profile"},
            {"status", "queued"},
        });
        records.append_event({
            {"run_id", "run_1"},
            {"artifact_id", "artifact_1"},
            {"revision_digest", "revision"},
            {"protocol_version", "protocol"},
            {"profile", "profile"},
            {"event_type", "test.event"},
            {"payload", {{"secret", "keep only in full"}, {"visible", true}}},
        });
        return records.list_events("run_1")[0]["payload"];
    };

    EXPECT_EQ(exercise("metadata.db", neograph::mcp::HarnessJournalPayloadMode::METADATA_ONLY),
              json::object());
    const auto full = exercise("full.db", neograph::mcp::HarnessJournalPayloadMode::FULL);
    EXPECT_EQ(full["secret"], "keep only in full");
    EXPECT_TRUE(full["visible"].get<bool>());
}

TEST(HarnessServiceTest, SqliteJournalMigratesVersionOneRunBindings) {
    const auto root = unique_temp_path("neograph-harness-journal-migration");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const auto database = root / "runs.db";
    sqlite3* legacy = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &legacy), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(legacy, R"SQL(
CREATE TABLE neograph_harness_schema (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1), version INTEGER NOT NULL
);
INSERT INTO neograph_harness_schema VALUES (1, 1);
CREATE TABLE neograph_harness_artifacts (
    artifact_id TEXT PRIMARY KEY, record_json TEXT NOT NULL, created_at_ms INTEGER NOT NULL
);
CREATE TABLE neograph_harness_runs (
    run_id TEXT PRIMARY KEY, artifact_id TEXT NOT NULL, record_json TEXT NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    FOREIGN KEY (artifact_id) REFERENCES neograph_harness_artifacts (artifact_id)
);
INSERT INTO neograph_harness_artifacts VALUES
    ('artifact_1', '{"artifact_id":"artifact_1","request":{}}', 1);
INSERT INTO neograph_harness_runs VALUES
    ('run_1', 'artifact_1', '{"run_id":"run_1","artifact_id":"artifact_1","status":"queued"}', 1);
)SQL", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(legacy);

    neograph::mcp::SqliteHarnessRecordStore records(database.string());
    auto run = records.load_run("run_1");
    ASSERT_TRUE(run.has_value());
    (*run)["revision_digest"] = "revision";
    (*run)["protocol_version"] = "protocol";
    (*run)["profile"] = "profile";
    records.save_run("run_1", *run);
    records.append_event({
        {"run_id", "run_1"},
        {"artifact_id", "artifact_1"},
        {"revision_digest", "revision"},
        {"protocol_version", "protocol"},
        {"profile", "profile"},
        {"event_type", "migration.verified"},
        {"payload", json::object()},
    });
    ASSERT_EQ(records.list_events("run_1").size(), 1u);
    EXPECT_EQ(records.list_events("run_1")[0]["event_type"], "migration.verified");
}

TEST(HarnessServiceTest, SqliteRetentionDeletesTerminalLeavesBeforeReferencedSources) {
    const auto           root = unique_temp_path("neograph-harness-retention");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    neograph::mcp::SqliteHarnessRecordStore records((root / "runs.db").string());
    for (int index = 1; index <= 3; ++index) {
        const auto artifact_id = "artifact_" + std::to_string(index);
        records.save_artifact(artifact_id, {
                                               {"artifact_id", artifact_id},
                                               {"request", json::object()},
                                           });
    }
    const auto run = [](std::string run_id, std::string artifact_id, std::string status,
                        std::string source_run_id = {}) {
        return json{
            {"run_id", std::move(run_id)},
            {"artifact_id", std::move(artifact_id)},
            {"revision_digest", "revision"},
            {"protocol_version", "protocol"},
            {"profile", "profile"},
            {"status", std::move(status)},
            {"source_run_id", std::move(source_run_id)},
        };
    };
    records.save_run("run_source", run("run_source", "artifact_1", "completed"));
    records.save_run("run_fork", run("run_fork", "artifact_2", "completed", "run_source"));
    records.save_run("run_active", run("run_active", "artifact_3", "running"));
    EXPECT_THROW(
        records.save_run("run_orphan", run("run_orphan", "artifact_2", "completed", "missing")),
        std::invalid_argument);
    records.append_event({
        {"run_id", "run_fork"},
        {"artifact_id", "artifact_2"},
        {"revision_digest", "revision"},
        {"protocol_version", "protocol"},
        {"profile", "profile"},
        {"event_type", "retention.test"},
        {"payload", json::object()},
    });

    neograph::mcp::HarnessRetentionPolicy blocked;
    blocked.max_runs      = 1;
    blocked.max_artifacts = 1;
    blocked.protected_run_ids.push_back("run_fork");
    const auto blocked_result = records.cleanup_retained(blocked);
    EXPECT_TRUE(blocked_result.run_ids.empty());
    EXPECT_TRUE(blocked_result.artifact_ids.empty());
    EXPECT_TRUE(records.load_run("run_source").has_value());
    EXPECT_TRUE(records.load_run("run_fork").has_value());

    neograph::mcp::HarnessRetentionPolicy remove_leaf;
    remove_leaf.max_runs      = 2;
    remove_leaf.max_artifacts = 2;
    const auto leaf_result    = records.cleanup_retained(remove_leaf);
    ASSERT_EQ(leaf_result.run_ids, std::vector<std::string>({"run_fork"}));
    ASSERT_EQ(leaf_result.artifact_ids, std::vector<std::string>({"artifact_2"}));
    EXPECT_FALSE(records.load_run("run_fork").has_value());
    EXPECT_TRUE(records.list_events("run_fork").empty());
    EXPECT_TRUE(records.load_run("run_source").has_value());
    EXPECT_TRUE(records.load_run("run_active").has_value());

    neograph::mcp::HarnessRetentionPolicy remove_source;
    remove_source.max_runs      = 1;
    remove_source.max_artifacts = 1;
    const auto source_result    = records.cleanup_retained(remove_source);
    ASSERT_EQ(source_result.run_ids, std::vector<std::string>({"run_source"}));
    ASSERT_EQ(source_result.artifact_ids, std::vector<std::string>({"artifact_1"}));
    EXPECT_FALSE(records.load_run("run_source").has_value());
    EXPECT_TRUE(records.load_run("run_active").has_value());
    EXPECT_TRUE(records.load_artifact("artifact_3").has_value());
}

TEST(HarnessServiceTest, HarnessCapacityCleanupPreservesReplaySourceAndDropsLeafCheckpoints) {
    const auto           root = unique_temp_path("neograph-harness-capacity-cleanup");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    auto records =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>((root / "runs.db").string());
    auto                 checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    std::atomic<int>     calls{0};
    HarnessServiceConfig config;
    config.record_store     = records;
    config.checkpoint_store = checkpoints;
    config.max_artifacts    = 2;
    config.max_runs         = 2;
    config.worker_executor  = [&](const HarnessWorkerCall&, const auto&) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));

    auto first_request                       = request();
    first_request["task"]["objective"]       = "first";
    const auto first                         = service.compile(first_request);
    auto       source_request                = request();
    source_request["task"]["objective"]      = "source";
    const auto source                        = service.compile(source_request);
    auto       replacement_request           = request();
    replacement_request["task"]["objective"] = "replacement";
    const auto replacement                   = service.compile(replacement_request);
    ASSERT_TRUE(first["ok"].get<bool>() && source["ok"].get<bool>() &&
                replacement["ok"].get<bool>());
    EXPECT_FALSE(records->load_artifact(first["artifact_id"].get<std::string>()).has_value());
    EXPECT_TRUE(records->load_artifact(source["artifact_id"].get<std::string>()).has_value());

    const auto source_start  = service.start({{"artifact_id", source["artifact_id"]}});
    const auto source_run_id = source_start["run_id"].get<std::string>();
    ASSERT_EQ(wait_terminal(service, source_run_id)["status"], "completed");
    const auto replay_start  = service.start({
        {"replay", {{"source_run_id", source_run_id}, {"mode", "live"}}},
    });
    const auto replay_run_id = replay_start["run_id"].get<std::string>();
    ASSERT_EQ(wait_terminal(service, replay_run_id)["status"], "completed");

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (records->list_events(replay_run_id).empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    std::this_thread::sleep_for(2ms);
    const auto replacement_start = service.start({{"artifact_id", replacement["artifact_id"]}});
    ASSERT_EQ(wait_terminal(service, replacement_start["run_id"].get<std::string>())["status"],
              "completed");

    EXPECT_TRUE(records->load_run(source_run_id).has_value());
    EXPECT_FALSE(records->load_run(replay_run_id).has_value());
    EXPECT_TRUE(checkpoints->list(replay_run_id).empty());
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 3);
}

TEST(HarnessServiceTest, JournalCorrelatesProviderAndCapabilityCallsToWorkerAttempt) {
    const auto root = unique_temp_path("neograph-harness-journal-correlation");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    auto records = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
        (root / "runs.db").string());
    auto provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"capability-call", "catalog.lookup", R"({"query":"needle"})"});
    neograph::ChatCompletion final;
    final.message.content = R"({"status":"ok","findings":[]})";
    provider->completions = {tool_request, final};

    auto authored = request();
    authored["workers"][0]["tools"] = json::array({"catalog.lookup"});
    authored["tool_catalog"] = json::array({{
        {"id", "catalog.lookup"},
        {"description", "Look up a catalog value"},
        {"input_schema", {
            {"type", "object"},
            {"required", json::array({"query"})},
            {"properties", {{"query", {{"type", "string"}}}}},
            {"additionalProperties", false},
        }},
        {"output_schema", {
            {"type", "object"},
            {"required", json::array({"answer"})},
            {"properties", {{"answer", {{"type", "string"}}}}},
            {"additionalProperties", false},
        }},
        {"executor", {{"kind", "mcp"}, {"server_ref", "catalog-server"}}},
    }});
    HarnessServiceConfig config;
    config.record_store = records;
    neograph::mcp::HarnessProviderExecutorConfig provider_config;
    provider_config.provider = provider;
    provider_config.capability_executor = [](const json&, const json&, const auto&) {
        return json{{"answer", "found"}};
    };
    config.worker_executor =
        neograph::mcp::make_provider_harness_executor(std::move(provider_config));
    HarnessService service(std::move(config));
    const auto compiled = service.compile(authored);
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    const auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto run_id = started["run_id"].get<std::string>();
    ASSERT_EQ(wait_terminal(service, run_id)["status"], "completed");

    const auto persisted = records->load_run(run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_FALSE((*persisted)["revision_digest"].get<std::string>().empty());
    EXPECT_EQ((*persisted)["protocol_version"], "2025-11-25");
    EXPECT_EQ((*persisted)["profile"], "harness-m4");

    std::vector<json> events;
    const auto journal_deadline = std::chrono::steady_clock::now() + 1s;
    do {
        events = records->list_events(run_id);
        bool flushed = false;
        for (const auto& event : events) {
            if (event["event_type"] == "run.terminal") {
                flushed = true;
                break;
            }
        }
        if (flushed) break;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < journal_deadline);
    std::size_t provider_started = 0;
    std::size_t provider_completed = 0;
    bool capability_started = false;
    bool capability_completed = false;
    bool worker_completed = false;
    bool terminal = false;
    for (const auto& event : events) {
        const auto type = event["event_type"].get<std::string>();
        if (type == "provider.call.started") ++provider_started;
        if (type == "provider.call.completed") ++provider_completed;
        if (type == "capability.call.started") {
            capability_started = event.value("correlation_id", "") == "capability-call";
        }
        if (type == "capability.call.completed") {
            capability_completed = event.value("correlation_id", "") == "capability-call";
        }
        if (type == "worker.attempt.completed") {
            worker_completed = event.value("node_id", "") == "worker_0" &&
                               event.value("worker_id", "") == "reviewer" &&
                               event.value("attempt", 0) == 1;
        }
        if (type == "run.terminal") terminal = event["payload"]["status"] == "completed";
    }
    EXPECT_EQ(provider_started, 2u);
    EXPECT_EQ(provider_completed, 2u);
    EXPECT_TRUE(capability_started);
    EXPECT_TRUE(capability_completed);
    EXPECT_TRUE(worker_completed);
    EXPECT_TRUE(terminal);
}

TEST(HarnessServiceTest, DebugViewsExposeJournalAndCheckpointHistoryThroughUris) {
    const auto           root = unique_temp_path("neograph-harness-debug-views");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    auto records =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>((root / "runs.db").string());
    auto checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    HarnessServiceConfig config;
    config.record_store     = records;
    config.checkpoint_store = checkpoint_store;
    config.worker_executor  = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({
            {"status", "ok"},
            {"findings", json::array({{{"evidence", "line 1"}}})},
        });
    };
    HarnessService service(std::move(config));
    const auto     compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    const auto started  = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto run_id   = started["run_id"].get<std::string>();
    const auto finished = wait_terminal(service, run_id);
    ASSERT_EQ(finished["status"], "completed") << finished.dump();

    const auto artifacts = service.get(run_id, "artifacts")["result"];
    ASSERT_TRUE(artifacts.contains("attempts"));
    ASSERT_TRUE(artifacts.contains("checkpoints"));
    ASSERT_TRUE(artifacts.contains("diff"));

    json       attempts;
    const auto journal_deadline = std::chrono::steady_clock::now() + 1s;
    do {
        attempts = service.get(run_id, "attempts", 0, 1);
        if (attempts["result"]["has_more"].get<bool>()) break;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < journal_deadline);
    ASSERT_TRUE(attempts["result"]["journal_available"].get<bool>());
    ASSERT_EQ(attempts["result"]["events"].size(), 1u);
    ASSERT_TRUE(attempts["result"]["has_more"].get<bool>());
    const auto cursor       = attempts["result"]["next_after_sequence"].get<std::size_t>();
    const auto attempts_uri = artifacts["attempts"]["uri"].get<std::string>() +
                              "?after_sequence=" + std::to_string(cursor) + "&limit=1";
    const auto next_attempt = service.read(attempts_uri);
    ASSERT_EQ(next_attempt["result"]["events"].size(), 1u);
    EXPECT_GT(next_attempt["result"]["events"][0]["sequence"].get<std::size_t>(), cursor);
    EXPECT_EQ(next_attempt["result"]["events"][0]["payload"]["output"], "[REDACTED]");

    const auto trace = service.read(artifacts["trace"]["uri"].get<std::string>() + "?limit=2");
    EXPECT_FALSE(trace["result"]["execution_trace"].empty());
    EXPECT_LE(trace["result"]["events"].size(), 2u);

    const auto checkpoints = service.read(artifacts["checkpoints"]["uri"].get<std::string>());
    ASSERT_TRUE(checkpoints["result"]["available"].get<bool>());
    ASSERT_FALSE(checkpoints["result"]["checkpoints"].empty());
    EXPECT_TRUE(checkpoints["result"]["checkpoints"][0].contains("checkpoint_id"));
    EXPECT_FALSE(checkpoints["result"]["checkpoints"][0].contains("channel_values"));
    for (const auto& checkpoint : checkpoint_store->list(run_id)) {
        ASSERT_TRUE(checkpoint.metadata.contains("harness"));
        const auto binding = checkpoint.metadata["harness"];
        EXPECT_EQ(binding["run_id"], run_id);
        EXPECT_EQ(binding["artifact_id"], compiled["artifact_id"]);
        EXPECT_EQ(binding["revision_digest"], finished["revision_digest"]);
        EXPECT_EQ(binding["protocol_version"], "2025-11-25");
        EXPECT_EQ(binding["profile"], "harness-m4");
    }

    const auto diffs = service.read(artifacts["diff"]["uri"].get<std::string>() + "?limit=2");
    ASSERT_TRUE(diffs["result"]["available"].get<bool>());
    ASSERT_FALSE(diffs["result"]["diffs"].empty());
    EXPECT_TRUE(diffs["result"]["diffs"][0].contains("changed_channels"));
    bool saw_channel_change = false;
    for (const auto& diff : diffs["result"]["diffs"]) {
        if (!diff["changed_channels"].empty()) saw_channel_change = true;
    }
    EXPECT_TRUE(saw_channel_change);

    neograph::graph::Checkpoint legacy_parent;
    legacy_parent.id             = "legacy-parent";
    legacy_parent.thread_id      = run_id;
    legacy_parent.channel_values = {{"channels", {{"legacy", "before"}}}};
    legacy_parent.step           = 100;
    checkpoint_store->save(legacy_parent);
    auto legacy_child           = legacy_parent;
    legacy_child.id             = "legacy-child";
    legacy_child.parent_id      = legacy_parent.id;
    legacy_child.channel_values = {{"channels", {{"legacy", "after"}}}};
    legacy_child.step           = 101;
    checkpoint_store->save(legacy_child);
    auto orphan      = legacy_child;
    orphan.id        = "orphan";
    orphan.parent_id = "missing-parent";
    orphan.step      = 102;
    checkpoint_store->save(orphan);

    const auto legacy_diffs = service.get(run_id, "diff", 0, 3)["result"]["diffs"];
    ASSERT_EQ(legacy_diffs.size(), 3u);
    EXPECT_FALSE(legacy_diffs[0]["parent_available"].get<bool>());
    ASSERT_EQ(legacy_diffs[1]["changed_channels"].size(), 1u);
    EXPECT_EQ(legacy_diffs[1]["changed_channels"][0]["before"], "before");
    EXPECT_EQ(legacy_diffs[1]["changed_channels"][0]["after"], "after");
    EXPECT_THROW(
        service.read(artifacts["checkpoints"]["uri"].get<std::string>() + "?after_sequence=1"),
        std::invalid_argument);
    EXPECT_THROW(service.read(artifacts["trace"]["uri"].get<std::string>() + "?unknown=1"),
                 std::invalid_argument);
    EXPECT_THROW(service.read(artifacts["trace"]["uri"].get<std::string>() + "?"),
                 std::invalid_argument);
    EXPECT_THROW(service.read(artifacts["trace"]["uri"].get<std::string>() + "?limit=1&limit=2"),
                 std::invalid_argument);
}

TEST(HarnessServiceTest, CompatibleForkResumesExactCheckpointWithTargetArtifact) {
    const auto           root = unique_temp_path("neograph-harness-compatible-fork");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    auto records     = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    auto checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    std::atomic<int>     live_calls{0};
    HarnessServiceConfig config;
    config.record_store     = records;
    config.checkpoint_store = checkpoints;
    config.worker_executor  = [&](const HarnessWorkerCall&, const auto&) {
        live_calls.fetch_add(1, std::memory_order_relaxed);
        return HarnessWorkerResponse::success({
            {"status", "ok"},
            {"findings", json::array({{{"evidence", "source checkpoint"}}})},
        });
    };
    HarnessService service(std::move(config));

    const auto source_artifact = service.compile(dsl_request("judge"));
    ASSERT_TRUE(source_artifact["ok"].get<bool>()) << source_artifact.dump();
    const auto source_start  = service.start({{"artifact_id", source_artifact["artifact_id"]}});
    const auto source_run_id = source_start["run_id"].get<std::string>();
    ASSERT_EQ(wait_terminal(service, source_run_id)["status"], "completed");
    ASSERT_EQ(live_calls.load(std::memory_order_relaxed), 1);

    std::string source_checkpoint_id;
    for (const auto& checkpoint : checkpoints->list(source_run_id)) {
        if (checkpoint.next_nodes.size() == 1 && checkpoint.next_nodes[0] == "judge") {
            source_checkpoint_id = checkpoint.id;
            break;
        }
    }
    ASSERT_FALSE(source_checkpoint_id.empty());

    auto repaired_request                          = dsl_request("judge");
    repaired_request["workers"][0]["instructions"] = "Repaired worker instructions";
    const auto target_artifact                     = service.compile(repaired_request);
    ASSERT_TRUE(target_artifact["ok"].get<bool>()) << target_artifact.dump();
    ASSERT_NE(target_artifact["artifact_id"], source_artifact["artifact_id"]);

    const auto started = service.start({
        {"fork",
         {{"source_run_id", source_run_id},
          {"checkpoint_id", source_checkpoint_id},
          {"artifact_id", target_artifact["artifact_id"]}}},
    });
    ASSERT_TRUE(started["started"].get<bool>()) << started.dump();
    EXPECT_EQ(started["execution_mode"], "compatible_fork");
    EXPECT_EQ(started["source_run_id"], source_run_id);
    EXPECT_EQ(started["source_checkpoint_id"], source_checkpoint_id);

    const auto fork_run_id = started["run_id"].get<std::string>();
    const auto finished    = wait_terminal(service, fork_run_id);
    ASSERT_EQ(finished["status"], "completed") << finished.dump();
    EXPECT_EQ(finished["execution_mode"], "compatible_fork");
    EXPECT_EQ(finished["source_checkpoint_id"], source_checkpoint_id);
    ASSERT_EQ(finished["result"]["findings"].size(), 1u);
    EXPECT_EQ(finished["result"]["findings"][0]["evidence"], "source checkpoint");
    EXPECT_EQ(live_calls.load(std::memory_order_relaxed), 1)
        << "forking after the worker checkpoint must not execute the worker again";

    bool saw_fork_anchor = false;
    for (const auto& checkpoint : checkpoints->list(fork_run_id)) {
        if (!checkpoint.metadata.contains("forked_from")) continue;
        EXPECT_EQ(checkpoint.parent_id, source_checkpoint_id);
        EXPECT_EQ(checkpoint.metadata["forked_from"]["thread_id"], source_run_id);
        EXPECT_EQ(checkpoint.metadata["forked_from"]["checkpoint_id"], source_checkpoint_id);
        saw_fork_anchor = true;
    }
    EXPECT_TRUE(saw_fork_anchor);
}

TEST(HarnessServiceTest, CompatibleForkLoadsSourceAndTargetAcrossSqliteReconnect) {
    const auto           root = unique_temp_path("neograph-harness-fork-reconnect");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const auto       records_database    = root / "runs.db";
    const auto       checkpoint_database = root / "checkpoints.db";
    std::atomic<int> live_calls{0};
    std::string      source_run_id;
    std::string      source_checkpoint_id;
    std::string      target_artifact_id;

    {
        auto checkpoints =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(checkpoint_database.string());
        HarnessServiceConfig config;
        config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(records_database.string());
        config.checkpoint_store = checkpoints;
        config.worker_executor  = [&](const HarnessWorkerCall&, const auto&) {
            live_calls.fetch_add(1, std::memory_order_relaxed);
            return HarnessWorkerResponse::success({
                {"status", "ok"},
                {"findings", json::array({{{"evidence", "durable source"}}})},
            });
        };
        HarnessService service(std::move(config));
        const auto     source_artifact = service.compile(dsl_request("judge"));
        ASSERT_TRUE(source_artifact["ok"].get<bool>()) << source_artifact.dump();
        const auto source_start = service.start({{"artifact_id", source_artifact["artifact_id"]}});
        source_run_id           = source_start["run_id"].get<std::string>();
        ASSERT_EQ(wait_terminal(service, source_run_id)["status"], "completed");
        for (const auto& checkpoint : checkpoints->list(source_run_id)) {
            if (checkpoint.next_nodes.size() == 1 && checkpoint.next_nodes[0] == "judge") {
                source_checkpoint_id = checkpoint.id;
                break;
            }
        }
        ASSERT_FALSE(source_checkpoint_id.empty());

        auto repaired                          = dsl_request("judge");
        repaired["workers"][0]["instructions"] = "Durably repaired instructions";
        const auto target_artifact             = service.compile(repaired);
        ASSERT_TRUE(target_artifact["ok"].get<bool>()) << target_artifact.dump();
        target_artifact_id = target_artifact["artifact_id"].get<std::string>();
    }

    {
        HarnessServiceConfig config;
        config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(records_database.string());
        config.checkpoint_store =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(checkpoint_database.string());
        config.worker_executor = [&](const HarnessWorkerCall&, const auto&) {
            live_calls.fetch_add(1, std::memory_order_relaxed);
            return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
        };
        HarnessService service(std::move(config));
        const auto     started = service.start({
            {"fork",
                 {{"source_run_id", source_run_id},
                  {"checkpoint_id", source_checkpoint_id},
                  {"artifact_id", target_artifact_id}}},
        });
        ASSERT_TRUE(started["started"].get<bool>()) << started.dump();
        const auto finished = wait_terminal(service, started["run_id"].get<std::string>());
        ASSERT_EQ(finished["status"], "completed") << finished.dump();
        ASSERT_EQ(finished["result"]["findings"].size(), 1u);
        EXPECT_EQ(finished["result"]["findings"][0]["evidence"], "durable source");
    }
    EXPECT_EQ(live_calls.load(std::memory_order_relaxed), 1);
}

TEST(HarnessServiceTest, IncompatibleForkReturnsChannelAndContinuationDiagnosticsBeforeRun) {
    const auto           root = unique_temp_path("neograph-harness-incompatible-fork");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    auto records     = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    auto checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    HarnessServiceConfig config;
    config.record_store     = records;
    config.checkpoint_store = checkpoints;
    config.max_runs         = 1;
    config.worker_executor  = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));

    const auto source_artifact = service.compile(dsl_request("source_judge"));
    ASSERT_TRUE(source_artifact["ok"].get<bool>()) << source_artifact.dump();
    const auto source_start  = service.start({{"artifact_id", source_artifact["artifact_id"]}});
    const auto source_run_id = source_start["run_id"].get<std::string>();
    ASSERT_EQ(wait_terminal(service, source_run_id)["status"], "completed");

    std::string source_checkpoint_id;
    for (const auto& checkpoint : checkpoints->list(source_run_id)) {
        if (checkpoint.next_nodes.size() == 1 && checkpoint.next_nodes[0] == "source_judge") {
            source_checkpoint_id = checkpoint.id;
            break;
        }
    }
    ASSERT_FALSE(source_checkpoint_id.empty());

    const auto target_artifact = service.compile(dsl_request("target_judge", "overwrite"));
    ASSERT_TRUE(target_artifact["ok"].get<bool>()) << target_artifact.dump();
    const auto rejected = service.start({
        {"fork",
         {{"source_run_id", source_run_id},
          {"checkpoint_id", source_checkpoint_id},
          {"artifact_id", target_artifact["artifact_id"]}}},
    });
    ASSERT_FALSE(rejected["started"].get<bool>());
    EXPECT_EQ(rejected["status"], "incompatible_fork");
    EXPECT_EQ(rejected["source_checkpoint_id"], source_checkpoint_id);

    bool saw_reducer   = false;
    bool saw_next_node = false;
    for (const auto& diagnostic : rejected["diagnostics"]) {
        EXPECT_EQ(diagnostic["phase"], "compatibility");
        EXPECT_TRUE(diagnostic.contains("witness"));
        if (diagnostic["code"] == "H_FORK_CHANNEL_REDUCER") {
            EXPECT_EQ(diagnostic["witness"]["channel"], "worker_results");
            EXPECT_EQ(diagnostic["witness"]["source_reducer"], "append");
            EXPECT_EQ(diagnostic["witness"]["target_reducer"], "overwrite");
            saw_reducer = true;
        }
        if (diagnostic["code"] == "H_FORK_NEXT_NODE") {
            EXPECT_EQ(diagnostic["witness"]["node_id"], "source_judge");
            saw_next_node = true;
        }
    }
    EXPECT_TRUE(saw_reducer) << rejected.dump();
    EXPECT_TRUE(saw_next_node) << rejected.dump();
}

TEST(HarnessServiceTest, RecordedReplaySurvivesReconnectWithoutCallingLiveExecutor) {
    const auto           root = unique_temp_path("neograph-harness-recorded-replay");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    neograph::mcp::SqliteHarnessJournalConfig journal_config;
    journal_config.mode = neograph::mcp::HarnessJournalPayloadMode::FULL;
    auto records        = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
        (root / "runs.db").string(), 5s, journal_config);
    std::atomic<int> live_calls{0};
    std::string      source_run_id;
    json             source_workers;

    {
        HarnessServiceConfig config;
        config.record_store    = records;
        config.worker_executor = [&](const HarnessWorkerCall& call, const auto&) {
            live_calls.fetch_add(1, std::memory_order_relaxed);
            const auto worker_id = call.worker["id"].get<std::string>();
            if (worker_id == "a" && call.attempt == 1) {
                return HarnessWorkerResponse::parse_error("recorded parse failure");
            }
            return HarnessWorkerResponse::success({
                {"status", "ok"},
                {"findings", json::array({{{"evidence", "source " + worker_id}}})},
            });
        };
        HarnessService service(std::move(config));
        const auto     compiled = service.compile(request(json::array({worker("a"), worker("b")})));
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        const auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
        source_run_id      = started["run_id"].get<std::string>();
        const auto source  = wait_terminal(service, source_run_id);
        ASSERT_EQ(source["status"], "completed") << source.dump();
        source_workers = source["result"]["workers"];
        EXPECT_EQ(live_calls.load(std::memory_order_relaxed), 3);
    }

    HarnessServiceConfig config;
    config.record_store    = records;
    config.worker_executor = [&](const HarnessWorkerCall& call, const auto&) {
        live_calls.fetch_add(1, std::memory_order_relaxed);
        const auto worker_id = call.worker["id"].get<std::string>();
        if (worker_id == "a" && call.attempt == 1) {
            return HarnessWorkerResponse::parse_error("live parse failure");
        }
        return HarnessWorkerResponse::success({
            {"status", "ok"},
            {"findings", json::array({{{"evidence", "live " + worker_id}}})},
        });
    };
    HarnessService service(std::move(config));

    const auto recorded = service.start({
        {"replay", {{"source_run_id", source_run_id}, {"mode", "recorded"}}},
    });
    ASSERT_TRUE(recorded["started"].get<bool>());
    EXPECT_EQ(recorded["execution_mode"], "recorded_replay");
    EXPECT_EQ(recorded["source_run_id"], source_run_id);
    const auto recorded_run_id = recorded["run_id"].get<std::string>();
    const auto replayed        = wait_terminal(service, recorded_run_id);
    ASSERT_EQ(replayed["status"], "completed") << replayed.dump();
    EXPECT_EQ(replayed["execution_mode"], "recorded_replay");
    EXPECT_EQ(replayed["source_run_id"], source_run_id);
    EXPECT_EQ(replayed["result"]["workers"], source_workers);
    EXPECT_EQ(live_calls.load(std::memory_order_relaxed), 3)
        << "recorded replay must not invoke provider/tool execution";

    const auto live = service.start({
        {"replay", {{"source_run_id", source_run_id}, {"mode", "live"}}},
    });
    EXPECT_EQ(live["execution_mode"], "live_replay");
    const auto live_result = wait_terminal(service, live["run_id"].get<std::string>());
    ASSERT_EQ(live_result["status"], "completed") << live_result.dump();
    EXPECT_EQ(live_result["execution_mode"], "live_replay");
    EXPECT_EQ(live_result["source_run_id"], source_run_id);
    EXPECT_EQ(live_calls.load(std::memory_order_relaxed), 6);
    ASSERT_EQ(live_result["result"]["findings"].size(), 2u);
    EXPECT_EQ(live_result["result"]["findings"][0]["evidence"], "live a");
    EXPECT_EQ(live_result["result"]["findings"][1]["evidence"], "live b");
}

TEST(HarnessServiceTest, RecordedReplayRejectsRedactedWorkerOutputs) {
    const auto           root = unique_temp_path("neograph-harness-redacted-replay");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    auto records =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>((root / "runs.db").string());
    HarnessServiceConfig config;
    config.record_store    = records;
    config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
    };
    HarnessService service(std::move(config));
    const auto     compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    const auto started       = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto source_run_id = started["run_id"].get<std::string>();
    ASSERT_EQ(wait_terminal(service, source_run_id)["status"], "completed");

    EXPECT_THROW(service.start({
                     {"replay", {{"source_run_id", source_run_id}, {"mode", "recorded"}}},
                 }),
                 std::runtime_error);
}

TEST(HarnessServiceTest, RecordedReplayPreservesTerminalWorkerFailure) {
    const auto           root = unique_temp_path("neograph-harness-failed-replay");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    neograph::mcp::SqliteHarnessJournalConfig journal_config;
    journal_config.mode = neograph::mcp::HarnessJournalPayloadMode::FULL;
    auto records        = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
        (root / "runs.db").string(), 5s, journal_config);
    std::atomic<int>     live_calls{0};
    HarnessServiceConfig config;
    config.record_store    = records;
    config.worker_executor = [&](const HarnessWorkerCall&, const auto&) {
        live_calls.fetch_add(1, std::memory_order_relaxed);
        return HarnessWorkerResponse::timeout("recorded timeout");
    };
    HarnessService service(std::move(config));
    const auto     compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    const auto started       = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto source_run_id = started["run_id"].get<std::string>();
    const auto source        = wait_terminal(service, source_run_id);
    ASSERT_EQ(source["status"], "completed") << source.dump();
    ASSERT_EQ(source["result"]["workers"][0]["failure_kind"], "timeout");

    const auto replay   = service.start({
        {"replay", {{"source_run_id", source_run_id}, {"mode", "recorded"}}},
    });
    const auto replayed = wait_terminal(service, replay["run_id"].get<std::string>());
    ASSERT_EQ(replayed["status"], "completed") << replayed.dump();
    EXPECT_EQ(replayed["result"]["workers"], source["result"]["workers"]);
    EXPECT_EQ(live_calls.load(std::memory_order_relaxed), 1);

    const auto events               = records->list_events(source_run_id);
    bool       saw_terminal_attempt = false;
    for (const auto& event : events) {
        if (event["event_type"] == "worker.attempt.completed" &&
            event["payload"].value("retry_reason", "") == "timeout") {
            EXPECT_FALSE(event["payload"]["retry_scheduled"].get<bool>());
            saw_terminal_attempt = true;
        }
    }
    EXPECT_TRUE(saw_terminal_attempt);
}

TEST(HarnessServiceTest, DurableHostResumeSurvivesServiceAndDatabaseReconnect) {
    const auto root     = unique_temp_path("neograph-harness-resume");
    const auto database = root / "checkpoints.db";
    const auto records_database = root / "runs.db";
    std::filesystem::create_directories(root);
    neograph::mcp::SqliteHarnessJournalConfig journal_config;
    journal_config.mode               = neograph::mcp::HarnessJournalPayloadMode::FULL;
    auto                     provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"provider-call", "host.lookup", R"({"query":"needle"})"});
    neograph::ChatCompletion final;
    final.message.content = R"({"status":"ok","findings":[]})";
    provider->completions = {tool_request, final};

    std::string run_id;
    std::string call_id;
    {
        HarnessServiceConfig config;
        config.checkpoint_store =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(database.string());
        config.record_store = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
            records_database.string(), 5s, journal_config);
        neograph::mcp::HarnessProviderExecutorConfig provider_config;
        provider_config.provider = provider;
        config.worker_executor =
            neograph::mcp::make_provider_harness_executor(std::move(provider_config));
        HarnessService service(std::move(config));
        auto           compiled = service.compile(host_brokered_request());
        ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
        auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
        run_id       = started["run_id"].get<std::string>();
        auto waiting = wait_terminal(service, run_id);
        ASSERT_EQ(waiting["status"], "awaiting_tool_results") << waiting.dump();
        call_id = waiting["pending"]["call_id"].get<std::string>();
    }

    {
        HarnessServiceConfig config;
        config.checkpoint_store =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(database.string());
        config.record_store = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
            records_database.string(), 5s, journal_config);
        neograph::mcp::HarnessProviderExecutorConfig provider_config;
        provider_config.provider = provider;
        config.worker_executor =
            neograph::mcp::make_provider_harness_executor(std::move(provider_config));
        HarnessService service(std::move(config));

        auto restored = service.get(run_id);
        ASSERT_EQ(restored["status"], "awaiting_tool_results");
        EXPECT_EQ(restored["pending"]["call_id"], call_id);
        EXPECT_THROW(
            service.resume(
                {{"run_id", run_id}, {"call_id", "wrong"}, {"result", {{"answer", "found"}}}}),
            std::invalid_argument);
        EXPECT_THROW(
            service.resume(
                {{"run_id", run_id}, {"call_id", call_id}, {"result", {{"unexpected", true}}}}),
            std::invalid_argument);

        auto accepted = service.resume(
            {{"run_id", run_id}, {"call_id", call_id}, {"result", {{"answer", "found"}}}});
        EXPECT_TRUE(accepted["accepted"].get<bool>());
        auto duplicate = service.resume(
            {{"run_id", run_id}, {"call_id", call_id}, {"result", {{"answer", "found"}}}});
        EXPECT_TRUE(duplicate["duplicate"].get<bool>());

        auto finished = wait_terminal(service, run_id);
        ASSERT_EQ(finished["status"], "completed") << finished.dump();
        EXPECT_EQ(finished["result"]["outcome"], "zero_findings");
        EXPECT_THROW(
            service.resume(
                {{"run_id", run_id}, {"call_id", "late"}, {"result", {{"answer", "found"}}}}),
            std::invalid_argument);
    }
    ASSERT_EQ(provider->calls.size(), 2u);
    EXPECT_NE(provider->calls[1].messages[0].content.find("host_resume"), std::string::npos);
    {
        HarnessServiceConfig config;
        config.checkpoint_store =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(database.string());
        config.record_store = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
            records_database.string(), 5s, journal_config);
        neograph::mcp::HarnessProviderExecutorConfig provider_config;
        provider_config.provider = provider;
        config.worker_executor =
            neograph::mcp::make_provider_harness_executor(std::move(provider_config));
        HarnessService service(std::move(config));
        const auto     replay   = service.start({
            {"replay", {{"source_run_id", run_id}, {"mode", "recorded"}}},
        });
        const auto     replayed = wait_terminal(service, replay["run_id"].get<std::string>());
        ASSERT_EQ(replayed["status"], "completed") << replayed.dump();
        EXPECT_EQ(replayed["execution_mode"], "recorded_replay");
    }
    EXPECT_EQ(provider->calls.size(), 2u)
        << "replaying a resumed run must not repeat provider or host calls";
    {
        neograph::mcp::SqliteHarnessRecordStore records(records_database.string(), 5s,
                                                        journal_config);
        const auto events = records.list_events(run_id);
        std::string requested;
        std::string accepted;
        for (const auto& event : events) {
            if (event["event_type"] == "host_brokered.call.requested") {
                requested = event.value("correlation_id", "");
            }
            if (event["event_type"] == "host_brokered.result.accepted") {
                accepted = event.value("correlation_id", "");
            }
        }
        EXPECT_EQ(requested, call_id);
        EXPECT_EQ(accepted, call_id);
    }
    std::filesystem::remove_all(root);
}

TEST(HarnessServiceTest, PersistedResumeIntentRecoversBeforeThreadSpawn) {
    const auto root     = unique_temp_path("neograph-harness-resume-intent");
    const auto database = root / "checkpoints.db";
    const auto records_database = root / "runs.db";
    std::filesystem::create_directories(root);
    auto                     provider = std::make_shared<ScriptedProvider>();
    neograph::ChatCompletion tool_request;
    tool_request.message.tool_calls.push_back(
        {"provider-call", "host.lookup", R"({"query":"needle"})"});
    neograph::ChatCompletion final;
    final.message.content = R"({"status":"ok","findings":[]})";
    provider->completions = {tool_request, final};

    std::string run_id;
    std::string call_id;
    {
        HarnessServiceConfig config;
        config.checkpoint_store =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(database.string());
        config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(records_database.string());
        neograph::mcp::HarnessProviderExecutorConfig provider_config;
        provider_config.provider = provider;
        config.worker_executor =
            neograph::mcp::make_provider_harness_executor(std::move(provider_config));
        HarnessService service(std::move(config));
        auto           compiled = service.compile(host_brokered_request());
        ASSERT_TRUE(compiled["ok"].get<bool>());
        auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
        run_id       = started["run_id"].get<std::string>();
        auto waiting = wait_terminal(service, run_id);
        ASSERT_EQ(waiting["status"], "awaiting_tool_results");
        call_id = waiting["pending"]["call_id"].get<std::string>();
    }

    auto records =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(records_database.string());
    auto record  = records->load_run(run_id);
    ASSERT_TRUE(record.has_value());
    (*record)["consumed"][call_id] = {{"answer", "found"}};
    (*record)["pending"]           = nullptr;
    (*record)["resume_value"]      = {{"call_id", call_id}, {"result", {{"answer", "found"}}}};
    (*record)["status"]            = "queued";
    records->save_run(run_id, *record);

    {
        HarnessServiceConfig config;
        config.checkpoint_store =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(database.string());
        config.record_store = records;
        neograph::mcp::HarnessProviderExecutorConfig provider_config;
        provider_config.provider = provider;
        config.worker_executor =
            neograph::mcp::make_provider_harness_executor(std::move(provider_config));
        HarnessService service(std::move(config));
        (void)service.get(run_id);
        auto finished = wait_terminal(service, run_id);
        ASSERT_EQ(finished["status"], "completed") << finished.dump();
        EXPECT_EQ(finished["result"]["outcome"], "zero_findings");
    }
    std::filesystem::remove_all(root);
}
#endif

TEST(HarnessServiceTest, BindingDiagnosticCarriesElaboratorSourceCoordinate) {
    HarnessService service;
    auto authored = request();
    authored["harness"]     = {
        {"mode", "dsl"},
        {"definition",
             {
             {"schema_version", 1},
             {"channels",
                  {
                  {"task", {{"reducer", "overwrite"}, {"initial", json::object()}}},
                  {"worker_results", {{"reducer", "append"}, {"initial", json::array()}}},
                  {"final_result", {{"reducer", "overwrite"}, {"initial", nullptr}}},
              }},
             {"nodes",
                  {
                  {"judge",
                       {
                       {"type", "neograph_harness_judge"},
                       {"barrier", {{"wait_for", json::array({"panel_worker"})}}},
                   }},
              }},
             {"edges", json::array({
                           {{"from", "__start__"}, {"to", "panel_worker"}},
                           {{"from", "panel_worker"}, {"to", "judge"}},
                           {{"from", "judge"}, {"to", "__end__"}},
                       })},
             {"templates",
                  {
                  {"worker",
                       {
                       {"params", json::array()},
                       {"nodes",
                            {
                            {"worker",
                                 {
                                 {"type", "neograph_harness_worker"},
                                 {"worker_id", "undeclared"},
                             }},
                        }},
                   }},
              }},
             {"use", json::array({{
                         {"template", "worker"},
                         {"prefix", "panel"},
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
        EXPECT_NE(diagnostic["source"].get<std::string>().find("use[0]"), std::string::npos);
        EXPECT_NE(diagnostic["source"].get<std::string>().find("'worker'"), std::string::npos);
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
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
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

TEST(HarnessServiceTest, ReturnsCompactResultAndUriLinkedDetails) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({
            {"status", "ok"},
            {"findings", json::array({{{"evidence", "line 1"}}})},
        });
    };
    HarnessService service(std::move(config));
    auto compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto run_id = started["run_id"].get<std::string>();
    (void)wait_terminal(service, run_id);

    auto compact = service.get(run_id);
    ASSERT_EQ(compact["status"], "completed");
    EXPECT_EQ(compact["result"]["finding_count"], 1);
    EXPECT_FALSE(compact["result"].contains("workers"));
    EXPECT_EQ(compact["result"]["artifacts"]["details"]["uri"],
              "neograph://runs/" + run_id + "/details");
    auto details = service.get(run_id, "details");
    EXPECT_EQ(details["result"]["workers"].size(), 1u);
    auto linked = service.read(compact["result"]["artifacts"]["details"]["uri"].get<std::string>());
    EXPECT_EQ(linked, details);
    auto trace = service.get(run_id, "trace");
    EXPECT_TRUE(trace["result"]["execution_trace"].is_array());
    EXPECT_THROW(service.get(run_id, "invalid"), std::invalid_argument);
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
        EXPECT_EQ(finished["result"]["workers"][0]["failure_kind"], "empty_response");
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
        while (!cancel->is_cancelled())
            std::this_thread::sleep_for(1ms);
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
        while (!cancel->is_cancelled())
            std::this_thread::sleep_for(1ms);
        return HarnessWorkerResponse::cancelled("test cancellation");
    };
    HarnessService service(std::move(config));
    auto compiled = service.compile(request());
    ASSERT_TRUE(compiled["ok"].get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled["artifact_id"]}});
    const auto run_id = started["run_id"].get<std::string>();

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (service.get(run_id)["status"] == "queued" &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(service.cancel(run_id));
    auto finished = wait_terminal(service, run_id);
    EXPECT_EQ(finished["status"], "cancelled") << finished.dump();
}

TEST(HarnessServiceTest, ExhaustedMaxStepsHasDistinctRunState) {
    HarnessServiceConfig config;
    config.worker_executor = [](const HarnessWorkerCall&, const auto&) {
        return HarnessWorkerResponse::success({{"status", "ok"}, {"findings", json::array()}});
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
