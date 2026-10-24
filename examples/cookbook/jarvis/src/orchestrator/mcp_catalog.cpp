// jarvis/src/orchestrator/mcp_catalog.cpp
//
// McpCatalog implementation — reads a JSON catalog file, creates MCP clients,
// and caches the tool list from each server.
// Also renders the text block injected into the router LLM system prompt.

#include "mcp_catalog.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace jarvis::orchestrator {

// ---------------------------------------------------------------------------
// load() — parse JSON file + connect MCP clients + cache tools
// ---------------------------------------------------------------------------
McpCatalog McpCatalog::load(const std::string& catalog_json_path) {
    // Read JSON file
    std::ifstream ifs(catalog_json_path);
    if (!ifs.is_open()) {
        throw std::runtime_error(
            "[McpCatalog] 카탈로그 파일을 열 수 없습니다: " + catalog_json_path);
    }

    neograph::json catalog;
    try {
        catalog = neograph::json::parse(ifs);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("[McpCatalog] JSON 파싱 실패: ") + e.what());
    }

    McpCatalog result;

    // ---------------------------------------------------------------------------
    // Process the "tools" array — enabled=true entries only
    // ---------------------------------------------------------------------------
    if (!catalog.contains("tools") || !catalog["tools"].is_array()) {
        // Return an empty catalog if the tools key is absent (not an error)
        return result;
    }

    for (const auto& item : catalog["tools"]) {
        // Skip entries with enabled=false
        bool enabled = item.value("enabled", true);
        if (!enabled) {
            continue;
        }

        const std::string name        = item.value("name", "");
        const std::string transport   = item.value("transport", "http");
        const std::string description = item.value("description", "");

        // Collect tags array
        std::vector<std::string> tags;
        if (item.contains("tags") && item["tags"].is_array()) {
            for (const auto& tag : item["tags"]) {
                tags.push_back(tag.get<std::string>());
            }
        }

        bool skip_synthesis = item.value("skip_synthesis_hint", false);

        CatalogEntry entry;
        entry.name              = name;
        entry.description       = description;
        entry.tags              = std::move(tags);
        entry.skip_synthesis_hint = skip_synthesis;

        // ---------------------------------------------------------------------------
        // Create MCPClient by transport type and fetch its tool list.
        // On failure, log and skip this entry while continuing with the rest.
        // ---------------------------------------------------------------------------
        try {
            if (transport == "stdio") {
                // Requires a "command" array (e.g. ["python3", "server.py"])
                if (!item.contains("command") || !item["command"].is_array()) {
                    std::cerr << "[McpCatalog] '" << name
                              << "': stdio 트랜스포트에 'command' 배열이 없습니다. 건너뜀.\n";
                    continue;
                }

                std::vector<std::string> cmd =
                    item["command"].get<std::vector<std::string>>();

                std::cerr << "[McpCatalog] '" << name << "' stdio 프로세스 시작: ";
                for (size_t i = 0; i < cmd.size(); ++i) {
                    std::cerr << cmd[i];
                    if (i + 1 < cmd.size()) std::cerr << " ";
                }
                std::cerr << "\n";

                entry.client =
                    std::make_unique<neograph::mcp::MCPClient>(std::move(cmd));

            } else {
                // Default = HTTP transport
                const std::string url = item.value("url", "");
                if (url.empty()) {
                    std::cerr << "[McpCatalog] '" << name
                              << "': http 트랜스포트에 'url' 필드가 없습니다. 건너뜀.\n";
                    continue;
                }

                std::cerr << "[McpCatalog] '" << name
                          << "' HTTP 연결: " << url << "\n";

                entry.client =
                    std::make_unique<neograph::mcp::MCPClient>(url);
            }

            // Fetch and cache the tool list
            entry.tools = entry.client->get_tools();

            std::cerr << "[McpCatalog] '" << name << "' 도구 "
                      << entry.tools.size() << "개 캐시 완료.\n";

        } catch (const std::exception& e) {
            std::cerr << "[McpCatalog] '" << name
                      << "' 연결/도구 수집 실패 — 건너뜀: " << e.what() << "\n";
            continue;
        }

        result.entries_.push_back(std::move(entry));
    }

    // ---------------------------------------------------------------------------
    // "routing_hints" section — format and store as text when present
    // ---------------------------------------------------------------------------
    if (catalog.contains("routing_hints") && catalog["routing_hints"].is_object()) {
        const auto& hints = catalog["routing_hints"];
        std::ostringstream oss;

        // Format each hint key as a bulleted list
        auto append_hint_list = [&](const std::string& key, const std::string& label) {
            if (!hints.contains(key)) return;
            const auto& arr = hints[key];
            if (!arr.is_array()) return;
            oss << label << ":\n";
            for (const auto& line : arr) {
                oss << "  - " << line.get<std::string>() << "\n";
            }
        };

        append_hint_list("direct_when",   "direct_when");
        append_hint_list("parallel_when", "parallel_when");
        append_hint_list("delegate_when", "delegate_when");

        result.routing_hints_ = oss.str();
    }

    return result;
}

