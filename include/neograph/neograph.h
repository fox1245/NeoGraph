/**
 * @file neograph.h
 * @brief Convenience header: includes the full NeoGraph core + graph engine API.
 *
 * Include this single header to access all core types, the graph engine,
 * checkpointing, state management, and the node system. LLM and MCP
 * modules must be included separately.
 *
 * @code
 * #include <neograph/neograph.h>          // Core + graph engine
 * #include <neograph/llm/openai_provider.h> // OpenAI provider (optional)
 * #include <neograph/mcp/client.h>          // MCP client (optional)
 * @endcode
 */
#pragma once

// Foundation types
#include <neograph/types.h>
#include <neograph/provider.h>
#include <neograph/tool.h>

// Graph engine
#include <neograph/graph/types.h>
#include <neograph/graph/state.h>
#include <neograph/graph/node.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/react_graph.h>
#include <neograph/graph/store.h>
