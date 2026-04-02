#pragma once

#include <neograph/graph/types.h>
#include <neograph/graph/state.h>
#include <neograph/graph/node.h>
#include <neograph/graph/checkpoint.h>
#include <memory>
#include <set>

namespace neograph::graph {

// --- Run configuration ---
struct RunConfig {
    std::string thread_id;       // for checkpoint association (Phase 2)
    json        input;           // initial channel writes (e.g. {"messages": [...]})
    int         max_steps = 50;  // safety limit for loops
};

// --- Run result ---
struct RunResult {
    json        output;                           // final serialized state
    bool        interrupted       = false;        // Phase 2: HITL
    std::string interrupt_node;
    json        interrupt_value;
    std::string checkpoint_id;
    std::vector<std::string> execution_trace;     // ordered list of executed nodes
};

// =========================================================================
// GraphEngine: super-step loop execution engine
// Sequential loop for Phase 1; Taskflow parallel fan-out in Phase 3.
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

    // Take ownership of tools (lifetime management)
    void own_tools(std::vector<std::unique_ptr<Tool>> tools);

    // Set checkpoint store after compilation
    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);

    const std::string& graph_name() const { return name_; }

private:
    GraphEngine() = default;

    // Initialize state from channel definitions (in-place, no move)
    void init_state(GraphState& state) const;

    // Apply initial input to state
    void apply_input(GraphState& state, const json& input) const;

    // Internal run (shared between run/run_stream)
    RunResult execute_graph(const RunConfig& config,
                            const GraphStreamCallback& cb,
                            const std::string& resume_from = "",
                            const json& resume_value = json());

    // Resolve next node(s) from current node + state
    // Returns multiple for fan-out (multiple outgoing edges)
    std::vector<std::string> resolve_next_nodes(
        const std::string& current, const GraphState& state) const;

    // Check if all predecessors of a node are in the completed set
    bool all_predecessors_done(const std::string& node,
                               const std::set<std::string>& completed) const;

    // Save a checkpoint at the current state
    Checkpoint save_checkpoint(const GraphState& state,
                               const std::string& thread_id,
                               const std::string& current_node,
                               const std::string& next_node,
                               const std::string& phase,
                               int step,
                               const std::string& parent_id) const;

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

    // Precomputed graph topology (built in compile())
    std::map<std::string, std::set<std::string>> predecessors_;  // node -> {required predecessors}

    std::shared_ptr<CheckpointStore> checkpoint_store_;
    std::vector<std::unique_ptr<Tool>> owned_tools_;
};

} // namespace neograph::graph
