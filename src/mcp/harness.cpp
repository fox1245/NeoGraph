#include <neograph/graph/checkpoint.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/validator.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/json_schema.h>
#include <neograph/mcp/server.h>
#include <neograph/provider.h>

#include "harness_journal_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neograph::mcp {
namespace {

constexpr const char* kWorkerNodeType = "neograph_harness_worker";
constexpr const char* kJudgeNodeType = "neograph_harness_judge";
constexpr const char* kTasksExtension = "io.modelcontextprotocol/tasks";
constexpr const char* kHarnessProfile = "harness-m4";

int64_t unix_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string revision_digest(const json& request, const json& core) {
    const auto canonical = json({{"core", core},
                                 {"profile", kHarnessProfile},
                                 {"protocol_version", MCP_PROTOCOL_VERSION},
                                 {"request", request}})
                               .dump();
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : canonical) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::string iso8601(int64_t millis) {
    const auto seconds = static_cast<std::time_t>(millis / 1000);
    std::tm    utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << (millis % 1000) << 'Z';
    return out.str();
}

bool tasks_requested(const json& meta) {
    if (!meta.is_object()) return false;
    const auto capabilities =
        meta.value("io.modelcontextprotocol/clientCapabilities", json::object());
    return capabilities.is_object() &&
           capabilities.value("extensions", json::object()).contains(kTasksExtension);
}

void require_tasks_negotiated(const json& context) {
    if (!context.is_object() ||
        !context.value("extensions", json::object()).contains(kTasksExtension)) {
        throw std::invalid_argument("MCP Tasks extension was not negotiated during initialize");
    }
}

json structured_text_content(const json& structured, std::string summary) {
    // MCP 2025-11-25 recommends a serialized JSON TextContent fallback for
    // clients that do not surface structuredContent.
    // https://modelcontextprotocol.io/specification/2025-11-25/server/tools#structured-content
    return json::array({
        {{"type", "text"}, {"text", std::move(summary)}},
        {{"type", "text"}, {"text", structured.dump()}},
    });
}

json task_from_snapshot(const json& snapshot, bool creation = false) {
    const auto  harness_status = snapshot.value("status", "failed");
    std::string status         = "working";
    if (harness_status == "awaiting_tool_results" || harness_status == "input_required")
        status = "input_required";
    else if (harness_status == "completed")
        status = "completed";
    else if (harness_status == "cancelled")
        status = "cancelled";
    else if (harness_status != "queued" && harness_status != "running") {
        status = "failed";
    }
    const auto created = snapshot.value("created_at", int64_t{0});
    const auto updated = snapshot.value("updated_at", created);
    const auto expires = snapshot.value("expires_at", int64_t{0});
    json       task    = {
        {"resultType", creation ? "task" : "complete"},
        {"taskId", snapshot.at("run_id")},
        {"status", status},
        {"createdAt", iso8601(created)},
        {"lastUpdatedAt", iso8601(updated)},
        {"ttlMs", expires > 0 ? json(expires - created) : json(nullptr)},
        {"pollIntervalMs", snapshot.value("poll_after_ms", int64_t{1000})},
    };
    if (status == "input_required") {
        const auto pending    = snapshot.at("pending");
        task["statusMessage"] = harness_status == "input_required"
                                    ? "Harness worker requires host input"
                                    : "Harness worker awaits a host-brokered tool result";
        task["inputRequests"] = {
            {pending.at("call_id").get<std::string>(),
             {
                 {"method", "elicitation/create"},
                 {"params",
                  {
                      {"message",
                       "Provide result for " + pending.value("tool_id", "host capability")},
                      {"requestedSchema", pending.value("result_schema", json::object())},
                      {"_meta",
                       {
                           {"toolId", pending.value("tool_id", "")},
                           {"arguments", pending.value("arguments", json::object())},
                       }},
                  }},
             }},
        };
    } else if (status == "completed") {
        CallToolResult result;
        result.content            = structured_text_content(snapshot, "Harness run completed");
        result.structured_content = snapshot;
        task["result"]            = result.to_json();
    } else if (status == "failed") {
        task["statusMessage"] = snapshot.value("error", "Harness run did not complete");
        task["error"]         = {
            {"code", -32000},
            {"message", task["statusMessage"]},
        };
    }
    return task;
}

json harness_request_schema() {
    return json::parse(R"JSON({
        "type":"object",
        "required":["task","harness","workers"],
        "properties":{
            "task":{"type":"object","required":["objective"],"properties":{
                "objective":{"type":"string"},
                "acceptance":{"type":"array","items":{"type":"string"}}
            },"additionalProperties":true},
            "harness":{"type":"object","required":["mode"],"properties":{
                "mode":{"enum":["preset","dsl"]},
                "preset":{"type":"string"},
                "definition":{"type":"object"}
            },"additionalProperties":false},
            "workers":{"type":"array","items":{"type":"object","required":["id","instructions","output_schema"],"properties":{
                "id":{"type":"string"},
                "instructions":{"type":"string"},
                "tools":{"type":"array","items":{"type":"string"}},
                "output_schema":{"type":"object"}
            },"additionalProperties":false}},
            "tool_catalog":{"type":"array","items":{"type":"object","required":["id","description","input_schema","executor"],"properties":{
                "id":{"type":"string"},
                "description":{"type":"string"},
                "input_schema":{"type":"object"},
                "output_schema":{"type":"object"},
                "read_only":{"type":"boolean"},
                "path_arguments":{"type":"array","items":{"type":"string"}},
                "executor":{"type":"object","required":["kind"],"properties":{
                    "kind":{"enum":["builtin","mcp","a2a","host_brokered","script"]},
                    "tool":{"type":"string"},
                    "server_ref":{"type":"string"},
                    "agent":{"type":"string"},
                    "interaction":{"enum":["tool_result","input"]}
                },"additionalProperties":true}
            },"additionalProperties":false}},
            "budgets":{"type":"object","properties":{
                "max_steps":{"type":"integer"},
                "timeout_seconds":{"type":"integer"},
                "max_parallel_workers":{"type":"integer"},
                "max_worker_retries":{"type":"integer"}
            },"additionalProperties":false},
            "policy":{"type":"object","properties":{
                "read_only":{"type":"boolean"},
                "workspace_roots":{"type":"array","items":{"type":"string"}},
                "evidence_required":{"type":"array","items":{"type":"string"}}
            },"additionalProperties":false}
        },
        "additionalProperties":false
    })JSON");
}

json compile_output_schema() {
    return json::parse(R"JSON({
        "type":"object","required":["ok","diagnostics","artifacts"],
        "properties":{
            "ok":{"type":"boolean"},
            "artifact_id":{"type":"string"},
            "diagnostics":{"type":"array"},
            "artifacts":{"type":"object"}
        },"additionalProperties":true
    })JSON");
}

json run_snapshot_schema() {
    return json::parse(R"JSON({
        "type":"object","required":["run_id","status"],
        "properties":{
            "run_id":{"type":"string"},
            "artifact_id":{"type":"string"},
            "revision_digest":{"type":"string"},
            "protocol_version":{"type":"string"},
            "profile":{"type":"string"},
            "execution_mode":{"enum":["live","live_replay","recorded_replay","compatible_fork"]},
            "source_run_id":{"type":"string"},
            "source_checkpoint_id":{"type":"string"},
            "status":{"type":"string"},
            "created_at":{"type":"integer"},
            "updated_at":{"type":"integer"},
            "expires_at":{"type":"integer"},
            "poll_after_ms":{"type":"integer"},
            "pending":{"type":"object"},
            "result":{"type":"object"},
            "error":{"type":"string"}
        },"additionalProperties":true
    })JSON");
}

json checkpoint_summary(const graph::Checkpoint& checkpoint) {
    json channel_names = json::array();
    if (checkpoint.channel_values.is_object() && checkpoint.channel_values.contains("channels") &&
        checkpoint.channel_values["channels"].is_object()) {
        for (const auto [name, value] : checkpoint.channel_values["channels"].items()) {
            (void)value;
            channel_names.push_back(name);
        }
    }
    return {
        {"checkpoint_id", checkpoint.id},
        {"parent_checkpoint_id", checkpoint.parent_id},
        {"current_node", checkpoint.current_node},
        {"next_nodes", checkpoint.next_nodes},
        {"phase", graph::to_string(checkpoint.interrupt_phase)},
        {"step", checkpoint.step},
        {"timestamp", checkpoint.timestamp},
        {"schema_version", checkpoint.schema_version},
        {"channel_names", std::move(channel_names)},
    };
}

std::map<std::string, json> checkpoint_channels(const graph::Checkpoint& checkpoint) {
    std::map<std::string, json> channels;
    if (!checkpoint.channel_values.is_object() || !checkpoint.channel_values.contains("channels") ||
        !checkpoint.channel_values["channels"].is_object()) {
        return channels;
    }
    for (const auto [name, value] : checkpoint.channel_values["channels"].items()) {
        channels.emplace(name, value);
    }
    return channels;
}

void add_channel_snapshot(json&       entry,
                          const char* value_key,
                          const char* version_key,
                          const json& channel) {
    if (!channel.is_object()) {
        entry[value_key] = channel;
        return;
    }
    entry[value_key] = channel.value("value", json());
    if (channel.contains("version")) entry[version_key] = channel["version"];
}

json checkpoint_diff(const graph::Checkpoint&                checkpoint,
                     const std::optional<graph::Checkpoint>& parent) {
    if (!checkpoint.parent_id.empty() && !parent) {
        return {
            {"checkpoint_id", checkpoint.id},
            {"parent_checkpoint_id", checkpoint.parent_id},
            {"node_id", checkpoint.current_node},
            {"phase", graph::to_string(checkpoint.interrupt_phase)},
            {"step", checkpoint.step},
            {"timestamp", checkpoint.timestamp},
            {"parent_available", false},
            {"changed_channels", json::array()},
        };
    }
    const auto before = parent ? checkpoint_channels(*parent) : std::map<std::string, json>{};
    const auto after  = checkpoint_channels(checkpoint);
    std::set<std::string> names;
    for (const auto& [name, value] : before) {
        (void)value;
        names.insert(name);
    }
    for (const auto& [name, value] : after) {
        (void)value;
        names.insert(name);
    }

    json changed = json::array();
    for (const auto& name : names) {
        const auto before_it = before.find(name);
        const auto after_it  = after.find(name);
        if (before_it != before.end() && after_it != after.end() &&
            before_it->second == after_it->second) {
            continue;
        }
        json entry = {{"channel", name}};
        if (before_it == before.end()) {
            entry["operation"] = "added";
            entry["before"]    = nullptr;
        } else {
            entry["operation"] = after_it == after.end() ? "removed" : "changed";
            add_channel_snapshot(entry, "before", "before_version", before_it->second);
        }
        if (after_it == after.end()) {
            entry["after"] = nullptr;
        } else {
            add_channel_snapshot(entry, "after", "after_version", after_it->second);
        }
        changed.push_back(std::move(entry));
    }
    return {
        {"checkpoint_id", checkpoint.id},
        {"parent_checkpoint_id", checkpoint.parent_id},
        {"node_id", checkpoint.current_node},
        {"phase", graph::to_string(checkpoint.interrupt_phase)},
        {"step", checkpoint.step},
        {"timestamp", checkpoint.timestamp},
        {"parent_available", true},
        {"changed_channels", std::move(changed)},
    };
}

std::size_t parse_view_size(const std::string& value, const char* name) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isdigit(character);
        })) {
        throw std::invalid_argument(std::string("invalid Harness ") + name);
    }
    std::size_t        parsed = 0;
    unsigned long long number = 0;
    try {
        number = std::stoull(value, &parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid Harness ") + name);
    }
    if (parsed != value.size() || number > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string("invalid Harness ") + name);
    }
    return static_cast<std::size_t>(number);
}

json journal_page(const std::shared_ptr<HarnessJournal>& journal,
                  const std::string&                     run_id,
                  std::size_t                            after_sequence,
                  std::size_t                            limit,
                  bool                                   attempts_only) {
    json        events              = json::array();
    std::size_t scan_cursor         = after_sequence;
    std::size_t next_after_sequence = after_sequence;
    bool        has_more            = false;
    if (journal) {
        while (true) {
            const auto batch = journal->list_events(run_id, scan_cursor, 1000);
            if (batch.empty()) break;
            for (const auto& event : batch) {
                scan_cursor           = event["sequence"].get<std::size_t>();
                const auto event_type = event.value("event_type", "");
                if (attempts_only && event_type.rfind("worker.attempt.", 0) != 0) continue;
                if (events.size() == limit) {
                    has_more = true;
                    break;
                }
                events.push_back(event);
                next_after_sequence = scan_cursor;
            }
            if (has_more || batch.size() < 1000) break;
        }
        if (!has_more && events.empty()) next_after_sequence = scan_cursor;
    }
    return {
        {"events", std::move(events)},
        {"journal_available", static_cast<bool>(journal)},
        {"after_sequence", after_sequence},
        {"next_after_sequence", next_after_sequence},
        {"has_more", has_more},
    };
}

json worker_result_schema() {
    return json::parse(R"JSON({
        "type":"object","required":["worker_id","status","attempts"],
        "properties":{
            "worker_id":{"type":"string"},
            "status":{"enum":["valid","failed"]},
            "attempts":{"type":"integer"},
            "output":{},
            "failure_kind":{"enum":["empty_response","parse_error","schema_validation","tool_error","timeout"]},
            "message":{"type":"string"}
        },"additionalProperties":false
    })JSON");
}

json make_diagnostic(std::string phase,
                     std::string code,
                     std::string severity,
                     std::string path,
                     std::string message,
                     json        witness = json::object(),
                     std::string source  = {}) {
    if (source.empty()) source = path;
    json value = {
        {"phase", std::move(phase)},       {"code", std::move(code)},
        {"severity", std::move(severity)}, {"path", std::move(path)},
        {"message", std::move(message)},   {"witness", std::move(witness)},
        {"source", std::move(source)},
    };
    return value;
}

json harness_checkpoint_binding(const std::string& run_id,
                                const std::string& artifact_id,
                                const std::string& revision_digest,
                                const std::string& protocol_version,
                                const std::string& profile) {
    return {
        {"run_id", run_id},
        {"artifact_id", artifact_id},
        {"revision_digest", revision_digest},
        {"protocol_version", protocol_version},
        {"profile", profile},
    };
}

