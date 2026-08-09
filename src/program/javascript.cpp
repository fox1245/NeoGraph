#include "javascript.h"

#include <neograph/program/compiler.h>

#include "canonical_json.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
#include <quickjs.h>
#endif

namespace neograph::program::detail {

JavaScriptCompileError::JavaScriptCompileError(std::string code, std::string message, json witness)
    : std::runtime_error(message), code_(std::move(code)), witness_(std::move(witness)) {}

const std::string& JavaScriptCompileError::code() const noexcept {
    return code_;
}

const json& JavaScriptCompileError::witness() const noexcept {
    return witness_;
}

#if defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
namespace {

constexpr std::size_t kMaxGeneratedDocumentBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxDiagnosticTextBytes    = 4096u;
constexpr std::size_t kMaxGraphValueDepth        = 64u;
constexpr std::size_t kMaxGraphValueElements     = 1'000'000u;

std::string bounded_utf8(std::string_view text) {
    if (text.size() <= kMaxDiagnosticTextBytes) return std::string(text);

    std::size_t offset = 0;
    while (offset < text.size() && offset < kMaxDiagnosticTextBytes) {
        const auto  first = static_cast<unsigned char>(text[offset]);
        std::size_t width = 1;
        if ((first & 0xe0u) == 0xc0u)
            width = 2;
        else if ((first & 0xf0u) == 0xe0u)
            width = 3;
        else if ((first & 0xf8u) == 0xf0u)
            width = 4;
        if (offset + width > kMaxDiagnosticTextBytes) break;
        offset += width;
    }
    return std::string(text.substr(0, offset)) + "…";
}

std::string exception_text(JSContext* context) {
    JSValue           exception = JS_GetException(context);
    const char*       text      = JS_ToCString(context, exception);
    const std::string result    = text ? bounded_utf8(text) : "QuickJS exception";
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, exception);
    return result;
}

std::string value_text(JSContext* context, JSValueConst value) {
    const char*       text   = JS_ToCString(context, value);
    const std::string result = text ? bounded_utf8(text) : exception_text(context);
    if (text) JS_FreeCString(context, text);
    return result;
}

bool looks_like_resource_exhaustion(std::string_view message) {
    std::string lower;
    lower.reserve(message.size());
    for (const unsigned char value : message) {
        if (value >= 'A' && value <= 'Z')
            lower.push_back(static_cast<char>(value - 'A' + 'a'));
        else
            lower.push_back(static_cast<char>(value));
    }
    return lower.find("out of memory") != std::string::npos ||
           lower.find("stack overflow") != std::string::npos;
}

struct InterruptBudget {
    std::uint64_t polls     = 0;
    std::uint64_t limit     = 0;
    bool          exhausted = false;
};

int interrupt_after_budget(JSRuntime*, void* opaque) {
    auto& budget = *static_cast<InterruptBudget*>(opaque);
    if (budget.polls >= budget.limit) {
        budget.exhausted = true;
        return 1;
    }
    ++budget.polls;
    return 0;
}

struct DefinitionCapture {
    std::string failure_code;
    std::string failure_message;
};

void record_failure(DefinitionCapture* capture, std::string code, std::string message) {
    if (!capture || !capture->failure_code.empty()) return;
    capture->failure_code    = std::move(code);
    capture->failure_message = std::move(message);
}

JSValue graph_error(JSContext* context, std::string code, std::string message) {
    record_failure(static_cast<DefinitionCapture*>(JS_GetContextOpaque(context)), std::move(code),
                   message);
    return JS_ThrowTypeError(context, "%s", message.c_str());
}

JSValue graph_internal_error(JSContext* context, std::string message) {
    record_failure(static_cast<DefinitionCapture*>(JS_GetContextOpaque(context)), "P_JS_RUNTIME",
                   message);
    return JS_ThrowInternalError(context, "%s", message.c_str());
}

class QuickJsScope final {
public:
    explicit QuickJsScope(const JavaScriptCompileLimits& limits) {
        runtime_ = JS_NewRuntime();
        if (!runtime_) {
            throw JavaScriptCompileError("P_JS_RUNTIME", "QuickJS runtime initialization failed");
        }
        JS_SetMemoryLimit(runtime_, limits.memory_limit_bytes);
        JS_SetMaxStackSize(runtime_, limits.max_stack_bytes);
        context_ = JS_NewContext(runtime_);
        if (!context_) {
            JS_FreeRuntime(runtime_);
            runtime_ = nullptr;
            throw JavaScriptCompileError("P_JS_RUNTIME", "QuickJS context initialization failed");
        }
    }

