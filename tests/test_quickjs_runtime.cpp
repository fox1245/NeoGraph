#include <gtest/gtest.h>
#include <quickjs.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace {

class Runtime final {
public:
    Runtime() : runtime_(JS_NewRuntime()), context_(runtime_ ? JS_NewContext(runtime_) : nullptr) {}
    Runtime(const JSMallocFunctions& allocator, void* opaque)
        : runtime_(JS_NewRuntime2(&allocator, opaque)),
          context_(runtime_ ? JS_NewContext(runtime_) : nullptr) {}

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    ~Runtime() {
        if (context_) JS_FreeContext(context_);
        if (runtime_) JS_FreeRuntime(runtime_);
    }

    JSRuntime* runtime() const noexcept { return runtime_; }
    JSContext* context() const noexcept { return context_; }

private:
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
};

JSValue evaluate(JSContext* context, std::string_view source, int flags = JS_EVAL_TYPE_GLOBAL) {
    return JS_Eval(context, source.data(), source.size(), "neograph-quickjs-test.js", flags);
}

std::string exception_message(JSContext* context) {
    JSValue     exception = JS_GetException(context);
    const char* text      = JS_ToCString(context, exception);
    std::string result    = text ? text : "<unprintable exception>";
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, exception);
    return result;
}

std::string evaluated_string(JSContext* context, std::string_view source) {
    JSValue result = evaluate(context, source);
    if (JS_IsException(result)) {
        const auto message = exception_message(context);
        JS_FreeValue(context, result);
        ADD_FAILURE() << message;
        return {};
    }
    const char* text  = JS_ToCString(context, result);
    std::string value = text ? text : "";
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, result);
    return value;
}

JSValue native_add(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 2) return JS_ThrowTypeError(context, "nativeAdd expects exactly two arguments");
    std::int32_t left  = 0;
    std::int32_t right = 0;
    if (JS_ToInt32(context, &left, argv[0]) < 0 || JS_ToInt32(context, &right, argv[1]) < 0) {
        return JS_EXCEPTION;
    }
    return JS_NewInt32(context, left + right);
}

struct InterruptBudget {
    std::uint64_t           polls                  = 0;
    std::uint64_t           limit                  = 0;
    const std::atomic_bool* cancellation_requested = nullptr;
};

int interrupt_after_budget(JSRuntime*, void* opaque) {
    auto& budget = *static_cast<InterruptBudget*>(opaque);
    if (budget.cancellation_requested &&
        budget.cancellation_requested->load(std::memory_order_acquire)) {
        return 1;
    }
    ++budget.polls;
    return budget.polls >= budget.limit;
}

union AllocationHeader {
    std::max_align_t alignment;
    std::size_t      size;
};

struct AllocationStats {
    std::size_t allocations = 0;
    std::size_t bytes       = 0;
};

void* accounting_malloc(JSMallocState* state, std::size_t size) {
    if (size == 0 || size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
        return nullptr;
    }
    if (size > state->malloc_limit || state->malloc_size > state->malloc_limit - size) {
        return nullptr;
    }
    auto* allocation = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
    if (!allocation) return nullptr;
    allocation->size = size;
    ++state->malloc_count;
    state->malloc_size += size;
    if (auto* stats = static_cast<AllocationStats*>(state->opaque)) {
        ++stats->allocations;
        stats->bytes += size;
    }
    return allocation + 1;
}

void accounting_free(JSMallocState* state, void* pointer) {
    if (!pointer) return;
    auto* allocation = static_cast<AllocationHeader*>(pointer) - 1;
    --state->malloc_count;
    state->malloc_size -= allocation->size;
    if (auto* stats = static_cast<AllocationStats*>(state->opaque)) {
        --stats->allocations;
        stats->bytes -= allocation->size;
    }
    std::free(allocation);
}