json fork_compatibility_diagnostics(const json&              source_core,
                                    const json&              target_core,
                                    const graph::Checkpoint& checkpoint,
                                    const json&              expected_binding) {
    json       diagnostics = json::array();
    const auto path        = "$.fork.checkpoint_id";
    if (checkpoint.schema_version != graph::CHECKPOINT_SCHEMA_VERSION) {
        diagnostics.push_back(
            make_diagnostic("compatibility", "H_FORK_CHECKPOINT_SCHEMA", "error", path,
                            "fork checkpoint schema is incompatible with this runtime",
                            {{"checkpoint_schema_version", checkpoint.schema_version},
                             {"runtime_schema_version", graph::CHECKPOINT_SCHEMA_VERSION}}));
        return diagnostics;
    }
    const auto binding =
        checkpoint.metadata.is_object() ? checkpoint.metadata.value("harness", json()) : json();
    if (binding != expected_binding) {
        diagnostics.push_back(
            make_diagnostic("compatibility", "H_FORK_CHECKPOINT_BINDING", "error", path,
                            "fork checkpoint Harness binding does not match its source run",
                            {{"expected", expected_binding}, {"actual", binding}}));
        return diagnostics;
    }

    const auto source_channels = source_core.value("channels", json::object());
    const auto target_channels = target_core.value("channels", json::object());
    if (!checkpoint.channel_values.is_object() || !checkpoint.channel_values.contains("channels") ||
        !checkpoint.channel_values["channels"].is_object()) {
        diagnostics.push_back(
            make_diagnostic("compatibility", "H_FORK_CHECKPOINT_STATE", "error", path,
                            "fork checkpoint does not contain serialized channel state"));
    } else {
        for (const auto& [name, state] : checkpoint.channel_values["channels"].items()) {
            (void)state;
            const auto channel_path = "$.harness.definition.channels." + name;
            if (!source_channels.is_object() || !source_channels.contains(name)) {
                diagnostics.push_back(
                    make_diagnostic("compatibility", "H_FORK_SOURCE_CHANNEL", "error", channel_path,
                                    "fork checkpoint channel is absent from its source artifact",
                                    {{"channel", name}, {"checkpoint_id", checkpoint.id}}));
                continue;
            }
            if (!target_channels.is_object() || !target_channels.contains(name)) {
                diagnostics.push_back(make_diagnostic(
                    "compatibility", "H_FORK_CHANNEL_MISSING", "error", channel_path,
                    "target artifact would discard a checkpoint channel",
                    {{"channel", name}, {"checkpoint_id", checkpoint.id}}));
                continue;
            }
            const auto source_reducer = source_channels[name].value("reducer", "overwrite");
            const auto target_reducer = target_channels[name].value("reducer", "overwrite");
            if (source_reducer != target_reducer) {
                diagnostics.push_back(make_diagnostic(
                    "compatibility", "H_FORK_CHANNEL_REDUCER", "error", channel_path,
                    "target artifact changes the reducer for a restored checkpoint channel",
                    {{"channel", name},
                     {"source_reducer", source_reducer},
                     {"target_reducer", target_reducer}}));
            }
        }
    }

    const auto target_nodes = target_core.value("nodes", json::object());
    for (const auto& node : checkpoint.next_nodes) {
        if (node == graph::END_NODE) continue;
        if (!target_nodes.is_object() || !target_nodes.contains(node)) {
            diagnostics.push_back(make_diagnostic(
                "compatibility", "H_FORK_NEXT_NODE", "error", "$.harness.definition.nodes." + node,
                "target artifact does not expose a checkpoint continuation node",
                {{"node_id", node}, {"checkpoint_id", checkpoint.id}}));
        }
    }

    const auto source_nodes = source_core.value("nodes", json::object());
    for (const auto& [node, arrivals] : checkpoint.barrier_state) {
        const auto source_interface = source_nodes.is_object() && source_nodes.contains(node)
                                          ? source_nodes[node].value("barrier", json())
                                          : json();
        const auto target_interface = target_nodes.is_object() && target_nodes.contains(node)
                                          ? target_nodes[node].value("barrier", json())
                                          : json();
        if (source_interface != target_interface) {
            json arrived = json::array();
            for (const auto& upstream : arrivals)
                arrived.push_back(upstream);
            diagnostics.push_back(
                make_diagnostic("compatibility", "H_FORK_BARRIER_INTERFACE", "error",
                                "$.harness.definition.nodes." + node + ".barrier",
                                "target artifact changes an active checkpoint barrier interface",
                                {{"node_id", node},
                                 {"arrived", std::move(arrived)},
                                 {"source_barrier", source_interface},
                                 {"target_barrier", target_interface}}));
        }
    }
    return diagnostics;
}

class BoundHarnessCheckpointStore final : public graph::CheckpointStore {
public:
    BoundHarnessCheckpointStore(std::shared_ptr<graph::CheckpointStore> inner, json binding)
        : inner_(std::move(inner)), binding_(std::move(binding)) {
        if (!inner_) throw std::invalid_argument("Harness checkpoint store must not be null");
    }

    void save(const graph::Checkpoint& checkpoint) override { inner_->save(bound(checkpoint)); }
    std::optional<graph::Checkpoint> load_latest(const std::string& thread_id) override {
        return inner_->load_latest(thread_id);
    }
    std::optional<graph::Checkpoint> load_by_id(const std::string& id) override {
        return inner_->load_by_id(id);
    }
    std::vector<graph::Checkpoint> list(const std::string& thread_id, int limit) override {
        return inner_->list(thread_id, limit);
    }
    void delete_thread(const std::string& thread_id) override { inner_->delete_thread(thread_id); }

    asio::awaitable<void> save_async(const graph::Checkpoint& checkpoint) override {
        auto     copy = bound(checkpoint);
        co_await inner_->save_async(copy);
    }
    asio::awaitable<std::optional<graph::Checkpoint>> load_latest_async(
        const std::string& thread_id) override {
        co_return co_await inner_->load_latest_async(thread_id);
    }
    asio::awaitable<std::optional<graph::Checkpoint>> load_by_id_async(
        const std::string& id) override {
        co_return co_await inner_->load_by_id_async(id);
    }
    asio::awaitable<std::vector<graph::Checkpoint>> list_async(const std::string& thread_id,
                                                               int                limit) override {
        co_return co_await inner_->list_async(thread_id, limit);
    }
    asio::awaitable<void> delete_thread_async(const std::string& thread_id) override {
        co_await inner_->delete_thread_async(thread_id);
    }

    void put_writes(const std::string&         thread_id,
                    const std::string&         parent_checkpoint_id,
                    const graph::PendingWrite& write) override {
        inner_->put_writes(thread_id, parent_checkpoint_id, write);
    }
    std::vector<graph::PendingWrite> get_writes(const std::string& thread_id,
                                                const std::string& parent_checkpoint_id) override {
        return inner_->get_writes(thread_id, parent_checkpoint_id);
    }
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override {
        inner_->clear_writes(thread_id, parent_checkpoint_id);
    }
    asio::awaitable<void> put_writes_async(const std::string&         thread_id,
                                           const std::string&         parent_checkpoint_id,
                                           const graph::PendingWrite& write) override {
        auto     thread = thread_id;
        auto     parent = parent_checkpoint_id;
        auto     copy   = write;
        co_await inner_->put_writes_async(thread, parent, copy);
    }
    asio::awaitable<std::vector<graph::PendingWrite>> get_writes_async(
        const std::string& thread_id, const std::string& parent_checkpoint_id) override {
        co_return co_await inner_->get_writes_async(thread_id, parent_checkpoint_id);
    }
    asio::awaitable<void> clear_writes_async(const std::string& thread_id,
                                             const std::string& parent_checkpoint_id) override {
        co_await inner_->clear_writes_async(thread_id, parent_checkpoint_id);
    }

private:
    graph::Checkpoint bound(graph::Checkpoint checkpoint) const {
        if (!checkpoint.metadata.is_object()) checkpoint.metadata = json::object();
        checkpoint.metadata["harness"] = binding_;
        return checkpoint;
    }

    std::shared_ptr<graph::CheckpointStore> inner_;
    json                                    binding_;
};

bool diagnostics_have_errors(const json& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.value("severity", "") == "error") return true;
    }
    return false;
}

std::string diagnostic_pointer(std::string path) {
    constexpr const char* definition_prefix = "$.harness.definition.";
    if (path.rfind(definition_prefix, 0) == 0) {
        path.erase(0, std::char_traits<char>::length(definition_prefix));
    } else if (path.rfind("nodes.", 0) != 0 && path.rfind("channels.", 0) != 0 &&
               path.rfind("edges", 0) != 0 && path.rfind("conditional_edges", 0) != 0) {
        return {};
    }
    std::replace(path.begin(), path.end(), '.', '/');
    return "/" + path;
}

void attach_elaborator_sources(json& diagnostics, const json& sourcemap) {
    if (!sourcemap.is_array()) return;
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        auto diagnostic = diagnostics[i];
        const auto pointer = diagnostic_pointer(diagnostic.value("path", ""));
        if (pointer.empty()) continue;
        std::size_t longest = 0;
        std::string source;
        for (const auto& mapping : sourcemap) {
            const auto target = mapping.value("target", "");
            if (target.size() > longest && pointer.rfind(target, 0) == 0) {
                longest = target.size();
                source = mapping.value("source", "");
            }
        }
        if (!source.empty()) {
            diagnostic["source"] = std::move(source);
            diagnostics[i] = std::move(diagnostic);
        }
    }
}

std::string response_kind_name(HarnessWorkerResponseKind kind) {
    switch (kind) {
        case HarnessWorkerResponseKind::VALUE:
            return "value";
        case HarnessWorkerResponseKind::EMPTY:
            return "empty_response";
        case HarnessWorkerResponseKind::PARSE_ERROR:
            return "parse_error";
        case HarnessWorkerResponseKind::TOOL_ERROR:
            return "tool_error";
        case HarnessWorkerResponseKind::TIMEOUT:
            return "timeout";
        case HarnessWorkerResponseKind::CANCELLED:
            return "cancelled";
        case HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS:
            return "awaiting_tool_results";
        case HarnessWorkerResponseKind::INPUT_REQUIRED:
            return "input_required";
    }
    return "unknown";
}

bool contains_redacted_marker(const json& value) {
    if (value.is_string()) {
        return value.get<std::string>() == detail::kHarnessRedactedMarker;
    }
    if (value.is_array()) {
        for (const auto& entry : value) {
            if (contains_redacted_marker(entry)) return true;
        }
    } else if (value.is_object()) {
        for (const auto& [key, entry] : value.items()) {
            (void)key;
            if (contains_redacted_marker(entry)) return true;
        }
    }
    return false;
}

class RecordedHarnessResults {
public:
    using Key        = std::pair<std::string, std::string>;
    using Invocation = std::vector<json>;

    explicit RecordedHarnessResults(std::map<Key, std::deque<Invocation>> invocations)
        : invocations_(std::move(invocations)) {
        for (const auto& [key, values] : invocations_) {
            (void)key;
            remaining_ += values.size();
        }
    }

    Invocation take(const std::string& node_id, const std::string& worker_id) {
        std::lock_guard lock(mutex_);
        auto            it = invocations_.find({node_id, worker_id});
        if (it == invocations_.end() || it->second.empty()) {
            throw std::runtime_error("recorded replay has no result for node " + node_id +
                                     " worker " + worker_id);
        }
        auto result = std::move(it->second.front());
        it->second.pop_front();
        --remaining_;
        return result;
    }

    void require_exhausted() const {
        std::lock_guard lock(mutex_);
        if (remaining_ != 0) {
            throw std::runtime_error("recorded replay did not consume every worker invocation");
        }
    }

private:
    mutable std::mutex                    mutex_;
    std::map<Key, std::deque<Invocation>> invocations_;
    std::size_t                           remaining_ = 0;
};

std::shared_ptr<RecordedHarnessResults> recorded_results_from_events(
    const std::vector<json>& events,
    const std::string&       run_id,
    const std::string&       artifact_id,
    const std::string&       revision_digest,
    const std::string&       protocol_version,
    const std::string&       profile) {
    using Key = RecordedHarnessResults::Key;
    std::map<Key, RecordedHarnessResults::Invocation>             pending;
    std::map<Key, std::deque<RecordedHarnessResults::Invocation>> invocations;

    for (const auto& event : events) {
        if (event.value("run_id", "") != run_id || event.value("artifact_id", "") != artifact_id ||
            event.value("revision_digest", "") != revision_digest ||
            event.value("protocol_version", "") != protocol_version ||
            event.value("profile", "") != profile) {
            throw std::runtime_error("recorded replay journal binding mismatch");
        }
        const auto event_type = event.value("event_type", "");
        if (event_type != "worker.attempt.completed" &&
            event_type != "worker.attempt.interrupted") {
            continue;
        }
        const auto node_id   = event.value("node_id", "");
        const auto worker_id = event.value("worker_id", "");
        if (node_id.empty() || worker_id.empty()) {
            throw std::runtime_error("recorded replay worker event is missing its identity");
        }
        const Key key{node_id, worker_id};
        if (event_type == "worker.attempt.interrupted") {
            pending.erase(key);
            continue;
        }

        if (!event.contains("payload") || !event["payload"].is_object()) {
            throw std::runtime_error("recorded replay requires FULL journal worker payloads");
        }
        const auto& payload          = event["payload"];
        const auto  outcome          = payload.value("outcome", "");
        const auto  expected_attempt = pending[key].size() + 1;
        if (event.value("attempt", std::size_t{0}) != expected_attempt) {
            throw std::runtime_error("recorded replay worker attempts are incomplete or unordered");
        }
        if (outcome == "valid") {
            if (!payload.contains("output") || contains_redacted_marker(payload["output"])) {
                throw std::runtime_error("recorded replay requires FULL journal worker outputs");
            }
        } else if (outcome == "retryable_failure") {
            const auto reason = payload.value("retry_reason", "");
            if (reason.empty()) {
                throw std::runtime_error("recorded replay requires FULL journal retry payloads");
            }
            if (!payload.contains("output") || contains_redacted_marker(payload["output"])) {
                throw std::runtime_error("recorded replay requires FULL journal worker outputs");
            }
        } else {
            throw std::runtime_error("recorded replay worker outcome is unsupported: " + outcome);
        }

        pending[key].push_back(event);
        const bool terminal = outcome == "valid" || (outcome == "retryable_failure" &&
                                                     !payload.value("retry_scheduled", false));
        if (terminal) {
            invocations[key].push_back(std::move(pending[key]));
            pending.erase(key);
        }
    }
    if (!pending.empty()) {
        throw std::runtime_error("recorded replay journal ends inside a worker invocation");
    }
    if (invocations.empty()) {
        throw std::runtime_error("recorded replay found no completed worker invocations");
    }
    return std::make_shared<RecordedHarnessResults>(std::move(invocations));
}

