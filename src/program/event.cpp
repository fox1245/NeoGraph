#include <neograph/program/event.h>
#include "canonical_json.h"
#include "../core/json_builder.h"
#include "node_retry_budget.h"
#include <limits>
#include <stdexcept>
#include <utility>
namespace neograph::program {namespace{
template<class...T>struct O:T...{using T::operator()...;};template<class...T>O(T...)->O<T...>;constexpr std::string_view FORMAT="neograph-program-event";
std::string rs(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k)||!v[k].is_string())throw std::invalid_argument("Program event string field invalid");return v[k].get<std::string>();}std::uint64_t ru(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k)||!v[k].is_number_unsigned())throw std::invalid_argument("Program event unsigned field invalid");return v[k].get<std::uint64_t>();}std::uint32_t r32(const json&v,std::string_view k){auto n=ru(v,k);if(n>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("Program event integer out of range");return static_cast<std::uint32_t>(n);}std::int64_t ri(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k)||!v[k].is_number_integer())throw std::invalid_argument("Program event integer field invalid");return v[k].get<std::int64_t>();}json rv(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k))throw std::invalid_argument("Program event field missing");return v[k];}std::optional<std::string>os(const json&v,std::string_view k){auto x=rv(v,k);if(x.is_null())return{};if(!x.is_string())throw std::invalid_argument("Program event optional string invalid");return x.get<std::string>();}std::optional<int>oi(const json&v,std::string_view k){auto x=rv(v,k);if(x.is_null())return{};if(!x.is_number_integer())throw std::invalid_argument("Program event optional int invalid");auto n=x.get<std::int64_t>();if(n<std::numeric_limits<int>::min()||n>std::numeric_limits<int>::max())throw std::invalid_argument("Program event optional int out of range");return static_cast<int>(n);}
using Builder=::neograph::detail::JsonBuilder;using BV=Builder::Value;
// Event bodies are built in one document (#478); each make() takes the
// argument the json initializer list took, so the members keep their types.
BV eb(Builder&j,const RunBudget&b){auto*e=j.object({{"wall_time_ms",j.make(b.wall_time_ms)},{"model_tokens",j.make(b.model_tokens)},{"monetary_microunits",j.make(b.monetary_microunits)},{"max_concurrency",j.make(b.max_concurrency)},{"max_program_operations",j.make(b.max_program_operations)},{"max_core_steps",j.make(b.max_core_steps)},{"max_dynamic_compiles",j.make(b.max_dynamic_compiles)},{"max_child_depth",j.make(b.max_child_depth)},{"max_total_children",j.make(b.max_total_children)}});detail::encode_node_retries(j,e,b);return e;}RunBudget db(const json&v){RunBudget b{ru(v,"wall_time_ms"),ru(v,"model_tokens"),ru(v,"monetary_microunits"),r32(v,"max_concurrency"),ru(v,"max_program_operations"),ru(v,"max_core_steps"),ru(v,"max_dynamic_compiles"),r32(v,"max_child_depth"),ru(v,"max_total_children")};b.max_node_retries=detail::decode_node_retries(v);return b;}BV ec(Builder&j,const CoreCheckpointIdentity&c){return j.object({{"core_name",j.make(c.core_name)},{"core_generation_id",j.make(c.core_generation_id)},{"core_thread_id",j.make(c.core_thread_id)},{"checkpoint_id",j.make(c.checkpoint_id)},{"checkpoint_schema_version",j.make(c.checkpoint_schema_version)}});}CoreCheckpointIdentity dc(const json&v){return{rs(v,"core_name"),rs(v,"core_generation_id"),rs(v,"core_thread_id"),rs(v,"checkpoint_id"),r32(v,"checkpoint_schema_version")};}
void validate_checkpoint(const CoreCheckpointIdentity& checkpoint) {
    detail::validate_token(checkpoint.core_name, "Program event checkpoint core_name");
    if (!detail::is_sha256_identity(checkpoint.core_generation_id)) {
        throw std::invalid_argument(
            "Program event checkpoint generation must be a sha256 identity");
    }
    detail::validate_token(
        checkpoint.core_thread_id, "Program event checkpoint core_thread_id");
    detail::validate_token(
        checkpoint.checkpoint_id, "Program event checkpoint checkpoint_id");
    if (checkpoint.checkpoint_schema_version == 0) {
        throw std::invalid_argument(
            "Program event checkpoint schema version must be positive");
    }
}
std::string_view rt(graph::GraphEvent::Type t){switch(t){case graph::GraphEvent::Type::NODE_START:return"node_start";case graph::GraphEvent::Type::NODE_END:return"node_end";case graph::GraphEvent::Type::LLM_TOKEN:return"llm_token";case graph::GraphEvent::Type::CHANNEL_WRITE:return"channel_write";case graph::GraphEvent::Type::INTERRUPT:return"interrupt";case graph::GraphEvent::Type::ERROR:return"error";}return"unknown";}graph::GraphEvent::Type drt(std::string_view t){if(t=="node_start")return graph::GraphEvent::Type::NODE_START;if(t=="node_end")return graph::GraphEvent::Type::NODE_END;if(t=="llm_token")return graph::GraphEvent::Type::LLM_TOKEN;if(t=="channel_write")return graph::GraphEvent::Type::CHANNEL_WRITE;if(t=="interrupt")return graph::GraphEvent::Type::INTERRUPT;if(t=="error")return graph::GraphEvent::Type::ERROR;throw std::invalid_argument("Unknown raw event type");}
BV eg(Builder&j,const graph::TypedGraphEvent&e){return std::visit(O{[&](const graph::NodeStartEvent&x){return j.object({{"type",j.make("node_start")},{"node",j.make(x.node_name)},{"retry",x.retry_attempt?j.make(*x.retry_attempt):j.null()},{"data",j.make(x.data)}});},[&](const graph::NodeEndEvent&x){return j.object({{"type",j.make("node_end")},{"node",j.make(x.node_name)},{"goto",x.command_goto?j.make(*x.command_goto):j.null()},{"sends",j.make(x.send_count)},{"data",j.make(x.data)}});},[&](const graph::LlmTokenEvent&x){return j.object({{"type",j.make("llm_token")},{"node",j.make(x.node_name)},{"token",j.make(x.token)}});},[&](const graph::ChannelWriteEvent&x){return j.object({{"type",j.make("channel_write")},{"node",j.make(x.node_name)},{"channel",j.make(x.channel)},{"value",j.make(x.value)}});},[&](const graph::StateSnapshotEvent&x){return j.object({{"type",j.make("state_snapshot")},{"state",j.make(x.state)}});},[&](const graph::RoutingEvent&x){return j.object({{"type",j.make("routing")},{"goto",x.command_goto?j.make(*x.command_goto):j.null()},{"nodes",j.make(x.next_nodes)},{"step",x.step?j.make(*x.step):j.null()},{"data",j.make(x.data)}});},[&](const graph::SendDispatchEvent&x){auto*a=j.array();for(auto&s:x.sends)j.push(a,j.object({{"target",j.make(s.target_node)},{"input",j.make(s.input)},{"source",j.make(s.source_node)}}));return j.object({{"type",j.make("send_dispatch")},{"items",a}});},[&](const graph::InterruptEvent&x){return j.object({{"type",j.make("interrupt")},{"node",j.make(x.node_name)},{"phase",x.phase?j.make(*x.phase):j.null()},{"checkpoint",x.checkpoint_id?j.make(*x.checkpoint_id):j.null()},{"data",j.make(x.data)}});},[&](const graph::ErrorEvent&x){return j.object({{"type",j.make("error")},{"node",j.make(x.node_name)},{"message",j.make(x.message)},{"data",j.make(x.data)}});},[&](const graph::RawGraphEvent&x){return j.object({{"type",j.make("raw")},{"raw_type",j.make(std::string(rt(x.event.type)))},{"node",j.make(x.event.node_name)},{"data",j.make(x.event.data)}});}},e);}
graph::TypedGraphEvent dg(const json& v) {
    const auto t = rs(v, "type");
    if (t == "node_start") {
        detail::reject_unknown_fields(
            v, "node_start event", {"type", "node", "retry", "data"});
        return graph::NodeStartEvent{rs(v, "node"), oi(v, "retry"), rv(v, "data")};
    }
    if (t == "node_end") {
        detail::reject_unknown_fields(
            v, "node_end event", {"type", "node", "goto", "sends", "data"});
        return graph::NodeEndEvent{
            rs(v, "node"), os(v, "goto"),
            static_cast<std::size_t>(ru(v, "sends")), rv(v, "data")};
    }
    if (t == "llm_token") {
        detail::reject_unknown_fields(
            v, "llm_token event", {"type", "node", "token"});
        return graph::LlmTokenEvent{rs(v, "node"), rs(v, "token")};
    }
    if (t == "channel_write") {
        detail::reject_unknown_fields(
            v, "channel_write event", {"type", "node", "channel", "value"});
        return graph::ChannelWriteEvent{
            rs(v, "node"), rs(v, "channel"), rv(v, "value")};
    }
    if (t == "state_snapshot") {
        detail::reject_unknown_fields(v, "state snapshot event", {"type", "state"});
        return graph::StateSnapshotEvent{rv(v, "state")};
    }
    if (t == "routing") {
        detail::reject_unknown_fields(
            v, "routing event", {"type", "goto", "nodes", "step", "data"});
        const auto& nodes = rv(v, "nodes");
        if (!nodes.is_array()) throw std::invalid_argument("routing nodes must be an array");
        std::vector<std::string> parsed;
        for (const auto& node : nodes) {
            if (!node.is_string()) throw std::invalid_argument("routing node invalid");
            parsed.push_back(node.get<std::string>());
        }
        return graph::RoutingEvent{
            os(v, "goto"), std::move(parsed), oi(v, "step"), rv(v, "data")};
    }
    if (t == "send_dispatch") {
        detail::reject_unknown_fields(v, "send dispatch event", {"type", "items"});
        const auto& items = rv(v, "items");
        if (!items.is_array()) throw std::invalid_argument("send items must be an array");
        graph::SendDispatchEvent event;
        for (const auto& item : items) {
            if (!item.is_object()) throw std::invalid_argument("send item must be an object");
            detail::reject_unknown_fields(
                item, "send item", {"target", "input", "source"});
            event.sends.push_back(
                {rs(item, "target"), rv(item, "input"), rs(item, "source")});
        }
        return event;
    }
    if (t == "interrupt") {
        detail::reject_unknown_fields(
            v, "interrupt event",
            {"type", "node", "phase", "checkpoint", "data"});
        return graph::InterruptEvent{
            rs(v, "node"), os(v, "phase"), os(v, "checkpoint"), rv(v, "data")};
    }
    if (t == "error") {
        detail::reject_unknown_fields(
            v, "error event", {"type", "node", "message", "data"});
        return graph::ErrorEvent{
            rs(v, "node"), rs(v, "message"), rv(v, "data")};
    }
    if (t == "raw") {
        detail::reject_unknown_fields(
            v, "raw event", {"type", "raw_type", "node", "data"});
        return graph::RawGraphEvent{
            {drt(rs(v, "raw_type")), rs(v, "node"), rv(v, "data")}};
    }
    throw std::invalid_argument("Unknown core event type");
}
BV ep(Builder& j, const ProgramEvent& e) {
    switch (e.kind) {
        case ProgramEventKind::Started:
            if (const auto* payload = std::get_if<ProgramStartedEvent>(&e.payload)) {
                return eb(j, payload->budget);
            }
            break;
        case ProgramEventKind::OperationStarted:
            if (const auto* payload = std::get_if<ProgramOperationEvent>(&e.payload))
                return j.object({{"operation_kind", j.make(payload->operation_kind)},
                                 {"input_digest", j.make(payload->input_digest)}});
            break;
        case ProgramEventKind::Core:
            if (const auto* payload =
                    std::get_if<graph::TypedGraphEvent>(&e.payload)) {
                return eg(j, *payload);
            }
            break;
        case ProgramEventKind::Emit:
            if (const auto* payload = std::get_if<ProgramEmitEvent>(&e.payload)) {
                return j.object({{"operation_id", j.make(payload->operation_id)},
                                 {"value", j.make(payload->value)}});
            }
            break;
        case ProgramEventKind::CheckpointPublished:
            if (const auto* payload =
                    std::get_if<ProgramCheckpointEvent>(&e.payload)) {
                return ec(j, payload->checkpoint);
            }
            break;
        case ProgramEventKind::Terminal:
            if (const auto* payload =
                    std::get_if<ProgramTerminalEvent>(&e.payload)) {
                return j.object({{"status", j.make(std::string(to_string(payload->status)))}});
            }
            break;
    }
    throw std::invalid_argument("Program event kind/payload mismatch");
}
ProgramEventPayload dp(ProgramEventKind kind, const json& v) {
    switch (kind) {
        case ProgramEventKind::Started:
            detail::reject_unknown_fields(
                v, "Program started event",
                {"wall_time_ms", "model_tokens", "monetary_microunits",
                 "max_concurrency", "max_program_operations", "max_core_steps",
                 "max_dynamic_compiles", "max_child_depth", "max_total_children",
                 "max_node_retries"});
            return ProgramStartedEvent{db(v)};
        case ProgramEventKind::OperationStarted:
            detail::reject_unknown_fields(v, "Program operation event",
                                          {"operation_kind", "input_digest"});
            return ProgramOperationEvent{rs(v, "operation_kind"), rs(v, "input_digest")};
        case ProgramEventKind::Core:
            return dg(v);
        case ProgramEventKind::Emit:
            detail::reject_unknown_fields(v, "Program emit event", {"operation_id", "value"});
            return ProgramEmitEvent{rs(v, "operation_id"), rv(v, "value")};
        case ProgramEventKind::CheckpointPublished:
            detail::reject_unknown_fields(
                v, "Program checkpoint event",
                {"core_name", "core_generation_id", "core_thread_id",
                 "checkpoint_id", "checkpoint_schema_version"});
            return ProgramCheckpointEvent{dc(v)};
        case ProgramEventKind::Terminal:
            detail::reject_unknown_fields(v, "Program terminal event", {"status"});
            return ProgramTerminalEvent{
                program_terminal_status_from_string(rs(v, "status"))};
    }
    throw std::invalid_argument("Unknown event kind");
}
void val(const ProgramEvent& e) {
    const bool payload_matches =
        (e.kind == ProgramEventKind::Started &&
         std::holds_alternative<ProgramStartedEvent>(e.payload)) ||
        (e.kind == ProgramEventKind::Core &&
         std::holds_alternative<graph::TypedGraphEvent>(e.payload)) ||
        (e.kind == ProgramEventKind::Emit &&
         std::holds_alternative<ProgramEmitEvent>(e.payload)) ||
        (e.kind == ProgramEventKind::CheckpointPublished &&
         std::holds_alternative<ProgramCheckpointEvent>(e.payload)) ||
        (e.kind == ProgramEventKind::Terminal &&
         std::holds_alternative<ProgramTerminalEvent>(e.payload)) ||
        (e.kind == ProgramEventKind::OperationStarted &&
         std::holds_alternative<ProgramOperationEvent>(e.payload));
    if (!payload_matches) {
        throw std::invalid_argument(
            "Program event kind does not match its payload");
    }
    if (!e.sequence || e.timestamp_ms < 0 || !e.attempt) {
        throw std::invalid_argument("Program event counters invalid");
    }
    detail::validate_token(e.run_id, "Program event run_id");
    detail::validate_token(e.operation_id, "Program event operation_id");
    detail::validate_token(e.core_run_id, "Program event core_run_id");
    if (e.kind == ProgramEventKind::Emit) {
        const auto* payload = std::get_if<ProgramEmitEvent>(&e.payload);
        if (!payload) throw std::invalid_argument("Program emit payload missing");
        detail::validate_token(payload->operation_id, "Program emit operation_id");
    }
    if (e.kind == ProgramEventKind::OperationStarted) {
        const auto& payload = std::get<ProgramOperationEvent>(e.payload);
        detail::validate_token(payload.operation_kind, "Program operation kind");
        if (!detail::is_sha256_identity(payload.input_digest))
            throw std::invalid_argument("Program operation input digest must be a sha256 identity");
    }
    if (!e.trace_id.empty()) {
        detail::validate_token(e.trace_id, "Program event trace_id");
    }
    if (!detail::is_sha256_identity(e.program_version_id) ||
        !detail::is_sha256_identity(e.bundle_id) ||
        !detail::is_sha256_identity(e.core_generation_id)) {
        throw std::invalid_argument("Program event identities invalid");
    }
    if (e.kind == ProgramEventKind::CheckpointPublished) {
        const auto* payload = std::get_if<ProgramCheckpointEvent>(&e.payload);
        if (!payload) throw std::invalid_argument("Program checkpoint payload missing");
        validate_checkpoint(payload->checkpoint);
    }
}
json body(const ProgramEvent& e) {
    Builder j;
    auto* root = j.object({{"format", j.make(std::string(FORMAT))},
                           {"storage_schema_version", j.make(ProgramEvent::STORAGE_SCHEMA_VERSION)},
                           {"sequence", j.make(e.sequence)},
                           {"timestamp_ms", j.make(e.timestamp_ms)},
                           {"run_id", j.make(e.run_id)},
                           {"program_version_id", j.make(e.program_version_id)},
                           {"bundle_id", j.make(e.bundle_id)},
                           {"operation_id", j.make(e.operation_id)},
                           {"core_generation_id", j.make(e.core_generation_id)},
                           {"core_run_id", j.make(e.core_run_id)},
                           {"trace_id", j.make(e.trace_id)},
                           {"attempt", j.make(e.attempt)},
                           {"kind", j.make(std::string(to_string(e.kind)))},
                           {"payload", ep(j, e)}});
    return std::move(j).finish(root);
}
std::string hash(const ProgramEvent& e) {
    return detail::sha256_identity(
        "program-event/v1", detail::canonical_json_bytes(body(e)));
}
}
std::string_view to_string(ProgramEventKind kind) noexcept {
    switch (kind) {
        case ProgramEventKind::Started: return "started";
        case ProgramEventKind::Core: return "core";
        case ProgramEventKind::Emit: return "emit";
        case ProgramEventKind::CheckpointPublished: return "checkpoint_published";
        case ProgramEventKind::Terminal: return "terminal";
        case ProgramEventKind::OperationStarted: return "operation_started";
    }
    return "unknown";
}
ProgramEventKind program_event_kind_from_string(std::string_view value) {
    if (value == "started") return ProgramEventKind::Started;
    if (value == "core") return ProgramEventKind::Core;
    if (value == "emit") return ProgramEventKind::Emit;
    if (value == "checkpoint_published") return ProgramEventKind::CheckpointPublished;
    if (value == "terminal") return ProgramEventKind::Terminal;
    if (value == "operation_started") return ProgramEventKind::OperationStarted;
    throw std::invalid_argument("Unknown Program event kind");
}
ProgramEvent ProgramEvent::create(ProgramEvent e){val(e);auto h=hash(e);if(!e.id.empty()&&e.id!=h)throw std::invalid_argument("Program event id mismatch");e.id=h;return e;}ProgramEvent ProgramEvent::parse(std::string_view bytes){auto v=detail::parse_json_strict(bytes);if(!v.is_object()||rs(v,"format")!=FORMAT)throw std::invalid_argument("Stored ProgramEvent format invalid");detail::reject_unknown_fields(v,"Stored ProgramEvent",{"format","storage_schema_version","id","sequence","timestamp_ms","run_id","program_version_id","bundle_id","operation_id","core_generation_id","core_run_id","trace_id","attempt","kind","payload"});if(r32(v,"storage_schema_version")!=STORAGE_SCHEMA_VERSION)throw std::invalid_argument("Stored ProgramEvent schema unsupported");ProgramEvent e;e.id=rs(v,"id");e.sequence=ru(v,"sequence");e.timestamp_ms=ri(v,"timestamp_ms");e.run_id=rs(v,"run_id");e.program_version_id=rs(v,"program_version_id");e.bundle_id=rs(v,"bundle_id");e.operation_id=rs(v,"operation_id");e.core_generation_id=rs(v,"core_generation_id");e.core_run_id=rs(v,"core_run_id");e.trace_id=rs(v,"trace_id");e.attempt=ru(v,"attempt");e.kind=program_event_kind_from_string(rs(v,"kind"));e.payload=dp(e.kind,rv(v,"payload"));return create(std::move(e));}std::string ProgramEvent::serialize_canonical()const{val(*this);auto bytes=detail::canonical_json_bytes(body(*this));if(id!=detail::sha256_identity("program-event/v1",bytes))throw std::invalid_argument("Program event id mismatch");const auto position=bytes.find(",\"kind\":");if(position==std::string::npos)throw std::invalid_argument("Program event canonical body is malformed");bytes.insert(position,",\"id\":\""+id+"\"");return bytes;} }
