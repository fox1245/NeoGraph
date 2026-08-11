// Q0 cold-process benchmarks for the exact vendored QuickJS C API.
// One invocation emits one JSON sample. Process repetition and gates live in
// scripts/run_quickjs_performance.py so a sample can never reuse a runtime.

#include <quickjs.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

union AllocationHeader {
    std::max_align_t alignment;
    std::size_t      size;
};

struct AllocationStats {
    std::size_t current_bytes = 0;
    std::size_t peak_bytes    = 0;
};

void update_peak(AllocationStats& stats) noexcept {
    if (stats.current_bytes > stats.peak_bytes) stats.peak_bytes = stats.current_bytes;
}

void* accounted_malloc(JSMallocState* state, std::size_t size) {
    if (size == 0 || size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
        return nullptr;
    if (size > state->malloc_limit || state->malloc_size > state->malloc_limit - size)
        return nullptr;

    auto* allocation = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
    if (!allocation) return nullptr;
    allocation->size = size;
    ++state->malloc_count;
    state->malloc_size += size;
    if (auto* stats = static_cast<AllocationStats*>(state->opaque)) {
        stats->current_bytes += size;
        update_peak(*stats);
    }
    return allocation + 1;
}

void accounted_free(JSMallocState* state, void* pointer) {
    if (!pointer) return;
    auto* allocation = static_cast<AllocationHeader*>(pointer) - 1;
    --state->malloc_count;
    state->malloc_size -= allocation->size;
    if (auto* stats = static_cast<AllocationStats*>(state->opaque))
        stats->current_bytes -= allocation->size;
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
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) ||
        size > state->malloc_limit || state->malloc_size - old_size > state->malloc_limit - size)
        return nullptr;

    auto* resized =
        static_cast<AllocationHeader*>(std::realloc(allocation, sizeof(AllocationHeader) + size));
    if (!resized) return nullptr;
    resized->size      = size;
    state->malloc_size = state->malloc_size - old_size + size;
    if (auto* stats = static_cast<AllocationStats*>(state->opaque)) {
        stats->current_bytes = stats->current_bytes - old_size + size;
        update_peak(*stats);
    }
    return resized + 1;
}

std::size_t accounted_usable_size(const void* pointer) {
    return pointer ? (static_cast<const AllocationHeader*>(pointer) - 1)->size : 0;
}

constexpr JSMallocFunctions kAllocator{
    accounted_malloc,
    accounted_free,
    accounted_realloc,
    accounted_usable_size,
};

class Runtime final {
public:
    explicit Runtime(AllocationStats& stats) {
        runtime_ = JS_NewRuntime2(&kAllocator, &stats);
        if (!runtime_) throw std::runtime_error("JS_NewRuntime2 failed");
        context_ = JS_NewContext(runtime_);
        if (!context_) {
            JS_FreeRuntime(runtime_);
            runtime_ = nullptr;
            throw std::runtime_error("JS_NewContext failed");
        }
    }

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    ~Runtime() {
        if (context_) JS_FreeContext(context_);
        if (runtime_) JS_FreeRuntime(runtime_);
    }

    JSContext* context() const noexcept { return context_; }

private:
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
};

struct Sample {
    double      value_us             = 0.0;
    std::size_t peak_allocated_bytes = 0;
};

double elapsed_us(Clock::time_point started) {
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

std::string exception_message(JSContext* context) {
    JSValue     exception = JS_GetException(context);
    const char* text      = JS_ToCString(context, exception);
    std::string message   = text ? text : "unprintable QuickJS exception";
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, exception);
    return message;
}

Sample runtime_creation() {
    AllocationStats stats;
    const auto      started = Clock::now();
    Runtime         runtime(stats);
    return {elapsed_us(started), stats.peak_bytes};
}

Sample compile_source(std::string_view source, int flags) {
    AllocationStats stats;
    Runtime         runtime(stats);
    const auto      started  = Clock::now();
    JSValue         compiled = JS_Eval(runtime.context(), source.data(), source.size(),
                                       "<quickjs-performance>", flags | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        const auto message = exception_message(runtime.context());
        JS_FreeValue(runtime.context(), compiled);
        throw std::runtime_error(message);
    }
    JS_FreeValue(runtime.context(), compiled);
    return {elapsed_us(started), stats.peak_bytes};
}

const char* build_type() noexcept {
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
    return NEOGRAPH_BENCH_BUILD_TYPE;
#else
    return "unspecified";
#endif
}

void print_sample(std::string_view case_id, const Sample& sample) {
    std::cout << std::setprecision(17) << "{\"schema_version\":1,\"case\":\"" << case_id
              << "\",\"status\":\"ok\",\"value\":" << sample.value_us
              << ",\"unit\":\"us\",\"peak_allocated_bytes\":" << sample.peak_allocated_bytes
              << ",\"memory_scope\":\"quickjs_allocator\",\"build_type\":\"" << build_type()
              << "\"}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::string_view(argv[1]) != "--case")
            throw std::invalid_argument(
                "usage: bench_quickjs_primitives --case "
                "runtime_creation_cold|source_compilation_cold|module_compilation_cold");

        const std::string_view case_id = argv[2];
        if (case_id == "runtime_creation_cold") {
            print_sample(case_id, runtime_creation());
        } else if (case_id == "source_compilation_cold") {
            print_sample(case_id,
                         compile_source("function fold(n) { let v = 0; for (let i = 0; i < n; ++i) "
                                        "v += i; return v; } fold(128);",
                                        JS_EVAL_TYPE_GLOBAL));
        } else if (case_id == "module_compilation_cold") {
            print_sample(
                case_id,
                compile_source("export function fold(n) { let v = 0; for (let i = 0; i < n; ++i) "
                               "v += i; return v; }",
                               JS_EVAL_TYPE_MODULE));
        } else {
            throw std::invalid_argument("unknown primitive benchmark case: " +
                                        std::string(case_id));
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
