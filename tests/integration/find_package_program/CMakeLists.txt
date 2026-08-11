# Installed-consumer fixture for the optional Program component.
cmake_minimum_required(VERSION 3.16)
project(neograph_program_consumer LANGUAGES C CXX)

option(NEOGRAPH_EXPECT_PROGRAM_COMPONENT "Require installed Program component" ON)
option(NEOGRAPH_TEST_REQUIRED_PROGRAM_REJECTION
       "Require a disabled Program component and expect package rejection" OFF)
option(NEOGRAPH_EXPECT_QUICKJS_CONTROL
       "Exercise the installed Program component with a co-linked second QuickJS" OFF)
if(NEOGRAPH_EXPECT_PROGRAM_COMPONENT)
    find_package(NeoGraph CONFIG REQUIRED COMPONENTS program)
else()
    # Package discovery must override ambient caller state even when Program is
    # not requested, then do the same for optional and required lookups.
    set(NeoGraph_program_FOUND TRUE)
    find_package(NeoGraph CONFIG REQUIRED)
    if(NeoGraph_program_FOUND)
        message(FATAL_ERROR
            "Disabled unrequested NeoGraph Program component retained caller state")
    endif()

    set(NeoGraph_program_FOUND TRUE)
    if(NEOGRAPH_TEST_REQUIRED_PROGRAM_REJECTION)
        find_package(NeoGraph CONFIG REQUIRED COMPONENTS program)
        message(FATAL_ERROR
            "Disabled NeoGraph Program component unexpectedly satisfied a required lookup")
    endif()

    find_package(NeoGraph CONFIG REQUIRED OPTIONAL_COMPONENTS program)
    if(NeoGraph_program_FOUND
       OR TARGET neograph::program
       OR "program" IN_LIST NEOGRAPH_COMPONENTS_BUILT)
        message(FATAL_ERROR "Disabled NeoGraph Program component was discoverable")
    endif()
    return()
endif()

if(NOT TARGET neograph::program)
    message(FATAL_ERROR "NeoGraph reported Program but exported no neograph::program target")
endif()
if(NOT "program" IN_LIST NEOGRAPH_COMPONENTS_BUILT)
    message(FATAL_ERROR "NeoGraph Program target is missing from NEOGRAPH_COMPONENTS_BUILT")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE neograph::program)
target_compile_features(consumer PRIVATE cxx_std_20)

# Compile a downstream consumer as C11 and link it through the C++ Program
# target. This keeps the public native ABI independent of C++ object layouts.
add_executable(native_abi_consumer native_abi_consumer.c)
set_target_properties(native_abi_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    LINKER_LANGUAGE CXX)
target_link_libraries(native_abi_consumer PRIVATE neograph::program)

if(NEOGRAPH_EXPECT_QUICKJS_CONTROL)
    target_compile_definitions(consumer PRIVATE NEOGRAPH_FIND_PACKAGE_QUICKJS_CONTROL=1)
    add_library(second_quickjs STATIC second_quickjs.c)
    set_target_properties(second_quickjs PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)

    add_executable(dual_quickjs_consumer dual_quickjs_consumer.cpp)
    target_compile_features(dual_quickjs_consumer PRIVATE cxx_std_20)
    target_link_libraries(dual_quickjs_consumer PRIVATE
        neograph::program
        second_quickjs)
endif()
