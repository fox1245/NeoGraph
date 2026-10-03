#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>
#include <neograph/harness/contract.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_host_agent.h>
#include <neograph/mcp/harness_program_store.h>
#include <neograph/mcp/server.h>
#include <neograph/provider.h>
#include <neograph/program/runtime.h>
#include <neograph/provider_outcome_codec.h>
#include "fixtures/typed_provider.h"
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
#include <neograph/mcp/sqlite_harness_store.h>

#include <sqlite3.h>
#endif

#include <asio/error.hpp>
#include <asio/system_error.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {
using neograph::json;
std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

json canonical_json(const json& value) {
    if (value.is_object()) {
        std::vector<std::string> keys;
        for (const auto& [key, child] : value.items()) {
            (void)child;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        json result = json::object();
        for (const auto& key : keys)
            result[key] = canonical_json(value.at(key));
        return result;
    }
    if (value.is_array()) {
        json result = json::array();
        for (const auto& child : value)
            result.push_back(canonical_json(child));
        return result;
    }
    return value;
}

class RepeatingToolProvider final : public neograph::test::LocalProvider {
    struct State {
        std::vector<sp::runtime::Result> completions;
        std::atomic<unsigned> calls{0};
        std::vector<std::optional<std::uint64_t>> max_tokens;
    };
public:
    explicit RepeatingToolProvider(std::shared_ptr<State> state = std::make_shared<State>())
        : LocalProvider([state](neograph::ProviderRequest request,
                                const neograph::PreparedProviderRequest&, const auto&)
                                -> asio::awaitable<sp::runtime::Result> {
              state->max_tokens.push_back(
                  std::get<sp::chat::Request>(request.payload).max_output_tokens);
              const auto index = state->calls.fetch_add(1, std::memory_order_relaxed);
              if (index >= state->completions.size())
                  throw std::runtime_error("unexpected provider call");
              co_return state->completions[index];
          }, "repeating-tool-provider", neograph::test::bounded_client()),
          completions(state->completions), calls(state->calls), max_tokens(state->max_tokens) {}
    std::vector<sp::runtime::Result>& completions;
    std::atomic<unsigned>& calls;
    std::vector<std::optional<std::uint64_t>>& max_tokens;
};

class ConcurrentBudgetProvider final : public neograph::test::LocalProvider {
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        bool wait_for_two = true;
        int started = 0;
    };
public:
    explicit ConcurrentBudgetProvider(bool wait_for_two = true)
        : ConcurrentBudgetProvider(wait_for_two, std::make_shared<State>()) {}
    int started() {
        std::lock_guard lock(state_->mutex);
        return state_->started;
    }
private:
    ConcurrentBudgetProvider(bool wait_for_two, std::shared_ptr<State> state)
        : LocalProvider([state](neograph::ProviderRequest request, const auto&, const auto&)
                            -> asio::awaitable<sp::runtime::Result> {
              int index;
              {
                  std::unique_lock lock(state->mutex);
                  index = state->started++;
                  state->ready.notify_all();
                  if (state->wait_for_two) {
                      const bool both_started = state->ready.wait_for(lock, 2s, [state, request] {
                          return state->started >= 2 ||
                              (request.cancel_token && request.cancel_token->is_cancelled());
                      });
                      if (!both_started || state->started < 2) {
                          if (request.cancel_token && request.cancel_token->is_cancelled())
                              throw neograph::graph::CancelledException("sibling budget cancellation");
                          throw std::runtime_error("concurrent budget provider did not reach two callers");
                      }
                  }
              }
              if (index == 0)
                  co_return neograph::test::success("{}", neograph::test::usage(0, 20, 20));
              while (!request.cancel_token->is_cancelled()) {
                  std::unique_lock lock(state->mutex);
                  state->ready.wait_for(lock, 1ms);
              }
              throw neograph::graph::CancelledException("sibling budget cancellation");
          }, "concurrent-budget-provider", neograph::test::bounded_client()),
          state_(std::move(state)) {
        state_->wait_for_two = wait_for_two;
    }
    std::shared_ptr<State> state_;
};

class ThrowOnceProvider final : public neograph::test::LocalProvider {
public:
    explicit ThrowOnceProvider(std::shared_ptr<std::atomic<unsigned>> count =
                                  std::make_shared<std::atomic<unsigned>>(0))
        : LocalProvider([count](auto, const auto&, const auto&)
                            -> asio::awaitable<sp::runtime::Result> {
              if (count->fetch_add(1, std::memory_order_relaxed) == 0)
                  throw std::runtime_error("transient provider failure");
              co_return neograph::test::success("{}", neograph::test::usage(0, 0, 0));
          }, "throw-once-provider", neograph::test::bounded_client()), calls(*count) {}
    std::atomic<unsigned>& calls;
};

class TimeoutProvider final : public neograph::test::LocalProvider {
public:
    explicit TimeoutProvider(std::shared_ptr<std::atomic<unsigned>> count =
                                 std::make_shared<std::atomic<unsigned>>(0))
        : LocalProvider([count](neograph::ProviderRequest request, const auto&, const auto&)
                            -> asio::awaitable<sp::runtime::Result> {
              count->fetch_add(1, std::memory_order_relaxed);
              while (!request.cancel_token->is_cancelled()) std::this_thread::sleep_for(1ms);
              throw neograph::graph::CancelledException("provider timeout");
              co_return nullptr;
          }, "timeout-provider", neograph::test::bounded_client()), calls(*count) {}
    std::atomic<unsigned>& calls;
};

class ParentCancelsThenAsioErrorProvider final : public neograph::test::LocalProvider {
public:
    explicit ParentCancelsThenAsioErrorProvider(
        std::shared_ptr<neograph::graph::CancelToken> parent)
        : LocalProvider([parent = std::move(parent)](auto, const auto&, const auto&)
                            -> asio::awaitable<sp::runtime::Result> {
              parent->cancel();
              throw asio::system_error(asio::error::make_error_code(asio::error::timed_out));
              co_return nullptr;
          }, "parent-cancels-then-asio-error-provider", neograph::test::bounded_client()) {}
};

class MutableHarnessRecordStore final : public neograph::mcp::HarnessRecordStore,
                                      public neograph::mcp::HarnessProgramAdapterStore {
public:
    void save_artifact(const std::string& artifact_id, const json& record) override {
        std::lock_guard lock(mutex_);
        artifacts_[artifact_id] = record;
    }

    std::optional<json> load_artifact(const std::string& artifact_id) override {
        std::lock_guard lock(mutex_);
        const auto      it = artifacts_.find(artifact_id);
        return it == artifacts_.end() ? std::nullopt : std::optional<json>{it->second};
    }

    void save_run(const std::string& run_id, const json& record) override {
        std::lock_guard lock(mutex_);
        runs_[run_id] = record;
    }

    std::optional<json> load_run(const std::string& run_id) override {
        std::lock_guard lock(mutex_);
        const auto      it = runs_.find(run_id);
        return it == runs_.end() ? std::nullopt : std::optional<json>{it->second};
    }

    std::shared_ptr<neograph::program::ProgramTransitionStore> bind_program_transitions(
        neograph::mcp::HarnessProgramArtifactRecord) override {
        std::lock_guard lock(mutex_);
        if (!transitions_)
            transitions_ = std::make_shared<neograph::program::InMemoryProgramTransitionStore>();
        return transitions_;
    }

    std::optional<neograph::mcp::HarnessProgramRunRecord> resolve_program_run(
        std::string_view owner_scope, std::string_view run_id) const override {
        std::lock_guard lock(mutex_);
        const auto found = runs_.find(std::string(run_id));
        if (found == runs_.end()) return std::nullopt;
        auto record = neograph::mcp::HarnessProgramRunRecord::parse(found->second);
        return record.owner_scope() == owner_scope
            ? std::optional<neograph::mcp::HarnessProgramRunRecord>(std::move(record))
            : std::nullopt;
    }

    void mutate_artifact(const std::string&                artifact_id,
                         const std::function<void(json&)>& mutation) {
        std::lock_guard lock(mutex_);
        mutation(artifacts_.at(artifact_id));
    }

private:
    mutable std::mutex          mutex_;
    std::map<std::string, json> artifacts_;
    std::map<std::string, json> runs_;
    std::shared_ptr<neograph::program::ProgramTransitionStore> transitions_;
};

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
          {"max_program_operations", 4},
          {"max_worker_retries", 0}}},
    };
}

std::string harness_javascript_source(std::uint32_t max_retries, std::string_view main_body) {
    std::string source = R"JS(
        export function define() {
            const graph = ng.graph("harness_fanout_judge");
            graph.channel("task", {reducer: "overwrite", initial: {}});
            graph.channel("worker_results", {reducer: "append", initial: []});
            graph.channel("final_result", {reducer: "overwrite", initial: null});
            graph.node("worker", {
                type: "neograph_harness_worker",
                worker_id: "reviewer",
                instructions: "Return findings",
                tool_ids: [],
                tool_descriptions: {},
                output_schema: {
                    type: "object",
                    required: ["status", "findings"],
                    properties: {
                        status: {type: "string"},
                        findings: {type: "array"}
                    },
                    additionalProperties: false
                },
                provider_timeout_ms: 30000,
                max_output_tokens: 100,
                input_token_ceiling: 16384,
                max_retries: )JS";
    source += std::to_string(max_retries);
    source += R"JS(,
                max_provider_tool_rounds: 8,
                evidence_required: [],
                read_only: false
            });
            graph.node("judge", {
                type: "neograph_harness_judge",
                barrier: {wait_for: ["worker"]}
            });
            graph.edge("__start__", "worker");
            graph.edge("worker", "judge");
            graph.edge("judge", "__end__");
            return graph;
        }

        export function* main(input) {
    )JS";
    source += main_body;
    source += R"JS(
        }
    )JS";
    return source;
}

std::string harness_define_only_source(std::uint32_t max_retries) {
    auto       source      = harness_javascript_source(max_retries, "return {};");
    const auto main_export = source.find("export function* main");
    if (main_export == std::string::npos)
        throw std::logic_error("Harness JavaScript test source omitted main export");
    source.erase(main_export);
    return source;
}

struct HarnessFixture {
    std::shared_ptr<std::atomic<int>> calls_state = std::make_shared<std::atomic<int>>(0);
    std::atomic<int>& calls = *calls_state;
    neograph::mcp::HarnessServiceConfig    config;
    neograph::mcp::HarnessServiceResources resources;

    explicit HarnessFixture(neograph::mcp::HarnessWorkerExecutor executor = {},
                            json provider_host_configuration              = json::object(),
                            neograph::mcp::HarnessCapabilityExecutor capability_executor = {})
        : resources(make_resources(executor ? std::move(executor) : success_executor(),
                                   std::move(provider_host_configuration),
                                   std::move(capability_executor))) {}

    neograph::mcp::HarnessWorkerExecutor success_executor() {
        return [calls = calls_state](const neograph::mcp::HarnessWorkerCall&,
                      const std::shared_ptr<neograph::graph::CancelToken>&) {
            ++*calls;
            return neograph::mcp::HarnessWorkerResponse::success(
                {{"status", "ok"}, {"findings", json::array({"grounded"})}});
        };
    }

