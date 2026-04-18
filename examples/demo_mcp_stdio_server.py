"""NeoGraph 데모용 stdio MCP 서버.

로컬 지식베이스 조회 / 메모 저장 툴을 stdio transport로 제공.

사용법 (직접 호출):
  python demo_mcp_stdio_server.py  # stdin/stdout JSON-RPC 직접 주고받음

일반적으로 클라이언트가 subprocess로 기동시킨다:
  MCPClient({"python", "examples/demo_mcp_stdio_server.py"})
"""

import json
from datetime import datetime
from fastmcp import FastMCP

mcp = FastMCP("local-kb")

_notes: list[dict] = []

_KB = {
    "neograph": "NeoGraph is a C++ graph agent engine (LangGraph-equivalent).",
    "langgraph": "LangGraph is Python's graph-based agent orchestration library.",
    "mcp": "Model Context Protocol — JSON-RPC protocol for tool discovery and invocation.",
    "taskflow": "Work-stealing parallel task scheduler used by NeoGraph's fan-out path.",
}


@mcp.tool()
def kb_lookup(topic: str) -> str:
    """Look up a topic in the local knowledge base. Returns short definition."""
    key = topic.strip().lower()
    return _KB.get(key, f"(no entry for '{topic}')")


@mcp.tool()
def save_note(content: str) -> str:
    """Persist a note for this session. Returns the stored note id."""
    nid = len(_notes) + 1
    _notes.append({"id": nid, "content": content, "ts": datetime.now().isoformat()})
    return f"note#{nid} saved"


@mcp.tool()
def list_notes() -> str:
    """List every note saved in this session."""
    if not _notes:
        return "(no notes)"
    return "\n".join(f"#{n['id']} [{n['ts']}] {n['content']}" for n in _notes)


if __name__ == "__main__":
    mcp.run(transport="stdio")
