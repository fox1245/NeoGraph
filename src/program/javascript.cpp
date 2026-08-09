#include "javascript.h"

#include <neograph/program/compiler.h>

#include "canonical_json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
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

JavaScriptCompileError::JavaScriptCompileError(std::string               code,
                                               std::string               message,
                                               json                      witness,
                                               std::optional<SourceSpan> source_span)
    : std::runtime_error(message),
      code_(std::move(code)),
      witness_(std::move(witness)),
      source_span_(std::move(source_span)) {}

const std::string& JavaScriptCompileError::code() const noexcept {
    return code_;
}

const json& JavaScriptCompileError::witness() const noexcept {
    return witness_;
}

const std::optional<SourceSpan>& JavaScriptCompileError::source_span() const noexcept {
    return source_span_;
}

#if defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
namespace {

constexpr std::size_t kMaxGeneratedDocumentBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxDiagnosticTextBytes    = 4096u;
constexpr std::size_t kMaxGraphValueDepth        = 64u;
constexpr std::size_t kMaxGraphValueElements     = 1'000'000u;

union AllocationHeader {
    std::max_align_t alignment;
    std::size_t      size;
};

struct AllocationAccounting {
    std::size_t memory_limit_bytes  = 0;
    std::size_t current_bytes       = 0;
    std::size_t peak_bytes          = 0;
    std::size_t allocation_count    = 0;
    std::size_t total_bytes         = 0;
    std::size_t denied_count        = 0;
    std::size_t limit_denied_count  = 0;
    std::size_t native_bytes        = 0;
    std::size_t native_peak_bytes   = 0;
    std::size_t combined_peak_bytes = 0;
};

void record_allocation_denial(AllocationAccounting* accounting, bool memory_limit) noexcept {
    if (!accounting) return;
    ++accounting->denied_count;
    if (memory_limit) ++accounting->limit_denied_count;
}

void* accounted_malloc(JSMallocState* state, std::size_t size) {
    if (size == 0 || size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
        record_allocation_denial(static_cast<AllocationAccounting*>(state->opaque), false);
        return nullptr;
    }
    auto*      accounting   = static_cast<AllocationAccounting*>(state->opaque);
    const auto native_bytes = accounting ? accounting->native_bytes : 0;
    if (size > state->malloc_limit || native_bytes > state->malloc_limit ||
        state->malloc_size > state->malloc_limit - native_bytes ||
        size > state->malloc_limit - native_bytes - state->malloc_size) {
        record_allocation_denial(accounting, true);
        return nullptr;
    }
    auto* allocation = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
    if (!allocation) {
        record_allocation_denial(static_cast<AllocationAccounting*>(state->opaque), false);
        return nullptr;
    }
    allocation->size = size;
    ++state->malloc_count;
    state->malloc_size += size;
    if (auto* accounting = static_cast<AllocationAccounting*>(state->opaque)) {
        ++accounting->allocation_count;
        accounting->current_bytes += size;
        accounting->peak_bytes = std::max(accounting->peak_bytes, accounting->current_bytes);
        accounting->combined_peak_bytes = std::max(
            accounting->combined_peak_bytes, accounting->current_bytes + accounting->native_bytes);
        accounting->total_bytes += size;
    }
    return allocation + 1;
}

void accounted_free(JSMallocState* state, void* pointer) {
    if (!pointer) return;
    auto* allocation = static_cast<AllocationHeader*>(pointer) - 1;
    if (state->malloc_count > 0) --state->malloc_count;
    if (state->malloc_size >= allocation->size) state->malloc_size -= allocation->size;
    if (auto* accounting = static_cast<AllocationAccounting*>(state->opaque)) {
        if (accounting->allocation_count > 0) --accounting->allocation_count;
        if (accounting->current_bytes >= allocation->size)
            accounting->current_bytes -= allocation->size;
    }
    std::free(allocation);
}

void* accounted_realloc(JSMallocState* state, void* pointer, std::size_t size) {
    if (!pointer) return accounted_malloc(state, size);
    auto*      allocation = static_cast<AllocationHeader*>(pointer) - 1;
    const auto old_size   = allocation->size;
    if (size == 0) {
        accounted_free(state, pointer);
        return nullptr;
    }
    auto*      accounting   = static_cast<AllocationAccounting*>(state->opaque);
    const auto native_bytes = accounting ? accounting->native_bytes : 0;
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) ||
        size > state->malloc_limit || native_bytes > state->malloc_limit ||
        old_size > state->malloc_size ||
        state->malloc_size - old_size > state->malloc_limit - native_bytes ||
        size > state->malloc_limit - native_bytes - (state->malloc_size - old_size)) {
        record_allocation_denial(accounting, true);
        return nullptr;
    }
    auto* resized =
        static_cast<AllocationHeader*>(std::realloc(allocation, sizeof(AllocationHeader) + size));
    if (!resized) {
        record_allocation_denial(static_cast<AllocationAccounting*>(state->opaque), false);
        return nullptr;
    }
    resized->size      = size;
    state->malloc_size = state->malloc_size - old_size + size;
    if (accounting) {
        if (size >= old_size)
            accounting->current_bytes += size - old_size;
        else
            accounting->current_bytes -= old_size - size;
        accounting->peak_bytes = std::max(accounting->peak_bytes, accounting->current_bytes);
        accounting->combined_peak_bytes = std::max(
            accounting->combined_peak_bytes, accounting->current_bytes + accounting->native_bytes);
        accounting->total_bytes += size >= old_size ? size - old_size : 0;
    }
    return resized + 1;
}

std::size_t accounted_usable_size(const void* pointer) {
    if (!pointer) return 0;
    return (static_cast<const AllocationHeader*>(pointer) - 1)->size;
}

const JSMallocFunctions kAccountedAllocator{
    accounted_malloc,
    accounted_free,
    accounted_realloc,
    accounted_usable_size,
};

bool reserve_native_bytes(AllocationAccounting& accounting, std::size_t bytes) {
    if (bytes > accounting.memory_limit_bytes ||
        accounting.current_bytes > accounting.memory_limit_bytes - bytes ||
        accounting.native_bytes >
            accounting.memory_limit_bytes - bytes - accounting.current_bytes) {
        record_allocation_denial(&accounting, true);
        return false;
    }
    accounting.native_bytes += bytes;
    accounting.native_peak_bytes = std::max(accounting.native_peak_bytes, accounting.native_bytes);
    accounting.combined_peak_bytes = std::max(accounting.combined_peak_bytes,
                                              accounting.current_bytes + accounting.native_bytes);
    return true;
}

void release_native_bytes(AllocationAccounting& accounting, std::size_t bytes) noexcept {
    accounting.native_bytes =
        bytes <= accounting.native_bytes ? accounting.native_bytes - bytes : 0;
}

struct JavaScriptExceptionDetails {
    std::string               message;
    std::optional<SourceSpan> source_span;
};

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

std::optional<SourceSpan> source_span_from_line_column(std::string_view source,
                                                       std::int32_t     line,
                                                       std::int32_t     column) {
    if (line <= 0 || column <= 0) return std::nullopt;
    std::size_t offset = 0;
    for (std::int32_t current = 1; current < line; ++current) {
        const auto newline = source.find('\n', offset);
        if (newline == std::string_view::npos) return std::nullopt;
        offset = newline + 1;
    }
    offset += static_cast<std::size_t>(column - 1);
    if (offset > source.size()) offset = source.size();
    const auto end = offset < source.size() && source[offset] != '\n' ? offset + 1 : offset;
    return SourceSpan{offset,
                      end,
                      static_cast<std::uint32_t>(line),
                      static_cast<std::uint32_t>(column),
                      static_cast<std::uint32_t>(line),
                      static_cast<std::uint32_t>(column + (end > offset ? 1 : 0))};
}

bool javascript_identifier_character(unsigned char value) noexcept {
    return value == '_' || value == '$' || std::isalnum(value) != 0;
}

std::optional<std::size_t> find_javascript_token(std::string_view source,
                                                 std::string_view token,
                                                 std::size_t      start = 0) {
    std::size_t offset = start;
    while (offset < source.size()) {
        if (source[offset] == '/' && offset + 1 < source.size() && source[offset + 1] == '/') {
            offset += 2;
            while (offset < source.size() && source[offset] != '\n')
                ++offset;
            continue;
        }
        if (source[offset] == '/' && offset + 1 < source.size() && source[offset + 1] == '*') {
            offset += 2;
            while (offset + 1 < source.size() &&
                   !(source[offset] == '*' && source[offset + 1] == '/'))
                ++offset;
            offset = std::min(source.size(), offset + 2);
            continue;
        }
        if (source[offset] == '\'' || source[offset] == '"' || source[offset] == '`') {
            const auto quote = source[offset++];
            while (offset < source.size()) {
                if (source[offset] == '\\') {
                    offset += std::min<std::size_t>(2, source.size() - offset);
                } else if (source[offset++] == quote) {
                    break;
                }
            }
            continue;
        }
        if (offset + token.size() <= source.size() &&
            source.substr(offset, token.size()) == token &&
            (offset == 0 ||
             !javascript_identifier_character(static_cast<unsigned char>(source[offset - 1]))) &&
            (offset + token.size() == source.size() ||
             !javascript_identifier_character(
                 static_cast<unsigned char>(source[offset + token.size()]))))
            return offset;
        ++offset;
    }
    return std::nullopt;
}

