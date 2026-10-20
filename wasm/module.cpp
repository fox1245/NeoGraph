// The JavaScript module (#535): a graph engine whose model calls go through the host's `fetch`, for Node, a browser
// or a Worker. wasm/neograph.mjs is its JavaScript side; this file is the C API it calls.
//
//   ng_compile(graph, provider)       -> handle (0 on failure; ng_error() says why)
//   ng_run(handle, run, input, stream)   starts a run and returns at once
//   ng_cancel(run), ng_release(handle)
//
// Results come back through Module.ngOnDone(run, json) and, for a streamed run, Module.ngOnEvent(run, json), always
// from a later turn of the host's event loop. Nothing here waits: the engine's coroutines run on an Asio io_context
// that async::HostLoop pumps from that event loop, and the SDK sends over fetch (sp_js_host) on the same executor.
// The API key comes in with the provider description at run time; it is not in the binary.
#include <neograph/async/host_loop.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>
#include <neograph/llm/schema_provider.h>

#include <descriptor/descriptor.h>
#include <runtime/js_host.h>

#include <emscripten.h>

#include <chrono>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace {

using neograph::json;
namespace graph = neograph::graph;

EM_JS(void, ng_js_event, (int run, const char* text), { Module.ngOnEvent(run, UTF8ToString(text)); });
EM_JS(void, ng_js_done, (int run, const char* text), { Module.ngOnDone(run, UTF8ToString(text)); });
EM_JS_DEPS(ng_js_deps, "$UTF8ToString");

// The loop that pumps every run of this module instance, on an executor of its own: a Client shuts down the executor
// it runs on when it is destroyed, so the loop cannot share one.
neograph::async::HostLoop& loop() {
    static neograph::async::HostLoop instance{sp::runtime::js::make_executor()};
    return instance;
}

struct Compiled {
    std::unique_ptr<graph::GraphEngine> engine;
    std::shared_ptr<neograph::Provider> provider;
};
struct Run {
    std::shared_ptr<graph::CancelToken> cancel;
};

std::map<int, std::shared_ptr<Compiled>> compiled;
std::map<int, Run> runs;
int last_handle = 0;
std::string last_error;

const char* const error_kinds[] = {
    "invalid_config", "invalid_request", "unsupported", "transport", "protocol_corrupt", "truncated",
    "remote_failure", "cancelled", "deadline_exceeded", "resource_limit", "misuse", "replay_ineligible",
    "authentication", "permission", "not_found", "rate_limited", "quota_exhausted", "limit_unknown", "overloaded"};

const char* kind_name(sp::ErrorKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return index < sizeof(error_kinds) / sizeof(*error_kinds) ? error_kinds[index] : "unknown";
}

const char* status_name(graph::RunStatus status) {
    switch (status) {
    case graph::RunStatus::Completed: return "completed";
    case graph::RunStatus::Interrupted: return "interrupted";
    case graph::RunStatus::SafePoint: return "safe_point";
    case graph::RunStatus::StepLimit: return "step_limit";
    case graph::RunStatus::FailureExit: return "failure_exit";
    case graph::RunStatus::BarrierStalled: return "barrier_stalled";
    }
    return "unknown";
}

const char* event_name(graph::GraphEvent::Type type) {
    switch (type) {
    case graph::GraphEvent::Type::NODE_START: return "node_start";
    case graph::GraphEvent::Type::NODE_END: return "node_end";
    case graph::GraphEvent::Type::LLM_TOKEN: return "llm_token";
    case graph::GraphEvent::Type::CHANNEL_WRITE: return "channel_write";
    case graph::GraphEvent::Type::INTERRUPT: return "interrupt";
    case graph::GraphEvent::Type::ERROR: return "error";
    }
    return "unknown";
}

sp::descriptor::ValidatedDescriptor load_descriptor(const json& description) {
    auto loaded = sp::descriptor::load(description.dump());
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument("provider.descriptor rejected at " + error->pointer + ": " + error->message);
    return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}

