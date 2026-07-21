/**
 * @file mcp/harness.h
 * @brief Compiler-backed subagent Harness service for the MCP server.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/json.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph {
class Provider;
}

namespace neograph::mcp {

class MCPServer;

enum class HarnessWorkerResponseKind {
    VALUE,
    EMPTY,
    PARSE_ERROR,
    TOOL_ERROR,
    TIMEOUT,
    CANCELLED,
    AWAITING_TOOL_RESULTS,
    INPUT_REQUIRED,
};

/** Input handed to the embedding's provider/tool execution backend. */
struct HarnessWorkerCall {
    json task;
    json worker;
    json tool_catalog;
    json policy;
    std::size_t attempt = 1;
    std::string repair_feedback;
    std::optional<json> resume_value;
};

/** Typed worker response; failure classes never masquerade as valid output. */
struct NEOGRAPH_MCP_SERVER_API HarnessWorkerResponse {
    HarnessWorkerResponseKind kind = HarnessWorkerResponseKind::VALUE;
    json value;
    std::string message;

    static HarnessWorkerResponse success(json value);
    static HarnessWorkerResponse empty(std::string message = {});
    static HarnessWorkerResponse parse_error(std::string message);
    static HarnessWorkerResponse tool_error(std::string message);
    static HarnessWorkerResponse timeout(std::string message = {});
    static HarnessWorkerResponse cancelled(std::string message = {});
    static HarnessWorkerResponse awaiting_tool_results(json pending);
    static HarnessWorkerResponse input_required(json pending);
};

using HarnessWorkerExecutor = std::function<HarnessWorkerResponse(
    const HarnessWorkerCall&, const std::shared_ptr<graph::CancelToken>&)>;

using HarnessCapabilityExecutor = std::function<json(const json& tool_definition,
                                                     const json& arguments,
                                                     const std::shared_ptr<graph::CancelToken>&)>;

struct HarnessProviderExecutorConfig {
    std::shared_ptr<Provider> provider;
    std::string model;
    HarnessCapabilityExecutor capability_executor;
    std::size_t max_tool_rounds = 8;
};

/// Build a worker executor that calls a NeoGraph Provider directly.
NEOGRAPH_MCP_SERVER_API HarnessWorkerExecutor
make_provider_harness_executor(HarnessProviderExecutorConfig config);

/** Append-only causal event boundary, separate from mutable run snapshots. */
class NEOGRAPH_MCP_SERVER_API HarnessJournal {
public:
    virtual ~HarnessJournal() = default;

    virtual void append_event(const json& event) = 0;
    virtual std::vector<json> list_events(const std::string& run_id,
                                          std::size_t        after_sequence = 0,
                                          std::size_t        limit = 1000) = 0;
};

/** Durable storage boundary for retained Harness artifacts and run records. */
class NEOGRAPH_MCP_SERVER_API HarnessRecordStore {
public:
    virtual ~HarnessRecordStore() = default;

    virtual void save_artifact(const std::string& artifact_id, const json& record)      = 0;
    virtual std::optional<json> load_artifact(const std::string& artifact_id)           = 0;
    virtual void                save_run(const std::string& run_id, const json& record) = 0;
    virtual std::optional<json> load_run(const std::string& run_id)                     = 0;
};

/** Count-bounded cleanup policy for stores that support durable retention. */
struct HarnessRetentionPolicy {
    std::size_t              max_artifacts = std::numeric_limits<std::size_t>::max();
    std::size_t              max_runs      = std::numeric_limits<std::size_t>::max();
    std::vector<std::string> protected_artifact_ids;
    std::vector<std::string> protected_run_ids;
};

/** Records removed by one atomic record-store cleanup pass. */
struct HarnessRetentionResult {
    std::vector<std::string> artifact_ids;
    std::vector<std::string> run_ids;
};

/** Optional sibling boundary; HarnessRecordStore's stable vtable remains unchanged. */
class NEOGRAPH_MCP_SERVER_API HarnessRetentionStore {
public:
    virtual ~HarnessRetentionStore()                                                      = default;
    virtual HarnessRetentionResult cleanup_retained(const HarnessRetentionPolicy& policy) = 0;
};

/** Atomic JSON-file implementation suitable for local process restarts. */
class NEOGRAPH_MCP_SERVER_API FileHarnessRecordStore final : public HarnessRecordStore {
public:
    explicit FileHarnessRecordStore(std::string root_directory);
    ~FileHarnessRecordStore() override;

    void                save_artifact(const std::string& artifact_id, const json& record) override;
    std::optional<json> load_artifact(const std::string& artifact_id) override;
    void                save_run(const std::string& run_id, const json& record) override;
    std::optional<json> load_run(const std::string& run_id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct HarnessServiceConfig {
    HarnessWorkerExecutor worker_executor;
    std::shared_ptr<graph::CheckpointStore> checkpoint_store;
    std::shared_ptr<HarnessRecordStore>     record_store;
    std::size_t max_artifacts = 128;
    std::size_t max_runs = 128;
    std::chrono::milliseconds               poll_interval{1000};
    std::chrono::milliseconds               run_ttl{std::chrono::hours(24)};
    bool                                    enable_experimental_tasks = false;
};

/**
 * Compile, run, inspect, and cancel immutable Harness artifacts.
 *
 * `register_tools()` captures this service by reference; the service must
 * outlive the MCPServer and be destroyed only after the server has stopped.
 */
class NEOGRAPH_MCP_SERVER_API HarnessService {
public:
    explicit HarnessService(HarnessServiceConfig config = {});
    HarnessService(HarnessServiceConfig config, std::shared_ptr<HarnessJournal> journal);
    ~HarnessService();

    HarnessService(const HarnessService&) = delete;
    HarnessService& operator=(const HarnessService&) = delete;

    /// Register neograph_schema/compile/start/get/resume/cancel on a server.
    void register_tools(MCPServer& server);

    /// Build-specific schemas, node palette, presets, and capabilities.
    json schema() const;

    /// Compile and retain an immutable artifact when the request is valid.
    json compile(const json& request);

    /// Start from {artifact_id} or compile-and-start from {request}.
    json start(const json& arguments);

    /// Resume the exact pending call with a schema-validated host result.
    json resume(const json& arguments);

    /// Return a compact snapshot for a run.
    json get(const std::string& run_id, const std::string& view = "status") const;

    /// Return a paginated debugger view without changing the compact default.
    json get(const std::string& run_id,
             const std::string& view,
             std::size_t        after_sequence,
             std::size_t        limit) const;

    /// Dereference a neograph://runs/<run_id>/<view> result URI.
    json read(const std::string& uri) const;

    /// Request cooperative cancellation. Returns false for unknown/terminal runs.
    bool cancel(const std::string& run_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::mcp