SourceSpan source_span_at(std::string_view source, std::size_t found, std::size_t length) {
    std::size_t line = 1;
    std::size_t col  = 1;
    for (std::size_t index = 0; index < found; ++index) {
        if (source[index] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
    return SourceSpan{found,
                      found + length,
                      static_cast<std::uint32_t>(line),
                      static_cast<std::uint32_t>(col),
                      static_cast<std::uint32_t>(line),
                      static_cast<std::uint32_t>(col + length)};
}

std::optional<SourceSpan> source_span_for_token(std::string_view source, std::string_view token) {
    const auto found = find_javascript_token(source, token);
    if (!found) return std::nullopt;
    return source_span_at(source, *found, token.size());
}

void reject_unsealed_module_syntax(std::string_view source) {
    auto span = source_span_for_token(source, "import");
    if (!span) {
        std::size_t search = 0;
        while (const auto exported = find_javascript_token(source, "export", search)) {
            const auto statement_end = source.find(';', *exported);
            const auto from          = find_javascript_token(source, "from", *exported + 6);
            if (from && (statement_end == std::string_view::npos || *from < statement_end)) {
                auto specifier = *from + 4;
                while (specifier < source.size() &&
                       std::isspace(static_cast<unsigned char>(source[specifier])) != 0)
                    ++specifier;
                if (specifier < source.size() &&
                    (source[specifier] == '\'' || source[specifier] == '"')) {
                    span = source_span_at(source, *exported, specifier - *exported + 1);
                    break;
                }
            }
            search = *exported + 6;
        }
    }
    if (!span) return;
    throw JavaScriptCompileError(
        "P_JS_IMPORT_UNAVAILABLE",
        "Executable JavaScript imports are unavailable without receipt-bound source bytes",
        json{{"engine", "quickjs"},
             {"facility", "import"},
             {"receipt_bound_source_available", false}},
        std::move(span));
}

bool read_error_integer(JSContext*    context,
                        JSValueConst  error,
                        const char*   property,
                        std::int32_t& output) {
    JSValue value = JS_GetPropertyStr(context, error, property);
    if (JS_IsException(value)) {
        JS_FreeValue(context, value);
        if (JS_HasException(context)) {
            JSValue ignored = JS_GetException(context);
            JS_FreeValue(context, ignored);
        }
        return false;
    }
    const bool present = JS_IsNumber(value) && JS_ToInt32(context, &output, value) >= 0;
    if (JS_HasException(context)) {
        JSValue ignored = JS_GetException(context);
        JS_FreeValue(context, ignored);
    }
    JS_FreeValue(context, value);
    return present;
}

JavaScriptExceptionDetails exception_details(JSContext*       context,
                                             JSValueConst     error,
                                             std::string_view source) {
    JavaScriptExceptionDetails details;
    const char*                text = JS_ToCString(context, error);
    details.message                 = text ? bounded_utf8(text) : "QuickJS exception";
    if (text) JS_FreeCString(context, text);

    std::int32_t line = 0;
    std::int32_t col  = 0;
    if (read_error_integer(context, error, "lineNumber", line) &&
        read_error_integer(context, error, "columnNumber", col)) {
        details.source_span = source_span_from_line_column(source, line, col);
    }
    if (!details.source_span) {
        JSValue stack = JS_GetPropertyStr(context, error, "stack");
        if (!JS_IsException(stack) && JS_IsString(stack)) {
            size_t      size       = 0;
            const char* text_stack = JS_ToCStringLen(context, &size, stack);
            if (text_stack) {
                std::string_view stack_view(text_stack, size);
                // QuickJS formats frames as `filename:line:column`. The
                // final pair is sufficient for a stable primary site.
                const auto colon = stack_view.rfind(':');
                if (colon != std::string_view::npos && colon > 0) {
                    const auto previous = stack_view.rfind(':', colon - 1);
                    if (previous != std::string_view::npos) {
                        try {
                            line = static_cast<std::int32_t>(std::stoi(std::string(
                                stack_view.substr(previous + 1, colon - previous - 1))));
                            col  = static_cast<std::int32_t>(
                                std::stoi(std::string(stack_view.substr(colon + 1))));
                            details.source_span = source_span_from_line_column(source, line, col);
                        } catch (...) {
                            // The engine's human-readable stack is a best
                            // effort fallback; the diagnostic remains useful
                            // with its source site even when parsing changes.
                        }
                    }
                }
                JS_FreeCString(context, text_stack);
            }
        }
        JS_FreeValue(context, stack);
        if (JS_HasException(context)) {
            JSValue ignored = JS_GetException(context);
            JS_FreeValue(context, ignored);
        }
    }
    return details;
}

JavaScriptExceptionDetails take_exception_details(JSContext* context, std::string_view source) {
    JSValue exception = JS_GetException(context);
    auto    details   = exception_details(context, exception, source);
    JS_FreeValue(context, exception);
    return details;
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

enum class InterruptReason : std::uint8_t {
    None,
    PollLimit,
    WallTime,
    Cancelled,
};

struct InterruptBudget {
    std::uint64_t                         polls = 0;
    std::uint64_t                         limit = 0;
    std::chrono::steady_clock::time_point deadline{};
    std::function<bool()>                 cancellation_requested;
    InterruptReason                       reason = InterruptReason::None;
};

int interrupt_after_budget(JSRuntime*, void* opaque) {
    auto& budget = *static_cast<InterruptBudget*>(opaque);
    if (budget.reason != InterruptReason::None) return 1;
    if (budget.cancellation_requested && budget.cancellation_requested()) {
        budget.reason = InterruptReason::Cancelled;
        return 1;
    }
    if (budget.deadline != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= budget.deadline) {
        budget.reason = InterruptReason::WallTime;
        return 1;
    }
    if (budget.polls >= budget.limit) {
        budget.reason = InterruptReason::PollLimit;
        return 1;
    }
    ++budget.polls;
    return 0;
}

void refresh_external_interrupt(InterruptBudget& budget) {
    if (budget.reason != InterruptReason::None) return;
    if (budget.cancellation_requested && budget.cancellation_requested()) {
        budget.reason = InterruptReason::Cancelled;
    } else if (budget.deadline != std::chrono::steady_clock::time_point{} &&
               std::chrono::steady_clock::now() >= budget.deadline) {
        budget.reason = InterruptReason::WallTime;
    }
}

struct DefinitionCapture {
    std::string           failure_code;
    std::string           failure_message;
    json                  failure_witness          = json::object();
    std::size_t           native_builder_bytes     = 0;
    std::size_t           max_native_builder_bytes = kMaxGeneratedDocumentBytes;
    AllocationAccounting* accounting               = nullptr;
};

void record_failure(DefinitionCapture* capture,
                    std::string        code,
                    std::string        message,
                    json               witness = json::object()) {
    if (!capture || !capture->failure_code.empty()) return;
    capture->failure_code    = std::move(code);
    capture->failure_message = std::move(message);
    capture->failure_witness = std::move(witness);
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
        accounting_.memory_limit_bytes = limits.memory_limit_bytes;
        runtime_ = JS_NewRuntime2(&kAccountedAllocator, &accounting_);
        if (!runtime_) {
            throw JavaScriptCompileError("P_JS_RUNTIME", "QuickJS runtime initialization failed");
        }
        JS_SetRuntimeOpaque(runtime_, &accounting_);
        JS_SetMemoryLimit(runtime_, limits.memory_limit_bytes);
        JS_SetMaxStackSize(runtime_, limits.max_stack_bytes);
        JS_SetCanBlock(runtime_, 0);
        // Build the ordinary language surface without installing Date. The
        // evaluator intrinsic is needed by JS_Eval for module execution, then
        // its public eval binding is replaced by the strict profile below.
        context_ = JS_NewContextRaw(runtime_);
        if (context_ &&
            (JS_AddIntrinsicBaseObjects(context_) || JS_AddIntrinsicEval(context_) ||
             JS_AddIntrinsicStringNormalize(context_) || JS_AddIntrinsicRegExp(context_) ||
             JS_AddIntrinsicJSON(context_) || JS_AddIntrinsicProxy(context_) ||
             JS_AddIntrinsicMapSet(context_) || JS_AddIntrinsicTypedArrays(context_) ||
             JS_AddIntrinsicPromise(context_) || JS_AddIntrinsicWeakRef(context_))) {
            JS_FreeContext(context_);
            context_ = nullptr;
        }
        if (!context_) {
            JS_FreeRuntime(runtime_);
            runtime_ = nullptr;
            throw JavaScriptCompileError("P_JS_RUNTIME", "QuickJS context initialization failed");
        }
    }

    QuickJsScope(const QuickJsScope&)            = delete;
    QuickJsScope& operator=(const QuickJsScope&) = delete;

    ~QuickJsScope() {
        if (runtime_) {
            JS_SetInterruptHandler(runtime_, nullptr, nullptr);
        }
        if (context_) JS_FreeContext(context_);
        if (runtime_) {
            JS_FreeRuntime(runtime_);
        }
    }

    JSRuntime*                  runtime() const noexcept { return runtime_; }
    JSContext*                  context() const noexcept { return context_; }
    AllocationAccounting&       accounting() noexcept { return accounting_; }
    const AllocationAccounting& accounting() const noexcept { return accounting_; }

private:
    AllocationAccounting accounting_;
    JSRuntime*           runtime_ = nullptr;
    JSContext*           context_ = nullptr;
};

struct GraphBuilder {
    DefinitionCapture* capture      = nullptr;
    JSContext*         context      = nullptr;
    bool               sealed       = false;
    std::size_t        native_bytes = 0;
    json               definition;
};

JSClassID      graph_builder_class_id = JS_INVALID_CLASS_ID;
std::once_flag graph_builder_class_once;
JSClassID      command_value_class_id = JS_INVALID_CLASS_ID;
std::once_flag command_value_class_once;

void graph_builder_finalizer(JSRuntime* runtime, JSValue value) {
    auto* builder = static_cast<GraphBuilder*>(JS_GetOpaque(value, graph_builder_class_id));
    if (!builder) return;
    auto*      accounting   = static_cast<AllocationAccounting*>(JS_GetRuntimeOpaque(runtime));
    const auto native_bytes = builder->native_bytes;
    delete builder;
    if (accounting) release_native_bytes(*accounting, native_bytes);
}

void ensure_graph_builder_class(JSRuntime* runtime) {
    std::call_once(graph_builder_class_once, [] { JS_NewClassID(&graph_builder_class_id); });
    if (JS_IsRegisteredClass(runtime, graph_builder_class_id)) return;
    static const JSClassDef definition{
        "NeoGraphGraphBuilder", graph_builder_finalizer, nullptr, nullptr, nullptr,
    };
    if (JS_NewClass(runtime, graph_builder_class_id, &definition) < 0) {
        throw JavaScriptCompileError("P_JS_RUNTIME",
                                     "QuickJS could not register the NeoGraph graph builder class");
    }
}

struct CommandValue {
    DefinitionCapture* capture      = nullptr;
    JSContext*         context      = nullptr;
    std::size_t        native_bytes = 0;
    JavaScriptCommand  command;
};

void command_value_finalizer(JSRuntime* runtime, JSValue value) {
    auto* command = static_cast<CommandValue*>(JS_GetOpaque(value, command_value_class_id));
    if (!command) return;
    auto*      accounting   = static_cast<AllocationAccounting*>(JS_GetRuntimeOpaque(runtime));
    const auto native_bytes = command->native_bytes;
    delete command;
    if (accounting) release_native_bytes(*accounting, native_bytes);
}

void ensure_command_value_class(JSRuntime* runtime) {
    std::call_once(command_value_class_once, [] { JS_NewClassID(&command_value_class_id); });
    if (JS_IsRegisteredClass(runtime, command_value_class_id)) return;
    static const JSClassDef definition{
        "NeoGraphJavaScriptCommand", command_value_finalizer, nullptr, nullptr, nullptr,
    };
    if (JS_NewClass(runtime, command_value_class_id, &definition) < 0) {
        throw JavaScriptCompileError("P_JS_RUNTIME",
                                     "QuickJS could not register the NeoGraph command value class");
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

bool read_string(JSContext*       context,
                 JSValueConst     value,
                 std::string&     output,
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
    std::size_t elements   = 0;
    std::size_t bytes      = 0;
    std::size_t byte_limit = kMaxGeneratedDocumentBytes;
    std::string error;
};

bool consume_json_bytes(JsonConversion& state, std::size_t bytes) {
    if (bytes > state.byte_limit || state.bytes > state.byte_limit - bytes) {
        state.error = "graph configuration exceeds the native byte limit";
        return false;
    }
    state.bytes += bytes;
    return true;
}

bool consume_json_element(JsonConversion& state) {
    if (state.elements >= kMaxGraphValueElements) {
        state.error = "graph configuration exceeds the element limit";
        return false;
    }
    ++state.elements;
    return true;
}

bool get_standard_prototype(JSContext*   context,
                            const char*  constructor_name,
                            JSValue&     output,
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

bool js_to_json(JSContext*      context,
                JSValueConst    value,
                json&           output,
                JsonConversion& state,
                std::size_t     depth) {
    if (depth > kMaxGraphValueDepth) {
        state.error = "graph configuration exceeds the nesting-depth limit";
        return false;
    }
    if (JS_IsNull(value)) {
        if (!consume_json_bytes(state, 4)) return false;
        output = nullptr;
        return true;
    }
    if (JS_IsBool(value)) {
        const int boolean = JS_ToBool(context, value);
        if (boolean < 0) {
            state.error = exception_text(context);
            return false;
        }
        if (!consume_json_bytes(state, boolean ? 4 : 5)) return false;
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
        if (!consume_json_bytes(state, size > std::numeric_limits<std::size_t>::max() - 2
                                           ? std::numeric_limits<std::size_t>::max()
                                           : size + 2)) {
            JS_FreeCString(context, data);
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
            constexpr double kMaxSafeInteger = 0x1fffffffffffffp0;
            if (number < -kMaxSafeInteger || number > kMaxSafeInteger) {
                state.error =
                    "graph configuration integer is outside the JavaScript safe-integer range";
                return false;
            }
            if (!consume_json_bytes(state, 24)) return false;
            if (number >= 0.0) {
                output = static_cast<std::uint64_t>(number);
            } else {
                output = static_cast<std::int64_t>(number);
            }
            return true;
        }
        if (!consume_json_bytes(state, 24)) return false;
        output = number;
        return true;
    }
    if (!JS_IsObject(value)) {
        state.error = "graph configuration contains an unsupported JavaScript value";
        return false;
    }
    if (command_value_class_id != JS_INVALID_CLASS_ID &&
        JS_GetOpaque(value, command_value_class_id) != nullptr) {
        state.error = "graph configuration cannot contain a sealed host command";
        return false;
    }
    if (graph_builder_class_id != JS_INVALID_CLASS_ID &&
        JS_GetOpaque(value, graph_builder_class_id) != nullptr) {
        state.error = "graph configuration cannot contain a live graph builder";
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
        if (!consume_json_bytes(state, 2)) return false;
        output = json::array();
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(length); ++index) {
            if (!consume_json_element(state)) return false;
            JSValue element = JS_GetPropertyUint32(context, value, index);
            if (JS_IsException(element)) {
                state.error = exception_text(context);
                return false;
            }
            json       converted;
            const bool converted_ok = js_to_json(context, element, converted, state, depth + 1);
            JS_FreeValue(context, element);
            if (!converted_ok) return false;
            output.push_back(std::move(converted));
        }
        return true;
    }
    if (!is_plain_object(context, value, state.error)) return false;

    JSPropertyEnum* symbols      = nullptr;
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

    JSPropertyEnum* properties     = nullptr;
    std::uint32_t   property_count = 0;
    if (JS_GetOwnPropertyNames(context, &properties, &property_count, value,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        state.error = exception_text(context);
        return false;
    }
    output = json::object();
    if (!consume_json_bytes(state, 2)) {
        JS_FreePropertyEnum(context, properties, property_count);
        return false;
    }
    for (std::uint32_t index = 0; index < property_count; ++index) {
        if (!consume_json_element(state)) {
            JS_FreePropertyEnum(context, properties, property_count);
            return false;
        }
        std::size_t key_size = 0;
        const char* key      = JS_AtomToCStringLen(context, &key_size, properties[index].atom);
        if (!key) {
            JS_FreePropertyEnum(context, properties, property_count);
            state.error = exception_text(context);
            return false;
        }
        const std::string name(key, key_size);
        JS_FreeCString(context, key);
        if (!consume_json_bytes(state, name.size() > std::numeric_limits<std::size_t>::max() - 4
                                           ? std::numeric_limits<std::size_t>::max()
                                           : name.size() + 4)) {
            JS_FreePropertyEnum(context, properties, property_count);
            return false;
        }
        JSValue property = JS_GetProperty(context, value, properties[index].atom);
        if (JS_IsException(property)) {
            JS_FreePropertyEnum(context, properties, property_count);
            state.error = exception_text(context);
            return false;
        }
        json       converted;
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

bool read_json(JSContext*       context,
               JSValueConst     value,
               json&            output,
               std::string_view argument_name) {
    JsonConversion conversion;
    if (const auto* capture = static_cast<const DefinitionCapture*>(JS_GetContextOpaque(context)))
        conversion.byte_limit = capture->max_native_builder_bytes;
    if (js_to_json(context, value, output, conversion, 0)) {
        const auto canonical = detail::canonical_json_bytes(output);
        if (canonical.size() <= conversion.byte_limit) return true;
        conversion.error = "graph configuration exceeds the native byte limit";
    }
    if (conversion.error.empty()) return false;
    const auto code = conversion.error == "graph configuration exceeds the native byte limit"
                          ? "P_JS_GRAPH_LIMIT"
                          : "P_JS_GRAPH_VALUE";
    graph_error(context, code,
                "NeoGraph graph " + std::string(argument_name) +
                    " is not canonical JSON data: " + conversion.error);
    return false;
}

std::size_t canonical_size(const json& value) {
    return detail::canonical_json_bytes(value).size();
}

std::size_t replace_serialized_component(std::size_t total,
                                         std::size_t old_size,
                                         std::size_t new_size) {
    if (old_size > total) throw std::logic_error("invalid JavaScript native-size accounting");
    const auto remainder = total - old_size;
    if (new_size > std::numeric_limits<std::size_t>::max() - remainder)
        throw std::length_error("JavaScript native-size accounting overflow");
    return remainder + new_size;
}

std::size_t size_after_object_set(std::size_t      total,
                                  const json&      object,
                                  std::string_view key,
                                  const json&      value) {
    const auto found = object.find(std::string(key));
    if (found != object.end())
        return replace_serialized_component(total, canonical_size(*found), canonical_size(value));
    const auto key_size   = canonical_size(json(std::string(key)));
    const auto value_size = canonical_size(value);
    const auto separator  = object.empty() ? 0u : 1u;
    if (key_size > std::numeric_limits<std::size_t>::max() - value_size - separator - 1u ||
        total > std::numeric_limits<std::size_t>::max() - key_size - value_size - separator - 1u)
        throw std::length_error("JavaScript native-size accounting overflow");
    return total + key_size + 1u + value_size + separator;
}

std::size_t size_after_array_append(std::size_t total, const json& array, const json& value) {
    const auto value_size = canonical_size(value);
    const auto separator  = array.empty() ? 0u : 1u;
    if (total > std::numeric_limits<std::size_t>::max() - value_size - separator)
        throw std::length_error("JavaScript native-size accounting overflow");
    return total + value_size + separator;
}

bool prepare_builder_size(JSContext* context, GraphBuilder* builder, std::size_t new_size) {
    if (!builder || !builder->capture || !builder->capture->accounting) {
        graph_internal_error(context, "JavaScript native memory accounting is unbound");
        return false;
    }
    builder->capture->native_builder_bytes =
        std::max(builder->capture->native_builder_bytes, new_size);
    if (new_size > builder->capture->max_native_builder_bytes) {
        graph_error(context, "P_JS_GRAPH_LIMIT",
                    "NeoGraph graph builder exceeds its native byte limit");
        return false;
    }
    auto& accounting = *builder->capture->accounting;
    if (new_size > builder->native_bytes) {
        if (!reserve_native_bytes(accounting, new_size - builder->native_bytes)) {
            graph_error(context, "P_JS_RESOURCE_LIMIT",
                        "NeoGraph native bridge exceeds the JavaScript memory limit");
            return false;
        }
    } else {
        release_native_bytes(accounting, builder->native_bytes - new_size);
    }
    builder->native_bytes = new_size;
    return true;
}

bool read_string_array(JSContext*       context,
                       JSValueConst     value,
                       json&            output,
                       std::string_view argument_name) {
    if (!read_json(context, value, output, argument_name)) return false;
    if (!output.is_array()) {
        graph_error(
            context, "P_JS_GRAPH_ARGUMENT",
            "NeoGraph graph " + std::string(argument_name) + " must be an array of strings");
        return false;
    }
    for (const auto& element : output) {
        if (!element.is_string()) {
            graph_error(
                context, "P_JS_GRAPH_ARGUMENT",
                "NeoGraph graph " + std::string(argument_name) + " must be an array of strings");
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
    const auto new_size = size_after_object_set(builder->native_bytes, nodes, name, config);
    if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
    nodes[name] = std::move(config);
    return return_builder(context, this_value);
}

JSValue graph_node(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_node_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph node failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph node failed unexpectedly");
    }
}

JSValue graph_channel_impl(JSContext*    context,
                           JSValueConst  this_value,
                           int           argc,
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
    if (!builder->definition.contains("channels")) {
        json channels  = json::object();
        channels[name] = config;
        const auto new_size =
            size_after_object_set(builder->native_bytes, builder->definition, "channels", channels);
        if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
        builder->definition["channels"] = std::move(channels);
    } else {
        auto channels = builder->definition["channels"];
        if (channels.contains(name)) {
            return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                               "NeoGraph graph channel names must be unique");
        }
        const auto new_size = size_after_object_set(builder->native_bytes, channels, name, config);
        if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
        channels[name] = std::move(config);
    }
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

bool append_edge(JSContext*         context,
                 GraphBuilder&      builder,
                 const std::string& from,
                 const std::string& to) {
    json edge{{"from", from}, {"to", to}};
    if (!builder.definition.contains("edges")) {
        json       edges = json::array({edge});
        const auto new_size =
            size_after_object_set(builder.native_bytes, builder.definition, "edges", edges);
        if (!prepare_builder_size(context, &builder, new_size)) return false;
        builder.definition["edges"] = std::move(edges);
        return true;
    }
    auto       edges    = builder.definition["edges"];
    const auto new_size = size_after_array_append(builder.native_bytes, edges, edge);
    if (!prepare_builder_size(context, &builder, new_size)) return false;
    edges.push_back(std::move(edge));
    return true;
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
    if (!append_edge(context, *builder, from, to)) return JS_EXCEPTION;
    return return_builder(context, this_value);
}

JSValue graph_edge(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_edge_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph edge failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph edge failed unexpectedly");
    }
}

JSValue graph_conditional_edge_impl(JSContext*    context,
                                    JSValueConst  this_value,
                                    int           argc,
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
    json conditional{{"from", from}, {"condition", condition}, {"routes", std::move(routes)}};
    if (!builder->definition.contains("conditional_edges")) {
        json       edges    = json::array({conditional});
        const auto new_size = size_after_object_set(builder->native_bytes, builder->definition,
                                                    "conditional_edges", edges);
        if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
        builder->definition["conditional_edges"] = std::move(edges);
    } else {
        auto       edges    = builder->definition["conditional_edges"];
        const auto new_size = size_after_array_append(builder->native_bytes, edges, conditional);
        if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
        edges.push_back(std::move(conditional));
    }
    return return_builder(context, this_value);
}

JSValue graph_conditional_edge(JSContext*    context,
                               JSValueConst  this_value,
                               int           argc,
                               JSValueConst* argv) {
    try {
        return graph_conditional_edge_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph graph conditionalEdge failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph conditionalEdge failed unexpectedly");
    }
}

JSValue graph_barrier_impl(JSContext*    context,
                           JSValueConst  this_value,
                           int           argc,
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
    json       barrier{{"wait_for", std::move(wait_for)}};
    auto       node_config = nodes[node];
    const auto new_size =
        size_after_object_set(builder->native_bytes, node_config, "barrier", barrier);
    if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
    node_config["barrier"] = std::move(barrier);
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

bool collect_interrupt_nodes(
    JSContext* context, int argc, JSValueConst* argv, json& output, std::string_view operation) {
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

JSValue graph_interrupt_before_impl(JSContext*    context,
                                    JSValueConst  this_value,
                                    int           argc,
                                    JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder) return JS_EXCEPTION;
    json nodes;
    if (!collect_interrupt_nodes(context, argc, argv, nodes, "interruptBefore"))
        return JS_EXCEPTION;
    if (!builder->definition.contains("interrupt_before")) {
        const auto new_size = size_after_object_set(builder->native_bytes, builder->definition,
                                                    "interrupt_before", nodes);
        if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
        builder->definition["interrupt_before"] = std::move(nodes);
    } else {
        auto       interrupts = builder->definition["interrupt_before"];
        const auto added      = canonical_size(nodes) - 2u + (interrupts.empty() ? 0u : 1u);
        if (builder->native_bytes > std::numeric_limits<std::size_t>::max() - added)
            throw std::length_error("JavaScript native-size accounting overflow");
        if (!prepare_builder_size(context, builder, builder->native_bytes + added))
            return JS_EXCEPTION;
        for (auto node : nodes)
            interrupts.push_back(std::move(node));
    }
    return return_builder(context, this_value);
}

JSValue graph_interrupt_before(JSContext*    context,
                               JSValueConst  this_value,
                               int           argc,
                               JSValueConst* argv) {
    try {
        return graph_interrupt_before_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph graph interruptBefore failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph interruptBefore failed unexpectedly");
    }
}

JSValue graph_interrupt_after_impl(JSContext*    context,
                                   JSValueConst  this_value,
                                   int           argc,
                                   JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder) return JS_EXCEPTION;
    json nodes;
    if (!collect_interrupt_nodes(context, argc, argv, nodes, "interruptAfter")) return JS_EXCEPTION;
    if (!builder->definition.contains("interrupt_after")) {
        const auto new_size = size_after_object_set(builder->native_bytes, builder->definition,
                                                    "interrupt_after", nodes);
        if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
        builder->definition["interrupt_after"] = std::move(nodes);
    } else {
        auto       interrupts = builder->definition["interrupt_after"];
        const auto added      = canonical_size(nodes) - 2u + (interrupts.empty() ? 0u : 1u);
        if (builder->native_bytes > std::numeric_limits<std::size_t>::max() - added)
            throw std::length_error("JavaScript native-size accounting overflow");
        if (!prepare_builder_size(context, builder, builder->native_bytes + added))
            return JS_EXCEPTION;
        for (auto node : nodes)
            interrupts.push_back(std::move(node));
    }
    return return_builder(context, this_value);
}

JSValue graph_interrupt_after(JSContext*    context,
                              JSValueConst  this_value,
                              int           argc,
                              JSValueConst* argv) {
    try {
        return graph_interrupt_after_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph graph interruptAfter failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph interruptAfter failed unexpectedly");
    }
}

JSValue graph_retry_policy_impl(JSContext*    context,
                                JSValueConst  this_value,
                                int           argc,
                                JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 1, "retryPolicy")) return JS_EXCEPTION;
    json policy;
    if (!read_json(context, argv[0], policy, "retry policy")) return JS_EXCEPTION;
    if (!policy.is_object()) {
        return graph_error(context, "P_JS_GRAPH_ARGUMENT",
                           "NeoGraph graph retry policy must be an object");
    }
    const auto new_size =
        size_after_object_set(builder->native_bytes, builder->definition, "retry_policy", policy);
    if (!prepare_builder_size(context, builder, new_size)) return JS_EXCEPTION;
    builder->definition["retry_policy"] = std::move(policy);
    return return_builder(context, this_value);
}

JSValue graph_retry_policy(JSContext*    context,
                           JSValueConst  this_value,
                           int           argc,
                           JSValueConst* argv) {
    try {
        return graph_retry_policy_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph graph retryPolicy failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph retryPolicy failed unexpectedly");
    }
}

JSValue graph_entry_impl(JSContext*    context,
                         JSValueConst  this_value,
                         int           argc,
                         JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 1, "entry")) return JS_EXCEPTION;
    std::string node;
    if (!read_string(context, argv[0], node, "entry node")) return JS_EXCEPTION;
    if (!append_edge(context, *builder, "__start__", node)) return JS_EXCEPTION;
    return return_builder(context, this_value);
}

JSValue graph_entry(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_entry_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph entry failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph entry failed unexpectedly");
    }
}

JSValue graph_exit_impl(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    auto* builder = require_open_builder(context, this_value);
    if (!builder || !require_arity(context, argc, 1, "exit")) return JS_EXCEPTION;
    std::string node;
    if (!read_string(context, argv[0], node, "exit node")) return JS_EXCEPTION;
    if (!append_edge(context, *builder, node, "__end__")) return JS_EXCEPTION;
    return return_builder(context, this_value);
}

JSValue graph_exit(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return graph_exit_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(context,
                                    "NeoGraph graph exit failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph exit failed unexpectedly");
    }
}

bool define_method(
    JSContext* context, JSValueConst object, const char* name, JSCFunction* function, int length) {
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
    json       definition{{"schema_version", 1}, {"name", name}, {"nodes", json::object()}};
    const auto document_size = canonical_size(definition);
    if (document_size > capture->max_native_builder_bytes) {
        return graph_error(context, "P_JS_GRAPH_LIMIT",
                           "NeoGraph graph builder exceeds its native byte limit");
    }
    const auto native_size        = sizeof(GraphBuilder) + document_size;
    capture->native_builder_bytes = std::max(capture->native_builder_bytes, document_size);
    if (!capture->accounting || !reserve_native_bytes(*capture->accounting, native_size)) {
        return graph_error(context, "P_JS_RESOURCE_LIMIT",
                           "NeoGraph native bridge exceeds the JavaScript memory limit");
    }
    JSValue object = JS_NewObjectClass(context, graph_builder_class_id);
    if (JS_IsException(object)) {
        release_native_bytes(*capture->accounting, native_size);
        return object;
    }
    GraphBuilder* builder = nullptr;
    try {
        builder = new GraphBuilder{capture, context, false, native_size, std::move(definition)};
    } catch (...) {
        release_native_bytes(*capture->accounting, native_size);
        JS_FreeValue(context, object);
        throw;
    }
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
        return graph_internal_error(context,
                                    "QuickJS could not initialize the NeoGraph graph builder");
    }
    return object;
}

