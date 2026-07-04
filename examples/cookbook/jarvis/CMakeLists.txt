# examples/cookbook/jarvis/CMakeLists.txt
#
# mock-first 빌드 전략:
#   - whisper / onnxruntime / miniaudio 를 찾지 못해도 빌드는 계속 진행.
#   - 각 의존성이 없으면 JARVIS_HAVE_WHISPER / JARVIS_HAVE_ONNXRUNTIME /
#     JARVIS_HAVE_MINIAUDIO 를 OFF 로 설정하고, 실제 호출 코드는
#     #ifdef 가드로 보호하는 방식.
#   - 골격(skeleton) 단계는 mock 구현만 사용하므로 외부 의존 없어도 컴파일 가능.
#
# NeoGraph 본체 CMakeLists.txt 에서 포함하는 방법:
#   option(NEOGRAPH_BUILD_COOKBOOK_JARVIS "Build the JARVIS voice cookbook" OFF)
#   if(NEOGRAPH_BUILD_COOKBOOK_JARVIS)
#       add_subdirectory(examples/cookbook/jarvis)
#   endif()

cmake_minimum_required(VERSION 3.16)

# 단독(standalone) 빌드 시에도 동작하도록
if(NOT TARGET neograph::core)
    project(jarvis_cookbook CXX)
    # C++20 — NeoGraph 본체가 이미 C++20 이라 링크 시 전파됨;
    # 안전을 위해 여기서도 명시.
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    message(STATUS "[jarvis] standalone build — looking for installed neograph package")
    find_package(neograph REQUIRED)
endif()

# NeoGraph 필수 타깃 확인 — 없으면 건너뜀
set(_jarvis_required_targets
    neograph::core
    neograph::llm
    neograph::mcp
    neograph::a2a
    neograph::async
)
foreach(tgt ${_jarvis_required_targets})
    if(NOT TARGET ${tgt})
        message(WARNING "[jarvis] missing target ${tgt} — JARVIS cookbook skipped.")
        return()
    endif()
endforeach()

# ── 외부 의존 — 없어도 빌드 계속 (mock-first 전략) ──────────────────────────

# 프로젝트 로컬 third_party 경로 (시스템 의존 0 — 모두 cookbook 안)
set(_jarvis_local_tp "${CMAKE_CURRENT_LIST_DIR}/third_party")

# onnxruntime — 없으면 JARVIS_HAVE_ONNXRUNTIME=OFF, 빌드는 계속
set(JARVIS_HAVE_ONNXRUNTIME OFF)
find_package(onnxruntime QUIET CONFIG)
if(onnxruntime_FOUND)
    set(JARVIS_HAVE_ONNXRUNTIME ON)
else()
    # third_party/onnxruntime 우선, 없으면 시스템 경로
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
if(NOT JARVIS_HAVE_ONNXRUNTIME)
    message(STATUS "[jarvis] onnxruntime not found — 실제 TTS/VAD 비활성화 (mock 빌드 계속)")
else()
    message(STATUS "[jarvis] onnxruntime OK — ${ONNXRUNTIME_LIB}")
endif()

# whisper.cpp — 없으면 JARVIS_HAVE_WHISPER=OFF, 빌드는 계속
set(JARVIS_HAVE_WHISPER OFF)
find_package(whisper QUIET CONFIG)
if(whisper_FOUND)
    set(JARVIS_HAVE_WHISPER ON)
else()
    # third_party/whisper_install 우선
    find_path(WHISPER_INCLUDE_DIR NAMES whisper.h
        HINTS ${_jarvis_local_tp}/whisper_install/include)
    find_library(WHISPER_LIB NAMES whisper
        HINTS ${_jarvis_local_tp}/whisper_install/lib)
    if(WHISPER_INCLUDE_DIR AND WHISPER_LIB)
        set(JARVIS_HAVE_WHISPER ON)
    endif()
endif()
if(NOT JARVIS_HAVE_WHISPER)
    message(STATUS "[jarvis] whisper.cpp not found — 실제 STT 비활성화 (mock 빌드 계속)")
else()
    message(STATUS "[jarvis] whisper.cpp OK — ${WHISPER_LIB}")
endif()

