add_executable(neograph_tests
    test_graph_state.cpp
    test_graph_engine.cpp
    test_checkpoint.cpp
    test_store.cpp
    test_node.cpp
    test_loader.cpp
    test_json_path.cpp
    test_pending_writes.cpp
)

target_link_libraries(neograph_tests PRIVATE
    neograph::core
    neograph::llm
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(neograph_tests)