JSValue create_graph(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_graph_impl(context, this_value, argc, argv);
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph graph construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph graph construction failed unexpectedly");
    }
}

JSValue json_to_js_value(JSContext* context, const json& value) {
    const auto canonical = detail::canonical_json_bytes(value);
    return JS_ParseJSON(context, canonical.c_str(), canonical.size(), "<program-command>");
}

constexpr std::string_view kDefaultCommandSourceSite = "<program-control>";

bool read_command_source_site(
    JSContext* context, int argc, JSValueConst* argv, int index, std::string& output) {
    output = std::string(kDefaultCommandSourceSite);
    if (index >= argc || JS_IsUndefined(argv[index])) return true;
    return read_string(context, argv[index], output, "command source_site");
}

bool read_command_uint64(JSContext*       context,
                         JSValueConst     value,
                         std::uint64_t&   output,
                         std::string_view argument_name,
                         bool             require_positive = false) {
    json converted;
    if (!read_json(context, value, converted, argument_name)) return false;
    if (!converted.is_number_unsigned() ||
        (require_positive && converted.get<std::uint64_t>() == 0)) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "NeoGraph command " + std::string(argument_name) +
                        " must be an unsigned integer" +
                        (require_positive ? " greater than zero" : ""));
        return false;
    }
    output = converted.get<std::uint64_t>();
    return true;
}

