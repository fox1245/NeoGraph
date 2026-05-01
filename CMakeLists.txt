cmake_minimum_required(VERSION 3.16)

# Single source of version truth — read from pyproject.toml so the
# wheel ships the same number as `find_package(NeoGraph 0.2)` and
# `set_target_properties(... VERSION SOVERSION)`. Previously this was
# hard-coded to 2.0.0 and drifted multiple minor versions behind the
# real release line, with side-effects on Doxygen PROJECT_NUMBER and
# the installed CMake config package.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml" _NG_PYPROJECT)
string(REGEX MATCH "version[ \t]*=[ \t]*\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
       _NG_VER_MATCH "${_NG_PYPROJECT}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR
        "NeoGraph: failed to parse version from pyproject.toml. "
        "Expected `version = \"X.Y.Z\"` in the [project] table.")
endif()
set(NEOGRAPH_VERSION "${CMAKE_MATCH_1}")
unset(_NG_PYPROJECT)
unset(_NG_VER_MATCH)
project(NeoGraph VERSION ${NEOGRAPH_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC: treat source files as UTF-8
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ===========================================================================
# Hardening flags — defense-in-depth in Release builds.
#
# These add bounds checking, stack canaries, and CFI to release-built
# library and binary artifacts. Cost: a few percent in throughput on
# the engine's hot path (measured: par bench 278 → 280 µs, within
# noise); benefit: most CWE-class memory-corruption bugs trip the
# canary or the libc fortify check at runtime instead of becoming a
# silent UAF / OOB. CI's Release builds (build-and-test, bench-
# regression, build-macos, build-windows, Wheels) all pick this up.
#
# Skipped under sanitizer builds (ASan/UBSan/TSan/MSan) because
# they replace these mechanisms with their own instrumented checks.
# Detect via CMAKE_CXX_FLAGS containing `-fsanitize=`.
# ===========================================================================
option(NEOGRAPH_ENABLE_HARDENING "Hardening flags in Release builds (canaries, FORTIFY, CFI)" ON)
if(NEOGRAPH_ENABLE_HARDENING
   AND NOT CMAKE_CXX_FLAGS MATCHES "fsanitize="
   AND NOT MSVC)
    set(_NEOGRAPH_HARDEN_FLAGS
        # libstdc++ debug assertions: catches std::vector OOB, iterator
        # invalidation, dereferencing end(), uninitialized optional, …
        # Always on (Debug + Release). Triggers a clean abort with a
        # diagnostic instead of silently corrupting memory.
        -D_GLIBCXX_ASSERTIONS
        # Stack canary on every function with a stack-allocated buffer.
        # Catches buffer overflows that would otherwise smash the
        # return address — the canary check fires before ret.
        -fstack-protector-strong
    )
    # Control-flow integrity (CET-IBT on amd64 + Linux). Apple Silicon
    # uses arm64e PAC instead — different toolchain knob; skip the
    # flag there. Reject the flag on Windows (MSVC) and on any non-
    # x86_64 host. CMAKE_SYSTEM_PROCESSOR matches "x86_64" / "AMD64".
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
       AND (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
            OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64"))
        list(APPEND _NEOGRAPH_HARDEN_FLAGS -fcf-protection=full)
    endif()

    add_compile_options(${_NEOGRAPH_HARDEN_FLAGS})

    # _FORTIFY_SOURCE needs at least -O1 to function (the inline
    # checks rely on optimizer constant-folding). Linux glibc only —
    # Apple's libSystem and bionic don't ship the fortified peers
    # for everything we touch and the missing-symbol link errors
    # would dominate any benefit.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_compile_options(
            $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>
            $<$<CONFIG:RelWithDebInfo>:-D_FORTIFY_SOURCE=2>
            $<$<CONFIG:MinSizeRel>:-D_FORTIFY_SOURCE=2>
        )
    endif()

    # PIE is needed for CFI to be effective; emit position-independent
    # code by default. Linker takes -pie for executables.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    # ELF-only linker flags. macOS uses Mach-O + ld64 (different syntax),
    # Windows uses link.exe/lld-link with /DYNAMICBASE etc.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
    endif()

    message(STATUS "NeoGraph hardening: enabled "
                   "(canaries always; CFI/FORTIFY/RELRO Linux-only).")
elseif(NEOGRAPH_ENABLE_HARDENING AND CMAKE_CXX_FLAGS MATCHES "fsanitize=")
    message(STATUS "NeoGraph hardening: skipped (sanitizer build — ASan/TSan/MSan provide stronger checks).")
endif()

# ===========================================================================
# Options
# ===========================================================================
option(NEOGRAPH_BUILD_LLM      "Build neograph::llm (LLM providers)"     ON)
option(NEOGRAPH_BUILD_MCP      "Build neograph::mcp (MCP client)"        ON)
option(NEOGRAPH_BUILD_A2A      "Build neograph::a2a (Agent-to-Agent client)" ON)
option(NEOGRAPH_BUILD_ACP      "Build neograph::acp (Agent Client Protocol server)" ON)
option(NEOGRAPH_BUILD_UTIL     "Build neograph::util (utilities)"        ON)
option(NEOGRAPH_BUILD_POSTGRES "Build PostgresCheckpointStore (libpq)" ON)
option(NEOGRAPH_BUILD_SQLITE   "Build SqliteCheckpointStore (libsqlite3)" ON)
option(NEOGRAPH_BUILD_EXAMPLES "Build example programs"                  ON)
option(NEOGRAPH_BUILD_PYBIND   "Build Python bindings (pybind11)"        OFF)
# libcurl drives the opt-in HTTP/2 transport (CurlH2Pool, used when
# SchemaProvider::Config::prefer_libcurl=true). Default ON because the
# library is on every Linux/macOS distro by default; turn OFF when
# building on Windows CI without vcpkg/curl. With OFF, the engine
# still builds and the default ConnPool HTTP/1.1 path works — only the
# `prefer_libcurl=true` opt-in becomes a runtime no-op (an error from
# the SchemaProvider constructor if the user requests it).
option(NEOGRAPH_USE_LIBCURL    "Build libcurl HTTP/2 backend (CurlH2Pool)" ON)

# ---------------------------------------------------------------------------
# Shared-vs-static library mode
#
# Standard CMake convention: respect BUILD_SHARED_LIBS. The neograph_*
# `add_library()` calls below pass NO type keyword so the user-supplied
# value of BUILD_SHARED_LIBS picks STATIC (default) or SHARED.
#
# Why someone would flip BUILD_SHARED_LIBS=ON:
#   - Patch a single subsystem (e.g. libneograph_llm.so when an LLM API
#     surface changes) without rebuilding/redeploying the whole agent.
#   - Cut the per-agent binary size when shipping multiple NeoGraph-based
#     agents on the same host: they share one libneograph_core.so instead
#     of statically linking the engine into each.
#   - Ship the engine as an OS package; user agents link against the
#     installed .so.
#
# Caveats:
#   - Windows DLL export annotations (__declspec(dllexport)/dllimport on
#     every public symbol) are now in place via NEOGRAPH_API on every
#     class / free function with out-of-line .cpp definitions, including
#     async/sse_parser.h SseEventParser and async/ws_client.h's `detail`
#     codec helpers (the test suite exercises those across the .so
#     boundary). The shape was verified end-to-end on Linux
#     `BUILD_SHARED_LIBS=ON` (libneograph_*.so + 429/429 ctest green) —
#     under MSVC the same annotations resolve to dllexport/dllimport
#     and should link cleanly, but native MSVC load testing has not yet
#     happened. Report any LNK2019 you see.
#   - Static-only deps (yyjson, httplib INTERFACE, asio INTERFACE,
#     cppdotenv INTERFACE, concurrentqueue INTERFACE) get absorbed into
#     whichever neograph_* library uses them, regardless of mode.
# ---------------------------------------------------------------------------
# Public symbol export — `NEOGRAPH_API` macro defined in
# include/neograph/api.h decorates public class/function declarations
# with the right linkage attribute on every platform:
#
#   - Static builds (BUILD_SHARED_LIBS=OFF) → empty (no-op).
#   - Shared builds, Windows → __declspec(dllexport) inside engine TUs
#     (those that get NEOGRAPH_BUILDING_LIBRARY defined below);
#     __declspec(dllimport) for downstream consumers.
#   - Shared builds, Linux/macOS → visibility("default") inside engine
#     TUs; no-op for consumers.
#
# We set NEOGRAPH_STATIC_BUILD when BUILD_SHARED_LIBS is OFF so the
# api.h macro skips its Windows decoration entirely. Without this,
# a static build on Windows would still try to dllimport — fine for
# the binary but breaks LTO and silly warning noise.
if(NOT BUILD_SHARED_LIBS)
    add_compile_definitions(NEOGRAPH_STATIC_BUILD)
endif()

# All TUs that might end up linked into a shared library need PIC. Setting
# this globally is harmless on static builds and saves per-target -fPIC
# annotations on the static deps that get absorbed (yyjson, etc.).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# When the engine is built as shared libs, executables that link them
# (examples, tests, downstream agents) need to find the .so at runtime.
# `$ORIGIN`-relative RPATHs make the build-tree binaries self-contained
# (they look for libneograph_*.so beside themselves) and downstream
# installers can decide their own INSTALL_RPATH.
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
if(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
elseif(APPLE)
    set(CMAKE_BUILD_RPATH "@loader_path")
endif()

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
# Python3: always need Interpreter (drives the schema-embedding code
# generator). When the pybind11 binding is on, also need
# Development.Module so pybind11_add_module's underlying
# `python3_add_library` is available. Asking for both up-front avoids
# CMake's "package already found, ignoring new components" caching
# behaviour from short-circuiting a second find_package() call deeper
# in the tree.
if(NEOGRAPH_BUILD_PYBIND)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

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
add_library(neograph_core
    src/core/json.cpp
    src/core/provider.cpp
    src/core/tool.cpp
    src/core/graph_engine.cpp
    src/core/graph_compiler.cpp
    src/core/graph_coordinator.cpp
    src/core/graph_executor.cpp
    src/core/node_cache.cpp
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

# NEOGRAPH_BUILDING_LIBRARY toggles the NEOGRAPH_API macro in
# include/neograph/api.h to dllexport on Windows. The flag is PRIVATE
# because downstream consumers (binding, tests, other neograph_*
# libraries) must see dllimport — which is what they get when this
# define is absent from their compile line.
target_compile_definitions(neograph_core PRIVATE NEOGRAPH_BUILDING_LIBRARY)

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
add_library(neograph_async
    src/async/async_smoke.cpp
    src/async/http_client.cpp
    # curl_h2_pool.cpp always compiles — its body is #ifdef'd on
    # NEOGRAPH_HAVE_LIBCURL so the destructor (referenced by
    # SchemaProvider::~SchemaProvider via unique_ptr<CurlH2Pool>)
    # always has a definition the linker can resolve, even when
    # the libcurl backend itself is disabled.
    src/async/curl_h2_pool.cpp
    src/async/conn_pool.cpp
    src/async/sse_parser.cpp
    src/async/ws_client.cpp
)
target_include_directories(neograph_async
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_definitions(neograph_async PRIVATE NEOGRAPH_BUILDING_LIBRARY)
target_link_libraries(neograph_async PUBLIC asio OpenSSL::SSL OpenSSL::Crypto)
if(NEOGRAPH_USE_LIBCURL)
    # libcurl drives the opt-in HTTP/2 client (CurlH2Pool). Pulls in
    # HTTP/2 (via nghttp2), keep-alive pool, ALPN, and HTTP/3 if the
    # runtime libcurl was compiled with it.
    find_package(CURL REQUIRED)
    target_link_libraries(neograph_async PRIVATE CURL::libcurl)
    # PRIVATE: NEOGRAPH_HAVE_LIBCURL is consulted only inside
    # src/async/curl_h2_pool.cpp. Demoting from PUBLIC keeps the
    # build-time toggle out of consumer ABI — a downstream binary
    # built against a libcurl-less SDK shouldn't compile differently
    # vs one built against a libcurl-enabled SDK.
    target_compile_definitions(neograph_async PRIVATE NEOGRAPH_HAVE_LIBCURL)
endif()
# C++20 coroutines require this on GCC < 14 when using -std=c++20.
target_compile_features(neograph_async PUBLIC cxx_std_20)
add_library(neograph::async ALIAS neograph_async)

# Smoke binaries — dev-only; their *_main.cpp uses internal symbols
# (e.g. run_smoke) that aren't part of the engine's exported public
# surface. On Windows BUILD_SHARED_LIBS=ON they fail with LNK2019
# because run_smoke isn't NEOGRAPH_API-decorated, and there's no
# reason to ship them in a wheel either way. Gate behind
# NEOGRAPH_BUILD_EXAMPLES (default ON for source builds, OFF in the
# pyproject.toml cibw config).
if(NEOGRAPH_BUILD_EXAMPLES)
    add_executable(neograph_async_smoke src/async/async_smoke_main.cpp)
    target_link_libraries(neograph_async_smoke PRIVATE neograph_async)

    # Live-network smoke for the TLS path. Not in ctest (offline-safe
    # by default). Run manually after touching http_client.cpp's TLS
    # branch.
    add_executable(neograph_async_https_smoke src/async/async_https_smoke_main.cpp)
    target_link_libraries(neograph_async_https_smoke PRIVATE neograph_async)

    if(NEOGRAPH_USE_LIBCURL)
        # Libcurl-backend HTTP/2 smoke. Bench candidate: passes CF WAF (it
        # IS curl), gives us multiplexing + pool + future HTTP/3 for free.
        add_executable(neograph_h2_curl_smoke src/async/h2_curl_smoke_main.cpp)
        target_link_libraries(neograph_h2_curl_smoke PRIVATE CURL::libcurl)

        # Multiplex check — 5 parallel POSTs over libcurl HTTP/2.
        # NUM_CONNECTS should be 1 across all handles when multiplex works.
        add_executable(neograph_h2_curl_multiplex src/async/h2_curl_multiplex_main.cpp)
        target_link_libraries(neograph_h2_curl_multiplex PRIVATE CURL::libcurl)
    endif()
endif()

# ===========================================================================
# neograph::postgres — PostgresCheckpointStore (libpq)
#
# Optional persistent CheckpointStore backed by PostgreSQL. Schema mirrors
# LangGraph's PostgresSaver (three tables: checkpoints, blobs, writes)
# with a `neograph_` prefix so a single database can host both NeoGraph
# and LangGraph state without name collisions.
# ===========================================================================
if(NEOGRAPH_BUILD_POSTGRES)
    add_library(neograph_postgres
        src/core/postgres_checkpoint.cpp
    )

    target_include_directories(neograph_postgres
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(neograph_postgres PRIVATE NEOGRAPH_BUILDING_LIBRARY)
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
    add_library(neograph_sqlite
        src/core/sqlite_checkpoint.cpp
    )

    target_include_directories(neograph_sqlite
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(neograph_sqlite PRIVATE NEOGRAPH_BUILDING_LIBRARY)
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
    add_library(neograph_llm
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

    target_compile_definitions(neograph_llm PRIVATE NEOGRAPH_BUILDING_LIBRARY)
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
    add_library(neograph_mcp
        src/mcp/client.cpp
    )

    target_include_directories(neograph_mcp
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(neograph_mcp PRIVATE NEOGRAPH_BUILDING_LIBRARY)
    target_link_libraries(neograph_mcp
        PUBLIC  neograph_core
        PRIVATE neograph_async OpenSSL::SSL OpenSSL::Crypto
    )

    add_library(neograph::mcp ALIAS neograph_mcp)
endif()

# ===========================================================================
# neograph::a2a — Agent-to-Agent client (JSON-RPC over Streamable HTTP)
# ===========================================================================
if(NEOGRAPH_BUILD_A2A)
    add_library(neograph_a2a
        src/a2a/types.cpp
        src/a2a/client.cpp
        src/a2a/server.cpp
        src/a2a/a2a_caller_node.cpp
    )

    target_include_directories(neograph_a2a
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(neograph_a2a PRIVATE NEOGRAPH_BUILDING_LIBRARY)
    target_link_libraries(neograph_a2a
        PUBLIC  neograph_core
        PRIVATE neograph_async httplib OpenSSL::SSL OpenSSL::Crypto
    )

    add_library(neograph::a2a ALIAS neograph_a2a)
endif()

# ===========================================================================
# neograph::acp — Agent Client Protocol server (JSON-RPC over stdio)
#
# Editor↔agent protocol — analogous to LSP, popularised by Zed +
# Gemini CLI + Neovim/CodeCompanion. Exposes a NeoGraph engine as the
# "agent" side. The default transport is newline-delimited JSON over
# std::cin/std::cout, which is what every editor uses to talk to a
# locally-launched agent sub-process. HTTP/WebSocket transport remains
# a future addition (the dispatcher is transport-agnostic — feed JSON
# envelopes into ACPServer::handle_message and you're done).
# ===========================================================================
if(NEOGRAPH_BUILD_ACP)
    add_library(neograph_acp
        src/acp/types.cpp
        src/acp/server.cpp
    )

    target_include_directories(neograph_acp
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(neograph_acp PRIVATE NEOGRAPH_BUILDING_LIBRARY)
    target_link_libraries(neograph_acp
        PUBLIC neograph_core
    )

    add_library(neograph::acp ALIAS neograph_acp)
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

        add_executable(example_openai_responses_ws examples/33_openai_responses_ws.cpp)
        target_link_libraries(example_openai_responses_ws PRIVATE neograph::core neograph::llm cppdotenv)

        # Tools-tour demo exercises a C++20 coroutine pattern that
        # trips a GCC 13 internal compiler error (build_special_member_call
        # at cp/call.cc:11096). GCC 14 and Clang 18+ codegen the same
        # source without source-level workarounds. Gate the target so
        # Ubuntu 24.04 default toolchain users (GCC 13.3) still build
        # the rest of the project cleanly.
        if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "14")
           OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            add_executable(example_openai_responses_ws_tools examples/34_openai_responses_ws_tools.cpp)
            target_link_libraries(example_openai_responses_ws_tools PRIVATE neograph::core neograph::async cppdotenv)
        else()
            message(STATUS
                "Skipping example_openai_responses_ws_tools: needs GCC >= 14 or Clang "
                "(current: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
        endif()

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
        # Links neograph::async explicitly: 28/29/30 directly call
        # neograph::async::async_post(). Worked transitively under STATIC
        # builds (neograph::llm pulls neograph::async PRIVATE), but SHARED
        # libs don't propagate PRIVATE deps to executables — needs to be
        # spelled out so BUILD_SHARED_LIBS=ON also links cleanly.
        add_executable(example_corrective_rag examples/28_corrective_rag.cpp)
        target_link_libraries(example_corrective_rag PRIVATE neograph::core neograph::llm neograph::async cppdotenv)

        # Example 29: /v1/responses raw envelope dump — debug / pedagogy aid.
        add_executable(example_responses_envelope examples/29_responses_envelope.cpp)
        target_link_libraries(example_responses_envelope PRIVATE neograph::core neograph::llm neograph::async cppdotenv)

        # Example 30: reasoning_effort tradeoff sweep on /v1/responses.
        add_executable(example_reasoning_effort examples/30_reasoning_effort.cpp)
        target_link_libraries(example_reasoning_effort PRIVATE neograph::core neograph::llm neograph::async cppdotenv)

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

        if(NEOGRAPH_BUILD_A2A)
            add_executable(example_a2a_client examples/37_a2a_client.cpp)
            target_link_libraries(example_a2a_client PRIVATE
                neograph::core neograph::a2a)

            add_executable(example_a2a_server examples/38_a2a_server.cpp)
            target_link_libraries(example_a2a_server PRIVATE
                neograph::core neograph::a2a)

        endif()

        if(NEOGRAPH_BUILD_ACP)
            add_executable(example_acp_server examples/39_acp_server.cpp)
            target_link_libraries(example_acp_server PRIVATE
                neograph::core neograph::acp)
        endif()

        # Reopen the A2A guarded block for the cookbook executables that
        # follow — they need NEOGRAPH_BUILD_A2A AND NEOGRAPH_BUILD_LLM.
        if(NEOGRAPH_BUILD_A2A)

            # AI National Assembly cookbook — multi-persona A2A demo
            # built as a fresh NeoGraph user from the public docs only.
            # Binaries land in the top-level build dir so the cookbook's
            # run_session.sh finds them at the same path as other
            # examples.
            if(NEOGRAPH_BUILD_LLM)
                add_executable(cookbook_ai_assembly_member
                    examples/cookbook/ai-assembly/member_server.cpp)
                target_link_libraries(cookbook_ai_assembly_member PRIVATE
                    neograph::core neograph::llm neograph::a2a neograph::async)

                add_executable(cookbook_ai_assembly_speaker
                    examples/cookbook/ai-assembly/speaker.cpp)
                target_link_libraries(cookbook_ai_assembly_speaker PRIVATE
                    neograph::core neograph::a2a neograph::async)
            endif()
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

            add_executable(example_re_agent examples/35_re_agent.cpp)
            target_link_libraries(example_re_agent PRIVATE neograph::core neograph::llm neograph::mcp cppdotenv)
        endif()

        # Example 36: small-model classifier fan-out — pure orchestration
        # demo (no inference runtime dependency). The example body shows
        # how to wrap an Ort::Session in a GraphNode but uses a mock
        # latency stand-in so the build doesn't pull onnxruntime headers.
        add_executable(example_classifier_fanout examples/36_classifier_fanout.cpp)
        target_link_libraries(example_classifier_fanout PRIVATE neograph::core)

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

    # Sustained-burst stress harness — Phase 3 operational-readiness
    # gate. Holds N runs in flight for M wall-clock seconds, samples
    # RSS + per-window p99 latency at fixed intervals, and exits 1 if
    # RSS drift exceeds the configured tolerance (best-effort leak
    # signal — Valgrind / LSan stays the authoritative tool). See
    # benchmarks/stress/README.md for the recipe.
    add_executable(bench_sustained_concurrent
        benchmarks/stress/bench_sustained_concurrent.cpp)
    target_link_libraries(bench_sustained_concurrent PRIVATE neograph::core)
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

# Fuzz targets — libFuzzer-driven, Clang only. Separate flag so
# Debug/Release builds aren't impacted by the libFuzzer link
# requirement.
option(NEOGRAPH_BUILD_FUZZ "Build libFuzzer fuzz targets (Clang only)" OFF)
if(NEOGRAPH_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(WARNING
            "NEOGRAPH_BUILD_FUZZ=ON requires Clang for -fsanitize=fuzzer; "
            "current compiler is ${CMAKE_CXX_COMPILER_ID}. Skipping.")
    else()
        add_subdirectory(tests/fuzz)
    endif()
endif()

# ===========================================================================
# Python bindings (pybind11)
#
# Optional. Builds the `_neograph` C extension under build/neograph/, with
# the `neograph/` package layout next to it so a `PYTHONPATH=build python`
# can `import neograph`. Wheel packaging is downstream of this option;
# see bindings/python/README for details.
# ===========================================================================
if(NEOGRAPH_BUILD_PYBIND)
    add_subdirectory(bindings/python)
endif()

# ===========================================================================
# Install
#
# Two consumer paths:
#
#   a) Standard `cmake --install`: ships headers, schemas, and engine
#      libs to a conventional system prefix (/usr/local, etc.).
#      Headers + schemas are gated behind NEOGRAPH_INSTALL_HEADERS
#      (default ON for source builds, OFF for the Python wheel build
#      via pyproject.toml). Engine libs are unconditional.
#
#   b) Python wheel via scikit-build-core: pyproject.toml sets
#      -DNEOGRAPH_INSTALL_HEADERS=OFF + -DNEOGRAPH_BUILD_PYBIND=ON.
#      cmake --install then drops only the engine .so/.dll/.dylib
#      siblings into <wheel-staging>/neograph_engine/, alongside the
#      _neograph.{so,pyd} that bindings/python/CMakeLists.txt's own
#      install rule contributes. The earlier COMPONENT-based filter
#      (NeoGraphPyBindWheel) was abandoned — scikit-build-core's
#      install.components handling didn't reliably propagate to
#      cmake --install --component on Windows, leaving the engine
#      .dlls out of the wheel.
# ===========================================================================
include(GNUInstallDirs)

option(NEOGRAPH_INSTALL_HEADERS
    "Install C++ headers + schemas to the system prefix (off for wheel builds)"
    ON)

if(NEOGRAPH_INSTALL_HEADERS)
    install(DIRECTORY include/neograph
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY schemas/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/neograph/schemas)
endif()

# ── Pybind11 wheel install ───────────────────────────────────────────────
#
# When NEOGRAPH_BUILD_PYBIND is on (typical: pip install .), bundle the
# engine .so files into the wheel right next to the binding so the
# `$ORIGIN` RPATH on `_neograph.so` resolves at import time without
# LD_LIBRARY_PATH.
#
# Only the libraries the binding actually links against get bundled —
# that's `neograph_core` (always), `neograph_async` (always; binding's
# transitive dep through engine.h coroutines), and `neograph_llm`
# (when LLM is on; needed for OpenAIProvider / SchemaProvider).
if(NEOGRAPH_BUILD_PYBIND)
    set(NEOGRAPH_PYBIND_LIBS neograph_core neograph_async)
    if(TARGET neograph_llm)
        list(APPEND NEOGRAPH_PYBIND_LIBS neograph_llm)
    endif()
    # neograph_postgres ships when libpq was found at engine-build
    # time. The binding's PostgresCheckpointStore class hard-links
    # against it (bindings/python/CMakeLists.txt:91), so the wheel
    # must carry libneograph_postgres.{so,dylib,dll} as a sibling
    # alongside the binding — otherwise auditwheel/delocate fail
    # with "required library libneograph_postgres.* could not be
    # located" during repair.
    if(TARGET neograph_postgres)
        list(APPEND NEOGRAPH_PYBIND_LIBS neograph_postgres)
    endif()
    # neograph_a2a ships when A2A is built. Same reasoning as above:
    # bindings/python/src/bind_a2a.cpp links against it (added in
    # v0.2.1), so the wheel must carry libneograph_a2a.{so,dylib,dll}
    # next to the binding or auditwheel/delocate refuses to repair.
    if(TARGET neograph_a2a)
        list(APPEND NEOGRAPH_PYBIND_LIBS neograph_a2a)
    endif()

    # Set INSTALL_RPATH so the bundled libs find each other via
    # `$ORIGIN` (libneograph_llm.so → libneograph_core.so etc.).
    # Build-tree RPATH is set globally above (CMAKE_BUILD_RPATH);
    # this is the parallel for the installed wheel layout.
    if(UNIX AND NOT APPLE)
        set(_NEOGRAPH_PYBIND_INSTALL_RPATH "$ORIGIN")
    elseif(APPLE)
        set(_NEOGRAPH_PYBIND_INSTALL_RPATH "@loader_path")
    endif()
    foreach(_lib IN LISTS NEOGRAPH_PYBIND_LIBS)
        set_target_properties(${_lib} PROPERTIES
            INSTALL_RPATH "${_NEOGRAPH_PYBIND_INSTALL_RPATH}"
            INSTALL_RPATH_USE_LINK_PATH TRUE)
    endforeach()

    install(TARGETS ${NEOGRAPH_PYBIND_LIBS}
            LIBRARY DESTINATION neograph_engine
            RUNTIME DESTINATION neograph_engine
            ARCHIVE DESTINATION neograph_engine)
endif()