    QuickJsScope(const QuickJsScope&)            = delete;
    QuickJsScope& operator=(const QuickJsScope&) = delete;

    ~QuickJsScope() {
        if (context_) JS_FreeContext(context_);
        if (runtime_) JS_FreeRuntime(runtime_);
    }

    JSRuntime* runtime() const noexcept { return runtime_; }
    JSContext* context() const noexcept { return context_; }

private:
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
};

struct GraphBuilder {
    DefinitionCapture* capture = nullptr;
    JSContext*         context = nullptr;
    bool               sealed  = false;
    json               definition;
};

JSClassID        graph_builder_class_id = JS_INVALID_CLASS_ID;
std::once_flag   graph_builder_class_once;

void graph_builder_finalizer(JSRuntime*, JSValue value) {
    delete static_cast<GraphBuilder*>(JS_GetOpaque(value, graph_builder_class_id));
}

void ensure_graph_builder_class(JSRuntime* runtime) {
    std::call_once(graph_builder_class_once,
                   [] { JS_NewClassID(&graph_builder_class_id); });
    if (JS_IsRegisteredClass(runtime, graph_builder_class_id)) return;
    static const JSClassDef definition{
        "NeoGraphGraphBuilder",
        graph_builder_finalizer,
        nullptr,
        nullptr,
        nullptr,
    };
    if (JS_NewClass(runtime, graph_builder_class_id, &definition) < 0) {
        throw JavaScriptCompileError("P_JS_RUNTIME",
                                     "QuickJS could not register the NeoGraph graph builder class");
    }
}

GraphBuilder* require_open_builder(JSContext* context, JSValueConst this_value) {
    auto* builder = static_cast<GraphBuilder*>(JS_GetOpaque(this_value, graph_builder_class_id));
    if (!builder) {
        graph_error(context, "P_JS_GRAPH_OWNER",
                    "NeoGraph graph-builder methods require an owning graph builder");
        return nullptr;
    }
    if (builder->context != context || builder->capture != JS_GetContextOpaque(context)) {
        graph_error(context, "P_JS_GRAPH_OWNER",
                    "NeoGraph graph builder belongs to a different definition context");
        return nullptr;
    }
    if (builder->sealed) {
        graph_error(context, "P_JS_GRAPH_SEALED", "NeoGraph graph builder is already sealed");
        return nullptr;
    }
    return builder;
}

bool read_string(JSContext* context,
                 JSValueConst value,
                 std::string&  output,
                 std::string_view argument_name) {
    if (!JS_IsString(value)) {
        graph_error(context, "P_JS_GRAPH_ARGUMENT",
                    "NeoGraph graph " + std::string(argument_name) + " must be a string");
        return false;
    }
    size_t      size = 0;
    const char* data = JS_ToCStringLen(context, &size, value);
    if (!data) return false;
    output.assign(data, size);
    JS_FreeCString(context, data);
    return true;
}

struct JsonConversion {
    std::size_t elements = 0;
    std::string error;
};

bool consume_json_element(JsonConversion& state) {
    if (state.elements >= kMaxGraphValueElements) {
        state.error = "graph configuration exceeds the element limit";
        return false;
    }
    ++state.elements;
    return true;
}

bool get_standard_prototype(JSContext* context,
                            const char* constructor_name,
                            JSValue&    output,
                            std::string& error) {
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) {
        error = exception_text(context);
        return false;
    }
    JSValue constructor = JS_GetPropertyStr(context, global, constructor_name);
    JS_FreeValue(context, global);
    if (JS_IsException(constructor)) {
        error = exception_text(context);
        return false;
    }
    output = JS_GetPropertyStr(context, constructor, "prototype");
    JS_FreeValue(context, constructor);
    if (JS_IsException(output)) {
        error = exception_text(context);
        return false;
    }
    return true;
}

