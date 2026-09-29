#pragma once

#include <neograph/mcp/harness.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace neograph::mcp {

/** Local CLI delegation: authentication remains exclusively inside the installed host. */
struct HostAgentExecutorConfig {
    std::string host;  // opencode, claude, or codex; never auto-select
    std::string executable; // optional absolute override (also useful for fixture CLIs)
    std::string model; // empty means the selected host's default
    std::filesystem::path workspace;
    std::chrono::milliseconds startup_timeout{10000};
    std::chrono::milliseconds request_timeout{120000};
    std::size_t max_prompt_bytes = 65536;
    std::size_t max_output_bytes = 1048576;
    std::size_t max_events = 4096;
};

/** Non-secret status; detail is an actionable diagnostic, never host output or tokens. */
struct HostAgentPreflight {
    bool available = false;
    std::string host;
    std::string executable;
    std::string version;
    std::string model;
    std::string detail;
    bool openai_oauth = false; // OpenCode only: reported by `opencode auth list`.
};

/** Run before compile/start. No authentication files or keychain entries are opened. */
NEOGRAPH_HARNESS_API HostAgentPreflight preflight_host_agent(const HostAgentExecutorConfig& config);
NEOGRAPH_HARNESS_API HarnessWorkerExecutor make_host_agent_executor(HostAgentExecutorConfig config);

} // namespace neograph::mcp