void* accounting_realloc(JSMallocState* state, void* pointer, std::size_t size) {
    if (!pointer) return accounting_malloc(state, size);

    auto*      allocation = static_cast<AllocationHeader*>(pointer) - 1;
    const auto old_size   = allocation->size;
    if (size == 0) {
        accounting_free(state, pointer);
        return nullptr;
    }
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) ||
        size > state->malloc_limit || state->malloc_size - old_size > state->malloc_limit - size) {
        return nullptr;
    }

    auto* resized =
        static_cast<AllocationHeader*>(std::realloc(allocation, sizeof(AllocationHeader) + size));
    if (!resized) return nullptr;
    resized->size      = size;
    state->malloc_size = state->malloc_size - old_size + size;
    if (auto* stats = static_cast<AllocationStats*>(state->opaque)) {
        stats->bytes = stats->bytes - old_size + size;
    }
    return resized + 1;
}

std::size_t accounting_usable_size(const void* pointer) {
    if (!pointer) return 0;
    return (static_cast<const AllocationHeader*>(pointer) - 1)->size;
}
#if defined(_MSC_VER)
JSValue msvc_generic_magic(JSContext*, JSValueConst, int, JSValueConst*, int) {
    return JS_UNDEFINED;
}

double msvc_unary(double value) {
    return value;
}

double msvc_binary(double left, double right) {
    return left + right;
}

JSValue msvc_getter(JSContext*, JSValueConst) {
    return JS_UNDEFINED;
}

JSValue msvc_setter(JSContext*, JSValueConst, JSValueConst) {
    return JS_UNDEFINED;
}

JSValue msvc_getter_magic(JSContext*, JSValueConst, int) {
    return JS_UNDEFINED;
}

JSValue msvc_setter_magic(JSContext*, JSValueConst, JSValueConst, int) {
    return JS_UNDEFINED;
}

JSValue msvc_iterator_next(JSContext*, JSValueConst, int, JSValueConst*, int*, int) {
    return JS_UNDEFINED;
}

const JSCFunctionListEntry msvc_nested_entries[]        = {JS_CFUNC_DEF("nested", 0, native_add)};
const JSCFunctionListEntry msvc_function_list_entries[] = {
    JS_CFUNC_DEF("generic", 1, native_add),
    JS_CFUNC_MAGIC_DEF("magic", 2, msvc_generic_magic, 3),
    JS_CFUNC_SPECIAL_DEF("unary", 1, f_f, msvc_unary),
    JS_CFUNC_SPECIAL_DEF("binary", 2, f_f_f, msvc_binary),
    JS_ITERATOR_NEXT_DEF("next", 1, msvc_iterator_next, 4),
    JS_CGETSET_DEF("property", msvc_getter, msvc_setter),
    JS_CGETSET_MAGIC_DEF("magicProperty", msvc_getter_magic, msvc_setter_magic, 5),
    JS_PROP_STRING_DEF("string", "value", JS_PROP_CONFIGURABLE),
    JS_PROP_INT32_DEF("int32", 7, JS_PROP_WRITABLE),
    JS_PROP_INT64_DEF("int64", 8, JS_PROP_WRITABLE),
    JS_PROP_DOUBLE_DEF("double", 9.0, JS_PROP_WRITABLE),
    JS_PROP_UNDEFINED_DEF("undefined", JS_PROP_WRITABLE),
    JS_PROP_ATOM_DEF("atom", 10, JS_PROP_WRITABLE),
    JS_PROP_BOOL_DEF("bool", 1, JS_PROP_WRITABLE),
    JS_OBJECT_DEF("object", msvc_nested_entries, 1, JS_PROP_WRITABLE),
    JS_ALIAS_DEF("alias", "generic"),
    JS_ALIAS_BASE_DEF("baseAlias", "generic", 3),
};
#endif