HarnessWorkerResponse recorded_worker_response(const json& event) {
    const auto& payload = event["payload"];
    const auto  outcome = payload.value("outcome", "");
    if (outcome == "valid") return HarnessWorkerResponse::success(payload["output"]);
    const auto reason  = payload.value("retry_reason", "");
    const auto message = payload.value("message", "");
    if (reason == "schema_validation") {
        return HarnessWorkerResponse::success(payload.value("output", json(nullptr)));
    }
    if (reason == "empty_response") return HarnessWorkerResponse::empty(message);
    if (reason == "parse_error") return HarnessWorkerResponse::parse_error(message);
    if (reason == "tool_error") return HarnessWorkerResponse::tool_error(message);
    if (reason == "timeout") return HarnessWorkerResponse::timeout(message);
    throw std::runtime_error("recorded replay failure kind is unsupported: " + reason);
}

class HarnessRuntimeProvider final : public Provider {
public:
    HarnessRuntimeProvider(json                                    request,
                           HarnessWorkerExecutor                   executor,
                           detail::HarnessJournalContext           journal_context  = {},
                           std::shared_ptr<RecordedHarnessResults> recorded_results = nullptr)
        : request_(std::move(request)),
          executor_(std::move(executor)),
          journal_context_(std::move(journal_context)),
          recorded_results_(std::move(recorded_results)) {
        for (const auto& worker : request_["workers"]) {
            workers_[worker["id"].get<std::string>()] = worker;
        }
        for (const auto& tool : request_.value("tool_catalog", json::array())) {
            tools_[tool["id"].get<std::string>()] = tool;
        }
        max_retries_ = request_.value("budgets", json::object()).value("max_worker_retries", 1);
    }

    std::string get_name() const override { return "harness-worker-executor"; }

    json execute(const std::string&                         node_id,
                 const std::string&                         worker_id,
                 const json&                                task,
                 const std::optional<json>&                 resume_value,
                 const std::shared_ptr<graph::CancelToken>& cancel) const {
        const auto worker_it = workers_.find(worker_id);
        if (worker_it == workers_.end()) {
            throw std::runtime_error("unknown Harness worker: " + worker_id);
        }

        json selected_tools = json::array();
        for (const auto& tool_id_json : worker_it->second.value("tools", json::array())) {
            const auto tool_id = tool_id_json.get<std::string>();
            auto tool_it = tools_.find(tool_id);
            if (tool_it != tools_.end()) selected_tools.push_back(tool_it->second);
        }

        std::string feedback;
        std::string failure_kind = "empty_response";
        int attempts_made = 0;
        const auto  recorded_attempts         = recorded_results_
                                                    ? recorded_results_->take(node_id, worker_id)
                                                    : RecordedHarnessResults::Invocation{};
        const auto  require_recorded_consumed = [&](std::size_t count) {
            if (recorded_results_ && recorded_attempts.size() != count) {
                throw std::runtime_error(
                    "recorded replay worker attempt count diverged from the source run");
            }
        };
        for (int attempt = 0; attempt <= max_retries_; ++attempt) {
            attempts_made = attempt + 1;
            cancel->throw_if_cancelled("before Harness worker " + worker_id);
            auto attempt_context = journal_context_;
            attempt_context.node_id = node_id;
            attempt_context.worker_id = worker_id;
            attempt_context.attempt = static_cast<std::size_t>(attempt + 1);
            detail::ScopedHarnessJournalContext journal_scope(attempt_context);
            const auto attempt_started = std::chrono::steady_clock::now();
            detail::append_harness_journal_event(
                attempt_context, "worker.attempt.started",
                {{"has_resume_value", resume_value.has_value()},
                 {"repair", !feedback.empty()},
                 {"selected_tools", worker_it->second.value("tools", json::array())}});
            HarnessWorkerCall call;
            call.task = task;
            call.worker = worker_it->second;
            call.tool_catalog = selected_tools;
            call.policy = request_.value("policy", json::object());
            call.attempt = static_cast<std::size_t>(attempt + 1);
            call.repair_feedback = feedback;
            call.resume_value    = resume_value;

            HarnessWorkerResponse response;
            if (recorded_results_) {
                if (static_cast<std::size_t>(attempt) >= recorded_attempts.size()) {
                    throw std::runtime_error(
                        "recorded replay exhausted worker attempts before execution completed");
                }
                response = recorded_worker_response(recorded_attempts[attempt]);
            } else if (!executor_) {
                response =
                    HarnessWorkerResponse::tool_error("no Harness worker executor is configured");
            } else {
                response = executor_(call, cancel);
            }

            if (response.kind == HarnessWorkerResponseKind::CANCELLED) {
                require_recorded_consumed(static_cast<std::size_t>(attempt + 1));
                detail::append_harness_journal_event(
                    attempt_context, "worker.attempt.completed",
                    {{"duration_ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - attempt_started)
                          .count()},
                     {"outcome", "cancelled"},
                     {"message", response.message}});
                throw graph::CancelledException(
                    response.message.empty() ? "Harness worker cancelled" : response.message);
            }

            if (response.kind == HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS ||
                response.kind == HarnessWorkerResponseKind::INPUT_REQUIRED) {
                auto pending     = response.value;
                pending["state"] = response_kind_name(response.kind);
                detail::append_harness_journal_event(
                    attempt_context, "worker.attempt.interrupted",
                    {{"duration_ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - attempt_started)
                          .count()},
                     {"pending", pending},
                     {"reason", pending["state"]}},
                    pending.value("call_id", ""));
                throw graph::NodeInterrupt(
                    response.kind == HarnessWorkerResponseKind::INPUT_REQUIRED
                        ? "Harness worker requires host input"
                        : "Harness worker awaits host-brokered tool results",
                    std::move(pending));
            }

            if (response.kind == HarnessWorkerResponseKind::VALUE) {
                if (response.value.is_null() ||
                    (response.value.is_string() && response.value.get<std::string>().empty())) {
                    failure_kind = "empty_response";
                    feedback = "worker returned an empty response";
                } else {
                    try {
                        validate_json_value(response.value, worker_it->second["output_schema"],
                                            "Harness worker output", "$");
                        detail::append_harness_journal_event(
                            attempt_context, "worker.attempt.completed",
                            {{"duration_ms",
                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - attempt_started)
                                  .count()},
                             {"outcome", "valid"},
                             {"output", response.value},
                             {"validation", "passed"}});
                        require_recorded_consumed(static_cast<std::size_t>(attempt + 1));
                        return {
                            {"worker_id", worker_id},
                            {"status", "valid"},
                            {"attempts", attempt + 1},
                            {"output", response.value},
                        };
                    } catch (const std::exception& error) {
                        failure_kind = "schema_validation";
                        feedback = error.what();
                    }
                }
            } else {
                failure_kind = response_kind_name(response.kind);
                feedback     = response.message.empty() ? failure_kind : response.message;
            }
            const bool stop_retry = response.kind == HarnessWorkerResponseKind::TIMEOUT ||
                                    response.kind == HarnessWorkerResponseKind::TOOL_ERROR;
            detail::append_harness_journal_event(
                attempt_context, "worker.attempt.completed",
                {{"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - attempt_started)
                                     .count()},
                 {"outcome", "retryable_failure"},
                 {"retry_scheduled", !stop_retry && attempt < max_retries_},
                 {"retry_reason", failure_kind},
                 {"validation", failure_kind == "schema_validation" ? "failed" : "not_run"},
                 {"message", feedback},
                 {"output", response.kind == HarnessWorkerResponseKind::VALUE ? response.value
                                                                              : json(nullptr)}});
            if (stop_retry) break;
        }

        require_recorded_consumed(static_cast<std::size_t>(attempts_made));
        return {
            {"worker_id", worker_id}, {"status", "failed"},        {"failure_kind", failure_kind},
            {"message", feedback},    {"attempts", attempts_made},
        };
    }

private:
    json request_;
    HarnessWorkerExecutor executor_;
    std::map<std::string, json> workers_;
    std::map<std::string, json> tools_;
    int max_retries_ = 1;
    detail::HarnessJournalContext journal_context_;
    std::shared_ptr<RecordedHarnessResults> recorded_results_;
};

class HarnessWorkerNode final : public graph::GraphNode {
public:
    HarnessWorkerNode(std::string                             name,
                      std::string                             worker_id,
                      std::shared_ptr<HarnessRuntimeProvider> runtime)
        : name_(std::move(name)), worker_id_(std::move(worker_id)), runtime_(std::move(runtime)) {}

    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        auto cancel = in.ctx.cancel_token;
        if (!cancel) cancel = std::make_shared<graph::CancelToken>();
        auto result = runtime_->execute(name_, worker_id_, in.state.get("task"),
                                        in.ctx.resume_value, cancel);
        graph::NodeOutput output;
        output.writes.push_back({"worker_results", std::move(result)});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::string worker_id_;
    std::shared_ptr<HarnessRuntimeProvider> runtime_;
};

class HarnessJudgeNode final : public graph::GraphNode {
public:
    explicit HarnessJudgeNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        std::vector<json> workers;
        const auto results = in.state.get("worker_results");
        for (const auto& result : results)
            workers.push_back(result);
        std::sort(workers.begin(), workers.end(), [](const json& lhs, const json& rhs) {
            return lhs.value("worker_id", "") < rhs.value("worker_id", "");
        });

        json normalized = json::array();
        json findings = json::array();
        int valid = 0;
        int failed = 0;
        bool partial = false;
        bool findings_contract = !workers.empty();
        for (const auto& worker : workers) {
            normalized.push_back(worker);
            if (worker.value("status", "") != "valid") {
                ++failed;
                findings_contract = false;
                continue;
            }
            ++valid;
            const auto output = worker["output"];
            const auto worker_status = output.value("status", "ok");
            if (worker_status == "partial") partial = true;
            if (worker_status == "failed") {
                ++failed;
                --valid;
            }
            if (!output.contains("findings") || !output["findings"].is_array()) {
                findings_contract = false;
            } else {
                for (const auto& finding : output["findings"]) {
                    findings.push_back(finding);
                }
            }
        }

        std::string outcome = "ok";
        if (valid == 0 && failed > 0)
            outcome = "failed";
        else if (failed > 0 || partial)
            outcome = "partial";
        else if (findings_contract && findings.empty())
            outcome = "zero_findings";

