add_executable(neograph_tests
    test_graph_state.cpp
    test_graph_engine.cpp
    test_engine_config.cpp
    test_checkpoint.cpp
    test_store.cpp
    test_node.cpp
    test_node_header.cpp
    test_loader.cpp
    test_schema_export.cpp
    test_json_path.cpp
    test_pending_writes.cpp
    test_channel_write_mode.cpp
    test_usage_accounting.cpp
    test_subgraph_context.cpp
    test_subgraph_output_deltas.cpp
    test_plan_execute_graph.cpp
    test_signal_dispatch.cpp
    test_multi_node_resume.cpp
    test_resume_if_exists.cpp
    test_cancel_token_propagation.cpp
    test_live_llm_cancel_fanout_cpp.cpp
    test_scheduler.cpp
    test_node_execution.cpp
    test_tool_dispatch_parity.cpp
    test_tool_execution.cpp
    test_host_admission.cpp
    test_compiler.cpp
    test_compiler_strict.cpp
    test_validator.cpp
    test_graph_registry_local.cpp
    test_pbt_roundtrip.cpp
    test_corpus.cpp
    test_elaborator.cpp
    test_evolution.cpp
    test_harness_bench.cpp
    test_schema_evolution.cpp
    test_barrier_persistence.cpp
    test_coordinator.cpp
    test_sends_interrupt_order.cpp
    test_dynamic_interrupt.cpp
    test_tool_gate.cpp
    test_multi_send_events.cpp
    test_multi_send_routing.cpp
    test_schema_provider_concurrent.cpp
    test_schema_provider_async.cpp
    test_schema_provider_stream_usage.cpp
    test_schema_provider_responses_stream.cpp
    test_schema_provider_stop_reason.cpp
    test_rate_limited_provider.cpp
    test_admin_api_barrier_preservation.cpp
    test_intent_classifier_streaming.cpp
    test_incremental_checkpoint.cpp
    test_deep_research_human_review.cpp
    test_async_conn_pool.cpp
    test_async_http_errors.cpp
    test_async_sse_parser.cpp
    test_async_ws_codec.cpp
    test_async_ws_loopback.cpp
    test_schema_provider_ws_responses.cpp
    test_async_http_stream.cpp
    test_async_http_options.cpp
    test_provider_async_default.cpp
    test_provider_invoke_default.cpp
    test_completion_provider.cpp
    test_artifact_provider.cpp
    test_run_context_store.cpp
    test_run_result_channel.cpp
    test_typed_events.cpp
    test_error_messages_friendly.cpp
    test_schema_provider_extra_fields_temperature.cpp
    test_openai_provider_async.cpp
    test_checkpoint_async_default.cpp
    test_graph_engine_async_api.cpp
    test_async_tool.cpp
    test_executor_async_retry.cpp
    test_executor_async_parallel.cpp
    test_async_run_sync_pool.cpp
    test_node_cache.cpp
    test_node_run_dispatch.cpp
    test_cancel_token_fork.cpp
    test_concurrent_stress.cpp
    test_openinference_cpp.cpp
    test_schema_provider_stream_async_outer_io.cpp
    test_graph_invocation.cpp
)
if(NEOGRAPH_BUILD_PROGRAM)
    add_executable(neograph_program_tests
        test_program_values.cpp
        test_program_registry.cpp
        test_program_admission.cpp
        test_program_compiler.cpp
        test_program_catalog.cpp
        test_program_module.cpp
        test_program_contract.cpp
        test_program_journal.cpp
        test_program_pending.cpp
        test_program_fork_replay.cpp
        test_program_transition_store.cpp
        test_program_runtime.cpp
        test_program_invocation.cpp
    )
    target_include_directories(neograph_program_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/program
    )
    target_link_libraries(neograph_program_tests PRIVATE
        neograph::program
        GTest::gtest_main
    )
    if(TARGET neograph_program_sqlite)
        target_compile_definitions(neograph_program_tests PRIVATE
            NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        )
        target_link_libraries(neograph_program_tests PRIVATE
            neograph::program_sqlite
        )
    endif()
endif()

if(NEOGRAPH_BUILD_MCP_CLIENT)
    # A stdio MCP tool used from inside a graph run — the combination nothing
    # covered, and a use-after-free until the session stopped depending on an
    # io_context it does not own.
    target_sources(neograph_tests PRIVATE test_mcp_stdio_in_graph.cpp)
    target_sources(neograph_tests PRIVATE
        test_mcp_client_async.cpp
        test_mcp_stdio_async.cpp
    )
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_MCP_CLIENT)
endif()

if(NEOGRAPH_BUILD_MCP_SERVER)
    target_sources(neograph_tests PRIVATE test_mcp_server.cpp)
endif()

