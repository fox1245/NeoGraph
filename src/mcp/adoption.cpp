#include <neograph/mcp/adoption.h>

#include <neograph/mcp/client.h>
#include "../core/sha256.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <sys/stat.h>

namespace neograph::mcp {
namespace {

std::string digest(std::string_view value) {
    const auto bytes = neograph::detail::sha256_digest(value);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "sha256:";
    out.reserve(71);
    for (const auto b : bytes) { out.push_back(hex[b >> 4]); out.push_back(hex[b & 15]); }
    return out;
}

bool unsafe_key(std::string_view key) {
    return key == "env" || key == "environment" || key == "file" || key == "headers" ||
           key == "authorization" || key == "token" || key == "password" || key == "secret" ||
           key == "api_key" || key == "apiKey" || key == "$env" || key == "$file";
}
bool contains_external_ref(const json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "environment" && it.value().is_object() && it.value().empty()) continue;
            if (unsafe_key(it.key()) || (it.value().is_string() &&
                (it.value().get<std::string>().rfind("env:", 0) == 0 ||
                 it.value().get<std::string>().rfind("file:", 0) == 0))) return true;
            if (contains_external_ref(it.value())) return true;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) if (contains_external_ref(item)) return true;
    }
    return false;
}

bool regular_user_file(const std::filesystem::path& path, std::string& reason) {
    std::error_code ec;
    const auto st = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st)) { reason = "source is not a regular file"; return false; }
#ifndef _WIN32
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) { reason = "source is not a regular file"; return false; }
    if (info.st_uid != ::geteuid()) { reason = "source is not owned by the current user"; return false; }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) { reason = "source is group/world writable"; return false; }
#endif
    return true;
}

std::filesystem::path default_source() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "opencode" / "opencode.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "opencode" / "opencode.json";
    throw std::invalid_argument("OpenCode global MCP source requires HOME or XDG_CONFIG_HOME");
}

std::filesystem::path canonical_dir(const std::filesystem::path& input, const std::filesystem::path& base) {
    if (input.empty()) return base;
    std::error_code ec;
    auto path = input.is_absolute() ? input : base / input;
    auto result = std::filesystem::canonical(path, ec);
    if (ec || !std::filesystem::is_directory(result)) throw std::invalid_argument("MCP cwd must be an existing directory");
    return result;
}

std::string resolve_executable(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front().empty()) throw std::invalid_argument("MCP command is empty");
    const auto& command = argv.front();
    if (command == "sh" || command == "bash" || command == "zsh" || command == "fish" ||
        command == "cmd" || command == "powershell" || command == "pwsh")
        throw std::invalid_argument("shell executors are not supported");
    std::error_code ec;
    if (command.find('/') != std::string::npos) {
        auto path = std::filesystem::canonical(command, ec);
        if (ec || !std::filesystem::is_regular_file(path) || ::access(path.c_str(), X_OK) != 0)
            throw std::invalid_argument("MCP executable is not an executable regular file");
        return path.string();
    }
    const char* raw = std::getenv("PATH");
    const std::string path = raw ? raw : "/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        const auto dir = path.substr(start, end == std::string::npos ? end : end - start);
        if (!dir.empty() && dir.front() == '/') {
            auto candidate = std::filesystem::path(dir) / command;
            if (::access(candidate.c_str(), X_OK) == 0) {
                auto resolved = std::filesystem::canonical(candidate, ec);
                if (!ec) return resolved.string();
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    throw std::invalid_argument("MCP executable is not on the approved PATH");
}

std::string executable_identity(const std::string& executable, const std::vector<std::string>& argv,
                                const std::filesystem::path& cwd) {
    std::ifstream file(executable, std::ios::binary);
    if (!file) throw std::invalid_argument("cannot read MCP executable identity");
    std::ostringstream bytes; bytes << file.rdbuf();
    std::ostringstream identity; identity << executable << '\n' << cwd.string() << '\n';
    for (const auto& arg : argv) identity << arg.size() << ':' << arg << '\n';
    identity << bytes.str();
    return digest(identity.str());
}

std::string schema_digest(const ToolDefinition& definition) {
    return digest(definition.to_json().dump());
}

bool forbidden_effects(const McpCapabilityManifest& manifest) {
    static const std::set<std::string> denied = {
        "mutation", "write", "delete", "deploy", "credential_access", "external_communication",
        "shell", "sql_write"};
    for (const auto& effect : manifest.effects) if (denied.count(effect)) return true;
    return false;
}

std::string trust_name(McpTrustMode mode) {
    return mode == McpTrustMode::pinned ? "pinned" : "trusted_mutable";
}

struct Adopted {
    DiscoveredMcpServer source;
    McpLaunchApproval launch;
    McpToolApproval tools;
    std::shared_ptr<MCPClient> client;
    std::map<std::string, std::string> schemas;
    bool revoked = false;
};

} // namespace

