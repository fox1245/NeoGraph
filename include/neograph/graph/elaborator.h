/**
 * @file graph/elaborator.h
 * @brief DSL surface → core topology JSON expansion (issue #75 M4).
 *
 * The core topology grammar (what GraphCompiler consumes) is frozen;
 * every DSL convenience lives here and is *fully expanded away* before
 * the compiler ever sees the document ("programmable surface → data
 * core"; Nickel/Dhall architecture). The engine, Studio, validation,
 * and translation validation all keep operating on the core only.
 *
 * The elaboration language is deliberately **non-Turing-complete and
 * total**: variable substitution (acyclic), template instantiation
 * (no recursion — a template body cannot `use` templates), and boolean
 * conditional inclusion. Every DSL document therefore normalizes to a
 * unique core document in bounded time — the property the compile-time
 * guarantees (M1 TV, M2 static checks) are conditioned on.
 *
 * ## Surface grammar (top-level keys, removed by elaboration)
 *
 *   "vars": { "<name>": <any JSON> }
 *       Named values. A var value may reference other vars; cycles are
 *       an error. Referenced as:
 *         {"$var": "<name>"}   — whole-value substitution, anywhere
 *         "...${name}..."      — string interpolation (scalar vars);
 *                                a string that is exactly "${name}"
 *                                substitutes the whole value.
 *
 *   "templates": { "<tname>": {
 *        "params": ["p1", ...],
 *        "channels"?: {...}, "nodes"?: {...},
 *        "edges"?: [...], "conditional_edges"?: [...] } }
 *       Reusable topology fragments. Inside a body, parameters are
 *       referenced as {"$param": "p1"} / "@{p1}" (same rules as vars).
 *
 *   "use": [ { "template": "<tname>", "prefix": "<p>",
 *              "args": { "p1": <JSON>, ... }, "when"?: <bool|$var> } ]
 *       Instantiations. Every node the template declares is renamed
 *       "<p>_<localName>", and local references (edge endpoints, route
 *       targets, barrier wait_for) are renamed with it; sentinels and
 *       non-local names pass through. Template channels merge globally
 *       (same name must be an identical definition — channels are the
 *       shared-state surface, deliberately NOT prefixed). "when": false
 *       skips the instantiation entirely.
 *
 * Annotation keys ('_'/'x-' prefixes) are passed through verbatim —
 * no substitution happens inside them, so a "${...}" in a _comment is
 * not an error.
 *
 * ## Source map
 *
 * Elaboration returns a source map: JSON-pointer-ish output locations
 * → the DSL construct that produced them (e.g. "/nodes/math_expert" →
 * "use[0] template 'expert' prefix 'math'"). Elaboration errors always
 * carry a source coordinate ("vars.model", "use[2].args", ...), so a
 * mistake in the DSL is reported against the DSL, not against the
 * expanded core.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>

namespace neograph::graph {

struct ElaborationResult {
    /// Pure core topology (no vars/templates/use keys) — feed to
    /// GraphCompiler::compile / GraphEngine::compile as-is. This is
    /// the "lockfile" artifact: commit it next to the DSL source.
    json core;
    /// Output-location → source-construct map (array of {target,
    /// source} objects), for tooling and diff review.
    json sourcemap;
};

class NEOGRAPH_API Elaborator {
public:
    /**
     * @brief Expand a DSL document into core topology JSON.
     *
     * Total function: either returns the unique normal form or throws
     * std::runtime_error with a DSL source coordinate (unknown var,
     * var cycle, unknown template, missing/unexpected template args,
     * node-name collision, conflicting channel merge, non-boolean
     * "when", "$param" outside a template body).
     *
     * A document with no DSL keys is returned unchanged (elaboration
     * is the identity on core documents — idempotence).
     */
    static ElaborationResult elaborate(const json& dsl_doc);
};

} // namespace neograph::graph
