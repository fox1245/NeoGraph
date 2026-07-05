// jarvis/src/orchestrator/mcp_catalog.h
//
// config/mcp_catalog.json 을 읽고 MCP 클라이언트들 + 도구 목록을 구성.
// 또 라우터의 시스템 프롬프트에 "available tools" 섹션을 만들어 주입.
//
// 도구 카탈로그가 단순한 도구 리스트가 아니라, 그 자체로 "자비스가 자기 능력을
// 어떻게 인식하는가" 의 자료 — 이 JSON 한 줄 추가하면 자비스가 새 능력을 즉시 씀.
//
// 모든 MCP 서버는 시작 시 1회 연결, get_tools() 호출해서 캐시.
// 도구 정의는 enabled=true 인 항목만 합쳐서 라우터/direct/parallel 노드들이 공유.
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
    std::vector<std::unique_ptr<neograph::Tool>> tools;  // get_tools() 결과 캐시
};

class McpCatalog {
  public:
    // catalog_json_path = "config/mcp_catalog.json"
    static McpCatalog load(const std::string& catalog_json_path);

    // 라우터 LLM 에 주입할 텍스트 — 각 도구 이름 + 1줄 설명 + 태그.
    std::string render_for_router_prompt() const;

    // direct_branch / parallel_branch 가 도구 이름으로 조회.
    neograph::Tool* find_tool(const std::string& fully_qualified_name) const;

    // 해당 도구의 카탈로그 엔트리가 skip_synthesis 를 허용하는가.
    // 라우터가 LLM 결정 위에 덮어쓰는 강제 규칙용 — hint=false 인 도구의
    // raw 출력(ISO 날짜 등 비발화 문자열)이 TTS 로 직행하는 것을 막는다.
    // 알 수 없는 도구는 true (LLM 결정 존중).
    bool allows_skip_synthesis(const std::string& fully_qualified_name) const;

    // routing_hints 섹션 (있으면) 도 라우터 프롬프트에 주입.
    std::string routing_hints_text() const;

    const std::vector<CatalogEntry>& entries() const { return entries_; }

  private:
    std::vector<CatalogEntry> entries_;
    std::string routing_hints_;
};

}  // namespace jarvis::orchestrator
