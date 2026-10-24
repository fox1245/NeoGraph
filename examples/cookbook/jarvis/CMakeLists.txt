# examples/cookbook/jarvis/CMakeLists.txt
#
# Mock-first build strategy:
#   - Continue building even if whisper / onnxruntime / miniaudio are not found.
#   - When a dependency is missing, set JARVIS_HAVE_WHISPER / JARVIS_HAVE_ONNXRUNTIME /
#     JARVIS_HAVE_MINIAUDIO to OFF and guard the actual call sites with
#     #ifdef blocks.
#   - The skeleton stage uses mock implementations only, so it compiles without external dependencies.
#
# How to include from the main NeoGraph CMakeLists.txt:
#   option(NEOGRAPH_BUILD_COOKBOOK_JARVIS "Build the JARVIS voice cookbook" OFF)
#   if(NEOGRAPH_BUILD_COOKBOOK_JARVIS)
#       add_subdirectory(examples/cookbook/jarvis)
#   endif()

cmake_minimum_required(VERSION 3.16)

# Support standalone builds as well
if(NOT TARGET neograph::core)
    project(jarvis_cookbook CXX)
    # C++20 — the main NeoGraph target already requires C++20 and propagates it on link;
    # set it explicitly here as well for safety.
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    message(STATUS "[jarvis] standalone build — looking for installed neograph package")
    find_package(NeoGraph CONFIG REQUIRED)
endif()

if(NOT TARGET SchemaProvider::runtime)
    find_package(SchemaProvider CONFIG REQUIRED)
endif()

if(TARGET cppdotenv)
    set(_jarvis_dotenv_target cppdotenv)
elseif(TARGET neograph::cppdotenv)
    set(_jarvis_dotenv_target neograph::cppdotenv)
else()
    message(FATAL_ERROR "[jarvis] installed neograph package must export cppdotenv")
endif()

# Check required NeoGraph targets — skip if any are missing
set(_jarvis_required_targets
    neograph::core
    neograph::llm
    neograph::mcp
    neograph::a2a
    neograph::async
    SchemaProvider::runtime
)
foreach(tgt ${_jarvis_required_targets})
    if(NOT TARGET ${tgt})
        message(WARNING "[jarvis] missing target ${tgt} — JARVIS cookbook skipped.")
        return()
    endif()
endforeach()

# ── External dependencies — continue building if missing (mock-first strategy) ──

option(NEOGRAPH_JARVIS_FORCE_MOCK
    "Build JARVIS without optional voice dependencies" OFF)

# Project-local third_party path (zero system dependencies — all inside the cookbook)
set(_jarvis_local_tp "${CMAKE_CURRENT_LIST_DIR}/third_party")

# onnxruntime — if missing, set JARVIS_HAVE_ONNXRUNTIME=OFF and continue building
set(JARVIS_HAVE_ONNXRUNTIME OFF)
if(NOT NEOGRAPH_JARVIS_FORCE_MOCK)
    find_package(onnxruntime QUIET CONFIG)
    if(onnxruntime_FOUND)
        set(JARVIS_HAVE_ONNXRUNTIME ON)
    else()
        # Prefer third_party/onnxruntime, fall back to system paths
        find_path(ONNXRUNTIME_INCLUDE_DIR
            NAMES onnxruntime_cxx_api.h
            HINTS ${_jarvis_local_tp}/onnxruntime/include
            PATH_SUFFIXES onnxruntime)
        find_library(ONNXRUNTIME_LIB
            NAMES onnxruntime libonnxruntime
            HINTS ${_jarvis_local_tp}/onnxruntime/lib)
        if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
            set(JARVIS_HAVE_ONNXRUNTIME ON)
        endif()
    endif()
endif()
if(NOT JARVIS_HAVE_ONNXRUNTIME)
    message(STATUS "[jarvis] onnxruntime not found — 실제 TTS/VAD 비활성화 (mock 빌드 계속)")
else()
    message(STATUS "[jarvis] onnxruntime OK — ${ONNXRUNTIME_LIB}")
