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
    Runtime runtime(allocator, &stats);
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

}  // namespace
