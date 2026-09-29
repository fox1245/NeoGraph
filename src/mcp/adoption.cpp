#include <neograph/mcp/adoption.h>

#include <neograph/mcp/client.h>
#include <neograph/mcp/json_schema.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include "../core/sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

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
            if (unsafe_key(it.key())) return true;
            if (contains_external_ref(it.value())) return true;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) if (contains_external_ref(item)) return true;
    }
    else if (value.is_string()) {
        const auto text = value.get<std::string>();
        return text.find("env:") != std::string::npos ||
               text.find("file:") != std::string::npos;
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
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE"); home && *home)
        return std::filesystem::path(home) / ".config" / "opencode" / "opencode.json";
#endif
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

std::string resolve_executable(const std::vector<std::string>& argv,
                               const std::filesystem::path& cwd) {
    if (argv.empty() || argv.front().empty()) throw std::invalid_argument("MCP command is empty");
    const auto command = std::filesystem::path(argv.front());
    auto base = command.stem().string();
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (base == "sh" || base == "bash" || base == "zsh" || base == "fish" ||
        base == "cmd" || base == "powershell" || base == "pwsh")
        throw std::invalid_argument("shell executors are not supported");
    auto resolve = [](const std::filesystem::path& candidate) -> std::string {
        std::error_code ec;
        const auto resolved = std::filesystem::canonical(candidate, ec);
        if (ec || !std::filesystem::is_regular_file(resolved)) return {};
#ifndef _WIN32
        if (::access(resolved.c_str(), X_OK) != 0) return {};
#else
        DWORD type = 0;
        if (!::GetBinaryTypeW(resolved.c_str(), &type)) return {};
#endif
        return resolved.string();
    };
    if (command.has_parent_path()) {
        const auto resolved = resolve(command.is_absolute() ? command : cwd / command);
        if (resolved.empty()) throw std::invalid_argument("MCP executable is not an executable regular file");
        return resolved;
    }
    const char* raw = std::getenv("PATH");
    const std::string path = raw ? raw : "";
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(separator, start);
        const auto dir = std::filesystem::path(path.substr(start, end == std::string::npos ? end : end - start));
        if (dir.is_absolute()) {
            auto candidate = dir / command;
#ifdef _WIN32
            if (!candidate.has_extension()) candidate += ".exe";
#endif
            if (auto resolved = resolve(candidate); !resolved.empty()) return resolved;
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

std::string script_identity(const std::string& executable,
                            const std::vector<std::string>& argv,
                            const std::filesystem::path& cwd, McpTrustMode mode) {
    if (mode == McpTrustMode::trusted_mutable) return {};
    const auto name = std::filesystem::path(executable).stem().string();
    const bool interpreter = name.rfind("python", 0) == 0 || name == "node" ||
                             name == "ruby" || name == "perl";
    if (interpreter) {
        if (argv.size() < 2 || argv[1].empty() || argv[1][0] == '-')
            throw std::invalid_argument("unpinned interpreter/package launch requires explicit trusted_mutable consent");
        const auto script = std::filesystem::canonical(cwd / argv[1]);
        if (!std::filesystem::is_regular_file(script))
            throw std::invalid_argument("pinned interpreter requires a regular script");
        return executable_identity(script.string(), {}, {});
    }
    std::ifstream input(executable, std::ios::binary);
    char magic[4]{};
    input.read(magic, sizeof(magic));
    if (!((magic[0] == '\x7f' && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') ||
          (magic[0] == 'M' && magic[1] == 'Z') ||
          static_cast<unsigned char>(magic[0]) == 0xcf ||
          static_cast<unsigned char>(magic[0]) == 0xfe))
        throw std::invalid_argument("unverifiable script/package launcher requires explicit trusted_mutable consent");
    if (name == "npx" || name == "npm" || name == "bun" || name == "deno" ||
        name == "uv" || name == "java" || name == "dotnet")
        throw std::invalid_argument("unpinned package launch requires explicit trusted_mutable consent");
    return {};
}

std::vector<ToolDefinition> list_definitions(
    const std::shared_ptr<MCPClient>& client,
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<graph::CancelToken>& cancel = {}) {
    std::vector<ToolDefinition> definitions;
    json params = json::object();
    std::set<std::string> cursors, names;
    for (std::size_t page_count = 0; page_count < 256; ++page_count) {
        auto page = ListToolsPage::from_json(async::run_sync(
            client->rpc_call_async("tools/list", params, deadline, cancel)));
        for (auto& definition : page.tools) {
            if (!names.insert(definition.name).second)
                throw std::runtime_error("MCP tools/list contains duplicate tool names");
            definitions.push_back(std::move(definition));
        }
        if (!page.next_cursor) return definitions;
        if (!cursors.insert(*page.next_cursor).second)
            throw std::runtime_error("MCP tools/list repeated a cursor");
        params["cursor"] = *page.next_cursor;
    }
    throw std::runtime_error("MCP tools/list exceeded page limit");
}

void validate_manifest(const McpCapabilityManifest& manifest) {
    static const std::set<std::string> effects = {"data_read", "network_egress", "nondeterminism", "cost"};
    if (manifest.tool.empty() || !manifest.auto_execute || manifest.effects.empty())
        throw std::invalid_argument("MCP capability manifest requires approved effects");
    for (const auto& effect : manifest.effects)
        if (!effects.count(effect)) throw std::invalid_argument("MCP manifest contains an unsupported effect");
    const auto& predicate = manifest.argument_predicate;
    if (manifest.argument_policy != "exact-arguments-v1" || !predicate.is_object() ||
        predicate.size() != 1 || !predicate.contains("allowed_arguments") ||
        !predicate.at("allowed_arguments").is_array() || predicate.at("allowed_arguments").empty())
        throw std::invalid_argument("MCP requires a host-approved exact-arguments-v1 value allowlist");
    for (const auto& arguments : predicate.at("allowed_arguments"))
        if (!arguments.is_object()) throw std::invalid_argument("MCP argument allowlist entries must be objects");
}

std::string tool_id(const std::string& server, const std::string& tool) {
    return "mcp." + server + "." + tool;
}

std::string schema_digest(const ToolDefinition& definition) {
    return digest(definition.to_json().dump());
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
    std::string state = "active";
    std::map<std::string, ToolDefinition> definitions;
};

} // namespace

std::string mcp_tool_schema_hash(const json& definition) { return digest(definition.dump()); }

McpLaunchApproval make_mcp_launch_approval(const DiscoveredMcpServer& server,
                                           McpTrustMode mode,
                                           bool argv_no_credentials_attested) {
    if (!argv_no_credentials_attested) throw std::invalid_argument("explicit argv-no-credential attestation is required");
    const auto cwd = std::filesystem::canonical(server.cwd);
    const auto executable = resolve_executable(server.argv, cwd);
    McpLaunchApproval approval;
    approval.trust_mode = mode;
    approval.argv_no_credentials_attested = true;
    approval.source_path = std::filesystem::canonical(server.source_path).string();
    approval.source_content_hash = server.source_content_hash;
    approval.cwd = cwd.string();
    approval.executable = executable;
    approval.interpreter_identity = executable_identity(executable, {}, {});
    approval.package_identity = script_identity(executable, server.argv, cwd, mode);
    approval.executable_identity = digest(executable_identity(executable, server.argv, cwd) +
                                          approval.package_identity);
    approval.argv_hash = digest(json(server.argv).dump());
    return approval;
}

McpDiscoveryReport OpenCodeGlobalMcpDiscovery::discover(const OpenCodeGlobalMcpConfig& config) {
    if (!config.single_user_local) throw std::invalid_argument("global MCP adoption requires single-user local mode");
    if (std::getenv("NEOGRAPH_HARNESS_HOST_DEPTH"))
        throw std::invalid_argument("nested NeoGraph Harness adoption is disabled");
    const auto path = config.source_path.empty() ? default_source() : config.source_path;
    McpDiscoveryReport report; report.source_path = path;
    std::error_code location_error;
    if (std::filesystem::absolute(path).lexically_normal() !=
        std::filesystem::absolute(default_source()).lexically_normal()) {
        report.rejected.push_back({{}, "source is not the documented user-global OpenCode source"});
        return report;
    }
    std::string reason;
    if (!regular_user_file(path, reason)) { report.rejected.push_back({{}, reason}); return report; }
    const auto size = std::filesystem::file_size(path, location_error);
    if (location_error || size > config.max_source_bytes) {
        report.rejected.push_back({{}, "source exceeds size limit or cannot be read"}); return report;
    }
    std::ifstream input(path, std::ios::binary);
    std::string text(config.max_source_bytes + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
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
            const auto safe_name = [](unsigned char c) {
                return std::isalnum(c) || c == '_' || c == '-';
            };
            if (name.empty() || !std::all_of(name.begin(), name.end(), safe_name))
                throw std::invalid_argument("server name is not safe for a namespaced capability");
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
    std::map<std::string, std::uint64_t> generations;
    bool closed = false;
};

HardenedMcpClientRegistry::HardenedMcpClientRegistry(std::string policy_version)
    : impl_(std::make_shared<Impl>()) {
    if (policy_version.empty()) throw std::invalid_argument("MCP policy version is required");
    impl_->policy_version = std::move(policy_version);
}
HardenedMcpClientRegistry::~HardenedMcpClientRegistry() {
    std::lock_guard lk(impl_->mutex);
    impl_->closed = true;
    for (auto& [name, value] : impl_->adopted) {
        value.revoked = true;
        value.state = "revoked";
        value.client->shutdown();
    }
}

void HardenedMcpClientRegistry::adopt(const McpAdoptionRequest& request) {
    const auto& source = request.server;
    const auto& launch = request.launch;
    const auto& approval = request.tools;
    if (!source.enabled || source.source_host != "opencode" || source.source_scope != "user-global" ||
        source.server_name.empty() || source.argv.empty())
        throw std::invalid_argument("MCP source descriptor is not adoptable");
    if (!launch.argv_no_credentials_attested)
        throw std::invalid_argument("explicit argv-no-credential attestation is required");
    if (launch.trust_mode != McpTrustMode::pinned && launch.trust_mode != McpTrustMode::trusted_mutable)
        throw std::invalid_argument("unknown MCP trust mode");
    OpenCodeGlobalMcpConfig discovery;
    discovery.source_path = source.source_path;
    const auto report = OpenCodeGlobalMcpDiscovery::discover(discovery);
    const auto fresh = std::find_if(report.servers.begin(), report.servers.end(),
        [&](const auto& value) { return value.server_name == source.server_name; });
    if (fresh == report.servers.end() || fresh->argv != source.argv || fresh->cwd != source.cwd ||
        fresh->source_content_hash != source.source_content_hash)
        throw std::invalid_argument("MCP source changed or is outside the global boundary");
    const auto current = make_mcp_launch_approval(source, launch.trust_mode, true);
    if (launch.source_path != current.source_path || launch.cwd != current.cwd ||
        launch.executable != current.executable || launch.argv_hash != current.argv_hash ||
        launch.source_content_hash != current.source_content_hash)
        throw std::invalid_argument("MCP launch configuration does not match explicit approval");
    if (launch.executable_identity.empty() || (launch.trust_mode == McpTrustMode::pinned &&
        (launch.executable_identity != current.executable_identity ||
         launch.interpreter_identity != current.interpreter_identity ||
         launch.package_identity != current.package_identity)))
        throw std::invalid_argument("MCP pinned executable or script identity changed");
    if (approval.server_name != source.server_name ||
        approval.launch_identity != launch.executable_identity ||
        approval.policy_version != impl_->policy_version || approval.selected_tools.empty())
        throw std::invalid_argument("MCP tool approval does not bind server, launch, and policy");
    std::set<std::string> selected;
    for (const auto& name : approval.selected_tools)
        if (name.empty() || !selected.insert(name).second)
            throw std::invalid_argument("MCP selected tools must be unique");
    if (approval.manifest.size() != selected.size() || approval.schema_hashes.size() != selected.size())
        throw std::invalid_argument("MCP approval must contain exact selected tool schemas and manifests");
    std::set<std::string> manifested;
    for (const auto& manifest : approval.manifest) {
        validate_manifest(manifest);
        if (!selected.count(manifest.tool) || !manifested.insert(manifest.tool).second ||
            !approval.schema_hashes.count(manifest.tool))
            throw std::invalid_argument("MCP manifest selection mismatch");
    }
    if (std::filesystem::path(current.executable).filename().string().find("neograph-harness") != std::string::npos)
        throw std::invalid_argument("recursive NeoGraph Harness executable is not adoptable");
    StdioClientConfig stdio;
    stdio.argv = source.argv;
    stdio.argv.front() = current.executable;
    stdio.cwd = current.cwd;
    stdio.environment = {{"LANG", "C"}, {"LC_ALL", "C"},
                         {"NEOGRAPH_MCP_ADOPTED", "1"}, {"NEOGRAPH_HARNESS_HOST_DEPTH", "1"}};
#ifdef _WIN32
    wchar_t windows[MAX_PATH]{};
    const auto length = ::GetWindowsDirectoryW(windows, MAX_PATH);
    if (!length || length >= MAX_PATH) throw std::runtime_error("cannot resolve Windows system directory");
    const auto root = std::filesystem::path(windows);
    stdio.environment.emplace_back("SystemRoot", root.string());
    stdio.environment.emplace_back("WINDIR", root.string());
    stdio.environment.emplace_back("PATH", (root / "System32").string());
#else
    stdio.environment.emplace_back("PATH", "/usr/bin:/bin");
#endif
    stdio.replace_environment = true;
    stdio.startup_timeout = source.startup_timeout;
    std::uint64_t generation;
    {
        std::lock_guard lk(impl_->mutex);
        if (impl_->closed) throw std::runtime_error("MCP registry is closed");
        generation = ++impl_->generations[source.server_name];
        const auto old = impl_->adopted.find(source.server_name);
        if (old != impl_->adopted.end()) {
            old->second.revoked = true;
            old->second.state = "revoked";
            old->second.client->shutdown();
        }
    }
    auto client = std::make_shared<MCPClient>(std::move(stdio));
    try {
        client->initialize("neograph-adopted-local");
        Adopted value;
        value.source = source;
        value.launch = launch;
        value.tools = approval;
        value.client = client;
        for (auto& definition : list_definitions(client, std::chrono::steady_clock::now() + source.startup_timeout)) {
            const auto name = definition.name;
            if (!selected.count(name)) continue;
            if (approval.schema_hashes.at(name) != schema_digest(definition))
                throw std::invalid_argument("MCP tool schema changed or is not approved: " + name);
            value.schemas.emplace(name, approval.schema_hashes.at(name));
            value.definitions.emplace(name, std::move(definition));
        }
        if (value.schemas.size() != selected.size())
            throw std::invalid_argument("selected MCP tool was not reported by server");
        std::lock_guard lk(impl_->mutex);
        if (impl_->closed || impl_->generations[source.server_name] != generation)
            throw std::runtime_error("MCP adoption was revoked during inspection");
        impl_->adopted[source.server_name] = std::move(value);
    } catch (...) {
        client->shutdown();
        throw;
    }
}

void HardenedMcpClientRegistry::revoke(const std::string& server_name) {
    std::lock_guard lk(impl_->mutex);
    ++impl_->generations[server_name];
    const auto it = impl_->adopted.find(server_name);
    if (it == impl_->adopted.end()) return;
    it->second.revoked = true;
    it->second.state = "revoked";
    it->second.client->shutdown();
}
bool HardenedMcpClientRegistry::contains(const std::string& server_name) const { std::lock_guard lk(impl_->mutex); auto it = impl_->adopted.find(server_name); return it != impl_->adopted.end() && !it->second.revoked && it->second.client; }

std::vector<McpAdoptionStatus> HardenedMcpClientRegistry::status() const {
    std::lock_guard lk(impl_->mutex); std::vector<McpAdoptionStatus> result;
    for (const auto& [name, value] : impl_->adopted) result.push_back({name, value.source.source_id, trust_name(value.launch.trust_mode), "credentialless_stdio", value.launch.executable_identity, value.tools.policy_version, value.tools.selected_tools, value.revoked, value.state, value.revoked ? value.state : ""});
    return result;
}
std::map<std::string, std::shared_ptr<MCPClient>> HardenedMcpClientRegistry::clients() const { std::lock_guard lk(impl_->mutex); std::map<std::string, std::shared_ptr<MCPClient>> result; for (const auto& [name, value] : impl_->adopted) if (!value.revoked && value.client) result.emplace(name, value.client); return result; }

HarnessCapabilityExecutor HardenedMcpClientRegistry::capability_executor() const {
    return [state = impl_, generations = clients()](const json& tool, const json& arguments,
                          const std::shared_ptr<graph::CancelToken>& cancel) {
        if (cancel) cancel->throw_if_cancelled("before adopted MCP call");
        const auto executor = tool.value("executor", json::object());
        const auto server = executor.value("server_ref", "");
        const auto name = executor.value("tool", "");
        if (executor.value("kind", "") != "mcp" || tool.value("id", "") != tool_id(server, name))
            throw std::runtime_error("MCP capability is not the approved namespaced tool");
        std::shared_ptr<MCPClient> client;
        McpCapabilityManifest manifest;
        std::map<std::string, std::string> expected_schemas;
        ToolDefinition selected_definition;
        {
            std::lock_guard lk(state->mutex);
            const auto it = state->adopted.find(server);
            if (state->closed || it == state->adopted.end() || it->second.revoked ||
                !generations.count(server) || generations.at(server) != it->second.client)
                throw std::runtime_error("adopted MCP server is unavailable");
            if (!it->second.schemas.count(name)) throw std::runtime_error("MCP tool is not selected");
            client = it->second.client;
            selected_definition = it->second.definitions.at(name);
            expected_schemas = it->second.schemas;
            for (const auto& value : it->second.tools.manifest)
                if (value.tool == name) manifest = value;
        }
        validate_manifest(manifest);
        bool allowed = false;
        for (const auto& candidate : manifest.argument_predicate.at("allowed_arguments"))
            if (arguments == candidate) { allowed = true; break; }
        if (!allowed) throw std::runtime_error("MCP exact argument value policy denied dispatch");
        validate_json_value(arguments, selected_definition.input_schema, "MCP tool arguments");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const auto definitions = list_definitions(client, deadline, cancel);
        std::map<std::string, std::string> current_schemas;
        for (const auto& definition : definitions)
            if (expected_schemas.count(definition.name))
                current_schemas.emplace(definition.name, schema_digest(definition));
        if (current_schemas != expected_schemas) {
            std::lock_guard lk(state->mutex);
            const auto it = state->adopted.find(server);
            if (it != state->adopted.end() && it->second.client == client) {
                it->second.revoked = true;
                it->second.state = "stale";
            }
            client->shutdown();
            throw std::runtime_error("MCP tool schema changed; re-adoption required");
        }
        {
            std::lock_guard lk(state->mutex);
            const auto it = state->adopted.find(server);
            if (state->closed || it == state->adopted.end() || it->second.revoked ||
                it->second.client != client)
                throw std::runtime_error("MCP adoption generation was revoked");
        }
        if (cancel) cancel->throw_if_cancelled("before adopted MCP dispatch");
        const auto result = CallToolResult::from_json(async::run_sync(client->rpc_call_async(
            "tools/call", {{"name", name}, {"arguments", arguments}}, deadline, cancel)));
        if (cancel) cancel->throw_if_cancelled("after adopted MCP dispatch");
        if (result.is_error) throw std::runtime_error("adopted MCP tool returned an error");
        if (!selected_definition.output_schema.is_null())
            validate_json_value(result.structured_content, selected_definition.output_schema,
                                "MCP structuredContent");
        return result.structured_content.is_null() ? result.to_json() : result.structured_content;
    };
}

json HardenedMcpClientRegistry::tool_catalog() const {
    std::lock_guard lk(impl_->mutex);
    json catalog = json::array();
    for (const auto& [server, value] : impl_->adopted) {
        if (value.revoked || impl_->closed) continue;
        for (const auto& [name, definition] : value.definitions) {
            catalog.push_back({{"id", tool_id(server, name)}, {"description", definition.description},
                {"input_schema", definition.input_schema},
                {"output_schema", definition.output_schema.is_null() ? json{{"type", "object"}} : definition.output_schema},
                {"read_only", true}, {"executor", {{"kind", "mcp"}, {"server_ref", server}, {"tool", name}}}});
        }
    }
    return catalog;
}

void HardenedMcpClientRegistry::configure_harness(HarnessProgramHostConfig& host,
                                                  HarnessProviderExecutorConfig& provider) const {
    auto execute = capability_executor();
    std::lock_guard lk(impl_->mutex);
    if (impl_->closed) throw std::runtime_error("MCP registry is closed");
    for (const auto& [server, value] : impl_->adopted) {
        if (value.revoked) continue;
        for (const auto& manifest : value.tools.manifest) {
            const auto& definition = value.definitions.at(manifest.tool);
            const auto id = tool_id(server, manifest.tool);
            const auto binding = digest(json{{"launch", value.tools.launch_identity},
                {"schema", value.schemas.at(manifest.tool)}, {"policy", value.tools.policy_version},
                {"argument_policy", manifest.argument_policy}, {"predicate", manifest.argument_predicate},
                {"effects", std::vector<std::string>(manifest.effects.begin(), manifest.effects.end())}}.dump());
            program::ExecutableIdentity identity{program::ExecutableKind::Tool, id, "1.0.0", binding};
            host.snapshots.registry.tools.push_back({{identity, program::EffectMode::Brokered,
                "adopted-local-mcp", {"tool.invoke"}, {manifest.effects.begin(), manifest.effects.end()}, {}},
                {definition.input_schema,
                 definition.output_schema.is_null() ? json{{"type", "object"}} : definition.output_schema}});
            host.snapshots.allowed_module_digests.push_back(binding);
            host.snapshots.allowed_capabilities.push_back("tool.invoke");
            for (const auto& effect : manifest.effects) host.snapshots.allowed_effects.push_back(effect);
            host.tool_binding_identities.emplace(id, binding);
        }
    }
    host.capability_executor = std::move(execute);
    provider.capability_executor = host.capability_executor;
}

} // namespace neograph::mcp