endif()

# whisper.cpp — if missing, set JARVIS_HAVE_WHISPER=OFF and continue building
set(JARVIS_HAVE_WHISPER OFF)
if(NOT NEOGRAPH_JARVIS_FORCE_MOCK)
    find_package(whisper QUIET CONFIG)
    if(whisper_FOUND)
        set(JARVIS_HAVE_WHISPER ON)
    else()
        # Prefer third_party/whisper_install
        find_path(WHISPER_INCLUDE_DIR NAMES whisper.h
            HINTS ${_jarvis_local_tp}/whisper_install/include)
        find_library(WHISPER_LIB NAMES whisper
            HINTS ${_jarvis_local_tp}/whisper_install/lib)
        if(WHISPER_INCLUDE_DIR AND WHISPER_LIB)
            set(JARVIS_HAVE_WHISPER ON)
        endif()
    endif()
endif()
if(NOT JARVIS_HAVE_WHISPER)
    message(STATUS "[jarvis] whisper.cpp not found — 실제 STT 비활성화 (mock 빌드 계속)")
else()
    message(STATUS "[jarvis] whisper.cpp OK — ${WHISPER_LIB}")
endif()

# supertonic helper.cpp — downloaded into third_party/supertonic/ by assets/download.sh
set(_jarvis_supertonic_helper "${_jarvis_local_tp}/supertonic/helper.cpp")
set(JARVIS_HAVE_SUPERTONIC OFF)
if(NOT NEOGRAPH_JARVIS_FORCE_MOCK AND EXISTS "${_jarvis_supertonic_helper}")
    set(JARVIS_HAVE_SUPERTONIC ON)
    message(STATUS "[jarvis] supertonic helper OK — ${_jarvis_supertonic_helper}")
endif()

# miniaudio — single header. If missing, generate a stub header and continue.
set(JARVIS_HAVE_MINIAUDIO OFF)
if(NOT NEOGRAPH_JARVIS_FORCE_MOCK)
    find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h
        HINTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party
              ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
endif()
if(MINIAUDIO_INCLUDE_DIR AND NOT NEOGRAPH_JARVIS_FORCE_MOCK)
    set(JARVIS_HAVE_MINIAUDIO ON)
