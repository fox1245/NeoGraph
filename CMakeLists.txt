cmake_minimum_required(VERSION 3.16)
project(NeoGraph VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC: treat source files as UTF-8
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ===========================================================================
# Options
# ===========================================================================
option(NEOGRAPH_BUILD_LLM      "Build neograph::llm (LLM providers)"     ON)
option(NEOGRAPH_BUILD_MCP      "Build neograph::mcp (MCP client)"        ON)
option(NEOGRAPH_BUILD_UTIL     "Build neograph::util (utilities)"        ON)
option(NEOGRAPH_BUILD_POSTGRES "Build PostgresCheckpointStore (libpq)" ON)
option(NEOGRAPH_BUILD_SQLITE   "Build SqliteCheckpointStore (libsqlite3)" ON)
option(NEOGRAPH_BUILD_EXAMPLES "Build example programs"                  ON)

# ===========================================================================
# Dependencies
# ===========================================================================
find_package(Threads REQUIRED)

set(DEPS_DIR ${PROJECT_SOURCE_DIR}/deps)

# yyjson (C, compiled) — used by all modules for JSON parse/write
add_library(yyjson STATIC ${DEPS_DIR}/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC ${DEPS_DIR}/yyjson)
set_target_properties(yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(yyjson PRIVATE -O2)
endif()

# cpp-httplib (header-only) — used by llm and mcp (PRIVATE)
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${DEPS_DIR})
# Windows: httplib hard-errors unless _WIN32_WINNT >= 0x0A00 (Windows 10).
# Set it on the INTERFACE so every TU that links httplib picks it up,
# regardless of whether it also links asio.
if(WIN32)
    target_compile_definitions(httplib INTERFACE
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
endif()
# httplib's TLS path on Apple pulls in system certs via
# SecTrustCopyAnchorCertificates (Security.framework) and
# CFArray/CFData (CoreFoundation.framework). These must be on the
# final link line for any target that links httplib with SSL.
if(APPLE)
    target_link_libraries(httplib INTERFACE
        "-framework CoreFoundation"
        "-framework Security")
endif()

# moodycamel::ConcurrentQueue (header-only) — used by util (PRIVATE)
add_library(concurrentqueue INTERFACE)
target_include_directories(concurrentqueue INTERFACE ${DEPS_DIR})

# cppdotenv (header-only) — used by examples that read .env for API keys
add_library(cppdotenv INTERFACE)
target_include_directories(cppdotenv INTERFACE ${DEPS_DIR})

# standalone asio (header-only) — used by the async runtime layer.
# Vendored as `deps/asio/include/` so the public macro ASIO_STANDALONE
# turns off the Boost.System coupling. Any TU linking this must also
# link a threads lib (std::thread backend).
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${DEPS_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
# Windows: target Windows 10+ so httplib compiles (it hard-errors on
# _WIN32_WINNT < 0x0A00) and asio's newer overlapped/pipe paths light up.
# Also scrub min/max macros from <windows.h> that clobber std::min/max.
if(WIN32)
    target_compile_definitions(asio INTERFACE
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
endif()
find_package(Threads REQUIRED)
target_link_libraries(asio INTERFACE Threads::Threads)

# OpenSSL — required by httplib for HTTPS, and by neograph::async
# (asio::ssl in the async HTTP client).
find_package(OpenSSL REQUIRED)

# SQLite3 — required by neograph::sqlite (SqliteCheckpointStore).
# Available as a system package on every Linux distro; on macOS comes
# with the system. find_package(SQLite3) is provided by CMake itself.
if(NEOGRAPH_BUILD_SQLITE)
    find_package(SQLite3 REQUIRED)
endif()

# libpq — required by neograph::postgres (PostgresCheckpointStore).
# Stage 3 / Sem 3.3: dropped libpqxx in favour of libpq directly.
# libpqxx-7.8t64 on Ubuntu 24.04 has a C++17/C++20 ABI split that
# broke the C++20 build; libpq's C ABI is stable.
# PostgreSQL::PostgreSQL target comes from CMake's bundled FindPostgreSQL.
if(NEOGRAPH_BUILD_POSTGRES)
    find_package(PostgreSQL REQUIRED)
endif()

# ===========================================================================
# Schema embedding (build-time code generation)
# ===========================================================================
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

add_custom_command(
    OUTPUT ${GENERATED_DIR}/builtin_schemas.h
    COMMAND Python3::Interpreter
            ${PROJECT_SOURCE_DIR}/scripts/embed_schemas.py
            ${PROJECT_SOURCE_DIR}/schemas
            ${GENERATED_DIR}/builtin_schemas.h
    DEPENDS
        ${PROJECT_SOURCE_DIR}/schemas/openai.json
        ${PROJECT_SOURCE_DIR}/schemas/openai_responses.json
        ${PROJECT_SOURCE_DIR}/schemas/claude.json
        ${PROJECT_SOURCE_DIR}/schemas/gemini.json
        ${PROJECT_SOURCE_DIR}/scripts/embed_schemas.py
    COMMENT "Embedding LLM provider schemas"
)
add_custom_target(generate_schemas DEPENDS ${GENERATED_DIR}/builtin_schemas.h)

# ===========================================================================
# neograph::core — Graph engine + foundation types
# ===========================================================================
add_library(neograph_core STATIC
    src/core/json.cpp
    src/core/provider.cpp
    src/core/tool.cpp
    src/core/graph_engine.cpp
    src/core/graph_compiler.cpp
    src/core/graph_coordinator.cpp
    src/core/graph_executor.cpp
    src/core/scheduler.cpp
    src/core/graph_state.cpp
    src/core/graph_node.cpp
    src/core/graph_loader.cpp
    src/core/graph_checkpoint.cpp
    src/core/react_graph.cpp
    src/core/plan_execute_graph.cpp
    src/core/deep_research_graph.cpp
    src/core/store.cpp
)
# postgres_checkpoint.cpp is built into a separate target below
# (neograph_postgres) so projects without libpq can still link
# neograph::core.

target_include_directories(neograph_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(neograph_core
    PUBLIC  yyjson Threads::Threads asio
)

# Coroutines are exposed through public headers (provider.h ships
# asio::awaitable<ChatCompletion>); propagate C++20 to consumers.
target_compile_features(neograph_core PUBLIC cxx_std_20)

add_library(neograph::core ALIAS neograph_core)

# ===========================================================================
# neograph::async — asio-based async runtime (PoC stage)
#
# Experimental. Not yet exposed through public headers. Goal: run a
# single-core io_context that multiplexes many in-flight LLM / PG / MCP
# calls so one agent no longer needs one OS thread. See
# benchmarks/bench_async_fanout for the scaling measurement that
# justifies (or kills) the bigger refactor.
# ===========================================================================
add_library(neograph_async STATIC
    src/async/async_smoke.cpp
    src/async/http_client.cpp
    src/async/conn_pool.cpp
    src/async/sse_parser.cpp
)
target_include_directories(neograph_async
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(neograph_async PUBLIC asio OpenSSL::SSL OpenSSL::Crypto)
# C++20 coroutines require this on GCC < 14 when using -std=c++20.
target_compile_features(neograph_async PUBLIC cxx_std_20)
add_library(neograph::async ALIAS neograph_async)

# Smoke binary — just proves the async TU compiles, links, and runs.
add_executable(neograph_async_smoke src/async/async_smoke_main.cpp)
target_link_libraries(neograph_async_smoke PRIVATE neograph_async)

# Live-network smoke for the TLS path. Not in ctest (offline-safe by
# default). Run manually after touching http_client.cpp's TLS branch.
add_executable(neograph_async_https_smoke src/async/async_https_smoke_main.cpp)
target_link_libraries(neograph_async_https_smoke PRIVATE neograph_async)

# ===========================================================================
# neograph::postgres — PostgresCheckpointStore (libpq)
#
# Optional persistent CheckpointStore backed by PostgreSQL. Schema mirrors
# LangGraph's PostgresSaver (three tables: checkpoints, blobs, writes)
# with a `neograph_` prefix so a single database can host both NeoGraph
# and LangGraph state without name collisions.
# ===========================================================================
if(NEOGRAPH_BUILD_POSTGRES)
    add_library(neograph_postgres STATIC
        src/core/postgres_checkpoint.cpp
    )

    target_include_directories(neograph_postgres
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(neograph_postgres
        PUBLIC  neograph_core
        PRIVATE PostgreSQL::PostgreSQL
    )

    add_library(neograph::postgres ALIAS neograph_postgres)
endif()

# ===========================================================================
# neograph::sqlite — SqliteCheckpointStore (libsqlite3)
#
# Single-file persistent CheckpointStore. Same schema shape as the
# Postgres variant (neograph_* tables, blob dedup via ON CONFLICT) but
# zero server. Picked when the deployment is single-process / embedded
# / desktop-CLI.
# ===========================================================================
if(NEOGRAPH_BUILD_SQLITE)
    add_library(neograph_sqlite STATIC
        src/core/sqlite_checkpoint.cpp
    )

    target_include_directories(neograph_sqlite
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(neograph_sqlite
        PUBLIC  neograph_core
        PRIVATE SQLite::SQLite3
    )

    add_library(neograph::sqlite ALIAS neograph_sqlite)
endif()

# ===========================================================================
# neograph::llm — LLM provider implementations (OpenAI, Schema, Agent)
# ===========================================================================
if(NEOGRAPH_BUILD_LLM)
    add_library(neograph_llm STATIC
        src/llm/openai_provider.cpp
        src/llm/schema_provider.cpp
        src/llm/rate_limited_provider.cpp
        src/llm/agent.cpp
    )

    add_dependencies(neograph_llm generate_schemas)

    target_include_directories(neograph_llm
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${GENERATED_DIR}
    )

    target_link_libraries(neograph_llm
        PUBLIC  neograph_core
        PRIVATE httplib OpenSSL::SSL OpenSSL::Crypto neograph_async
    )

    add_library(neograph::llm ALIAS neograph_llm)
endif()

# ===========================================================================
# neograph::mcp — MCP client (JSON-RPC over HTTP/stdio)
# ===========================================================================
if(NEOGRAPH_BUILD_MCP)
    add_library(neograph_mcp STATIC
        src/mcp/client.cpp
    )

    target_include_directories(neograph_mcp
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(neograph_mcp
        PUBLIC  neograph_core
        PRIVATE neograph_async OpenSSL::SSL OpenSSL::Crypto
    )

    add_library(neograph::mcp ALIAS neograph_mcp)
endif()

# ===========================================================================
# neograph::util — Utilities (RequestQueue, header-only)
# ===========================================================================
if(NEOGRAPH_BUILD_UTIL)
    add_library(neograph_util INTERFACE)

    target_include_directories(neograph_util
        INTERFACE
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(neograph_util
        INTERFACE neograph_core concurrentqueue
    )

    add_library(neograph::util ALIAS neograph_util)
endif()

# ===========================================================================
# Examples
# ===========================================================================
if(NEOGRAPH_BUILD_EXAMPLES)
    if(NEOGRAPH_BUILD_LLM)
        add_executable(example_react_agent examples/01_react_agent.cpp)
        target_link_libraries(example_react_agent PRIVATE neograph::core neograph::llm cppdotenv)

        add_executable(example_custom_graph examples/02_custom_graph.cpp)
        target_link_libraries(example_custom_graph PRIVATE neograph::core neograph::llm)

        add_executable(example_checkpoint_hitl examples/04_checkpoint_hitl.cpp)
        target_link_libraries(example_checkpoint_hitl PRIVATE neograph::core neograph::llm)

        add_executable(example_parallel_fanout examples/05_parallel_fanout.cpp)
        target_link_libraries(example_parallel_fanout PRIVATE neograph::core)

        add_executable(example_subgraph examples/06_subgraph.cpp)
        target_link_libraries(example_subgraph PRIVATE neograph::core neograph::llm)

        add_executable(example_intent_routing examples/07_intent_routing.cpp)
        target_link_libraries(example_intent_routing PRIVATE neograph::core neograph::llm)

        add_executable(example_state_management examples/08_state_management.cpp)
        target_link_libraries(example_state_management PRIVATE neograph::core neograph::llm)

        add_executable(example_all_features examples/09_all_features.cpp)
        target_link_libraries(example_all_features PRIVATE neograph::core neograph::llm)

        add_executable(example_send_command examples/10_send_command.cpp)
        target_link_libraries(example_send_command PRIVATE neograph::core)

        add_executable(example_rag_agent examples/12_rag_agent.cpp)
        target_link_libraries(example_rag_agent PRIVATE neograph::core neograph::llm httplib OpenSSL::SSL OpenSSL::Crypto cppdotenv)

        add_executable(example_openai_responses examples/13_openai_responses.cpp)
        target_link_libraries(example_openai_responses PRIVATE neograph::core neograph::llm cppdotenv)

        add_executable(example_plan_executor examples/14_plan_executor.cpp)
        target_link_libraries(example_plan_executor PRIVATE neograph::core)

        add_executable(example_reflexion examples/15_reflexion.cpp)
        target_link_libraries(example_reflexion PRIVATE neograph::core neograph::llm cppdotenv)

        add_executable(example_tree_of_thoughts examples/16_tree_of_thoughts.cpp)
        target_link_libraries(example_tree_of_thoughts PRIVATE neograph::core neograph::llm cppdotenv)

        add_executable(example_self_ask examples/17_self_ask.cpp)
        target_link_libraries(example_self_ask PRIVATE neograph::core neograph::llm cppdotenv)

        add_executable(example_multi_agent_debate examples/18_multi_agent_debate.cpp)
        target_link_libraries(example_multi_agent_debate PRIVATE neograph::core neograph::llm cppdotenv)

        add_executable(example_rewoo examples/19_rewoo.cpp)
        target_link_libraries(example_rewoo PRIVATE neograph::core neograph::llm cppdotenv)

        add_executable(example_deep_research examples/25_deep_research.cpp)
        target_link_libraries(example_deep_research PRIVATE neograph::core neograph::llm httplib OpenSSL::SSL OpenSSL::Crypto cppdotenv)

        # Example 27: async concurrent runs (Stage 3 / Sem 4.1).
        # Demonstrates engine->run_async() under asio::io_context.
        add_executable(example_async_concurrent_runs examples/27_async_concurrent_runs.cpp)
        target_link_libraries(example_async_concurrent_runs PRIVATE neograph::core)

        # Example 28: Corrective RAG (arXiv:2401.15884) over /v1/responses.
        add_executable(example_corrective_rag examples/28_corrective_rag.cpp)
        target_link_libraries(example_corrective_rag PRIVATE neograph::core neograph::llm cppdotenv)

        # Example 29: /v1/responses raw envelope dump — debug / pedagogy aid.
        add_executable(example_responses_envelope examples/29_responses_envelope.cpp)
        target_link_libraries(example_responses_envelope PRIVATE neograph::core neograph::llm cppdotenv)

        # Example 30: reasoning_effort tradeoff sweep on /v1/responses.
        add_executable(example_reasoning_effort examples/30_reasoning_effort.cpp)
        target_link_libraries(example_reasoning_effort PRIVATE neograph::core neograph::llm cppdotenv)

        # Example 31: NeoGraph driving a local OpenAI-compatible inference
        # server (TransformerCPP, llama.cpp server, vLLM OpenAI-compat,
        # etc.). Demonstrates that the two-process agent/inference split
        # keeps the agent L3-resident even when the model is multi-GB.
        add_executable(example_local_transformer examples/31_local_transformer.cpp)
        target_link_libraries(example_local_transformer PRIVATE neograph::core neograph::llm)

        # Example 32: in-process sibling of 31. Links TransformerCPP
        # (https://github.com/fox1245/TransformerCPP) directly into the
        # agent process so there's no HTTP round trip to the model. Both
        # examples together show both halves of a C++ LLM-agent stack:
        # external inference server (31) vs single-binary edge (32).
        # Opt-in because TransformerCPP is an external dep.
        option(NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE
            "Build example 32 (in-process local GGUF via TransformerCPP). Requires TRANSFORMERCPP_DIR pointing at a built sibling clone." OFF)
        if(NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE)
            if(NOT TRANSFORMERCPP_DIR)
                message(FATAL_ERROR "NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE=ON requires -DTRANSFORMERCPP_DIR=/path/to/TransformerCPP (must be a built sibling clone with build/libtransformercpp.a).")
            endif()
            if(NOT EXISTS "${TRANSFORMERCPP_DIR}/include/transformercpp/neograph/transformer_provider.h")
                message(FATAL_ERROR "TRANSFORMERCPP_DIR='${TRANSFORMERCPP_DIR}' does not contain include/transformercpp/neograph/transformer_provider.h — check the path.")
            endif()
            if(NOT EXISTS "${TRANSFORMERCPP_DIR}/build/libtransformercpp.a")
                message(FATAL_ERROR "TRANSFORMERCPP_DIR='${TRANSFORMERCPP_DIR}' has no build/libtransformercpp.a — build TransformerCPP first.")
            endif()

            add_executable(example_inproc_gemma examples/32_inproc_gemma.cpp)
            target_include_directories(example_inproc_gemma PRIVATE
                ${TRANSFORMERCPP_DIR}/include)
            # TransformerCPP's static lib carries undefined refs into
            # llama.cpp + ggml, which live in the shared libs under
            # build/bin/. Link those explicitly and add an RPATH so the
            # binary finds them at run time without LD_LIBRARY_PATH.
            target_link_libraries(example_inproc_gemma PRIVATE
                neograph::core neograph::llm
                ${TRANSFORMERCPP_DIR}/build/libtransformercpp.a
                ${TRANSFORMERCPP_DIR}/build/bin/libllama.so
                ${TRANSFORMERCPP_DIR}/build/bin/libggml.so
                ${TRANSFORMERCPP_DIR}/build/bin/libggml-cpu.so
                ${TRANSFORMERCPP_DIR}/build/bin/libggml-base.so)
            set_target_properties(example_inproc_gemma PROPERTIES
                BUILD_RPATH "${TRANSFORMERCPP_DIR}/build/bin"
                INSTALL_RPATH "${TRANSFORMERCPP_DIR}/build/bin")
            message(STATUS "  [+] example_inproc_gemma (TRANSFORMERCPP_DIR=${TRANSFORMERCPP_DIR})")
        endif()

        # Example 26 needs both LLM (Claude provider) and the Postgres
        # backend; gate it on both build flags so a project that turns
        # off Postgres doesn't try to link a missing target.
        if(NEOGRAPH_BUILD_POSTGRES)
            add_executable(example_postgres_react_hitl
                examples/26_postgres_react_hitl/main.cpp)
            target_link_libraries(example_postgres_react_hitl PRIVATE
                neograph::core neograph::llm neograph::postgres
                httplib OpenSSL::SSL OpenSSL::Crypto cppdotenv)
        endif()

        if(NEOGRAPH_BUILD_MCP)
            add_executable(example_mcp_agent examples/03_mcp_agent.cpp)
            target_link_libraries(example_mcp_agent PRIVATE neograph::core neograph::llm neograph::mcp cppdotenv)

            add_executable(example_mcp_hitl examples/20_mcp_hitl.cpp)
            target_link_libraries(example_mcp_hitl PRIVATE neograph::core neograph::llm neograph::mcp cppdotenv)

            add_executable(example_mcp_fanout examples/21_mcp_fanout.cpp)
            target_link_libraries(example_mcp_fanout PRIVATE neograph::core neograph::mcp)

            add_executable(example_mcp_stdio examples/22_mcp_stdio.cpp)
            target_link_libraries(example_mcp_stdio PRIVATE neograph::core neograph::llm neograph::mcp cppdotenv)

            add_executable(example_mcp_multi examples/23_mcp_multi.cpp)
            target_link_libraries(example_mcp_multi PRIVATE neograph::core neograph::llm neograph::mcp cppdotenv)

            add_executable(example_mcp_feedback examples/24_mcp_feedback.cpp)
            target_link_libraries(example_mcp_feedback PRIVATE neograph::core neograph::llm neograph::mcp cppdotenv)
        endif()

        # Clay + Raylib chatbot (optional — requires Raylib)
        option(NEOGRAPH_BUILD_CLAY_EXAMPLE "Build Clay chatbot example (fetches Raylib)" OFF)
        if(NEOGRAPH_BUILD_CLAY_EXAMPLE)
            include(FetchContent)
            FetchContent_Declare(raylib
                GIT_REPOSITORY https://github.com/raysan5/raylib.git
                GIT_TAG 5.5
                GIT_SHALLOW TRUE)
            set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(raylib)

            add_executable(example_clay_chatbot examples/11_clay_chatbot.cpp examples/clay_impl.c)
            target_link_libraries(example_clay_chatbot PRIVATE neograph::core neograph::llm raylib cppdotenv)
            target_include_directories(example_clay_chatbot PRIVATE ${DEPS_DIR})
        endif()
    endif()
endif()

# ===========================================================================
# Benchmarks (opt-in)
# ===========================================================================
option(NEOGRAPH_BUILD_BENCHMARKS "Build micro/load benchmark binaries" OFF)
if(NEOGRAPH_BUILD_BENCHMARKS)
    add_executable(bench_neograph benchmarks/bench_neograph.cpp)
    target_link_libraries(bench_neograph PRIVATE neograph::core)

    # Checkpoint-store load benchmark — compares InMemory / SQLite /
    # Postgres under N×M synthetic save workload. Postgres is opt-in
    # via NEOGRAPH_HAVE_POSTGRES so the binary still builds when the
    # PG backend is off.
    if(NEOGRAPH_BUILD_SQLITE)
        add_executable(bench_checkpoint_store benchmarks/bench_checkpoint_store.cpp)
        target_link_libraries(bench_checkpoint_store PRIVATE
            neograph::core neograph::sqlite Threads::Threads)
        if(NEOGRAPH_BUILD_POSTGRES)
            target_link_libraries(bench_checkpoint_store PRIVATE neograph::postgres)
            target_compile_definitions(bench_checkpoint_store PRIVATE NEOGRAPH_HAVE_POSTGRES)
        endif()
    endif()

    # async-runtime PoC: pure-timer fan-out to compare thread-per-agent
    # vs io_context + coroutines at 100/1K/10K concurrency. No HTTP in
    # this cut — measures the scheduling primitive in isolation.
    add_executable(bench_async_fanout benchmarks/bench_async_fanout.cpp)
    target_link_libraries(bench_async_fanout PRIVATE neograph::async)
    target_compile_features(bench_async_fanout PRIVATE cxx_std_20)

    # async-runtime PoC stage 2: mock HTTP server + sync/async clients.
    # Confirms the timer-only advantage survives real TCP / HTTP parse
    # / socket setup overhead.
    add_executable(bench_async_http benchmarks/bench_async_http.cpp)
    target_link_libraries(bench_async_http PRIVATE
        neograph::async httplib OpenSSL::SSL OpenSSL::Crypto)
    target_compile_features(bench_async_http PRIVATE cxx_std_20)
endif()

# ===========================================================================
# Tests (Google Test)
# ===========================================================================
option(NEOGRAPH_BUILD_TESTS "Build unit tests" OFF)

if(NEOGRAPH_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()

# ===========================================================================
# Install (TODO: proper install with generator expressions for deps)
# ===========================================================================
include(GNUInstallDirs)

install(DIRECTORY include/neograph
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY schemas/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neograph/schemas)
