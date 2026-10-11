/**
 * @file graph/configuration_failure.h
 * @brief Marker for exceptions that are configuration or definition verdicts (#364).
 */
#pragma once

#include <neograph/api.h>

namespace neograph::graph {

/**
 * @brief Marks an exception type as a configuration or definition verdict.
 *
 * Mix it into an exception type as a second public base. The node failure
 * classifier ("Node failure contract" in the reference) classifies a failure
 * of such a type NG.ConfigError wherever it appears in the cause chain: it is
 * never retried and never routed to a failure handler.
 * program::ProgramDiagnosticError carries it: a Program diagnostic is a
 * deterministic verdict about a program, its definition or its admission, so
 * running the node again reproduces it, and a failure route would hide a
 * defect behind a handler.
 */
class NEOGRAPH_API ConfigurationFailure {
public:
    virtual ~ConfigurationFailure();

protected:
    ConfigurationFailure() = default;
    ConfigurationFailure(const ConfigurationFailure&) = default;
    ConfigurationFailure& operator=(const ConfigurationFailure&) = default;
};

}  // namespace neograph::graph
