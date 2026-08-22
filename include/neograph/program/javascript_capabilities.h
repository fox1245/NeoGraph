/** @file program/javascript_capabilities.h @brief Machine-readable QuickJS authoring surface. */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

namespace neograph::program {

/**
 * Return the closed, versioned JavaScript authoring vocabulary.
 *
 * The value describes the compile-time graph builder, durable generator
 * commands, signatures, classifications, and source-profile constraints. It
 * grants no runtime authority and is safe to provide to model synthesis and
 * topology tooling.
 */
NEOGRAPH_PROGRAM_API json javascript_authoring_capability_manifest();

}  // namespace neograph::program
