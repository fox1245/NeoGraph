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

#include <neograph/api.h>
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
    /// True iff the JSON channel definition carried an explicit
    /// "initial" key. Distinguishes `"initial": null` from an absent
    /// key so CompiledGraph::to_json() can re-emit exactly what was
    /// declared (round-trip fidelity).
    bool         has_initial = false;
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

    /// Declared "schema_version" from the topology JSON, or 0 when the
    /// document did not carry one. Version >= 1 opts the document into
    /// strict compilation (unknown/unconsumed keys are hard errors —
    /// see GraphCompiler docs). 0 preserves the historical lenient
    /// behavior for existing files.
    int schema_version = 0;

    /// Verbatim copy of each node's JSON definition (config + "type"),
    /// minus the "barrier" key (barriers are re-emitted from
    /// barrier_specs, so translation validation compares the compiled
    /// barrier state — not an input echo). Backs to_json().
    std::map<std::string, json> node_defs;

    /**
     * @brief Re-emit this compiled graph as topology JSON.
     *
     * Every field is reconstructed from compiled (L2) state where such
     * state exists — channels from channel_defs, edges from
     * edges/conditional_edges, barriers from barrier_specs, interrupts
     * from the sets, retry_policy from the parsed struct. Node configs
     * are the one pass-through: the compiler hands them verbatim to
     * NodeFactory, so they are re-emitted from node_defs.
     *
     * Contract (translation validation):
     *   canon(definition) == canon(compile(definition).to_json())
     * must hold for every accepted document. A mismatch means the
     * compiler dropped or rewired something silently — exactly the
     * v0.1.0–v0.1.7 conditional_edges regression class.
     */
    json to_json() const;
};

/**
 * @brief Stateless JSON → CompiledGraph translator.
 *
 * Single static entry point. Separating this stage lets routing /
 * execution tests build a CompiledGraph fixture directly (bypassing
 * JSON) and lets parsing tests run without touching the runtime.
 */
class NEOGRAPH_API GraphCompiler {
public:
    /**
     * @brief Parse a JSON graph definition into a CompiledGraph.
     *
     * @param definition Top-level graph JSON ({schema_version, name,
     *                   channels, nodes, edges, conditional_edges,
     *                   interrupt_before, interrupt_after,
     *                   retry_policy}).
     * @param default_context Shared NodeContext forwarded to every
     *                        NodeFactory-created node.
     * @return Fully parsed CompiledGraph. Safe to `std::move` into
     *         GraphEngine internals.
     * @throws std::runtime_error If a required field is malformed or a
     *                            referenced node type is unknown.
     *
     * ## Strict mode (consumed-key accounting)
     *
     * A document declaring `"schema_version": 1` opts into strict
     * compilation: every key of every object the compiler owns
     * (top level, channel defs, node defs, barrier, edges,
     * conditional_edges items, retry_policy) must be *consumed* by the
     * parser. A key nobody consumed — a typo like `conditionnal_edges`,
     * a field the engine doesn't support, a barrier whose wait_for is
     * missing/empty — is a hard compile error listing every offender.
     * This makes the v0.1.0–v0.1.7 "silently dropped block" regression
     * class structurally impossible: deleting a parsing step deletes
     * its consumption mark, and any document using that feature fails
     * loudly instead of degrading silently.
     *
     * Annotation namespace: keys starting with `_` or `x-` (e.g.
     * `_comment`, `x-studio-pos`) are never consumed, never errors,
     * and are ignored by canon() — they belong to humans and tooling,
     * not to the engine.
     *
     * Documents without `schema_version` (or 0) keep the historical
     * lenient behavior byte-for-byte. `schema_version` above the
     * engine's supported version (currently 1) is always an error.
     */
    static CompiledGraph compile(const json& definition,
                                 const NodeContext& default_context);

    /**
     * @brief Canonicalize a topology JSON document for equivalence
     *        comparison. Pure function; no registries touched.
     *
     * Two topology documents describe the same graph iff their canon()
     * forms are equal (nlohmann object comparison is key-order
     * insensitive; canon sorts all owned arrays). Used as the
     * translation-validation oracle:
     *   canon(definition) == canon(compile(definition).to_json())
     *
     * Normalizations applied (the *whitelist* — anything the compiler
     * rewrites on purpose must be mirrored here, and nothing else;
     * every whitelist addition needs review):
     *   - defaults made explicit: name, channel reducer, retry_policy
     *     fields, schema_version dropped when 0/absent
     *   - inline conditional edges (legacy `edges` items carrying
     *     `condition` / type:"conditional") are moved into
     *     `conditional_edges`
     *   - owned arrays sorted: edges, conditional_edges,
     *     interrupt_before/after (deduped), barrier wait_for (deduped)
     *   - empty owned containers dropped (routes:{}, interrupt:[])
     *   - `_`/`x-` annotation keys stripped at every owned level
     *     (node config *contents* are not descended into)
     *
     * Unknown keys are deliberately PRESERVED, not stripped — that is
     * how the TV compare flags a key the compiler ignored.
     */
    static json canon(const json& definition);

    /**
     * @brief Upgrade a legacy (schema_version 0 / absent) topology
     *        document to the current schema version.
     *
     * v0 → v1 is purely mechanical and **lossless**: it stamps
     * `schema_version: 1`, removes barrier blocks whose wait_for is
     * missing/empty (making the legacy silent drop explicit), and
     * renames every key strict mode would refuse to
     * `x-upgraded-<key>` — the annotation namespace — so no user data
     * is deleted, it is just moved out of the engine's way exactly as
     * the lenient parser ignored it.
     *
     * Guarantee (tested over the corpus): compiling the legacy
     * document leniently and compiling the upgraded document strictly
     * yield IR-equivalent graphs
     * (canon(to_json()) equal modulo the schema_version stamp).
     *
     * Like compile(), node-config upgrading consults NodeFactory —
     * register custom node types before calling. Documents already at
     * the current version are returned unchanged.
     */
    static json upgrade_to_latest(const json& definition);

    /**
     * @brief Translation validation: assert compile() lost nothing.
     *
     * Compares canon(definition) against canon(cg.to_json()). On
     * mismatch: throws std::runtime_error carrying the JSON-Patch diff
     * when the document is strict (schema_version >= 1); writes a
     * one-shot warning to stderr when lenient (legacy documents keep
     * compiling, but silent drops become visible).
     */
    static void verify_roundtrip(const json& definition,
                                 const CompiledGraph& cg);
};

} // namespace neograph::graph