CommandValue* require_sealed_command(JSContext* context, JSValueConst value) {
    if (!JS_IsObject(value)) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "NeoGraph command arguments must be sealed ng command values");
        return nullptr;
    }
    auto* command = static_cast<CommandValue*>(JS_GetOpaque(value, command_value_class_id));
    if (!command || command->context != context ||
        command->capture != JS_GetContextOpaque(context)) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "NeoGraph command value is forged or belongs to another control context");
        return nullptr;
    }
    return command;
}

bool read_sealed_command(JSContext* context, JSValueConst value, JavaScriptCommand& output) {
    auto* command = require_sealed_command(context, value);
    if (!command) return false;
    output = command->command;
    return true;
}

bool read_sealed_command_array(JSContext*                      context,
                               JSValueConst                    value,
                               std::vector<JavaScriptCommand>& output) {
    std::string array_error;
    if (!JS_IsArray(context, value) || !is_plain_array(context, value, array_error)) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "NeoGraph command members must be an ordinary array of sealed commands");
        return false;
    }
    JSValue length_value = JS_GetPropertyStr(context, value, "length");
    if (JS_IsException(length_value)) return false;
    std::uint64_t length = 0;
    if (JS_ToIndex(context, &length, length_value) < 0) {
        JS_FreeValue(context, length_value);
        return false;
    }
    JS_FreeValue(context, length_value);
    if (length == 0) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "NeoGraph command members must be nonempty and bounded");
        return false;
    }
    if (length >= JAVASCRIPT_COMMAND_MAX_AGGREGATE_MEMBERS) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "JavaScript command exceeds maximum aggregate member count");
        return false;
    }
    output.clear();
    output.reserve(static_cast<std::size_t>(length));
    for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(length); ++index) {
        JSValue member = JS_GetPropertyUint32(context, value, index);
        if (JS_IsException(member)) return false;
        std::optional<JavaScriptCommand> command;
        const bool                       valid = [&] {
            JavaScriptCommand decoded = JavaScriptCommand::call_core(
                std::string(kDefaultCommandSourceSite), "__placeholder__", json::object());
            if (!read_sealed_command(context, member, decoded)) return false;
            command = std::move(decoded);
            return true;
        }();
        JS_FreeValue(context, member);
        if (!valid) return false;
        output.push_back(std::move(*command));
    }
    return true;
}

JSValue make_command_value(JSContext*         context,
                           DefinitionCapture* capture,
                           JavaScriptCommand  command) {
    ensure_command_value_class(JS_GetRuntime(context));
    const auto encoded     = command.to_json();
    const auto native_size = sizeof(CommandValue) + canonical_size(encoded);
    if (!capture || !capture->accounting ||
        !reserve_native_bytes(*capture->accounting, native_size)) {
        return graph_error(context, "P_JS_RESOURCE_LIMIT",
                           "NeoGraph native bridge exceeds the JavaScript memory limit");
    }
    JSValue object = JS_NewObjectClass(context, command_value_class_id);
    if (JS_IsException(object)) {
        release_native_bytes(*capture->accounting, native_size);
        return object;
    }
    CommandValue* stored = nullptr;
    try {
        stored = new CommandValue{capture, context, native_size, std::move(command)};
    } catch (...) {
        release_native_bytes(*capture->accounting, native_size);
        JS_FreeValue(context, object);
        throw;
    }
    JS_SetOpaque(object, stored);
    for (const auto& [key, value] : encoded.items()) {
        JSValue property = json_to_js_value(context, value);
        if (JS_IsException(property)) {
            JS_FreeValue(context, object);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueStr(context, object, key.c_str(), property, JS_PROP_ENUMERABLE) <
            0) {
            JS_FreeValue(context, object);
            return JS_EXCEPTION;
        }
    }
    if (JS_PreventExtensions(context, object) < 0) {
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    return object;
}

JSValue create_call_core_impl(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || argc > 3)
        return graph_error(context, "P_JS_CONTROL_COMMAND",
                           "ng.callCore expects a Core name, optional JSON input, and source_site");
    std::string name;
    if (!read_string(context, argv[0], name, "callCore name")) return JS_EXCEPTION;
    json input = json::object();
    if (argc >= 2 && !JS_IsUndefined(argv[1]) &&
        !read_json(context, argv[1], input, "callCore input"))
        return JS_EXCEPTION;
    std::string source_site;
    if (!read_command_source_site(context, argc, argv, 2, source_site)) return JS_EXCEPTION;
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(
        context, capture,
        JavaScriptCommand::call_core(std::move(source_site), std::move(name), std::move(input)));
}

