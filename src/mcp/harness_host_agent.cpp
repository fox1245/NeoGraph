#include <neograph/mcp/harness_host_agent.h>
#if defined(_WIN32) || !defined(__linux__)
#include <stdexcept>
namespace neograph::mcp {
HostAgentPreflight preflight_host_agent(const HostAgentExecutorConfig& config) {
    return {false, config.host, {}, {}, config.model,
            "local host CLI subprocess isolation requires Linux"};
}
HarnessWorkerExecutor make_host_agent_executor(HostAgentExecutorConfig) {
    throw std::runtime_error("local host CLI subprocess isolation requires Linux");
}
} // namespace neograph::mcp
#else
#include <neograph/provider.h>

#include "harness_journal_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

namespace neograph::mcp {
namespace {

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    void reset() { if (value_ >= 0) ::close(value_); value_ = -1; }
private:
    int value_;
};

class Child {
public:
    explicit Child(pid_t pid) : pid_(pid), group_(pid) {}
    ~Child() { stop(); }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    void stop() noexcept {
        if (group_ <= 0) return;
        // Kill the entire session even after its leader exits; grandchildren may survive.
        ::kill(-group_, SIGTERM);
        for (int n = 0; n < 20; ++n) {
            if (pid_ > 0) {
                const auto waited = ::waitpid(pid_, &status_, WNOHANG);
                if (waited == pid_ || (waited < 0 && errno == ECHILD)) pid_ = -1;
            }
            if (::kill(-group_, 0) < 0 && errno == ESRCH) break;
            ::usleep(10000);
        }
        ::kill(-group_, SIGKILL);
        if (pid_ > 0) {
            while (::waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {}
        }
        pid_ = -1;
        group_ = -1;
    }
    bool reap() noexcept {
        if (pid_ <= 0) return true;
        int value = 0;
        const auto result = ::waitpid(pid_, &value, WNOHANG);
        if (result == pid_) { status_ = value; pid_ = -1; return true; }
        if (result < 0 && errno == ECHILD) { pid_ = -1; return true; }
        return false;
    }
    int status() const { return status_; }
private:
    pid_t pid_;
    pid_t group_;
    int status_ = 0;
};

struct ProcessResult {
    std::string out, err;
    int status = 0;
    bool timed_out = false, startup_timed_out = false, cancelled = false, truncated = false;
};

std::string executable_path(const HostAgentExecutorConfig& config) {
    if (!config.executable.empty()) {
        if (!std::filesystem::path(config.executable).is_absolute())
            throw std::invalid_argument("host executable override must be absolute");
        if (::access(config.executable.c_str(), X_OK) != 0)
            throw std::runtime_error("selected host executable is missing or not executable");
        return config.executable;
    }
    // Resolve through PATH without invoking a shell, and never search the current directory.
    const char* path = std::getenv("PATH");
    const std::string name = config.host == "claude" ? "claude" : config.host == "codex" ? "codex" : "opencode";
    std::string_view dirs = path ? path : "";
    while (!dirs.empty()) {
        auto end = dirs.find(':');
        const auto dir = dirs.substr(0, end);
        if (!dir.empty() && dir.front() == '/') {
            auto candidate = (std::filesystem::path(dir) / name).string();
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (end == std::string_view::npos) break;
        dirs.remove_prefix(end + 1);
    }
    throw std::runtime_error("selected host CLI is not installed or not on PATH");
}

std::filesystem::path workspace_path(const HostAgentExecutorConfig& config) {
    if (config.workspace.empty()) throw std::invalid_argument("host worker requires workspace root");
    std::error_code ec;
    auto path = std::filesystem::canonical(config.workspace, ec);
    if (ec || !std::filesystem::is_directory(path))
        throw std::invalid_argument("host workspace root must be an existing directory");
    return path;
}

class IsolatedConfig {
public:
    explicit IsolatedConfig(bool enabled) {
        if (!enabled) return;
        auto pattern = (std::filesystem::temp_directory_path() / "neograph-host-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* path = ::mkdtemp(buffer.data());
        if (!path) throw std::runtime_error("cannot create isolated host config directory");
        try { path_ = path; }
        catch (...) { ::rmdir(path); throw; }
    }
    ~IsolatedConfig() { if (!path_.empty()) { std::error_code ec; std::filesystem::remove_all(path_, ec); } }
    IsolatedConfig(const IsolatedConfig&) = delete;
    IsolatedConfig& operator=(const IsolatedConfig&) = delete;
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

// Only the official host CLI receives HOME/XDG_DATA_HOME to use its saved login.
// The worker gets a fresh config and working directory, not the caller's MCP/plugin configuration.
std::vector<std::string> environment_for_child(const std::filesystem::path& isolated,
                                                const std::filesystem::path& workspace) {
    static constexpr const char* allowed[] = {
        "HOME", "USER", "LOGNAME", "PATH", "LANG", "LC_ALL", "TERM",
        "TMPDIR", "XDG_DATA_HOME", "XDG_CACHE_HOME", "CODEX_HOME", "CLAUDE_CONFIG_DIR"
    };
    std::vector<std::string> result;
    for (const char* key : allowed)
        if (const char* value = ::getenv(key)) result.emplace_back(std::string(key) + "=" + value);
    if (isolated.empty()) {
        if (const char* value = ::getenv("XDG_CONFIG_HOME")) result.emplace_back(std::string("XDG_CONFIG_HOME=") + value);
    } else {
        result.emplace_back("XDG_CONFIG_HOME=" + isolated.string());
        result.emplace_back("OPENCODE_CONFIG_DIR=" + isolated.string());
    }
    result.emplace_back("NEOGRAPH_HARNESS_HOST_DEPTH=1");
    result.emplace_back("OPENCODE_DISABLE_AUTOUPDATE=1");
    result.emplace_back("OPENCODE_DISABLE_DEFAULT_PLUGINS=1");
    result.emplace_back("OPENCODE_DISABLE_CLAUDE_CODE=1");
    const json policy = {{"autoupdate", false}, {"snapshot", false},
                         {"permission", {{"*", "deny"},
                                          {"read", {{"*", "allow"}, {"*.env", "deny"},
                                                    {"*.env.*", "deny"}, {"*.env.example", "allow"},
                                                    {"**/.codex/auth.json", "deny"},
                                                    {"**/.claude/.credentials.json", "deny"},
                                                    {"**/opencode/auth.json", "deny"}}},
                                          {"glob", "allow"}, {"grep", "allow"},
                                          {"external_directory", {{workspace.string() + "/**", "allow"}}}}}};
    result.emplace_back("OPENCODE_CONFIG_CONTENT=" + policy.dump());
    return result;
}

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& args,
                          const std::filesystem::path& cwd, std::chrono::milliseconds timeout,
                          std::size_t cap, const std::shared_ptr<graph::CancelToken>& cancel,
                          bool isolate_opencode = false,
                          std::chrono::milliseconds startup_timeout = std::chrono::milliseconds::max()) {
    IsolatedConfig isolated(isolate_opencode);
    auto workdir = isolate_opencode ? isolated.path() : cwd;
    const auto workdir_bytes = workdir.string();
    auto variables = environment_for_child(isolated.path(), cwd);
    std::vector<char*> envp;
    for (auto& variable : variables) envp.push_back(variable.data());
    envp.push_back(nullptr);
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int out_pipe[2], err_pipe[2];
    if (::pipe2(out_pipe, O_CLOEXEC) != 0) throw std::runtime_error("host stdout pipe failed");
    Fd out_read(out_pipe[0]), out_write(out_pipe[1]);
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) throw std::runtime_error("host stderr pipe failed");
    Fd err_read(err_pipe[0]), err_write(err_pipe[1]);
    if (::fcntl(out_read.get(), F_SETFL, O_NONBLOCK) < 0 ||
        ::fcntl(err_read.get(), F_SETFL, O_NONBLOCK) < 0)
        throw std::runtime_error("host capture pipe configuration failed");
    const auto pid = ::fork();
    if (pid < 0) throw std::runtime_error("host fork failed");
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(out_write.get(), STDOUT_FILENO);
        ::dup2(err_write.get(), STDERR_FILENO);
        int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd >= 0) { ::dup2(null_fd, STDIN_FILENO); ::close(null_fd); }
        out_read.reset(); out_write.reset(); err_read.reset(); err_write.reset();
        if (::chdir(workdir_bytes.c_str()) != 0) _exit(126);
        ::execve(executable.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    ::setpgid(pid, pid);
    Child child(pid);
    out_write.reset(); err_write.reset();
    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeout;
    bool out_open = true, err_open = true, exited = false, first_output = false;
    while (out_open || err_open || !exited) {
        if (cancel && cancel->is_cancelled()) { result.cancelled = true; break; }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { result.timed_out = true; break; }
        if (!first_output && !exited &&
            startup_timeout != std::chrono::milliseconds::max() &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started) >= startup_timeout) {
            result.startup_timed_out = true; break;
        }
        pollfd fds[] = {{out_read.get(), static_cast<short>(out_open ? POLLIN : 0), 0},
                        {err_read.get(), static_cast<short>(err_open ? POLLIN : 0), 0}};
        ::poll(fds, 2, 10);
        const auto drain = [&](Fd& fd, bool& open, std::string& target) {
            if (!open) return;
            char bytes[4096];
            for (;;) {
                const auto count = ::read(fd.get(), bytes, sizeof(bytes));
                if (count > 0) {
                    first_output = true;
                    const auto room = cap > target.size() ? cap - target.size() : 0;
                    target.append(bytes, std::min<std::size_t>(room, static_cast<std::size_t>(count)));
                    if (static_cast<std::size_t>(count) > room) result.truncated = true;
                } else if (count == 0) { open = false; fd.reset(); break; }
                else { if (errno == EINTR) continue; if (errno == EAGAIN) break; open = false; fd.reset(); break; }
            }
        };
        drain(out_read, out_open, result.out);
        drain(err_read, err_open, result.err);
        exited = child.reap();
        if (result.truncated) break;
    }
    result.status = child.status();
    // Child destructor terminates the entire group even if CLI forked a descendant.
    return result;
}

bool has(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool has_ci(std::string_view text, std::string_view phrase) {
    return std::search(text.begin(), text.end(), phrase.begin(), phrase.end(),
                       [](unsigned char lhs, unsigned char rhs) {
                           const auto lower = [](unsigned char c) {
                               return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : static_cast<int>(c);
                           };
                           return lower(lhs) == lower(rhs);
                       }) != text.end();
}

std::string classify_error(std::string_view text) {
    // Do not persist raw host stderr; it can contain sensitive host diagnostics.
    if (has_ci(text, "authentication") || has_ci(text, "unauthorized") ||
        has_ci(text, "not logged in") || has_ci(text, "login required") ||
        has_ci(text, "invalid credentials"))
        return "HOST_AUTH: authenticate with the selected host CLI";
    if (has_ci(text, "quota") || has_ci(text, "rate_limit") ||
        has_ci(text, "rate limit") || has_ci(text, "usage limit"))
        return "HOST_QUOTA: selected host usage limit reached";
    if (has_ci(text, "model_not_found") || has_ci(text, "model not found") ||
        has_ci(text, "unknown model") || has_ci(text, "model unavailable"))
        return "HOST_MODEL: selected model unavailable";
    if (has_ci(text, "policy") || has_ci(text, "permission denied") ||
        has_ci(text, "forbidden")) return "HOST_POLICY: selected host policy rejected request";
    return "HOST_EXIT: selected host CLI failed; inspect host diagnostics locally";
}

std::string redacted_excerpt(std::string_view source) {
    std::string excerpt;
    excerpt.reserve(std::min<std::size_t>(source.size(), 96));
    bool redacting = false;
    for (const auto byte : source.substr(0, 96)) {
        const auto safe = byte == '{' || byte == '}' || byte == '[' || byte == ']' ||
                          byte == ':' || byte == ',' || byte == ' ' || byte == '\n';
        if (safe) { excerpt.push_back(byte == '\n' ? ' ' : byte); redacting = false; }
        else if (!redacting) { excerpt.push_back('*'); redacting = true; }
    }
    return excerpt;
}

std::optional<json> parse_json(std::string_view input) {
    try { return json::parse(input); }
    catch (const json::exception&) { return std::nullopt; }
}

struct FinalValue { std::optional<json> value; std::string error; std::uint64_t input = 0, output = 0; };
FinalValue final_value(const std::string& host, const std::string& output, std::size_t max_events) {
    FinalValue result;
    try {
    if (host == "claude") {
        const auto event = parse_json(output);
        if (!event || !event->is_object() || event->value("type", "") != "result") {
            result.error = "invalid Claude result framing; excerpt=" + redacted_excerpt(output);
            return result;
        }
        if (event->value("is_error", false)) { result.error = classify_error(event->value("result", "")); return result; }
        if (event->contains("usage") && event->at("usage").is_object()) {
            const auto& usage = event->at("usage");
            result.input = usage.value("input_tokens", std::uint64_t{0}) +
                           usage.value("cache_read_input_tokens", std::uint64_t{0}) +
                           usage.value("cache_creation_input_tokens", std::uint64_t{0});
            result.output = usage.value("output_tokens", std::uint64_t{0});
        }
        if (event->contains("structured_output")) { result.value = event->at("structured_output"); return result; }
        if (event->contains("result") && event->at("result").is_string()) result.value = parse_json(event->at("result").get<std::string>());
    } else {
        std::string text;
        std::size_t events = 0;
        bool complete = false;
        for (std::size_t begin = 0; begin < output.size();) {
            if (++events > max_events) { result.error = "host event limit exceeded"; return result; }
            auto end = output.find('\n', begin);
            if (end == std::string::npos) end = output.size();
            const auto event = parse_json(std::string_view(output).substr(begin, end - begin));
            if (!event || !event->is_object()) {
                result.error = "invalid host JSONL event framing; excerpt=" +
                               redacted_excerpt(std::string_view(output).substr(begin, end - begin));
                return result;
            }
            if (!event->contains("type") || !event->at("type").is_string()) {
                result.error = "invalid host event field types";
                return result;
            }
            const auto type = event->at("type").get<std::string>();
            if (host == "opencode") {
                if (type == "text" && event->contains("part") && event->at("part").is_object() && event->at("part").value("type", "") == "text")
                    text += event->at("part").value("text", "");
                if (type == "step_finish") {
                    complete = true;
                    if (event->contains("part") && event->at("part").is_object() &&
                        event->at("part").contains("tokens") && event->at("part").at("tokens").is_object()) {
                        const auto& tokens = event->at("part").at("tokens");
                        result.input += tokens.value("input", std::uint64_t{0});
                        if (tokens.contains("cache") && tokens.at("cache").is_object()) {
                            result.input += tokens.at("cache").value("read", std::uint64_t{0}) +
                                            tokens.at("cache").value("write", std::uint64_t{0});
                        }
                        result.output += tokens.value("output", std::uint64_t{0}) +
                                         tokens.value("reasoning", std::uint64_t{0});
                    }
                }
                if (type == "error") { result.error = classify_error(event->dump()); return result; }
            } else {
                if (type == "item.completed" && event->contains("item") && event->at("item").is_object() && event->at("item").value("type", "") == "agent_message")
                    text = event->at("item").value("text", "");
                if (type == "turn.completed") {
                    complete = true;
                    if (event->contains("usage") && event->at("usage").is_object()) {
                        const auto& usage = event->at("usage");
                        result.input = usage.value("input_tokens", std::uint64_t{0});
                        result.output = usage.value("output_tokens", std::uint64_t{0});
                    }
                }
                if (type == "turn.failed" || type == "error") { result.error = classify_error(event->dump()); return result; }
            }
            begin = end + 1;
        }
        if (!complete) { result.error = "host event stream has no completed turn"; return result; }
        result.value = parse_json(text);
    }
    if (!result.value) result.error = "final host message is not JSON; excerpt=" + redacted_excerpt(output);
    } catch (const json::exception&) {
        result.value.reset();
        result.error = "invalid host event field types; excerpt=" + redacted_excerpt(output);
    }
    return result;
}

std::string worker_prompt(const HarnessWorkerCall& call, const std::filesystem::path& root) {
    json contract = {{"objective", call.task.value("objective", "")},
                     {"acceptance", call.task.value("acceptance", json::array())},
                     {"instructions", call.worker.value("instructions", "")},
                     {"output_schema", call.worker.at("output_schema")},
                     {"workspace", root.string()}};
    if (!call.repair_feedback.empty()) contract["repair_feedback"] = call.repair_feedback;
    if (call.resume_value) contract["resume_value"] = *call.resume_value;
    return "Read-only worker. Never edit files or run commands. Return exactly one JSON value matching output_schema; no fences. Contract:\n" + contract.dump();
}

bool supported_version(const std::string& host, std::string_view text) {
    const auto first = text.find_first_of("0123456789");
    if (first == std::string_view::npos) return false;
    text.remove_prefix(first);
    int numbers[3] = {};
    for (int& number : numbers) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc{}) return false;
        const auto consumed = static_cast<std::size_t>(end - text.data());
        text.remove_prefix(consumed);
        if (&number != &numbers[2]) {
            if (text.empty() || text.front() != '.') return false;
            text.remove_prefix(1);
        }
    }
    if (host == "opencode") return numbers[0] > 1 || (numbers[0] == 1 && (numbers[1] > 1 || (numbers[1] == 1 && numbers[2] >= 1)));
    if (host == "claude") return numbers[0] > 2 || (numbers[0] == 2 && numbers[1] >= 1);
    return true;
}

} // namespace