std::string mcp_tool_schema_hash(const json& definition) { return digest(definition.dump()); }

McpLaunchApproval make_mcp_launch_approval(const DiscoveredMcpServer& server,
                                           McpTrustMode mode,
                                           bool argv_no_credentials_attested) {
    if (!argv_no_credentials_attested) throw std::invalid_argument("explicit argv-no-credential attestation is required");
    const auto cwd = std::filesystem::canonical(server.cwd);
    const auto executable = resolve_executable(server.argv);
    McpLaunchApproval approval;
    approval.trust_mode = mode;
    approval.argv_no_credentials_attested = true;
    approval.source_path = std::filesystem::canonical(server.source_path).string();
    approval.source_content_hash = server.source_content_hash;
    approval.cwd = cwd.string();
    approval.executable = executable;
    approval.executable_identity = executable_identity(executable, server.argv, cwd);
    approval.argv_hash = digest(json(server.argv).dump());
    return approval;
}

McpDiscoveryReport OpenCodeGlobalMcpDiscovery::discover(const OpenCodeGlobalMcpConfig& config) {
    if (!config.single_user_local) throw std::invalid_argument("global MCP adoption requires single-user local mode");
    const auto path = config.source_path.empty() ? default_source() : config.source_path;
    McpDiscoveryReport report; report.source_path = path;
    std::string reason;
    if (!regular_user_file(path, reason)) { report.rejected.push_back({{}, reason}); return report; }
    std::ifstream input(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.size() > config.max_source_bytes) { report.rejected.push_back({{}, "source exceeds size limit"}); return report; }
    report.source_content_hash = digest(text);
    json root;
    try { root = json::parse(text); } catch (const std::exception&) { report.rejected.push_back({{}, "source is not valid JSON"}); return report; }
    if (!root.is_object() || !root.value("mcp", json()).is_object()) {
        report.rejected.push_back({{}, "user-global source has no mcp object"}); return report;
    }
    const auto& servers = root.at("mcp");
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        const auto name = it.key();
        try {
            if (!it.value().is_object()) throw std::invalid_argument("entry is not an object");
            const auto& entry = it.value();
            static const std::set<std::string> allowed = {"type", "command", "cwd", "enabled", "environment"};
            for (auto field = entry.begin(); field != entry.end(); ++field)
                if (!allowed.count(field.key())) throw std::invalid_argument("unsupported field: " + field.key());
            if (contains_external_ref(entry)) throw std::invalid_argument("environment or external secret reference is not supported");
            if (entry.value("enabled", true) != true) throw std::invalid_argument("disabled MCP entry is not adoptable");
            if (entry.value("type", "local") != "local") throw std::invalid_argument("only local stdio MCP entries are supported");
            if (!entry.contains("command") || !entry.at("command").is_array() || entry.at("command").empty())
                throw std::invalid_argument("local entry requires a command array");
            std::vector<std::string> argv;
            for (const auto& value : entry.at("command")) {
                if (!value.is_string() || value.get<std::string>().empty()) throw std::invalid_argument("command arguments must be nonempty strings");
                argv.push_back(value.get<std::string>());
            }
            if (argv.front().find("neograph-harness") != std::string::npos ||
                std::find(argv.begin(), argv.end(), "NEOGRAPH_HARNESS_HOST_DEPTH") != argv.end())
                throw std::invalid_argument("recursive NeoGraph Harness entry is not adoptable");
            const auto cwd = canonical_dir(entry.value("cwd", ""), path.parent_path());
            DiscoveredMcpServer descriptor;
            descriptor.source_id = "opencode:" + name;
            descriptor.server_name = name;
            descriptor.source_path = path;
            descriptor.cwd = cwd;
            descriptor.argv = std::move(argv);
            descriptor.source_content_hash = report.source_content_hash;
            descriptor.redacted_config_hash = digest(entry.dump());
            descriptor.enabled = true;
            report.servers.push_back(std::move(descriptor));
        } catch (const std::exception& error) { report.rejected.push_back({name, error.what()}); }
    }
    return report;
}

struct HardenedMcpClientRegistry::Impl {
    mutable std::mutex mutex;
    std::string policy_version;
    std::map<std::string, Adopted> adopted;
};

HardenedMcpClientRegistry::HardenedMcpClientRegistry(std::string policy_version)
    : impl_(std::make_unique<Impl>()) { if (policy_version.empty()) throw std::invalid_argument("MCP policy version is required"); impl_->policy_version = std::move(policy_version); }
