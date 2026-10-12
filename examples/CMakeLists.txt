# Examples: smoke tests, the example index check, and a helper for new examples.
#
# Added from the top-level CMakeLists.txt after every older example target and after
# enable_testing(). It builds nothing by itself except what examples/cmake/*.cmake define.
#
# 1. example.<name> / cookbook.<name> (ctest, label examples-offline, 60 s): one test per built
#    example whose source declares `smoke-expect` lines in its leading comment block.
#    examples/run_example_smoke.py runs the binary with no API keys and checks the output.
# 2. examples/cmake/*.cmake: one file per area; define new example targets with
#    neograph_add_example() there. Files are included in name order. No other file needs editing.
#
# Run only these tests:  ctest -L examples-offline --output-on-failure

if(NOT NEOGRAPH_BUILD_EXAMPLES AND NOT NEOGRAPH_BUILD_TESTS)
    return()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

# Targets defined in this directory land beside the older example targets, in the build root.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(NEOGRAPH_EXAMPLES_DIR "${CMAKE_CURRENT_LIST_DIR}")

# neograph_add_example(<name> <source> [LINK <targets>...])
#   Builds examples/<source> as target example_<name>, with examples/ on its include path
#   (so it can include scripted_provider.h). The example is skipped, with a status message, when
#   a LINK target does not exist because its component is switched off.
function(neograph_add_example name source)
    cmake_parse_arguments(ARG "" "" "LINK" ${ARGN})
    foreach(_dep IN LISTS ARG_LINK)
        if(NOT TARGET ${_dep})
            message(STATUS "example_${name} skipped: ${_dep} is not built")
            return()
        endif()
    endforeach()
    add_executable(example_${name} "${NEOGRAPH_EXAMPLES_DIR}/${source}")
    target_link_libraries(example_${name} PRIVATE ${ARG_LINK})
    target_include_directories(example_${name} PRIVATE "${NEOGRAPH_EXAMPLES_DIR}")
endfunction()

if(NEOGRAPH_BUILD_EXAMPLES)
    file(GLOB _neograph_example_areas CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/cmake/*.cmake")
    list(SORT _neograph_example_areas)
    foreach(_area IN LISTS _neograph_example_areas)
        include("${_area}")
    endforeach()
endif()

if(NOT NEOGRAPH_BUILD_TESTS OR NOT Python3_Interpreter_FOUND)
    return()
endif()

if(NOT NEOGRAPH_BUILD_EXAMPLES)
    return()
endif()

# Smoke tests: every executable target built from a source under examples/ whose leading comment
# block declares `smoke-expect` (or `smoke-expect-line`). A source that declares `smoke-skip`
# instead is deliberately not run (live model, service or display needed).
get_property(_root_targets DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
get_property(_here_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _root_targets _here_targets)
    get_target_property(_type ${_target} TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
        continue()
    endif()
    get_target_property(_dir ${_target} SOURCE_DIR)
    get_target_property(_sources ${_target} SOURCES)
    foreach(_source IN LISTS _sources)
        if(IS_ABSOLUTE "${_source}")
            set(_path "${_source}")
        else()
            set(_path "${_dir}/${_source}")
        endif()
        if(NOT _path MATCHES "/examples/.*[.]cpp$" OR NOT EXISTS "${_path}")
            continue()
        endif()
        file(READ "${_path}" _header LIMIT 8192)
        string(FIND "${_header}" "smoke-expect" _declared)
        if(_declared EQUAL -1)
            continue()
        endif()
        string(REGEX REPLACE "^example_" "example." _name "${_target}")
        string(REGEX REPLACE "^cookbook_" "cookbook." _name "${_name}")
        add_test(NAME ${_name}
            COMMAND "${Python3_EXECUTABLE}" "${NEOGRAPH_EXAMPLES_DIR}/run_example_smoke.py"
                --source "${_path}" --exe "$<TARGET_FILE:${_target}>"
                --build-dir "${CMAKE_BINARY_DIR}" --python "${Python3_EXECUTABLE}")
        set_tests_properties(${_name} PROPERTIES
            LABELS "examples-offline" TIMEOUT 60 SKIP_RETURN_CODE 77)
        break()
    endforeach()
endforeach()