        json result = {
            {"outcome", outcome},
            {"workers", std::move(normalized)},
            {"findings", std::move(findings)},
            {"valid_workers", valid},
            {"failed_workers", failed},
        };
        graph::NodeOutput output;
        output.writes.push_back({"final_result", std::move(result)});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

void register_harness_node_types() {
    static std::once_flag once;
    std::call_once(once, [] {
        graph::NodeFactory::instance().register_type(
            kWorkerNodeType,
            [](const std::string& name, const json& config,
               const graph::NodeContext& ctx) -> std::unique_ptr<graph::GraphNode> {
                auto runtime = std::dynamic_pointer_cast<HarnessRuntimeProvider>(ctx.provider);
                if (!runtime) {
                    throw std::runtime_error(
                        "Harness worker nodes require a Harness runtime context");
                }
                return std::make_unique<HarnessWorkerNode>(
                    name, config.at("worker_id").get<std::string>(), std::move(runtime));
            },
            json::parse(
                R"JSON({"type":"object","required":["worker_id"],"properties":{"worker_id":{"type":"string"}},"additionalProperties":false})JSON"),
            json::parse(R"JSON({"reads":["task"],"writes":["worker_results"]})JSON"));

        graph::NodeFactory::instance().register_type(
            kJudgeNodeType,
            [](const std::string& name, const json&,
               const graph::NodeContext&) -> std::unique_ptr<graph::GraphNode> {
                return std::make_unique<HarnessJudgeNode>(name);
            },
            json::parse(
                R"JSON({"type":"object","properties":{},"additionalProperties":false})JSON"),
            json::parse(R"JSON({"reads":["worker_results"],"writes":["final_result"]})JSON"));
    });
}

json preset_fanout_judge(const json& request, const std::string& preset) {
    json core = {
        {"schema_version", 1},
        {"name", "harness_" + preset},
        {"channels",
         {
             {"task", {{"reducer", "overwrite"}, {"initial", json::object()}}},
             {"worker_results", {{"reducer", "append"}, {"initial", json::array()}}},
             {"final_result", {{"reducer", "overwrite"}, {"initial", nullptr}}},
         }},
        {"nodes", json::object()},
        {"edges", json::array()},
    };

    json wait_for = json::array();
    std::size_t index = 0;
    for (const auto& worker : request["workers"]) {
        const auto node = "worker_" + std::to_string(index++);
        core["nodes"][node] = {
            {"type", kWorkerNodeType},
            {"worker_id", worker["id"]},
        };
        core["edges"].push_back({{"from", graph::START_NODE}, {"to", node}});
        core["edges"].push_back({{"from", node}, {"to", "judge"}});
        wait_for.push_back(node);
    }
    core["nodes"]["judge"] = {
        {"type", kJudgeNodeType},
        {"barrier", {{"wait_for", wait_for}}},
    };
    core["edges"].push_back({{"from", "judge"}, {"to", graph::END_NODE}});
    return core;
}

void validate_range(const json& budgets,
                    const char* name,
                    int         minimum,
                    int         maximum,
                    int         default_value,
                    json&       diagnostics) {
    const int value = budgets.value(name, default_value);
    if (value < minimum || value > maximum) {
        diagnostics.push_back(make_diagnostic(
            "request", "H_REQUEST_BUDGET", "error", std::string("$.budgets.") + name,
            std::string(name) + " must be between " + std::to_string(minimum) + " and " +
                std::to_string(maximum),
            {{"value", value}, {"minimum", minimum}, {"maximum", maximum}}));
    }
}

void validate_bindings(const json& request,
                       const json& core,
                       bool        durable_resume_available,
                       json&       diagnostics) {
    std::map<std::string, json> workers;
    std::map<std::string, json> tools;
    std::set<std::string> duplicates;

    for (const auto& root :
         request.value("policy", json::object()).value("workspace_roots", json::array())) {
        if (root.get<std::string>().empty()) {
            diagnostics.push_back(make_diagnostic("binding", "H_WORKSPACE_ROOT", "error",
                                                  "$.policy.workspace_roots",
                                                  "workspace roots must not contain empty paths"));
        }
    }

    std::size_t index = 0;
    for (const auto& worker : request["workers"]) {
        const auto id = worker.value("id", "");
        const auto path = "$.workers[" + std::to_string(index++) + "]";
        if (id.empty()) {
            diagnostics.push_back(make_diagnostic("binding", "H_WORKER_ID", "error", path + ".id",
                                                  "worker id must not be empty"));
            continue;
        }
        if (!workers.emplace(id, worker).second) duplicates.insert(id);
        try {
            validate_json_schema(worker["output_schema"], path + ".output_schema");
        } catch (const std::exception& error) {
            diagnostics.push_back(make_diagnostic("binding", "H_WORKER_SCHEMA", "error",
                                                  path + ".output_schema", error.what()));
        }
    }
    for (const auto& id : duplicates) {
        diagnostics.push_back(make_diagnostic("binding", "H_DUPLICATE_WORKER", "error", "$.workers",
                                              "duplicate worker id: " + id, {{"id", id}}));
    }

    const auto evidence_required =
        request.value("policy", json::object()).value("evidence_required", json::array());
    if (!evidence_required.empty()) {
        for (const auto& [id, worker] : workers) {
            const auto properties = worker["output_schema"].value("properties", json::object());
            const auto findings = properties.value("findings", json::object());
            const auto item = findings.value("items", json::object());
            const auto item_properties = item.value("properties", json::object());
            const auto item_required = item.value("required", json::array());
            for (const auto& field_json : evidence_required) {
                const auto field = field_json.get<std::string>();
                bool required = false;
                for (const auto& required_json : item_required) {
                    if (required_json == field_json) {
                        required = true;
                        break;
                    }
                }
                if (!item_properties.contains(field) || !required) {
                    diagnostics.push_back(make_diagnostic(
                        "binding", "H_EVIDENCE_SCHEMA", "error",
                        "$.workers." + id + ".output_schema",
                        "worker finding schema must require evidence field: " + field,
                        {{"worker_id", id}, {"field", field}}));
                }
            }
        }
    }

    index = 0;
    for (const auto& tool : request.value("tool_catalog", json::array())) {
        const auto id = tool.value("id", "");
        const auto path = "$.tool_catalog[" + std::to_string(index++) + "]";
        if (id.empty() || !tools.emplace(id, tool).second) {
            diagnostics.push_back(make_diagnostic(
                "binding", "H_DUPLICATE_TOOL", "error", path + ".id",
                id.empty() ? "tool id must not be empty" : "duplicate tool id: " + id));
        }
        try {
            validate_json_schema(tool["input_schema"], path + ".input_schema");
            if (tool.contains("output_schema")) {
                validate_json_schema(tool["output_schema"], path + ".output_schema");
            }
        } catch (const std::exception& error) {
            diagnostics.push_back(
                make_diagnostic("binding", "H_TOOL_SCHEMA", "error", path, error.what()));
        }

        for (const auto& argument_json : tool.value("path_arguments", json::array())) {
            const auto argument = argument_json.get<std::string>();
            const auto properties = tool["input_schema"].value("properties", json::object());
            if (!properties.contains(argument) ||
                properties[argument].value("type", "") != "string") {
                diagnostics.push_back(
                    make_diagnostic("binding", "H_PATH_ARGUMENT", "error", path + ".path_arguments",
                                    "path_arguments must name string properties in input_schema",
                                    {{"tool_id", id}, {"argument", argument}}));
            }
        }

        const auto executor = tool.value("executor", json::object());
        const auto kind = executor.value("kind", "");
        if (kind == "mcp" && executor.value("server_ref", "").empty()) {
            diagnostics.push_back(make_diagnostic("binding", "H_MCP_UNRESOLVED", "error",
                                                  path + ".executor.server_ref",
                                                  "mcp executor requires a resolvable server_ref"));
        } else if (kind == "a2a" && executor.value("agent", "").empty()) {
            diagnostics.push_back(make_diagnostic("binding", "H_A2A_UNRESOLVED", "error",
                                                  path + ".executor.agent",
                                                  "a2a executor requires an agent identifier"));
        } else if (kind == "host_brokered" && !durable_resume_available) {
            diagnostics.push_back(make_diagnostic(
                "binding", "H_HOST_BROKER_UNAVAILABLE", "error", path + ".executor.kind",
                "host_brokered tools require both checkpoint_store and record_store"));
        } else if (kind == "script") {
            diagnostics.push_back(make_diagnostic("binding", "H_SCRIPT_DISABLED", "error",
                                                  path + ".executor.kind",
                                                  "script executors are disabled by default"));
        }

        const bool read_only = request.value("policy", json::object()).value("read_only", false);
        if (read_only && !tool.value("read_only", false)) {
            diagnostics.push_back(make_diagnostic("binding", "H_WRITE_TOOL", "error", path,
                                                  "read-only harness contains a write-capable tool",
                                                  {{"tool_id", id}}));
        }
        if (!tool.value("path_arguments", json::array()).empty() &&
            request.value("policy", json::object())
                .value("workspace_roots", json::array())
                .empty()) {
            diagnostics.push_back(make_diagnostic(
                "binding", "H_WORKSPACE_ROOT", "error", path,
                "path-bearing tools require policy.workspace_roots", {{"tool_id", id}}));
        }
    }

    for (const auto& [id, worker] : workers) {
        for (const auto& tool_id_json : worker.value("tools", json::array())) {
            const auto tool_id = tool_id_json.get<std::string>();
            if (tools.find(tool_id) == tools.end()) {
                diagnostics.push_back(make_diagnostic("binding", "H_UNKNOWN_TOOL", "error",
                                                      "$.workers." + id + ".tools",
                                                      "worker references unknown tool: " + tool_id,
                                                      {{"worker_id", id}, {"tool_id", tool_id}}));
            }
        }
    }

    std::map<std::string, int> topology_workers;
    int judges = 0;
    if (core.contains("nodes") && core["nodes"].is_object()) {
        for (const auto& [node_name, node] : core["nodes"].items()) {
            const auto type = node.value("type", "");
            if (type == kWorkerNodeType) {
                const auto worker_id = node.value("worker_id", "");
                ++topology_workers[worker_id];
                if (workers.find(worker_id) == workers.end()) {
                    diagnostics.push_back(
                        make_diagnostic("binding", "H_UNKNOWN_WORKER", "error",
                                        "$.harness.definition.nodes." + node_name,
                                        "topology references unknown worker: " + worker_id,
                                        {{"node", node_name}, {"worker_id", worker_id}}));
                }
            } else if (type == kJudgeNodeType) {
                ++judges;
            } else {
                diagnostics.push_back(make_diagnostic(
                    "binding", "H_NODE_TYPE", "error",
                    "$.harness.definition.nodes." + node_name + ".type",
                    "Harness DSL only permits compiler-backed worker and judge nodes",
                    {{"node", node_name}, {"type", type}}));
            }
        }
    }
    for (const auto& [id, worker] : workers) {
        (void)worker;
        if (topology_workers[id] != 1) {
            diagnostics.push_back(make_diagnostic(
                "binding", "H_WORKER_BINDING", "error", "$.harness.definition.nodes",
                "each declared worker must be bound exactly once",
                {{"worker_id", id}, {"bindings", topology_workers[id]}}));
        }
    }
    if (judges != 1) {
        diagnostics.push_back(
            make_diagnostic("binding", "H_JUDGE_BINDING", "error", "$.harness.definition.nodes",
                            "fanout_judge topology requires exactly one result-reading judge",
                            {{"judges", judges}}));
    }
}

CallToolResult mcp_result(json structured, std::string text) {
    CallToolResult result;
    result.content            = structured_text_content(structured, std::move(text));
    result.structured_content = std::move(structured);
    return result;
}

ToolDefinition tool_definition(std::string name,
                               std::string title,
                               std::string description,
                               json        input,
                               json        output,
                               bool        read_only = false) {
    ToolDefinition definition;
    definition.name = std::move(name);
    definition.title = std::move(title);
    definition.description = std::move(description);
    definition.input_schema = std::move(input);
    definition.output_schema = std::move(output);
    definition.annotations = {{"readOnlyHint", read_only}};
    definition.execution = {{"taskSupport", "forbidden"}};
    return definition;
}

} // namespace

HarnessWorkerResponse HarnessWorkerResponse::success(json value) {
    return {HarnessWorkerResponseKind::VALUE, std::move(value), {}};
}
HarnessWorkerResponse HarnessWorkerResponse::empty(std::string message) {
    return {HarnessWorkerResponseKind::EMPTY, nullptr, std::move(message)};
}
HarnessWorkerResponse HarnessWorkerResponse::parse_error(std::string message) {
    return {HarnessWorkerResponseKind::PARSE_ERROR, nullptr, std::move(message)};
}
HarnessWorkerResponse HarnessWorkerResponse::tool_error(std::string message) {
    return {HarnessWorkerResponseKind::TOOL_ERROR, nullptr, std::move(message)};
}
HarnessWorkerResponse HarnessWorkerResponse::timeout(std::string message) {
    return {HarnessWorkerResponseKind::TIMEOUT, nullptr, std::move(message)};
}
HarnessWorkerResponse HarnessWorkerResponse::cancelled(std::string message) {
    return {HarnessWorkerResponseKind::CANCELLED, nullptr, std::move(message)};
}
HarnessWorkerResponse HarnessWorkerResponse::awaiting_tool_results(json pending) {
    return {HarnessWorkerResponseKind::AWAITING_TOOL_RESULTS, std::move(pending), {}};
}
HarnessWorkerResponse HarnessWorkerResponse::input_required(json pending) {
    return {HarnessWorkerResponseKind::INPUT_REQUIRED, std::move(pending), {}};
}

struct FileHarnessRecordStore::Impl {
    explicit Impl(std::string directory) : root(std::move(directory)) {
        if (root.empty()) {
            throw std::invalid_argument("FileHarnessRecordStore root directory must not be empty");
        }
        std::filesystem::create_directories(root / "artifacts");
        std::filesystem::create_directories(root / "runs");
    }

    static void validate_id(const std::string& id) {
        if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            })) {
            throw std::invalid_argument("invalid Harness record identifier");
        }
    }

    std::filesystem::path path(const char* collection, const std::string& id) const {
        validate_id(id);
        return root / collection / (id + ".json");
    }

    void save(const char* collection, const std::string& id, const json& record) {
        const auto                  target = path(collection, id);
        std::lock_guard             lock(mutex);
        const std::filesystem::path temporary =
            target.string() + ".tmp." +
            std::to_string(next_temp.fetch_add(1, std::memory_order_relaxed));
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("cannot open temporary Harness record: " +
                                         temporary.string());
            }
            output << record.dump();
            output.flush();
            if (!output) {
                throw std::runtime_error("cannot write temporary Harness record: " +
                                         temporary.string());
            }
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto code = GetLastError();
            std::filesystem::remove(temporary);
            throw std::runtime_error("cannot commit Harness record (Windows error " +
                                     std::to_string(code) + ")");
        }
#else
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("cannot commit Harness record: " + error.message());
        }
#endif
    }

    std::optional<json> load(const char* collection, const std::string& id) {
        const auto      target = path(collection, id);
        std::lock_guard lock(mutex);
        std::ifstream   input(target, std::ios::binary);
        if (!input) {
            if (!std::filesystem::exists(target)) return std::nullopt;
            throw std::runtime_error("cannot read Harness record: " + target.string());
        }
        std::ostringstream content;
        content << input.rdbuf();
        return json::parse(content.str());
    }

    std::filesystem::path      root;
    std::mutex                 mutex;
    std::atomic<std::uint64_t> next_temp{1};
};

FileHarnessRecordStore::FileHarnessRecordStore(std::string root_directory)
    : impl_(std::make_unique<Impl>(std::move(root_directory))) {}

FileHarnessRecordStore::~FileHarnessRecordStore() = default;

void FileHarnessRecordStore::save_artifact(const std::string& artifact_id, const json& record) {
    impl_->save("artifacts", artifact_id, record);
}

std::optional<json> FileHarnessRecordStore::load_artifact(const std::string& artifact_id) {
    return impl_->load("artifacts", artifact_id);
}

void FileHarnessRecordStore::save_run(const std::string& run_id, const json& record) {
    impl_->save("runs", run_id, record);
}

std::optional<json> FileHarnessRecordStore::load_run(const std::string& run_id) {
    return impl_->load("runs", run_id);
}

struct HarnessService::Impl {
    struct Artifact {
        std::string id;
        std::string revision_digest;
        std::string protocol_version = MCP_PROTOCOL_VERSION;
        std::string profile          = kHarnessProfile;
        json request;
        json core;
        json sourcemap;
        json diagnostics;
    };

    struct Run {
        mutable std::mutex mutex;
        std::string id;
        std::string artifact_id;
        std::string revision_digest;
        std::string protocol_version = MCP_PROTOCOL_VERSION;
        std::string profile = kHarnessProfile;
        std::string                         execution_mode   = "live";
        std::string                         source_run_id;
        std::string                         source_checkpoint_id;
        std::string status = "queued";
        json result;
        json details;
        json                                pending;
        json                                consumed = json::object();
        json                                resume_value;
        bool                                resume_scheduled = false;
        bool                                execution_finished = false;
        std::string error;
        int64_t                             created_at = unix_millis();
        int64_t                             updated_at = created_at;
        int64_t                             expires_at = 0;
        std::shared_ptr<graph::CancelToken> cancel     = std::make_shared<graph::CancelToken>();
    };

    Impl(HarnessServiceConfig value, std::shared_ptr<HarnessJournal> journal_value)
        : config(std::move(value)), journal(std::move(journal_value)), nonce(std::random_device{}()) {
        if (config.poll_interval.count() <= 0 || config.run_ttl.count() <= 0 ||
            config.max_artifacts == 0 || config.max_runs == 0) {
            throw std::invalid_argument(
                "Harness limits, poll_interval, and run_ttl must be positive");
        }
        if (config.enable_experimental_tasks && !config.record_store) {
            throw std::invalid_argument(
                "experimental Tasks profile requires a durable record_store");
        }
        if (!journal && config.record_store) {
            journal = std::dynamic_pointer_cast<HarnessJournal>(config.record_store);
        }
        register_harness_node_types();
    }

