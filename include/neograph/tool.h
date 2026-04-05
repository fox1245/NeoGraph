/**
 * @file tool.h
 * @brief Abstract tool interface for callable functions.
 *
 * Defines the Tool base class. Implement this to create tools that
 * LLM agents can discover and invoke during the ReAct loop.
 */
#pragma once

#include <neograph/types.h>
#include <string>

namespace neograph {

/**
 * @brief Abstract base class for tools that agents can call.
 *
 * A tool provides its definition (name, description, parameter schema)
 * so the LLM knows when and how to call it, and an execute() method
 * that performs the actual work.
 *
 * @see neograph::mcp::MCPTool for remote MCP server tools.
 */
class Tool {
  public:
    virtual ~Tool() = default;

    /**
     * @brief Get the tool definition metadata.
     *
     * Returns a ChatTool containing the tool's name, description, and
     * JSON Schema for its parameters. This is sent to the LLM so it
     * can decide when to call the tool.
     *
     * @return Tool definition including name, description, and parameter schema.
     */
    virtual ChatTool get_definition() const = 0;

    /**
     * @brief Execute the tool with the given arguments.
     * @param arguments JSON object containing the tool's input parameters.
     * @return Result string to be fed back to the LLM.
     */
    virtual std::string execute(const json& arguments) = 0;

    /**
     * @brief Get the tool name.
     * @return Tool name string (must match the name in get_definition()).
     */
    virtual std::string get_name() const = 0;
};

} // namespace neograph
