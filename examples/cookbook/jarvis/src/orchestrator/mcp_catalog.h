// jarvis/src/orchestrator/mcp_catalog.h
//
// Reads config/mcp_catalog.json and builds the MCP clients and tool inventory.
// Also renders the "available tools" section injected into the router's system prompt.
//
// The catalog is not just a static tool list — it defines how JARVIS perceives
// its own capabilities; adding a JSON entry makes a new capability available at once.
//
// Each MCP server is connected once at startup and its get_tools() result is cached.
// Only enabled=true entries are merged and shared across the router, direct, and parallel nodes.
#pragma once

#include <neograph/neograph.h>
#include <neograph/mcp/client.h>

#include <memory>
#include <string>
#include <vector>

namespace jarvis::orchestrator {

struct CatalogEntry {
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    bool skip_synthesis_hint = false;
    std::unique_ptr<neograph::mcp::MCPClient> client;
    std::vector<std::unique_ptr<neograph::Tool>> tools;  // cached get_tools() result
};

class McpCatalog {
  public:
    // catalog_json_path = "config/mcp_catalog.json"
    static McpCatalog load(const std::string& catalog_json_path);

    // Text injected into the router LLM prompt — tool name + 1-line description + tags.
    std::string render_for_router_prompt() const;

    // Look up a tool by name for direct_branch / parallel_branch.
    neograph::Tool* find_tool(const std::string& fully_qualified_name) const;

    // Whether the tool's catalog entry permits skip_synthesis.
    // Used by the router's post-LLM enforcement rule to prevent raw output from
    // hint=false tools (such as unformatted ISO timestamps) from going straight
    // to TTS. Returns true for unknown tools (respecting the LLM decision).
    bool allows_skip_synthesis(const std::string& fully_qualified_name) const;

    // Inject the routing_hints section (if present) into the router prompt.
    std::string routing_hints_text() const;

    const std::vector<CatalogEntry>& entries() const { return entries_; }

  private:
    std::vector<CatalogEntry> entries_;
    std::string routing_hints_;
};

}  // namespace jarvis::orchestrator
