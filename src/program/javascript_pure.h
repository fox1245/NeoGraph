#pragma once

#include <neograph/program/gate.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace neograph::program::detail {

struct GateCaseSandboxResult {
    bool                             passed  = false;
    std::optional<GateFailureCategory> failure{};
    std::optional<json>              actual{};
};

void validate_gate_javascript_module_no_imports(std::string_view         source_id,
                                                std::string_view         script,
                                                const GateSandboxLimits& limits);

GateCaseSandboxResult evaluate_gate_candidate_case(std::string_view         source_id,
                                                   std::string_view         candidate_script,
                                                   const json&              input,
                                                   const GateSandboxLimits& limits);

GateCaseSandboxResult evaluate_gate_property_case(std::string_view         source_id,
                                                  std::string_view         property_script,
                                                  std::string_view         entrypoint,
                                                  const json&              input,
                                                  const json&              actual,
                                                  const GateSandboxLimits& limits);

GateCaseSandboxResult generate_gate_case_input(std::string_view         source_id,
                                               std::string_view         generator_script,
                                               std::string_view         seed_hex,
                                               std::uint64_t            case_index,
                                               const GateSandboxLimits& limits);

}  // namespace neograph::program::detail
