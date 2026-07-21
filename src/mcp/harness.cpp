#include <neograph/mcp/harness.h>

#include <neograph/graph/compiler.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/validator.h>
#include <neograph/mcp/json_schema.h>
#include <neograph/mcp/server.h>
#include <neograph/provider.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neograph::mcp {
namespace {

constexpr const char* kWorkerNodeType = "neograph_harness_worker";
constexpr const char* kJudgeNodeType = "neograph_harness_judge";

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
                "executor":{"type":"object","required":["kind"],"properties":{
                    "kind":{"enum":["builtin","mcp","a2a","host_brokered","script"]},
                    "tool":{"type":"string"},
                    "server_ref":{"type":"string"},
                    "agent":{"type":"string"}
                },"additionalProperties":true}
            },"additionalProperties":false}},
            "budgets":{"type":"object","properties":{
                "max_steps":{"type":"integer"},
                "timeout_seconds":{"type":"integer"},
                "max_parallel_workers":{"type":"integer"},
                "max_worker_retries":{"type":"integer"}
            },"additionalProperties":false},
            "policy":{"type":"object","properties":{
                "read_only":{"type":"boolean"}
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
            "status":{"type":"string"},
            "result":{"type":"object"},
            "error":{"type":"string"}
        },"additionalProperties":true
    })JSON");
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

json make_diagnostic(std::string phase, std::string code,
                     std::string severity, std::string path,
                     std::string message, json witness = json::object(),
                     std::string source = {}) {
    if (source.empty()) source = path;
    json value = {
        {"phase", std::move(phase)},
        {"code", std::move(code)},
        {"severity", std::move(severity)},
        {"path", std::move(path)},
        {"message", std::move(message)},
        {"witness", std::move(witness)},
        {"source", std::move(source)},
    };
    return value;
}

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
    } else if (path.rfind("nodes.", 0) != 0
               && path.rfind("channels.", 0) != 0
               && path.rfind("edges", 0) != 0
               && path.rfind("conditional_edges", 0) != 0) {
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
    case HarnessWorkerResponseKind::VALUE: return "value";
    case HarnessWorkerResponseKind::EMPTY: return "empty_response";
    case HarnessWorkerResponseKind::PARSE_ERROR: return "parse_error";
    case HarnessWorkerResponseKind::TOOL_ERROR: return "tool_error";
    case HarnessWorkerResponseKind::TIMEOUT: return "timeout";
    case HarnessWorkerResponseKind::CANCELLED: return "cancelled";
    }
    return "unknown";
}

class HarnessRuntimeProvider final : public Provider {
public:
    HarnessRuntimeProvider(json request, HarnessWorkerExecutor executor)
        : request_(std::move(request)), executor_(std::move(executor)) {
        for (const auto& worker : request_["workers"]) {
            workers_[worker["id"].get<std::string>()] = worker;
        }
        for (const auto& tool : request_.value("tool_catalog", json::array())) {
            tools_[tool["id"].get<std::string>()] = tool;
        }
        max_retries_ = request_.value("budgets", json::object())
                           .value("max_worker_retries", 1);
    }

    std::string get_name() const override { return "harness-worker-executor"; }

