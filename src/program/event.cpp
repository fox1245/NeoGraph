#include <neograph/program/event.h>
#include "canonical_json.h"
#include <limits>
#include <stdexcept>
#include <utility>
namespace neograph::program {namespace{
template<class...T>struct O:T...{using T::operator()...;};template<class...T>O(T...)->O<T...>;constexpr std::string_view FORMAT="neograph-program-event";
std::string rs(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k)||!v[k].is_string())throw std::invalid_argument("Program event string field invalid");return v[k].get<std::string>();}std::uint64_t ru(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k)||!v[k].is_number_unsigned())throw std::invalid_argument("Program event unsigned field invalid");return v[k].get<std::uint64_t>();}std::uint32_t r32(const json&v,std::string_view k){auto n=ru(v,k);if(n>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("Program event integer out of range");return static_cast<std::uint32_t>(n);}std::int64_t ri(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k)||!v[k].is_number_integer())throw std::invalid_argument("Program event integer field invalid");return v[k].get<std::int64_t>();}json rv(const json&v,std::string_view k0){std::string k(k0);if(!v.contains(k))throw std::invalid_argument("Program event field missing");return v[k];}std::optional<std::string>os(const json&v,std::string_view k){auto x=rv(v,k);if(x.is_null())return{};if(!x.is_string())throw std::invalid_argument("Program event optional string invalid");return x.get<std::string>();}std::optional<int>oi(const json&v,std::string_view k){auto x=rv(v,k);if(x.is_null())return{};if(!x.is_number_integer())throw std::invalid_argument("Program event optional int invalid");auto n=x.get<std::int64_t>();if(n<std::numeric_limits<int>::min()||n>std::numeric_limits<int>::max())throw std::invalid_argument("Program event optional int out of range");return static_cast<int>(n);}
json eb(const RunBudget&b){return{{"wall_time_ms",b.wall_time_ms},{"model_tokens",b.model_tokens},{"monetary_microunits",b.monetary_microunits},{"max_concurrency",b.max_concurrency},{"max_program_operations",b.max_program_operations},{"max_core_steps",b.max_core_steps},{"max_dynamic_compiles",b.max_dynamic_compiles},{"max_child_depth",b.max_child_depth},{"max_total_children",b.max_total_children}};}RunBudget db(const json&v){return{ru(v,"wall_time_ms"),ru(v,"model_tokens"),ru(v,"monetary_microunits"),r32(v,"max_concurrency"),ru(v,"max_program_operations"),ru(v,"max_core_steps"),ru(v,"max_dynamic_compiles"),r32(v,"max_child_depth"),ru(v,"max_total_children")};}json ec(const CoreCheckpointIdentity&c){return{{"core_name",c.core_name},{"core_generation_id",c.core_generation_id},{"core_thread_id",c.core_thread_id},{"checkpoint_id",c.checkpoint_id},{"checkpoint_schema_version",c.checkpoint_schema_version}};}CoreCheckpointIdentity dc(const json&v){return{rs(v,"core_name"),rs(v,"core_generation_id"),rs(v,"core_thread_id"),rs(v,"checkpoint_id"),r32(v,"checkpoint_schema_version")};}
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
json eg(const graph::TypedGraphEvent&e){return std::visit(O{[](const graph::NodeStartEvent&x){return json{{"type","node_start"},{"node",x.node_name},{"retry",x.retry_attempt?json(*x.retry_attempt):json(nullptr)},{"data",x.data}};},[](const graph::NodeEndEvent&x){return json{{"type","node_end"},{"node",x.node_name},{"goto",x.command_goto?json(*x.command_goto):json(nullptr)},{"sends",x.send_count},{"data",x.data}};},[](const graph::LlmTokenEvent&x){return json{{"type","llm_token"},{"node",x.node_name},{"token",x.token}};},[](const graph::ChannelWriteEvent&x){return json{{"type","channel_write"},{"node",x.node_name},{"channel",x.channel},{"value",x.value}};},[](const graph::StateSnapshotEvent&x){return json{{"type","state_snapshot"},{"state",x.state}};},[](const graph::RoutingEvent&x){return json{{"type","routing"},{"goto",x.command_goto?json(*x.command_goto):json(nullptr)},{"nodes",x.next_nodes},{"step",x.step?json(*x.step):json(nullptr)},{"data",x.data}};},[](const graph::SendDispatchEvent&x){json a=json::array();for(auto&s:x.sends)a.push_back(json{{"target",s.target_node},{"input",s.input},{"source",s.source_node}});return json{{"type","send_dispatch"},{"items",a}};},[](const graph::InterruptEvent&x){return json{{"type","interrupt"},{"node",x.node_name},{"phase",x.phase?json(*x.phase):json(nullptr)},{"checkpoint",x.checkpoint_id?json(*x.checkpoint_id):json(nullptr)},{"data",x.data}};},[](const graph::ErrorEvent&x){return json{{"type","error"},{"node",x.node_name},{"message",x.message},{"data",x.data}};},[](const graph::RawGraphEvent&x){return json{{"type","raw"},{"raw_type",std::string(rt(x.event.type))},{"node",x.event.node_name},{"data",x.event.data}};}},e);}
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
json ep(const ProgramEvent& e) {
    switch (e.kind) {
        case ProgramEventKind::Started:
            if (const auto* payload = std::get_if<ProgramStartedEvent>(&e.payload)) {
                return eb(payload->budget);
            }
            break;
        case ProgramEventKind::Core:
            if (const auto* payload =
                    std::get_if<graph::TypedGraphEvent>(&e.payload)) {
                return eg(*payload);
            }
            break;
        case ProgramEventKind::Emit:
            if (const auto* payload = std::get_if<ProgramEmitEvent>(&e.payload)) {
                return json{{"operation_id", payload->operation_id}, {"value", payload->value}};
            }
            break;
        case ProgramEventKind::CheckpointPublished:
            if (const auto* payload =
                    std::get_if<ProgramCheckpointEvent>(&e.payload)) {
                return ec(payload->checkpoint);
            }
            break;
        case ProgramEventKind::Terminal:
            if (const auto* payload =
                    std::get_if<ProgramTerminalEvent>(&e.payload)) {
                return json{{"status", std::string(to_string(payload->status))}};
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
                 "max_dynamic_compiles", "max_child_depth", "max_total_children"});
            return ProgramStartedEvent{db(v)};
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
         std::holds_alternative<ProgramTerminalEvent>(e.payload));
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
    return {{"format", std::string(FORMAT)},
            {"storage_schema_version", ProgramEvent::STORAGE_SCHEMA_VERSION},
            {"sequence", e.sequence},
            {"timestamp_ms", e.timestamp_ms},
            {"run_id", e.run_id},
            {"program_version_id", e.program_version_id},
            {"bundle_id", e.bundle_id},
            {"operation_id", e.operation_id},
            {"core_generation_id", e.core_generation_id},
            {"core_run_id", e.core_run_id},
            {"trace_id", e.trace_id},
            {"attempt", e.attempt},
            {"kind", std::string(to_string(e.kind))},
            {"payload", ep(e)}};
}
std::string hash(const ProgramEvent& e) {
    return detail::sha256_identity(
        "program-event/v1", detail::canonical_json_bytes(body(e)));
}
}std::string_view to_string(ProgramEventKind k)noexcept{switch(k){case ProgramEventKind::Started:return"started";case ProgramEventKind::Core:return"core";case ProgramEventKind::Emit:return"emit";case ProgramEventKind::CheckpointPublished:return"checkpoint_published";case ProgramEventKind::Terminal:return"terminal";}return"unknown";}ProgramEventKind program_event_kind_from_string(std::string_view v){if(v=="started")return ProgramEventKind::Started;if(v=="core")return ProgramEventKind::Core;if(v=="emit")return ProgramEventKind::Emit;if(v=="checkpoint_published")return ProgramEventKind::CheckpointPublished;if(v=="terminal")return ProgramEventKind::Terminal;throw std::invalid_argument("Unknown Program event kind");}
ProgramEvent ProgramEvent::create(ProgramEvent e){val(e);auto h=hash(e);if(!e.id.empty()&&e.id!=h)throw std::invalid_argument("Program event id mismatch");e.id=h;return e;}ProgramEvent ProgramEvent::parse(std::string_view bytes){auto v=detail::parse_json_strict(bytes);if(!v.is_object()||rs(v,"format")!=FORMAT)throw std::invalid_argument("Stored ProgramEvent format invalid");detail::reject_unknown_fields(v,"Stored ProgramEvent",{"format","storage_schema_version","id","sequence","timestamp_ms","run_id","program_version_id","bundle_id","operation_id","core_generation_id","core_run_id","trace_id","attempt","kind","payload"});if(r32(v,"storage_schema_version")!=STORAGE_SCHEMA_VERSION)throw std::invalid_argument("Stored ProgramEvent schema unsupported");ProgramEvent e;e.id=rs(v,"id");e.sequence=ru(v,"sequence");e.timestamp_ms=ri(v,"timestamp_ms");e.run_id=rs(v,"run_id");e.program_version_id=rs(v,"program_version_id");e.bundle_id=rs(v,"bundle_id");e.operation_id=rs(v,"operation_id");e.core_generation_id=rs(v,"core_generation_id");e.core_run_id=rs(v,"core_run_id");e.trace_id=rs(v,"trace_id");e.attempt=ru(v,"attempt");e.kind=program_event_kind_from_string(rs(v,"kind"));e.payload=dp(e.kind,rv(v,"payload"));return create(std::move(e));}std::string ProgramEvent::serialize_canonical()const{val(*this);auto bytes=detail::canonical_json_bytes(body(*this));if(id!=detail::sha256_identity("program-event/v1",bytes))throw std::invalid_argument("Program event id mismatch");const auto position=bytes.find(",\"kind\":");if(position==std::string::npos)throw std::invalid_argument("Program event canonical body is malformed");bytes.insert(position,",\"id\":\""+id+"\"");return bytes;} }