    neograph::mcp::HarnessServiceResources make_resources(
        neograph::mcp::HarnessWorkerExecutor     executor,
        json                                     provider_host_configuration,
        neograph::mcp::HarnessCapabilityExecutor capability_executor) {
        neograph::mcp::HarnessProgramHostConfig host;
        host.worker_executor             = std::move(executor);
        host.compiler_build_id           = "harness-cutover-test-v1";
        host.provider_binding_identity   = digest('c');
        host.provider_host_configuration = std::move(provider_host_configuration);
        host.snapshots.owner_scope       = "harness-cutover-test";
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
        host.snapshots.allowed_capabilities   = {"tool.invoke"};
        host.snapshots.allowed_effects        = {"filesystem.read"};
        host.snapshots.allowed_module_digests = {
            digest('a'), digest('b'), provider.implementation_digest, tool.implementation_digest};
        host.snapshots.budget_ceiling = {10000, 1000000, 100000, 8, 1000, 100, 100, 8, 100};
        host.checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
        host.state_store = std::make_shared<neograph::graph::InMemoryStore>();
        host.capability_executor =
            capability_executor
                ? std::move(capability_executor)
                : neograph::mcp::HarnessCapabilityExecutor(
                      [](const json&, const json&, const auto&) { return json::object(); });
        host.tool_binding_identities.emplace(tool.name, digest('f'));
        config.translation_defaults.provider          = provider;
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

#ifdef __linux__
TEST(HarnessProgramCutover, AuthenticatedHostCliRunsCompileStartGetWithoutProviderKey) {
    struct LocalCli {
        struct Workspace {
            std::filesystem::path path;
            Workspace() {
                auto pattern = (std::filesystem::temp_directory_path() /
                                "neograph-harness-host-flow-XXXXXX").string();
                std::vector<char> bytes(pattern.begin(), pattern.end());
                bytes.push_back('\0');
                auto dir = ::mkdtemp(bytes.data());
                if (!dir) throw std::runtime_error("cannot create host fixture");
                try { path = dir; }
                catch (...) { ::rmdir(dir); throw; }
            }
            ~Workspace() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } root;
        std::filesystem::path binary;
        LocalCli() {
            binary = root.path / "cli";
            std::ofstream out(binary);
            out << "#!/bin/sh\n"
                   "if [ -n \"${OPENAI_API_KEY:-}${NEOGRAPH_HARNESS_API_KEY:-}${CODEX_API_KEY:-}\" ]; then exit 17; fi\n"
                   "case \"$1\" in\n"
                   " --version) printf '2.2.0\\n'; exit 0;;\n"
                   " auth) printf '1 credentials\\n'; exit 0;;\n"
                   " models) printf 'openai/test\\n'; exit 0;;\n"
                   " run) if [ \"$2\" = --help ]; then printf '%s\\n' '--format --model'; exit 0; fi;;\n"
                   "esac\n"
                   "printf '%s\\n' "
                   "'{\"type\":\"text\",\"part\":{\"type\":\"text\",\"text\":\"{\\\"status\\\":\\\"ok\\\",\\\"findings\\\":[\\\"grounded\\\"]}\"}}' "
                   "'{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":25,\"output\":16,\"reasoning\":0,\"cache\":{\"read\":0,\"write\":0}}}}'\n";
            out.close();
            ::chmod(binary.c_str(), 0700);
        }
    } cli;
    neograph::mcp::HostAgentExecutorConfig agent_config;
    agent_config.host = "opencode";
    agent_config.executable = cli.binary.string();
    agent_config.model = "openai/test";
    agent_config.workspace = cli.root.path;
    HarnessFixture fixture(neograph::mcp::make_host_agent_executor(agent_config),
                           {{"executor", "opencode"}, {"model", agent_config.model}});
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto value = request();
    value["policy"] = {{"read_only", true}};
    auto compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto terminal = await_terminal(service, started.at("run_id").get<std::string>());
    ASSERT_EQ(terminal.at("status"), "completed") << terminal.dump();
    EXPECT_EQ(terminal.at("result").at("outcome"), "ok");
    EXPECT_EQ(terminal.at("result").at("valid_workers"), 1);
}
#endif

TEST(HarnessProgramCutover, DrainOnlyRetainedArtifactCannotStartNewRun) {
    HarnessFixture fixture;
    auto           records      = std::make_shared<MutableHarnessRecordStore>();
    fixture.config.record_store = records;
    std::string artifact_id;
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto                    compiled = service.compile(request());
        ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
        artifact_id = compiled.at("artifact_id").get<std::string>();

        const auto stored = records->load_artifact(artifact_id);
        ASSERT_TRUE(stored.has_value());
        EXPECT_EQ(stored->at("projection").at("stored_artifact_classification"), "translated");
    }

    records->mutate_artifact(artifact_id, [](json& record) {
        json retained = json::object();
        for (const auto& [key, value] : record.at("projection").items()) {
            if (key != "authoring_frontend" && key != "stored_artifact_classification")
                retained[key] = value;
        }
        record["projection"] = std::move(retained);
    });

    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto                    blocked = service.start({{"artifact_id", artifact_id}});
    ASSERT_FALSE(blocked.at("started").get<bool>()) << blocked.dump();
    EXPECT_EQ(blocked.at("status"), "migration_blocked");
    ASSERT_EQ(blocked.at("diagnostics").size(), 1U);
    EXPECT_EQ(blocked.at("diagnostics")[0].at("code"), "P_MIGRATION_DRAIN_ONLY");
    EXPECT_EQ(fixture.calls.load(), 0);
}

TEST(HarnessProgramCutover, DefineOnlyJavaScriptRetainsCoreOutputContract) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          value          = request();
    value["budgets"]["provider_timeout_seconds"] = 30;
    value["budgets"]["max_output_tokens"]        = 100;
    value["harness"]                             = {{"mode", "javascript"},
                                                    {"source_id", "harness:define-only.js"},
                                                    {"source", harness_define_only_source(0)}};

    const auto compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    const auto bundle =
        json::parse(compiled.at("artifacts").at("core_lockfile").at("content").get<std::string>());
    const auto& output_schema = bundle.at("output_contract").at("schema");
    EXPECT_EQ(output_schema.at("required"), json::array({"channels"}));
    EXPECT_EQ(output_schema.at("properties").at("channels").at("required"),
              json::array({"final_result"}));

    const auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto result = await_terminal(service, started.at("run_id").get<std::string>());
    ASSERT_EQ(result.at("status"), "completed") << result.dump();
    EXPECT_EQ(result.at("result").at("outcome"), "ok");
    EXPECT_EQ(result.at("result").at("valid_workers"), 1);
    EXPECT_EQ(fixture.calls.load(), 1);
}

TEST(HarnessProgramCutover, JavaScriptAuthoringUsesProgramSourceAndAdmittedRuntime) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          value          = request();
    value["budgets"]["provider_timeout_seconds"] = 30;
    value["budgets"]["max_output_tokens"]        = 100;
    value["harness"]                             = {{"mode", "javascript"},
                                                    {"source_id", "harness:direct.js"},
                                                    {"source", harness_javascript_source(0,
                                                                                         R"JS(
            yield ng.callCore(
                "harness_fanout_judge", {task: input.task}, "review:initial");
            const results = yield ng.all([
                ng.callCore("harness_fanout_judge", {task: input.task}, "review:first"),
                ng.callCore("harness_fanout_judge", {task: input.task}, "review:second")
            ], {max_in_flight: 2}, "review:all");
            return results[0].channels.final_result.value;
         )JS")}};

    const auto compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    EXPECT_FALSE(compiled.at("bundle_id").get<std::string>().empty());
    const auto bundle =
        json::parse(compiled.at("artifacts").at("core_lockfile").at("content").get<std::string>());
    EXPECT_EQ(bundle.at("source_kind"), "javascript");
    EXPECT_EQ(bundle.at("input_contract").at("schema").at("required"), json::array({"task"}));
    EXPECT_EQ(canonical_json(bundle.at("output_contract").at("schema")),
              canonical_json(neograph::mcp::harness_program_output_schema()));
    for (const auto& requirement : bundle.at("declared_budget_requirements")) {
        if (requirement.at("resource") == "max_concurrency")
            EXPECT_EQ(requirement.at("minimum"), 2);
        if (requirement.at("resource") == "max_program_operations")
            EXPECT_EQ(requirement.at("minimum"), 4);
        EXPECT_EQ(requirement.at("minimum"), requirement.at("maximum"));
    }
    const auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto result = await_terminal(service, started.at("run_id").get<std::string>());
    ASSERT_EQ(result.at("status"), "completed") << result.dump();
    EXPECT_EQ(result.at("result").at("valid_workers"), 1);
    EXPECT_EQ(fixture.calls.load(), 3);
}

TEST(HarnessProgramCutover, JavaScriptMalformedTerminalResultFailsItsAdvertisedContract) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          value          = request();
    value["budgets"]["provider_timeout_seconds"] = 30;
    value["budgets"]["max_output_tokens"]        = 100;
    value["harness"]                             = {{"mode", "javascript"},
                                                    {"source_id", "harness:malformed-result.js"},
                                                    {"source", harness_javascript_source(0, "return {};")}};

    const auto compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    const auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto terminal = await_terminal(service, started.at("run_id").get<std::string>());
    ASSERT_EQ(terminal.at("status"), "failed") << terminal.dump();
    ASSERT_TRUE(terminal.contains("failure")) << terminal.dump();
    EXPECT_EQ(terminal.at("failure").at("code"), "P_OUTPUT_CONTRACT");
    EXPECT_EQ(fixture.calls.load(), 0);
}

TEST(HarnessProgramCutover, JavaScriptWorkersMustMatchHostSealedConfigurationsBeforePublication) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto                          value          = request();
    value["budgets"]["provider_timeout_seconds"] = 30;
    value["budgets"]["max_output_tokens"]        = 100;

    auto       absent_worker = harness_javascript_source(0, "return {};");
    const auto id_position   = absent_worker.find("worker_id: \"reviewer\"");
    ASSERT_NE(id_position, std::string::npos);
    absent_worker.replace(id_position, std::string("worker_id: \"reviewer\"").size(),
                          "worker_id: \"not-requested\"");
    const std::vector<std::pair<std::string, std::string>> rejected_sources = {
        {harness_javascript_source(1, "return {};"), "H_WORKER_CONFIG_MISMATCH"},
        {std::move(absent_worker), "H_WORKER_BINDING"},
    };
    for (const auto& [source, expected_code] : rejected_sources) {
        value["harness"]    = {{"mode", "javascript"}, {"source", source}};
        const auto rejected = service.compile(value);
        ASSERT_FALSE(rejected.at("ok").get<bool>()) << rejected.dump();
        ASSERT_EQ(rejected.at("diagnostics").size(), 1U) << rejected.dump();
        EXPECT_EQ(rejected.at("diagnostics")[0].at("code"), expected_code);
        EXPECT_TRUE(rejected.at("artifacts").empty());
        EXPECT_FALSE(rejected.contains("artifact_id"));
    }
    EXPECT_EQ(fixture.calls.load(), 0);
}