JSValue create_call_core(JSContext*    context,
                         JSValueConst  this_value,
                         int           argc,
                         JSValueConst* argv) {
    try {
        return create_call_core_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph call_core construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph call_core construction failed unexpectedly");
    }
}

JSValue create_spawn_impl(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || argc > 3)
        return graph_error(
            context, "P_JS_CONTROL_COMMAND",
            "ng.spawn expects a child binding, optional JSON input, and source_site");
    std::string binding;
    if (!read_string(context, argv[0], binding, "spawn child_binding")) return JS_EXCEPTION;
    json input = json::object();
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !read_json(context, argv[1], input, "spawn input"))
        return JS_EXCEPTION;
    std::string source_site;
    if (!read_command_source_site(context, argc, argv, 2, source_site)) return JS_EXCEPTION;
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(
        context, capture,
        JavaScriptCommand::spawn(std::move(source_site), std::move(binding), std::move(input)));
}

JSValue create_spawn(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_spawn_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph spawn construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph spawn construction failed unexpectedly");
    }
}

JSValue create_await_impl(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || argc > 3)
        return graph_error(context, "P_JS_CONTROL_COMMAND",
                           "ng.await expects a sealed command, optional timeout, and source_site");
    auto child = JavaScriptCommand::call_core(std::string(kDefaultCommandSourceSite),
                                              "__placeholder__", json::object());
    if (!read_sealed_command(context, argv[0], child)) return JS_EXCEPTION;
    std::uint64_t timeout = 0;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) &&
        !read_command_uint64(context, argv[1], timeout, "await timeout_ms", true))
        return JS_EXCEPTION;
    std::string source_site;
    if (!read_command_source_site(context, argc, argv, 2, source_site)) return JS_EXCEPTION;
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(
        context, capture,
        JavaScriptCommand::await(std::move(source_site), std::move(child), timeout));
}

JSValue create_await(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_await_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph await construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph await construction failed unexpectedly");
    }
}

struct JoinConstructorOptions {
    std::string                mode;
    std::uint64_t              required_successes = 0;
    std::uint64_t              max_in_flight      = 0;
    std::string                failure_policy;
    std::optional<std::string> source_site;
};

bool read_join_constructor_options(JSContext*              context,
                                    JSValueConst            value,
                                    std::string_view        fixed_mode,
                                    JoinConstructorOptions& output) {
    json options;
    if (!read_json(context, value, options, "join options")) return false;
    if (!options.is_object()) {
        graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph join options must be an object");
        return false;
    }
    for (const auto& [field, ignored] : options.items()) {
        (void)ignored;
        if (field != "mode" && field != "required_successes" && field != "max_in_flight" &&
            field != "failure_policy" && field != "fail_fast" && field != "collect" &&
            field != "source_site") {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "Unknown field in NeoGraph join options: " + field);
            return false;
        }
    }
    if (options.contains("mode")) {
        if (!options.at("mode").is_string() || options.at("mode").get<std::string>().empty()) {
            graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph join option mode must be a string");
            return false;
        }
        output.mode = options.at("mode").get<std::string>();
    }
    if (!fixed_mode.empty() && !output.mode.empty() && output.mode != fixed_mode) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "NeoGraph fixed join constructor mode does not match its options");
        return false;
    }
    if (options.contains("required_successes")) {
        const auto& required = options.at("required_successes");
        if (!required.is_number_unsigned() || required.get<std::uint64_t>() == 0) {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "NeoGraph join option required_successes must be positive");
            return false;
        }
        output.required_successes = required.get<std::uint64_t>();
    }
    if (options.contains("max_in_flight")) {
        const auto& max_in_flight = options.at("max_in_flight");
        if (!max_in_flight.is_number_unsigned() || max_in_flight.get<std::uint64_t>() == 0) {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "NeoGraph join option max_in_flight must be positive");
            return false;
        }
        output.max_in_flight = max_in_flight.get<std::uint64_t>();
    }
    if (options.contains("failure_policy")) {
        const auto& policy = options.at("failure_policy");
        if (!policy.is_string() || policy.get<std::string>().empty()) {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "NeoGraph join option failure_policy must be a string");
            return false;
        }
        output.failure_policy = policy.get<std::string>();
    }
    bool has_fail_fast = false;
    bool fail_fast     = false;
    if (options.contains("fail_fast")) {
        if (!options.at("fail_fast").is_boolean()) {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "NeoGraph join option fail_fast must be boolean");
            return false;
        }
        has_fail_fast = true;
        fail_fast = options.at("fail_fast").get<bool>();
    }
    bool has_collect = false;
    bool collect     = false;
    if (options.contains("collect")) {
        if (!options.at("collect").is_boolean()) {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "NeoGraph join option collect must be boolean");
            return false;
        }
        has_collect = true;
        collect = options.at("collect").get<bool>();
    }
    if (has_fail_fast && has_collect && fail_fast == collect) {
        graph_error(context, "P_JS_CONTROL_COMMAND",
                    "NeoGraph join options fail_fast and collect conflict");
        return false;
    }
    if (!output.failure_policy.empty() && (has_fail_fast || has_collect)) {
        const bool requested_fail_fast = has_fail_fast ? fail_fast : !collect;
        const auto requested_policy = requested_fail_fast ? "fail_fast" : "collect";
        if (output.failure_policy != requested_policy) {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "NeoGraph join failure policy fields conflict");
            return false;
        }
    }
    if (has_fail_fast || has_collect) {
        const bool use_fail_fast = has_fail_fast ? fail_fast : !collect;
        output.failure_policy = use_fail_fast ? "fail_fast" : "collect";
    }
    if (options.contains("source_site")) {
        if (!options.at("source_site").is_string() ||
            options.at("source_site").get<std::string>().empty()) {
            graph_error(context, "P_JS_CONTROL_COMMAND",
                        "NeoGraph join option source_site must be a nonempty string");
            return false;
        }
        output.source_site = options.at("source_site").get<std::string>();
    }
    return true;
}

JSValue create_join_impl(JSContext* context,
                         JSValueConst,
                         int              argc,
                         JSValueConst*    argv,
                         std::string_view fixed_mode = {}) {
    const int minimum = fixed_mode == "quorum" ? 2 : 1;
    const int maximum = fixed_mode.empty() ? 5 : 4;
    if (argc < minimum || argc > maximum)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph join constructor arity is invalid");
    std::vector<JavaScriptCommand> members;
    if (!read_sealed_command_array(context, argv[0], members)) return JS_EXCEPTION;

    JoinConstructorOptions options;
    options.mode = fixed_mode.empty() ? "" : std::string(fixed_mode);
    int next = 1;
    const bool options_object = next < argc && JS_IsObject(argv[next]) &&
                                !JS_IsArray(context, argv[next]);
    if (options_object) {
        if (!read_join_constructor_options(context, argv[next], fixed_mode, options))
            return JS_EXCEPTION;
        ++next;
    } else if (fixed_mode.empty() && next < argc) {
        if (!read_string(context, argv[next], options.mode, "join mode")) return JS_EXCEPTION;
        ++next;
    }
    if (fixed_mode.empty() && next < argc && JS_IsObject(argv[next]) &&
        !JS_IsArray(context, argv[next])) {
        if (!read_join_constructor_options(context, argv[next], fixed_mode, options))
            return JS_EXCEPTION;
        ++next;
    }

    if (options.mode.empty()) options.mode = fixed_mode.empty() ? "all" : std::string(fixed_mode);
    if (options.mode != "all" && options.mode != "race" && options.mode != "quorum")
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph join mode is unsupported");

    if (options.mode == "quorum" && options.required_successes == 0) {
        if (next >= argc || JS_IsObject(argv[next]) || JS_IsString(argv[next]) ||
            JS_IsUndefined(argv[next]) ||
            !read_command_uint64(context, argv[next], options.required_successes,
                                 "join required_successes", true))
            return JS_EXCEPTION;
        ++next;
    }

    if (next < argc && JS_IsObject(argv[next]) && !JS_IsArray(context, argv[next])) {
        if (!read_join_constructor_options(context, argv[next], fixed_mode, options))
            return JS_EXCEPTION;
        ++next;
    }

    std::string source_site;
    if (next < argc && JS_IsUndefined(argv[next])) ++next;
    if (next < argc) {
        if (!read_command_source_site(context, argc, argv, next, source_site))
            return JS_EXCEPTION;
        ++next;
    }
    if (next != argc)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph join constructor arguments are invalid");
    if (source_site.empty())
        source_site = options.source_site.value_or(std::string(kDefaultCommandSourceSite));

    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(
        context, capture,
        JavaScriptCommand::join(std::move(source_site), std::move(options.mode),
                                std::move(members), options.required_successes,
                                options.max_in_flight, std::move(options.failure_policy)));
}

JSValue create_join(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_join_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph join construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph join construction failed unexpectedly");
    }
}

JSValue create_all(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_join_impl(context, this_value, argc, argv, "all");
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph all construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph all construction failed unexpectedly");
    }
}

JSValue create_race(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_join_impl(context, this_value, argc, argv, "race");
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph race construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph race construction failed unexpectedly");
    }
}

JSValue create_quorum(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_join_impl(context, this_value, argc, argv, "quorum");
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph quorum construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph quorum construction failed unexpectedly");
    }
}

JSValue create_emit_impl(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || argc > 2)
        return graph_error(context, "P_JS_CONTROL_COMMAND",
                           "ng.emit expects a JSON value and optional source_site");
    json value;
    if (!read_json(context, argv[0], value, "emit value")) return JS_EXCEPTION;
    std::string source_site;
    if (!read_command_source_site(context, argc, argv, 1, source_site)) return JS_EXCEPTION;
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(context, capture,
                              JavaScriptCommand::emit(std::move(source_site), std::move(value)));
}

JSValue create_emit(JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv) {
    try {
        return create_emit_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph emit construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context, "NeoGraph emit construction failed unexpectedly");
    }
}

JSValue create_checkpoint_impl(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || argc > 2)
        return graph_error(context, "P_JS_CONTROL_COMMAND",
                           "ng.checkpoint expects a JSON value and optional source_site");
    json value;
    if (!read_json(context, argv[0], value, "checkpoint value")) return JS_EXCEPTION;
    std::string source_site;
    if (!read_command_source_site(context, argc, argv, 1, source_site)) return JS_EXCEPTION;
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(
        context, capture, JavaScriptCommand::checkpoint(std::move(source_site), std::move(value)));
}

JSValue create_checkpoint(JSContext*    context,
                          JSValueConst  this_value,
                          int           argc,
                          JSValueConst* argv) {
    try {
        return create_checkpoint_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph checkpoint construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context,
                                    "NeoGraph checkpoint construction failed unexpectedly");
    }
}

JSValue create_cancel_scope_impl(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || argc > 3)
        return graph_error(context, "P_JS_CONTROL_COMMAND",
                           "ng.cancelScope expects a scope, optional reason, and source_site");
    std::string scope;
    if (!read_string(context, argv[0], scope, "cancelScope scope")) return JS_EXCEPTION;
    std::string reason;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) &&
        !read_string(context, argv[1], reason, "cancelScope reason"))
        return JS_EXCEPTION;
    std::string source_site;
    if (!read_command_source_site(context, argc, argv, 2, source_site)) return JS_EXCEPTION;
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(context, capture,
                              JavaScriptCommand::cancel_scope(std::move(source_site),
                                                              std::move(scope), std::move(reason)));
}