TEST(QuickJsRuntimeTest, BindsOnlyExplicitNativeFunctions) {
    Runtime runtime;
    ASSERT_NE(runtime.runtime(), nullptr);
    ASSERT_NE(runtime.context(), nullptr);

    JSValue global = JS_GetGlobalObject(runtime.context());
    ASSERT_FALSE(JS_IsException(global));
    ASSERT_EQ(JS_SetPropertyStr(runtime.context(), global, "nativeAdd",
                                JS_NewCFunction(runtime.context(), native_add, "nativeAdd", 2)),
              1);
    JS_FreeValue(runtime.context(), global);

    EXPECT_EQ(evaluated_string(runtime.context(), "String(nativeAdd(40, 2))"), "42");
    EXPECT_EQ(evaluated_string(runtime.context(),
                               "[typeof std, typeof os, typeof process, typeof require, "
                               " typeof fetch, typeof Worker, typeof load].join(',')"),
              "undefined,undefined,undefined,undefined,undefined,undefined,undefined");
}

TEST(QuickJsRuntimeTest, RejectsUnregisteredSystemModule) {
    Runtime runtime;
    ASSERT_NE(runtime.context(), nullptr);

    JSValue result = evaluate(runtime.context(), "import * as os from 'os';", JS_EVAL_TYPE_MODULE);
    EXPECT_TRUE(JS_IsException(result));
    JS_FreeValue(runtime.context(), result);
    EXPECT_FALSE(exception_message(runtime.context()).empty());
}

TEST(QuickJsRuntimeTest, InterruptsInfiniteEvaluation) {
    Runtime runtime;
    ASSERT_NE(runtime.runtime(), nullptr);
    ASSERT_NE(runtime.context(), nullptr);

    InterruptBudget budget{0, 8};
    JS_SetInterruptHandler(runtime.runtime(), interrupt_after_budget, &budget);
    JSValue result = evaluate(runtime.context(), "for (;;) {};");
    EXPECT_TRUE(JS_IsException(result));
    JS_FreeValue(runtime.context(), result);
    EXPECT_GE(budget.polls, budget.limit);
    EXPECT_FALSE(exception_message(runtime.context()).empty());
}

TEST(QuickJsRuntimeTest, InterruptsEvaluationWhenCancellationWasRequested) {
    Runtime runtime;
    ASSERT_NE(runtime.runtime(), nullptr);
    ASSERT_NE(runtime.context(), nullptr);

    std::atomic_bool cancellation_requested{true};
    InterruptBudget  budget{0, std::numeric_limits<std::uint64_t>::max(), &cancellation_requested};
    JS_SetInterruptHandler(runtime.runtime(), interrupt_after_budget, &budget);
    JSValue result = evaluate(runtime.context(), "for (;;) {};");
    EXPECT_TRUE(JS_IsException(result));
    JS_FreeValue(runtime.context(), result);
    EXPECT_EQ(budget.polls, 0U);
    EXPECT_FALSE(exception_message(runtime.context()).empty());
}

TEST(QuickJsRuntimeTest, EnforcesRuntimeMemoryAndStackLimits) {
    const JSMallocFunctions allocator{
        accounting_malloc,
        accounting_free,
        accounting_realloc,
        accounting_usable_size,
    };
    AllocationStats stats;
    Runtime         runtime(allocator, &stats);
    ASSERT_NE(runtime.runtime(), nullptr);
    ASSERT_NE(runtime.context(), nullptr);

    JS_SetMemoryLimit(runtime.runtime(), 256 * 1024);
    JSValue allocation = evaluate(runtime.context(), "new Uint8Array(1024 * 1024)");
    EXPECT_TRUE(JS_IsException(allocation));
    JS_FreeValue(runtime.context(), allocation);
    EXPECT_FALSE(exception_message(runtime.context()).empty());

    JS_SetMemoryLimit(runtime.runtime(), 8 * 1024 * 1024);
    JS_SetMaxStackSize(runtime.runtime(), 64 * 1024);
    JSValue recursion =
        evaluate(runtime.context(), "function recurse() { return recurse(); } recurse();");
    EXPECT_TRUE(JS_IsException(recursion));
    JS_FreeValue(runtime.context(), recursion);
    EXPECT_FALSE(exception_message(runtime.context()).empty());
}

