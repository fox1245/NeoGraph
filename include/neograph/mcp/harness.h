/**
 * @file mcp/harness.h
 * @brief Compiler-backed subagent Harness service for the MCP server.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/cancel.h>
#include <neograph/json.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace neograph::mcp {

class MCPServer;

enum class HarnessWorkerResponseKind {
    VALUE,
    EMPTY,
    PARSE_ERROR,
    TOOL_ERROR,
    TIMEOUT,
    CANCELLED,
};

/** Input handed to the embedding's provider/tool execution backend. */
struct HarnessWorkerCall {
    json task;
    json worker;
    json tool_catalog;
    std::size_t attempt = 1;
    std::string repair_feedback;
};

/** Typed worker response; failure classes never masquerade as valid output. */
struct NEOGRAPH_API HarnessWorkerResponse {
    HarnessWorkerResponseKind kind = HarnessWorkerResponseKind::VALUE;
    json value;
    std::string message;

    static HarnessWorkerResponse success(json value);
    static HarnessWorkerResponse empty(std::string message = {});
    static HarnessWorkerResponse parse_error(std::string message);
    static HarnessWorkerResponse tool_error(std::string message);
    static HarnessWorkerResponse timeout(std::string message = {});
    static HarnessWorkerResponse cancelled(std::string message = {});
};

using HarnessWorkerExecutor = std::function<HarnessWorkerResponse(
    const HarnessWorkerCall&,
    const std::shared_ptr<graph::CancelToken>&)>;

struct HarnessServiceConfig {
    HarnessWorkerExecutor worker_executor;
    std::size_t max_artifacts = 128;
    std::size_t max_runs = 128;
};

/**
 * Compile, run, inspect, and cancel immutable Harness artifacts.
 *
 * `register_tools()` captures this service by reference; the service must
 * outlive the MCPServer and be destroyed only after the server has stopped.
 */
class NEOGRAPH_API HarnessService {
public:
    explicit HarnessService(HarnessServiceConfig config = {});
    ~HarnessService();

    HarnessService(const HarnessService&) = delete;
    HarnessService& operator=(const HarnessService&) = delete;

    /// Register neograph_schema/compile/start/get/cancel on a server.
    void register_tools(MCPServer& server);

    /// Build-specific schemas, node palette, presets, and capabilities.
    json schema() const;

    /// Compile and retain an immutable artifact when the request is valid.
    json compile(const json& request);

    /// Start from {artifact_id} or compile-and-start from {request}.
    json start(const json& arguments);

    /// Return a compact snapshot for a run.
    json get(const std::string& run_id) const;

    /// Request cooperative cancellation. Returns false for unknown/terminal runs.
    bool cancel(const std::string& run_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::mcp
