add_executable(neograph_tests
    test_graph_state.cpp
    test_graph_engine.cpp
    test_checkpoint.cpp
    test_store.cpp
    test_node.cpp
    test_loader.cpp
    test_json_path.cpp
    test_pending_writes.cpp
    test_plan_execute_graph.cpp
    test_signal_dispatch.cpp
    test_multi_node_resume.cpp
    test_scheduler.cpp
    test_node_execution.cpp
    test_compiler.cpp
    test_barrier_persistence.cpp
    test_coordinator.cpp
    test_sends_interrupt_order.cpp
    test_multi_send_events.cpp
    test_schema_provider_concurrent.cpp
    test_schema_provider_stream_usage.cpp
    test_rate_limited_provider.cpp
    test_admin_api_barrier_preservation.cpp
    test_intent_classifier_streaming.cpp
    test_incremental_checkpoint.cpp
)

# PostgresCheckpointStore integration tests are gated on NEOGRAPH_BUILD_POSTGRES
# at the build level (so projects without libpqxx still build the test
# binary) and on NEOGRAPH_TEST_POSTGRES_URL at runtime (skipped if unset).
if(NEOGRAPH_BUILD_POSTGRES)
    target_sources(neograph_tests PRIVATE test_postgres_checkpoint.cpp)
endif()

target_link_libraries(neograph_tests PRIVATE
    neograph::core
    neograph::llm
    httplib OpenSSL::SSL OpenSSL::Crypto
    GTest::gtest_main
)

if(NEOGRAPH_BUILD_POSTGRES)
    target_link_libraries(neograph_tests PRIVATE neograph::postgres)
endif()

include(GoogleTest)
gtest_discover_tests(neograph_tests)