TEST(QuickJsRuntimeTest, SupportsAccountedCustomAllocator) {
    const JSMallocFunctions allocator{
        accounting_malloc,
        accounting_free,
        accounting_realloc,
        accounting_usable_size,
    };
    AllocationStats stats;
    JSRuntime*      runtime = JS_NewRuntime2(&allocator, &stats);
    ASSERT_NE(runtime, nullptr);

    JSContext* context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        FAIL() << "QuickJS could not create a context with the custom allocator";
    }
    EXPECT_EQ(evaluated_string(context, "String(6 * 7)"), "42");

    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    EXPECT_EQ(stats.allocations, 0U);
    EXPECT_EQ(stats.bytes, 0U);
}

TEST(QuickJsRuntimeTest, ExceptionResourceKindDistinguishesEngineLimitsFromThrownErrors) {
    Runtime engine;
    ASSERT_NE(engine.runtime(), nullptr);
    ASSERT_NE(engine.context(), nullptr);
    JSRuntime* runtime = engine.runtime();
    JSContext* context = engine.context();
    JS_SetMaxStackSize(runtime, 32U * 1024U);

    EXPECT_EQ(JS_GetPendingExceptionResourceKind(context), JS_EXCEPTION_RESOURCE_NONE);

    // Evaluates a script that must fail and checks why the engine threw its exception: while it is pending, and
    // as the value that JS_GetException() takes.
    const auto expect_failure_kind = [&](std::string_view source, JSExceptionResourceKindEnum expected) {
        SCOPED_TRACE(std::string(source));
        JSValue result = evaluate(context, source);
        EXPECT_TRUE(JS_IsException(result));
        EXPECT_EQ(JS_GetPendingExceptionResourceKind(context), expected);
        JSValue exception = JS_GetException(context);
        EXPECT_EQ(JS_GetPendingExceptionResourceKind(context), JS_EXCEPTION_RESOURCE_NONE);
        EXPECT_EQ(JS_GetExceptionResourceKind(context, exception), expected);
        JS_FreeValue(context, exception);
        JS_FreeValue(context, result);
    };

    expect_failure_kind("(function recurse() { return 1 + recurse(); })()", JS_EXCEPTION_RESOURCE_STACK_OVERFLOW);

    // An error that a script throws is not an engine limit, whatever its text says, and neither is the error
    // that a script throws after it caught an overflow.
    expect_failure_kind("throw new Error('stack overflow')", JS_EXCEPTION_RESOURCE_NONE);
    expect_failure_kind("throw new Error('out of memory')", JS_EXCEPTION_RESOURCE_NONE);
    expect_failure_kind("throw new InternalError('stack overflow')", JS_EXCEPTION_RESOURCE_NONE);
    expect_failure_kind(
        "try { (function recurse() { return 1 + recurse(); })(); } "
        "catch (err) { throw new Error('after stack overflow'); }",
        JS_EXCEPTION_RESOURCE_NONE);

    // The engine's own error keeps its kind when a finally block, a for-of loop or a catch clause throws it again.
    expect_failure_kind(
        "function recurse() { try { return 1 + recurse(); } finally { } } recurse();",
        JS_EXCEPTION_RESOURCE_STACK_OVERFLOW);
    expect_failure_kind(
        "function recurse() { for (const item of [1]) { return 1 + recurse(); } } recurse();",
        JS_EXCEPTION_RESOURCE_STACK_OVERFLOW);
    expect_failure_kind(
        "try { (function recurse() { return 1 + recurse(); })(); } catch (err) { throw err; }",
        JS_EXCEPTION_RESOURCE_STACK_OVERFLOW);

    // A failure that has no pending exception any more, the reason of a rejected promise, still has its kind.
    JSValue promise = evaluate(context, "(async function recurse() { return 1 + await recurse(); })()");
    ASSERT_FALSE(JS_IsException(promise));
    JSContext* job_context = nullptr;
    while (JS_ExecutePendingJob(runtime, &job_context) > 0) {
    }
    EXPECT_EQ(JS_PromiseState(context, promise), JS_PROMISE_REJECTED);
    JSValue reason = JS_PromiseResult(context, promise);
    EXPECT_EQ(JS_GetExceptionResourceKind(context, reason), JS_EXCEPTION_RESOURCE_STACK_OVERFLOW);
    JS_FreeValue(context, reason);
    JS_FreeValue(context, promise);
}

