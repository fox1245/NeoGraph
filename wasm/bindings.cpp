#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/json.h>

#include <emscripten/bind.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using emscripten::val;
using neograph::json;
using namespace neograph::graph;

json run_result_json(const RunResult& result) {
    json out = json::object();
    out["output"] = result.output;
    out["interrupted"] = result.interrupted;
    out["interruptNode"] = result.interrupt_node;
    out["interruptValue"] = result.interrupt_value;
    out["checkpointId"] = result.checkpoint_id;
    out["executionTrace"] = json(result.execution_trace);
    out["maxStepsExhausted"] = result.max_steps_exhausted();
    out["usage"] = json{
        {"promptTokens", result.usage.prompt_tokens},
        {"completionTokens", result.usage.completion_tokens},
        {"totalTokens", result.usage.total_tokens},
    };
    return out;
}

ChannelWrite parse_write(const json& value) {
    if (!value.is_object()) {
        throw std::runtime_error("JavaScript node write must be an object");
    }

    const auto channel = value.value("channel", "");
    if (channel.empty() || !value.contains("value")) {
        throw std::runtime_error(
            "JavaScript node write requires non-empty 'channel' and 'value'");
    }

    ChannelWrite write{channel, value["value"]};
    const auto mode = value.value("mode", "reduce");
    if (mode == "overwrite") {
        write.mode = ChannelWrite::Mode::Overwrite;
    } else if (mode != "reduce") {
        throw std::runtime_error(
            "JavaScript node write mode must be 'reduce' or 'overwrite'");
    }
    return write;
}

NodeOutput parse_node_output(const std::string& encoded) {
    const auto value = json::parse(encoded);
    if (!value.is_object()) {
        throw std::runtime_error("JavaScript node callback must return a JSON object");
    }

    if (value.contains("interrupt")) {
        if (value.contains("writes") || value.contains("command")
            || value.contains("sends")) {
            throw std::runtime_error(
                "JavaScript node interrupt cannot be combined with writes, command, or sends");
        }
        const auto interrupt = value["interrupt"];
        if (!interrupt.is_object()) {
            throw std::runtime_error("JavaScript node interrupt must be an object");
        }
        throw NodeInterrupt(
            interrupt.value("reason", "JavaScript node requested an interrupt"),
            interrupt.contains("value") ? interrupt["value"] : json());
    }

    NodeOutput output;
    if (value.contains("writes")) {
        if (!value["writes"].is_array()) {
            throw std::runtime_error("JavaScript node 'writes' must be an array");
        }
        for (const auto& write : value["writes"]) {
            output.writes.push_back(parse_write(write));
        }
    }

    if (value.contains("command")) {
        const auto command_value = value["command"];
        if (!command_value.is_object()) {
            throw std::runtime_error("JavaScript node 'command' must be an object");
        }
        Command command;
        command.goto_node = command_value.value("goto", "");
        if (command.goto_node.empty()) {
            throw std::runtime_error("JavaScript node command requires non-empty 'goto'");
        }
        if (command_value.contains("updates")) {
            if (!command_value["updates"].is_array()) {
                throw std::runtime_error("JavaScript node command updates must be an array");
            }
            for (const auto& update : command_value["updates"]) {
                command.updates.push_back(parse_write(update));
            }
        }
        output.command = std::move(command);
    }

    if (value.contains("sends")) {
        if (!value["sends"].is_array()) {
            throw std::runtime_error("JavaScript node 'sends' must be an array");
        }
        for (const auto& send_value : value["sends"]) {
            if (!send_value.is_object()) {
                throw std::runtime_error("JavaScript node send must be an object");
            }
            const auto target = send_value.value("target", "");
            if (target.empty()) {
                throw std::runtime_error("JavaScript node send requires non-empty 'target'");
            }
            output.sends.push_back(Send{
                target,
                send_value.contains("input") ? send_value["input"] : json::object(),
            });
        }
    }

    return output;
}

