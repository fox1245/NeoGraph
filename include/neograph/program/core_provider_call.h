#pragma once

#include <neograph/graph/provider_call_broker.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

/** Exact Program operation requesting a host-owned Core provider broker. */
struct ProgramCoreProviderCallContext {
    std::string_view owner_scope;
    std::string_view program_version_id;
    std::string_view run_id;
    std::string_view operation_id;
    std::uint64_t attempt = 0;
};

/** Host binding for one Program operation. No credential is stored here. */
struct ProgramCoreProviderCallBinding {
    std::string owner_scope;
    std::string program_version_id;
    std::string run_id;
    std::string operation_id;
    std::uint64_t attempt = 0;
    std::shared_ptr<graph::ProviderCallBroker> broker;
};

using ProgramCoreProviderCallResolver = std::function<
    std::optional<ProgramCoreProviderCallBinding>(
        const ProgramCoreProviderCallContext&)>;

}  // namespace neograph::program