TEST(QuickJsRuntimeTest, ExceptionResourceKindNamesAnExhaustedMemoryLimit) {
    Runtime engine;
    ASSERT_NE(engine.runtime(), nullptr);
    ASSERT_NE(engine.context(), nullptr);
    JS_SetMemoryLimit(engine.runtime(), 256U * 1024U);

    JSValue result = evaluate(
        engine.context(), "(function () { const items = []; while (true) { items.push('x'.repeat(4096)); } })()");
    EXPECT_TRUE(JS_IsException(result));
    EXPECT_EQ(JS_GetPendingExceptionResourceKind(engine.context()), JS_EXCEPTION_RESOURCE_OUT_OF_MEMORY);
    JSValue exception = JS_GetException(engine.context());
    JS_FreeValue(engine.context(), exception);
    JS_FreeValue(engine.context(), result);

    // The next failure is the script's own.
    result = evaluate(engine.context(), "throw new Error('out of memory')");
    EXPECT_TRUE(JS_IsException(result));
    EXPECT_EQ(JS_GetPendingExceptionResourceKind(engine.context()), JS_EXCEPTION_RESOURCE_NONE);
    exception = JS_GetException(engine.context());
    JS_FreeValue(engine.context(), exception);
    JS_FreeValue(engine.context(), result);
}
namespace {

// Evaluates a script that must fail, takes its exception, and returns why the engine threw it. `message` is for the
// failure report only.
JSExceptionResourceKindEnum failure_kind(JSContext* context, std::string_view source, std::string* message = nullptr) {
    JSValue result = evaluate(context, source);
    EXPECT_TRUE(JS_IsException(result)) << "the script did not fail: " << source.substr(0, 80);
    const auto pending = JS_GetPendingExceptionResourceKind(context);
    JSValue    exception = JS_GetException(context);
    EXPECT_EQ(JS_GetExceptionResourceKind(context, exception), pending);
    if (message != nullptr) {
        const char* text = JS_ToCString(context, exception);
        *message         = text ? text : "<unprintable exception>";
        if (text) JS_FreeCString(context, text);
        JSValue stack = JS_GetPropertyStr(context, exception, "stack");
        if (const char* stack_text = JS_IsString(stack) ? JS_ToCString(context, stack) : nullptr) {
            *message += std::string("\n") + stack_text;
            JS_FreeCString(context, stack_text);
        }
        JS_FreeValue(context, stack);
    }
    JS_FreeValue(context, exception);
    JS_FreeValue(context, result);
    return pending;
}

}  // namespace

