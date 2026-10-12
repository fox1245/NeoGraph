# Area B: protocol examples (MCP, A2A, ACP, gRPC). Included by examples/CMakeLists.txt.
# Older protocol examples (03, 20-24, 35, 37-39, 45, 53-55, 57, 59, 60) keep their targets in the
# root CMakeLists.txt.

neograph_add_example(mcp_tool_errors 75_mcp_tool_errors.cpp LINK neograph::core neograph::llm neograph::mcp)
neograph_add_example(mcp_http_server 76_mcp_http_server.cpp
    LINK neograph::core neograph::mcp neograph::mcp_http_server)
neograph_add_example(a2a_dialects 77_a2a_dialects.cpp LINK neograph::core neograph::a2a)
neograph_add_example(a2a_input_required 78_a2a_input_required.cpp LINK neograph::core neograph::a2a)
neograph_add_example(a2a_collaboration 79_a2a_collaboration.cpp LINK neograph::core neograph::a2a)
neograph_add_example(acp_permission 80_acp_permission.cpp LINK neograph::core neograph::acp)
neograph_add_example(acp_elicitation 81_acp_elicitation.cpp LINK neograph::core neograph::acp)
