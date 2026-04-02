#pragma once

#include <neograph/graph/types.h>
#include <neograph/graph/state.h>

namespace neograph::graph {

// --- Abstract graph node ---
class GraphNode {
public:
    virtual ~GraphNode() = default;

    // Execute node: read state, return channel writes
    virtual std::vector<ChannelWrite> execute(const GraphState& state) = 0;

    // Streaming variant (default: delegates to execute)
    virtual std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    virtual std::string name() const = 0;
};

// --- LLM Call node: mirrors Agent::run() LLM call logic ---
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string name() const override { return name_; }

private:
    std::string              name_;
    std::shared_ptr<Provider> provider_;
    std::vector<Tool*>       tools_;
    std::string              model_;
    std::string              instructions_;

    // Shared logic: build messages + params from state
    CompletionParams build_params(const GraphState& state) const;
};

// --- Tool Dispatch node: mirrors Agent::run() tool execution logic ---
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::string name() const override { return name_; }

private:
    std::string        name_;
    std::vector<Tool*> tools_;
};

// Forward declaration
class GraphEngine;

// --- Intent Classifier node: LLM-based intent routing ---
// Calls the LLM to classify user intent, writes result to __route__ channel.
// Used with "route_channel" condition for dynamic routing.
class IntentClassifierNode : public GraphNode {
public:
    IntentClassifierNode(const std::string& name, const NodeContext& ctx,
                         const std::string& prompt,
                         std::vector<std::string> valid_routes);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::string name() const override { return name_; }

private:
    std::string               name_;
    std::shared_ptr<Provider>  provider_;
    std::string               model_;
    std::string               prompt_;
    std::vector<std::string>  valid_routes_;
};

// --- Subgraph node: runs a compiled GraphEngine as a single node ---
// Enables hierarchical composition (Supervisor pattern, nested workflows).
// Input/output channels are mapped between parent and child graph.
class SubgraphNode : public GraphNode {
public:
    // input_map:  parent_channel -> child_channel (read from parent, write to child input)
    // output_map: child_channel  -> parent_channel (read from child result, write to parent)
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {});

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string name() const override { return name_; }

private:
    std::string name_;
    std::shared_ptr<GraphEngine> subgraph_;
    std::map<std::string, std::string> input_map_;   // parent -> child
    std::map<std::string, std::string> output_map_;   // child -> parent

    // Build subgraph input from parent state
    json build_subgraph_input(const GraphState& state) const;
    // Extract parent writes from subgraph result
    std::vector<ChannelWrite> extract_output(const json& subgraph_output) const;
};

} // namespace neograph::graph
