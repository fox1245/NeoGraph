#pragma once

#include <neograph/graph/types.h>
#include <neograph/graph/state.h>
#include <neograph/graph/node.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>
#include <memory>
#include <set>

namespace neograph::graph {

// --- Run configuration ---
struct RunConfig {
    std::string thread_id;           // for checkpoint association
    json        input;               // initial channel writes (e.g. {"messages": [...]})
    int         max_steps  = 50;     // safety limit for loops
    StreamMode  stream_mode = StreamMode::ALL;  // which events to emit
};

// --- Run result ---
struct RunResult {
    json        output;                           // final serialized state
    bool        interrupted       = false;        // HITL
    std::string interrupt_node;
    json        interrupt_value;
    std::string checkpoint_id;
    std::vector<std::string> execution_trace;     // ordered list of executed nodes
};

// =========================================================================
// GraphEngine: super-step loop execution engine
// =========================================================================
class GraphEngine {
public:
    // Compile a graph from JSON definition + default context
    static std::unique_ptr<GraphEngine> compile(
        const json& definition,
        const NodeContext& default_context,
        std::shared_ptr<CheckpointStore> store = nullptr);

    // Execute the graph (blocking)
    RunResult run(const RunConfig& config);

    // Execute with streaming events
    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    // Resume from interrupt (HITL)
    RunResult resume(const std::string& thread_id,
                     const json& resume_value = json(),
                     const GraphStreamCallback& cb = nullptr);

    // ── State inspection & manipulation (LangGraph Checkpointer API) ──

    std::optional<json> get_state(const std::string& thread_id) const;

    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;

    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "");

    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "");

    // ── Configuration ──

    void own_tools(std::vector<std::unique_ptr<Tool>> tools);
    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);

    // Set cross-thread shared memory store
    void set_store(std::shared_ptr<Store> store);
    std::shared_ptr<Store> get_store() const { return store_; }

    // Set default retry policy for all nodes
    void set_retry_policy(const RetryPolicy& policy);

    // Set retry policy for a specific node
    void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);

    const std::string& graph_name() const { return name_; }

private:
    GraphEngine() = default;

    void init_state(GraphState& state) const;
    void apply_input(GraphState& state, const json& input) const;

    RunResult execute_graph(const RunConfig& config,
                            const GraphStreamCallback& cb,
                            const std::string& resume_from = "",
                            const json& resume_value = json());

    std::vector<std::string> resolve_next_nodes(
        const std::string& current, const GraphState& state) const;

    bool all_predecessors_done(const std::string& node,
                               const std::set<std::string>& completed) const;

    Checkpoint save_checkpoint(const GraphState& state,
                               const std::string& thread_id,
                               const std::string& current_node,
                               const std::string& next_node,
                               const std::string& phase,
                               int step,
                               const std::string& parent_id) const;

    // Execute a node with retry policy, returns full NodeResult
    NodeResult execute_node_with_retry(
        const std::string& node_name,
        GraphState& state,
        const GraphStreamCallback& cb,
        StreamMode stream_mode);

    // Get retry policy for a node (node-specific or default)
    RetryPolicy get_retry_policy(const std::string& node_name) const;

    // --- Graph definition ---
    std::string name_;

    struct ChannelDef {
        std::string  name;
        ReducerType  type;
        std::string  reducer_name;
        json         initial_value;
    };
    std::vector<ChannelDef> channel_defs_;

    std::map<std::string, std::unique_ptr<GraphNode>> nodes_;
    std::vector<Edge>            edges_;
    std::vector<ConditionalEdge> conditional_edges_;

    std::set<std::string> interrupt_before_;
    std::set<std::string> interrupt_after_;

    std::map<std::string, std::set<std::string>> predecessors_;

    std::shared_ptr<CheckpointStore> checkpoint_store_;
    std::shared_ptr<Store>           store_;
    std::vector<std::unique_ptr<Tool>> owned_tools_;

    // Retry policies
    RetryPolicy default_retry_policy_;
    std::map<std::string, RetryPolicy> node_retry_policies_;
};

} // namespace neograph::graph
