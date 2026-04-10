cmake_minimum_required(VERSION 3.16)
project(NeoGraph VERSION 0.1.0 LANGUAGES C CXX)

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
option(NEOGRAPH_BUILD_EXAMPLES "Build example programs"                  ON)

# ===========================================================================
# Dependencies
# ===========================================================================
find_package(Threads REQUIRED)

set(DEPS_DIR ${CMAKE_SOURCE_DIR}/deps)

# nlohmann/json (header-only) — used by all modules
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${DEPS_DIR})

# Taskflow (header-only) — used by core (graph engine parallelism)
add_library(taskflow INTERFACE)
target_include_directories(taskflow INTERFACE ${DEPS_DIR})

# cpp-httplib (header-only) — used by llm and mcp (PRIVATE)
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${DEPS_DIR})

# moodycamel::ConcurrentQueue (header-only) — used by util (PRIVATE)
add_library(concurrentqueue INTERFACE)
target_include_directories(concurrentqueue INTERFACE ${DEPS_DIR})

# OpenSSL — required by httplib for HTTPS
if(NEOGRAPH_BUILD_LLM OR NEOGRAPH_BUILD_MCP)
    find_package(OpenSSL REQUIRED)
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
            ${CMAKE_SOURCE_DIR}/scripts/embed_schemas.py
            ${CMAKE_SOURCE_DIR}/schemas
            ${GENERATED_DIR}/builtin_schemas.h
    DEPENDS
        ${CMAKE_SOURCE_DIR}/schemas/openai.json
        ${CMAKE_SOURCE_DIR}/schemas/claude.json
        ${CMAKE_SOURCE_DIR}/schemas/gemini.json
        ${CMAKE_SOURCE_DIR}/scripts/embed_schemas.py
    COMMENT "Embedding LLM provider schemas"
)
add_custom_target(generate_schemas DEPENDS ${GENERATED_DIR}/builtin_schemas.h)

# ===========================================================================
# neograph::core — Graph engine + foundation types
# ===========================================================================
add_library(neograph_core STATIC
    src/core/graph_engine.cpp
    src/core/graph_state.cpp
    src/core/graph_node.cpp
    src/core/graph_loader.cpp
    src/core/graph_checkpoint.cpp
    src/core/react_graph.cpp
    src/core/store.cpp
)

target_include_directories(neograph_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(neograph_core
    PUBLIC  nlohmann_json Threads::Threads
    PRIVATE taskflow
)

add_library(neograph::core ALIAS neograph_core)

# ===========================================================================
# neograph::llm — LLM provider implementations (OpenAI, Schema, Agent)
# ===========================================================================
if(NEOGRAPH_BUILD_LLM)
    add_library(neograph_llm STATIC
        src/llm/openai_provider.cpp
        src/llm/schema_provider.cpp
        src/llm/agent.cpp
    )

    add_dependencies(neograph_llm generate_schemas)

    target_include_directories(neograph_llm
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${GENERATED_DIR}
    )

    target_link_libraries(neograph_llm
        PUBLIC  neograph_core
        PRIVATE httplib OpenSSL::SSL OpenSSL::Crypto
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
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(neograph_mcp
        PUBLIC  neograph_core
        PRIVATE httplib OpenSSL::SSL OpenSSL::Crypto
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
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
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
        target_link_libraries(example_react_agent PRIVATE neograph::core neograph::llm)

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

        if(NEOGRAPH_BUILD_MCP)
            add_executable(example_mcp_agent examples/03_mcp_agent.cpp)
            target_link_libraries(example_mcp_agent PRIVATE neograph::core neograph::llm neograph::mcp)
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
            target_link_libraries(example_clay_chatbot PRIVATE neograph::core neograph::llm raylib)
            target_include_directories(example_clay_chatbot PRIVATE ${DEPS_DIR})
        endif()
    endif()
endif()

# ===========================================================================
# Install (TODO: proper install with generator expressions for deps)
# ===========================================================================
include(GNUInstallDirs)

install(DIRECTORY include/neograph
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY schemas/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neograph/schemas)
