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
#     neograph_engine/             ← Python package (this target)
#       __init__.py                ← copied from source tree at configure
#       _neograph.cpython-*.so     ← pybind11 module (build target)
#
# So a user running from the build dir can do:
#
#   PYTHONPATH=build python -c "import neograph_engine; print(neograph_engine.__version__)"
#
# Note: distribution name (PyPI) is `neograph-engine`, Python import
# name is `neograph_engine`. The C extension itself stays `_neograph`
# (private; lives inside the package). The bare `neograph` name on
# PyPI was already taken by an unrelated LangGraph wrapper.
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

# Resolve pybind11. Two paths:
#
#   1. `pip install .` / scikit-build-core supplies pybind11 via the
#      build-system requirement; CMAKE_PREFIX_PATH already includes
#      its install dir. `find_package(pybind11 CONFIG)` succeeds and
#      we use the pip-managed version (no network during install).
#   2. Direct `cmake -DNEOGRAPH_BUILD_PYBIND=ON` from a developer
#      checkout: `find_package` fails, fall through to FetchContent
#      to download pybind11. Vendored into the build dir; no commit
#      to the source tree.
#
# Both produce the same `pybind11::module` import target.
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    message(STATUS
        "pybind11 not found via CMAKE_PREFIX_PATH — falling back to "
        "FetchContent. Set pybind11_DIR or install pybind11 via pip "
        "to avoid the download.")
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.13.6
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# pybind11_add_module produces a Python extension target with the right
# suffix (.cpython-XYZ-PLATFORM.so / .pyd) and CXX visibility flags.
pybind11_add_module(_neograph
    src/module.cpp
    src/json_bridge.cpp
    src/bind_provider.cpp
    src/bind_state.cpp
    src/bind_graph.cpp
    src/bind_node.cpp
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

# Locate the binding next to neograph_engine/__init__.py inside the build
# tree so PYTHONPATH=build works out of the box. CMAKE_LIBRARY_OUTPUT_DIRECTORY
# is the canonical place for module-style libraries (which pybind11_add_module
# creates as MODULE-typed).
set(_NEOGRAPH_PY_PKG_DIR "${CMAKE_BINARY_DIR}/neograph_engine")
set_target_properties(_neograph PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_NEOGRAPH_PY_PKG_DIR}"
)

# RPATH on the binding so it finds libneograph_core.so (and friends).
#
# Build tree: the engine .so files live one level up from the binding
# (`build/libneograph_*.so` vs `build/neograph_engine/_neograph.so`), so the
# build RPATH includes both `$ORIGIN/..` and `$ORIGIN`.
#
# Install / wheel: the engine .so files are bundled NEXT TO the
# binding (`neograph_engine/_neograph.so` + `neograph_engine/libneograph_*.so`),
# so the install RPATH is just `$ORIGIN`. CMake honours the install
# RPATH only on `cmake --install`; the build-tree binary still uses
# the BUILD_RPATH above.
if(UNIX AND NOT APPLE)
    set_target_properties(_neograph PROPERTIES
        BUILD_RPATH   "$ORIGIN/..;$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
elseif(APPLE)
    set_target_properties(_neograph PROPERTIES
        BUILD_RPATH   "@loader_path/..;@loader_path"
        INSTALL_RPATH "@loader_path")
endif()

# Stage the Python source files alongside the binding in the build tree.
# Configure-time copy so editing __init__.py in source re-runs CMake; for
# day-to-day iteration use -DNEOGRAPH_BUILD_PYBIND=ON ; cmake --build then
# re-run cmake -B build to refresh.
#
# (For the wheel build path: scikit-build-core copies these via the
# `wheel.packages = ["bindings/python/neograph"]` directive in
# pyproject.toml — no install rule needed here for them.)
configure_file(neograph_engine/__init__.py "${_NEOGRAPH_PY_PKG_DIR}/__init__.py" COPYONLY)
configure_file(neograph_engine/llm.py      "${_NEOGRAPH_PY_PKG_DIR}/llm.py"      COPYONLY)

# Install rule for the wheel build. Tagged with the same component as
# the engine .so files in the root CMakeLists, so a single
# `cmake --install --component NeoGraphPyBindWheel` brings everything
# into the wheel staging dir.
install(TARGETS _neograph
    LIBRARY DESTINATION neograph_engine
    RUNTIME DESTINATION neograph_engine)

# Install the Python sources via cmake too (instead of relying on
# scikit-build-core's `wheel.packages = [...]` directive). Empirically
# wheel.packages produced an inconsistent result with cmake-install on
# Windows (engine .dll siblings missing from the staged wheel even
# though Linux/macOS staged correctly). Routing both Python sources
# AND engine .dlls through the same install mechanism keeps the
# Windows wheel consistent with the other platforms.
install(FILES neograph_engine/__init__.py neograph_engine/llm.py
    DESTINATION neograph_engine)

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