TEST(HarnessProgramCutover, HostConfigurationChangesProviderBindingAndArtifactIdentity) {
    HarnessFixture                first({}, {{"route", "provider-a"}, {"region", "test"}});
    HarnessFixture                second({}, {{"route", "provider-b"}, {"region", "test"}});
    neograph::mcp::HarnessService first_service(first.config, nullptr, first.resources);
    neograph::mcp::HarnessService second_service(second.config, nullptr, second.resources);

    const auto first_compiled  = first_service.compile(request());
    const auto second_compiled = second_service.compile(request());
    ASSERT_TRUE(first_compiled.at("ok").get<bool>()) << first_compiled.dump();
    ASSERT_TRUE(second_compiled.at("ok").get<bool>()) << second_compiled.dump();
    EXPECT_NE(first_compiled.at("artifact_id"), second_compiled.at("artifact_id"));
    EXPECT_NE(first_compiled.at("program_version_id"), second_compiled.at("program_version_id"));
}

TEST(HarnessProgramCutover, ProviderTokenBudgetCancelsRepeatedToolRequests) {
    auto                     provider = std::make_shared<RepeatingToolProvider>();
    const auto first = neograph::test::success(std::vector<sp::Message>{
        {"first", sp::Role::Assistant, {sp::ToolCall{
            "call-1", "harness.lookup", sp::ToolCallKind::ClientExecuted,
            neograph::test::document("{}")}}}}, neograph::test::usage(1, 20, 21));
    const auto second = neograph::test::success(std::vector<sp::Message>{
        {"second", sp::Role::Assistant, {sp::ToolCall{
            "call-2", "harness.lookup", sp::ToolCallKind::ClientExecuted,
            neograph::test::document("{}")}}}}, neograph::test::usage(1, 2, 3));
    provider->completions = {first, second};

    std::atomic<unsigned>                        capability_calls{0};
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider        = provider;
    provider_config.max_tool_rounds = 8;
    provider_config.capability_executor =
        [&capability_calls](const json&, const json&,
                            const std::shared_ptr<neograph::graph::CancelToken>&) {
            capability_calls.fetch_add(1, std::memory_order_relaxed);
            return json::object();
        };
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(provider_config));

    HarnessFixture fixture(std::move(executor));
    fixture.config.translation_defaults.max_output_tokens             = 1;
    fixture.config.translation_defaults.input_token_ceiling_per_round = 1;
    auto value                                                        = request();
    value["workers"][0]["tools"] = json::array({"harness.lookup"});
    value["tool_catalog"]        = json::array({{
        {"id", "harness.lookup"},
        {"description", "Read a bounded result"},
        {"input_schema", {{"type", "object"}, {"additionalProperties", true}}},
        {"output_schema", {{"type", "object"}, {"additionalProperties", true}}},
        {"read_only", true},
        {"executor", {{"kind", "builtin"}}},
    }});

    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto                    compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    const auto started =
        service.start({{"artifact_id", compiled.at("artifact_id").get<std::string>()}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto terminal = await_terminal(service, started.at("run_id").get<std::string>());

    EXPECT_EQ(terminal.at("status"), "max_steps_exhausted") << terminal.dump();
    EXPECT_EQ(provider->calls.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(capability_calls.load(std::memory_order_relaxed), 0U);
    ASSERT_EQ(provider->max_tokens.size(), 1U);
    EXPECT_EQ(provider->max_tokens.front(), 1);
}

TEST(HarnessProgramCutover, ProviderExceptionRetainsUncertainTokenReservation) {
    auto                                         provider = std::make_shared<ThrowOnceProvider>();
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider        = provider;
    provider_config.max_tool_rounds = 1;
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(provider_config));

    neograph::mcp::HarnessWorkerCall call;
    call.task               = {{"objective", "retry budget"}, {"acceptance", json::array()}};
    call.worker             = {{"worker_id", "retry-worker"},
                               {"instructions", "return JSON"},
                               {"output_schema", {{"type", "object"}}},
                               {"_harness_provider_budget",
                                {{"max_output_tokens", 1}, {"input_token_ceiling", 1}}}};
    call.usage              = std::make_shared<neograph::UsageAccumulator>();
    call.model_token_budget = 2;
    call.budget_exhausted   = std::make_shared<std::atomic_bool>(false);
    auto cancel             = std::make_shared<neograph::graph::CancelToken>();

    const auto failed = executor(call, cancel);
    EXPECT_EQ(failed.kind, neograph::mcp::HarnessWorkerResponseKind::TOOL_ERROR);
    EXPECT_EQ(call.usage->total_tokens_wide(), 2);
    EXPECT_FALSE(cancel->is_cancelled());

    const auto retried = executor(call, cancel);
    EXPECT_EQ(retried.kind, neograph::mcp::HarnessWorkerResponseKind::CANCELLED);
    EXPECT_EQ(provider->calls.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(call.usage->total_tokens_wide(), 2);
    // A fresh independent budget may settle reported final known-zero usage;
    // this does not renew the exhausted authority above.
    call.usage = std::make_shared<neograph::UsageAccumulator>();
    call.budget_exhausted = std::make_shared<std::atomic_bool>(false);
    cancel = std::make_shared<neograph::graph::CancelToken>();
    EXPECT_EQ(executor(call, cancel).kind, neograph::mcp::HarnessWorkerResponseKind::VALUE);
    EXPECT_EQ(executor(call, cancel).kind, neograph::mcp::HarnessWorkerResponseKind::VALUE);
    EXPECT_EQ(provider->calls.load(std::memory_order_relaxed), 3U);
    EXPECT_EQ(call.usage->total_tokens_wide(), 0);
}

TEST(HarnessProgramCutover, ProviderTimeoutRetainsUncertainTokenReservation) {
    auto                                         provider = std::make_shared<TimeoutProvider>();
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider        = provider;
    provider_config.max_tool_rounds = 1;
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(provider_config));

    neograph::mcp::HarnessWorkerCall call;
    call.task   = {{"objective", "timeout budget"}, {"acceptance", json::array()}};
    call.worker = {
        {"worker_id", "timeout-worker"},
        {"instructions", "wait for cancellation"},
        {"output_schema", {{"type", "object"}}},
        {"_harness_provider_budget", {{"max_output_tokens", 1}, {"input_token_ceiling", 1},
                                      {"provider_timeout_seconds", 1}}}};
    call.usage              = std::make_shared<neograph::UsageAccumulator>();
    call.model_token_budget = 2;
    call.budget_exhausted   = std::make_shared<std::atomic_bool>(false);
    auto cancel             = std::make_shared<neograph::graph::CancelToken>();

    const auto timed_out = executor(call, cancel);
    EXPECT_EQ(timed_out.kind, neograph::mcp::HarnessWorkerResponseKind::TIMEOUT);
    EXPECT_EQ(provider->calls.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(call.usage->total_tokens_wide(), 2);
    EXPECT_FALSE(cancel->is_cancelled());
}

TEST(HarnessProgramCutover, ProviderCancellationWinsOverTransportTimeoutCode) {
    auto parent = std::make_shared<neograph::graph::CancelToken>();
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider = std::make_shared<ParentCancelsThenAsioErrorProvider>(parent);
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(provider_config));

    neograph::mcp::HarnessWorkerCall call;
    call.task   = {{"objective", "cancellation precedence"}};
    call.worker = {{"worker_id", "cancel-worker"},
                   {"instructions", "return JSON"},
                   {"output_schema", {{"type", "object"}}}};

    const auto response = executor(call, parent);
    EXPECT_EQ(response.kind, neograph::mcp::HarnessWorkerResponseKind::CANCELLED);
    EXPECT_TRUE(parent->is_cancelled());
}
TEST(HarnessProgramCutover, ProviderBudgetCancellationFansOutToConcurrentWorkers) {
    for (int attempt = 0; attempt != 32; ++attempt) {
        auto provider = std::make_shared<ConcurrentBudgetProvider>();
        neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
        provider_config.provider        = provider;
        provider_config.max_tool_rounds = 8;
        auto executor = neograph::mcp::make_provider_harness_executor(std::move(provider_config));

        neograph::mcp::HarnessWorkerCall call;
        call.task               = {{"objective", "budget probe"}, {"acceptance", json::array()}};
        call.worker             = {{"worker_id", "budget-worker"},
                                   {"instructions", "return JSON"},
                                   {"output_schema", {{"type", "object"}}},
                                   {"_harness_provider_budget",
                                    {{"max_output_tokens", 1}, {"input_token_ceiling", 1}}}};
        call.usage              = std::make_shared<neograph::UsageAccumulator>();
        call.model_token_budget = 4;
        call.budget_exhausted   = std::make_shared<std::atomic_bool>(false);
        auto cancel             = std::make_shared<neograph::graph::CancelToken>();

        auto       first  = std::async(std::launch::async, [&] { return executor(call, cancel); });
        auto       second = std::async(std::launch::async, [&] { return executor(call, cancel); });
        const auto first_status  = first.wait_for(2s);
        const auto second_status = second.wait_for(2s);
        if (first_status != std::future_status::ready ||
            second_status != std::future_status::ready) {
            cancel->cancel();
        }
        ASSERT_EQ(first_status, std::future_status::ready) << "attempt " << attempt;
        ASSERT_EQ(second_status, std::future_status::ready) << "attempt " << attempt;
        const auto first_response  = first.get();
        const auto second_response = second.get();

        EXPECT_EQ(first_response.kind, neograph::mcp::HarnessWorkerResponseKind::CANCELLED);
        EXPECT_EQ(second_response.kind, neograph::mcp::HarnessWorkerResponseKind::CANCELLED);
        EXPECT_EQ(provider->started(), 2);
        EXPECT_TRUE(cancel->is_cancelled());
        EXPECT_TRUE(call.budget_exhausted->load(std::memory_order_acquire));
        // The reporting worker charged 20; its uncertain cancelled sibling's
        // two-token reservation remains nonrenewable and separate from reports.
        EXPECT_EQ(call.usage->total_tokens_wide(), 22);
        const auto reports = call.usage->snapshot();
        ASSERT_TRUE(reports.total.has_value());
        EXPECT_EQ(reports.total->value, 20);
    }
}

TEST(HarnessProgramCutover, ProgramBudgetCancellationStopsWorkerGraph) {
    auto provider = std::make_shared<ConcurrentBudgetProvider>(false);
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider        = provider;
    provider_config.max_tool_rounds = 8;
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(provider_config));

    HarnessFixture fixture(std::move(executor));
    fixture.config.translation_defaults.max_output_tokens             = 1;
    fixture.config.translation_defaults.input_token_ceiling_per_round = 1;
    fixture.config.translation_defaults.max_provider_tool_rounds      = 1;
    auto value                                                        = request();
    value["workers"].push_back(value["workers"][0]);
    value["workers"][1]["id"]                = "second-reviewer";
    value["budgets"]["max_parallel_workers"] = 2;

    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto                    compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    const auto started =
        service.start({{"artifact_id", compiled.at("artifact_id").get<std::string>()}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto terminal = await_terminal(service, started.at("run_id").get<std::string>());

    EXPECT_EQ(terminal.at("status"), "max_steps_exhausted") << terminal.dump();
    EXPECT_EQ(provider->started(), 1);
}

TEST(HarnessProgramCutover, ProviderToolRoundBudgetCancelsRepeatedToolRequests) {
    auto provider = std::make_shared<RepeatingToolProvider>();
    for (int i = 0; i != 9; ++i) {
        auto completion = neograph::test::success(std::vector<sp::Message>{
            {"tool-request", sp::Role::Assistant, {sp::ToolCall{
                "tool-call-" + std::to_string(i), "harness.lookup",
                sp::ToolCallKind::ClientExecuted, neograph::test::document("{}")}}}},
            neograph::test::usage(0, 0, 0));
        provider->completions.push_back(std::move(completion));
    }

    std::atomic<unsigned>                        capability_calls{0};
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider        = provider;
    provider_config.max_tool_rounds = 8;
    provider_config.capability_executor =
        [&capability_calls](const json&, const json&,
                            const std::shared_ptr<neograph::graph::CancelToken>&) {
            capability_calls.fetch_add(1, std::memory_order_relaxed);
            return json::object();
        };
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(provider_config));

    HarnessFixture fixture(std::move(executor));
    auto           value         = request();
    value["workers"][0]["tools"] = json::array({"harness.lookup"});
    value["tool_catalog"]        = json::array({{
        {"id", "harness.lookup"},
        {"description", "Read a bounded result"},
        {"input_schema", {{"type", "object"}, {"additionalProperties", true}}},
        {"output_schema", {{"type", "object"}, {"additionalProperties", true}}},
        {"read_only", true},
        {"executor", {{"kind", "builtin"}}},
    }});

    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto                    compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    const auto started =
        service.start({{"artifact_id", compiled.at("artifact_id").get<std::string>()}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto terminal = await_terminal(service, started.at("run_id").get<std::string>());

    EXPECT_EQ(terminal.at("status"), "max_steps_exhausted") << terminal.dump();
    EXPECT_EQ(provider->calls.load(std::memory_order_relaxed), 9U);
    EXPECT_EQ(capability_calls.load(std::memory_order_relaxed), 8U);
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

TEST(HarnessProgramCutover, SchemaAdvertisesAllInstalledPresetsAndAdmissionProfile) {
    HarnessFixture                fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto                    schema = service.schema();
    ASSERT_EQ(schema.at("presets").size(), 4U);
    ASSERT_EQ(schema.at("preset_contracts").size(), 4U);
    EXPECT_EQ(schema.at("preset_contracts").at("fanout_judge").at("core_name"),
              "harness_fanout_judge");
    EXPECT_EQ(schema.at("preset_contracts").at("research_synthesis").at("mode"), "preset");
    EXPECT_EQ(schema.at("admission_profile").at("id"),
              fixture.resources.snapshots.admission_profile.id());
}

TEST(HarnessProgramCutover, DuplicateInputResumeIsIdempotent) {
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
    const auto                    started = service.start({{"request", request()}});
    const auto                    run_id  = started.at("run_id").get<std::string>();
    const auto                    paused  = await_terminal(service, run_id);
    ASSERT_EQ(paused.at("status"), "input_required") << paused.dump();
    const auto call_id = paused.at("pending").at("call_id").get<std::string>();
    const json result{{"approved", true}};

    const auto accepted =
        service.resume({{"run_id", run_id}, {"call_id", call_id}, {"result", result}});
    EXPECT_TRUE(accepted.at("accepted").get<bool>());
    EXPECT_FALSE(accepted.at("duplicate").get<bool>());

    const auto duplicate =
        service.resume({{"run_id", run_id}, {"call_id", call_id}, {"result", result}});
    EXPECT_TRUE(duplicate.at("accepted").get<bool>());
    EXPECT_TRUE(duplicate.at("duplicate").get<bool>());
    EXPECT_EQ(await_terminal(service, run_id).at("status"), "completed");
    EXPECT_EQ(fixture.calls.load(), 2);
}

TEST(HarnessProgramCutover, ConcurrentDuplicateResumeKeepsActiveHandle) {
    std::mutex              resume_mutex;
    std::condition_variable resume_cv;
    bool                    resumed_started = false;
    bool                    release_resume  = false;
    HarnessFixture*         fixture_ptr     = nullptr;
    HarnessFixture          fixture([&](const neograph::mcp::HarnessWorkerCall& call,
                               const std::shared_ptr<neograph::graph::CancelToken>&) {
        ++fixture_ptr->calls;
        if (!call.resume_value)
            return neograph::mcp::HarnessWorkerResponse::input_required(
                {{"kind", "input"},
                 {"result_schema", {{"type", "object"}}},
                 {"payload", {{"question", "approve?"}}}});
        {
            std::unique_lock lock(resume_mutex);
            resumed_started = true;
            resume_cv.notify_all();
            resume_cv.wait(lock, [&] { return release_resume; });
        }
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"resumed"})}});
    });
    fixture_ptr = &fixture;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto                    started = service.start({{"request", request()}});
    const auto                    run_id  = started.at("run_id").get<std::string>();
    const auto                    paused  = await_terminal(service, run_id);
    ASSERT_EQ(paused.at("status"), "input_required") << paused.dump();
    const auto call_id = paused.at("pending").at("call_id").get<std::string>();
    const json result{{"approved", true}};

    auto first           = std::async(std::launch::async, [&] {
        return service.resume({{"run_id", run_id}, {"call_id", call_id}, {"result", result}});
    });
    bool observed_resume = false;
    {
        std::unique_lock lock(resume_mutex);
        observed_resume = resume_cv.wait_for(lock, 1s, [&] { return resumed_started; });
    }
    EXPECT_TRUE(observed_resume);
    auto duplicate = std::async(std::launch::async, [&] {
        return service.resume({{"run_id", run_id}, {"call_id", call_id}, {"result", result}});
    });
    EXPECT_EQ(duplicate.wait_for(1s), std::future_status::ready);

    {
        std::lock_guard lock(resume_mutex);
        release_resume = true;
    }
    resume_cv.notify_all();

    const auto first_result     = first.get();
    const auto duplicate_result = duplicate.get();
    EXPECT_TRUE(first_result.at("accepted").get<bool>());
    EXPECT_TRUE(duplicate_result.at("accepted").get<bool>());
    EXPECT_TRUE(duplicate_result.at("duplicate").get<bool>());
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
                    {{"tool_results", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}}},
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
    const auto                    started = service.start({{"request", request()}});
    const auto                    run_id  = started.at("run_id").get<std::string>();
    const auto                    paused  = await_terminal(service, run_id);

    ASSERT_EQ(paused.at("status"), "awaiting_tool_results") << paused.dump();
    ASSERT_TRUE(paused.at("pending").is_object());
    EXPECT_EQ(paused.at("pending").at("call_id"), "tool-call-1");
    EXPECT_EQ(paused.at("pending").at("effect_id"), "tool-effect-1");
    EXPECT_EQ(paused.at("pending").at("state"), "awaiting");
    EXPECT_THROW((void)service.resume(
                     {{"run_id", run_id}, {"call_id", "wrong-call"}, {"result", json::object()}}),
                 neograph::program::ProgramDiagnosticError);
    EXPECT_EQ(fixture.calls.load(), 1);

    const auto accepted = service.resume(
        {{"run_id", run_id},
         {"call_id", "tool-call-1"},
         {"result",
          {{"tool_results", json::array({{{"name", "search"}, {"result", {{"matches", 3}}}}})}}}});
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