JSValue create_cancel_scope(JSContext*    context,
                            JSValueConst  this_value,
                            int           argc,
                            JSValueConst* argv) {
    try {
        return create_cancel_scope_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph cancelScope construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context,
                                    "NeoGraph cancelScope construction failed unexpectedly");
    }
}

JSValue create_host_capability_impl(JSContext* context,
                                    JSValueConst,
                                    int           argc,
                                    JSValueConst* argv) {
    if (argc < 1 || argc > 3)
        return graph_error(
            context, "P_JS_CONTROL_COMMAND",
            "ng.hostCapability expects an import slot, optional JSON input, and source_site");
    std::uint64_t slot = 0;
    if (!read_command_uint64(context, argv[0], slot, "hostCapability import_slot"))
        return JS_EXCEPTION;
    if (slot > std::numeric_limits<std::uint32_t>::max())
        return graph_error(context, "P_JS_CONTROL_COMMAND",
                           "hostCapability import_slot exceeds uint32 range");
    json input = json::object();
    if (argc >= 2 && !JS_IsUndefined(argv[1]) &&
        !read_json(context, argv[1], input, "hostCapability input"))
        return JS_EXCEPTION;
    std::string source_site;
    if (!read_command_source_site(context, argc, argv, 2, source_site)) return JS_EXCEPTION;
    auto* capture = static_cast<DefinitionCapture*>(JS_GetContextOpaque(context));
    if (!capture)
        return graph_error(context, "P_JS_CONTROL_COMMAND", "NeoGraph command context is unbound");
    return make_command_value(
        context, capture,
        JavaScriptCommand::host_capability(std::move(source_site), static_cast<std::uint32_t>(slot),
                                           std::move(input)));
}

JSValue create_host_capability(JSContext*    context,
                               JSValueConst  this_value,
                               int           argc,
                               JSValueConst* argv) {
    try {
        return create_host_capability_impl(context, this_value, argc, argv);
    } catch (const std::invalid_argument& error) {
        return graph_error(context, "P_JS_CONTROL_COMMAND", bounded_utf8(error.what()));
    } catch (const std::exception& error) {
        return graph_internal_error(
            context, "NeoGraph hostCapability construction failed: " + bounded_utf8(error.what()));
    } catch (...) {
        return graph_internal_error(context,
                                    "NeoGraph hostCapability construction failed unexpectedly");
    }
}

enum AmbientFacility : int {
    AmbientDate = 1,
    AmbientEval,
    AmbientFunction,
};

const char* ambient_facility_name(int facility) noexcept {
    switch (facility) {
        case AmbientDate:
            return "Date/clock";
        case AmbientEval:
            return "eval";
        case AmbientFunction:
            return "Function";
    }
    return "ambient facility";
}

JSValue denied_ambient(JSContext* context, JSValueConst, int, JSValueConst*, int facility) {
    const auto name = ambient_facility_name(facility);
    record_failure(
        static_cast<DefinitionCapture*>(JS_GetContextOpaque(context)), "P_JS_AMBIENT_DENIED",
        std::string("QuickJS strict profile denies ambient ") + name, json{{"facility", name}});
    return JS_ThrowTypeError(context, "QuickJS strict profile denies ambient %s", name);
}

JSValue deterministic_random(JSContext* context, JSValueConst, int, JSValueConst*) {
    // Math.random() is retained as a pure convenience with a fixed result.
    // It is deliberately not backed by QuickJS' process/time-seeded state.
    return JS_NewFloat64(context, 0.0);
}

bool define_locked_global(JSContext*   context,
                          JSValueConst global,
                          const char*  name,
                          JSValue      value) {
    return JS_DefinePropertyValueStr(context, global, name, value, 0) >= 0;
}

bool deny_callable_prototype_constructor(JSContext*       context,
                                         std::string_view expression,
                                         std::string_view name) {
    JSValue callable = JS_Eval(context, expression.data(), expression.size(), "<strict-profile>",
                               JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(callable)) return false;
    JSValue prototype = JS_GetPrototype(context, callable);
    JS_FreeValue(context, callable);
    if (JS_IsException(prototype)) return false;
    JSValue denied = JS_NewCFunctionMagic(context, denied_ambient, std::string(name).c_str(), 0,
                                          JS_CFUNC_constructor_or_func_magic, AmbientFunction);
    if (JS_IsException(denied)) {
        JS_FreeValue(context, prototype);
        return false;
    }
    const int status = JS_DefinePropertyValueStr(context, prototype, "constructor", denied, 0);
    JS_FreeValue(context, prototype);
    return status >= 0;
}

bool install_strict_intrinsics(JSContext* context) {
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) return false;

    const auto make_denied = [&](const char* name, int facility, bool constructor) {
        const auto proto =
            constructor ? JS_CFUNC_constructor_or_func_magic : JS_CFUNC_generic_magic;
        return JS_NewCFunctionMagic(context, denied_ambient, name, 0, proto, facility);
    };
    const auto install_denied = [&](const char* name, int facility, bool constructor) {
        JSValue value = make_denied(name, facility, constructor);
        if (JS_IsException(value)) return false;
        return define_locked_global(context, global, name, value);
    };
    constexpr std::pair<std::string_view, std::string_view> callable_prototypes[] = {
        {"(function () {})", "Function"},
        {"(function* () {})", "GeneratorFunction"},
        {"(async function () {})", "AsyncFunction"},
        {"(async function* () {})", "AsyncGeneratorFunction"},
    };
    for (const auto& [expression, name] : callable_prototypes) {
        if (!deny_callable_prototype_constructor(context, expression, name)) {
            JS_FreeValue(context, global);
            return false;
        }
    }
    JSValue date = make_denied("Date", AmbientDate, true);
    if (JS_IsException(date) ||
        JS_DefinePropertyValueStr(context, date, "now", make_denied("now", AmbientDate, false), 0) <
            0 ||
        JS_DefinePropertyValueStr(context, date, "parse", make_denied("parse", AmbientDate, false),
                                  0) < 0 ||
        JS_DefinePropertyValueStr(context, date, "UTC", make_denied("UTC", AmbientDate, false), 0) <
            0 ||
        !define_locked_global(context, global, "Date", date) ||
        !install_denied("eval", AmbientEval, false) ||
        !install_denied("Function", AmbientFunction, true)) {
        JS_FreeValue(context, global);
        return false;
    }

    const char* denied_globals[] = {
        "Worker",
        "SharedWorker",
        "SharedArrayBuffer",
        "Atomics",
        "performance",
        "crypto",
        "process",
        "require",
        "fetch",
        "load",
        "os",
        "std",
        "setTimeout",
        "setInterval",
        "clearTimeout",
        "clearInterval",
        "queueMicrotask",
        "WebAssembly",
        "WeakRef",
        "FinalizationRegistry",
    };
    for (const auto* name : denied_globals) {
        if (!define_locked_global(context, global, name, JS_UNDEFINED)) {
            JS_FreeValue(context, global);
            return false;
        }
    }

    JSValue math = JS_GetPropertyStr(context, global, "Math");
    if (JS_IsException(math) ||
        JS_DefinePropertyValueStr(context, math, "random",
                                  JS_NewCFunction(context, deterministic_random, "random", 0),
                                  0) < 0) {
        if (!JS_IsException(math)) JS_FreeValue(context, math);
        JS_FreeValue(context, global);
        return false;
    }
    // define_locked_global consumes `math`, including when the definition
    // fails, so do not free it again on this path.
    if (!define_locked_global(context, global, "Math", math)) {
        JS_FreeValue(context, global);
        return false;
    }

    // Do not let source code replace the realm handle and then install a fake
    // global capability surface around the locked properties above.
    if (!define_locked_global(context, global, "globalThis", JS_DupValue(context, global))) {
        JS_FreeValue(context, global);
        return false;
    }
    JS_FreeValue(context, global);
    return true;
}

enum class HostContext {
    Definition,
    Program,
};

void install_host(JSContext* context, HostContext profile) {
    const bool definition_context = profile == HostContext::Definition;
    if (definition_context) ensure_graph_builder_class(JS_GetRuntime(context));
    if (!install_strict_intrinsics(context)) {
        throw JavaScriptCompileError("P_JS_RUNTIME",
                                     "QuickJS strict profile initialization failed");
    }
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
        context, host, "apiVersion",
        JS_NewUint32(context, ProgramSource::JAVASCRIPT_HOST_API_VERSION), JS_PROP_ENUMERABLE);
    if (version_status < 0) {
        JS_FreeValue(context, host);
        fail();
    }
    if (definition_context) {
        const int graph_status = JS_DefinePropertyValueStr(
            context, host, "graph", JS_NewCFunction(context, create_graph, "graph", 1),
            JS_PROP_ENUMERABLE);
        if (graph_status < 0) {
            JS_FreeValue(context, host);
            fail();
        }
    } else {
        ensure_command_value_class(JS_GetRuntime(context));
        const bool installed =
            define_method(context, host, "callCore", create_call_core, 3) &&
            define_method(context, host, "spawn", create_spawn, 3) &&
            define_method(context, host, "await", create_await, 3) &&
            define_method(context, host, "join", create_join, 4) &&
            define_method(context, host, "all", create_all, 2) &&
            define_method(context, host, "parallel", create_all, 2) &&
            define_method(context, host, "race", create_race, 2) &&
            define_method(context, host, "quorum", create_quorum, 3) &&
            define_method(context, host, "emit", create_emit, 2) &&
            define_method(context, host, "checkpoint", create_checkpoint, 2) &&
            define_method(context, host, "cancelScope", create_cancel_scope, 3) &&
            define_method(context, host, "hostCapability", create_host_capability, 3);
        if (!installed) {
            JS_FreeValue(context, host);
            fail();
        }
    }
    if (JS_PreventExtensions(context, host) < 0) {
        JS_FreeValue(context, host);
        fail();
    }
    const int host_status =
        JS_DefinePropertyValueStr(context, global, "ng", host, JS_PROP_ENUMERABLE);
    if (host_status < 0) fail();
    JS_FreeValue(context, global);
}

void validate_limits(const JavaScriptCompileLimits& limits) {
    if (limits.memory_limit_bytes == 0 || limits.max_stack_bytes == 0 ||
        limits.max_interrupt_polls == 0 || limits.max_wall_time_ms == 0 ||
        limits.max_generated_document_bytes == 0) {
        throw std::invalid_argument("JavaScript compiler limits must be positive");
    }
    if (limits.max_stack_bytes > limits.memory_limit_bytes) {
        throw std::invalid_argument("JavaScript compiler stack limit exceeds memory limit");
    }
}

const char* interrupt_reason_name(InterruptReason reason) noexcept {
    switch (reason) {
        case InterruptReason::PollLimit:
            return "interrupt_polls";
        case InterruptReason::WallTime:
            return "wall_time_ms";
        case InterruptReason::Cancelled:
            return "cancelled";
        case InterruptReason::None:
            break;
    }
    return "none";
}

