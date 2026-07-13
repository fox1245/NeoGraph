# A throwaway downstream project. It knows nothing about NeoGraph's source tree
# — it only sees an install prefix, exactly like a real consumer would.
#
# If this configures, builds and runs, then `find_package(NeoGraph)` works. If
# it does not, the library is not consumable no matter how green the unit tests
# are (issue #92).
cmake_minimum_required(VERSION 3.16)
project(neograph_consumer LANGUAGES CXX)

find_package(NeoGraph CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE neograph::core)
