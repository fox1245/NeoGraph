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
    test_deep_research_human_review.cpp
    test_async_conn_pool.cpp
    test_async_http_errors.cpp
    test_async_sse_parser.cpp
    test_async_http_stream.cpp
    test_async_http_options.cpp
)

# PostgresCheckpointStore integration tests are gated on NEOGRAPH_BUILD_POSTGRES
# at the build level (so projects without libpqxx still build the test
# binary) and on NEOGRAPH_TEST_POSTGRES_URL at runtime (skipped if unset).
if(NEOGRAPH_BUILD_POSTGRES)
    target_sources(neograph_tests PRIVATE test_postgres_checkpoint.cpp)
endif()

if(NEOGRAPH_BUILD_SQLITE)
    target_sources(neograph_tests PRIVATE test_sqlite_checkpoint.cpp)
endif()

target_link_libraries(neograph_tests PRIVATE
    neograph::core
    neograph::llm
    neograph::async
    httplib OpenSSL::SSL OpenSSL::Crypto
    GTest::gtest_main
)

if(NEOGRAPH_BUILD_POSTGRES)
    target_link_libraries(neograph_tests PRIVATE neograph::postgres)
endif()

if(NEOGRAPH_BUILD_SQLITE)
    target_link_libraries(neograph_tests PRIVATE neograph::sqlite)
endif()

include(GoogleTest)

if(NEOGRAPH_BUILD_POSTGRES)
    # PostgresCheckpointTest cases share one DB and call drop_schema in
    # SetUp/TearDown — running them in parallel collides on DDL. Mark
    # them with a RESOURCE_LOCK so ctest serializes only this group
    # while the other ~200 tests still run concurrently.
    gtest_discover_tests(neograph_tests
        TEST_FILTER "PostgresCheckpointTest.*"
        PROPERTIES RESOURCE_LOCK postgres_test_db
    )
    gtest_discover_tests(neograph_tests
        TEST_FILTER "-PostgresCheckpointTest.*"
    )
else()
    gtest_discover_tests(neograph_tests)
endif()
