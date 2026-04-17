/**
 * @file graph/compiler.h
 * @brief Pure JSON-definition → CompiledGraph parser, extracted from GraphEngine.
 *
 * GraphCompiler turns a JSON graph definition into a CompiledGraph — a
 * value-type bundle of channels, nodes, edges, barriers, interrupts, and
 * retry policy. It has NO knowledge of runtime (threading, checkpointing,
 * execution) and NO dependency on GraphEngine; it depends only on
 * NodeFactory (to instantiate GraphNode subclasses).
 *
 * Why a separate stage: parsing failures surface here cleanly
 * (malformed edge, unknown reducer, missing node type) before any
 * runtime state is constructed, and the resulting CompiledGraph can be
 * inspected or mutated in tests without spinning up the engine.
 */
#pragma once

#include <neograph/graph/types.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/scheduler.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neograph::graph {

class GraphNode;

/**
 * @brief Declarative configuration for a single state channel.
 *
 * Lifted out of GraphEngine so the compilation stage can own and return
 * it independently of the engine.
 */
struct ChannelDef {
    std::string  name;
    ReducerType  type = ReducerType::OVERWRITE;
    std::string  reducer_name = "overwrite";
    json         initial_value;
};

/**
 * @brief Parsed graph definition, ready for the engine to run.
 *
 * Value type. Contains everything the engine needs from the JSON
 * definition and nothing it needs at runtime — no schedulers, no
 * checkpoint store, no retry-policy overrides (those are set via
 * GraphEngine setters after compilation).
 *
 * Moved, not copied, into GraphEngine. Each field matches a private
 * member of GraphEngine that previously was populated directly inside
 * GraphEngine::compile().
 */
struct CompiledGraph {
    std::string name;
    std::vector<ChannelDef> channel_defs;
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> conditional_edges;
    BarrierSpecs barrier_specs;
    std::set<std::string> interrupt_before;
    std::set<std::string> interrupt_after;
    /// Present iff the JSON defined a top-level "retry_policy" object.
    /// When absent, GraphEngine keeps its RetryPolicy default-constructed.
    std::optional<RetryPolicy> retry_policy;
};

/**
 * @brief Stateless JSON → CompiledGraph translator.
 *
 * Single static entry point. Separating this stage lets routing /
 * execution tests build a CompiledGraph fixture directly (bypassing
 * JSON) and lets parsing tests run without touching the runtime.
 */
class GraphCompiler {
public:
    /**
     * @brief Parse a JSON graph definition into a CompiledGraph.
     *
     * @param definition Top-level graph JSON ({name, channels, nodes,
     *                   edges, interrupt_before, interrupt_after,
     *                   retry_policy}).
     * @param default_context Shared NodeContext forwarded to every
     *                        NodeFactory-created node.
     * @return Fully parsed CompiledGraph. Safe to `std::move` into
     *         GraphEngine internals.
     * @throws std::runtime_error If a required field is malformed or a
     *                            referenced node type is unknown.
     */
    static CompiledGraph compile(const json& definition,
                                 const NodeContext& default_context);
};

} // namespace neograph::graph
