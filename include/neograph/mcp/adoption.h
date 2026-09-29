#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/mcp/harness.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace neograph::mcp {
class MCPClient;
/** The only credential mode implemented by the local OpenCode adapter. */
enum class McpCredentialMode { credentialless_stdio };
/** Launch identity policy. pinned is the safe default. */
enum class McpTrustMode { pinned, trusted_mutable };

struct NEOGRAPH_API OpenCodeGlobalMcpConfig {
    /// Empty selects the documented global source; explicit paths must name that same file.
    std::filesystem::path source_path;
    std::size_t max_source_bytes = 1024 * 1024;
    bool single_user_local = true;
};

struct NEOGRAPH_API DiscoveredMcpServer {
    std::string source_host = "opencode";
    std::string source_scope = "user-global";
    std::string source_id;
    std::string server_name;
    std::filesystem::path source_path;
    std::filesystem::path cwd;
    std::vector<std::string> argv; // in-memory only; status() never returns it
    std::chrono::milliseconds startup_timeout{10000};
    std::string source_content_hash;
    std::string redacted_config_hash;
    McpCredentialMode credential_mode = McpCredentialMode::credentialless_stdio;
    bool enabled = false;
};

struct NEOGRAPH_API DiscoveryRejection {
    std::string server_name;
    std::string reason;
};
struct NEOGRAPH_API McpDiscoveryReport {
    std::filesystem::path source_path;
    std::string source_content_hash;
    std::vector<DiscoveredMcpServer> servers;
    std::vector<DiscoveryRejection> rejected;
};

/** Read only OpenCode's user-global raw source. It never starts a process. */
class NEOGRAPH_API OpenCodeGlobalMcpDiscovery final {
public:
    static McpDiscoveryReport discover(const OpenCodeGlobalMcpConfig& config = {});
};

struct NEOGRAPH_API McpCapabilityManifest {
    std::string tool;
    bool auto_execute = false;
    std::set<std::string> effects;
    std::string argument_policy;
    /// exact-arguments-v1: {"allowed_arguments":[<complete argument object>, ...]}.
    /// Values are host-approved, not inferred safe from tool annotations or SQL text.
    json argument_predicate = json::object();
};

struct NEOGRAPH_API McpLaunchApproval {
    McpTrustMode trust_mode = McpTrustMode::pinned;
    bool argv_no_credentials_attested = false;
    std::string source_path;
    std::string source_content_hash;
    std::string cwd;
    std::string executable;
    std::string executable_identity;
    std::string argv_hash;
    std::string interpreter_identity;
    std::string package_identity;
};

struct NEOGRAPH_API McpToolApproval {
    std::string server_name;
    std::string launch_identity;
    std::vector<std::string> selected_tools;
    std::map<std::string, std::string> schema_hashes;
    std::vector<McpCapabilityManifest> manifest;
    std::string policy_version;
};

struct NEOGRAPH_API McpAdoptionRequest {
    DiscoveredMcpServer server;
    McpLaunchApproval launch;
    McpToolApproval tools;
};


/** Resolve and fingerprint a descriptor without launching its server. */
NEOGRAPH_API McpLaunchApproval make_mcp_launch_approval(
    const DiscoveredMcpServer& server, McpTrustMode mode,
    bool argv_no_credentials_attested);
/** Stable digest used when recording a selected ToolDefinition schema. */
NEOGRAPH_API std::string mcp_tool_schema_hash(const json& definition);
/** Metadata-safe adoption record; argv and raw config are intentionally absent. */
struct NEOGRAPH_API McpAdoptionStatus {
    std::string server_name;
    std::string source_id;
    std::string trust_mode;
    std::string credential_mode = "credentialless_stdio";
    std::string launch_identity;
    std::string policy_version;
    std::vector<std::string> selected_tools;
    bool revoked = false;
    std::string state;
    std::string reason;
};

/** Owns initialized, policy-bound clients and drains them on revocation. */
class NEOGRAPH_API HardenedMcpClientRegistry final {
public:
    explicit HardenedMcpClientRegistry(std::string policy_version = "mcp-local-v1");
    ~HardenedMcpClientRegistry();
    HardenedMcpClientRegistry(const HardenedMcpClientRegistry&) = delete;

    /// Approves, inspects, initializes and lists tools; never accepts implicit consent.
    void adopt(const McpAdoptionRequest& request);
    void revoke(const std::string& server_name);
    bool contains(const std::string& server_name) const;
    std::vector<McpAdoptionStatus> status() const;
    std::map<std::string, std::shared_ptr<MCPClient>> clients() const;
    HarnessCapabilityExecutor capability_executor() const;
    /// Immutable namespaced metadata for constructing Harness requests.
    json tool_catalog() const;
    /// Install approved metadata/bindings before constructing the host and provider worker.
    void configure_harness(HarnessProgramHostConfig& host,
                           HarnessProviderExecutorConfig& provider) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace neograph::mcp