bool is_plain_object(JSContext* context, JSValueConst value, std::string& error) {
    JSValue prototype = JS_GetPrototype(context, value);
    if (JS_IsException(prototype)) {
        error = exception_text(context);
        return false;
    }
    if (JS_IsNull(prototype)) {
        JS_FreeValue(context, prototype);
        return true;
    }
    JSValue object_prototype = JS_UNDEFINED;
    if (!get_standard_prototype(context, "Object", object_prototype, error)) {
        JS_FreeValue(context, prototype);
        return false;
    }
    const bool matches = JS_StrictEq(context, prototype, object_prototype);
    JS_FreeValue(context, object_prototype);
    JS_FreeValue(context, prototype);
    if (!matches) error = "graph configuration objects must be plain objects";
    return matches;
}

bool is_plain_array(JSContext* context, JSValueConst value, std::string& error) {
    JSValue prototype = JS_GetPrototype(context, value);
    if (JS_IsException(prototype)) {
        error = exception_text(context);
        return false;
    }
    JSValue array_prototype = JS_UNDEFINED;
    if (!get_standard_prototype(context, "Array", array_prototype, error)) {
        JS_FreeValue(context, prototype);
        return false;
    }
    const bool matches = JS_StrictEq(context, prototype, array_prototype);
    JS_FreeValue(context, array_prototype);
    JS_FreeValue(context, prototype);
    if (!matches) error = "graph configuration arrays must be ordinary arrays";
    return matches;
}

bool js_to_json(JSContext* context,
                JSValueConst value,
                json&        output,
                JsonConversion& state,
                std::size_t depth) {
    if (depth > kMaxGraphValueDepth) {
        state.error = "graph configuration exceeds the nesting-depth limit";
        return false;
    }
    if (JS_IsNull(value)) {
        output = nullptr;
        return true;
    }
    if (JS_IsBool(value)) {
        const int boolean = JS_ToBool(context, value);
        if (boolean < 0) {
            state.error = exception_text(context);
            return false;
        }
        output = boolean != 0;
        return true;
    }
    if (JS_IsString(value)) {
        size_t      size = 0;
        const char* data = JS_ToCStringLen(context, &size, value);
        if (!data) {
            state.error = exception_text(context);
            return false;
        }
        output = std::string(data, size);
        JS_FreeCString(context, data);
        return true;
    }
    if (JS_IsNumber(value)) {
        double number = 0.0;
        if (JS_ToFloat64(context, &number, value) < 0) {
            state.error = exception_text(context);
            return false;
        }
        if (!std::isfinite(number)) {
            state.error = "graph configuration numbers must be finite";
            return false;
        }
        if (std::trunc(number) == number) {
            if (number >= 0.0 &&
                number <= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
                output = static_cast<std::uint64_t>(number);
                return true;
            }
            if (number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                output = static_cast<std::int64_t>(number);
                return true;
            }
        }
        output = number;
        return true;
    }
    if (!JS_IsObject(value)) {
        state.error = "graph configuration contains an unsupported JavaScript value";
        return false;
    }
    if (JS_IsFunction(context, value)) {
        state.error = "graph configuration cannot contain functions";
        return false;
    }
    if (JS_IsArray(context, value)) {
        if (!is_plain_array(context, value, state.error)) return false;
        JSValue length_value = JS_GetPropertyStr(context, value, "length");
        if (JS_IsException(length_value)) {
            state.error = exception_text(context);
            return false;
        }
        std::uint64_t length = 0;
        const int     status = JS_ToIndex(context, &length, length_value);
        JS_FreeValue(context, length_value);
        if (status < 0) {
            state.error = exception_text(context);
            return false;
        }
        if (length > kMaxGraphValueElements || length > std::numeric_limits<std::uint32_t>::max()) {
            state.error = "graph configuration array exceeds the element limit";
            return false;
        }
        output = json::array();
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(length); ++index) {
            if (!consume_json_element(state)) return false;
            JSValue element = JS_GetPropertyUint32(context, value, index);
            if (JS_IsException(element)) {
                state.error = exception_text(context);
                return false;
            }
            json converted;
            const bool converted_ok = js_to_json(context, element, converted, state, depth + 1);
            JS_FreeValue(context, element);
            if (!converted_ok) return false;
            output.push_back(std::move(converted));
        }
        return true;
    }
    if (!is_plain_object(context, value, state.error)) return false;

    JSPropertyEnum* symbols = nullptr;
    std::uint32_t   symbol_count = 0;
    if (JS_GetOwnPropertyNames(context, &symbols, &symbol_count, value,
                               JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY) < 0) {
        state.error = exception_text(context);
        return false;
    }
    JS_FreePropertyEnum(context, symbols, symbol_count);
    if (symbol_count != 0) {
        state.error = "graph configuration cannot contain symbol properties";
        return false;
    }

    JSPropertyEnum* properties = nullptr;
    std::uint32_t   property_count = 0;
    if (JS_GetOwnPropertyNames(context, &properties, &property_count, value,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        state.error = exception_text(context);
        return false;
    }
    output = json::object();
    for (std::uint32_t index = 0; index < property_count; ++index) {
        if (!consume_json_element(state)) {
            JS_FreePropertyEnum(context, properties, property_count);
            return false;
        }
        const char* key = JS_AtomToCString(context, properties[index].atom);
        if (!key) {
            JS_FreePropertyEnum(context, properties, property_count);
            state.error = exception_text(context);
            return false;
        }
        const std::string name(key);
        JS_FreeCString(context, key);
        JSValue property = JS_GetProperty(context, value, properties[index].atom);
        if (JS_IsException(property)) {
            JS_FreePropertyEnum(context, properties, property_count);
            state.error = exception_text(context);
            return false;
        }
        json converted;
        const bool converted_ok = js_to_json(context, property, converted, state, depth + 1);
        JS_FreeValue(context, property);
        if (!converted_ok) {
            JS_FreePropertyEnum(context, properties, property_count);
            return false;
        }
        output[name] = std::move(converted);
    }
    JS_FreePropertyEnum(context, properties, property_count);
    return true;
}