// What a failed run says. A node's failure wraps its cause (NodeExecutionError); the typed detail is the cause's.
void describe(const std::exception_ptr& error, json& detail, int depth) {
    try {
        std::rethrow_exception(error);
    } catch (const graph::NodeExecutionError& failure) {
        detail["node"] = failure.node_name();
        detail["message"] = std::string(failure.what());
        if (failure.cause() && depth < 8) describe(failure.cause(), detail, depth + 1);
    } catch (const neograph::ProviderFailure& failure) {
        const auto& cause = std::get<sp::Failure>(*failure.outcome()).error;
        detail["type"] = "provider";
        if (!detail.contains("message")) detail["message"] = std::string(failure.what());
        detail["kind"] = kind_name(cause.kind);
        detail["reason"] = cause.reason;
        detail["http_status"] = cause.http_status;
    } catch (const graph::CancelledException& failure) {
        detail["type"] = "cancelled";
        if (!detail.contains("message")) detail["message"] = std::string(failure.what());
    } catch (const std::exception& failure) {
        detail["type"] = "error";
        if (!detail.contains("message")) detail["message"] = std::string(failure.what());
    } catch (...) {
        detail["type"] = "error";
        if (!detail.contains("message")) detail["message"] = "unknown failure";
    }
}

json failure_json(const std::exception_ptr& error) {
    json out = json::object();
    out["ok"] = false;
    json detail = json::object();
    describe(error, detail, 0);
    out["error"] = std::move(detail);
    return out;
}

std::string assistant_text(const graph::RunResult& result) {
    for (auto it = result.native_messages.rbegin(); it != result.native_messages.rend(); ++it) {
        if (it->role != sp::Role::Assistant) continue;
        std::string text;
        for (const auto& part : it->parts)
            if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
        if (!text.empty()) return text;
    }
    return {};
}

json result_json(const graph::RunResult& result) {
    json out = json::object();
    out["ok"] = true;
    json body = json::object();
    body["status"] = status_name(result.status());
    body["text"] = assistant_text(result);
    body["output"] = result.output;
    json trace = json::array();
    for (const auto& step : result.execution_trace) trace.push_back(step);
    body["trace"] = std::move(trace);
    json usage = json::object();
    if (const auto value = result.usage.input_total_value()) usage["input_tokens"] = *value;
    if (const auto value = result.usage.output_total_value()) usage["output_tokens"] = *value;
    if (const auto value = result.usage.total_tokens_value()) usage["total_tokens"] = *value;
    body["usage"] = std::move(usage);
    if (result.interrupted) {
        body["interrupt"] = json{{"node", result.interrupt_node}, {"value", result.interrupt_value}};
    }
    out["result"] = std::move(body);
    return out;
}

asio::awaitable<graph::RunResult> drive(std::shared_ptr<Compiled> graph_handle, graph::RunConfig config,
                                        graph::RunMetadata metadata, int run, bool stream) {
    // The shared pointer keeps the engine alive for the run, whatever the host does with its handle.
    if (!stream) co_return co_await graph_handle->engine->run_async(std::move(config), std::move(metadata));
    graph::GraphStreamCallback emit = [run](const graph::GraphEvent& event) {
        json item = json::object();
        item["type"] = event_name(event.type);
        item["node"] = event.node_name;
        item["data"] = event.data;
        ng_js_event(run, item.dump().c_str());
    };
    co_return co_await graph_handle->engine->run_stream_async(std::move(config), std::move(emit), std::move(metadata));
}

}  // namespace

// The last error message of ng_compile or ng_run. Valid until the next call.
extern "C" EMSCRIPTEN_KEEPALIVE const char* ng_error() { return last_error.c_str(); }

extern "C" EMSCRIPTEN_KEEPALIVE void* ng_alloc(int size) { return ::operator new(static_cast<std::size_t>(size)); }
extern "C" EMSCRIPTEN_KEEPALIVE void ng_free(void* pointer) { ::operator delete(pointer); }