else()
    # Generate a stub header — create an empty file so mic_input.cpp / tts_output.cpp
    # compile without #ifdef guards around the #include.
    set(MINIAUDIO_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/miniaudio_stub")
    file(MAKE_DIRECTORY "${MINIAUDIO_STUB_DIR}")
    if(NOT EXISTS "${MINIAUDIO_STUB_DIR}/miniaudio.h")
        file(WRITE "${MINIAUDIO_STUB_DIR}/miniaudio.h"
            "// miniaudio stub — 실제 헤더가 없을 때 컴파일 오류 방지용\n"
            "// 실제 동작은 JARVIS_HAVE_MINIAUDIO 가 ON 일 때만 가능\n"
            "#pragma once\n")
    endif()
    set(MINIAUDIO_INCLUDE_DIR "${MINIAUDIO_STUB_DIR}")
    message(STATUS "[jarvis] miniaudio.h not found — stub 헤더 생성, mock 빌드 계속")
endif()

# ── Main JARVIS executable ──────────────────────────────────────────────────

set(_jarvis_sources
    src/main.cpp
    src/audio/mic_input.cpp
    src/audio/tts_output.cpp
    src/audio/audio_playback.cpp
    src/audio/miniaudio_impl.cpp
    src/stt/whisper_node.cpp
    src/stt/moonshine_node.cpp
    src/orchestrator/mcp_catalog.cpp
    src/orchestrator/agent_dispatcher.cpp
    src/orchestrator/intent_router_node.cpp
    src/memory/conversation_store.cpp
    src/memory/json_file_store.cpp
)
if(JARVIS_HAVE_SUPERTONIC AND JARVIS_HAVE_ONNXRUNTIME)
    # supertonic helper.cpp depends on ONNX Runtime headers. Build only when both are present.
    list(APPEND _jarvis_sources "${_jarvis_supertonic_helper}")
    # helper.cpp assumes C++17 u8"..." literals (const char*).
    # Under GCC 13 + C++20, u8"" is const char8_t*, which conflicts with std::string/const char*.
    # Allow conversions with -fpermissive.
    set_source_files_properties("${_jarvis_supertonic_helper}" PROPERTIES
        COMPILE_OPTIONS "-fno-char8_t;-fpermissive;-Wno-narrowing")
endif()

add_executable(cookbook_jarvis ${_jarvis_sources})

target_include_directories(cookbook_jarvis PRIVATE
    src
    ${MINIAUDIO_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/supertonic
)

# Conditional linking and compile definitions per dependency
if(JARVIS_HAVE_ONNXRUNTIME)
    if(onnxruntime_FOUND)
        target_link_libraries(cookbook_jarvis PRIVATE onnxruntime::onnxruntime)
    else()
        target_include_directories(cookbook_jarvis PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(cookbook_jarvis PRIVATE ${ONNXRUNTIME_LIB})
    endif()
    target_compile_definitions(cookbook_jarvis PRIVATE JARVIS_HAVE_ONNXRUNTIME=1)
endif()

if(JARVIS_HAVE_WHISPER)
    if(whisper_FOUND)
        target_link_libraries(cookbook_jarvis PRIVATE whisper)
    else()
        target_include_directories(cookbook_jarvis PRIVATE ${WHISPER_INCLUDE_DIR})
        target_link_libraries(cookbook_jarvis PRIVATE ${WHISPER_LIB})
    endif()
    target_compile_definitions(cookbook_jarvis PRIVATE JARVIS_HAVE_WHISPER=1)
endif()

if(JARVIS_HAVE_SUPERTONIC)
    target_compile_definitions(cookbook_jarvis PRIVATE JARVIS_HAVE_SUPERTONIC=1)
endif()

# ── RPATH — embed paths so third_party .so files are found at runtime.
#         (Runs without LD_LIBRARY_PATH since nothing is installed into /usr/local)
set_target_properties(cookbook_jarvis PROPERTIES
    BUILD_RPATH
        "${_jarvis_local_tp}/onnxruntime/lib;${_jarvis_local_tp}/whisper_install/lib"
    INSTALL_RPATH
        "${_jarvis_local_tp}/onnxruntime/lib;${_jarvis_local_tp}/whisper_install/lib"
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
)

if(JARVIS_HAVE_MINIAUDIO)
    target_compile_definitions(cookbook_jarvis PRIVATE JARVIS_HAVE_MINIAUDIO=1)
endif()

# NeoGraph targets + cppdotenv are always required
target_link_libraries(cookbook_jarvis PRIVATE
    neograph::core
    neograph::llm
    SchemaProvider::runtime
    neograph::mcp
    neograph::a2a
    neograph::async
    ${_jarvis_dotenv_target}
)

# ── Specialists (separate A2A server processes) ─────────────────────────────
# Specialists to which JARVIS delegates over A2A when it selects mode="delegate".
# Each is a separate executable launched in the background by run_session.sh.

add_executable(cookbook_jarvis_specialist_coder
    specialists/coder/server.cpp)
target_link_libraries(cookbook_jarvis_specialist_coder PRIVATE
    neograph::core neograph::llm neograph::a2a neograph::async SchemaProvider::runtime ${_jarvis_dotenv_target})

add_executable(cookbook_jarvis_specialist_researcher
    specialists/researcher/server.cpp)
target_link_libraries(cookbook_jarvis_specialist_researcher PRIVATE
    neograph::core neograph::llm neograph::a2a neograph::async SchemaProvider::runtime ${_jarvis_dotenv_target})

# ── Install (preserving assets path) ────────────────────────────────────────

install(TARGETS cookbook_jarvis DESTINATION bin)
install(DIRECTORY config DESTINATION share/jarvis)
# assets/ is populated at build time by the model download script — installing it is optional.