bool read_json(JSContext* context, JSValueConst value, json& output, std::string_view argument_name) {
    JsonConversion conversion;
    if (js_to_json(context, value, output, conversion, 0)) return true;
    if (conversion.error.empty()) return false;
    graph_error(context, "P_JS_GRAPH_VALUE",
                "NeoGraph graph " + std::string(argument_name) + " is not canonical JSON data: " +
                    conversion.error);
    return false;
}

bool read_string_array(JSContext* context,
                       JSValueConst value,
                       json&        output,
                       std::string_view argument_name) {
    if (!read_json(context, value, output, argument_name)) return false;
    if (!output.is_array()) {
        graph_error(context, "P_JS_GRAPH_ARGUMENT",
                    "NeoGraph graph " + std::string(argument_name) + " must be an array of strings");
        return false;
    }
    for (const auto& element : output) {
        if (!element.is_string()) {
            graph_error(context, "P_JS_GRAPH_ARGUMENT",
                        "NeoGraph graph " + std::string(argument_name) +
                            " must be an array of strings");
            return false;
        }
    }
    return true;
}

JSValue return_builder(JSContext* context, JSValueConst this_value) {
    return JS_DupValue(context, this_value);
}

bool require_arity(JSContext* context, int actual, int expected, std::string_view operation) {
    if (actual == expected) return true;
    graph_error(context, "P_JS_GRAPH_ARGUMENT",
                "NeoGraph graph " + std::string(operation) + " expects " +
                    std::to_string(expected) + " argument(s)");
    return false;
}

JSValue graph_node_impl(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 2, "node")) return JS_EXCEPTION;
    std::string name;
    json        config;
    if (!read_string(context, argv[0], name, "node name") ||
        !read_json(context, argv[1], config, "node configuration")) {
        return JS_EXCEPTION;
    }
    if (!config.is_object()) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph node configuration must be an object");
    }
    auto nodes = builder->definition["nodes"];
    if (nodes.contains(name)) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph node names must be unique");
    }
    nodes[name] = std::move(config);
    return return_builder(context, this_value);
}

JSValue graph_node(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_node_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context, "NeoGraph graph node failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph node failed unexpectedly");
    }
}

JSValue graph_channel_impl(JSContext* context,
                           JSValueConst this_value,
                           int argc,
                           JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 2, "channel")) return JS_EXCEPTION;
    std::string name;
    json        config;
    if (!read_string(context, argv[0], name, "channel name") ||
        !read_json(context, argv[1], config, "channel configuration")) {
        return JS_EXCEPTION;
    }
    if (!config.is_object()) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph channel configuration must be an object");
    }
    auto channels = builder->definition["channels"];
    if (channels.contains(name)) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph channel names must be unique");
    }
    channels[name] = std::move(config);
    return return_builder(context, this_value);
}