HostAgentPreflight preflight_host_agent(const HostAgentExecutorConfig& config) {
    HostAgentPreflight result;
    result.host = config.host;
    result.model = config.model.empty() ? "host default" : config.model;
    if (config.host != "opencode" && config.host != "claude" && config.host != "codex") {
        result.detail = "select exactly one host: opencode, claude, or codex (auto is not supported)";
        return result;
    }
    if (config.startup_timeout.count() <= 0 || config.request_timeout.count() <= 0 ||
        config.max_prompt_bytes == 0 || config.max_output_bytes == 0 || config.max_events == 0) {
        result.detail = "host startup/request and prompt/output/event budgets must be positive";
        return result;
    }
    if (std::getenv("NEOGRAPH_HARNESS_HOST_DEPTH")) {
        result.detail = "nested Harness host delegation is disabled";
        return result;
    }
    try {
        const auto cwd = workspace_path(config);
        result.executable = executable_path(config);
        const auto version = run_process(result.executable, {"--version"}, cwd, config.startup_timeout, 4096, nullptr);
        if (version.timed_out || version.truncated || !WIFEXITED(version.status) || WEXITSTATUS(version.status) != 0)
            throw std::runtime_error("host version check failed or is unsupported");
        std::string_view version_string = version.out.empty() ? std::string_view(version.err) :
                                                                std::string_view(version.out);
        const auto first = version_string.find_first_of("0123456789");
        if (first == std::string::npos)
            throw std::runtime_error("host version check returned no version");
        const auto last = version_string.find_first_not_of("0123456789.", first);
        result.version = version_string.substr(first, std::min<std::size_t>(
            last == std::string::npos ? version_string.size() - first : last - first, 32));
        if (!supported_version(config.host, result.version))
            throw std::runtime_error("HOST_VERSION: installed CLI does not support the required isolated worker profile");
        if (config.host == "opencode" || config.host == "codex") {
            const auto flags = config.host == "opencode" ?
                std::vector<std::string>{"run", "--help"} : std::vector<std::string>{"exec", "--help"};
            const auto help = run_process(result.executable, flags, cwd, config.startup_timeout, 16384, nullptr);
            const auto text = help.out + help.err;
            if (help.timed_out || help.truncated || !WIFEXITED(help.status) || WEXITSTATUS(help.status) != 0 ||
                (config.host == "opencode" ? (!has(text, "--format") || !has(text, "--model")) :
                   (!has(text, "--ignore-user-config") || !has(text, "--ignore-rules") ||
                    !has(text, "--ephemeral") || !has(text, "--sandbox") || !has(text, "--json"))))
                throw std::runtime_error("HOST_VERSION: installed CLI lacks required JSON or isolation flags");
        }
        std::vector<std::string> auth = config.host == "opencode" ? std::vector<std::string>{"auth", "list"} :
                                        config.host == "claude" ? std::vector<std::string>{"auth", "status"} :
                                                                   std::vector<std::string>{"login", "status"};
        const auto login = run_process(result.executable, auth, cwd, config.startup_timeout, 8192, nullptr);
        if (login.timed_out || login.truncated || !WIFEXITED(login.status) || WEXITSTATUS(login.status) != 0)
            throw std::runtime_error("host is logged out or auth status unavailable; sign in with its CLI");
        if (config.host == "opencode") {
            // OpenCode auth list has a human-readable count; zero entries is not logged in.
            if (has(login.out, "0 credentials") || has(login.out, "0 providers") || login.out.empty())
                throw std::runtime_error("no authenticated OpenCode providers; run opencode auth login");
            for (std::size_t begin = 0; begin < login.out.size();) {
                auto end = login.out.find('\n', begin);
                if (end == std::string::npos) end = login.out.size();
                const std::string_view line(login.out.data() + begin, end - begin);
                if (has_ci(line, "openai") && has_ci(line, "oauth")) {
                    result.openai_oauth = true;
                    break;
                }
                begin = end + 1;
            }
            if (!config.model.empty()) {
                const auto models = run_process(result.executable, {"models"}, cwd, config.startup_timeout, 65536, nullptr);
                if (models.timed_out || models.truncated || !WIFEXITED(models.status) || WEXITSTATUS(models.status) != 0 ||
                    !has(models.out, config.model))
                    throw std::runtime_error("requested OpenCode model unavailable; check opencode models");
            }
        } else if (config.host == "claude") {
            auto status = parse_json(login.out);
            if (!status || !status->is_object() || !status->value("loggedIn", false))
                throw std::runtime_error("Claude Code is logged out; run claude auth login");
        }
        result.available = true;
        result.detail = "local CLI authenticated; read-only subprocess with bounded JSON output";
    } catch (const std::exception& error) {
        result.detail = error.what();
    }
    return result;
}