TEST(HarnessProgramCutover, RecordedReplayTransfersExactCustodyOnceWithoutRenewingHeldSpend) {
    auto provider = std::make_shared<ThrowOnceProvider>();
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider = provider;
    provider_config.max_tool_rounds = 1;
    HarnessFixture fixture(neograph::mcp::make_provider_harness_executor(std::move(provider_config)));
    fixture.config.translation_defaults.max_output_tokens = 1;
    fixture.config.translation_defaults.input_token_ceiling_per_round = 1;
    fixture.config.translation_defaults.max_provider_tool_rounds = 1;
    std::shared_ptr<neograph::program::ProgramRuntime> runtime;
    std::shared_ptr<neograph::program::ProgramCatalog> catalog;
    std::shared_ptr<neograph::program::ProgramTransitionStore> transitions;
    const auto runtime_factory = fixture.resources.make_program_runtime;
    fixture.resources.make_program_runtime =
        [&, runtime_factory](auto admitted_catalog, auto store) {
            catalog = admitted_catalog;
            transitions = store;
            runtime = runtime_factory(std::move(admitted_catalog), std::move(store));
            return runtime;
        };
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    auto value = request();
    value["budgets"]["max_worker_retries"] = 2;
    const auto started = service.start({{"request", value}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    const auto source_id = started.at("run_id").get<std::string>();
    ASSERT_EQ(await_terminal(service, source_id).at("status"), "completed");
    const auto source = transitions->load(fixture.resources.owner_scope, source_id);
    ASSERT_TRUE(source);
    ASSERT_TRUE(source->terminal_result());
    const auto source_result = *source->terminal_result();
    ASSERT_TRUE(source_result.provider_budget_authority());
    const auto& source_bank = *source_result.provider_budget_authority();
    EXPECT_EQ(source_bank.charged, 0U);
    EXPECT_EQ(source_bank.reserved, 2U);
    EXPECT_FALSE(source_bank.has_report);
    EXPECT_FALSE(source_bank.reports.total);
    const auto source_lineage = transitions->load_run_lineage(fixture.resources.owner_scope, source_id);
    ASSERT_TRUE(source_lineage);
    EXPECT_EQ(source_lineage->remaining_budget().max_program_operations, 0U);
    const auto version = catalog->resolve_version(fixture.resources.owner_scope, source->program_version_id());
    ASSERT_TRUE(version);
    const auto translated = neograph::mcp::HarnessRequestTranslator::translate(
        value, fixture.resources.snapshots.registry, fixture.config.translation_defaults);
    const auto recorded = [&] {
        return fixture.resources.make_recorded_binding(
            *version, translated.bindings,
            transitions->load_events(fixture.resources.owner_scope, source_id));
    };
    auto invocation = source->invocation();
    invocation.run_id = "recorded-custody-target";
    invocation.correlation_id = "recorded-custody";
    auto widened = invocation;
    ++widened.budget.model_tokens;
    EXPECT_THROW(runtime->replay_recorded(source_id, widened, recorded()),
                 neograph::program::ProgramDiagnosticError);
    EXPECT_EQ(transitions->load_run_lineage(fixture.resources.owner_scope, source_id)->id(),
              source_lineage->id());
    const auto replay = runtime->replay_recorded(source_id, invocation, recorded()).wait();
    ASSERT_EQ(replay.status(), neograph::program::ProgramTerminalStatus::Completed);
    EXPECT_EQ(replay.output(), source_result.output());
    EXPECT_GT(replay.usage().core_steps, 0U)
        << "captured worker calls must execute through real Core, not a cached terminal projection";
    EXPECT_EQ(replay.remaining_budget().max_program_operations, 0U);
    EXPECT_LT(replay.remaining_budget().max_core_steps,
              source_lineage->remaining_budget().max_core_steps);
    EXPECT_EQ(replay.remaining_budget().model_tokens, source_lineage->remaining_budget().model_tokens);
    ASSERT_TRUE(replay.provider_budget_authority());
    EXPECT_EQ(replay.provider_budget_authority()->charged, 0U);
    EXPECT_EQ(replay.provider_budget_authority()->reserved, 2U);
    EXPECT_EQ(replay.provider_budget_authority()->provider_effects, source_bank.provider_effects);
    EXPECT_FALSE(replay.provider_budget_authority()->has_report);
    EXPECT_FALSE(replay.provider_budget_authority()->reports.total);
    EXPECT_EQ(transitions->load_run_lineage(fixture.resources.owner_scope, source_id)->remaining_budget(),
              neograph::program::RunBudget{});
    EXPECT_EQ(service.get(source_id).at("remaining_budget").at("model_tokens"), 0U);
    const auto target_lineage = transitions->load_run_lineage(
        fixture.resources.owner_scope, invocation.run_id);
    ASSERT_TRUE(target_lineage);
    const auto initial = transitions->load_generation_initial_publication(
        fixture.resources.owner_scope, target_lineage->lineage_id(), 1);
    ASSERT_TRUE(initial && initial->recorded_replay_source);
    EXPECT_EQ(initial->recorded_replay_source->run_record_id, source->id());
    EXPECT_EQ(initial->recorded_replay_source->lineage_head_id, source_lineage->id());
    const auto retry = runtime->replay_recorded(source_id, invocation, recorded()).wait();
    EXPECT_EQ(retry.id(), replay.id());
    EXPECT_EQ(transitions->load_run_lineage(fixture.resources.owner_scope, invocation.run_id)->id(),
              target_lineage->id());
    invocation.run_id = "recorded-custody-second-target";
    EXPECT_THROW(runtime->replay_recorded(source_id, invocation, recorded()),
                 neograph::program::ProgramDiagnosticError);
    EXPECT_EQ(provider->calls.load(), 1U);
}

TEST(HarnessProgramCutover, RecordedReplayRetainsLargeHistoricalChargeAndFullCeilingHold) {
    for (const bool unknown_hold : {false, true}) {
        auto effects = std::make_shared<std::atomic<unsigned>>(0);
        HarnessFixture fixture(
            [effects, unknown_hold](const neograph::mcp::HarnessWorkerCall& call, const auto&) {
                ++*effects;  // Deliberately occurs before touching the bank.
                const auto amount = unknown_hold ? call.model_token_budget
                    : call.model_token_budget - call.model_token_budget / 4;
                if (unknown_hold) {
                    if (!call.usage->try_reserve(amount, call.model_token_budget))
                        throw std::runtime_error("cannot retain the source unknown hold");
                } else {
                    call.usage->add(neograph::test::usage(amount, 0, amount));
                }
                call.usage->remember_provider_effect("source-effect");
                return neograph::mcp::HarnessWorkerResponse::success(
                    {{"status", "ok"}, {"findings", json::array({"captured"})}});
            });
        std::shared_ptr<neograph::program::ProgramTransitionStore> transitions;
        const auto make_runtime = fixture.resources.make_program_runtime;
        fixture.resources.make_program_runtime = [&, make_runtime](auto catalog, auto store) {
            transitions = store;
            return make_runtime(std::move(catalog), std::move(store));
        };
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto started = service.start({{"request", request()}});
        ASSERT_TRUE(started.at("started").get<bool>());
        const auto source_id = started.at("run_id").get<std::string>();
        ASSERT_EQ(await_terminal(service, source_id).at("status"), "completed");
        const auto source = transitions->load(fixture.resources.owner_scope, source_id);
        ASSERT_TRUE(source && source->terminal_result());
        const auto source_result = *source->terminal_result();
        ASSERT_TRUE(source_result.provider_budget_authority());
        const auto source_bank = *source_result.provider_budget_authority();
        EXPECT_GT(source_bank.charged + source_bank.reserved, source_result.remaining_budget().model_tokens);
        if (unknown_hold) EXPECT_EQ(source_result.remaining_budget().model_tokens, 0U);
        const auto replay_started = service.start(
            {{"replay", {{"source_run_id", source_id}, {"mode", "recorded"}}}});
        ASSERT_TRUE(replay_started.at("started").get<bool>()) << replay_started.dump();
        const auto replay_id = replay_started.at("run_id").get<std::string>();
        ASSERT_EQ(await_terminal(service, replay_id).at("status"), "completed");
        const auto replay_record = transitions->load(fixture.resources.owner_scope, replay_id);
        ASSERT_TRUE(replay_record && replay_record->terminal_result());
        const auto replay = *replay_record->terminal_result();
        EXPECT_EQ(replay.output(), source_result.output());
        EXPECT_GT(replay.usage().core_steps, 0U);
        EXPECT_EQ(replay.remaining_budget().model_tokens, source_result.remaining_budget().model_tokens);
        EXPECT_EQ(replay_record->invocation().budget, source->invocation().budget);
        ASSERT_TRUE(replay.provider_budget_authority());
        EXPECT_EQ(replay.provider_budget_authority()->charged, source_bank.charged);
        EXPECT_EQ(replay.provider_budget_authority()->reserved, source_bank.reserved);
        EXPECT_EQ(replay.provider_budget_authority()->provider_effects, source_bank.provider_effects);
        EXPECT_EQ(replay.provider_budget_authority()->has_report, source_bank.has_report);
        EXPECT_EQ(neograph::provider_codec::encode_usage(replay.provider_budget_authority()->reports),
                  neograph::provider_codec::encode_usage(source_bank.reports));
        EXPECT_EQ(effects->load(), 1U);
    }
}

TEST(HarnessProgramCutover, LiveReplayThenRecordedReplayUsesSelectedSourceImmutablePermissions) {
    auto effects = std::make_shared<std::atomic<unsigned>>(0);
    HarnessFixture fixture([effects](const neograph::mcp::HarnessWorkerCall& call, const auto&) {
        ++*effects;
        const auto charged = call.model_token_budget / 4;
        call.usage->add(neograph::test::usage(charged, 0, charged));
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"captured"})}});
    });
    auto records = std::make_shared<MutableHarnessRecordStore>();
    fixture.config.record_store = records;
    std::shared_ptr<neograph::program::ProgramTransitionStore> transitions;
    const auto make_runtime = fixture.resources.make_program_runtime;
    fixture.resources.make_program_runtime = [&, make_runtime](auto catalog, auto store) {
        transitions = store;
        return make_runtime(std::move(catalog), std::move(store));
    };
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
    const auto value = request();
    const auto compiled = service.compile(value);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    const auto stored = records->load_artifact(compiled.at("artifact_id").get<std::string>());
    ASSERT_TRUE(stored);
    const auto original_artifact = neograph::mcp::HarnessProgramArtifactRecord::parse(*stored);
    const auto translated = neograph::mcp::HarnessRequestTranslator::translate(
        value, fixture.resources.snapshots.registry, fixture.config.translation_defaults);
    auto document = translated.source.document();
    document["program_schema_version"] = neograph::program::PROGRAM_SCHEMA_VERSION_V2;
    auto invocation_template = translated.invocation_template;
    // Two real one-operation live runs share this nonrenewable grant. Unlike
    // the schema-v1 preset, the host-authored v2 artifact admits its remainder.
    invocation_template.budget.max_program_operations = 2;
    for (auto requirement : document["declared_budget_requirements"]) {
        const auto resource = requirement.at("resource").get<std::string>();
        if (resource == "max_program_operations") {
            requirement["minimum"] = 1;
            requirement["maximum"] = 2;
        } else if (resource == "wall_time_ms" || resource == "max_core_steps") {
            requirement["minimum"] = 1;
        } else if (resource == "model_tokens" || resource == "monetary_microunits") {
            requirement["minimum"] = 0;
        }
    }
    const auto bundle = fixture.resources.compiler->compile(
        neograph::program::ProgramSource::from_cpp_builder(
            "harness:two-live-workloads", neograph::program::PROGRAM_SCHEMA_VERSION_V2,
            std::move(document)));
    const std::string artifact_id = "harness:two-live-workloads";
    auto projection = original_artifact.projection();
    projection["core"] = bundle.serialize_canonical();
    projection["source_id"] = "harness:two-live-workloads";
    auto store = std::make_shared<neograph::mcp::HarnessBoundedProgramStore>(
        records, artifact_id, fixture.resources.owner_scope, invocation_template,
        std::move(projection));
    auto catalog = fixture.resources.make_program_catalog(store, translated.bindings);
    (void)catalog->admit(bundle, {fixture.resources.owner_scope,
                                  fixture.resources.snapshots.admission_profile,
                                  fixture.resources.snapshots.policy, {}});
    const auto first_started = service.start({{"artifact_id", artifact_id}});
    ASSERT_TRUE(first_started.at("started").get<bool>());
    const auto first_id = first_started.at("run_id").get<std::string>();
    ASSERT_EQ(await_terminal(service, first_id).at("status"), "completed");
    const auto first = transitions->load(fixture.resources.owner_scope, first_id);
    ASSERT_TRUE(first && first->terminal_result());
    ASSERT_EQ(first->terminal_result()->usage().program_operations, 1U);
    const auto first_lineage = transitions->load_run_lineage(fixture.resources.owner_scope, first_id);
    ASSERT_TRUE(first_lineage);
    ASSERT_EQ(first_lineage->remaining_budget().max_program_operations, 1U);
    const auto live_started = service.start(
        {{"replay", {{"source_run_id", first_id}, {"mode", "live"}}}});
    ASSERT_TRUE(live_started.at("started").get<bool>()) << live_started.dump();
    const auto live_id = live_started.at("run_id").get<std::string>();
    ASSERT_EQ(await_terminal(service, live_id).at("status"), "completed");
    const auto live = transitions->load(fixture.resources.owner_scope, live_id);
    ASSERT_TRUE(live && live->terminal_result());
    EXPECT_EQ(live->invocation().budget, first_lineage->remaining_budget());
    EXPECT_EQ(live->terminal_result()->usage().program_operations, 1U);
    EXPECT_EQ(live->terminal_result()->remaining_budget().max_program_operations, 0U);
    const auto live_lineage = transitions->load_run_lineage(fixture.resources.owner_scope, live_id);
    ASSERT_TRUE(live_lineage);
    EXPECT_THROW(service.start(
        {{"replay", {{"source_run_id", live_id}, {"mode", "live"}}}}),
        neograph::program::ProgramDiagnosticError);
    EXPECT_EQ(effects->load(), 2U);
    EXPECT_EQ(transitions->load_run_lineage(fixture.resources.owner_scope, live_id)->id(),
              live_lineage->id());
    EXPECT_LT(live->invocation().budget.model_tokens, first->invocation().budget.model_tokens);
    const auto lineage_before = transitions->load_run_lineage(fixture.resources.owner_scope, live_id);
    ASSERT_TRUE(lineage_before);
    const auto recorded_started = service.start(
        {{"replay", {{"source_run_id", live_id}, {"mode", "recorded"}}}});
    ASSERT_TRUE(recorded_started.at("started").get<bool>()) << recorded_started.dump();
    const auto recorded_id = recorded_started.at("run_id").get<std::string>();
    ASSERT_EQ(await_terminal(service, recorded_id).at("status"), "completed");
    const auto recorded = transitions->load(fixture.resources.owner_scope, recorded_id);
    ASSERT_TRUE(recorded && recorded->terminal_result());
    EXPECT_EQ(recorded->invocation().input, live->invocation().input);
    EXPECT_EQ(recorded->invocation().budget, live->invocation().budget);
    EXPECT_EQ(recorded->terminal_result()->output(), live->terminal_result()->output());
    EXPECT_EQ(recorded->terminal_result()->remaining_budget().model_tokens,
              lineage_before->remaining_budget().model_tokens);
    EXPECT_GT(recorded->terminal_result()->usage().core_steps, 0U);
    EXPECT_EQ(recorded->terminal_result()->usage().program_operations, 0U);
    EXPECT_EQ(recorded->terminal_result()->remaining_budget().max_program_operations, 0U);
    EXPECT_LT(recorded->terminal_result()->remaining_budget().max_core_steps,
              lineage_before->remaining_budget().max_core_steps);
    const auto target_lineage = transitions->load_run_lineage(
        fixture.resources.owner_scope, recorded_id);
    ASSERT_TRUE(target_lineage);
    const auto allocation = transitions->load_generation_initial_publication(
        fixture.resources.owner_scope, target_lineage->lineage_id(), 1);
    ASSERT_TRUE(allocation && allocation->recorded_replay_source);
    EXPECT_EQ(allocation->recorded_replay_source->run_record_id, live->id());
    EXPECT_EQ(allocation->recorded_replay_source->lineage_head_id, lineage_before->id());
    EXPECT_EQ(effects->load(), 2U);
    EXPECT_EQ(transitions->load_run_lineage(fixture.resources.owner_scope, live_id)->remaining_budget(),
              neograph::program::RunBudget{});
    EXPECT_THROW(service.start(
        {{"replay", {{"source_run_id", live_id}, {"mode", "recorded"}}}}),
        neograph::program::ProgramDiagnosticError);
    EXPECT_EQ(effects->load(), 2U);
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
    std::atomic<int>               worker_calls{0};
    HarnessFixture                 fixture([&](const neograph::mcp::HarnessWorkerCall&,
                               const std::shared_ptr<neograph::graph::CancelToken>& cancel) {
        if (worker_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
            return neograph::mcp::HarnessWorkerResponse::input_required(
                {{"call_id", "mcp-input-1"},
                 {"result_schema", {{"type", "object"}}},
                 {"payload", {{"question", "approve?"}}}});
        }
        while (!cancel->is_cancelled())
            std::this_thread::sleep_for(1ms);
        return neograph::mcp::HarnessWorkerResponse::cancelled("resumed worker cancelled");
    });
    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {{"name", "harness-test"}, {"version", "1"}};
    std::mutex               response_mutex;
    std::condition_variable  response_cv;
    std::map<int, json>      responses;
    neograph::mcp::MCPServer server(server_config);
    server.set_response_sink([&](const json& response) {
        if (!response.is_object() || !response.contains("id") ||
            !response.at("id").is_number_integer())
            return;
        {
            std::lock_guard lock(response_mutex);
            responses[response.at("id").get<int>()] = response;
        }
        response_cv.notify_all();
    });
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
    const auto listed = server.handle_message(
        {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", json::object()}});
    ASSERT_EQ(listed.at("result").at("tools").size(), 6);

    const auto call_tool = [&server, &response_mutex, &response_cv, &responses](
                               int id, std::string name, json arguments) {
        auto immediate = server.handle_message(
            {{"jsonrpc", "2.0"},
             {"id", id},
             {"method", "tools/call"},
             {"params", {{"name", std::move(name)}, {"arguments", std::move(arguments)}}}});
        if (!immediate.is_null()) return immediate;
        std::unique_lock lock(response_mutex);
        if (!response_cv.wait_for(lock, 2s, [&] { return responses.contains(id); }))
            throw std::runtime_error("MCP response timeout");
        auto response = responses.at(id);
        responses.erase(id);
        return response;
    };
    const auto schema = call_tool(3, "neograph_schema", json::object());
    ASSERT_TRUE(schema.contains("result")) << schema.dump();
    ASSERT_TRUE(schema.at("result").at("structuredContent").contains("request_schema"));
    EXPECT_EQ(schema.at("result").at("structuredContent").at("output_schema"),
              neograph::mcp::harness_program_output_schema());

    const auto compiled = call_tool(4, "neograph_compile", request());
    ASSERT_TRUE(compiled.contains("result")) << compiled.dump();
    const auto compiled_value = compiled.at("result").at("structuredContent");
    ASSERT_TRUE(compiled_value.at("ok").get<bool>()) << compiled.dump();

    const auto started =
        call_tool(5, "neograph_start",
                  {{"artifact_id", compiled_value.at("artifact_id").get<std::string>()}});
    ASSERT_TRUE(started.contains("result")) << started.dump();
    const auto started_value = started.at("result").at("structuredContent");
    ASSERT_TRUE(started_value.at("started").get<bool>()) << started.dump();
    const auto run_id = started_value.at("run_id").get<std::string>();

    json paused = json::object();
    for (int poll = 0; poll != 200; ++poll) {
        paused = call_tool(6 + poll, "neograph_get", {{"run_id", run_id}});
        ASSERT_TRUE(paused.contains("result")) << paused.dump();
        if (paused.at("result").at("structuredContent").at("status") == "input_required") break;
        std::this_thread::sleep_for(5ms);
    }
    const auto paused_value = paused.at("result").at("structuredContent");
    ASSERT_EQ(paused_value.at("status"), "input_required") << paused.dump();
    ASSERT_EQ(paused_value.at("pending").at("call_id"), "mcp-input-1");

    const auto resumed = call_tool(
        207, "neograph_resume",
        {{"run_id", run_id}, {"call_id", "mcp-input-1"}, {"result", {{"approved", true}}}});
    ASSERT_TRUE(resumed.contains("result")) << resumed.dump();
    EXPECT_TRUE(resumed.at("result").at("structuredContent").at("accepted").get<bool>());

    const auto dispatch_deadline = std::chrono::steady_clock::now() + 1s;
    while (worker_calls.load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < dispatch_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(worker_calls.load(std::memory_order_acquire), 2);

    const auto cancelled = call_tool(208, "neograph_cancel", {{"run_id", run_id}});
    ASSERT_TRUE(cancelled.contains("result")) << cancelled.dump();
    const auto cancelled_value = cancelled.at("result").at("structuredContent");
    ASSERT_TRUE(cancelled_value.at("cancelled").get<bool>()) << cancelled.dump();

    json terminal = json::object();
    for (int poll = 0; poll != 200; ++poll) {
        terminal = call_tool(209 + poll, "neograph_get", {{"run_id", run_id}});
        ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
        if (terminal.at("result").at("structuredContent").at("status") == "cancelled") break;
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(terminal.at("result").at("structuredContent").at("status"), "cancelled")
        << terminal.dump();
}

TEST(HarnessProgramCutover, HostToolConfigurationChangesBindingAndArtifactIdentity) {
    HarnessFixture fixture;
    const auto     root = std::filesystem::temp_directory_path() /
                      ("neograph-harness-binding-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    auto records = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    fixture.config.record_store = records;
    neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);

    const auto configured_request = [](std::string server_ref) {
        auto value                   = request();
        value["workers"][0]["tools"] = json::array({"harness.lookup"});
        value["tool_catalog"]        = json::array(
            {{{"id", "harness.lookup"},
                     {"description", "Look up a value"},
                     {"input_schema", {{"type", "object"}, {"additionalProperties", true}}},
                     {"output_schema", {{"type", "object"}, {"additionalProperties", true}}},
                     {"read_only", true},
                     {"executor",
                      {{"kind", "mcp"}, {"server_ref", std::move(server_ref)}, {"tool", "lookup"}}}}});
        return value;
    };
    const auto first  = service.compile(configured_request("server-a"));
    const auto second = service.compile(configured_request("server-b"));
    ASSERT_TRUE(first.at("ok").get<bool>()) << first.dump();
    ASSERT_TRUE(second.at("ok").get<bool>()) << second.dump();
    EXPECT_EQ(first.at("bundle_id"), second.at("bundle_id"));
    EXPECT_NE(first.at("artifact_id"), second.at("artifact_id"));
    EXPECT_NE(first.at("program_version_id"), second.at("program_version_id"));

    const auto first_stored  = records->load_artifact(first.at("artifact_id").get<std::string>());
    const auto second_stored = records->load_artifact(second.at("artifact_id").get<std::string>());
    ASSERT_TRUE(first_stored.has_value());
    ASSERT_TRUE(second_stored.has_value());
    const auto first_record  = neograph::mcp::HarnessProgramArtifactRecord::parse(*first_stored);
    const auto second_record = neograph::mcp::HarnessProgramArtifactRecord::parse(*second_stored);
    EXPECT_NE(neograph::program::capability_binding_receipt_root(
                  first_record.version().core_materialization_receipt().capability_bindings),
              neograph::program::capability_binding_receipt_root(
                  second_record.version().core_materialization_receipt().capability_bindings));
    std::error_code cleanup_error;
#ifdef _WIN32
    std::filesystem::remove_all(std::filesystem::path(LR"(\\?\)" + root.native()),
                                cleanup_error);
#else
    std::filesystem::remove_all(root, cleanup_error);
#endif
}

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
TEST(HarnessProgramCutover, FrozenContractSurvivesSqliteServiceRestart) {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-contract-restart-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto contract_spec         = neograph::program::ContractManifestSpec{};
    contract_spec.manifest_id  = "harness-restart-contract";
    contract_spec.owner_scope  = "harness-cutover-test";
    contract_spec.scope        = "harness-execution";
    contract_spec.assumptions  = {"ProgramRuntime is the executor"};
    contract_spec.requirements = {{"execute", "Execute the retained Harness Program"}};
    contract_spec.non_goals    = {"Provider implementation selection"};
    contract_spec.acceptance   = {
        {"runtime", "ProgramRuntime returns a value", true, json::object()}};
    contract_spec.fixed_test_vectors  = {{"smoke", json::object(), json{{"outcome", "ok"}}}};
    contract_spec.independent_oracles = {"runtime-oracle"};
    contract_spec.risk_register = {{"executor", "Executor may drift", "Pin ProgramRuntime", true}};
    contract_spec.retry_policy  = {1, 100, 5000};
    const auto manifest = neograph::program::ContractManifest::propose(std::move(contract_spec))
                              .review({"reviewer", "approved", true})
                              .freeze();
    auto contract_request                  = request();
    contract_request["contract"]           = json::parse(manifest.serialize_canonical());
    contract_request["workspace_revision"] = "workspace-restart-1";

    std::string run_id;
    std::string artifact_id;
    json        before;
    json        frozen_artifact;
    {
        HarnessFixture fixture;
        fixture.config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto                    compiled = service.compile(contract_request);
        ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
        artifact_id        = compiled.at("artifact_id").get<std::string>();
        frozen_artifact    = service.read("neograph://artifacts/" + artifact_id + "/contract");
        const auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
        ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
        run_id = started.at("run_id").get<std::string>();
        before = await_terminal(service, run_id);
        ASSERT_EQ(before.at("status"), "completed") << before.dump();
        ASSERT_EQ(before.at("contract").at("status"), "published") << before.dump();
        EXPECT_EQ(before.at("contract").at("manifest_hash"), manifest.content_hash());
        bool saw_runtime_evidence = false;
        for (const auto& evidence : before.at("contract").at("evidence")) {
            if (evidence.at("kind") != "deterministic_run") continue;
            saw_runtime_evidence = true;
            EXPECT_EQ(evidence.at("run_id"), run_id);
            EXPECT_EQ(evidence.at("program_version_id"), before.at("program_version_id"));
            EXPECT_EQ(evidence.at("artifact_hash"), before.at("bundle_id"));
        }
        EXPECT_TRUE(saw_runtime_evidence);
    }

    {
        HarnessFixture fixture;
        fixture.config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        EXPECT_EQ(canonical_json(service.read("neograph://artifacts/" + artifact_id + "/contract")),
                  canonical_json(frozen_artifact));
        const auto recovered = service.get(run_id, "details");
        EXPECT_EQ(recovered.at("status"), before.at("status"));
        EXPECT_EQ(recovered.at("program_version_id"), before.at("program_version_id"));
        EXPECT_EQ(recovered.at("bundle_id"), before.at("bundle_id"));
        EXPECT_EQ(canonical_json(recovered.at("contract")), canonical_json(before.at("contract")));
        EXPECT_EQ(canonical_json(recovered.at("result")), canonical_json(before.at("result")));
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}

TEST(HarnessProgramCutover, TamperedPersistedContractStateFailsClosed) {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-contract-tamper-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    neograph::program::ContractManifestSpec contract_spec;
    contract_spec.manifest_id  = "harness-tamper-contract";
    contract_spec.owner_scope  = "harness-cutover-test";
    contract_spec.scope        = "harness-execution";
    contract_spec.assumptions  = {"ProgramRuntime is the executor"};
    contract_spec.requirements = {{"execute", "Execute the retained Harness Program"}};
    contract_spec.non_goals    = {"Provider implementation selection"};
    contract_spec.acceptance   = {
        {"runtime", "ProgramRuntime returns a value", true, json::object()}};
    contract_spec.fixed_test_vectors  = {{"smoke", json::object(), json{{"outcome", "ok"}}}};
    contract_spec.independent_oracles = {"runtime-oracle"};
    contract_spec.risk_register = {{"executor", "Executor may drift", "Pin ProgramRuntime", true}};
    contract_spec.retry_policy  = {1, 100, 5000};
    const auto manifest = neograph::program::ContractManifest::propose(std::move(contract_spec))
                              .review({"reviewer", "approved", true})
                              .freeze();
    auto contract_request        = request();
    contract_request["contract"] = json::parse(manifest.serialize_canonical());

    std::string run_id;
    {
        HarnessFixture fixture;
        fixture.config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto                    compiled = service.compile(contract_request);
        ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
        const auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
        ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
        run_id = started.at("run_id").get<std::string>();
        ASSERT_EQ(await_terminal(service, run_id).at("status"), "completed");
    }
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &database), SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(database, "UPDATE neograph_harness_contract_runs SET bundle_id='tampered'",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    sqlite3_close(database);

    bool rejected = false;
    try {
        HarnessFixture fixture;
        fixture.config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        (void)service.get(run_id);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    std::error_code error;
    std::filesystem::remove(path, error);
}

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
    fixture.config.record_store.reset();
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
            {{"run_id", run_id}, {"call_id", "restart-input-1"}, {"result", {{"approved", true}}}});
        EXPECT_TRUE(accepted.at("accepted").get<bool>());
        const auto completed = await_terminal(service, run_id);
        EXPECT_EQ(completed.at("status"), "completed") << completed.dump();
    }
    EXPECT_EQ(fixture.calls.load(), 2);
    fixture.config.record_store.reset();
    std::filesystem::remove(path);
}

TEST(HarnessProgramCutover, ReconnectsSqlitePendingEffectWithCheckpointAndJournal) {
    HarnessFixture* fixture_ptr = nullptr;
    HarnessFixture  fixture([&](const neograph::mcp::HarnessWorkerCall& call,
                               const std::shared_ptr<neograph::graph::CancelToken>&) {
        ++fixture_ptr->calls;
        if (!call.resume_value) {
            return neograph::mcp::HarnessWorkerResponse::awaiting_tool_results(
                {{"call_id", "restart-effect-call"},
                 {"result_schema",
                  {{"type", "object"},
                   {"required", json::array({"tool_results"})},
                   {"properties",
                    {{"tool_results", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}}},
                 {"effect",
                  {{"effect_id", "restart-effect-1"},
                   {"idempotency", "supported"},
                   {"tool_calls", json::array({{{"name", "search"},
                                                {"arguments", {{"query", "NeoGraph"}}}}})}}}});
        }
        EXPECT_EQ(call.resume_value->at("tool_results").size(), 1U);
        return neograph::mcp::HarnessWorkerResponse::success(
            {{"status", "ok"}, {"findings", json::array({"reconciled"})}});
    });
    fixture_ptr = &fixture;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("neograph-harness-effect-reconnect-" + neograph::graph::Checkpoint::generate_id() + ".db");
    fixture.config.record_store =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());

    std::string run_id;
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto                    started = service.start({{"request", request()}});
        run_id                                = started.at("run_id").get<std::string>();
        const auto paused                     = await_terminal(service, run_id);
        ASSERT_EQ(paused.at("status"), "awaiting_tool_results") << paused.dump();
        ASSERT_EQ(paused.at("pending").at("call_id"), "restart-effect-call");
        ASSERT_EQ(paused.at("pending").at("effect_id"), "restart-effect-1");
        ASSERT_TRUE(paused.at("checkpoint").at("checkpoint_id").is_string());
    }

    fixture.config.record_store =
        std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(path.string());
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto                    reconnected = service.get(run_id);
        ASSERT_EQ(reconnected.at("status"), "awaiting_tool_results") << reconnected.dump();
        ASSERT_EQ(reconnected.at("pending").at("call_id"), "restart-effect-call");
        ASSERT_EQ(reconnected.at("pending").at("effect_id"), "restart-effect-1");
        ASSERT_TRUE(reconnected.at("checkpoint").at("checkpoint_id").is_string());
        const auto accepted = service.resume(
            {{"run_id", run_id},
             {"call_id", "restart-effect-call"},
             {"result",
              {{"tool_results",
                json::array({{{"name", "search"}, {"result", {{"matches", 3}}}}})}}}});
        EXPECT_TRUE(accepted.at("accepted").get<bool>());
        EXPECT_EQ(await_terminal(service, run_id).at("status"), "completed");
    }
    EXPECT_EQ(fixture.calls.load(), 2);
    fixture.config.record_store.reset();
    std::filesystem::remove(path);
}

TEST(HarnessProgramCutover, ConcurrentSqliteReconnectResumeHasOneWinnerAndOneDispatch) {
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
    {
        neograph::mcp::HarnessService first(fixture.config, nullptr, fixture.resources);
        neograph::mcp::HarnessService second(second_config, nullptr, fixture.resources);

        const auto started = first.start({{"request", request()}});
        const auto run_id  = started.at("run_id").get<std::string>();
        ASSERT_EQ(await_terminal(first, run_id).at("status"), "input_required");
        ASSERT_EQ(second.get(run_id).at("status"), "input_required");

        std::promise<void> ready;
        auto               gate        = ready.get_future().share();
        const auto         resume_once = [&](neograph::mcp::HarnessService& service, bool approved) {
            gate.wait();
            try {
                const auto accepted = service.resume({{"run_id", run_id},
                                                      {"call_id", "resume-race-1"},
                                                      {"result", {{"approved", approved}}}});
                return accepted.at("accepted").get<bool>() ? 1 : -1;
            } catch (const neograph::program::ProgramDiagnosticError& error) {
                const auto& code = error.diagnostic().code;
                return code == "P_RESUME_CONFLICT" || code == "P_RESUME_STATE" ? 0 : -2;
            }
        };
        auto first_resume  = std::async(std::launch::async, [&] { return resume_once(first, true); });
        auto second_resume = std::async(std::launch::async, [&] { return resume_once(second, false); });
        ready.set_value();
        const int first_result  = first_resume.get();
        const int second_result = second_resume.get();
        EXPECT_EQ(first_result + second_result, 1);
        ASSERT_TRUE(first_result == 1 || second_result == 1);
        auto&      winner    = first_result == 1 ? first : second;
        const auto completed = await_terminal(winner, run_id);
        EXPECT_EQ(completed.at("status"), "completed") << completed.dump();
        EXPECT_EQ(fixture.calls.load(), 2);
    }
    second_config.record_store.reset();
    fixture.config.record_store.reset();
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
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);

        const auto source_artifact = service.compile(request());
        ASSERT_TRUE(source_artifact.at("ok").get<bool>());
        auto target_request                          = request();
        // This test verifies exact cross-artifact fork state, not timeout
        // behavior. Leave enough wall time for loaded Debug/parallel CTest runs.
        target_request["budgets"]["timeout_seconds"] = 10;
        const auto target_artifact                   = service.compile(target_request);
        ASSERT_TRUE(target_artifact.at("ok").get<bool>()) << target_artifact.dump();
        ASSERT_NE(source_artifact.at("artifact_id"), target_artifact.at("artifact_id"));

        const auto source_started = service.start({{"artifact_id", source_artifact.at("artifact_id")}});
        const auto source_run_id  = source_started.at("run_id").get<std::string>();
        const auto source_paused  = await_terminal(service, source_run_id);
        ASSERT_EQ(source_paused.at("status"), "input_required") << source_paused.dump();
        const auto checkpoint_id =
            source_paused.at("checkpoint").at("checkpoint_id").get<std::string>();

        const auto fork_started = service.start({{"fork",
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

        const auto forked = await_terminal(service, fork_started.at("run_id").get<std::string>());
        EXPECT_EQ(forked.at("status"), "completed") << forked.dump();
        EXPECT_EQ(forked.at("result").at("outcome"), "ok");
        EXPECT_EQ(fixture.calls.load(), 2);
    }
    fixture.config.record_store.reset();
    std::filesystem::remove(path);
}

#endif

}  // namespace

TEST(HarnessProgramCutover, MissingPartialAndInconsistentUsageCannotRenewProviderBudget) {
    auto inconsistent = neograph::test::usage(0, 0, 0);
    inconsistent.quality = sp::UsageQuality::Inconsistent;
    inconsistent.conflicts.push_back({"total", "conflicting terminal report"});
    const std::vector<sp::Usage> reports{
        sp::Usage{},
        neograph::test::usage(1, 0, 1, sp::UsageStage::Partial),
        neograph::test::usage(std::nullopt, 0, std::nullopt),
        inconsistent};
    for (const auto& report : reports) {
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        const auto result = neograph::test::success("{}", report);
        auto provider = std::make_shared<neograph::test::LocalProvider>(
            [calls, result](auto, const auto&, const auto&)
                -> asio::awaitable<sp::runtime::Result> {
                ++*calls;
                co_return result;
            }, "unknown-usage-provider", neograph::test::bounded_client());
        neograph::mcp::HarnessProviderExecutorConfig config{.model = "test-model"};
        config.provider = provider;
        auto executor = neograph::mcp::make_provider_harness_executor(std::move(config));
        neograph::mcp::HarnessWorkerCall call;
        call.task = {{"objective", "preserve nonrenewable authority"}};
        call.worker = {{"worker_id", "usage-worker"}, {"instructions", "return JSON"},
                       {"output_schema", {{"type", "object"}}},
                       {"_harness_provider_budget",
                        {{"max_output_tokens", 1}, {"input_token_ceiling", 1}}}};
        call.usage = std::make_shared<neograph::UsageAccumulator>();
        call.model_token_budget = 2;
        call.budget_exhausted = std::make_shared<std::atomic_bool>(false);
        auto cancel = std::make_shared<neograph::graph::CancelToken>();
        EXPECT_EQ(executor(call, cancel).kind, neograph::mcp::HarnessWorkerResponseKind::VALUE);
        EXPECT_EQ(call.usage->total_tokens_wide(), 2);
        const auto observed = call.usage->snapshot();
        EXPECT_EQ(observed.input_total.has_value(), report.input_total.has_value());
        EXPECT_EQ(observed.output_total.has_value(), report.output_total.has_value());
        EXPECT_EQ(observed.total.has_value(), report.total.has_value());
        if (report.total && observed.total)
            EXPECT_EQ(observed.total->value, report.total->value);
        EXPECT_EQ(executor(call, cancel).kind, neograph::mcp::HarnessWorkerResponseKind::CANCELLED);
        EXPECT_EQ(calls->load(), 1U);
        EXPECT_EQ(call.usage->total_tokens_wide(), 2);
    }
}

TEST(HarnessProgramCutover, TypedFailurePartialUsageRetainsReservationWithoutRedispatch) {
    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::Transport;
    failure.error.retry_safety = sp::RetrySafety::OutputObserved;
    failure.error.attempt.request_may_have_left = true;
    failure.partial.messages = {neograph::test::message("partial answer")};
    failure.partial.usage = neograph::test::usage(std::nullopt, 0, std::nullopt,
                                                sp::UsageStage::Partial);
    const auto result = std::make_shared<const sp::Outcome>(std::move(failure));
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    neograph::mcp::HarnessProviderExecutorConfig config{.model = "test-model"};
    config.provider = std::make_shared<neograph::test::LocalProvider>(
        [calls, result](auto, const auto&, const auto&) -> asio::awaitable<sp::runtime::Result> {
            ++*calls;
            co_return result;
        }, "partial-failure-provider", neograph::test::bounded_client());
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(config));
    neograph::mcp::HarnessWorkerCall call;
    call.task = {{"objective", "uncertain partial delivery"}};
    call.worker = {{"worker_id", "partial-worker"}, {"instructions", "return JSON"},
                   {"output_schema", {{"type", "object"}}},
                   {"_harness_provider_budget",
                    {{"max_output_tokens", 1}, {"input_token_ceiling", 1}}}};
    call.usage = std::make_shared<neograph::UsageAccumulator>();
    call.model_token_budget = 2;
    call.budget_exhausted = std::make_shared<std::atomic_bool>(false);
    auto cancel = std::make_shared<neograph::graph::CancelToken>();
    EXPECT_EQ(executor(call, cancel).kind, neograph::mcp::HarnessWorkerResponseKind::TOOL_ERROR);
    EXPECT_EQ(call.usage->total_tokens_wide(), 2);
    const auto observed = call.usage->snapshot();
    EXPECT_FALSE(observed.input_total);
    ASSERT_TRUE(observed.output_total);
    EXPECT_EQ(observed.output_total->value, 0U);
    EXPECT_FALSE(observed.total);
    EXPECT_EQ(executor(call, cancel).kind, neograph::mcp::HarnessWorkerResponseKind::CANCELLED);
    EXPECT_EQ(calls->load(), 1U);
    EXPECT_EQ(call.usage->total_tokens_wide(), 2);
}