JSValue graph_channel(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_channel_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph channel failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph channel failed unexpectedly");
    }
}

void append_edge(GraphBuilder& builder, const std::string& from, const std::string& to) {
    builder.definition["edges"].push_back(json{{"from", from}, {"to", to}});
}

JSValue graph_edge_impl(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 2, "edge")) return JS_EXCEPTION;
    std::string from;
    std::string to;
    if (!read_string(context, argv[0], from, "edge source") ||
        !read_string(context, argv[1], to, "edge destination")) {
        return JS_EXCEPTION;
    }
    append_edge(*builder, from, to);
    return return_builder(context, this_value);
}

JSValue graph_edge(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_edge_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context, "NeoGraph graph edge failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph edge failed unexpectedly");
    }
}

JSValue graph_conditional_edge_impl(JSContext* context,
                                    JSValueConst this_value,
                                    int argc,
                                    JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 3, "conditionalEdge")) return JS_EXCEPTION;
    std::string from;
    std::string condition;
    json        routes;
    if (!read_string(context, argv[0], from, "conditional edge source") ||
        !read_string(context, argv[1], condition, "conditional edge condition") ||
        !read_json(context, argv[2], routes, "conditional edge routes")) {
        return JS_EXCEPTION;
    }
    if (!routes.is_object()) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph conditional edge routes must be an object");
    }
    builder->definition["conditional_edges"].push_back(
        json{{"from", from}, {"condition", condition}, {"routes", std::move(routes)}});
    return return_builder(context, this_value);
}

JSValue graph_conditional_edge(JSContext* context,
                               JSValueConst this_value,
                               int argc,
                               JSValueConst* argv) {
    try {
        return graph_conditional_edge_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph conditionalEdge failed: " +
                                        bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph conditionalEdge failed unexpectedly");
    }
}

JSValue graph_barrier_impl(JSContext* context,
                           JSValueConst this_value,
                           int argc,
                           JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 2, "barrier")) return JS_EXCEPTION;
    std::string node;
    json        wait_for;
    if (!read_string(context, argv[0], node, "barrier node") ||
        !read_string_array(context, argv[1], wait_for, "barrier wait_for")) {
        return JS_EXCEPTION;
    }
    auto nodes = builder->definition["nodes"];
    if (!nodes.contains(node)) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph barrier node must already be declared");
    }
    nodes[node]["barrier"] = json{{"wait_for", std::move(wait_for)}};
    return return_builder(context, this_value);
}

JSValue graph_barrier(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_barrier_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph barrier failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph barrier failed unexpectedly");
    }
}

bool collect_interrupt_nodes(JSContext* context,
                             int        argc,
                             JSValueConst* argv,
                             json&      output,
                             std::string_view operation) {
    if (argc == 0) {
        graph_error(context, "P_JS_GRAPH_ARGUMENT",
                    "NeoGraph graph " + std::string(operation) + " expects at least one node");
        return false;
    }
    if (argc == 1 && JS_IsArray(context, argv[0])) {
        return read_string_array(context, argv[0], output, operation);
    }
    output = json::array();
    for (int index = 0; index < argc; ++index) {
        std::string name;
        if (!read_string(context, argv[index], name, operation)) return false;
        output.push_back(std::move(name));
    }
    return true;
}

JSValue graph_interrupt_before_impl(JSContext* context,
                                    JSValueConst this_value,
                                    int argc,
                                    JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder) return JS_EXCEPTION;
    json nodes;
    if (!collect_interrupt_nodes(context, argc, argv, nodes, "interruptBefore")) return JS_EXCEPTION;
    auto interrupts = builder->definition["interrupt_before"];
    for (const auto& node : nodes)
        interrupts.push_back(node);
    return return_builder(context, this_value);
}

JSValue graph_interrupt_before(JSContext* context,
                               JSValueConst this_value,
                               int argc,
                               JSValueConst* argv) {
    try {
        return graph_interrupt_before_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph interruptBefore failed: " +
                                        bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph interruptBefore failed unexpectedly");
    }
}

JSValue graph_interrupt_after_impl(JSContext* context,
                                   JSValueConst this_value,
                                   int argc,
                                   JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder) return JS_EXCEPTION;
    json nodes;
    if (!collect_interrupt_nodes(context, argc, argv, nodes, "interruptAfter")) return JS_EXCEPTION;
    auto interrupts = builder->definition["interrupt_after"];
    for (const auto& node : nodes)
        interrupts.push_back(node);
    return return_builder(context, this_value);
}