if(TARGET neograph_harness)
    target_sources(neograph_tests PRIVATE
        test_harness_program_translator.cpp
        test_harness_program_cutover.cpp
    )
    target_sources(neograph_tests PRIVATE
        test_harness_contract.cpp
    )
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_HARNESS)
endif()
if(TARGET neograph_mcp_sqlite)
    target_sources(neograph_tests PRIVATE test_harness_program_store.cpp)
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTING)
endif()

if(NEOGRAPH_BUILD_MCP_HTTP_SERVER)
    target_sources(neograph_tests PRIVATE test_mcp_http_server.cpp)
endif()

if(NEOGRAPH_BUILD_MCP_CLIENT AND TARGET neograph_harness)
    target_sources(neograph_tests PRIVATE test_harness_mcp_backend.cpp)
endif()

if(NEOGRAPH_BUILD_A2A)
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_A2A_TESTING)
    target_sources(neograph_tests PRIVATE
        test_a2a_types.cpp
        test_a2a_collaboration.cpp
        test_a2a_client.cpp
        test_a2a_server.cpp
    )
endif()

if(NEOGRAPH_BUILD_UTIL)
    target_sources(neograph_tests PRIVATE test_request_queue.cpp)
endif()

if(NEOGRAPH_BUILD_ACP)
    target_sources(neograph_tests PRIVATE
        test_acp_types.cpp
        test_acp_server.cpp
    )
endif()

# Keep the protocol-neutral invocation and ACP adapter contract independently
# buildable.  The aggregate neograph_tests target also contains HTTP/schema
# tests that require OpenSSL headers; ACP itself depends only on Core and must
# retain a runnable build gate in minimal dependency environments.
if(NEOGRAPH_BUILD_ACP)
    add_executable(neograph_acp_tests
        test_graph_invocation.cpp
        test_acp_types.cpp
        test_acp_server.cpp
    )
    target_link_libraries(neograph_acp_tests PRIVATE
        neograph::core
        neograph::acp
        GTest::gtest_main
    )
endif()

# Smoke tests for the async::async_get helper added with the A2A client
# — the GET path was new and build_request gained a method parameter,
# so these guard the surface against future POST/GET regressions.
target_sources(neograph_tests PRIVATE test_async_get.cpp)

# PostgresCheckpointStore integration tests are gated on NEOGRAPH_BUILD_POSTGRES
# at the build level (so projects without libpqxx still build the test
# binary) and on NEOGRAPH_TEST_POSTGRES_URL at runtime (skipped if unset).
# The repo's habit is to gate a whole test FILE on its backend. That does not
# fit a file whose backend cases sit next to backend-independent ones (see
# test_channel_write_mode.cpp), so those get a compile definition to guard on
# instead. Without it the file references SqliteCheckpointStore /
# PostgresCheckpointStore unconditionally and the link fails on any build
# without them — which is what the Windows and TSan jobs are.
if(NEOGRAPH_BUILD_POSTGRES)
    target_sources(neograph_tests PRIVATE test_postgres_checkpoint.cpp)
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_POSTGRES)

    add_executable(neograph_postgres_old_header_abi
        test_postgres_old_header_abi.cpp)
    target_link_libraries(neograph_postgres_old_header_abi PRIVATE
        neograph::postgres)
endif()

if(NEOGRAPH_BUILD_SQLITE)
    target_sources(neograph_tests PRIVATE test_sqlite_checkpoint.cpp test_evidence_ledger.cpp)
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_SQLITE)
endif()

if(NOT NEOGRAPH_USE_LIBCURL)
    target_sources(neograph_tests PRIVATE test_provider_constructor_exception_safety.cpp)
else()
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_LIBCURL)
endif()

target_link_libraries(neograph_tests PRIVATE
    neograph::core
    GTest::gtest_main
)

# Keep the core and Program test targets buildable in the minimal dependency
# configuration.  The corresponding source files are already compile-gated
# where a component-specific API is used; a test target must not manufacture a
# link dependency on an optional library that was intentionally disabled.
foreach(_neograph_test_component IN ITEMS llm async mcp mcp_server harness mcp_http_server a2a acp util postgres)
    if(TARGET neograph::${_neograph_test_component})
        target_link_libraries(neograph_tests PRIVATE neograph::${_neograph_test_component})
    endif()
endforeach()
if(TARGET httplib)
    target_link_libraries(neograph_tests PRIVATE httplib)
endif()
if(TARGET cppdotenv)
    target_link_libraries(neograph_tests PRIVATE cppdotenv)