HardenedMcpClientRegistry::~HardenedMcpClientRegistry() { std::lock_guard lk(impl_->mutex); impl_->adopted.clear(); }

void HardenedMcpClientRegistry::adopt(const McpAdoptionRequest& request) {
    const auto& source = request.server;
    const auto& launch = request.launch;
    const auto& tool_approval = request.tools;
    if (!source.enabled || source.server_name.empty() || source.argv.empty()) throw std::invalid_argument("MCP source descriptor is not adoptable");
    if (!launch.argv_no_credentials_attested) throw std::invalid_argument("explicit argv-no-credential attestation is required");
    if (launch.trust_mode != McpTrustMode::pinned && launch.trust_mode != McpTrustMode::trusted_mutable) throw std::invalid_argument("unknown MCP trust mode");
    if (tool_approval.policy_version.empty()) throw std::invalid_argument("MCP policy version is required");
    if (tool_approval.selected_tools.empty()) throw std::invalid_argument("at least one MCP tool must be selected");
    const auto path = std::filesystem::canonical(source.source_path);
    std::ifstream input(path, std::ios::binary); std::string text((std::istreambuf_iterator<char>(input)), {});
    if (launch.trust_mode == McpTrustMode::pinned &&
        digest(text) != source.source_content_hash) throw std::invalid_argument("MCP source changed since discovery");
    if (!launch.source_path.empty() && launch.source_path != path.string()) throw std::invalid_argument("MCP source path changed");
    if (launch.trust_mode == McpTrustMode::pinned && !launch.source_content_hash.empty() &&
        launch.source_content_hash != source.source_content_hash)
        throw std::invalid_argument("MCP source content approval changed");
    const auto cwd = std::filesystem::canonical(source.cwd);
    const auto executable = resolve_executable(source.argv);
    const auto identity = executable_identity(executable, source.argv, cwd);
    if (std::getenv("NEOGRAPH_HARNESS_HOST_DEPTH")) throw std::invalid_argument("nested NeoGraph Harness adoption is disabled");
    if (std::filesystem::path(executable).filename().string().find("neograph-harness") != std::string::npos)
        throw std::invalid_argument("recursive NeoGraph Harness executable is not adoptable");
    if (launch.trust_mode == McpTrustMode::pinned && !launch.cwd.empty() && launch.cwd != cwd.string())
        throw std::invalid_argument("MCP cwd does not match launch approval");
    if (launch.trust_mode == McpTrustMode::pinned && !launch.executable.empty() && launch.executable != executable)
        throw std::invalid_argument("MCP executable does not match launch approval");
    if (launch.trust_mode == McpTrustMode::pinned && launch.executable_identity != identity)
        throw std::invalid_argument("MCP executable identity changed");
    if (launch.trust_mode == McpTrustMode::pinned && launch.argv_hash != digest(json(source.argv).dump()))
        throw std::invalid_argument("MCP argv changed");
    if (tool_approval.launch_identity.empty() ||
        (launch.trust_mode == McpTrustMode::pinned && tool_approval.launch_identity != identity))
        throw std::invalid_argument("tool approval must bind the approved launch identity");
    if (tool_approval.policy_version != impl_->policy_version) throw std::invalid_argument("MCP policy version changed");
    std::map<std::string, McpCapabilityManifest> manifests;
    for (const auto& manifest : tool_approval.manifest) {
        if (manifest.tool.empty() || !manifest.auto_execute || forbidden_effects(manifest)) throw std::invalid_argument("MCP manifest denies unsafe or unapproved effects");
        manifests.emplace(manifest.tool, manifest);
    }
    std::vector<std::pair<std::string, std::string>> env = {{"PATH", "/usr/bin:/bin"}, {"LANG", "C"}, {"LC_ALL", "C"}, {"NEOGRAPH_MCP_ADOPTED", "1"}};
    auto launch_argv = source.argv;
    launch_argv.front() = executable;
    StdioClientConfig stdio;
    stdio.argv = std::move(launch_argv);
    stdio.cwd = cwd;
    stdio.environment = std::move(env);
    stdio.replace_environment = true;
    stdio.startup_timeout = source.startup_timeout;
    auto client = std::make_shared<MCPClient>(std::move(stdio));
    client->initialize("neograph-adopted-local");
    std::map<std::string, std::string> schemas;
    for (const auto& definition : client->get_tool_definitions()) {
        const auto name = definition.name;
        if (std::find(tool_approval.selected_tools.begin(), tool_approval.selected_tools.end(), name) == tool_approval.selected_tools.end()) continue;
        const auto expected = tool_approval.schema_hashes.find(name);
        if (expected == tool_approval.schema_hashes.end() || expected->second != schema_digest(definition)) throw std::invalid_argument("MCP tool schema changed or is not approved: " + name);
        if (!manifests.count(name)) throw std::invalid_argument("MCP tool has no capability manifest: " + name);
        schemas.emplace(name, expected->second);
    }
    for (const auto& name : tool_approval.selected_tools) if (!schemas.count(name)) throw std::invalid_argument("selected MCP tool was not reported by server: " + name);
    Adopted value{source, launch, tool_approval, std::move(client), std::move(schemas), false};
    std::lock_guard lk(impl_->mutex);
    impl_->adopted[source.server_name] = std::move(value);
}