// ---------------------------------------------------------------------------
// render_for_router_prompt() — render text for the router LLM system prompt
//
// Format:
//   ## Available MCP tools
//
//   ### {catalog_name}  [{tags}] (skip_synthesis ok)
//   {description}
//     - {catalog_name}.{tool_name}: {tool_description}
//     ...
//
//   ## Routing hints
//   {routing_hints_}
// ---------------------------------------------------------------------------
std::string McpCatalog::render_for_router_prompt() const {
    std::ostringstream oss;
    oss << "## Available MCP tools\n";

    for (const auto& entry : entries_) {
        oss << "\n### " << entry.name;

        // Render tags
        if (!entry.tags.empty()) {
            oss << "  [";
            for (size_t i = 0; i < entry.tags.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << entry.tags[i];
            }
            oss << "]";
        }

        if (entry.skip_synthesis_hint) {
            oss << " (skip_synthesis ok)";
        }
        oss << "\n";

        // Server-level description
        if (!entry.description.empty()) {
            oss << entry.description << "\n";
        }

        // Individual tools — fully qualified as "catalog_name.tool_name".
        // Always render the parameter schema (def.parameters): without it, the
        // LLM does not know how to populate args, calls with empty args ({}),
        // and the server rejects the call with "missing required argument".
        for (const auto& tool : entry.tools) {
            const auto def = tool->get_definition();
            oss << "  - " << entry.name << "." << def.name;
            if (!def.description.empty()) {
                oss << ": " << def.description;
            }
            // JSON Schema properties → "args: {city: string [required], ...}"
            const auto& p = def.parameters;
            if (p.is_object() && p.contains("properties") &&
                p["properties"].is_object() && !p["properties"].empty()) {
                std::vector<std::string> req;
                if (p.contains("required") && p["required"].is_array())
                    for (const auto& r : p["required"])
                        if (r.is_string()) req.push_back(r.get<std::string>());
                // Expose name, type, required flag, and description so the LLM
                // populates args accurately.
                for (auto it = p["properties"].begin();
                     it != p["properties"].end(); ++it) {
                    oss << "\n      · " << it.key();
                    const auto& pv = it.value();
                    if (pv.is_object() && pv.contains("type") &&
                        pv["type"].is_string())
                        oss << " (" << pv["type"].get<std::string>() << ")";
                    if (std::find(req.begin(), req.end(), it.key()) != req.end())
                        oss << " [required]";
                    if (pv.is_object() && pv.contains("description") &&
                        pv["description"].is_string())
                        oss << " — " << pv["description"].get<std::string>();
                }
            }
            oss << "\n";
        }
    }

    // Append routing_hints section when present
    if (!routing_hints_.empty()) {
        oss << "\n## Routing hints\n\n" << routing_hints_;
    }

    return oss.str();
}

// ---------------------------------------------------------------------------
// find_tool() — look up a Tool* by "catalog_name.tool_name"
// Returns nullptr if not found
// ---------------------------------------------------------------------------
neograph::Tool* McpCatalog::find_tool(const std::string& fully_qualified_name) const {
    // Split on '.' into catalog_name and tool_name
    const auto dot_pos = fully_qualified_name.find('.');
    if (dot_pos == std::string::npos) {
        // Not a fully qualified name without a dot — return nullptr
        return nullptr;
    }

    const std::string catalog_name = fully_qualified_name.substr(0, dot_pos);
    const std::string tool_name    = fully_qualified_name.substr(dot_pos + 1);

    for (const auto& entry : entries_) {
        if (entry.name != catalog_name) continue;

        // Search for tool_name within the matching catalog entry
        for (const auto& tool : entry.tools) {
            if (tool->get_name() == tool_name) {
                return tool.get();
            }
        }
        // catalog_name matched, but the tool was not found — stop searching
        break;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// allows_skip_synthesis() — query skip_synthesis_hint for the tool's entry
// ---------------------------------------------------------------------------
bool McpCatalog::allows_skip_synthesis(const std::string& fully_qualified_name) const {
    const auto dot_pos = fully_qualified_name.find('.');
    if (dot_pos == std::string::npos) return true;  // unknown format — respect LLM decision

    const std::string catalog_name = fully_qualified_name.substr(0, dot_pos);
    for (const auto& entry : entries_) {
        if (entry.name == catalog_name) return entry.skip_synthesis_hint;
    }
    return true;  // unregistered entry — respect LLM decision
}

// ---------------------------------------------------------------------------
// routing_hints_text() — return the stored routing hints string as-is
// ---------------------------------------------------------------------------
std::string McpCatalog::routing_hints_text() const {
    return routing_hints_;
}

}  // namespace jarvis::orchestrator