TEST(HarnessProgramCutover, UncertainProviderFailureNeverConsumesAvailableRepairRetries) {
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
    const auto directory = std::filesystem::temp_directory_path() /
        ("neograph-uncertain-budget-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct DatabaseCleanup {
        std::filesystem::path path;
        ~DatabaseCleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    std::filesystem::create_directory(directory);
#endif
    auto provider = std::make_shared<ThrowOnceProvider>();
    neograph::mcp::HarnessProviderExecutorConfig provider_config{.model = "test-model"};
    provider_config.provider = provider;
    provider_config.max_tool_rounds = 1;
    auto provider_executor =
        neograph::mcp::make_provider_harness_executor(std::move(provider_config));
    struct Observation {
        std::mutex mutex;
        std::shared_ptr<neograph::UsageAccumulator> usage;
        std::uint64_t budget = 0;
    };
    auto observation = std::make_shared<Observation>();
    HarnessFixture fixture(
        [provider_executor = std::move(provider_executor), observation](
            const neograph::mcp::HarnessWorkerCall& call,
            const std::shared_ptr<neograph::graph::CancelToken>& cancel) {
            {
                std::lock_guard lock(observation->mutex);
                observation->usage = call.usage;
                observation->budget = call.model_token_budget;
            }
            return provider_executor(call, cancel);
        });
    fixture.config.translation_defaults.max_output_tokens = 1;
    fixture.config.translation_defaults.input_token_ceiling_per_round = 1;
    fixture.config.translation_defaults.max_provider_tool_rounds = 1;
    auto value = request();
    value["budgets"]["max_worker_retries"] = 2;
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
    fixture.config.record_store = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
        (directory / "records.sqlite").string());
#endif
    json terminal;
    std::string run_id;
    {
        neograph::mcp::HarnessService service(fixture.config, nullptr, fixture.resources);
        const auto compiled = service.compile(value);
        ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
        const auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
        ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
        run_id = started.at("run_id").get<std::string>();
        terminal = await_terminal(service, run_id);
    }
    ASSERT_EQ(terminal.at("status"), "completed") << terminal.dump();
    EXPECT_EQ(terminal.at("result").at("outcome"), "failed");
    EXPECT_EQ(terminal.at("result").at("valid_workers"), 0);
    EXPECT_EQ(terminal.at("result").at("failed_workers"), 1);
    EXPECT_EQ(provider->calls.load(), 1U);
    {
        std::lock_guard lock(observation->mutex);
        ASSERT_TRUE(observation->usage);
        EXPECT_GT(observation->budget, 2U)
            << "available retry budget must not authorize uncertain redispatch";
        EXPECT_EQ(observation->usage->total_tokens_wide(), 2);
        EXPECT_EQ(terminal.at("remaining_budget").at("model_tokens"), observation->budget - 2);
    }
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
    fixture.config.record_store.reset();
    fixture.config.record_store = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
        (directory / "records.sqlite").string());
    neograph::mcp::HarnessService reopened(fixture.config, nullptr, fixture.resources);
    const auto reconnected = reopened.get(run_id);
    EXPECT_EQ(reconnected.at("result"), terminal.at("result"));
    EXPECT_EQ(reconnected.at("remaining_budget"), terminal.at("remaining_budget"));
    const auto adapter = neograph::mcp::require_harness_program_adapter_store(fixture.config.record_store);
    const auto stored_source = adapter->resolve_program_run(fixture.resources.owner_scope, run_id);
    ASSERT_TRUE(stored_source && stored_source->run_record().terminal_result());
    const auto source_terminal = *stored_source->run_record().terminal_result();
    ASSERT_TRUE(source_terminal.provider_budget_authority());
    EXPECT_EQ(source_terminal.provider_budget_authority()->charged, 0U);
    EXPECT_EQ(source_terminal.provider_budget_authority()->reserved, 2U);
    EXPECT_FALSE(source_terminal.provider_budget_authority()->has_report);
    EXPECT_FALSE(source_terminal.provider_budget_authority()->reports.total);
    const auto replay = reopened.start(
        {{"replay", {{"source_run_id", run_id}, {"mode", "recorded"}}}});
    ASSERT_TRUE(replay.at("started").get<bool>()) << replay.dump();
    const auto replayed = await_terminal(reopened, replay.at("run_id").get<std::string>());
    EXPECT_EQ(replayed.at("result"), terminal.at("result"));
    EXPECT_LE(replayed.at("remaining_budget").at("model_tokens").get<std::uint64_t>(),
              terminal.at("remaining_budget").at("model_tokens").get<std::uint64_t>());
    EXPECT_EQ(provider->calls.load(), 1U)
        << "persisted uncertainty must survive recorded replay without provider repair dispatch";
    const auto stored_replay = adapter->resolve_program_run(
        fixture.resources.owner_scope, replay.at("run_id").get<std::string>());
    ASSERT_TRUE(stored_replay && stored_replay->run_record().terminal_result());
    const auto replay_terminal = *stored_replay->run_record().terminal_result();
    ASSERT_TRUE(replay_terminal.provider_budget_authority());
    EXPECT_EQ(replay_terminal.provider_budget_authority()->charged, 0U);
    EXPECT_EQ(replay_terminal.provider_budget_authority()->reserved, 2U);
    EXPECT_EQ(replay_terminal.provider_budget_authority()->provider_effects,
              source_terminal.provider_budget_authority()->provider_effects);
    EXPECT_FALSE(replay_terminal.provider_budget_authority()->has_report);
    EXPECT_FALSE(replay_terminal.provider_budget_authority()->reports.total);
    EXPECT_EQ(reopened.get(run_id).at("remaining_budget").at("model_tokens"), 0U);
    EXPECT_THROW(reopened.start(
        {{"replay", {{"source_run_id", run_id}, {"mode", "recorded"}}}}),
        neograph::program::ProgramDiagnosticError);
