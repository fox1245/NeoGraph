// jarvis/src/orchestrator/mcp_catalog.cpp
//
// McpCatalog 구현 — JSON 카탈로그 파일을 읽어 MCP 클라이언트를 만들고,
// 각 서버에서 도구 목록을 받아와 캐시한다.
// 라우터 LLM 시스템 프롬프트에 주입할 텍스트 생성도 담당.

#include "mcp_catalog.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace jarvis::orchestrator {

// ---------------------------------------------------------------------------
// load() — JSON 파일 파싱 + MCP 클라이언트 연결 + 도구 캐시
// ---------------------------------------------------------------------------
McpCatalog McpCatalog::load(const std::string& catalog_json_path) {
    // JSON 파일 읽기
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
    // "tools" 배열 처리 — enabled=true 항목만
    // ---------------------------------------------------------------------------
    if (!catalog.contains("tools") || !catalog["tools"].is_array()) {
        // tools 키가 없으면 빈 카탈로그로 반환 (오류는 아님)
        return result;
    }

    for (const auto& item : catalog["tools"]) {
        // enabled 필드가 false 면 건너뜀
        bool enabled = item.value("enabled", true);
        if (!enabled) {
            continue;
        }

        const std::string name        = item.value("name", "");
        const std::string transport   = item.value("transport", "http");
        const std::string description = item.value("description", "");

        // tags 배열 수집
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
        // 트랜스포트 종류에 따라 MCPClient 생성 + 도구 목록 수집
        // 실패하면 로그만 남기고 이 항목은 건너뜀 (다른 항목은 계속 처리)
        // ---------------------------------------------------------------------------
        try {
            if (transport == "stdio") {
                // command 배열 필수 (예: ["python3", "server.py"])
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
                // 기본값 = HTTP 트랜스포트
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

            // 도구 목록 받아서 캐시
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
    // "routing_hints" 섹션 — 있으면 텍스트로 조립해서 저장
    // ---------------------------------------------------------------------------
    if (catalog.contains("routing_hints") && catalog["routing_hints"].is_object()) {
        const auto& hints = catalog["routing_hints"];
        std::ostringstream oss;

        // 각 힌트 키를 순회하며 목록 형태로 문자열 조립
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
// render_for_router_prompt() — 라우터 LLM 시스템 프롬프트에 넣을 텍스트 생성
//
// 형식:
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

        // 태그 표시
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

        // 서버 단위 설명
        if (!entry.description.empty()) {
            oss << entry.description << "\n";
        }

        // 개별 도구 목록 — 이름은 "catalog_name.tool_name" 완전 수식 형식
        for (const auto& tool : entry.tools) {
            const auto def = tool->get_definition();
            oss << "  - " << entry.name << "." << def.name;
            if (!def.description.empty()) {
                oss << ": " << def.description;
            }
            oss << "\n";
        }
    }

    // routing_hints 섹션이 있으면 뒤에 덧붙임
    if (!routing_hints_.empty()) {
        oss << "\n## Routing hints\n\n" << routing_hints_;
    }

    return oss.str();
}

// ---------------------------------------------------------------------------
// find_tool() — "catalog_name.tool_name" 형식으로 도구 포인터 조회
// 못 찾으면 nullptr 반환
// ---------------------------------------------------------------------------
neograph::Tool* McpCatalog::find_tool(const std::string& fully_qualified_name) const {
    // '.' 기준으로 앞부분 = catalog_name, 뒷부분 = tool_name
    const auto dot_pos = fully_qualified_name.find('.');
    if (dot_pos == std::string::npos) {
        // 점이 없으면 완전 수식 이름이 아님 — nullptr
        return nullptr;
    }

    const std::string catalog_name = fully_qualified_name.substr(0, dot_pos);
    const std::string tool_name    = fully_qualified_name.substr(dot_pos + 1);

    for (const auto& entry : entries_) {
        if (entry.name != catalog_name) continue;

        // 해당 catalog entry 안에서 tool_name 검색
        for (const auto& tool : entry.tools) {
            if (tool->get_name() == tool_name) {
                return tool.get();
            }
        }
        // catalog_name 은 일치했지만 도구가 없음 — 더 볼 필요 없음
        break;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// allows_skip_synthesis() — 도구가 속한 엔트리의 skip_synthesis_hint 조회
// ---------------------------------------------------------------------------
bool McpCatalog::allows_skip_synthesis(const std::string& fully_qualified_name) const {
    const auto dot_pos = fully_qualified_name.find('.');
    if (dot_pos == std::string::npos) return true;  // 형식 불명 — LLM 결정 존중

    const std::string catalog_name = fully_qualified_name.substr(0, dot_pos);
    for (const auto& entry : entries_) {
        if (entry.name == catalog_name) return entry.skip_synthesis_hint;
    }
    return true;  // 미등록 엔트리 — LLM 결정 존중
}

// ---------------------------------------------------------------------------
// routing_hints_text() — 저장된 routing hints 문자열 그대로 반환
// ---------------------------------------------------------------------------
std::string McpCatalog::routing_hints_text() const {
    return routing_hints_;
}

}  // namespace jarvis::orchestrator