class JavaScriptNode final : public GraphNode {
public:
    JavaScriptNode(std::string name, json config, val callback)
        : name_(std::move(name)), config_(std::move(config)),
          callback_(std::move(callback)) {}

    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        json context = json{
            {"threadId", in.ctx.thread_id},
            {"step", in.ctx.step},
        };
        if (in.ctx.resume_value) {
            context["resumeValue"] = *in.ctx.resume_value;
        }

        const auto request = json{
            {"name", name_},
            {"config", config_},
            {"state", in.state.serialize()},
            {"context", context},
        };
        const auto encoded = callback_
            .call<val>("call", val::undefined(), request.dump())
            .as<std::string>();
        co_return parse_node_output(encoded);
    }

private:
    std::string name_;
    json config_;
    val callback_;
};

std::map<std::string, val>& javascript_callbacks() {
    static std::map<std::string, val> callbacks;
    return callbacks;
}

void register_node_type(const std::string& type, val callback) {
    if (type.empty()) {
        throw std::runtime_error("JavaScript node type must not be empty");
    }
    if (callback.typeOf().as<std::string>() != "function") {
        throw std::runtime_error("JavaScript node callback must be a function");
    }

    javascript_callbacks().insert_or_assign(type, std::move(callback));
    NodeFactory::instance().register_type(
        type,
        [type](
            const std::string& name, const json& config, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            const auto callback = javascript_callbacks().find(type);
            if (callback == javascript_callbacks().end()) {
                throw std::runtime_error(
                    "JavaScript node type '" + type + "' is no longer registered");
            }
            return std::make_unique<JavaScriptNode>(name, config, callback->second);
        });
}

void unregister_node_type(const std::string& type) {
    javascript_callbacks().erase(type);
}

std::string registered_node_types() {
    std::vector<std::string> types;
    types.reserve(javascript_callbacks().size());
    for (const auto& [type, callback] : javascript_callbacks()) {
        (void)callback;
        types.push_back(type);
    }
    return json(types).dump();
}

std::string neograph_version() {
    return NEOGRAPH_VERSION_STR;
}

class WasmGraph {
public:
    explicit WasmGraph(const std::string& definition_json)
        : checkpoints_(std::make_shared<InMemoryCheckpointStore>()),
          engine_(GraphEngine::compile(
              json::parse(definition_json), NodeContext(), checkpoints_)) {}

    std::string run(const std::string& config_json) {
        const auto value = json::parse(config_json);
        if (!value.is_object()) {
            throw std::runtime_error("run config must be a JSON object");
        }

        RunConfig config;
        config.thread_id = value.value("threadId", "");
        if (config.thread_id.empty()) {
            throw std::runtime_error("run config requires non-empty 'threadId'");
        }
        config.input = value.contains("input") ? value["input"] : json::object();
        config.max_steps = value.value("maxSteps", 50);
        config.resume_if_exists = value.value("resumeIfExists", false);
        return run_result_json(engine_->run(config)).dump();
    }

    std::string resume(const std::string& thread_id,
                       const std::string& resume_value_json) {
        if (thread_id.empty()) {
            throw std::runtime_error("resume requires a non-empty thread ID");
        }
        const auto resume_value = json::parse(resume_value_json);
        if (resume_value.is_null()) {
            throw std::runtime_error(
                "resume value must not be null; NeoGraph reserves null for no response");
        }
        return run_result_json(engine_->resume(thread_id, resume_value)).dump();
    }

private:
    std::shared_ptr<InMemoryCheckpointStore> checkpoints_;
    std::unique_ptr<GraphEngine> engine_;
};

} // namespace

EMSCRIPTEN_BINDINGS(neograph_wasm) {
    emscripten::function("registerNodeType", &register_node_type);
    emscripten::function("unregisterNodeType", &unregister_node_type);
    emscripten::function("registeredNodeTypes", &registered_node_types);
    emscripten::function("version", &neograph_version);

    emscripten::class_<WasmGraph>("Graph")
        .constructor<const std::string&>()
        .function("run", &WasmGraph::run)
        .function("resume", &WasmGraph::resume);
}