    json execute(const std::string& worker_id, const json& task,
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
        for (int attempt = 0; attempt <= max_retries_; ++attempt) {
            attempts_made = attempt + 1;
            cancel->throw_if_cancelled("before Harness worker " + worker_id);
            HarnessWorkerCall call;
            call.task = task;
            call.worker = worker_it->second;
            call.tool_catalog = selected_tools;
            call.attempt = static_cast<std::size_t>(attempt + 1);
            call.repair_feedback = feedback;

            HarnessWorkerResponse response;
            if (!executor_) {
                response = HarnessWorkerResponse::tool_error(
                    "no Harness worker executor is configured");
            } else {
                response = executor_(call, cancel);
            }

            if (response.kind == HarnessWorkerResponseKind::CANCELLED) {
                throw graph::CancelledException(response.message.empty()
                    ? "Harness worker cancelled" : response.message);
            }

            if (response.kind == HarnessWorkerResponseKind::VALUE) {
                if (response.value.is_null()
                    || (response.value.is_string()
                        && response.value.get<std::string>().empty())) {
                    failure_kind = "empty_response";
                    feedback = "worker returned an empty response";
                } else {
                    try {
                        validate_json_value(response.value,
                            worker_it->second["output_schema"],
                            "Harness worker output", "$");
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
                feedback = response.message.empty() ? failure_kind : response.message;
                if (response.kind == HarnessWorkerResponseKind::TIMEOUT
                    || response.kind == HarnessWorkerResponseKind::TOOL_ERROR) {
                    break;
                }
            }
        }

        return {
            {"worker_id", worker_id},
            {"status", "failed"},
            {"failure_kind", failure_kind},
            {"message", feedback},
            {"attempts", attempts_made},
        };
    }

private:
    json request_;
    HarnessWorkerExecutor executor_;
    std::map<std::string, json> workers_;
    std::map<std::string, json> tools_;
    int max_retries_ = 1;
};

class HarnessWorkerNode final : public graph::GraphNode {
public:
    HarnessWorkerNode(std::string name, std::string worker_id,
                      std::shared_ptr<HarnessRuntimeProvider> runtime)
        : name_(std::move(name)), worker_id_(std::move(worker_id)),
          runtime_(std::move(runtime)) {}

    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        auto cancel = in.ctx.cancel_token;
        if (!cancel) cancel = std::make_shared<graph::CancelToken>();
        auto result = runtime_->execute(worker_id_, in.state.get("task"), cancel);
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
        for (const auto& result : results) workers.push_back(result);
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
        if (valid == 0 && failed > 0) outcome = "failed";
        else if (failed > 0 || partial) outcome = "partial";
        else if (findings_contract && findings.empty()) outcome = "zero_findings";

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
        graph::NodeFactory::instance().register_type(kWorkerNodeType,
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
            json::parse(R"JSON({"type":"object","required":["worker_id"],"properties":{"worker_id":{"type":"string"}},"additionalProperties":false})JSON"),
            json::parse(R"JSON({"reads":["task"],"writes":["worker_results"]})JSON"));

        graph::NodeFactory::instance().register_type(kJudgeNodeType,
            [](const std::string& name, const json&,
               const graph::NodeContext&) -> std::unique_ptr<graph::GraphNode> {
                return std::make_unique<HarnessJudgeNode>(name);
            },
            json::parse(R"JSON({"type":"object","properties":{},"additionalProperties":false})JSON"),
            json::parse(R"JSON({"reads":["worker_results"],"writes":["final_result"]})JSON"));
    });
}

