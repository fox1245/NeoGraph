# Area B: protocol examples (MCP, A2A, ACP, gRPC). Included by examples/CMakeLists.txt.
# Older protocol examples (03, 20-24, 35, 37-39, 45, 53-55, 57, 59, 60) keep their targets in the
# root CMakeLists.txt.

neograph_add_example(mcp_tool_errors 75_mcp_tool_errors.cpp LINK neograph::core neograph::llm neograph::mcp)
neograph_add_example(mcp_http_server 76_mcp_http_server.cpp
    LINK neograph::core neograph::mcp neograph::mcp_http_server)
neograph_add_example(a2a_dialects 77_a2a_dialects.cpp LINK neograph::core neograph::a2a)
