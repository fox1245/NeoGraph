/**
 * @file hook.h
 * @brief Host-admitted, deterministic mandatory runtime hooks.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/tool.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace neograph {

// This is deliberately closed: a new phase changes the hook contract.
enum class HookPhase : std::uint8_t {
    MessageAdmitted,
    BeforeProviderRequest,
    AfterProviderResponse,
    BeforeToolExecution,
    AfterToolExecution,
    CheckpointPublished,
    BeforeTerminalPublication,
    RunFailed,
    RunCancelled,
};
NEOGRAPH_API std::string_view to_string(HookPhase value) noexcept;
NEOGRAPH_API HookPhase hook_phase_from_string(std::string_view value);

enum class HookDelivery : std::uint8_t { BlockingMandatory, DurableObservational };
enum class HookFailureMode : std::uint8_t { FailClosed, Continue };
enum class HookIdempotency : std::uint8_t { Idempotent, NonIdempotent };

/** Immutable, typed event input to the deterministic hook planner. */
struct RuntimeEventData {
    std::string event_id;
    std::uint64_t sequence = 0;
    HookPhase phase = HookPhase::MessageAdmitted;
    std::string type;
    std::string owner_scope;
    std::string run_id;
    json data;
};

class NEOGRAPH_API RuntimeEvent final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;
    static RuntimeEvent create(RuntimeEventData data);
    static RuntimeEvent parse(std::string_view stored_bytes);

    [[nodiscard]] const std::string& id() const noexcept { return data_.event_id; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return data_.sequence; }
    [[nodiscard]] HookPhase phase() const noexcept { return data_.phase; }
    [[nodiscard]] const std::string& type() const noexcept { return data_.type; }
    [[nodiscard]] const std::string& owner_scope() const noexcept { return data_.owner_scope; }
    [[nodiscard]] const std::string& run_id() const noexcept { return data_.run_id; }
    [[nodiscard]] const json& data() const noexcept { return data_.data; }
    [[nodiscard]] std::string serialize_canonical() const;

private:
    explicit RuntimeEvent(RuntimeEventData data) : data_(std::move(data)) {}
    RuntimeEventData data_;
};

enum class HookPredicateKind : std::uint8_t { True, All, Any, Not, Equals, Exists };
enum class RuntimeEventField : std::uint8_t { Type, OwnerScope, RunId, DataPointer };

/** Bounded declarative predicate AST. DataPointer is an RFC 6901 pointer below /data. */
struct HookPredicate {
    HookPredicateKind kind = HookPredicateKind::True;
    RuntimeEventField field = RuntimeEventField::Type;
    std::string pointer;
    json value;
    std::vector<HookPredicate> children;
};

enum class HookInputMapperKind : std::uint8_t { JsonPointer, Template, HostMapper };
/** Template leaves are literals or {"$event":"/data/..."}; no expressions are evaluated. */
struct HookInputMapper {
    HookInputMapperKind kind = HookInputMapperKind::JsonPointer;
    std::string json_pointer;
    json value_template;
    std::string host_mapper_id;
};

struct HookDefinitionData {
    std::string definition_id;
    std::uint32_t priority = 0;
    HookPhase phase = HookPhase::MessageAdmitted;
    HookDelivery delivery = HookDelivery::BlockingMandatory;
    HookFailureMode failure_mode = HookFailureMode::FailClosed;
    HookIdempotency idempotency = HookIdempotency::Idempotent;
    ToolEffectClass effect = ToolEffectClass::ReadOnly;
    std::string target_id;
    std::set<std::string> required_capabilities;
    HookPredicate predicate;
    HookInputMapper input_mapper;
};

class NEOGRAPH_API HookDefinition final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;
    static HookDefinition create(HookDefinitionData data);
    static HookDefinition parse(std::string_view stored_bytes);
    [[nodiscard]] const std::string& id() const noexcept { return data_.definition_id; }
    [[nodiscard]] const HookDefinitionData& data() const noexcept { return data_; }
    [[nodiscard]] std::string serialize_canonical() const;

private:
    explicit HookDefinition(HookDefinitionData data) : data_(std::move(data)) {}
    HookDefinitionData data_;
};

struct HookInvocationData {
    std::string invocation_id;
    std::string definition_id;
    std::string event_id;
    std::string target_id;
    HookPhase phase = HookPhase::MessageAdmitted;
    HookDelivery delivery = HookDelivery::BlockingMandatory;
    HookFailureMode failure_mode = HookFailureMode::FailClosed;
    HookIdempotency idempotency = HookIdempotency::Idempotent;
    ToolEffectClass effect = ToolEffectClass::ReadOnly;
    std::set<std::string> required_capabilities;
    std::string host_mapper_id;
    json arguments;
};

class NEOGRAPH_API HookInvocation final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;
    static HookInvocation create(HookInvocationData data);
    static HookInvocation parse(std::string_view stored_bytes);
    [[nodiscard]] const std::string& id() const noexcept { return data_.invocation_id; }
    [[nodiscard]] const HookInvocationData& data() const noexcept { return data_; }
    [[nodiscard]] std::string serialize_canonical() const;

private:
    explicit HookInvocation(HookInvocationData data) : data_(std::move(data)) {}
    HookInvocationData data_;
};

/** The host's admitted target contract. Hook definitions can only attenuate it. */
struct HookTargetContract {
    Tool* tool = nullptr;
    std::set<std::string> capabilities;
    ToolEffectClass maximum_effect = ToolEffectClass::Unknown;
};
using HookTargetResolver = std::function<std::optional<HookTargetContract>(std::string_view)>;
using HookHostMapper = std::function<json(std::string_view, const RuntimeEvent&)>;

class NEOGRAPH_API HookRegistry final {
public:
    HookRegistry(HookTargetResolver target_resolver,
                 std::set<std::string> admitted_host_mapper_ids = {});
    void admit(HookDefinition definition);
    [[nodiscard]] std::vector<HookInvocation> plan(const RuntimeEvent& event) const;

private:
    HookTargetResolver target_resolver_;
    std::set<std::string> admitted_host_mapper_ids_;
    std::vector<HookDefinition> definitions_;
};

/** Native-only execution bridge. It never interprets model ToolCall values.
 * A supplied identity.request_id is retained as a durable idempotency key. */
class NEOGRAPH_API NativeHookExecutionAdapter final {
public:
    NativeHookExecutionAdapter(HookTargetResolver target_resolver,
                               std::shared_ptr<ToolExecutionController> controller,
                               HookHostMapper host_mapper = {},
                               std::set<std::string> admitted_host_mapper_ids = {});
    asio::awaitable<ToolExecutionResult> execute_result_async(
        const HookInvocation& invocation, const RuntimeEvent& event,
        ToolExecutionContext context = {}) const;

private:
    HookTargetResolver target_resolver_;
    std::shared_ptr<ToolExecutionController> controller_;
    HookHostMapper host_mapper_;
    std::set<std::string> admitted_host_mapper_ids_;
};

} // namespace neograph
