set(NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR "" CACHE PATH
    "Optional SchemaProvider source checkout; otherwise prefer an installed package")
option(NEOGRAPH_FETCH_SCHEMAPROVIDER
    "Download the pinned SchemaProvider source if no installed package is available" ON)

# Pin the SDK source, not a moving branch or release tag.
set(NEOGRAPH_SCHEMAPROVIDER_REVISION
    "812d4808f777a3eb3e6b16cba04f884e4e19fe3a")

if(NOT NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR)
    find_package(SchemaProvider 0.1.0 CONFIG QUIET COMPONENTS runtime)
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

if(NOT TARGET SchemaProvider::runtime)
    message(FATAL_ERROR "SchemaProvider does not provide its required runtime target")
endif()