json resource_witness(const JavaScriptCompileLimits& limits,
                      const InterruptBudget&         budget,
                      const AllocationAccounting*    accounting = nullptr) {
    json witness{{"engine", "quickjs"},
                 {"memory_limit_bytes", limits.memory_limit_bytes},
                 {"max_stack_bytes", limits.max_stack_bytes},
                 {"max_interrupt_polls", limits.max_interrupt_polls},
                 {"interrupt_polls", budget.polls},
                 {"max_wall_time_ms", limits.max_wall_time_ms},
                 {"max_generated_document_bytes", limits.max_generated_document_bytes},
                 {"interrupt_reason", interrupt_reason_name(budget.reason)}};
    if (accounting) {
        witness["allocator_current_bytes"]      = accounting->current_bytes;
        witness["allocator_peak_bytes"]         = accounting->peak_bytes;
        witness["allocator_denied_count"]       = accounting->denied_count;
        witness["allocator_limit_denied_count"] = accounting->limit_denied_count;
        witness["allocator_total_bytes"]        = accounting->total_bytes;
        witness["allocator_allocations"]        = accounting->allocation_count;
        witness["native_current_bytes"]         = accounting->native_bytes;
        witness["native_peak_bytes"]            = accounting->native_peak_bytes;
        witness["combined_peak_bytes"]          = accounting->combined_peak_bytes;
    }
    return witness;
}

[[noreturn]] void throw_captured_failure(const DefinitionCapture&  capture,
                                         std::optional<SourceSpan> source_span = std::nullopt) {
    json witness = capture.failure_witness;
    if (!witness.is_object()) witness = json::object();
    witness["engine"] = "quickjs";
    if (capture.native_builder_bytes != 0)
        witness["native_builder_bytes"] = capture.native_builder_bytes;
    throw JavaScriptCompileError(capture.failure_code, capture.failure_message, std::move(witness),
                                 std::move(source_span));
}

[[noreturn]] void throw_evaluation_failure(JSContext*                     context,
                                           const DefinitionCapture&       capture,
                                           InterruptBudget&               budget,
                                           const JavaScriptCompileLimits& limits,
                                           const AllocationAccounting*    accounting = nullptr,
                                           std::string_view               source     = {},
                                           std::string                    message    = {},
                                           std::optional<SourceSpan> source_span = std::nullopt) {
    refresh_external_interrupt(budget);
    if (budget.reason == InterruptReason::Cancelled) {
        throw JavaScriptCompileError("P_JS_CANCELLED", "JavaScript evaluation was cancelled",
                                     resource_witness(limits, budget, accounting));
    }
    if (budget.reason == InterruptReason::WallTime) {
        throw JavaScriptCompileError("P_JS_TIMEOUT",
                                     "JavaScript evaluation exceeded its wall-time ceiling",
                                     resource_witness(limits, budget, accounting));
    }
    if (message.empty()) {
        if (source.empty()) {
            message = exception_text(context);
        } else {
            auto details = take_exception_details(context, source);
            message      = std::move(details.message);
            source_span  = std::move(details.source_span);
        }
    }
    if (!capture.failure_code.empty()) {
        throw_captured_failure(capture, std::move(source_span));
    }
    const bool allocator_limit_exhausted = accounting && accounting->limit_denied_count != 0;
    if (budget.reason == InterruptReason::PollLimit || allocator_limit_exhausted ||
        looks_like_resource_exhaustion(message)) {
        throw JavaScriptCompileError(
            "P_JS_RESOURCE_LIMIT", "JavaScript evaluation exceeded a configured resource limit",
            resource_witness(limits, budget, accounting), std::move(source_span));
    }
    throw JavaScriptCompileError("P_JS_EVALUATION", "JavaScript evaluation failed: " + message,
                                 resource_witness(limits, budget, accounting),
                                 std::move(source_span));
}

json projected_program_document(const json& definition) {
    return json{
        {"program_schema_version", ProgramCompiler::PROGRAM_SCHEMA_VERSION},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", definition.at("name")}, {"definition", definition}}},
        {"declared_budget_requirements",
         json::array({
             json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 60000}},
             json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000}},
             json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000000}},
             json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 4}},
             json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 32}},
             json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 100}},
             json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
             json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 4}},
             json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 4}},
         })},
    };
}

}  // namespace
#endif

JavaScriptSourceEvaluation evaluate_javascript_source(const ProgramSource&           source,
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
    reject_unsealed_module_syntax(script);

    const auto        started_at = std::chrono::steady_clock::now();
    QuickJsScope      scope(limits);
    DefinitionCapture capture;
    capture.max_native_builder_bytes = limits.max_generated_document_bytes;
    capture.accounting               = &scope.accounting();
    JS_SetContextOpaque(scope.context(), &capture);
    try {
        install_host(scope.context(), HostContext::Definition);
        InterruptBudget budget;
        budget.limit    = limits.max_interrupt_polls;
        budget.deadline = started_at + std::chrono::milliseconds(limits.max_wall_time_ms);
        JS_SetInterruptHandler(scope.runtime(), interrupt_after_budget, &budget);
        refresh_external_interrupt(budget);
        if (budget.reason != InterruptReason::None)
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script);

        JSValue compiled = JS_Eval(scope.context(), script.data(), script.size(), "<program>",
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script);
        }
        auto*   module        = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JSValue module_result = JS_EvalFunction(scope.context(), compiled);
        if (JS_IsException(module_result)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script);
        }
        const auto module_state = JS_PromiseState(scope.context(), module_result);
        if (module_state == JS_PROMISE_PENDING) {
            JS_FreeValue(scope.context(), module_result);
            throw JavaScriptCompileError(
                "P_JS_DEFINE_ASYNC",
                "JavaScript definition modules must complete synchronously without top-level await",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "define"));
        }
        if (module_state == JS_PROMISE_REJECTED) {
            JSValue     rejection = JS_PromiseResult(scope.context(), module_result);
            const auto  details   = exception_details(scope.context(), rejection, script);
            const auto& message   = details.message;
            JS_FreeValue(scope.context(), rejection);
            JS_FreeValue(scope.context(), module_result);
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script, "JavaScript definition module failed: " + message,
                                     details.source_span);
        }
        if (module_state != JS_PROMISE_FULFILLED) {
            JS_FreeValue(scope.context(), module_result);
            throw JavaScriptCompileError("P_JS_RUNTIME",
                                         "QuickJS module evaluation did not return a promise",
                                         json{{"engine", "quickjs"}});
        }
        JS_FreeValue(scope.context(), module_result);
        if (!capture.failure_code.empty()) throw_captured_failure(capture);

        JSValue module_namespace = JS_GetModuleNamespace(scope.context(), module);
        if (JS_IsException(module_namespace)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script);
        }
        JSValue define = JS_GetPropertyStr(scope.context(), module_namespace, "define");
        if (JS_IsException(define)) {
            JS_FreeValue(scope.context(), module_namespace);
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script);
        }
        if (!JS_IsFunction(scope.context(), define)) {
            JS_FreeValue(scope.context(), define);
            JS_FreeValue(scope.context(), module_namespace);
            throw JavaScriptCompileError(
                "P_JS_DEFINE_MISSING",
                "JavaScript source must export a synchronous define() function",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "define"));
        }
        JSValue main = JS_GetPropertyStr(scope.context(), module_namespace, "main");
        if (JS_IsException(main)) {
            JS_FreeValue(scope.context(), define);
            JS_FreeValue(scope.context(), module_namespace);
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script);
        }
        const bool has_control_generator = !JS_IsUndefined(main);
        if (has_control_generator && !JS_IsFunction(scope.context(), main)) {
            JS_FreeValue(scope.context(), main);
            JS_FreeValue(scope.context(), define);
            JS_FreeValue(scope.context(), module_namespace);
            throw JavaScriptCompileError(
                "P_JS_CONTROL_MAIN",
                "JavaScript control entry main must be a function when exported",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "main"));
        }
        JS_FreeValue(scope.context(), main);
        JSValue builder = JS_Call(scope.context(), define, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(scope.context(), define);
        JS_FreeValue(scope.context(), module_namespace);
        if (JS_IsException(builder)) {
            throw_evaluation_failure(scope.context(), capture, budget, limits, &scope.accounting(),
                                     script);
        }
        if (!capture.failure_code.empty()) {
            JS_FreeValue(scope.context(), builder);
            throw_captured_failure(capture, source_span_for_token(script, "define"));
        }
        auto* graph = static_cast<GraphBuilder*>(JS_GetOpaque(builder, graph_builder_class_id));
        if (!graph || graph->context != scope.context() || graph->capture != &capture ||
            graph->sealed) {
            JS_FreeValue(scope.context(), builder);
            throw JavaScriptCompileError(
                "P_JS_DEFINE_VALUE",
                "JavaScript define() must return exactly one open NeoGraph graph builder",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "define"));
        }
        graph->sealed   = true;
        auto definition = detail::owned_json_copy(graph->definition);
        JS_FreeValue(scope.context(), builder);
        const auto canonical_definition = detail::canonical_json_bytes(definition);
        if (canonical_definition.size() > limits.max_generated_document_bytes) {
            throw JavaScriptCompileError(
                "P_JS_GRAPH_LIMIT",
                "JavaScript graph builder generated a definition exceeding its native byte limit",
                json{{"bytes", canonical_definition.size()},
                     {"limit", limits.max_generated_document_bytes},
                     {"engine", "quickjs"}});
        }
        JS_SetContextOpaque(scope.context(), nullptr);
        return JavaScriptSourceEvaluation{projected_program_document(definition),
                                          has_control_generator};
    } catch (...) {
        JS_SetContextOpaque(scope.context(), nullptr);
        throw;
    }
#endif
}

struct JavaScriptGenerator::Impl {
#if defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
    explicit Impl(const JavaScriptCompileLimits&                       limits,
                  std::string                                          source,
                  std::function<bool()>                                cancellation_requested,
                  std::optional<std::chrono::steady_clock::time_point> deadline)
        : configured_limits(limits), source_text(std::move(source)), scope(configured_limits) {
        capture.max_native_builder_bytes = configured_limits.max_generated_document_bytes;
        capture.accounting               = &scope.accounting();
        budget.limit                     = configured_limits.max_interrupt_polls;
        budget.deadline =
            deadline.value_or(std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(configured_limits.max_wall_time_ms));
        budget.cancellation_requested = std::move(cancellation_requested);
    }

    ~Impl() {
        JS_SetContextOpaque(scope.context(), nullptr);
        JS_FreeValue(scope.context(), iterator);
    }

    JavaScriptCompileLimits configured_limits;
    std::string             source_text;
    QuickJsScope            scope;
    DefinitionCapture       capture;
    InterruptBudget         budget;
    JSValue                 iterator = JS_UNDEFINED;
    bool                    finished = false;
#else
    explicit Impl(const JavaScriptCompileLimits&,
                  std::string,
                  std::function<bool()>,
                  std::optional<std::chrono::steady_clock::time_point>) {}
#endif
};

JavaScriptGenerator::JavaScriptGenerator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
JavaScriptGenerator::JavaScriptGenerator(JavaScriptGenerator&&) noexcept            = default;
JavaScriptGenerator& JavaScriptGenerator::operator=(JavaScriptGenerator&&) noexcept = default;
JavaScriptGenerator::~JavaScriptGenerator()                                         = default;

