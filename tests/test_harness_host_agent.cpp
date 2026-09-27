#include <neograph/mcp/harness_host_agent.h>

#include <gtest/gtest.h>

#ifdef __linux__
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace neograph;
using namespace neograph::mcp;

class TempWorkspace {
public:
    TempWorkspace() {
        auto value = (std::filesystem::temp_directory_path() / "neograph-host-test-XXXXXX").string();
        std::vector<char> pattern(value.begin(), value.end());
        pattern.push_back('\0');
        const auto dir = ::mkdtemp(pattern.data());
        if (!dir) throw std::runtime_error("fixture directory creation failed");
        try { path = dir; }
        catch (...) { ::rmdir(dir); throw; }
    }
    ~TempWorkspace() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};

class Fixture {
public:
    explicit Fixture(std::string body, bool authenticated = true,
                     std::string version = "2.2.0", bool openai_oauth = false) {
        root = temp_.path;
        binary = root / "fake-cli";
        std::ofstream file(binary);
        file << "#!/bin/sh\n"
                "if [ -n \"${OPENAI_API_KEY:-}${NEOGRAPH_HARNESS_API_KEY:-}${ANTHROPIC_AUTH_TOKEN:-}${CODEX_API_KEY:-}${AWS_SECRET_ACCESS_KEY:-}\" ]; then exit 17; fi\n"
                "case \"$1\" in\n"
                "  --version) printf '";
        file << version;
        file << "\\n'; exit 0;;\n";
        file << "  auth) case \"$2\" in status) printf '{\"loggedIn\":";
        file << (authenticated ? "true" : "false");
        file << "}\\n';; *) printf '";
        file << (openai_oauth ? "OpenAI OAuth\\n1" : authenticated ? "1" : "0");
        file << " credentials\\n';; esac; ";
        file << (authenticated ? "exit 0;;\n" : "exit 1;;\n");
        file << "  login) printf '";
        file << (authenticated ? "Logged in" : "Not logged in");
        file << "\\n'; ";
        file << (authenticated ? "exit 0;;\n" : "exit 1;;\n");
        file << "  models) printf 'openai/test-model\\n'; exit 0;;\n"
                "  exec|run) if [ \"$2\" = --help ]; then printf '%s\\n' '--format --model --ignore-user-config --ignore-rules --ephemeral --sandbox --json'; exit 0; fi;;\n"
                "esac\n";
        file << body << '\n';
        file.close();
        ::chmod(binary.c_str(), 0700);
    }
    ~Fixture() = default;
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    HostAgentExecutorConfig config(std::string host) const {
        HostAgentExecutorConfig c;
        c.host = std::move(host);
        c.executable = binary.string();
        c.workspace = root;
        c.request_timeout = std::chrono::seconds(5);
        return c;
    }
    std::filesystem::path root, binary;
private:
    TempWorkspace temp_;
};

class ScopedEnvironment {
public:
    ScopedEnvironment(std::string name, const char* value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) previous_ = previous;
        ::setenv(name_.c_str(), value, 1);
    }
    ~ScopedEnvironment() {
        if (previous_) ::setenv(name_.c_str(), previous_->c_str(), 1);
        else ::unsetenv(name_.c_str());
    }
private:
    std::string name_;
    std::optional<std::string> previous_;
};

HarnessWorkerCall request() {
    HarnessWorkerCall call;
    call.task = {{"objective", "return verdict"}, {"acceptance", json::array({"valid"})}};
    call.worker = {{"instructions", "read only"}, {"output_schema", {{"type", "object"}}},
                   {"_harness_provider_budget", {{"provider_timeout_seconds", 3},
                                                  {"max_output_tokens", 1000},
                                                  {"input_token_ceiling", 10000}}}};
    call.tool_catalog = json::array();
    call.policy = {{"read_only", true}};
    return call;
}

TEST(HarnessHostAgentTest, OpenCodeEmitsOnlyFinalJsonAndHonorsExplicitModel) {
    Fixture cli("case \"$*\" in *'--model openai/test-model'*) ;; *) exit 8;; esac\n"
                "printf '%s\\n' '{\"type\":\"step_start\"}'"
                " '{\"type\":\"text\",\"part\":{\"type\":\"text\",\"text\":\"{\\\"valid\\\":true}\"}}'"
                " '{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":15,\"output\":8}}}'");
    ScopedEnvironment disallowed("OPENAI_API_KEY", "nonsecret-fixture-sentinel");
    auto config = cli.config("opencode");
    config.model = "openai/test-model";
    ASSERT_TRUE(preflight_host_agent(config).available);
    EXPECT_EQ(make_host_agent_executor(config)(request(), std::make_shared<graph::CancelToken>()).value,
              json({{"valid", true}}));
}

