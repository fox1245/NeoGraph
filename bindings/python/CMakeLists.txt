# ===========================================================================
# bindings/python — pybind11 Python bindings for NeoGraph
#
# Triggered by -DNEOGRAPH_BUILD_PYBIND=ON in the root CMakeLists. Output
# layout under the build dir:
#
#   build/
#     libneograph_core.so          ← engine, from root CMakeLists
#     libneograph_llm.so
#     libneograph_async.so
#     ...
#     neograph/                    ← Python package (this target)
#       __init__.py                ← copied from source tree at configure
#       _neograph.cpython-*.so     ← pybind11 module (build target)
#
# So a user running from the build dir can do:
#
#   PYTHONPATH=build python -c "import neograph; print(neograph.__version__)"
#
# RPATH on `_neograph.so` is `$ORIGIN/..` so it finds the libneograph_*.so
# siblings without LD_LIBRARY_PATH. Wheel-packaging downstream relocates
# both the package dir and the .so files into a single relocatable layout
# (see future bindings/python/README).
#
# pybind11 is fetched via FetchContent rather than vendored as a submodule
# — the engine itself doesn't need it (NEOGRAPH_BUILD_PYBIND is OFF by
# default), and the binding's only consumer is this directory.
# ===========================================================================

include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(pybind11)

# pybind11_add_module produces a Python extension target with the right
# suffix (.cpython-XYZ-PLATFORM.so / .pyd) and CXX visibility flags.
pybind11_add_module(_neograph
    src/module.cpp
    src/json_bridge.cpp
    src/bind_provider.cpp
    src/bind_state.cpp
    src/bind_graph.cpp
)

target_link_libraries(_neograph PRIVATE
    neograph::core
)
# Provider implementations (OpenAI, Schema) live in neograph::llm.
# Bind them only when llm is built; the binding compiles either way
# but `neograph.OpenAIProvider` etc. only show up when llm is present.
if(TARGET neograph::llm)
    target_link_libraries(_neograph PRIVATE neograph::llm)
    target_compile_definitions(_neograph PRIVATE NEOGRAPH_PYBIND_HAS_LLM)
endif()

# Locate the binding next to neograph/__init__.py inside the build tree
# so PYTHONPATH=build works out of the box. CMAKE_LIBRARY_OUTPUT_DIRECTORY
# is the canonical place for module-style libraries (which pybind11_add_module
# creates as MODULE-typed).
set(_NEOGRAPH_PY_PKG_DIR "${CMAKE_BINARY_DIR}/neograph")
set_target_properties(_neograph PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_NEOGRAPH_PY_PKG_DIR}"
)

# RPATH on the binding so it finds libneograph_core.so (and friends) at
# import time without LD_LIBRARY_PATH. The libs sit one level up from
# `build/neograph/_neograph.so` → `build/libneograph_*.so`.
if(UNIX AND NOT APPLE)
    set_target_properties(_neograph PROPERTIES
        BUILD_RPATH "$ORIGIN/..;$ORIGIN")
elseif(APPLE)
    set_target_properties(_neograph PROPERTIES
        BUILD_RPATH "@loader_path/..;@loader_path")
endif()

# Stage the Python source files alongside the binding in the build tree.
# Configure-time copy so editing __init__.py in source re-runs CMake; for
# day-to-day iteration use -DNEOGRAPH_BUILD_PYBIND=ON ; cmake --build then
# re-run cmake -B build to refresh.
configure_file(neograph/__init__.py "${_NEOGRAPH_PY_PKG_DIR}/__init__.py" COPYONLY)
configure_file(neograph/llm.py      "${_NEOGRAPH_PY_PKG_DIR}/llm.py"      COPYONLY)

# Make smoke tests runnable straight from the build tree:
#   ctest -V -R pybind_smoke
# Skipped if pytest isn't on the system Python.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NEOGRAPH_BUILD_TESTS)
    add_test(NAME pybind_smoke
        COMMAND "${Python3_EXECUTABLE}" -m pytest
                -q "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    set_tests_properties(pybind_smoke PROPERTIES
        ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}")
endif()
