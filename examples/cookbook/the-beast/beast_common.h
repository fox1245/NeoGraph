// NeoGraph Cookbook — shared live-Beast validation helpers
#pragma once

#include <neograph/graph/loader.h>
#include <neograph/graph/validator.h>
#include <neograph/json.h>

#include <stdexcept>
#include <string>

namespace neograph::cookbook::beast {

/** Result of the strict Core schema → compiler/validator gate sequence. */
struct HarnessVerdict {
    bool        ok = false;
    std::string gate;
    std::string report;
    json        core;
};

/**
 * Validate a generated strict Core topology through the same coherence gates
 * used by the live Beast. On success, returns the canonical interchange JSON.
 */
inline HarnessVerdict validate_harness(const json& core, const graph::NodeContext& ctx) {
    if (!core.contains("schema_version") || !core["schema_version"].is_number_integer() ||
        core["schema_version"].get<int>() != graph::TOPOLOGY_SCHEMA_VERSION) {
        return {false, "schema", "schema_version must match TOPOLOGY_SCHEMA_VERSION", {}};
    }

    try {
        auto compiled = graph::GraphCompiler::compile(core, ctx);
        graph::GraphCompiler::verify_roundtrip(core, compiled);
        const auto report = graph::GraphValidator::validate(compiled);
        if (report.has_errors()) return {false, "validate", report.summary(), {}};
        return {true, "accepted", {}, core};
    } catch (const std::exception& e) {
        return {false, "compile", e.what(), {}};
    }
}

/** Extract the outer JSON object from an LLM response that may include prose. */
inline json extract_json_object(const std::string& text) {
    const auto begin = text.find('{');
    const auto end   = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        throw std::runtime_error("no JSON object in model reply");
    }
    return json::parse(text.substr(begin, end - begin + 1));
}

}  // namespace neograph::cookbook::beast