#endif
}

TEST(HarnessProgramCutover, InputGrantBelowAdmittedProviderCeilingRejectsBeforeReservationAndDispatch) {
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto provider = std::make_shared<neograph::test::LocalProvider>(
        [calls](auto, const auto&, const auto&) -> asio::awaitable<sp::runtime::Result> {
            ++*calls;
            co_return neograph::test::success("{}", neograph::test::usage(0, 0, 0));
        }, "bounded-input-provider", neograph::test::bounded_client("test-model", 2));
    neograph::mcp::HarnessProviderExecutorConfig config{.model = "test-model"};
    config.provider = provider;
    auto executor = neograph::mcp::make_provider_harness_executor(std::move(config));
    neograph::mcp::HarnessWorkerCall call;
    call.task = {{"objective", "reject understated model input ceiling"}};
    call.worker = {{"worker_id", "grant-worker"}, {"instructions", "return JSON"},
                   {"output_schema", {{"type", "object"}}},
                   {"_harness_provider_budget",
                    {{"max_output_tokens", 1}, {"input_token_ceiling", 1}}}};
    call.usage = std::make_shared<neograph::UsageAccumulator>();
    call.model_token_budget = 2;
    call.budget_exhausted = std::make_shared<std::atomic_bool>(false);
    auto cancel = std::make_shared<neograph::graph::CancelToken>();
    const auto rejected = executor(call, cancel);
    EXPECT_EQ(rejected.kind, neograph::mcp::HarnessWorkerResponseKind::TOOL_ERROR);
    EXPECT_EQ(calls->load(), 0U);
    EXPECT_EQ(call.usage->total_tokens_wide(), 0U);
    EXPECT_FALSE(call.usage->snapshot().total);
    EXPECT_FALSE(cancel->is_cancelled());
}