JSValue graph_interrupt_after(JSContext* context,
                              JSValueConst this_value,
                              int argc,
                              JSValueConst* argv) {
    try {
        return graph_interrupt_after_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph interruptAfter failed: " +
                                        bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph interruptAfter failed unexpectedly");
    }
}

JSValue graph_retry_policy_impl(JSContext* context,
                                JSValueConst this_value,
                                int argc,
                                JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 1, "retryPolicy")) return JS_EXCEPTION;
    json policy;
    if (!read_json(context, argv[0], policy, "retry policy")) return JS_EXCEPTION;
    if (!policy.is_object()) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph retry policy must be an object");
    }
    builder->definition["retry_policy"] = std::move(policy);
    return return_builder(context, this_value);
}

JSValue graph_retry_policy(JSContext* context,
                           JSValueConst this_value,
                           int argc,
                           JSValueConst* argv) {
    try {
        return graph_retry_policy_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph retryPolicy failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph retryPolicy failed unexpectedly");
    }
}

JSValue graph_entry_impl(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 1, "entry")) return JS_EXCEPTION;
    std::string node;
    if (!read_string(context, argv[0], node, "entry node")) return JS_EXCEPTION;
    append_edge(*builder, "__start__", node);
    return return_builder(context, this_value);
}

JSValue graph_entry(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_entry_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context, "NeoGraph graph entry failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph entry failed unexpectedly");
    }
}

JSValue graph_exit_impl(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 1, "exit")) return JS_EXCEPTION;
    std::string node;
    if (!read_string(context, argv[0], node, "exit node")) return JS_EXCEPTION;
    append_edge(*builder, node, "__end__");
    return return_builder(context, this_value);
}

JSValue graph_exit(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_exit_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context, "NeoGraph graph exit failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph exit failed unexpectedly");
    }
}

bool define_method(JSContext* context,
                   JSValueConst object,
                   const char* name,
                   JSCFunction* function,
                   int length) {
    JSValue value = JS_NewCFunction(context, function, name, length);
    if (JS_IsException(value)) return false;
    return JS_DefinePropertyValueStr(context, object, name, value, JS_PROP_ENUMERABLE) >= 0;
}

JSValue create_graph_impl(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture) return graph_internal_error(context, "NeoGraph graph constructor is unbound");
    if (!require_arity(context, argc, 1, "graph")) return JS_EXCEPTION;
    std::string name;
    if (!read_string(context, argv[0], name, "name")) return JS_EXCEPTION;
    if (name.empty()) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT", "NeoGraph graph name must not be empty");
    }
    JSValue object = JS_NewObjectClass(context, graph_builder_class_id);
    if (JS_IsException(object)) return object;
    auto* builder = new GraphBuilder{capture,
                                     context,
                                     false,
                                     json{{"schema_version", 1},
                                          {"name", name},
                                          {"nodes", json::object()}}};
    JS_SetOpaque(object, builder);
    const bool installed =
        define_method(context, object, "node", graph_node, 2) &&
        define_method(context, object, "channel", graph_channel, 2) &&
        define_method(context, object, "edge", graph_edge, 2) &&
        define_method(context, object, "conditionalEdge", graph_conditional_edge, 3) &&
        define_method(context, object, "barrier", graph_barrier, 2) &&
        define_method(context, object, "interruptBefore", graph_interrupt_before, 1) &&
        define_method(context, object, "interruptAfter", graph_interrupt_after, 1) &&
        define_method(context, object, "retryPolicy", graph_retry_policy, 1) &&
        define_method(context, object, "entry", graph_entry, 1) &&
        define_method(context, object, "exit", graph_exit, 1);
    if (!installed || JS_PreventExtensions(context, object) < 0) {
        JS_FreeValue(context, object);
        return graph_internal_error(context, "QuickJS could not initialize the NeoGraph graph builder");
    }
    return object;
}

JSValue create_graph(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_graph_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph construction failed unexpectedly");
    }
}