// Every place where the engine gives up because the stack ran out records it, whichever check reaches the end of the
// stack first: the check moves with the size of the stack, so each script runs under a range of sizes.
TEST(QuickJsRuntimeTest, ExceptionResourceKindNamesEveryStackCheck) {
    Runtime engine;
    ASSERT_NE(engine.runtime(), nullptr);
    ASSERT_NE(engine.context(), nullptr);

    const std::string_view scripts[] = {
        // a JSON reviver that calls JSON.parse again
        "(function recurse() { return JSON.parse('[0]', function () { return recurse(); }); })()",
        // nested JSON text, nested source text, nested regular expression patterns
        "JSON.parse('['.repeat(200000))",
        "JSON.parse('{\"a\":'.repeat(200000))",
        "eval('('.repeat(100000))",
        "eval('['.repeat(100000))",
        "eval('({a:'.repeat(100000))",
        "new RegExp('('.repeat(100000))",
        "new RegExp('(?:'.repeat(100000))",
        "new RegExp('['.repeat(100000), 'v')",
        "new RegExp('[[a-z]'.repeat(50000) + ']'.repeat(50000), 'v')",
    };
    constexpr std::size_t first_stack_bytes = 16U * 1024U;
    constexpr std::size_t last_stack_bytes  = 40U * 1024U;
    constexpr std::size_t stack_step_bytes  = 61U;
    for (const auto script : scripts) {
        SCOPED_TRACE(std::string(script));
        for (std::size_t stack_bytes = first_stack_bytes; stack_bytes <= last_stack_bytes; stack_bytes += stack_step_bytes) {
            JS_SetMaxStackSize(engine.runtime(), stack_bytes);
            std::string message;
            const auto  kind = failure_kind(engine.context(), script, &message);
            if (kind != JS_EXCEPTION_RESOURCE_STACK_OVERFLOW) {
                ADD_FAILURE() << "stack " << stack_bytes << " bytes: " << message;
                break;
            }
        }
    }
}

// A function that needs more operand stack than the engine allows fails to compile with a stack overflow error.
TEST(QuickJsRuntimeTest, ExceptionResourceKindNamesAnOperandStackLimit) {
    Runtime engine;
    ASSERT_NE(engine.runtime(), nullptr);
    ASSERT_NE(engine.context(), nullptr);
    // The parser allows 65535 arguments in a call, and a function may use 65534 operand stack slots.
    const std::string_view scripts[] = {
        "new Function('f', 'return f(' + '0,'.repeat(65533) + '0)')",
        "new Function('f', 'return f.g(' + '0,'.repeat(65533) + '0)')",
        "new Function('f', 'return new f(' + '0,'.repeat(65533) + '0)')",
    };
    for (const auto script : scripts) {
        SCOPED_TRACE(std::string(script));
        std::string message;
        EXPECT_EQ(failure_kind(engine.context(), script, &message), JS_EXCEPTION_RESOURCE_STACK_OVERFLOW) << message;
    }
}

// A regular expression that runs out of memory while it runs: its backtracking stack outgrows the memory limit.
TEST(QuickJsRuntimeTest, ExceptionResourceKindNamesARegularExpressionThatRunsOutOfMemory) {
    Runtime engine;
    ASSERT_NE(engine.runtime(), nullptr);
    ASSERT_NE(engine.context(), nullptr);
    ASSERT_EQ(evaluated_string(engine.context(),
                               "globalThis.text = 'ab'.repeat(50000); globalThis.pattern = /(?:a|b)*c/; "
                               "globalThis.global_pattern = /(?:a|b)*c/g; 'ready'"),
              "ready");
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(engine.runtime(), &usage);
    JS_SetMemoryLimit(engine.runtime(), static_cast<std::size_t>(usage.malloc_size) + 64U * 1024U);

    const std::string_view scripts[] = {
        "pattern.test(text)",
        "text.replace(global_pattern, '')",
    };
    for (const auto script : scripts) {
        SCOPED_TRACE(std::string(script));
        std::string message;
        EXPECT_EQ(failure_kind(engine.context(), script, &message), JS_EXCEPTION_RESOURCE_OUT_OF_MEMORY) << message;
        EXPECT_NE(message.find("regexp execution"), std::string::npos) << message;
    }
}