endif()
if(TARGET OpenSSL::SSL)
    target_link_libraries(neograph_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(NEOGRAPH_BUILD_MCP_CLIENT)
    target_link_libraries(neograph_tests PRIVATE neograph::mcp)
endif()

if(NEOGRAPH_BUILD_MCP_SERVER)
    target_link_libraries(neograph_tests PRIVATE neograph::mcp_server)
endif()

if(TARGET neograph_harness)
    target_link_libraries(neograph_tests PRIVATE neograph::harness)
endif()

if(NEOGRAPH_BUILD_MCP_HTTP_SERVER)
    target_link_libraries(neograph_tests PRIVATE neograph::mcp_http_server)
endif()

if(NEOGRAPH_BUILD_A2A)
    target_link_libraries(neograph_tests PRIVATE neograph::a2a)
endif()

if(NEOGRAPH_BUILD_ACP)
    target_link_libraries(neograph_tests PRIVATE neograph::acp)
endif()

if(NEOGRAPH_BUILD_UTIL)
    target_link_libraries(neograph_tests PRIVATE neograph::util)
endif()

if(NEOGRAPH_BUILD_POSTGRES)
    # The test TU pokes libpq directly (PQconnectdb + pg_terminate_backend
    # sibling path in RetriesAfterBrokenConnection), so it needs the
    # libpq include / link in addition to neograph::postgres.
    target_link_libraries(neograph_tests PRIVATE neograph::postgres PostgreSQL::PostgreSQL)
endif()

if(NEOGRAPH_BUILD_SQLITE)
    target_link_libraries(neograph_tests PRIVATE neograph::sqlite)
    if(TARGET neograph_mcp_sqlite)
        target_link_libraries(neograph_tests PRIVATE neograph::mcp_sqlite SQLite::SQLite3)
    endif()
endif()

include(GoogleTest)

# Cross-repository compatibility is a metadata gate, not an integration
# build.  Keep it runnable in the minimal test configuration so a historical
# NeoCode/NeoProtocol reference cannot silently become current conformance.
add_test(NAME CrossRepositoryCompatibility.Metadata
    COMMAND Python3::Interpreter
        ${PROJECT_SOURCE_DIR}/scripts/check_cross_repository_compatibility.py
        --metadata ${PROJECT_SOURCE_DIR}/spec/cross-repository-compatibility-v1.json
        --schema ${PROJECT_SOURCE_DIR}/spec/cross-repository-compatibility-v1.schema.json)
set_tests_properties(CrossRepositoryCompatibility.Metadata
    PROPERTIES LABELS "compatibility;metadata")

if(TARGET neograph_program_tests)
    gtest_discover_tests(neograph_program_tests)
endif()
if(TARGET neograph_acp_tests)
    gtest_discover_tests(neograph_acp_tests)
endif()

if(NEOGRAPH_BUILD_POSTGRES)
    add_test(NAME PostgresCheckpointAbiTest.OldHeaderNewLibrary
        COMMAND neograph_postgres_old_header_abi)
    set_tests_properties(PostgresCheckpointAbiTest.OldHeaderNewLibrary
        PROPERTIES RESOURCE_LOCK postgres_test_db)
endif()

# ThreadSanitizer ASLR notice: when CMAKE_CXX_FLAGS contains
# `-fsanitize=thread`, the test binaries can FATAL with
# "unexpected memory mapping" on Linux kernels with high
# mmap_rnd_bits (newer Ubuntu / RHEL defaults). Workaround is to
# invoke ctest itself through `setarch x86_64 -R` — the
# ADDR_NO_RANDOMIZE flag inherits through fork, so every test
# child also gets a low-randomization layout TSan understands:
#
#     setarch x86_64 -R ctest -E "BIG_|valgrind"
#
# CI's tsan-test job does this in its run-step. There is no clean
# CMake hook to inject the launcher into add_test commands
# generated by gtest_discover_tests, so we document the contract
# rather than try to hide it.
if(CMAKE_CXX_FLAGS MATCHES "fsanitize=thread")
    message(STATUS "TSan detected — invoke ctest through "
                   "`setarch x86_64 -R ctest ...` to avoid the "
                   "ASLR/`unexpected memory mapping` FATAL.")
endif()

if(NEOGRAPH_BUILD_POSTGRES)
    # PostgresCheckpointTest cases share one DB and call drop_schema in
    # SetUp/TearDown — running them in parallel collides on DDL. Mark
    # them with a RESOURCE_LOCK so ctest serializes only this group
    # while the other ~200 tests still run concurrently.
    # ChannelWriteModeTest.OverwriteSurvivesReplayThroughPostgres joins the
    # locked group: it talks to the same database, and the Postgres suite's
    # drop_schema() will wipe its tables mid-run otherwise. Observed exactly
    # that — green standalone, red under `ctest -j8`.
    set(NEOGRAPH_PG_LOCKED_TESTS
        "PostgresCheckpointTest.*:ChannelWriteModeTest.OverwriteSurvivesReplayThroughPostgres")
    gtest_discover_tests(neograph_tests
        TEST_FILTER "${NEOGRAPH_PG_LOCKED_TESTS}"
        PROPERTIES RESOURCE_LOCK postgres_test_db
    )
    gtest_discover_tests(neograph_tests
        TEST_FILTER "-${NEOGRAPH_PG_LOCKED_TESTS}"
    )
else()
    gtest_discover_tests(neograph_tests)
endif()