void install_host(JSContext* context) {
    ensure_graph_builder_class(JS_GetRuntime(context));
    JSValue global = JS_GetGlobalObject(context);
    JSValue host   = JS_NewObject(context);
    if (JS_IsException(host)) {
        JS_FreeValue(context, global);
        throw JavaScriptCompileError("P_JS_RUNTIME",
                                     "QuickJS could not allocate the NeoGraph host object");
    }

    const auto fail = [&] {
        JS_FreeValue(context, global);
        throw JavaScriptCompileError("P_JS_RUNTIME", exception_text(context));
    };

    const int version_status = JS_DefinePropertyValueStr(
        context, host, "apiVersion", JS_NewUint32(context, ProgramSource::JAVASCRIPT_HOST_API_VERSION),
        JS_PROP_ENUMERABLE);
    if (version_status < 0) {
        JS_FreeValue(context, host);
        fail();
    }
    const int graph_status = JS_DefinePropertyValueStr(
        context, host, "graph", JS_NewCFunction(context, create_graph, "graph", 1), JS_PROP_ENUMERABLE);
    if (graph_status < 0) {
        JS_FreeValue(context, host);
        fail();
    }
    if (JS_PreventExtensions(context, host) < 0) {
        JS_FreeValue(context, host);
        fail();
    }
    const int host_status = JS_DefinePropertyValueStr(context, global, "ng", host, JS_PROP_ENUMERABLE);
    if (host_status < 0) fail();
    JS_FreeValue(context, global);
}

void validate_limits(const JavaScriptCompileLimits& limits) {
    if (limits.memory_limit_bytes == 0 || limits.max_stack_bytes == 0 ||
        limits.max_interrupt_polls == 0) {
        throw std::invalid_argument("JavaScript compiler limits must be positive");
    }
    if (limits.max_stack_bytes > limits.memory_limit_bytes) {
        throw std::invalid_argument("JavaScript compiler stack limit exceeds memory limit");
    }
}

[[noreturn]] void throw_evaluation_failure(JSContext* context,
                                           const DefinitionCapture& capture,
                                           const InterruptBudget& budget,
                                           const JavaScriptCompileLimits& limits) {
    const auto message = exception_text(context);
    if (!capture.failure_code.empty()) {
        throw JavaScriptCompileError(capture.failure_code, capture.failure_message,
                                     json{{"engine", "quickjs"}});
    }
    if (budget.exhausted || looks_like_resource_exhaustion(message)) {
        throw JavaScriptCompileError(
            "P_JS_RESOURCE_LIMIT", "JavaScript evaluation exceeded a configured resource limit",
            json{{"memory_limit_bytes", limits.memory_limit_bytes},
                 {"max_stack_bytes", limits.max_stack_bytes},
                 {"max_interrupt_polls", limits.max_interrupt_polls},
                 {"interrupt_polls", budget.polls}});
    }
    throw JavaScriptCompileError("P_JS_EVALUATION", "JavaScript evaluation failed: " + message,
                                 json{{"engine", "quickjs"}});
}

json projected_program_document(const json& definition) {
    return json{
        {"program_schema_version", ProgramCompiler::PROGRAM_SCHEMA_VERSION},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root", json{{"op", "call_core"}, {"name", definition.at("name")}, {"definition", definition}}},
        {"declared_budget_requirements",
         json::array({
             json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 60000}},
             json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000}},
             json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000000}},
             json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
             json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
             json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 100}},
             json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
             json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
             json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
         })},
    };
}

}  // namespace
#endif