    ~Impl() {
        std::vector<std::shared_ptr<Run>> active;
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, run] : runs) {
                (void)id;
                active.push_back(run);
            }
        }
        for (const auto& run : active) {
            std::lock_guard lock(run->mutex);
            if (run->status == "queued" || run->status == "running") {
                run->cancel->cancel();
            }
        }
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }

    std::string id(const char* prefix) {
        const auto sequence = next_id.fetch_add(1, std::memory_order_relaxed);
        std::ostringstream out;
        out << prefix << '_' << std::hex << nonce << sequence;
        return out.str();
    }

    json artifact_record(const Artifact& artifact) const {
        return {
            {"artifact_id", artifact.id},
            {"revision_digest", artifact.revision_digest},
            {"protocol_version", artifact.protocol_version},
            {"profile", artifact.profile},
            {"request", artifact.request},
            {"core", artifact.core},
            {"sourcemap", artifact.sourcemap},
            {"diagnostics", artifact.diagnostics},
        };
    }

    json run_record_locked(const Run& run) const {
        return {
            {"run_id", run.id},
            {"artifact_id", run.artifact_id},
            {"revision_digest", run.revision_digest},
            {"protocol_version", run.protocol_version},
            {"profile", run.profile},
            {"execution_mode", run.execution_mode},
            {"source_run_id", run.source_run_id},
            {"source_checkpoint_id", run.source_checkpoint_id},
            {"status", run.status},
            {"result", run.result},
            {"details", run.details},
            {"pending", run.pending},
            {"consumed", run.consumed},
            {"resume_value", run.resume_value},
            {"error", run.error},
            {"created_at", run.created_at},
            {"updated_at", run.updated_at},
            {"expires_at", run.expires_at},
        };
    }

    detail::HarnessJournalContext journal_context(const Run& run) const {
        return {
            journal,
            run.id,
            run.artifact_id,
            run.revision_digest,
            run.protocol_version,
            run.profile,
        };
    }

    void persist_run(const std::shared_ptr<Run>& run) const {
        if (!config.record_store) return;
        json record;
        {
            std::lock_guard lock(run->mutex);
            record = run_record_locked(*run);
        }
        config.record_store->save_run(run->id, record);
    }

    void cleanup_retained_locked(std::size_t                     max_artifacts,
                                 std::size_t                     max_runs,
                                 const std::vector<std::string>& protected_artifacts = {},
                                 const std::vector<std::string>& protected_runs      = {}) {
        auto* retention = dynamic_cast<HarnessRetentionStore*>(config.record_store.get());
        if (!retention) return;

        HarnessRetentionPolicy policy;
        policy.max_artifacts          = max_artifacts;
        policy.max_runs               = max_runs;
        policy.protected_artifact_ids = protected_artifacts;
        policy.protected_run_ids      = protected_runs;
        for (const auto& [run_id, run] : runs) {
            std::lock_guard run_lock(run->mutex);
            // SQLite status/dependency predicates are authoritative. This flag
            // additionally protects terminal state while its execution thread
            // is still finalizing the journal and persistence sequence.
            if (run->execution_finished) continue;
            policy.protected_run_ids.push_back(run_id);
            policy.protected_artifact_ids.push_back(run->artifact_id);
        }

        const auto removed = retention->cleanup_retained(policy);
        for (const auto& run_id : removed.run_ids) {
            runs.erase(run_id);
            if (config.checkpoint_store) {
                try {
                    config.checkpoint_store->delete_thread(run_id);
                } catch (...) {
                    // The authoritative run record is gone; a failed secondary
                    // checkpoint cleanup leaves only unreachable storage.
                }
            }
        }
        for (const auto& artifact_id : removed.artifact_ids) {
            artifacts_by_id.erase(artifact_id);
        }
    }

    std::shared_ptr<Artifact> find_artifact(const std::string& artifact_id) {
        {
            std::lock_guard lock(mutex);
            auto            it = artifacts_by_id.find(artifact_id);
            if (it != artifacts_by_id.end()) return it->second;
        }
        if (!config.record_store) return nullptr;
        auto record = config.record_store->load_artifact(artifact_id);
        if (!record) return nullptr;
        if (record->value("artifact_id", "") != artifact_id) {
            throw std::runtime_error("Harness artifact record id mismatch");
        }
        auto artifact         = std::make_shared<Artifact>();
        artifact->id          = artifact_id;
        artifact->request     = record->at("request");
        artifact->core        = record->at("core");
        artifact->revision_digest = record->value(
            "revision_digest", revision_digest(artifact->request, artifact->core));
        artifact->protocol_version = record->value("protocol_version", MCP_PROTOCOL_VERSION);
        artifact->profile          = record->value("profile", kHarnessProfile);
        artifact->sourcemap   = record->value("sourcemap", json::array());
        artifact->diagnostics = record->value("diagnostics", json::array());
        std::lock_guard lock(mutex);
        auto [it, inserted] = artifacts_by_id.emplace(artifact_id, artifact);
        return inserted ? artifact : it->second;
    }

    std::shared_ptr<Run> find_run(const std::string& run_id) {
        {
            std::lock_guard lock(mutex);
            auto            it = runs.find(run_id);
            if (it != runs.end()) return it->second;
        }
        if (!config.record_store) return nullptr;
        auto record = config.record_store->load_run(run_id);
        if (!record) return nullptr;
        if (record->value("run_id", "") != run_id) {
            throw std::runtime_error("Harness run record id mismatch");
        }
        auto run          = std::make_shared<Run>();
        run->id           = run_id;
        run->artifact_id  = record->at("artifact_id").get<std::string>();
        run->revision_digest = record->value("revision_digest", "");
        run->protocol_version = record->value("protocol_version", MCP_PROTOCOL_VERSION);
        run->profile = record->value("profile", kHarnessProfile);
        run->execution_mode   = record->value("execution_mode", "live");
        run->source_run_id    = record->value("source_run_id", "");
        run->source_checkpoint_id = record->value("source_checkpoint_id", "");
        run->status       = record->at("status").get<std::string>();
        run->result       = record->value("result", json());
        run->details      = record->value("details", json());
        run->pending      = record->value("pending", json());
        run->consumed     = record->value("consumed", json::object());
        run->resume_value = record->value("resume_value", json());
        run->error        = record->value("error", "");
        run->created_at   = record->value("created_at", unix_millis());
        run->updated_at   = record->value("updated_at", run->created_at);
        run->expires_at   = record->value("expires_at", int64_t{0});
        run->execution_finished   = run->status == "completed" || run->status == "failed" ||
                                  run->status == "cancelled" || run->status == "timeout" ||
                                  run->status == "expired" || run->status == "max_steps_exhausted";
        if (run->revision_digest.empty()) {
            auto artifact = find_artifact(run->artifact_id);
            if (!artifact) {
                throw std::runtime_error("Harness run revision artifact is unavailable");
            }
            run->revision_digest = artifact->revision_digest;
        }
        if (run->status == "running" && !run->resume_value.is_null()) {
            run->status = "queued";
        }
        std::lock_guard lock(mutex);
        auto [it, inserted] = runs.emplace(run_id, run);
        return inserted ? run : it->second;
    }

    json compile_request(const json& request, bool retain) {
        json diagnostics = json::array();
        json core;
        json sourcemap = json::array();

        try {
            validate_json_value(request, harness_request_schema(), "Harness request", "$");
        } catch (const std::exception& error) {
            diagnostics.push_back(
                make_diagnostic("request", "H_REQUEST_SCHEMA", "error", "$", error.what()));
        }

        if (!diagnostics_have_errors(diagnostics)) {
            if (request["workers"].empty()) {
                diagnostics.push_back(make_diagnostic("request", "H_WORKERS_EMPTY", "error",
                                                      "$.workers",
                                                      "at least one worker is required"));
            }
            const auto harness = request["harness"];
            const auto mode = harness.value("mode", "");
            if (mode == "preset") {
                static const std::set<std::string> presets = {"fanout_judge", "pr_review_panel",
                                                              "bug_triage", "research_synthesis"};
                if (presets.find(harness.value("preset", "")) == presets.end()) {
                    diagnostics.push_back(make_diagnostic("request", "H_PRESET", "error",
                                                          "$.harness.preset",
                                                          "unknown Harness preset"));
                }
            } else if (mode == "dsl" && !harness.contains("definition")) {
                diagnostics.push_back(make_diagnostic("request", "H_DSL_DEFINITION", "error",
                                                      "$.harness.definition",
                                                      "dsl mode requires a definition"));
            }
            const auto budgets = request.value("budgets", json::object());
            validate_range(budgets, "max_steps", 1, 1000, 40, diagnostics);
            validate_range(budgets, "timeout_seconds", 1, 86400, 600, diagnostics);
            validate_range(budgets, "max_parallel_workers", 1, 64, 4, diagnostics);
            validate_range(budgets, "max_worker_retries", 0, 5, 1, diagnostics);
        }

        if (!diagnostics_have_errors(diagnostics)) {
            try {
                const auto harness = request["harness"];
                json surface = harness.value("mode", "") == "preset"
                    ? preset_fanout_judge(request, harness.value("preset", ""))
                    : harness["definition"];
                auto elaborated = graph::Elaborator::elaborate(surface);
                core = std::move(elaborated.core);
                sourcemap = std::move(elaborated.sourcemap);
                if (core.value("schema_version", 0) != 1) {
                    diagnostics.push_back(
                        make_diagnostic("elaboration", "H_STRICT_CORE", "error",
                                        "$.harness.definition.schema_version",
                                        "Harness core must declare schema_version: 1"));
                }
            } catch (const std::exception& error) {
                diagnostics.push_back(make_diagnostic("elaboration", "H_ELABORATION", "error",
                                                      "$.harness.definition", error.what()));
            }
        }

        if (!diagnostics_have_errors(diagnostics)) {
            try {
                auto provider =
                    std::make_shared<HarnessRuntimeProvider>(request, HarnessWorkerExecutor{});
                graph::NodeContext context;
                context.provider = provider;
                auto compiled = graph::GraphCompiler::compile(core, context);
                graph::GraphCompiler::verify_roundtrip(core, compiled);
                auto report = graph::GraphValidator::validate(compiled);
                for (const auto& diagnostic : report.diagnostics) {
                    diagnostics.push_back(make_diagnostic("static", diagnostic.code,
                                                          diagnostic.severity, diagnostic.path,
                                                          diagnostic.message, diagnostic.witness));
                }
            } catch (const std::exception& error) {
                diagnostics.push_back(make_diagnostic("compile", "H_COMPILE", "error",
                                                      "$.harness.definition", error.what()));
            }
        }

        if (!core.is_null()) {
            validate_bindings(request, core, config.checkpoint_store && config.record_store,
                              diagnostics);
        }
        attach_elaborator_sources(diagnostics, sourcemap);

        json artifacts = {
            {"core_lockfile", {{"uri", ""}, {"content", core}}},
            {"source_map", {{"uri", ""}, {"content", sourcemap}}},
            {"diagnostics", {{"uri", ""}, {"content", diagnostics}}},
        };
        const bool ok = !diagnostics_have_errors(diagnostics);
        json result = {{"ok", ok}, {"diagnostics", diagnostics}, {"artifacts", artifacts}};
        if (!ok || !retain) return result;

        auto artifact = std::make_shared<Artifact>();
        artifact->id = id("artifact");
        artifact->request = request;
        artifact->core = core;
        artifact->revision_digest = revision_digest(request, core);
        artifact->sourcemap = sourcemap;
        artifact->diagnostics = diagnostics;
        const auto base_uri = "neograph://artifacts/" + artifact->id;
        result["artifact_id"] = artifact->id;
        result["artifacts"]["core_lockfile"]["uri"] = base_uri + "/core";
        result["artifacts"]["source_map"]["uri"] = base_uri + "/sourcemap";
        result["artifacts"]["diagnostics"]["uri"] = base_uri + "/diagnostics";

        std::lock_guard lock(mutex);
        cleanup_retained_locked(config.max_artifacts - 1, config.max_runs);
        if (!dynamic_cast<HarnessRetentionStore*>(config.record_store.get()) &&
            artifacts_by_id.size() >= config.max_artifacts && !artifacts_by_id.empty()) {
            artifacts_by_id.erase(artifacts_by_id.begin());
        }
        artifacts_by_id[artifact->id] = std::move(artifact);
        if (config.record_store) {
            const auto& retained = artifacts_by_id.at(result["artifact_id"].get<std::string>());
            config.record_store->save_artifact(retained->id, artifact_record(*retained));
        }
        return result;
    }

    std::shared_ptr<RecordedHarnessResults> load_recorded_results(
        const std::string& run_id,
        const std::string& artifact_id,
        const std::string& revision_digest,
        const std::string& protocol_version,
        const std::string& profile) const {
        if (!journal) {
            throw std::invalid_argument("recorded replay requires a Harness journal");
        }
        std::vector<json> events;
        std::size_t       cursor = 0;
        while (true) {
            const auto page = journal->list_events(run_id, cursor, 1000);
            if (page.empty()) break;
            for (const auto& event : page) {
                const auto sequence = event.value("sequence", std::size_t{0});
                if (sequence <= cursor) {
                    throw std::runtime_error("recorded replay journal sequence did not advance");
                }
                cursor = sequence;
                events.push_back(event);
            }
            if (page.size() < 1000) break;
        }
        return recorded_results_from_events(events, run_id, artifact_id, revision_digest,
                                            protocol_version, profile);
    }

    void execute(std::shared_ptr<Run>                    run,
                 std::shared_ptr<Artifact>               artifact,
                 std::optional<json>                     resume_value     = std::nullopt,
                 std::shared_ptr<RecordedHarnessResults> recorded_results = nullptr) {
        struct ExecutionFinished {
            std::shared_ptr<Run> run;
            ~ExecutionFinished() {
                std::lock_guard lock(run->mutex);
                run->execution_finished = true;
            }
        } execution_finished{run};
        {
            std::lock_guard lock(run->mutex);
            run->status             = "running";
            run->resume_scheduled   = resume_value.has_value();
            run->execution_finished = false;
            run->updated_at         = unix_millis();
        }
        try {
            persist_run(run);
            json lifecycle = {
                {"execution_mode", run->execution_mode},
                {"status", "running"},
            };
            if (!run->source_run_id.empty()) lifecycle["source_run_id"] = run->source_run_id;
            if (!run->source_checkpoint_id.empty()) {
                lifecycle["source_checkpoint_id"] = run->source_checkpoint_id;
            }
            detail::append_harness_journal_event(journal_context(*run),
                                                 resume_value ? "run.resumed" : "run.started",
                                                 std::move(lifecycle));
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(run->mutex);
                run->status = "failed";
                run->error = std::string("cannot initialize Harness journal: ") + error.what();
                run->resume_scheduled = false;
                run->updated_at = unix_millis();
            }
            try {
                persist_run(run);
            } catch (...) {}
            return;
        }

        const auto budgets = artifact->request.value("budgets", json::object());
        const int timeout_seconds = budgets.value("timeout_seconds", 600);
        std::atomic<bool> timed_out{false};
        std::mutex timer_mutex;
        std::condition_variable timer_cv;
        bool done = false;
        std::thread timer([&] {
            std::unique_lock lock(timer_mutex);
            if (!timer_cv.wait_for(lock, std::chrono::seconds(timeout_seconds),
                                   [&] { return done; })) {
                timed_out.store(true, std::memory_order_release);
                std::lock_guard run_lock(run->mutex);
                if (run->status == "running") run->cancel->cancel();
            }
        });

        std::unique_ptr<graph::GraphEngine> engine;
        try {
            auto provider = std::make_shared<HarnessRuntimeProvider>(
                artifact->request, config.worker_executor, journal_context(*run), recorded_results);
            graph::NodeContext context;
            context.provider = std::move(provider);
            std::shared_ptr<graph::CheckpointStore> checkpoint_store = config.checkpoint_store;
            if (checkpoint_store) {
                checkpoint_store = std::make_shared<BoundHarnessCheckpointStore>(
                    std::move(checkpoint_store),
                    harness_checkpoint_binding(run->id, run->artifact_id, run->revision_digest,
                                               run->protocol_version, run->profile));
            }
            engine =
                graph::GraphEngine::compile(artifact->core, context, std::move(checkpoint_store));
            engine->set_worker_count(
                static_cast<std::size_t>(budgets.value("max_parallel_workers", 4)));

            if (!run->source_checkpoint_id.empty()) {
                const auto fork_checkpoint_id =
                    engine->fork(run->source_run_id, run->id, run->source_checkpoint_id);
                detail::append_harness_journal_event(
                    journal_context(*run), "run.forked",
                    {{"source_run_id", run->source_run_id},
                     {"source_checkpoint_id", run->source_checkpoint_id},
                     {"target_artifact_id", run->artifact_id},
                     {"fork_checkpoint_id", fork_checkpoint_id}});
            }

            graph::RunConfig run_config;
            run_config.thread_id = run->id;
            run_config.input = {{"task", artifact->request["task"]}};
            run_config.max_steps = budgets.value("max_steps", 40);
            run_config.cancel_token = run->cancel;
            auto graph_result = !run->source_checkpoint_id.empty() ? engine->resume(run_config)
                                : resume_value ? engine->resume(run_config, *resume_value)
                                               : engine->run(run_config);
            if (recorded_results && graph_result.interrupted) {
                throw std::runtime_error(
                    "recorded replay cannot request live input or tool results");
            }
            if (recorded_results) recorded_results->require_exhausted();

            json interrupt_payload;
            std::string interrupt_correlation;
            {
                std::lock_guard lock(run->mutex);
                // CancelToken callbacks are bound to the engine's executor. Drain
                // that executor while holding the same lock used by cancel().
                engine.reset();
                run->cancel     = std::make_shared<graph::CancelToken>();
                run->updated_at = unix_millis();
                if (graph_result.interrupted) {
                    if (!graph_result.interrupt_value.contains("value") ||
                        !graph_result.interrupt_value["value"].is_object()) {
                        throw std::runtime_error(
                            "Harness interrupt omitted its pending-call payload");
                    }
                    auto       pending = graph_result.interrupt_value["value"];
                    const auto state   = pending.value("state", "");
                    if (state != "awaiting_tool_results" && state != "input_required") {
                        throw std::runtime_error(
                            "Harness interrupt returned an unsupported pending state");
                    }
                    run->pending = std::move(pending);
                    run->status  = state;
                    interrupt_payload = {{"pending", run->pending}, {"status", run->status}};
                    interrupt_correlation = run->pending.value("call_id", "");
                } else if (graph_result.max_steps_exhausted()) {
                    run->status = "max_steps_exhausted";
                } else {
                    run->status = "completed";
                    run->pending                    = nullptr;
                    run->details = graph_result.channel_raw("final_result");
                    run->details["execution_trace"] = graph_result.execution_trace;
                    const auto base_uri = "neograph://runs/" + run->id;
                    run->result                     = {
                        {"outcome", run->details.value("outcome", "unknown")},
                        {"valid_workers", run->details.value("valid_workers", 0)},
                        {"failed_workers", run->details.value("failed_workers", 0)},
                        {"finding_count", run->details.value("findings", json::array()).size()},
                        {"artifacts",
                                             {
                             {"details", {{"uri", base_uri + "/details"}}},
                             {"trace", {{"uri", base_uri + "/trace"}}},
                             {"attempts", {{"uri", base_uri + "/attempts"}}},
                             {"checkpoints", {{"uri", base_uri + "/checkpoints"}}},
                             {"diff", {{"uri", base_uri + "/diff"}}},
                         }},
                    };
                }
                run->resume_value     = nullptr;
                run->resume_scheduled = false;
            }
            if (!interrupt_payload.is_null()) {
                detail::append_harness_journal_event(
                    journal_context(*run), "run.interrupted", std::move(interrupt_payload),
                    std::move(interrupt_correlation));
            }
        } catch (const graph::CancelledException& error) {
            std::lock_guard lock(run->mutex);
            engine.reset();
            run->cancel       = std::make_shared<graph::CancelToken>();
            run->status       = timed_out.load(std::memory_order_acquire) ? "timeout" : "cancelled";
            run->error = error.what();
            run->resume_value = nullptr;
            run->resume_scheduled = false;
            run->updated_at       = unix_millis();
        } catch (const std::exception& error) {
            std::lock_guard lock(run->mutex);
            engine.reset();
            run->cancel           = std::make_shared<graph::CancelToken>();
            run->status = "failed";
            run->error = error.what();
            run->resume_value     = nullptr;
            run->resume_scheduled = false;
            run->updated_at       = unix_millis();
        } catch (...) {
            std::lock_guard lock(run->mutex);
            engine.reset();
            run->cancel           = std::make_shared<graph::CancelToken>();
            run->status = "failed";
            run->error = "unknown Harness execution failure";
            run->resume_value     = nullptr;
            run->resume_scheduled = false;
            run->updated_at       = unix_millis();
        }

        {
            std::lock_guard lock(timer_mutex);
            done = true;
            timer_cv.notify_all();
        }
        timer.join();
        try {
            persist_run(run);
            json terminal_payload;
            {
                std::lock_guard lock(run->mutex);
                if (run->status != "awaiting_tool_results" && run->status != "input_required") {
                    terminal_payload = {
                        {"error", run->error}, {"result", run->result}, {"status", run->status}};
                }
            }
            if (!terminal_payload.is_null()) {
                detail::append_harness_journal_event(
                    journal_context(*run), "run.terminal", std::move(terminal_payload));
            }
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(run->mutex);
                run->status = "failed";
                run->error = std::string("cannot finalize Harness journal: ") + error.what();
                run->updated_at = unix_millis();
            }
            try {
                persist_run(run);
            } catch (...) {}
        }
    }

    void ensure_resume_scheduled(const std::shared_ptr<Run>& run) {
        json        value;
        std::string artifact_id;
        bool        rejected_recorded_resume = false;
        {
            std::lock_guard lock(run->mutex);
            if (run->status != "queued" || run->resume_value.is_null() || run->resume_scheduled)
                return;
            if (run->execution_mode == "recorded_replay") {
                run->status              = "failed";
                run->error               = "recorded replay cannot resume with live input";
                run->resume_value        = nullptr;
                run->updated_at          = unix_millis();
                rejected_recorded_resume = true;
            } else {
                run->resume_scheduled   = true;
                run->execution_finished = false;
                value                   = run->resume_value;
                artifact_id             = run->artifact_id;
            }
        }
        if (rejected_recorded_resume) {
            persist_run(run);
            return;
        }
        auto artifact = find_artifact(artifact_id);
        if (!artifact) {
            {
                std::lock_guard lock(run->mutex);
                run->resume_scheduled = false;
                run->status           = "failed";
                run->error            = "Harness resume artifact is unavailable";
                run->updated_at       = unix_millis();
            }
            persist_run(run);
            return;
        }
        try {
            std::lock_guard lock(mutex);
            threads.emplace_back([this, run, artifact, value = std::move(value)]() mutable {
                execute(run, artifact, std::move(value));
            });
        } catch (...) {
            {
                std::lock_guard lock(run->mutex);
                run->resume_scheduled = false;
                run->status           = "failed";
                run->error            = "failed to schedule Harness resume";
                run->updated_at       = unix_millis();
            }
            persist_run(run);
            throw;
        }
    }

    HarnessServiceConfig config;
    std::shared_ptr<HarnessJournal> journal;
    std::uint64_t nonce;
    std::atomic<std::uint64_t> next_id{1};
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<Artifact>> artifacts_by_id;
    std::map<std::string, std::shared_ptr<Run>> runs;
    std::vector<std::thread> threads;
};

