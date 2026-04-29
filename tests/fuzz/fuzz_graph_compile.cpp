// libFuzzer entry — feed arbitrary bytes as JSON into GraphCompiler::compile.
//
// What this catches:
//   - Asserts triggered by malformed JSON shapes the compiler doesn't
//     guard (e.g. node type that's a number instead of string).
//   - Heap-buffer-overflow / UB inside yyjson's parser when invoked
//     by neograph::json::parse — already exercised by the loader's
//     try/catch but a fuzzer drives it harder than unit tests.
//   - Out-of-bounds reads from container access if the compiler
//     dereferences fields without first checking they exist (the
//     `value()` helpers in src/core/graph_compiler.cpp default to
//     a known type, but mistakes happen).
//
// Build (link with -fsanitize=fuzzer,address):
//   cmake -B build-fuzz -DNEOGRAPH_BUILD_FUZZ=ON
//   cmake --build build-fuzz --target fuzz_graph_compile
//   ./build-fuzz/fuzz_graph_compile -max_total_time=60 corpus/
//
// To seed the corpus, drop valid JSON graph definitions in
// `corpus/graph_compile/`. The included seeds.cpp file in tests/fuzz/
// has a few canonical shapes if no corpus exists yet.

#include <cstddef>
#include <cstdint>
#include <string>

#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Reject obviously-too-large inputs — the JSON loader is not the
    // place to test multi-MB allocations and they slow the fuzzer.
    if (size > 64 * 1024) return 0;

    // Fuzzer-friendly: silently swallow any std::exception thrown
    // during parsing or compile. The sanitizers (UBSan / ASan)
    // separately abort on undefined behaviour and memory errors,
    // which is what we want libFuzzer to surface.
    try {
        std::string s(reinterpret_cast<const char*>(data), size);
        auto def = neograph::json::parse(s);
        neograph::graph::NodeContext ctx;
        auto cg = neograph::graph::GraphCompiler::compile(def, ctx);
        // Touch the compiled fields so any latent UB in field
        // initializers fires (the compiler's a struct of vectors —
        // touching .size() exercises the move semantics if any).
        (void)cg.edges.size();
        (void)cg.conditional_edges.size();
        (void)cg.channel_defs.size();
        (void)cg.nodes.size();
        (void)cg.barrier_specs.size();
    } catch (const std::exception&) {
        // Expected — most fuzz inputs aren't valid graph JSON.
        // The fuzzer cares about ASan/UBSan signals, not exceptions.
    } catch (...) {
        // Unexpected non-std exception — re-throw so the fuzzer
        // surfaces it as a crash signal.
        throw;
    }
    return 0;
}
