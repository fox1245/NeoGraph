#include <neograph/graph/checkpoint.h>
#include <neograph/graph/compiler.h>

#include <gtest/gtest.h>
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
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
    for (const auto& tool : listed["result"]["tools"]) {
        EXPECT_TRUE(tool.contains("outputSchema"));
        EXPECT_EQ(tool["inputSchema"].value("type", ""), "object");
    }

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
TEST(HarnessServiceTest, DurableHostResumeSurvivesServiceAndDatabaseReconnect) {
    const auto root     = unique_temp_path("neograph-harness-resume");
    const auto database = root / "checkpoints.db";
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
            std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
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
        config.record_store =
            std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
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
    std::filesystem::remove_all(root);
}

TEST(HarnessServiceTest, PersistedResumeIntentRecoversBeforeThreadSpawn) {
    const auto root     = unique_temp_path("neograph-harness-resume-intent");
    const auto database = root / "checkpoints.db";
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
            std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
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

    auto records = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
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
