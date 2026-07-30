# Installed-consumer fixture for the optional Program component.
cmake_minimum_required(VERSION 3.16)
project(neograph_program_consumer LANGUAGES CXX)

option(NEOGRAPH_EXPECT_PROGRAM_COMPONENT "Require installed Program component" ON)
if(NEOGRAPH_EXPECT_PROGRAM_COMPONENT)
    find_package(NeoGraph CONFIG REQUIRED COMPONENTS program)
else()
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