TEST(HarnessHostAgentTest, OpenCodeReportsOAuthOnlyFromHostStatus) {
    Fixture oauth("exit 1", true, "2.2.0", true);
    const auto status = preflight_host_agent(oauth.config("opencode"));
    ASSERT_TRUE(status.available);
    EXPECT_TRUE(status.openai_oauth);
    Fixture no_oauth("exit 1");
    EXPECT_FALSE(preflight_host_agent(no_oauth.config("opencode")).openai_oauth);
}

TEST(HarnessHostAgentTest, ClaudeStructuredOutputAndCodexFinalMessage) {
    Fixture claude("printf '%s\\n' '{\"type\":\"result\",\"is_error\":false,\"structured_output\":{\"valid\":true},\"usage\":{\"input_tokens\":12,\"output_tokens\":9}}'");
    auto response = make_host_agent_executor(claude.config("claude"))(request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::VALUE);
    EXPECT_EQ(response.value, json({{"valid", true}}));
    Fixture codex("printf '%s\\n' '{\"type\":\"thread.started\"}'"
                  " '{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"{\\\"valid\\\":true}\"}}'"
                  " '{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":12,\"output_tokens\":9}}'");
    response = make_host_agent_executor(codex.config("codex"))(request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::VALUE);
    EXPECT_EQ(response.value, json({{"valid", true}}));
}

TEST(HarnessHostAgentTest, DistinguishesAuthModelQuotaPolicyAndMalformedStdout) {
    for (const auto& [diagnostic, expected] :
         std::vector<std::pair<std::string, std::string>>{{"authentication failed", "HOST_AUTH"},
                                                         {"quota exceeded", "HOST_QUOTA"},
                                                         {"model not found", "HOST_MODEL"},
                                                         {"policy violation", "HOST_POLICY"}}) {
        Fixture cli("printf '%s\\n' '" + diagnostic + "' >&2; exit 9");
        auto response = make_host_agent_executor(cli.config("codex"))(request(), std::make_shared<graph::CancelToken>());
        EXPECT_EQ(response.kind, HarnessWorkerResponseKind::TOOL_ERROR);
        EXPECT_NE(response.message.find(expected), std::string::npos);
        EXPECT_EQ(response.message.find(diagnostic), std::string::npos);
    }
    Fixture malformed("printf '%s\\n' 'not-json'");
    auto response = make_host_agent_executor(malformed.config("codex"))(request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::PARSE_ERROR);
    Fixture wrong_type("printf '%s\\n' '{\"type\":7}'");
    response = make_host_agent_executor(wrong_type.config("codex"))(
        request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::PARSE_ERROR);
    EXPECT_NE(response.message.find("field types"), std::string::npos);
}

TEST(HarnessHostAgentTest, DistinguishesStartupTimeoutExitSignalAndOutputLimit) {
    Fixture slow("sleep 2");
    auto config = slow.config("codex");
    config.startup_timeout = std::chrono::milliseconds(500);
    config.request_timeout = std::chrono::milliseconds(1800);
    auto response = make_host_agent_executor(config)(request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::TIMEOUT);
    EXPECT_NE(response.message.find("HOST_STARTUP_TIMEOUT"), std::string::npos);

    Fixture signalled("kill -TERM $$");
    response = make_host_agent_executor(signalled.config("codex"))(request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::TOOL_ERROR);
    EXPECT_NE(response.message.find("HOST_SIGNAL"), std::string::npos);

    Fixture oversized("printf '%0200d\\n' 1");
    config = oversized.config("codex");
    config.max_output_bytes = 64;
    response = make_host_agent_executor(config)(request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::TOOL_ERROR);
    EXPECT_NE(response.message.find("HOST_OUTPUT_LIMIT"), std::string::npos);

    // A blocking CLI write larger than the kernel pipe must drain completely:
    // malformed content is a parse error, not an EAGAIN process failure.
    Fixture large("dd if=/dev/zero bs=131072 count=1 2>/dev/null | tr '\\000' '0'");
    config = large.config("codex");
    config.max_output_bytes = 200000;
    response = make_host_agent_executor(config)(request(), std::make_shared<graph::CancelToken>());
    EXPECT_EQ(response.kind, HarnessWorkerResponseKind::PARSE_ERROR);
}

TEST(HarnessHostAgentTest, CancellationAndTimeoutKillDescendantsAndReleaseReservations) {
    Fixture cli("sleep 60 & echo $! > child.pid; wait");
    auto config = cli.config("codex");
    config.request_timeout = std::chrono::milliseconds(250);
    auto executor = make_host_agent_executor(config);
    auto call = request();
    call.usage = std::make_shared<UsageAccumulator>();
    call.model_token_budget = 30000;
    const auto timed = executor(call, std::make_shared<graph::CancelToken>());
    EXPECT_EQ(timed.kind, HarnessWorkerResponseKind::TIMEOUT);
    EXPECT_TRUE(call.usage->try_reserve(25000, 30000));
    call.usage->release_reservation(25000);
    auto token = std::make_shared<graph::CancelToken>();
    auto running = std::async(std::launch::async, [&] { return executor(call, token); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    token->cancel();
    EXPECT_EQ(running.get().kind, HarnessWorkerResponseKind::CANCELLED);
    std::ifstream pid_file(cli.root / "child.pid");
    pid_t descendant = 0;
    ASSERT_TRUE(static_cast<bool>(pid_file >> descendant));
    ASSERT_GT(descendant, 0);
    // A reparented zombie is not a running child; its PID may briefly persist.
    std::ifstream status("/proc/" + std::to_string(descendant) + "/stat");
    std::string pid_text, command;
    char process_state = '\0';
    if (status >> pid_text >> command >> process_state) EXPECT_EQ(process_state, 'Z');
}

TEST(HarnessHostAgentTest, PreflightRejectsAutoLoggedOutAndNestedDispatch) {
    Fixture cli("printf '1 credentials\\n'");
    auto config = cli.config("auto");
    EXPECT_FALSE(preflight_host_agent(config).available);
    config.host = "opencode";
    config.model = "openai/missing";
    EXPECT_FALSE(preflight_host_agent(config).available);
    EXPECT_FALSE(preflight_host_agent(HostAgentExecutorConfig{
        "codex", (cli.root / "missing-cli").string(), {}, cli.root}).available);
    Fixture logged_out("exit 1", false);
    EXPECT_FALSE(preflight_host_agent(logged_out.config("opencode")).available);
    EXPECT_FALSE(preflight_host_agent(logged_out.config("claude")).available);
    EXPECT_FALSE(preflight_host_agent(logged_out.config("codex")).available);
    Fixture old_version("exit 1", true, "1.0.0");
    const auto unsupported = preflight_host_agent(old_version.config("opencode"));
    EXPECT_FALSE(unsupported.available);
    EXPECT_NE(unsupported.detail.find("HOST_VERSION"), std::string::npos);
    ScopedEnvironment nested("NEOGRAPH_HARNESS_HOST_DEPTH", "1");
    EXPECT_FALSE(preflight_host_agent(cli.config("codex")).available);
}

TEST(HarnessHostAgentLiveTest, SavedHostLoginWithoutNeoGraphProviderKey) {
    const char* selected = std::getenv("NEOGRAPH_HARNESS_LIVE_HOST");
    if (!selected || !*selected)
        GTEST_SKIP() << "Set NEOGRAPH_HARNESS_LIVE_HOST on a private authenticated runner";
    HostAgentExecutorConfig config;
    config.host = selected;
    config.workspace = std::filesystem::current_path();
    if (const char* model = std::getenv("NEOGRAPH_HARNESS_LIVE_MODEL")) config.model = model;
    config.startup_timeout = std::chrono::seconds(30);
    config.request_timeout = std::chrono::minutes(2);
    const auto status = preflight_host_agent(config);
    if (!status.available) GTEST_SKIP() << "Selected host login unavailable: " << status.detail;
    // The child allowlist excludes all provider API-key environment variables.
    if (config.host == "opencode") {
        if (!std::getenv("NEOGRAPH_HARNESS_LIVE_OPENCODE_OAUTH"))
            GTEST_SKIP() << "OpenCode OAuth live proof requires the private OAuth runner";
        if (!status.openai_oauth)
            GTEST_SKIP() << "opencode auth list does not report an OpenAI OAuth login";
        if (config.model.rfind("openai/", 0) != 0)
            GTEST_SKIP() << "Set NEOGRAPH_HARNESS_LIVE_MODEL to an available openai/ model";
    }
    auto call = request();
    call.task["objective"] = "Return {\"valid\":true} without writing to the filesystem.";
    call.worker["_harness_provider_budget"]["provider_timeout_seconds"] = 120;
    call.usage = std::make_shared<UsageAccumulator>();
    call.model_token_budget = 200000;
    const auto response = make_host_agent_executor(config)(call, std::make_shared<graph::CancelToken>());
    ASSERT_EQ(response.kind, HarnessWorkerResponseKind::VALUE) << response.message;
    EXPECT_EQ(response.value, json({{"valid", true}}));
}
} // namespace
#endif
