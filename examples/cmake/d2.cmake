# Area D2: core basics. Included from examples/CMakeLists.txt; see neograph_add_example() there.
# Each example links only neograph::core, so it builds in a Core-only configuration too.
neograph_add_example(stream_modes 100_stream_modes.cpp LINK neograph::core)
neograph_add_example(retry_policy 101_retry_policy.cpp LINK neograph::core)
neograph_add_example(node_interrupt 102_node_interrupt.cpp LINK neograph::core)
neograph_add_example(max_steps 103_max_steps.cpp LINK neograph::core)
neograph_add_example(channels_reducers 104_channels_reducers.cpp LINK neograph::core)
neograph_add_example(conditional_edges 105_conditional_edges.cpp LINK neograph::core)
