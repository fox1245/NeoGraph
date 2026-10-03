# Link the installed SDK transitively through neograph::core; no SDK source checkout is needed.
if(NOT TARGET neograph::core OR NOT TARGET neograph::llm)
    message(FATAL_ERROR "Provider benchmark requires NEOGRAPH_BUILD_LLM and NEOGRAPH_BUILD_ASYNC")
endif()
find_package(Threads REQUIRED)
add_executable(neograph_provider_cutover_benchmark
    "${CMAKE_CURRENT_LIST_DIR}/provider_cutover_benchmark.cpp")
target_compile_features(neograph_provider_cutover_benchmark PRIVATE cxx_std_20)
target_link_libraries(neograph_provider_cutover_benchmark
    PRIVATE neograph::core neograph::llm Threads::Threads)
