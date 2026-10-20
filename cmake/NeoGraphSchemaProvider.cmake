set(NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR "" CACHE PATH
    "Optional SchemaProvider source checkout; otherwise prefer an installed package")
option(NEOGRAPH_FETCH_SCHEMAPROVIDER
    "Download the pinned SchemaProvider source if no installed package is available" ON)

# Pin the SDK source, not a moving branch or release tag.
set(NEOGRAPH_SCHEMAPROVIDER_REVISION
    "34fd00f7e0eb3e43878cdc11b4e84d5b44d21d46")

# What NeoGraph needs of the SDK: the runtime, and the libcurl transport wherever the SDK can have one. The
# transport runs its own I/O threads, so a single-thread build has none, and neither has a WebAssembly host.
set(NEOGRAPH_SCHEMAPROVIDER_COMPONENTS runtime)
set(_neograph_sp_transport OFF)
if(NEOGRAPH_THREADS AND NOT NEOGRAPH_WASM_HOST)
    list(APPEND NEOGRAPH_SCHEMAPROVIDER_COMPONENTS transport)
    set(_neograph_sp_transport ON)
endif()

# The JavaScript module sends over the SDK's JavaScript host (an executor over setTimeout and an HTTP transport over
# fetch). It is not part of the installed dependency list: only the module links it.
set(_neograph_sp_required_components ${NEOGRAPH_SCHEMAPROVIDER_COMPONENTS})
if(NEOGRAPH_BUILD_WASM_JS)
    list(APPEND _neograph_sp_required_components js_host)
    set(SP_BUILD_JS_HOST ON CACHE BOOL "Build the SDK's JavaScript host (follows NEOGRAPH_BUILD_WASM_JS)" FORCE)
endif()

# A source build of the SDK takes NeoGraph's thread mode and transport. An SDK that is already built is checked
# below. The values follow NEOGRAPH_THREADS, also in a build directory that changes it.
set(SP_THREADS ${NEOGRAPH_THREADS} CACHE BOOL "Build SchemaProvider with OS threads (follows NEOGRAPH_THREADS)" FORCE)
set(SP_BUILD_TRANSPORT ${_neograph_sp_transport} CACHE STRING
    "Build the libcurl transport (follows NEOGRAPH_THREADS)" FORCE)
unset(_neograph_sp_transport)

if(NOT NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR)
    find_package(SchemaProvider 0.4.0 EXACT CONFIG QUIET COMPONENTS ${_neograph_sp_required_components})
    if(NOT SchemaProvider_FOUND)
        if(NOT NEOGRAPH_FETCH_SCHEMAPROVIDER)
            message(FATAL_ERROR
                "SchemaProvider runtime not found. Set CMAKE_PREFIX_PATH or "
                "NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR, or enable NEOGRAPH_FETCH_SCHEMAPROVIDER.")
        endif()
        include(FetchContent)
        if(POLICY CMP0135)
            cmake_policy(SET CMP0135 NEW)
        endif()
        FetchContent_Declare(neograph_schemaprovider
            URL "https://github.com/fox1245/SchemaProvider/archive/${NEOGRAPH_SCHEMAPROVIDER_REVISION}.tar.gz")
        # Use NeoGraph's checked-in parser and Asio headers for the source SDK.
        set(ASIO_ROOT "${DEPS_DIR}/asio")
        set(YYJSON_ROOT "${DEPS_DIR}/yyjson")
        set(SP_BUILD_TESTS OFF CACHE BOOL "Build SchemaProvider tests")
        set(SP_BUILD_BENCHMARKS OFF CACHE BOOL "Build SchemaProvider benchmarks")
        FetchContent_MakeAvailable(neograph_schemaprovider)
        if(NEOGRAPH_BUILD_PYBIND)
            # Only the SDK runtime libraries belong in the Python wheel, not
            # its C++ headers, CMake exports, canaries or qualification tools.
            set_property(DIRECTORY "${neograph_schemaprovider_SOURCE_DIR}"
                PROPERTY EXCLUDE_FROM_ALL TRUE)
        endif()
    endif()
else()
    set(ASIO_ROOT "${DEPS_DIR}/asio")
    set(YYJSON_ROOT "${DEPS_DIR}/yyjson")
    if(NEOGRAPH_BUILD_PYBIND)
        add_subdirectory("${NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/schemaprovider" EXCLUDE_FROM_ALL)
    else()
        add_subdirectory("${NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/schemaprovider")
    endif()
endif()

foreach(_component IN LISTS _neograph_sp_required_components)
    if(NOT TARGET SchemaProvider::${_component})
        message(FATAL_ERROR "SchemaProvider does not provide its required ${_component} target")
    endif()
endforeach()

# The SDK exports ASIO_DISABLE_THREADS from a single-thread build. Its headers refuse a translation unit that
# disagrees, so say so at configure time instead of at the first include.
get_target_property(_neograph_sp_definitions SchemaProvider::runtime INTERFACE_COMPILE_DEFINITIONS)
if(_neograph_sp_definitions AND "ASIO_DISABLE_THREADS" IN_LIST _neograph_sp_definitions)
    if(NEOGRAPH_THREADS)
        message(FATAL_ERROR
            "SchemaProvider was built without threads (SP_THREADS=OFF), but NEOGRAPH_THREADS=ON. "
            "Build both the same way.")
    endif()
elseif(NOT NEOGRAPH_THREADS)
    message(FATAL_ERROR
        "SchemaProvider was built with threads (SP_THREADS=ON), but NEOGRAPH_THREADS=OFF. "
        "Build both the same way.")
endif()
unset(_neograph_sp_definitions)
unset(_neograph_sp_required_components)