HarnessService::HarnessService(HarnessServiceConfig config)
    : HarnessService(std::move(config), nullptr) {}

HarnessService::HarnessService(HarnessServiceConfig config,
                               std::shared_ptr<HarnessJournal> journal)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(journal))) {}

HarnessService::~HarnessService() = default;

json HarnessService::schema() const {
    json       executor_kinds = json::array({
        {{"kind", "builtin"}, {"availability", "provider-capability-adapter"}},
        {{"kind", "mcp"}, {"availability", "downstream-client-adapter"}},
        {{"kind", "a2a"}, {"availability", "downstream-client-adapter"}},
        {{"kind", "host_brokered"},
               {"availability", impl_->config.checkpoint_store && impl_->config.record_store
                                    ? "durable-resume"
                                    : "requires-checkpoint-and-record-stores"}},
        {{"kind", "script"}, {"availability", "disabled"}},
    });
    const auto request = harness_request_schema();
    return {
        {"protocol_version", MCP_PROTOCOL_VERSION},
        {"service", "neograph-harness-m4"},
        {"presets",
         json::array({"fanout_judge", "pr_review_panel", "bug_triage", "research_synthesis"})},
        {"preset_contracts",
         {
             {"fanout_judge",
              {
                  {"topology", "parallel workers, barrier, normalized judge"},
              }},
             {"pr_review_panel",
              {
                  {"topology", "parallel review specialists, evidence judge"},
                  {"recommended_roles", json::array({"correctness", "security", "regression"})},
                  {"recommended_policy",
                   {
                       {"read_only", true},
                       {"evidence_required", json::array({"file", "line", "evidence"})},
                   }},
              }},
             {"bug_triage",
              {
                  {"topology", "parallel hypotheses, reproduction judge"},
                  {"recommended_roles", json::array({"reproducer", "root_cause", "fix_validator"})},
              }},
             {"research_synthesis",
              {
                  {"topology", "parallel sources, contradiction-aware judge"},
                  {"recommended_roles", json::array({"researcher", "skeptic", "synthesizer"})},
              }},
         }},
        {"executor_kinds", std::move(executor_kinds)},
        {"request_schema", request},
        {"schemas",
         {
             {"request", request},
             {"worker", request["properties"]["workers"]["items"]},
             {"tool_catalog_entry", request["properties"]["tool_catalog"]["items"]},
             {"budgets", request["properties"]["budgets"]},
             {"worker_result", worker_result_schema()},
             {"run_snapshot", run_snapshot_schema()},
         }},
        {"node_palette", graph::NodeFactory::instance().export_schema()},
        {"capabilities",
         {
             {"strict_core", true},
             {"translation_validation", true},
             {"semantic_validation", true},
             {"binding_validation", true},
             {"bounded_worker_repair", true},
             {"direct_provider_workers", true},
             {"downstream_mcp_tools", true},
             {"downstream_a2a_tools", true},
             {"read_only_workspace_policy", true},
             {"compact_results", true},
             {"durable_runs", static_cast<bool>(impl_->config.record_store)},
             {"recorded_result_replay", static_cast<bool>(impl_->journal)},
             {"live_replay", true},
             {"compatible_fork", static_cast<bool>(impl_->config.checkpoint_store)},
             {"host_brokered_resume",
              static_cast<bool>(impl_->config.checkpoint_store && impl_->config.record_store)},
             {"experimental_tasks", impl_->config.enable_experimental_tasks},
         }},
    };
}

json HarnessService::compile(const json& request) {
    return impl_->compile_request(request, true);
}