#if defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
namespace {

JSValue json_to_js_value(JSContext*                     context,
                         const json&                    value,
                         const JavaScriptCompileLimits& limits) {
    const auto canonical = detail::canonical_json_bytes(value);
    if (canonical.size() > limits.max_generated_document_bytes) {
        JS_ThrowRangeError(context, "JavaScript control value exceeds its native byte limit");
        return JS_EXCEPTION;
    }
    return JS_ParseJSON(context, canonical.c_str(), canonical.size(), "<program-control>");
}

[[noreturn]] void throw_control_failure(JSContext*                     context,
                                        const DefinitionCapture&       capture,
                                        InterruptBudget&               budget,
                                        const JavaScriptCompileLimits& limits,
                                        const AllocationAccounting*    accounting = nullptr,
                                        std::string_view               source     = {}) {
    throw_evaluation_failure(context, capture, budget, limits, accounting, source);
}

[[noreturn]] void throw_control_value_error(std::string message,
                                            std::string code = "P_JS_CONTROL_VALUE") {
    throw JavaScriptCompileError(std::move(code), std::move(message), json{{"engine", "quickjs"}});
}

[[noreturn]] void throw_control_value_error(std::string               message,
                                            std::optional<SourceSpan> source_span) {
    throw JavaScriptCompileError("P_JS_CONTROL_VALUE", std::move(message),
                                 json{{"engine", "quickjs"}}, std::move(source_span));
}

JavaScriptGeneratorStep decode_generator_step(JavaScriptGenerator::Impl&     impl,
                                              JSValue                        result,
                                              const JavaScriptCompileLimits& limits) {
    auto* context = impl.scope.context();
    if (!JS_IsObject(result)) {
        JS_FreeValue(context, result);
        throw_control_value_error("JavaScript control main().next() must return an iterator result",
                                  source_span_for_token(impl.source_text, "main"));
    }

    JSValue done = JS_GetPropertyStr(context, result, "done");
    if (JS_IsException(done)) {
        JS_FreeValue(context, result);
        throw_control_failure(context, impl.capture, impl.budget, limits, &impl.scope.accounting(),
                              impl.source_text);
    }
    if (!JS_IsBool(done)) {
        JS_FreeValue(context, done);
        JS_FreeValue(context, result);
        throw_control_value_error("JavaScript control iterator result.done must be boolean",
                                  source_span_for_token(impl.source_text, "main"));
    }
    const bool is_done = JS_ToBool(context, done) != 0;
    JS_FreeValue(context, done);

    JSValue value = JS_GetPropertyStr(context, result, "value");
    if (JS_IsException(value)) {
        JS_FreeValue(context, result);
        throw_control_failure(context, impl.capture, impl.budget, limits, &impl.scope.accounting(),
                              impl.source_text);
    }
    if (JS_IsUndefined(value)) {
        JS_FreeValue(context, value);
        JS_FreeValue(context, result);
        throw_control_value_error(
            "JavaScript control iterator result.value must be canonical JSON, not undefined",
            source_span_for_token(impl.source_text, "yield"));
    }

    json           converted;
    JsonConversion conversion;
    conversion.byte_limit = impl.capture.max_native_builder_bytes;
    std::optional<JavaScriptCommand> sealed_command;
    auto*                            command_value = static_cast<CommandValue*>(
        JS_IsObject(value) ? JS_GetOpaque(value, command_value_class_id) : nullptr);
    bool converted_ok = false;
    if (command_value) {
        if (command_value->context != context ||
            command_value->capture != JS_GetContextOpaque(context)) {
            JS_FreeValue(context, value);
            JS_FreeValue(context, result);
            throw_control_value_error("JavaScript control yielded a forged command value");
        }
        converted = command_value->command.to_json();
        if (!is_done) sealed_command = command_value->command;
        converted_ok = true;
    } else {
        converted_ok = js_to_json(context, value, converted, conversion, 0);
    }
    JS_FreeValue(context, value);
    JS_FreeValue(context, result);
    if (!converted_ok) {
        if (conversion.error.empty()) {
            throw_control_failure(context, impl.capture, impl.budget, limits,
                                  &impl.scope.accounting(), impl.source_text);
        }
        if (conversion.error == "graph configuration exceeds the native byte limit")
            throw_control_value_error(
                "JavaScript control iterator result.value exceeds its native byte limit",
                "P_JS_RESOURCE_LIMIT");
        throw_control_value_error(
            "JavaScript control iterator result.value is not canonical JSON: " + conversion.error,
            source_span_for_token(impl.source_text, "yield"));
    }
    return JavaScriptGeneratorStep{is_done, std::move(converted), std::move(sealed_command)};
}

}  // namespace
#endif

std::optional<JavaScriptGenerator> JavaScriptGenerator::open(
    const ProgramSource&                                 source,
    json                                                 input,
    const JavaScriptCompileLimits&                       limits,
    std::function<bool()>                                cancellation_requested,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (source.kind() != SourceKind::JavaScript) {
        throw std::invalid_argument("JavaScript generator received a non-JavaScript ProgramSource");
    }
#if !defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
    (void)input;
    (void)limits;
    (void)cancellation_requested;
    (void)deadline;
    throw JavaScriptCompileError(
        "P_JS_UNAVAILABLE", "JavaScript Program sources require NEOGRAPH_BUILD_QUICKJS_CONTROL=ON");
#else
    validate_limits(limits);
    const auto envelope = source.document();
    const auto script   = envelope.at("source").get<std::string>();
    reject_unsealed_module_syntax(script);

    auto impl = std::make_unique<Impl>(limits, script, std::move(cancellation_requested), deadline);
    auto* context = impl->scope.context();
    JS_SetContextOpaque(context, &impl->capture);
    try {
        install_host(context, HostContext::Program);
        JS_SetInterruptHandler(impl->scope.runtime(), interrupt_after_budget, &impl->budget);
        refresh_external_interrupt(impl->budget);
        if (impl->budget.reason != InterruptReason::None)
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);

        JSValue compiled = JS_Eval(context, script.data(), script.size(), "<program-control>",
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);
        }
        auto*   module        = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JSValue module_result = JS_EvalFunction(context, compiled);
        if (JS_IsException(module_result)) {
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);
        }
        const auto module_state = JS_PromiseState(context, module_result);
        if (module_state == JS_PROMISE_PENDING) {
            JS_FreeValue(context, module_result);
            throw JavaScriptCompileError(
                "P_JS_CONTROL_ASYNC",
                "JavaScript control modules must complete synchronously without top-level await",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "main"));
        }
        if (module_state == JS_PROMISE_REJECTED) {
            JSValue     rejection = JS_PromiseResult(context, module_result);
            const auto  details   = exception_details(context, rejection, script);
            const auto& message   = details.message;
            JS_FreeValue(context, rejection);
            JS_FreeValue(context, module_result);
            throw_evaluation_failure(context, impl->capture, impl->budget, limits,
                                     &impl->scope.accounting(), impl->source_text,
                                     "JavaScript control module failed: " + message,
                                     details.source_span);
        }
        if (module_state != JS_PROMISE_FULFILLED) {
            JS_FreeValue(context, module_result);
            throw JavaScriptCompileError(
                "P_JS_RUNTIME", "QuickJS control module evaluation did not return a promise",
                json{{"engine", "quickjs"}});
        }
        JS_FreeValue(context, module_result);
        if (!impl->capture.failure_code.empty()) throw_captured_failure(impl->capture);

        JSValue module_namespace = JS_GetModuleNamespace(context, module);
        if (JS_IsException(module_namespace)) {
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);
        }
        JSValue main = JS_GetPropertyStr(context, module_namespace, "main");
        JS_FreeValue(context, module_namespace);
        if (JS_IsException(main)) {
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);
        }
        if (JS_IsUndefined(main)) {
            JS_FreeValue(context, main);
            return std::nullopt;
        }
        if (!JS_IsFunction(context, main)) {
            JS_FreeValue(context, main);
            throw JavaScriptCompileError(
                "P_JS_CONTROL_MAIN", "JavaScript control entry main must be a function",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "main"));
        }

        JSValue input_value = json_to_js_value(context, input, limits);
        if (JS_IsException(input_value)) {
            JS_FreeValue(context, main);
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);
        }
        JSValue iterator = JS_Call(context, main, JS_UNDEFINED, 1, &input_value);
        JS_FreeValue(context, input_value);
        JS_FreeValue(context, main);
        if (JS_IsException(iterator)) {
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);
        }
        if (!JS_IsObject(iterator)) {
            JS_FreeValue(context, iterator);
            throw JavaScriptCompileError(
                "P_JS_CONTROL_MAIN",
                "JavaScript control main(input) must return a synchronous generator",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "main"));
        }
        JSValue next = JS_GetPropertyStr(context, iterator, "next");
        if (JS_IsException(next)) {
            JS_FreeValue(context, iterator);
            throw_control_failure(context, impl->capture, impl->budget, limits,
                                  &impl->scope.accounting(), impl->source_text);
        }
        const bool is_generator = JS_IsFunction(context, next);
        JS_FreeValue(context, next);
        if (!is_generator) {
            JS_FreeValue(context, iterator);
            throw JavaScriptCompileError(
                "P_JS_CONTROL_MAIN",
                "JavaScript control main(input) must return a synchronous generator",
                json{{"engine", "quickjs"}}, source_span_for_token(script, "main"));
        }
        impl->iterator = iterator;
        JavaScriptGenerator generator(std::move(impl));
        return std::optional<JavaScriptGenerator>(std::move(generator));
    } catch (...) {
        JS_SetContextOpaque(context, nullptr);
        throw;
    }
#endif
}

JavaScriptGeneratorStep JavaScriptGenerator::next(std::optional<json> response) {
    if (!impl_) throw std::logic_error("JavaScript generator is not initialized");
#if !defined(NEOGRAPH_PROGRAM_HAS_QUICKJS)
    (void)response;
    throw JavaScriptCompileError(
        "P_JS_UNAVAILABLE", "JavaScript Program sources require NEOGRAPH_BUILD_QUICKJS_CONTROL=ON");
#else
    if (impl_->finished) {
        throw JavaScriptCompileError("P_JS_CONTROL_FINISHED",
                                     "JavaScript control generator is already complete",
                                     json{{"engine", "quickjs"}});
    }
    const auto& limits  = impl_->configured_limits;
    auto*       context = impl_->scope.context();
    // A Program scheduler may resume this serialized generator on a different
    // worker after a C++ join. QuickJS tracks stack bounds per runtime, so
    // refresh its stack top at each turn while keeping JS execution serialized.
    JS_UpdateStackTop(impl_->scope.runtime());
    refresh_external_interrupt(impl_->budget);
    if (impl_->budget.reason != InterruptReason::None)
        throw_control_failure(context, impl_->capture, impl_->budget, limits,
                              &impl_->scope.accounting(), impl_->source_text);
    JSValue next = JS_GetPropertyStr(context, impl_->iterator, "next");
    if (JS_IsException(next)) {
        throw_control_failure(context, impl_->capture, impl_->budget, limits,
                              &impl_->scope.accounting(), impl_->source_text);
    }
    if (!JS_IsFunction(context, next)) {
        JS_FreeValue(context, next);
        throw JavaScriptCompileError("P_JS_CONTROL_MAIN",
                                     "JavaScript control iterator no longer has next()",
                                     json{{"engine", "quickjs"}});
    }
    JSValue response_value = JS_UNDEFINED;
    int     argc           = 0;
    if (response) {
        response_value = json_to_js_value(context, *response, limits);
        if (JS_IsException(response_value)) {
            JS_FreeValue(context, next);
            throw_control_failure(context, impl_->capture, impl_->budget, limits,
                                  &impl_->scope.accounting(), impl_->source_text);
        }
        argc = 1;
    }
    JSValue result =
        JS_Call(context, next, impl_->iterator, argc, response ? &response_value : nullptr);
    if (response) JS_FreeValue(context, response_value);
    JS_FreeValue(context, next);
    if (JS_IsException(result)) {
        throw_control_failure(context, impl_->capture, impl_->budget, limits,
                              &impl_->scope.accounting(), impl_->source_text);
    }
    auto step       = decode_generator_step(*impl_, result, limits);
    impl_->finished = step.done;
    return step;
#endif
}
}  // namespace neograph::program::detail
