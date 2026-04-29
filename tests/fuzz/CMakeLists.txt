# libFuzzer-based fuzz targets for NeoGraph's parsing surface.
#
# Enable with: -DNEOGRAPH_BUILD_FUZZ=ON. Requires Clang
# (libFuzzer ships in its instrumentation runtime). Each target is
# linked with `-fsanitize=fuzzer,address,undefined` so a crash
# bisected by libFuzzer also surfaces ASan/UBSan diagnostics.
#
# To run a target locally:
#   ./build-fuzz/fuzz_graph_compile \
#       -max_total_time=120 -timeout=10 \
#       tests/fuzz/corpus/graph_compile/
#
# Corpus directories under tests/fuzz/corpus/ are intentionally seeded
# with canonical valid JSON shapes — the fuzzer mutates from those.

add_executable(fuzz_graph_compile fuzz_graph_compile.cpp)
target_compile_options(fuzz_graph_compile PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer)
target_link_options(fuzz_graph_compile PRIVATE
    -fsanitize=fuzzer,address,undefined)
target_link_libraries(fuzz_graph_compile PRIVATE
    neograph::core)