void HardenedMcpClientRegistry::revoke(const std::string& server_name) { std::lock_guard lk(impl_->mutex); auto it = impl_->adopted.find(server_name); if (it != impl_->adopted.end()) { it->second.revoked = true; it->second.client.reset(); } }
bool HardenedMcpClientRegistry::contains(const std::string& server_name) const { std::lock_guard lk(impl_->mutex); auto it = impl_->adopted.find(server_name); return it != impl_->adopted.end() && !it->second.revoked && it->second.client; }

std::vector<McpAdoptionStatus> HardenedMcpClientRegistry::status() const {
    std::lock_guard lk(impl_->mutex); std::vector<McpAdoptionStatus> result;
    for (const auto& [name, value] : impl_->adopted) result.push_back({name, value.source.source_id, trust_name(value.launch.trust_mode), "credentialless_stdio", value.launch.executable_identity, value.tools.policy_version, value.tools.selected_tools, value.revoked, value.revoked ? "revoked" : "active", value.revoked ? "revoked" : ""});
    return result;
}
std::map<std::string, std::shared_ptr<MCPClient>> HardenedMcpClientRegistry::clients() const { std::lock_guard lk(impl_->mutex); std::map<std::string, std::shared_ptr<MCPClient>> result; for (const auto& [name, value] : impl_->adopted) if (!value.revoked && value.client) result.emplace(name, value.client); return result; }

HarnessCapabilityExecutor HardenedMcpClientRegistry::capability_executor() const {
    return [this](const json& tool, const json& arguments, const std::shared_ptr<graph::CancelToken>& cancel) {
        if (cancel) cancel->throw_if_cancelled("before adopted MCP call");
        const auto server = tool.value("executor", json::object()).value("server_ref", "");
        const auto name = tool.value("executor", json::object()).value("tool", tool.value("id", ""));
        std::shared_ptr<MCPClient> client;
        McpCapabilityManifest manifest;
        std::string expected_schema;
        { std::lock_guard lk(impl_->mutex); auto it = impl_->adopted.find(server); if (it == impl_->adopted.end() || it->second.revoked || !it->second.client) throw std::runtime_error("adopted MCP server is unavailable"); auto schema = it->second.schemas.find(name); if (schema == it->second.schemas.end()) throw std::runtime_error("MCP tool is not selected or schema is stale"); expected_schema = schema->second; client = it->second.client; for (const auto& m : it->second.tools.manifest) if (m.tool == name) manifest = m; }
        bool schema_current = false;
        for (const auto& definition : client->get_tool_definitions())
            if (definition.name == name && mcp_tool_schema_hash(definition.to_json()) == expected_schema) { schema_current = true; break; }
        if (!schema_current) throw std::runtime_error("MCP tool schema changed or list_changed; re-adoption required");
        if (!manifest.auto_execute || forbidden_effects(manifest)) throw std::runtime_error("MCP capability manifest denies tool");
        if (!arguments.is_object()) throw std::runtime_error("MCP tool arguments must be an object");
        if (manifest.argument_predicate.is_object() && manifest.argument_predicate.contains("allowed_keys")) {
            const auto& allowed = manifest.argument_predicate.at("allowed_keys");
            if (!allowed.is_array()) throw std::runtime_error("MCP argument predicate is invalid");
            for (auto it = arguments.begin(); it != arguments.end(); ++it) {
                bool found = false;
                for (const auto& key : allowed)
                    if (key.is_string() && key.get<std::string>() == it.key()) found = true;
                if (!found) throw std::runtime_error("MCP argument predicate denied an argument");
            }
        }
        if (cancel) cancel->throw_if_cancelled("before adopted MCP dispatch");
        const auto result = client->call_tool_result(name, arguments);
        if (cancel) cancel->throw_if_cancelled("after adopted MCP dispatch");
        if (result.is_error) throw std::runtime_error("adopted MCP tool returned an error");
        return result.structured_content.is_null() ? result.to_json() : result.structured_content;
    };
}

} // namespace neograph::mcp