json HarnessService::start(const json& arguments) {
    if (!arguments.is_object()) {
        throw std::invalid_argument("neograph_start arguments must be an object");
    }
    std::string artifact_id;
    std::string                             execution_mode = "live";
    std::string                             source_run_id;
    std::string                             source_checkpoint_id;
    std::string                             source_revision_digest;
    std::string                             source_protocol_version;
    std::string                             source_profile;
    std::shared_ptr<Impl::Run>              fork_source;
    std::shared_ptr<RecordedHarnessResults> recorded_results;
    if (arguments.contains("replay")) {
        if (arguments.contains("request") || arguments.contains("artifact_id")) {
            throw std::invalid_argument(
                "neograph_start replay cannot include artifact_id or request");
        }
        const auto& replay = arguments["replay"];
        if (!replay.is_object() || replay.size() != 2 || !replay.contains("source_run_id") ||
            !replay["source_run_id"].is_string() || !replay.contains("mode") ||
            !replay["mode"].is_string()) {
            throw std::invalid_argument(
                "neograph_start replay requires only string source_run_id and mode");
        }
        source_run_id   = replay["source_run_id"].get<std::string>();
        const auto mode = replay["mode"].get<std::string>();
        if (source_run_id.empty() || (mode != "recorded" && mode != "live")) {
            throw std::invalid_argument(
                "neograph_start replay mode must be recorded or live with a source_run_id");
        }
        auto source = impl_->find_run(source_run_id);
        if (!source) {
            throw std::invalid_argument("unknown Harness replay source_run_id: " + source_run_id);
        }
        {
            std::lock_guard lock(source->mutex);
            if (source->status != "completed") {
                throw std::invalid_argument("Harness replay source run must be completed");
            }
            if (source->protocol_version != MCP_PROTOCOL_VERSION ||
                source->profile != kHarnessProfile) {
                throw std::invalid_argument(
                    "Harness replay source protocol or profile is incompatible");
            }
            artifact_id             = source->artifact_id;
            source_revision_digest  = source->revision_digest;
            source_protocol_version = source->protocol_version;
            source_profile          = source->profile;
            execution_mode          = mode == "recorded" ? "recorded_replay" : "live_replay";
        }
        if (mode == "recorded") {
            recorded_results =
                impl_->load_recorded_results(source_run_id, artifact_id, source_revision_digest,
                                             source_protocol_version, source_profile);
        }
    } else if (arguments.contains("fork")) {
        if (arguments.size() != 1) {
            throw std::invalid_argument("neograph_start fork cannot include other start modes");
        }
        const auto& fork = arguments["fork"];
        if (!fork.is_object() || fork.size() != 3 || !fork.contains("source_run_id") ||
            !fork["source_run_id"].is_string() || !fork.contains("checkpoint_id") ||
            !fork["checkpoint_id"].is_string() || !fork.contains("artifact_id") ||
            !fork["artifact_id"].is_string()) {
            throw std::invalid_argument(
                "neograph_start fork requires only string source_run_id, checkpoint_id, and "
                "artifact_id");
        }
        source_run_id        = fork["source_run_id"].get<std::string>();
        source_checkpoint_id = fork["checkpoint_id"].get<std::string>();
        artifact_id          = fork["artifact_id"].get<std::string>();
        if (source_run_id.empty() || source_checkpoint_id.empty() || artifact_id.empty()) {
            throw std::invalid_argument("neograph_start fork identifiers must not be empty");
        }
        fork_source = impl_->find_run(source_run_id);
        if (!fork_source) {
            throw std::invalid_argument("unknown Harness fork source_run_id: " + source_run_id);
        }
        {
            std::lock_guard lock(fork_source->mutex);
            source_revision_digest  = fork_source->revision_digest;
            source_protocol_version = fork_source->protocol_version;
            source_profile          = fork_source->profile;
        }
        execution_mode = "compatible_fork";
    } else if (arguments.contains("request")) {
        if (arguments.contains("artifact_id")) {
            throw std::invalid_argument(
                "neograph_start accepts exactly one of artifact_id or request");
        }
        auto compiled = impl_->compile_request(arguments["request"], true);
        if (!compiled.value("ok", false)) {
            return {
                {"started", false},
                {"status", "compile_failed"},
                {"diagnostics", compiled["diagnostics"]},
                {"artifacts", compiled["artifacts"]},
            };
        }
        artifact_id = compiled["artifact_id"].get<std::string>();
    } else if (arguments.contains("artifact_id") && arguments["artifact_id"].is_string()) {
        artifact_id = arguments["artifact_id"].get<std::string>();
    } else {
        throw std::invalid_argument(
            "neograph_start requires artifact_id, request, replay, or fork");
    }

    auto artifact = impl_->find_artifact(artifact_id);
    if (!artifact) {
        throw std::invalid_argument("unknown Harness artifact_id: " + artifact_id);
    }
    if (!source_run_id.empty() && execution_mode != "compatible_fork") {
        if (source_revision_digest != artifact->revision_digest) {
            throw std::invalid_argument("Harness replay source revision is unavailable");
        }
    }
    if (execution_mode == "compatible_fork") {
        json diagnostics = json::array();
        if (!impl_->config.checkpoint_store) {
            diagnostics.push_back(make_diagnostic("compatibility", "H_FORK_CHECKPOINT_STORE",
                                                  "error", "$.fork.checkpoint_id",
                                                  "compatible fork requires a checkpoint store"));
        }
        if (source_protocol_version != MCP_PROTOCOL_VERSION ||
            artifact->protocol_version != MCP_PROTOCOL_VERSION) {
            diagnostics.push_back(
                make_diagnostic("compatibility", "H_FORK_PROTOCOL", "error", "$.fork",
                                "source run and target artifact must use the current MCP protocol",
                                {{"source_protocol_version", source_protocol_version},
                                 {"target_protocol_version", artifact->protocol_version},
                                 {"runtime_protocol_version", MCP_PROTOCOL_VERSION}}));
        }
        if (source_profile != kHarnessProfile || artifact->profile != kHarnessProfile) {
            diagnostics.push_back(make_diagnostic(
                "compatibility", "H_FORK_PROFILE", "error", "$.fork",
                "source run and target artifact must use the current Harness profile",
                {{"source_profile", source_profile},
                 {"target_profile", artifact->profile},
                 {"runtime_profile", kHarnessProfile}}));
        }
        auto source_artifact = impl_->find_artifact(fork_source->artifact_id);
        if (!source_artifact) {
            diagnostics.push_back(make_diagnostic(
                "compatibility", "H_FORK_SOURCE_ARTIFACT", "error", "$.fork.source_run_id",
                "fork source run references an unavailable artifact",
                {{"source_run_id", source_run_id}, {"artifact_id", fork_source->artifact_id}}));
        } else if (source_artifact->revision_digest != source_revision_digest) {
            diagnostics.push_back(make_diagnostic(
                "compatibility", "H_FORK_SOURCE_REVISION", "error", "$.fork.source_run_id",
                "fork source run revision does not match its retained artifact",
                {{"run_revision_digest", source_revision_digest},
                 {"artifact_revision_digest", source_artifact->revision_digest}}));
        }

        std::optional<graph::Checkpoint> checkpoint;
        if (impl_->config.checkpoint_store) {
            checkpoint = impl_->config.checkpoint_store->load_by_id(source_checkpoint_id);
            if (!checkpoint) {
                diagnostics.push_back(make_diagnostic(
                    "compatibility", "H_FORK_CHECKPOINT_NOT_FOUND", "error", "$.fork.checkpoint_id",
                    "fork checkpoint is unavailable", {{"checkpoint_id", source_checkpoint_id}}));
            } else if (checkpoint->thread_id != source_run_id) {
                diagnostics.push_back(make_diagnostic(
                    "compatibility", "H_FORK_CHECKPOINT_OWNER", "error", "$.fork.checkpoint_id",
                    "fork checkpoint does not belong to the source run",
                    {{"checkpoint_id", source_checkpoint_id},
                     {"checkpoint_run_id", checkpoint->thread_id},
                     {"source_run_id", source_run_id}}));
            } else if (source_artifact) {
                const auto interface_diagnostics = fork_compatibility_diagnostics(
                    source_artifact->core, artifact->core, *checkpoint,
                    harness_checkpoint_binding(source_run_id, source_artifact->id,
                                               source_revision_digest, source_protocol_version,
                                               source_profile));
                for (const auto& diagnostic : interface_diagnostics) {
                    diagnostics.push_back(diagnostic);
                }
            }
        }
        if (diagnostics_have_errors(diagnostics)) {
            return {
                {"started", false},
                {"status", "incompatible_fork"},
                {"artifact_id", artifact_id},
                {"execution_mode", execution_mode},
                {"source_run_id", source_run_id},
                {"source_checkpoint_id", source_checkpoint_id},
                {"diagnostics", std::move(diagnostics)},
            };
        }
    }
    auto run = std::make_shared<Impl::Run>();
    {
        std::lock_guard lock(impl_->mutex);
        std::vector<std::string> protected_runs;
        if (!source_run_id.empty()) protected_runs.push_back(source_run_id);
        impl_->cleanup_retained_locked(impl_->config.max_artifacts, impl_->config.max_runs - 1,
                                       {artifact_id}, protected_runs);
        std::size_t active_runs = 0;
        for (const auto& [id, existing] : impl_->runs) {
            (void)id;
            std::lock_guard run_lock(existing->mutex);
            if (existing->status == "queued" || existing->status == "running" ||
                existing->status == "awaiting_tool_results" ||
                existing->status == "input_required") {
                ++active_runs;
            }
        }
        const auto retained_capacity =
            dynamic_cast<HarnessRetentionStore*>(impl_->config.record_store.get())
                ? active_runs
                : impl_->runs.size();
        if (retained_capacity >= impl_->config.max_runs) {
            throw std::runtime_error("Harness run capacity is exhausted");
        }
        run->id = impl_->id("run");
        run->artifact_id = artifact_id;
        run->revision_digest = artifact->revision_digest;
        run->execution_mode  = execution_mode;
        run->source_run_id   = source_run_id;
        run->source_checkpoint_id = source_checkpoint_id;
        run->expires_at      = run->created_at + impl_->config.run_ttl.count();
        impl_->runs[run->id] = run;
        try {
            impl_->persist_run(run);
            json lifecycle = {
                {"execution_mode", execution_mode},
                {"status", "queued"},
            };
            if (!source_run_id.empty()) lifecycle["source_run_id"] = source_run_id;
            if (!source_checkpoint_id.empty()) {
                lifecycle["source_checkpoint_id"] = source_checkpoint_id;
            }
            detail::append_harness_journal_event(impl_->journal_context(*run), "run.queued",
                                                 std::move(lifecycle));
            impl_->threads.emplace_back([impl             = impl_.get(), run, artifact,
                                         recorded_results = std::move(recorded_results)] {
                impl->execute(run, artifact, std::nullopt, std::move(recorded_results));
            });
        } catch (...) {
            impl_->runs.erase(run->id);
            throw;
        }
    }
    json response = {
        {"started", true},
        {"run_id", run->id},
        {"artifact_id", artifact_id},
        {"execution_mode", execution_mode},
        {"status", "queued"},
    };
    if (!source_run_id.empty()) response["source_run_id"] = source_run_id;
    if (!source_checkpoint_id.empty()) response["source_checkpoint_id"] = source_checkpoint_id;
    return response;
}

json HarnessService::resume(const json& arguments) {
    if (!arguments.is_object() || !arguments.contains("run_id") ||
        !arguments["run_id"].is_string() || !arguments.contains("call_id") ||
        !arguments["call_id"].is_string() || !arguments.contains("result")) {
        throw std::invalid_argument(
            "neograph_resume requires string run_id, string call_id, and result");
    }
    const auto run_id  = arguments["run_id"].get<std::string>();
    const auto call_id = arguments["call_id"].get<std::string>();
    auto       run     = impl_->find_run(run_id);
    if (!run) throw std::invalid_argument("unknown Harness run_id: " + run_id);
    auto artifact = impl_->find_artifact(run->artifact_id);
    if (!artifact) {
        throw std::runtime_error("Harness run references an unavailable artifact: " +
                                 run->artifact_id);
    }

    bool expired   = false;
    bool duplicate = false;
    {
        std::lock_guard lock(run->mutex);
        if (run->consumed.contains(call_id)) {
            if (run->consumed[call_id] != arguments["result"]) {
                throw std::invalid_argument(
                    "conflicting duplicate result for consumed Harness call_id");
            }
            duplicate = true;
        }
        if (!duplicate && run->expires_at > 0 && unix_millis() >= run->expires_at) {
            run->status     = "expired";
            run->pending    = nullptr;
            run->updated_at = unix_millis();
            expired         = true;
        }
        if (duplicate || expired) {
            // Persist after releasing the run lock below.
        } else {
            if (run->status != "awaiting_tool_results" && run->status != "input_required") {
                throw std::invalid_argument(
                    "late result rejected: Harness run is not awaiting input");
            }
            if (!run->pending.is_object() || run->pending.value("call_id", "") != call_id) {
                throw std::invalid_argument("Harness call_id does not match the pending call");
            }
            if (run->pending.contains("result_schema")) {
                validate_json_value(arguments["result"], run->pending["result_schema"],
                                    "Harness resume result", "$");
            }
            run->consumed[call_id] = arguments["result"];
            run->pending           = nullptr;
            run->status            = "queued";
            run->resume_value      = {
                {"call_id", call_id},
                {"result", arguments["result"]},
            };
            run->updated_at = unix_millis();
        }
    }
    if (!duplicate) impl_->persist_run(run);
    if (expired) throw std::invalid_argument("Harness run has expired");
    if (!duplicate) {
        detail::append_harness_journal_event(
            impl_->journal_context(*run), "host_brokered.result.accepted",
            {{"result", arguments["result"]}}, call_id);
    }
    impl_->ensure_resume_scheduled(run);
    std::string status;
    {
        std::lock_guard lock(run->mutex);
        status = run->status;
    }
    return {
        {"accepted", !duplicate}, {"duplicate", duplicate}, {"run_id", run_id},
        {"call_id", call_id},     {"status", status},
    };
}

json HarnessService::get(const std::string& run_id, const std::string& view) const {
    return get(run_id, view, 0, 100);
}