json evaluate_javascript_source(const ProgramSource&           source,
                                const JavaScriptCompileLimits& limits) {
    if (source.kind() != SourceKind::JavaScript) {
        throw std::invalid_argument("JavaScript evaluator received a non-JavaScript ProgramSource");
    }
#if !defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
    (void)limits;
    throw JavaScriptCompileError(
        "P_JS_UNAVAILABLE", "JavaScript Program sources require NEOGRAPH_BUILD_QUICKJS_CONTROL=ON");
#else
    validate_limits(limits);
    const auto envelope = source.document();
    const auto script   = envelope.at("source").get<std::string>();

    QuickJsScope      scope(limits);
    DefinitionCapture capture;
    JS_SetContextOpaque(scope.context(), &capture);
    try {
        install_host(scope.context());
        InterruptBudget budget{0, limits.max_interrupt_polls, false};
        JS_SetInterruptHandler(scope.runtime(), interrupt_after_budget, &budget);

        JSValue compiled = JS_Eval(scope.context(), script.data(), script.size(), "<program>",
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits);
        }
        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JSValue module_result = JS_EvalFunction(scope.context(), compiled);
        if (JS_IsException(module_result)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits);
        }
        const auto module_state = JS_PromiseState(scope.context(), module_result);
        if (module_state == JS_PROMISE_PENDING) {
            JS_FreeValue(scope.context(), module_result);
            throw JavaScriptCompileError(
                "P_JS_DEFINE_ASYNC",
                "JavaScript definition modules must complete synchronously without top-level await",
                json{{"engine", "quickjs"}});
        }
        if (module_state == JS_PROMISE_REJECTED) {
            JSValue          rejection = JS_PromiseResult(scope.context(), module_result);
            const auto       message   = value_text(scope.context(), rejection);
            JS_FreeValue(scope.context(), rejection);
            JS_FreeValue(scope.context(), module_result);
            if (!capture.failure_code.empty()) {
                throw JavaScriptCompileError(capture.failure_code, capture.failure_message,
                                             json{{"engine", "quickjs"}});
            }
            if (budget.exhausted || looks_like_resource_exhaustion(message)) {
                throw JavaScriptCompileError(
                    "P_JS_RESOURCE_LIMIT",
                    "JavaScript evaluation exceeded a configured resource limit",
                    json{{"memory_limit_bytes", limits.memory_limit_bytes},
                         {"max_stack_bytes", limits.max_stack_bytes},
                         {"max_interrupt_polls", limits.max_interrupt_polls},
                         {"interrupt_polls", budget.polls}});
            }
            throw JavaScriptCompileError(
                "P_JS_EVALUATION", "JavaScript definition module failed: " + message,
                json{{"engine", "quickjs"}});
        }
        if (module_state != JS_PROMISE_FULFILLED) {
            JS_FreeValue(scope.context(), module_result);
            throw JavaScriptCompileError(
                "P_JS_RUNTIME", "QuickJS module evaluation did not return a promise",
                json{{"engine", "quickjs"}});
        }
        JS_FreeValue(scope.context(), module_result);

        JSValue module_namespace = JS_GetModuleNamespace(scope.context(), module);
        if (JS_IsException(module_namespace)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits);
        }
        JSValue define = JS_GetPropertyStr(scope.context(), module_namespace, "define");
        if (JS_IsException(define)) {
            JS_FreeValue(scope.context(), module_namespace);
            throw_evaluation_failure(scope.context(), capture, budget, limits);
        }
        if (!JS_IsFunction(scope.context(), define)) {
            JS_FreeValue(scope.context(), define);
            JS_FreeValue(scope.context(), module_namespace);
            throw JavaScriptCompileError(
                "P_JS_DEFINE_MISSING", "JavaScript source must export a synchronous define() function",
                json{{"engine", "quickjs"}});
        }
        JSValue builder = JS_Call(scope.context(), define, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(scope.context(), define);
        JS_FreeValue(scope.context(), module_namespace);
        if (JS_IsException(builder)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits);
        }
        auto* graph = static_cast<GraphBuilder*>(JS_GetOpaque(builder, graph_builder_class_id));
        if (!graph || graph->context != scope.context() || graph->capture != &capture || graph->sealed) {
            JS_FreeValue(scope.context(), builder);
            throw JavaScriptCompileError(
                "P_JS_DEFINE_VALUE",
                "JavaScript define() must return exactly one open NeoGraph graph builder",
                json{{"engine", "quickjs"}});
        }
        graph->sealed = true;
        auto definition = detail::owned_json_copy(graph->definition);
        JS_FreeValue(scope.context(), builder);
        const auto canonical_definition = detail::canonical_json_bytes(definition);
        if (canonical_definition.size() > kMaxGeneratedDocumentBytes) {
            throw JavaScriptCompileError(
                "P_JS_GRAPH_LIMIT", "JavaScript graph builder generated a definition exceeding 16 MiB",
                json{{"bytes", canonical_definition.size()}, {"limit", kMaxGeneratedDocumentBytes}});
        }
        JS_SetContextOpaque(scope.context(), nullptr);
        return projected_program_document(definition);
    } catch (...) {
        JS_SetContextOpaque(scope.context(), nullptr);
        throw;
    }
#endif
}

}  // namespace neograph::program::detail