HarnessWorkerExecutor make_host_agent_executor(HostAgentExecutorConfig config) {
    const auto preflight = preflight_host_agent(config);
    if (!preflight.available) throw std::invalid_argument(preflight.detail);
    const auto cwd = workspace_path(config);
    const auto executable = preflight.executable;
    return [config = std::move(config), cwd, executable](const HarnessWorkerCall& call,
                                                           const std::shared_ptr<graph::CancelToken>& cancel) {
        if (cancel && cancel->is_cancelled()) return HarnessWorkerResponse::cancelled("host worker cancelled before dispatch");
        if (call.policy.value("read_only", true) == false)
            return HarnessWorkerResponse::tool_error("HOST_POLICY: host worker only supports read-only policy");
        if (!call.tool_catalog.empty())
            return HarnessWorkerResponse::tool_error("HOST_POLICY: host CLI cannot execute Harness capability tools");
        if (call.model_token_budget && !call.usage)
            return HarnessWorkerResponse::tool_error("bounded host execution requires usage accounting");
        const auto budget = call.worker.value("_harness_provider_budget", json::object());
        const auto timeout_seconds = budget.value("provider_timeout_seconds", 0);
        const auto output_tokens = budget.value("max_output_tokens", 0);
        const auto input_tokens = budget.value("input_token_ceiling", std::uint64_t{0});
        if (output_tokens <= 0 || input_tokens == 0)
            return HarnessWorkerResponse::tool_error("host worker requires finite input/output token budgets");
        auto prompt = worker_prompt(call, cwd);
        if (prompt.size() > config.max_prompt_bytes || prompt.size() / 4 > input_tokens)
            return HarnessWorkerResponse::tool_error("host worker prompt exceeds configured input ceiling");
        const auto requested_output = static_cast<std::uint64_t>(output_tokens);
        const auto reserve = std::min<std::uint64_t>(
            std::numeric_limits<long long>::max(),
            input_tokens > std::numeric_limits<std::uint64_t>::max() - requested_output
                ? std::numeric_limits<std::uint64_t>::max()
                : input_tokens + requested_output);
        bool held = false;
        if (call.model_token_budget) {
            if (!call.usage->try_reserve(static_cast<long long>(reserve), static_cast<long long>(std::min<std::uint64_t>(call.model_token_budget, std::numeric_limits<long long>::max())))) {
                if (call.budget_exhausted) call.budget_exhausted->store(true);
                return HarnessWorkerResponse::cancelled("Program model-token budget exhausted before host dispatch");
            }
            held = true;
        }
        struct Reservation {
            const HarnessWorkerCall& call;
            long long tokens;
            bool held;
            ~Reservation() { if (held && call.usage) call.usage->release_reservation(tokens); }
        } reservation{call, static_cast<long long>(reserve), held};
        std::vector<std::string> args;
        if (config.host == "claude") {
            args = {"-p", "--safe-mode", "--output-format", "json", "--json-schema", call.worker.at("output_schema").dump(),
                    "--permission-mode", "plan", "--tools", "Read,Glob,Grep", "--no-session-persistence"};
            if (!config.model.empty()) { args.push_back("--model"); args.push_back(config.model); }
            args.push_back(std::move(prompt));
        } else if (config.host == "codex") {
            args = {"exec", "--json", "--ephemeral", "--ignore-user-config",
                    "--ignore-rules", "--sandbox", "read-only"};
            if (!config.model.empty()) { args.push_back("--model"); args.push_back(config.model); }
            args.push_back(std::move(prompt));
        } else {
            args = {"--pure", "run", "--format", "json"};
            if (!config.model.empty()) { args.push_back("--model"); args.push_back(config.model); }
            args.push_back(std::move(prompt));
        }
        const auto correlation = detail::journal_correlation_id("host");
        detail::append_current_harness_journal_event("host.call.started", {{"host", config.host}, {"model", config.model.empty() ? "host default" : config.model}, {"attempt", call.attempt}}, correlation);
        try {
            auto deadline = config.request_timeout;
            if (timeout_seconds > 0)
                deadline = std::min(deadline,
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::seconds(timeout_seconds)));
            const auto process = run_process(executable, args, cwd, deadline, config.max_output_bytes,
                                             cancel, config.host == "opencode", config.startup_timeout);
            HarnessWorkerResponse response;
            if ((cancel && cancel->is_cancelled()) || process.cancelled ||
                (call.budget_exhausted && call.budget_exhausted->load()))
                response = HarnessWorkerResponse::cancelled("host request cancelled");
            else if (process.startup_timed_out)
                response = HarnessWorkerResponse::timeout("HOST_STARTUP_TIMEOUT: host CLI produced no response before startup deadline");
            else if (process.timed_out) response = HarnessWorkerResponse::timeout("host request exceeded deadline");
            else if (process.truncated) response = HarnessWorkerResponse::tool_error("HOST_OUTPUT_LIMIT: host stream exceeds configured capture limit");
            else if (WIFSIGNALED(process.status)) response = HarnessWorkerResponse::tool_error("HOST_SIGNAL: host process terminated by signal " + std::to_string(WTERMSIG(process.status)));
            else if (!WIFEXITED(process.status) || WEXITSTATUS(process.status) != 0) {
                auto error = classify_error(process.err);
                if (error.rfind("HOST_EXIT:", 0) == 0) error = classify_error(process.out);
                response = HarnessWorkerResponse::tool_error(
                    error + "; exit=" + (WIFEXITED(process.status) ?
                        std::to_string(WEXITSTATUS(process.status)) : "unknown"));
            }
            else {
                auto final = final_value(config.host, process.out, config.max_events);
                if (call.usage && (final.input || final.output)) {
                    ChatCompletion::Usage usage;
                    usage.prompt_tokens = static_cast<int>(std::min<std::uint64_t>(final.input, std::numeric_limits<int>::max()));
                    usage.completion_tokens = static_cast<int>(std::min<std::uint64_t>(final.output, std::numeric_limits<int>::max()));
                    if (held) {
                        call.usage->settle_reservation(static_cast<long long>(reserve), usage);
                        reservation.held = false;
                    } else call.usage->add(usage);
                }
                if (call.model_token_budget && call.usage &&
                    call.usage->total_tokens_wide() >= 0 &&
                    static_cast<std::uint64_t>(call.usage->total_tokens_wide()) > call.model_token_budget) {
                    if (call.budget_exhausted) call.budget_exhausted->store(true);
                    response = HarnessWorkerResponse::cancelled("Program model-token budget exhausted during host request");
                } else if (!final.error.empty())
                    response = final.error.rfind("HOST_", 0) == 0 ?
                        HarnessWorkerResponse::tool_error(final.error) : HarnessWorkerResponse::parse_error(final.error);
                else if (call.model_token_budget && (final.input == 0 && final.output == 0))
                    response = HarnessWorkerResponse::tool_error("HOST_USAGE: host did not provide enforceable token usage");
                else if (final.output > static_cast<std::uint64_t>(output_tokens))
                    response = HarnessWorkerResponse::tool_error("HOST_OUTPUT_BUDGET: host exceeded output token ceiling");
                else if (final.input > input_tokens)
                    response = HarnessWorkerResponse::tool_error("HOST_INPUT_BUDGET: host exceeded input token ceiling");
                else response = HarnessWorkerResponse::success(std::move(*final.value));
            }
            detail::append_current_harness_journal_event("host.call.completed", {{"host", config.host}, {"outcome", static_cast<int>(response.kind)}}, correlation);
            return response;
        } catch (const std::exception&) {
            return HarnessWorkerResponse::tool_error("HOST_PROCESS: host CLI launch failed");
        }
    };
}

} // namespace neograph::mcp
#endif
