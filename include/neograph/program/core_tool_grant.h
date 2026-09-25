#pragma once

#include <neograph/tool_dispatch.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph { class ToolExecutionController; }
namespace neograph { class ToolEffectBroker; }

namespace neograph::program {

/** Immutable Program identity presented to a host before one Core operation. */
struct ProgramCoreToolGrantContext {
    std::string_view owner_scope;
    std::string_view program_version_id;
    std::string_view run_id;
    std::string_view operation_id;
    std::uint64_t attempt = 0;
};

/**
 * Host-owned authority for mediated Core tool dispatch in one exact Program run.
 * A capability binding receipt alone cannot supply this grant. The host must
 * recover its durable grant record before returning one after process restart.
 * Native GraphNode code that invokes Tool::execute directly is outside this
 * boundary and must be admitted as trusted effectful code separately.
 */
struct ProgramCoreToolGrant {
    std::string owner_scope;
    std::string program_version_id;
    std::string run_id;
    std::string operation_id;
    std::uint64_t attempt = 0;
    std::string grant_id;
    ToolGate gate;
    std::shared_ptr<ToolExecutionController> controller;
    /// Optional host-owned per-call effect journal for this exact grant.
    std::shared_ptr<ToolEffectBroker> effect_broker;
};

using ProgramCoreToolGrantResolver =
    std::function<std::optional<ProgramCoreToolGrant>(const ProgramCoreToolGrantContext&)>;

}  // namespace neograph::program
