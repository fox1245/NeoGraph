// Request headers that vary per deployment: environment templating in schema
// headers (`${VAR}` required, `${VAR?}` optional) and Config::extra_headers.
//
// Live motivation: an Anthropic multi-workspace API key answers HTTP 400 ("this
// request must include the anthropic-workspace-id header") on every call, and
// `anthropic-beta` (e.g. interleaved thinking) had no way in either.

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

using neograph::CompletionParams;
using neograph::json;
using neograph::llm::SchemaProvider;
using neograph::llm::test_access::SchemaProviderTestAccess;

namespace {

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

struct EnvGuard {
    std::string name;
    explicit EnvGuard(std::string n, const char* value) : name(std::move(n)) {
        set_env(name.c_str(), value);
    }
    ~EnvGuard() { set_env(name.c_str(), nullptr); }
};

std::unique_ptr<SchemaProvider> claude(std::map<std::string, std::string> extra = {},
                                       const std::string& base_url = "") {
    SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = "test-key";
    cfg.extra_headers = std::move(extra);
    if (!base_url.empty()) {
        cfg.base_url_override = base_url;
        cfg.allow_insecure_loopback = true;
    }
    return SchemaProvider::create(cfg);
}

std::map<std::string, std::string> headers(SchemaProvider& sp) {
    return SchemaProviderTestAccess::build_headers(sp, "test-key");
}

// Case-insensitive lookup of a header name; counts how many spellings exist.
int count_ci(const std::map<std::string, std::string>& map, const std::string& name) {
    int n = 0;
    for (const auto& [key, _] : map) {
        if (key.size() != name.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < key.size(); ++i) {
            same = same && std::tolower(static_cast<unsigned char>(key[i])) ==
                               std::tolower(static_cast<unsigned char>(name[i]));
        }
        n += same ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST(SchemaProviderHeaders, OptionalEnvHeaderIsSentWhenTheVariableIsSet) {
    EnvGuard workspace("ANTHROPIC_WORKSPACE_ID", "wrkspc_123");
    EnvGuard beta("ANTHROPIC_BETA", nullptr);
    auto sp = claude();
    const auto h = headers(*sp);
    ASSERT_TRUE(h.count("anthropic-workspace-id"));
    EXPECT_EQ(h.at("anthropic-workspace-id"), "wrkspc_123");
    EXPECT_EQ(h.at("anthropic-version"), "2023-06-01");
    EXPECT_EQ(h.at("x-api-key"), "test-key");
    EXPECT_EQ(h.count("anthropic-beta"), 0u) << "an unset optional variable omits the whole header";
}

TEST(SchemaProviderHeaders, OptionalEnvHeaderIsOmittedWhenUnsetOrEmpty) {
    EnvGuard workspace("ANTHROPIC_WORKSPACE_ID", nullptr);
    EnvGuard beta("ANTHROPIC_BETA", "");
    auto sp = claude();
    const auto h = headers(*sp);
    EXPECT_EQ(h.count("anthropic-workspace-id"), 0u);
    EXPECT_EQ(h.count("anthropic-beta"), 0u);
    EXPECT_EQ(h.at("anthropic-version"), "2023-06-01");
}

TEST(SchemaProviderHeaders, ConfigHeadersOverrideTheSchemaCaseInsensitively) {
    EnvGuard workspace("ANTHROPIC_WORKSPACE_ID", "from-env");
    EnvGuard beta("ANTHROPIC_BETA", "env-beta");
    auto sp = claude({{"Anthropic-Version", "2099-01-01"},
                      {"anthropic-beta", "interleaved-thinking-2025-05-14"}});
    const auto h = headers(*sp);
    EXPECT_EQ(count_ci(h, "anthropic-version"), 1) << "two spellings would both be sent";
    ASSERT_TRUE(h.count("Anthropic-Version"));
    EXPECT_EQ(h.at("Anthropic-Version"), "2099-01-01");
    EXPECT_EQ(h.at("anthropic-beta"), "interleaved-thinking-2025-05-14") << "Config beats the env value";
    EXPECT_EQ(h.at("anthropic-workspace-id"), "from-env") << "untouched headers keep the env value";
}

TEST(SchemaProviderHeaders, ConfigHeadersAreLiteralNotTemplates) {
    auto sp = claude({{"x-note", "${NOT_EXPANDED}"}});
    EXPECT_EQ(headers(*sp).at("x-note"), "${NOT_EXPANDED}");
}

TEST(SchemaProviderHeaders, HeadersReachTheWire) {
    std::mutex mutex;
    std::map<std::string, std::string> seen;
    httplib::Server server;
    server.Post(R"(.*)", [&](const httplib::Request& req, httplib::Response& res) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const char* name : {"anthropic-workspace-id", "anthropic-beta", "anthropic-version"}) {
                seen[name] = req.get_header_value(name);
            }
        }
        res.set_content(R"({"role":"assistant","content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn"})",
                        "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread worker([&] { server.listen_after_bind(); });
    for (int i = 0; i < 200 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EnvGuard workspace("ANTHROPIC_WORKSPACE_ID", "wrkspc_wire");
    EnvGuard beta("ANTHROPIC_BETA", nullptr);
    auto sp = claude({{"anthropic-beta", "interleaved-thinking-2025-05-14"}},
                     "http://127.0.0.1:" + std::to_string(port));
    CompletionParams params;
    params.model = "test-model";
    params.messages.push_back({"user", "hi"});
    sp->complete(params);

    server.stop();
    worker.join();
    EXPECT_EQ(seen["anthropic-workspace-id"], "wrkspc_wire");
    EXPECT_EQ(seen["anthropic-beta"], "interleaved-thinking-2025-05-14");
    EXPECT_EQ(seen["anthropic-version"], "2023-06-01");
}

TEST(SchemaProviderHeaders, CredentialsAreStillRefusedOverPlainHttp) {
    SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = "test-key";
    cfg.base_url_override = "http://example.invalid";
    cfg.extra_headers = {{"anthropic-workspace-id", "wrkspc_1"}};
    EXPECT_THROW((void)SchemaProvider::create(cfg), std::exception);
    cfg.extra_headers.clear();
    EXPECT_THROW((void)SchemaProvider::create(cfg), std::exception);
}

TEST(SchemaProviderHeaders, UnusableConfigHeadersAreRejectedAtCreation) {
    for (const auto& bad : {std::map<std::string, std::string>{{"", "v"}},
                            {{"bad name", "v"}},
                            {{"x-a:b", "v"}},
                            {{"x-ok", "line1\r\nInjected: yes"}},
                            {{"x-ok", "line1\nline2"}}}) {
        EXPECT_THROW((void)claude(bad), std::invalid_argument);
    }
}

#ifndef _WIN32
namespace {
std::unique_ptr<SchemaProvider> custom(const std::string& header_json) {
    const std::string schema = std::string(R"({"name":"hdr","connection":{"base_url":"http://127.0.0.1:9","endpoint":"/e","auth_header":"Authorization","auth_prefix":"Bearer ","extra_headers":)") +
        header_json + R"(},)" +
        R"("request":{"model_field":"model","messages_field":"messages"},"system_prompt":{"strategy":"in_messages"},)" +
        R"("messages":{"role_field":"role","content_field":"content"},"tool_call_in_message":{"strategy":"tool_calls_array"},)" +
        R"("tool_result":{"strategy":"flat"},"image":{"strategy":"none"},)" +
        R"("response":{"strategy":"choices_message","message_path":"choices.0.message"},"streaming":{"format":"sse_data"}})";
    char path[] = "/tmp/neograph_hdr_XXXXXX.json";
    const int fd = mkstemps(path, 5);
    EXPECT_GE(fd, 0);
    close(fd);
    { std::ofstream(path) << schema; }
    SchemaProvider::Config cfg;
    cfg.schema_path = path;
    cfg.api_key = "k";
    cfg.allow_insecure_loopback = true;
    std::unique_ptr<SchemaProvider> sp;
    try {
        sp = SchemaProvider::create(cfg);
    } catch (...) {
        std::remove(path);
        throw;
    }
    std::remove(path);
    return sp;
}
}  // namespace

TEST(SchemaProviderHeaders, RequiredVariableMustBeSetAndIsNamedWhenMissing) {
    auto sp = custom(R"({"x-team":"team-${NEOGRAPH_TEST_TEAM}"})");
    {
        EnvGuard team("NEOGRAPH_TEST_TEAM", nullptr);
        try {
            (void)headers(*sp);
            FAIL() << "a required variable that is unset must not become an empty header";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("NEOGRAPH_TEST_TEAM"), std::string::npos)
                << error.what();
        }
    }
    EnvGuard team("NEOGRAPH_TEST_TEAM", "blue");
    EXPECT_EQ(headers(*sp).at("x-team"), "team-blue");
}

TEST(SchemaProviderHeaders, SeveralVariablesExpandAndOneMissingOptionalOmitsTheHeader) {
    auto sp = custom(R"({"x-both":"${NEOGRAPH_TEST_A}-${NEOGRAPH_TEST_B?}"})");
    EnvGuard a("NEOGRAPH_TEST_A", "alpha");
    {
        EnvGuard b("NEOGRAPH_TEST_B", "beta");
        EXPECT_EQ(headers(*sp).at("x-both"), "alpha-beta");
    }
    EnvGuard b("NEOGRAPH_TEST_B", nullptr);
    EXPECT_EQ(headers(*sp).count("x-both"), 0u);
}

TEST(SchemaProviderHeaders, AnEnvValueWithALineBreakIsRefused) {
    auto sp = custom(R"({"x-team":"${NEOGRAPH_TEST_TEAM}"})");
    EnvGuard team("NEOGRAPH_TEST_TEAM", "blue\r\nX-Injected: 1");
    EXPECT_THROW((void)headers(*sp), std::runtime_error);
}

TEST(SchemaProviderHeaders, MalformedTemplatesAreRejectedAtCreation) {
    for (const char* bad : {R"({"x":"${"})", R"({"x":"${}"})", R"({"x":"${?}"})", R"({"x":"${1BAD}"})",
                            R"({"x":"${A B}"})", R"({"bad name":"v"})"}) {
        EXPECT_THROW((void)custom(bad), std::invalid_argument) << bad;
    }
}
#endif  // !_WIN32