# supertonic helper.cpp — assets/download.sh 가 third_party/supertonic/ 에 받음
set(_jarvis_supertonic_helper "${_jarvis_local_tp}/supertonic/helper.cpp")
set(JARVIS_HAVE_SUPERTONIC OFF)
if(EXISTS "${_jarvis_supertonic_helper}")
    set(JARVIS_HAVE_SUPERTONIC ON)
    message(STATUS "[jarvis] supertonic helper OK — ${_jarvis_supertonic_helper}")
endif()

# miniaudio — 헤더 하나. 없으면 stub 헤더 생성 후 계속 진행.
set(JARVIS_HAVE_MINIAUDIO OFF)
find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
if(MINIAUDIO_INCLUDE_DIR)
    set(JARVIS_HAVE_MINIAUDIO ON)
else()
    # stub 헤더 생성 — mic_input.cpp / tts_output.cpp 가 #ifdef 없이
    # include 해도 컴파일이 끊기지 않게 빈 파일을 하나 만들어 둔다.
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

# ── 자비스 본체 실행파일 ────────────────────────────────────────────────────

set(_jarvis_sources
    src/main.cpp
    src/audio/mic_input.cpp
    src/audio/tts_output.cpp
    src/audio/audio_playback.cpp
    src/audio/miniaudio_impl.cpp
    src/stt/whisper_node.cpp
    src/orchestrator/mcp_catalog.cpp
    src/orchestrator/agent_dispatcher.cpp
    src/orchestrator/intent_router_node.cpp
    src/memory/conversation_store.cpp
    src/memory/json_file_store.cpp
)
if(JARVIS_HAVE_SUPERTONIC AND JARVIS_HAVE_ONNXRUNTIME)
    # supertonic helper.cpp 가 ONNX Runtime 헤더 의존. 둘 다 있을 때만 빌드.
    list(APPEND _jarvis_sources "${_jarvis_supertonic_helper}")
    # helper.cpp 가 C++17 시절 u8"..." 리터럴(const char*) 가정.
    # GCC 13 + C++20 에선 u8"" 가 const char8_t* 라 std::string/const char* 와
    # 충돌. -fpermissive 로 양방향 변환 허용.
    set_source_files_properties("${_jarvis_supertonic_helper}" PROPERTIES
        COMPILE_OPTIONS "-fno-char8_t;-fpermissive;-Wno-narrowing")
endif()

add_executable(cookbook_jarvis ${_jarvis_sources})

target_include_directories(cookbook_jarvis PRIVATE
    src
    ${MINIAUDIO_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/supertonic
)

# 의존성별 조건부 연결 / 정의 추가
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

# ── RPATH — third_party 의 .so 들을 실행 시 찾을 수 있게 박아둔다.
#         (시스템 /usr/local 에 안 깔리므로 LD_LIBRARY_PATH 의존 없이 동작)
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

# NeoGraph 타깃 + cppdotenv 는 항상 필요
target_link_libraries(cookbook_jarvis PRIVATE
    neograph::core
    neograph::llm
    neograph::mcp
    neograph::a2a
    neograph::async
    cppdotenv
)

# ── specialists (별도 A2A 서버 프로세스들) ──────────────────────────────────
# 자비스가 mode="delegate" 결정 시 A2A 로 위임하는 전문가들.
# 각자 별도 실행 파일 — run_session.sh 가 백그라운드로 띄움.

add_executable(cookbook_jarvis_specialist_coder
    specialists/coder/server.cpp)
target_link_libraries(cookbook_jarvis_specialist_coder PRIVATE
    neograph::core neograph::llm neograph::a2a neograph::async cppdotenv)

add_executable(cookbook_jarvis_specialist_researcher
    specialists/researcher/server.cpp)
target_link_libraries(cookbook_jarvis_specialist_researcher PRIVATE
    neograph::core neograph::llm neograph::a2a neograph::async cppdotenv)

# ── 설치 (assets 경로 보존) ─────────────────────────────────────────────────

install(TARGETS cookbook_jarvis DESTINATION bin)
install(DIRECTORY config DESTINATION share/jarvis)
# assets/ 는 빌드 시점에 모델 다운로드 스크립트가 채움 — 설치는 사용자 선택.