json preset_fanout_judge(const json& request) {
    json core = {
        {"schema_version", 1},
        {"name", "harness_fanout_judge"},
        {"channels", {
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

void validate_range(const json& budgets, const char* name, int minimum,
                    int maximum, int default_value, json& diagnostics) {
    const int value = budgets.value(name, default_value);
    if (value < minimum || value > maximum) {
        diagnostics.push_back(make_diagnostic("request", "H_REQUEST_BUDGET",
            "error", std::string("$.budgets.") + name,
            std::string(name) + " must be between " + std::to_string(minimum)
                + " and " + std::to_string(maximum),
            {{"value", value}, {"minimum", minimum}, {"maximum", maximum}}));
    }
}

void validate_bindings(const json& request, const json& core,
                       json& diagnostics) {
    std::map<std::string, json> workers;
    std::map<std::string, json> tools;
    std::set<std::string> duplicates;

    std::size_t index = 0;
    for (const auto& worker : request["workers"]) {
        const auto id = worker.value("id", "");
        const auto path = "$.workers[" + std::to_string(index++) + "]";
        if (id.empty()) {
            diagnostics.push_back(make_diagnostic("binding", "H_WORKER_ID",
                "error", path + ".id", "worker id must not be empty"));
            continue;
        }
        if (!workers.emplace(id, worker).second) duplicates.insert(id);
        try {
            validate_json_schema(worker["output_schema"], path + ".output_schema");
        } catch (const std::exception& error) {
            diagnostics.push_back(make_diagnostic("binding", "H_WORKER_SCHEMA",
                "error", path + ".output_schema", error.what()));
        }
    }
    for (const auto& id : duplicates) {
        diagnostics.push_back(make_diagnostic("binding", "H_DUPLICATE_WORKER",
            "error", "$.workers", "duplicate worker id: " + id, {{"id", id}}));
    }

    index = 0;
    for (const auto& tool : request.value("tool_catalog", json::array())) {
        const auto id = tool.value("id", "");
        const auto path = "$.tool_catalog[" + std::to_string(index++) + "]";
        if (id.empty() || !tools.emplace(id, tool).second) {
            diagnostics.push_back(make_diagnostic("binding", "H_DUPLICATE_TOOL",
                "error", path + ".id", id.empty() ? "tool id must not be empty"
                                                    : "duplicate tool id: " + id));
        }
        try {
            validate_json_schema(tool["input_schema"], path + ".input_schema");
            if (tool.contains("output_schema")) {
                validate_json_schema(tool["output_schema"], path + ".output_schema");
            }
        } catch (const std::exception& error) {
            diagnostics.push_back(make_diagnostic("binding", "H_TOOL_SCHEMA",
                "error", path, error.what()));
        }

        const auto executor = tool.value("executor", json::object());
        const auto kind = executor.value("kind", "");
        if (kind == "mcp" && executor.value("server_ref", "").empty()) {
            diagnostics.push_back(make_diagnostic("binding", "H_MCP_UNRESOLVED",
                "error", path + ".executor.server_ref",
                "mcp executor requires a resolvable server_ref"));
        } else if (kind == "a2a" && executor.value("agent", "").empty()) {
            diagnostics.push_back(make_diagnostic("binding", "H_A2A_UNRESOLVED",
                "error", path + ".executor.agent",
                "a2a executor requires an agent identifier"));
        } else if (kind == "host_brokered") {
            diagnostics.push_back(make_diagnostic("binding", "H_HOST_BROKER_UNAVAILABLE",
                "error", path + ".executor.kind",
                "host_brokered tools require neograph_resume, planned for M4"));
        } else if (kind == "script") {
            diagnostics.push_back(make_diagnostic("binding", "H_SCRIPT_DISABLED",
                "error", path + ".executor.kind",
                "script executors are disabled by default"));
        }

        const bool read_only = request.value("policy", json::object())
                                   .value("read_only", false);
        if (read_only && !tool.value("read_only", false)) {
            diagnostics.push_back(make_diagnostic("binding", "H_WRITE_TOOL",
                "error", path, "read-only harness contains a write-capable tool",
                {{"tool_id", id}}));
        }
    }

    for (const auto& [id, worker] : workers) {
        for (const auto& tool_id_json : worker.value("tools", json::array())) {
            const auto tool_id = tool_id_json.get<std::string>();
            if (tools.find(tool_id) == tools.end()) {
                diagnostics.push_back(make_diagnostic("binding", "H_UNKNOWN_TOOL",
                    "error", "$.workers." + id + ".tools",
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
                    diagnostics.push_back(make_diagnostic("binding", "H_UNKNOWN_WORKER",
                        "error", "$.harness.definition.nodes." + node_name,
                        "topology references unknown worker: " + worker_id,
                        {{"node", node_name}, {"worker_id", worker_id}}));
                }
            } else if (type == kJudgeNodeType) {
                ++judges;
            } else {
                diagnostics.push_back(make_diagnostic("binding", "H_NODE_TYPE",
                    "error", "$.harness.definition.nodes." + node_name + ".type",
                    "M2 Harness DSL only permits compiler-backed worker and judge nodes",
                    {{"node", node_name}, {"type", type}}));
            }
        }
    }
    for (const auto& [id, worker] : workers) {
        (void)worker;
        if (topology_workers[id] != 1) {
            diagnostics.push_back(make_diagnostic("binding", "H_WORKER_BINDING",
                "error", "$.harness.definition.nodes",
                "each declared worker must be bound exactly once",
                {{"worker_id", id}, {"bindings", topology_workers[id]}}));
        }
    }
    if (judges != 1) {
        diagnostics.push_back(make_diagnostic("binding", "H_JUDGE_BINDING",
            "error", "$.harness.definition.nodes",
            "fanout_judge topology requires exactly one result-reading judge",
            {{"judges", judges}}));
    }
}

CallToolResult mcp_result(json structured, std::string text) {
    CallToolResult result;
    result.content = json::array({{{"type", "text"}, {"text", std::move(text)}}});
    result.structured_content = std::move(structured);
    return result;
}

ToolDefinition tool_definition(std::string name, std::string title,
                               std::string description, json input, json output,
                               bool read_only = false) {
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

struct HarnessService::Impl {
    struct Artifact {
        std::string id;
        json request;
        json core;
        json sourcemap;
        json diagnostics;
    };

    struct Run {
        mutable std::mutex mutex;
        std::string id;
        std::string artifact_id;
        std::string status = "queued";
        json result;
        std::string error;
        std::shared_ptr<graph::CancelToken> cancel =
            std::make_shared<graph::CancelToken>();
    };

    explicit Impl(HarnessServiceConfig value)
        : config(std::move(value)), nonce(std::random_device{}()) {
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
        for (const auto& run : active) run->cancel->cancel();
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

    json compile_request(const json& request, bool retain) {
        json diagnostics = json::array();
        json core;
        json sourcemap = json::array();

        try {
            validate_json_value(request, harness_request_schema(),
                                "Harness request", "$");
        } catch (const std::exception& error) {
            diagnostics.push_back(make_diagnostic("request", "H_REQUEST_SCHEMA",
                "error", "$", error.what()));
        }

        if (!diagnostics_have_errors(diagnostics)) {
            if (request["workers"].empty()) {
                diagnostics.push_back(make_diagnostic("request", "H_WORKERS_EMPTY",
                    "error", "$.workers", "at least one worker is required"));
            }
            const auto harness = request["harness"];
            const auto mode = harness.value("mode", "");
            if (mode == "preset") {
                if (harness.value("preset", "") != "fanout_judge") {
                    diagnostics.push_back(make_diagnostic("request", "H_PRESET",
                        "error", "$.harness.preset",
                        "M2 supports the fanout_judge preset"));
                }
            } else if (mode == "dsl" && !harness.contains("definition")) {
                diagnostics.push_back(make_diagnostic("request", "H_DSL_DEFINITION",
                    "error", "$.harness.definition",
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
                    ? preset_fanout_judge(request)
                    : harness["definition"];
                auto elaborated = graph::Elaborator::elaborate(surface);
                core = std::move(elaborated.core);
                sourcemap = std::move(elaborated.sourcemap);
                if (core.value("schema_version", 0) != 1) {
                    diagnostics.push_back(make_diagnostic("elaboration", "H_STRICT_CORE",
                        "error", "$.harness.definition.schema_version",
                        "Harness core must declare schema_version: 1"));
                }
            } catch (const std::exception& error) {
                diagnostics.push_back(make_diagnostic("elaboration", "H_ELABORATION",
                    "error", "$.harness.definition", error.what()));
            }
        }

        if (!diagnostics_have_errors(diagnostics)) {
            try {
                auto provider = std::make_shared<HarnessRuntimeProvider>(
                    request, HarnessWorkerExecutor{});
                graph::NodeContext context;
                context.provider = provider;
                auto compiled = graph::GraphCompiler::compile(core, context);
                graph::GraphCompiler::verify_roundtrip(core, compiled);
                auto report = graph::GraphValidator::validate(compiled);
                for (const auto& diagnostic : report.diagnostics) {
                    diagnostics.push_back(make_diagnostic("static", diagnostic.code,
                        diagnostic.severity, diagnostic.path, diagnostic.message,
                        diagnostic.witness));
                }
            } catch (const std::exception& error) {
                diagnostics.push_back(make_diagnostic("compile", "H_COMPILE",
                    "error", "$.harness.definition", error.what()));
            }
        }

        if (!core.is_null()) validate_bindings(request, core, diagnostics);
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
        artifact->sourcemap = sourcemap;
        artifact->diagnostics = diagnostics;
        const auto base_uri = "neograph://artifacts/" + artifact->id;
        result["artifact_id"] = artifact->id;
        result["artifacts"]["core_lockfile"]["uri"] = base_uri + "/core";
        result["artifacts"]["source_map"]["uri"] = base_uri + "/sourcemap";
        result["artifacts"]["diagnostics"]["uri"] = base_uri + "/diagnostics";

        std::lock_guard lock(mutex);
        if (artifacts_by_id.size() >= config.max_artifacts && !artifacts_by_id.empty()) {
            artifacts_by_id.erase(artifacts_by_id.begin());
        }
        artifacts_by_id[artifact->id] = std::move(artifact);
        return result;
    }

    void execute(std::shared_ptr<Run> run, std::shared_ptr<Artifact> artifact) {
        {
            std::lock_guard lock(run->mutex);
            run->status = "running";
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
                run->cancel->cancel();
            }
        });

        try {
            auto provider = std::make_shared<HarnessRuntimeProvider>(
                artifact->request, config.worker_executor);
            graph::NodeContext context;
            context.provider = std::move(provider);
            auto engine = graph::GraphEngine::compile(artifact->core, context);
            engine->set_worker_count(static_cast<std::size_t>(
                budgets.value("max_parallel_workers", 4)));

            graph::RunConfig run_config;
            run_config.thread_id = run->id;
            run_config.input = {{"task", artifact->request["task"]}};
            run_config.max_steps = budgets.value("max_steps", 40);
            run_config.cancel_token = run->cancel;
            auto graph_result = engine->run(run_config);

            std::lock_guard lock(run->mutex);
            if (graph_result.max_steps_exhausted()) {
                run->status = "max_steps_exhausted";
            } else {
                run->status = "completed";
                run->result = graph_result.channel_raw("final_result");
                run->result["execution_trace"] = graph_result.execution_trace;
            }
        } catch (const graph::CancelledException& error) {
            std::lock_guard lock(run->mutex);
            run->status = timed_out.load(std::memory_order_acquire)
                ? "timeout" : "cancelled";
            run->error = error.what();
        } catch (const std::exception& error) {
            std::lock_guard lock(run->mutex);
            run->status = "failed";
            run->error = error.what();
        } catch (...) {
            std::lock_guard lock(run->mutex);
            run->status = "failed";
            run->error = "unknown Harness execution failure";
        }

        {
            std::lock_guard lock(timer_mutex);
            done = true;
            timer_cv.notify_all();
        }
        timer.join();
    }

    HarnessServiceConfig config;
    std::uint64_t nonce;
    std::atomic<std::uint64_t> next_id{1};
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<Artifact>> artifacts_by_id;
    std::map<std::string, std::shared_ptr<Run>> runs;
    std::vector<std::thread> threads;
};

HarnessService::HarnessService(HarnessServiceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

HarnessService::~HarnessService() = default;

json HarnessService::schema() const {
    json executor_kinds = json::array({
        {{"kind", "builtin"}, {"availability", "binding-only-in-m2"}},
        {{"kind", "mcp"}, {"availability", "binding-only-in-m2"}},
        {{"kind", "a2a"}, {"availability", "binding-only-in-m2"}},
        {{"kind", "host_brokered"}, {"availability", "requires-neograph-resume-m4"}},
        {{"kind", "script"}, {"availability", "disabled"}},
    });
    const auto request = harness_request_schema();
    return {
        {"protocol_version", MCP_PROTOCOL_VERSION},
        {"service", "neograph-harness-m2"},
        {"presets", json::array({"fanout_judge"})},
        {"executor_kinds", std::move(executor_kinds)},
        {"request_schema", request},
        {"schemas", {
            {"request", request},
            {"worker", request["properties"]["workers"]["items"]},
            {"tool_catalog_entry", request["properties"]["tool_catalog"]["items"]},
            {"budgets", request["properties"]["budgets"]},
            {"worker_result", worker_result_schema()},
            {"run_snapshot", run_snapshot_schema()},
        }},
        {"node_palette", graph::NodeFactory::instance().export_schema()},
        {"capabilities", {
            {"strict_core", true},
            {"translation_validation", true},
            {"semantic_validation", true},
            {"binding_validation", true},
            {"bounded_worker_repair", true},
            {"durable_runs", false},
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
    if (arguments.contains("request")) {
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
    } else if (arguments.contains("artifact_id")
               && arguments["artifact_id"].is_string()) {
        artifact_id = arguments["artifact_id"].get<std::string>();
    } else {
        throw std::invalid_argument(
            "neograph_start requires artifact_id or request");
    }

    std::shared_ptr<Impl::Artifact> artifact;
    auto run = std::make_shared<Impl::Run>();
    {
        std::lock_guard lock(impl_->mutex);
        auto artifact_it = impl_->artifacts_by_id.find(artifact_id);
        if (artifact_it == impl_->artifacts_by_id.end()) {
            throw std::invalid_argument("unknown Harness artifact_id: " + artifact_id);
        }
        if (impl_->runs.size() >= impl_->config.max_runs) {
            throw std::runtime_error("Harness run capacity is exhausted");
        }
        artifact = artifact_it->second;
        run->id = impl_->id("run");
        run->artifact_id = artifact_id;
        impl_->runs[run->id] = run;
        try {
            impl_->threads.emplace_back([impl = impl_.get(), run, artifact] {
                impl->execute(run, artifact);
            });
        } catch (...) {
            impl_->runs.erase(run->id);
            throw;
        }
    }
    return {
        {"started", true},
        {"run_id", run->id},
        {"artifact_id", artifact_id},
        {"status", "queued"},
    };
}

json HarnessService::get(const std::string& run_id) const {
    std::shared_ptr<Impl::Run> run;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->runs.find(run_id);
        if (it == impl_->runs.end()) {
            throw std::invalid_argument("unknown Harness run_id: " + run_id);
        }
        run = it->second;
    }
    std::lock_guard lock(run->mutex);
    json snapshot = {
        {"run_id", run->id},
        {"artifact_id", run->artifact_id},
        {"status", run->status},
    };
    if (!run->result.is_null()) snapshot["result"] = run->result;
    if (!run->error.empty()) snapshot["error"] = run->error;
    return snapshot;
}

bool HarnessService::cancel(const std::string& run_id) {
    std::shared_ptr<Impl::Run> run;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->runs.find(run_id);
        if (it == impl_->runs.end()) return false;
        run = it->second;
    }
    {
        std::lock_guard lock(run->mutex);
        if (run->status != "queued" && run->status != "running") return false;
    }
    run->cancel->cancel();
    return true;
}

void HarnessService::register_tools(MCPServer& server) {
    server.register_tool(tool_definition(
        "neograph_schema", "NeoGraph Harness schema",
        "Return build-specific Harness request schemas, presets, executor kinds, and node palette.",
        json::parse(R"JSON({"type":"object","properties":{},"additionalProperties":false})JSON"),
        json::parse(R"JSON({"type":"object","required":["service","request_schema","node_palette"],"properties":{"service":{"type":"string"},"request_schema":{"type":"object"},"node_palette":{"type":"object"}},"additionalProperties":true})JSON"), true),
        [this](const json&, const auto&) {
            auto value = schema();
            return mcp_result(std::move(value), "NeoGraph Harness M2 schema");
        });

    server.register_tool(tool_definition(
        "neograph_compile", "Compile Harness",
        "Elaborate, strict-compile, translation-validate, semantic-validate, and binding-validate a Harness request.",
        harness_request_schema(), compile_output_schema()),
        [this](const json& arguments, const auto&) {
            auto value = compile(arguments);
            const auto text = value.value("ok", false)
                ? "Harness compiled: " + value.value("artifact_id", "")
                : "Harness rejected by compiler diagnostics";
            return mcp_result(std::move(value), text);
        });

    server.register_tool(tool_definition(
        "neograph_start", "Start Harness",
        "Start a retained artifact or compile-and-start an inline Harness request.",
        json::parse(R"JSON({"type":"object","properties":{"artifact_id":{"type":"string"},"request":{"type":"object"}},"additionalProperties":false})JSON"),
        json::parse(R"JSON({"type":"object","required":["started","status"],"properties":{"started":{"type":"boolean"},"run_id":{"type":"string"},"artifact_id":{"type":"string"},"status":{"type":"string"},"diagnostics":{"type":"array"},"artifacts":{"type":"object"}},"additionalProperties":true})JSON")),
        [this](const json& arguments, const auto&) {
            auto value = start(arguments);
            const auto text = value.value("started", false)
                ? "Harness started: " + value.value("run_id", "")
                : "Harness did not start";
            return mcp_result(std::move(value), text);
        });

    server.register_tool(tool_definition(
        "neograph_get", "Get Harness run",
        "Read compact run status, result, and failure classification.",
        json::parse(R"JSON({"type":"object","required":["run_id"],"properties":{"run_id":{"type":"string"}},"additionalProperties":false})JSON"),
        run_snapshot_schema(), true),
        [this](const json& arguments, const auto&) {
            auto value = get(arguments["run_id"].get<std::string>());
            const auto text = "Harness run " + value.value("run_id", "")
                + ": " + value.value("status", "unknown");
            return mcp_result(std::move(value), text);
        });

    server.register_tool(tool_definition(
        "neograph_cancel", "Cancel Harness run",
        "Cooperatively cancel a queued or running Harness run.",
        json::parse(R"JSON({"type":"object","required":["run_id"],"properties":{"run_id":{"type":"string"}},"additionalProperties":false})JSON"),
        json::parse(R"JSON({"type":"object","required":["run_id","cancelled"],"properties":{"run_id":{"type":"string"},"cancelled":{"type":"boolean"}},"additionalProperties":false})JSON")),
        [this](const json& arguments, const auto&) {
            const auto run_id = arguments["run_id"].get<std::string>();
            const bool accepted = cancel(run_id);
            json value = {{"run_id", run_id}, {"cancelled", accepted}};
            return mcp_result(std::move(value), accepted
                ? "Harness cancellation requested" : "Harness run is unknown or terminal");
        });
}

} // namespace neograph::mcp