// A regular expression whose bytecode outgrows the memory limit while it is compiled.
TEST(QuickJsRuntimeTest, ExceptionResourceKindNamesARegularExpressionThatCannotBeCompiledInMemory) {
    Runtime engine;
    ASSERT_NE(engine.runtime(), nullptr);
    ASSERT_NE(engine.context(), nullptr);
    ASSERT_EQ(evaluated_string(engine.context(), "globalThis.source = 'a'.repeat(100000); 'ready'"), "ready");
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(engine.runtime(), &usage);
    // The pattern is copied as text first, and its bytecode needs about three times as much.
    JS_SetMemoryLimit(engine.runtime(), static_cast<std::size_t>(usage.malloc_size) + 150U * 1024U);
    std::string message;
    EXPECT_EQ(failure_kind(engine.context(), "new RegExp(source)", &message), JS_EXCEPTION_RESOURCE_OUT_OF_MEMORY)
        << message;
    EXPECT_NE(message.find("SyntaxError"), std::string::npos) << message;
}

// String.prototype.normalize reports the memory it could not get.
TEST(QuickJsRuntimeTest, ExceptionResourceKindNamesANormalizationThatRunsOutOfMemory) {
    Runtime engine;
    ASSERT_NE(engine.runtime(), nullptr);
    ASSERT_NE(engine.context(), nullptr);
    constexpr std::size_t length = 20000;
    ASSERT_EQ(evaluated_string(engine.context(), "globalThis.text = '\\u00e9'.repeat(20000); 'ready'"), "ready");
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(engine.runtime(), &usage);
    // The first buffer of 4 bytes per code point fits and the second does not.
    JS_SetMemoryLimit(engine.runtime(), static_cast<std::size_t>(usage.malloc_size) + 6U * length);
    std::string message;
    EXPECT_EQ(failure_kind(engine.context(), "text.normalize('NFD')", &message), JS_EXCEPTION_RESOURCE_OUT_OF_MEMORY)
        << message;
}

#if defined(_MSC_VER)
TEST(QuickJsRuntimeTest, MsvcFunctionListMacrosPreserveMemberTypes) {
    EXPECT_EQ(msvc_function_list_entries[0].u.func.cfunc.generic, native_add);
    EXPECT_EQ(msvc_function_list_entries[1].u.func.cfunc.generic_magic, msvc_generic_magic);
    EXPECT_EQ(msvc_function_list_entries[2].u.func.cfunc.f_f, msvc_unary);
    EXPECT_EQ(msvc_function_list_entries[3].u.func.cfunc.f_f_f, msvc_binary);
    EXPECT_EQ(msvc_function_list_entries[4].u.func.cfunc.iterator_next, msvc_iterator_next);
    EXPECT_EQ(msvc_function_list_entries[5].u.getset.get.getter, msvc_getter);
    EXPECT_EQ(msvc_function_list_entries[5].u.getset.set.setter, msvc_setter);
    EXPECT_EQ(msvc_function_list_entries[6].u.getset.get.getter_magic, msvc_getter_magic);
    EXPECT_EQ(msvc_function_list_entries[6].u.getset.set.setter_magic, msvc_setter_magic);
    EXPECT_STREQ(msvc_function_list_entries[7].u.str, "value");
    EXPECT_EQ(msvc_function_list_entries[8].u.i32, 7);
    EXPECT_EQ(msvc_function_list_entries[9].u.i64, 8);
    EXPECT_DOUBLE_EQ(msvc_function_list_entries[10].u.f64, 9.0);
    EXPECT_EQ(msvc_function_list_entries[11].u.i32, 0);
    EXPECT_EQ(msvc_function_list_entries[12].u.i32, 10);
    EXPECT_EQ(msvc_function_list_entries[13].u.i32, 1);
    EXPECT_EQ(msvc_function_list_entries[14].u.prop_list.tab, msvc_nested_entries);
    EXPECT_EQ(msvc_function_list_entries[14].u.prop_list.len, 1);
    EXPECT_EQ(msvc_function_list_entries[15].u.alias.base, -1);
    EXPECT_EQ(msvc_function_list_entries[16].u.alias.base, 3);
}
#endif

}  // namespace
