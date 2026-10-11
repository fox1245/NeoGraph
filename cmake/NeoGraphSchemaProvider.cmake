set(NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR "" CACHE PATH
    "Optional SchemaProvider source checkout; otherwise prefer an installed package")
option(NEOGRAPH_FETCH_SCHEMAPROVIDER
    "Download the pinned SchemaProvider source if no installed package is available" ON)

# Pin the SDK source, not a moving branch or release tag.
set(NEOGRAPH_SCHEMAPROVIDER_REVISION
    "83112573ba59e3b561fc33c22394638be7aa5294")

if(NOT NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR)
    find_package(SchemaProvider 0.3.0 EXACT CONFIG QUIET COMPONENTS runtime transport)
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

if(NOT TARGET SchemaProvider::runtime OR NOT TARGET SchemaProvider::transport)
    message(FATAL_ERROR "SchemaProvider does not provide its required runtime and transport targets")
endif()
