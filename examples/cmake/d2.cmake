# Area D2: core basics. Included from examples/CMakeLists.txt; see neograph_add_example() there.
# Each example links only neograph::core, so it builds in a Core-only configuration too.
neograph_add_example(stream_modes 100_stream_modes.cpp LINK neograph::core)