// `graph_json` is a graph definition. `provider_json`:
//   { "descriptor": {...},            an SDK descriptor (the family, the origin and the routes)
//     "api_key": "...",               sent as the descriptor's family sends a key
//     "model": "...",                 the model of the graph's llm_call nodes
//     "instructions": "...",          optional system instructions
//     "max_output_tokens": N, "temperature": X }   optional
// Returns a handle, or 0 with ng_error() set.
extern "C" EMSCRIPTEN_KEEPALIVE int ng_compile(const char* graph_json, const char* provider_json) {
    try {
        const auto definition = json::parse(graph_json);
        const auto provider = json::parse(provider_json);
        auto handle = std::make_shared<Compiled>();
        sp::runtime::Options options = sp::runtime::Options::with_api_key(provider.value("api_key", ""));
        // The SDK's host seam for this provider: its executor, and the transport that sends over fetch on it.
        auto executor = sp::runtime::js::make_executor();
        auto transport = sp::runtime::js::make_fetch_transport(executor);
        handle->provider = neograph::llm::SchemaProvider::create(
            load_descriptor(provider["descriptor"]), std::move(options), std::move(executor), std::move(transport));
        graph::NodeContext context;
        context.provider = handle->provider;
        context.model = provider.value("model", "");
        context.instructions = provider.value("instructions", "");
        if (provider.contains("max_output_tokens"))
            context.provider_controls.max_output_tokens = provider["max_output_tokens"].get<unsigned long long>();
        if (provider.contains("temperature"))
            context.provider_controls.temperature = provider["temperature"].get<double>();
        handle->engine = graph::GraphEngine::compile(definition, context);
        compiled.emplace(++last_handle, std::move(handle));
        return last_handle;
    } catch (const std::exception& error) {
        last_error = error.what();
    } catch (const sp::descriptor::ConfigError& error) {
        last_error = error.pointer + ": " + error.message;
    } catch (...) {
        last_error = "unknown failure";
    }
    return 0;
}

// Starts run `run` of graph `handle`. `input_json` holds the initial channel writes; a `messages` array of
// {role, content} is the conversation. `timeout_ms` bounds the whole run (0: none). A streamed run reports each graph
// event through Module.ngOnEvent. Returns 0, or -1 with ng_error() set when the run could not start (it reports
// nothing then); otherwise the outcome arrives through Module.ngOnDone.
extern "C" EMSCRIPTEN_KEEPALIVE int ng_run(int handle, int run, const char* input_json, int stream, int timeout_ms) {
    try {
        const auto found = compiled.find(handle);
        if (found == compiled.end()) throw std::invalid_argument("no such graph");
        if (runs.count(run)) throw std::invalid_argument("run id in use");
        auto input = json::parse(input_json);
        graph::RunConfig config;
        config.thread_id = "run-" + std::to_string(run);
        config.stream_mode = graph::StreamMode::EVENTS | graph::StreamMode::TOKENS;
        config.cancel_token = std::make_shared<graph::CancelToken>();
        if (input.contains("messages")) {
            std::vector<sp::Message> messages;
            for (const auto& item : input["messages"])
                messages.push_back(neograph::portable_message(
                    neograph::ChatMessage{item.value("role", "user"), item.value("content", "")}));
            config.provider_messages = std::move(messages);
            json rest = json::object();
            for (const auto& [key, value] : input.items())
                if (key != "messages") rest[key] = value;
            input = std::move(rest);
        }
        config.input = std::move(input);
        graph::RunMetadata metadata;
        if (timeout_ms > 0) metadata.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        runs.emplace(run, Run{config.cancel_token});
        loop().spawn(drive(found->second, std::move(config), std::move(metadata), run, stream != 0),
                          [run](std::exception_ptr error, graph::RunResult result) {
                              runs.erase(run);
                              ng_js_done(run, (error ? failure_json(error) : result_json(result)).dump().c_str());
                          });
        return 0;
    } catch (const std::exception& error) {
        last_error = error.what();
    } catch (...) {
        last_error = "unknown failure";
    }
    return -1;
}

extern "C" EMSCRIPTEN_KEEPALIVE void ng_cancel(int run) {
    const auto found = runs.find(run);
    if (found == runs.end()) return;
    found->second.cancel->cancel();
    loop().wake();
}

// A run in flight keeps the graph alive until it finishes.
extern "C" EMSCRIPTEN_KEEPALIVE void ng_release(int handle) { compiled.erase(handle); }