json HarnessService::get(const std::string& run_id,
                         const std::string& view,
                         std::size_t        after_sequence,
                         std::size_t        limit) const {
    if (limit == 0 || limit > 1000) {
        throw std::invalid_argument("Harness view limit must be between 1 and 1000");
    }
    auto run = impl_->find_run(run_id);
    if (!run) throw std::invalid_argument("unknown Harness run_id: " + run_id);
    const bool journal_view    = view == "trace" || view == "attempts";
    const bool checkpoint_view = view == "checkpoints" || view == "diff";
    const bool unpaged_view    = view == "status" || view == "details" || view == "artifacts";
    if (!journal_view && !checkpoint_view && !unpaged_view) {
        throw std::invalid_argument(
            "Harness view must be status, details, trace, attempts, checkpoints, diff, or "
            "artifacts");
    }
    if (after_sequence != 0 && !journal_view) {
        throw std::invalid_argument("after_sequence applies only to trace and attempts views");
    }
    if (limit != 100 && unpaged_view) {
        throw std::invalid_argument("limit applies only to debugger views");
    }
    bool expired = false;
    {
        std::lock_guard lock(run->mutex);
        if ((run->status == "awaiting_tool_results" || run->status == "input_required") &&
            run->expires_at > 0 && unix_millis() >= run->expires_at) {
            run->status     = "expired";
            run->pending    = nullptr;
            run->updated_at = unix_millis();
            expired         = true;
        }
    }
    if (expired) impl_->persist_run(run);
    impl_->ensure_resume_scheduled(run);
    json        snapshot;
    json        result;
    json        details;
    std::string error;
    {
        std::lock_guard lock(run->mutex);
        auto            visible_status = run->status;
        if (!run->execution_finished &&
            (visible_status == "completed" || visible_status == "failed" ||
             visible_status == "cancelled" || visible_status == "timeout" ||
             visible_status == "max_steps_exhausted")) {
            visible_status = "running";
        }
        snapshot = {
            {"run_id", run->id},
            {"artifact_id", run->artifact_id},
            {"revision_digest", run->revision_digest},
            {"protocol_version", run->protocol_version},
            {"profile", run->profile},
            {"execution_mode", run->execution_mode},
            {"status", std::move(visible_status)},
            {"created_at", run->created_at},
            {"updated_at", run->updated_at},
            {"expires_at", run->expires_at},
            {"poll_after_ms", impl_->config.poll_interval.count()},
        };
        if (!run->source_run_id.empty()) snapshot["source_run_id"] = run->source_run_id;
        if (!run->source_checkpoint_id.empty()) {
            snapshot["source_checkpoint_id"] = run->source_checkpoint_id;
        }
        if (!run->pending.is_null()) snapshot["pending"] = run->pending;
        result  = run->result;
        details = run->details;
        error   = run->error;
    }

    if (view == "status") {
        if (!result.is_null()) snapshot["result"] = std::move(result);
    } else if (view == "details") {
        if (!details.is_null()) snapshot["result"] = std::move(details);
    } else if (view == "trace") {
        auto page = journal_page(impl_->journal, run_id, after_sequence, limit, false);
        page["execution_trace"] =
            details.is_object() ? details.value("execution_trace", json::array()) : json::array();
        snapshot["result"] = std::move(page);
    } else if (view == "attempts") {
        snapshot["result"] = journal_page(impl_->journal, run_id, after_sequence, limit, true);
    } else if (view == "checkpoints") {
        json checkpoints = json::array();
        if (impl_->config.checkpoint_store) {
            for (const auto& checkpoint :
                 impl_->config.checkpoint_store->list(run_id, static_cast<int>(limit))) {
                checkpoints.push_back(checkpoint_summary(checkpoint));
            }
        }
        snapshot["result"] = {
            {"available", static_cast<bool>(impl_->config.checkpoint_store)},
            {"checkpoints", std::move(checkpoints)},
            {"limit", limit},
        };
    } else if (view == "diff") {
        json diffs = json::array();
        if (impl_->config.checkpoint_store) {
            const auto checkpoints =
                impl_->config.checkpoint_store->list(run_id, static_cast<int>(limit));
            for (const auto& checkpoint : checkpoints) {
                std::optional<graph::Checkpoint> parent;
                if (!checkpoint.parent_id.empty()) {
                    parent = impl_->config.checkpoint_store->load_by_id(checkpoint.parent_id);
                    if (parent && parent->thread_id != run_id) parent.reset();
                }
                diffs.push_back(checkpoint_diff(checkpoint, parent));
            }
        }
        snapshot["result"] = {
            {"available", static_cast<bool>(impl_->config.checkpoint_store)},
            {"diffs", std::move(diffs)},
            {"limit", limit},
        };
    } else if (view == "artifacts") {
        if (!result.is_null()) {
            snapshot["result"] = result.value("artifacts", json::object());
        }
    }
    if (!error.empty()) snapshot["error"] = std::move(error);
    return snapshot;
}

json HarnessService::read(const std::string& uri) const {
    constexpr std::string_view prefix = "neograph://runs/";
    if (uri.rfind(prefix.data(), 0) != 0) {
        throw std::invalid_argument("unsupported Harness artifact URI: " + uri);
    }
    const auto remainder = uri.substr(prefix.size());
    const auto query_separator = remainder.find('?');
    const auto path            = remainder.substr(0, query_separator);
    const auto separator       = path.rfind('/');
    if (separator == std::string::npos || separator == 0 || separator + 1 == path.size()) {
        throw std::invalid_argument("malformed Harness artifact URI: " + uri);
    }
    std::size_t after_sequence     = 0;
    std::size_t limit              = 100;
    bool        saw_after_sequence = false;
    bool        saw_limit          = false;
    if (query_separator != std::string::npos) {
        const auto query = remainder.substr(query_separator + 1);
        if (query.empty() || query.back() == '&') {
            throw std::invalid_argument("malformed Harness artifact URI query: " + uri);
        }
        std::size_t offset = 0;
        while (offset < query.size()) {
            const auto end    = query.find('&', offset);
            const auto part   = query.substr(offset, end == std::string::npos ? end : end - offset);
            const auto equals = part.find('=');
            if (equals == std::string::npos || equals == 0 || equals + 1 == part.size()) {
                throw std::invalid_argument("malformed Harness artifact URI query: " + uri);
            }
            const auto key   = part.substr(0, equals);
            const auto value = part.substr(equals + 1);
            if (key == "after_sequence") {
                if (saw_after_sequence) {
                    throw std::invalid_argument("duplicate Harness artifact URI query: " + key);
                }
                saw_after_sequence = true;
                after_sequence     = parse_view_size(value, "after_sequence");
            } else if (key == "limit") {
                if (saw_limit) {
                    throw std::invalid_argument("duplicate Harness artifact URI query: " + key);
                }
                saw_limit = true;
                limit     = parse_view_size(value, "limit");
            } else {
                throw std::invalid_argument("unknown Harness artifact URI query: " + key);
            }
            if (end == std::string::npos) break;
            offset = end + 1;
        }
    }
    return get(path.substr(0, separator), path.substr(separator + 1), after_sequence, limit);
}

bool HarnessService::cancel(const std::string& run_id) {
    auto run = impl_->find_run(run_id);
    if (!run) return false;
    {
        std::lock_guard lock(run->mutex);
        if (run->status != "queued" && run->status != "running" &&
            run->status != "awaiting_tool_results" && run->status != "input_required")
            return false;
        if (run->status == "awaiting_tool_results" || run->status == "input_required") {
            run->status     = "cancelled";
            run->pending    = nullptr;
            run->updated_at = unix_millis();
        }
        run->cancel->cancel();
    }
    impl_->persist_run(run);
    detail::append_harness_journal_event(
        impl_->journal_context(*run), "run.cancel.requested", {{"status", "cancelled"}});
    return true;
}

void HarnessService::register_tools(MCPServer& server) {
    server.register_tool(
        tool_definition(
            "neograph_schema", "NeoGraph Harness schema",
            "Return build-specific Harness request schemas, presets, executor kinds, and node "
            "palette.",
            json::parse(
                R"JSON({"type":"object","properties":{},"additionalProperties":false})JSON"),
            json::parse(
                R"JSON({"type":"object","required":["service","request_schema","node_palette"],"properties":{"service":{"type":"string"},"request_schema":{"type":"object"},"node_palette":{"type":"object"}},"additionalProperties":true})JSON"),
            true),
        [this](const json&, const auto&) {
            auto value = schema();
            return mcp_result(std::move(value), "NeoGraph Harness M4 schema");
        });

    server.register_tool(
        tool_definition("neograph_compile", "Compile Harness",
                        "Elaborate, strict-compile, translation-validate, semantic-validate, and "
                        "binding-validate a Harness request.",
                        harness_request_schema(), compile_output_schema()),
        [this](const json& arguments, const auto&) {
            auto value = compile(arguments);
            const auto text = value.value("ok", false)
                ? "Harness compiled: " + value.value("artifact_id", "")
                : "Harness rejected by compiler diagnostics";
            return mcp_result(std::move(value), text);
        });

    auto start_definition = tool_definition(
        "neograph_start", "Start Harness",
        "Start a retained artifact, compile-and-start an inline request, or replay a completed "
        "run, or compatibility-check and fork a retained artifact from an exact checkpoint.",
        json::parse(
            R"JSON({"type":"object","description":"Provide exactly one of artifact_id, request, replay, or fork.","properties":{"artifact_id":{"type":"string","description":"Artifact returned by neograph_compile."},"request":{"type":"object","description":"Inline Harness request; do not also provide artifact_id."},"replay":{"type":"object","required":["source_run_id","mode"],"properties":{"source_run_id":{"type":"string"},"mode":{"enum":["recorded","live"],"description":"recorded injects FULL journal results without live calls; live executes the source artifact again."}},"additionalProperties":false},"fork":{"type":"object","required":["source_run_id","checkpoint_id","artifact_id"],"properties":{"source_run_id":{"type":"string"},"checkpoint_id":{"type":"string","description":"Exact preceding checkpoint to branch from."},"artifact_id":{"type":"string","description":"Compiled target artifact whose channel and continuation interfaces are compatibility-checked before the fork is retained."}},"additionalProperties":false}},"additionalProperties":false})JSON"),
        json::parse(
            R"JSON({"type":"object","required":["started","status"],"properties":{"started":{"type":"boolean"},"run_id":{"type":"string"},"artifact_id":{"type":"string"},"execution_mode":{"enum":["live","live_replay","recorded_replay","compatible_fork"]},"source_run_id":{"type":"string"},"source_checkpoint_id":{"type":"string"},"status":{"type":"string"},"diagnostics":{"type":"array"},"artifacts":{"type":"object"}},"additionalProperties":true})JSON"));
    if (impl_->config.enable_experimental_tasks) {
        start_definition.execution = {{"taskSupport", "optional"}};
        server.register_raw_tool(
            std::move(start_definition),
            [this](const json& arguments, const auto&, const json& meta) {
                auto value = start(arguments);
                if (value.value("started", false) && tasks_requested(meta)) {
                    return task_from_snapshot(get(value["run_id"].get<std::string>()), true);
                }
                const auto text = value.value("started", false)
                                      ? "Harness started: " + value.value("run_id", "")
                                      : "Harness did not start";
                return mcp_result(std::move(value), text).to_json();
            });
    } else {
        server.register_tool(std::move(start_definition), [this](const json& arguments,
                                                                 const auto&) {
            auto value = start(arguments);
            const auto text = value.value("started", false)
                ? "Harness started: " + value.value("run_id", "")
                : "Harness did not start";
            return mcp_result(std::move(value), text);
        });
    }

    server.register_tool(
        tool_definition(
            "neograph_get", "Get Harness run",
            "Read compact run status or dereference a detailed neograph:// run artifact URI.",
            json::parse(
                R"JSON({"type":"object","required":["run_id"],"properties":{"run_id":{"type":"string"},"view":{"enum":["status","details","trace","attempts","checkpoints","diff","artifacts"],"description":"Named view used when uri is absent."},"uri":{"type":"string","description":"Returned artifact URI. When present, its embedded view and query are authoritative."},"after_sequence":{"type":"integer","minimum":0,"description":"Journal cursor for trace and attempts views."},"limit":{"type":"integer","minimum":1,"maximum":1000,"description":"Maximum journal events, checkpoints, or diffs to return."}},"additionalProperties":false})JSON"),
            run_snapshot_schema(), true),
        [this](const json& arguments, const auto&) {
            const bool has_uri = arguments.contains("uri");
            const auto run_id = arguments["run_id"].get<std::string>();
            auto       value   = has_uri ? read(arguments["uri"].get<std::string>())
                                         : get(run_id, arguments.value("view", "status"),
                                               arguments.value("after_sequence", std::uint64_t{0}),
                                               arguments.value("limit", std::uint64_t{100}));
            if (value.value("run_id", "") != run_id) {
                throw std::invalid_argument("Harness artifact URI does not belong to run_id");
            }
            const auto text = "Harness run " + value.value("run_id", "") + ": " +
                              value.value("status", "unknown");
            return mcp_result(std::move(value), text);
        });

    server.register_tool(
        tool_definition(
            "neograph_resume", "Resume Harness run",
            "Submit the exact pending host result and resume from the durable checkpoint.",
            json::parse(
                R"JSON({"type":"object","required":["run_id","call_id","result"],"properties":{"run_id":{"type":"string"},"call_id":{"type":"string"},"result":{}} ,"additionalProperties":false})JSON"),
            json::parse(
                R"JSON({"type":"object","required":["accepted","duplicate","run_id","call_id","status"],"properties":{"accepted":{"type":"boolean"},"duplicate":{"type":"boolean"},"run_id":{"type":"string"},"call_id":{"type":"string"},"status":{"type":"string"}},"additionalProperties":false})JSON")),
        [this](const json& arguments, const auto&) {
            auto       value = resume(arguments);
            const auto text  = value.value("duplicate", false)
                                   ? "Harness result was already consumed"
                                   : "Harness resume accepted";
            return mcp_result(std::move(value), text);
        });

    server.register_tool(
        tool_definition(
            "neograph_cancel", "Cancel Harness run",
            "Cooperatively cancel a queued, running, or input-waiting Harness run.",
            json::parse(
                R"JSON({"type":"object","required":["run_id"],"properties":{"run_id":{"type":"string"}},"additionalProperties":false})JSON"),
            json::parse(
                R"JSON({"type":"object","required":["run_id","cancelled"],"properties":{"run_id":{"type":"string"},"cancelled":{"type":"boolean"}},"additionalProperties":false})JSON")),
        [this](const json& arguments, const auto&) {
            const auto run_id = arguments["run_id"].get<std::string>();
            const bool accepted = cancel(run_id);
            json value = {{"run_id", run_id}, {"cancelled", accepted}};
            return mcp_result(std::move(value), accepted ? "Harness cancellation requested"
                                                         : "Harness run is unknown or terminal");
        });

    if (!impl_->config.enable_experimental_tasks) return;
    server.register_extension(kTasksExtension);
    server.register_method("tasks/get", [this](const json& params, const json& context) {
        require_tasks_negotiated(context);
        if (!params.is_object() || params.size() != 1 || !params.contains("taskId") ||
            !params["taskId"].is_string()) {
            throw std::invalid_argument("tasks/get requires only string taskId");
        }
        return task_from_snapshot(get(params["taskId"].get<std::string>()));
    });
    server.register_method("tasks/update", [this](const json& params, const json& context) {
        require_tasks_negotiated(context);
        if (!params.is_object() || params.size() != 2 || !params.contains("taskId") ||
            !params["taskId"].is_string() || !params.contains("inputResponses") ||
            !params["inputResponses"].is_object() || params["inputResponses"].size() != 1) {
            throw std::invalid_argument(
                "tasks/update requires taskId and exactly one input response");
        }
        const auto& responses = params["inputResponses"];
        const auto  response  = responses.begin();
        auto        supplied  = response.value();
        if (supplied.is_object() && supplied.contains("content")) {
            supplied = supplied["content"];
        }
        (void)resume({
            {"run_id", params["taskId"]},
            {"call_id", response.key()},
            {"result", std::move(supplied)},
        });
        return json{{"resultType", "complete"}};
    });
    server.register_method("tasks/cancel", [this](const json& params, const json& context) {
        require_tasks_negotiated(context);
        if (!params.is_object() || params.size() != 1 || !params.contains("taskId") ||
            !params["taskId"].is_string()) {
            throw std::invalid_argument("tasks/cancel requires only string taskId");
        }
        if (!cancel(params["taskId"].get<std::string>())) {
            throw std::invalid_argument("task is unknown or terminal");
        }
        return json{{"resultType", "complete"}};
    });
}

} // namespace neograph::mcp
