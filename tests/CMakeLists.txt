# With the vcpkg toolchain on Windows, every executable gets a post-link step that copies the DLLs it
# imports next to it. The test executables share one output directory and link in parallel, so two of
# those steps can write the same DLL at once, and the link that loses fails (#515). Copy the DLLs once,
# here, before any target is built, and turn the toolchain's per-target step off for this directory.
if(WIN32 AND VCPKG_APPLOCAL_DEPS AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    function(_neograph_stage_vcpkg_dlls config destination)
        set(installed "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        if(config MATCHES "^[Dd][Ee][Bb][Uu][Gg]$")
            string(APPEND installed "/debug")
        endif()
        file(GLOB dlls "${installed}/bin/*.dll")
        if(dlls)
            file(COPY ${dlls} DESTINATION "${destination}")
        endif()
    endfunction()

    if(CMAKE_CONFIGURATION_TYPES)
        foreach(_config IN LISTS CMAKE_CONFIGURATION_TYPES)
            _neograph_stage_vcpkg_dlls("${_config}" "${CMAKE_CURRENT_BINARY_DIR}/${_config}")
        endforeach()
    else()
        _neograph_stage_vcpkg_dlls("${CMAKE_BUILD_TYPE}" "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    set(VCPKG_APPLOCAL_DEPS OFF)
endif()

add_executable(neograph_tests
    test_canonical_json.cpp
    test_sha256.cpp
    test_graph_state.cpp
    test_graph_engine.cpp
    test_engine_config.cpp
    test_checkpoint.cpp
    test_store.cpp
    test_tenant.cpp
    test_node.cpp
    test_node_header.cpp
    test_fanout_serial_notice.cpp
    test_tool_request_refusal.cpp
    test_loader.cpp
    test_schema_export.cpp
    test_json_array.cpp
    test_json_document_memory.cpp
    test_json_parse_depth.cpp
    test_pending_writes.cpp
    test_channel_write_mode.cpp
    test_graph_state_reducer_ownership.cpp
    test_usage_accounting.cpp
    test_subgraph_context.cpp
    test_node_invocation_identity.cpp
    test_subgraph_output_deltas.cpp
    test_subgraph_persistence.cpp
    test_plan_execute_graph.cpp
    test_deep_research_truncation.cpp
    test_fork_resume_semantics.cpp
    test_signal_dispatch.cpp
    test_multi_node_resume.cpp
    test_resume_if_exists.cpp
    test_cancel_token_propagation.cpp
    test_live_llm_cancel_fanout_cpp.cpp
    test_scheduler.cpp
    test_node_execution.cpp
    test_tool_dispatch_parity.cpp
    test_tool_execution.cpp
    test_tool_failure_contract.cpp
    test_host_admission.cpp
    test_compiler.cpp
    test_compiler_strict.cpp
    test_graph_upgrade.cpp
    test_validator.cpp
    test_graph_registry_local.cpp
    test_pbt_roundtrip.cpp
    test_corpus.cpp
    test_evolution.cpp
    test_harness_bench.cpp
    test_schema_evolution.cpp
    test_barrier_persistence.cpp
    test_coordinator.cpp
    test_sends_interrupt_order.cpp
    test_dynamic_interrupt.cpp
    test_tool_gate.cpp
    test_resume_tool_calls.cpp
    test_multi_send_events.cpp
    test_multi_send_routing.cpp
    test_schema_provider_concurrent.cpp
    test_schema_provider_async.cpp
    test_schema_provider_stream_usage.cpp
    test_schema_provider_responses_stream.cpp
    test_schema_provider_stop_reason.cpp
    test_admin_api_barrier_preservation.cpp
    test_intent_classifier_streaming.cpp
    test_incremental_checkpoint.cpp
    test_async_conn_pool.cpp
    test_schema_provider_wire_contract.cpp
    test_schema_provider_media.cpp
    test_async_http_errors.cpp
    test_async_sse_parser.cpp
    test_async_ws_codec.cpp
    test_async_ws_loopback.cpp
    test_async_http_stream.cpp
    test_async_http_options.cpp
    test_prepared_provider.cpp
    test_provider_endpoints.cpp
    test_endpoint_nodes.cpp
    test_artifact_provider.cpp
    test_run_context_store.cpp
    test_runtime_context.cpp
    test_run_result_channel.cpp
    test_channel_versions.cpp
    test_typed_events.cpp
    test_error_messages_friendly.cpp
    test_schema_provider_extra_fields_temperature.cpp
    test_openai_provider_async.cpp
    test_message_reasoning.cpp
    test_provider_format_upgrade.cpp
    test_provider_family_controls.cpp
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
    test_engine_pool_owner_thread.cpp
    test_openinference_cpp.cpp
    test_schema_provider_stream_async_outer_io.cpp
    test_schema_provider_stream_async_nested_thread.cpp
    test_graph_invocation.cpp
    test_graph_safe_point.cpp
)
if(NEOGRAPH_BUILD_PROGRAM)
    add_executable(neograph_program_tests
        test_program_authoring.cpp
        test_program_values.cpp
        test_program_command.cpp
        test_program_native.cpp
        native_abi_c_smoke.c
        test_program_registry.cpp
        test_program_admission.cpp
        test_program_compiler.cpp
        test_program_catalog.cpp
        test_program_module.cpp
        test_program_contract.cpp
        test_program_journal.cpp
        test_program_lineage.cpp
        test_program_pending.cpp
        test_program_fork_replay.cpp
        test_graph_migration_capsule.cpp
        test_program_transition_store.cpp
        test_program_record_identity.cpp
        test_program_runtime.cpp
        test_program_runtime_executor.cpp
        test_program_runtime_instruction.cpp
        test_program_invocation.cpp
        test_program_task_graph_proposal.cpp
        test_program_task_graph_scheduler.cpp
        test_program_task_graph_expansion.cpp
        test_program_step_plan.cpp
        test_program_plan_intake.cpp
        test_program_budget_overrun.cpp
    )
    target_include_directories(neograph_program_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/program
    )
    target_link_libraries(neograph_program_tests PRIVATE
        neograph::program
        GTest::gtest_main
    )
    if(TARGET neograph::llm)
        target_link_libraries(neograph_program_tests PRIVATE neograph::llm)
        target_compile_definitions(neograph_program_tests PRIVATE NEOGRAPH_PROGRAM_TESTS_HAVE_LLM)
    else()
        # Fixtures construct sp::runtime::Client, whose constructor neograph::llm normally brings in.
        target_link_libraries(neograph_program_tests PRIVATE SchemaProvider::transport)
    endif()
    # test_program_runtime.cpp exceeds COFF's default section count; the
    # top-level CMakeLists.txt passes /bigobj to every MSVC target.
    if(NEOGRAPH_BUILD_QUICKJS_CONTROL)
        target_compile_definitions(neograph_program_tests PRIVATE
            NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS
        )
        add_executable(program_model_synthesis_probe
            program_model_synthesis_probe.cpp
        )
        target_link_libraries(program_model_synthesis_probe PRIVATE
            neograph::program
        )
        add_test(
            NAME Program.ModelSynthesisProbe
            COMMAND program_model_synthesis_probe
                ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/model_program_synthesis/valid.js
        )
        set_tests_properties(Program.ModelSynthesisProbe PROPERTIES
            LABELS "program;quickjs;synthesis"
        )
        add_executable(program_dsl_capability_probe
            program_dsl_capability_probe.cpp
        )
        target_include_directories(program_dsl_capability_probe PRIVATE
            ${PROJECT_SOURCE_DIR}/src/program
        )
        target_link_libraries(program_dsl_capability_probe PRIVATE
            neograph::program
        )
        set(_neograph_dsl_capability_cases
            graph_basics
            graph_routing
            graph_fanout_barrier
            graph_hitl_retry
            registry_mediated
            program_control_flow
            program_map
            program_structured_concurrency
            program_spawn_await
            program_durability
            program_host_capability
        )
        foreach(_case IN LISTS _neograph_dsl_capability_cases)
            add_test(
                NAME Program.DslCapability.${_case}
                COMMAND program_dsl_capability_probe
                    ${_case}
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dsl_capabilities/${_case}.js
            )
            set_tests_properties(Program.DslCapability.${_case} PROPERTIES
                LABELS "program;quickjs;dsl-capability"
            )
        endforeach()
        add_executable(program_model_semantic_migration_probe
            program_model_semantic_migration_probe.cpp
        )
        target_link_libraries(program_model_semantic_migration_probe PRIVATE
            neograph::program
        )
        add_test(
            NAME Program.ModelSemanticMigrationProbe
            COMMAND program_model_semantic_migration_probe
                ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/model_semantic_migration/valid.js
        )
        set_tests_properties(Program.ModelSemanticMigrationProbe PROPERTIES
            LABELS "program;quickjs;synthesis;migration"
        )
    endif()
    if(TARGET example_program_plan_intake)
        add_test(NAME Program.PlanIntakeExample COMMAND example_program_plan_intake)
        set_tests_properties(Program.PlanIntakeExample PROPERTIES
            PASS_REGULAR_EXPRESSION "s3 completed -> 35"
            LABELS "program;example"
        )
    endif()
    if(TARGET neograph_program_sqlite)
        target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_PROGRAM_SQLITE)
        target_link_libraries(neograph_tests PRIVATE neograph::program_sqlite SQLite::SQLite3)
        target_sources(neograph_program_tests PRIVATE test_program_provider_journal.cpp)
        target_compile_definitions(neograph_program_tests PRIVATE
            NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
        )
        target_link_libraries(neograph_program_tests PRIVATE
            neograph::program_sqlite
            SQLite::SQLite3
        )
        if(TARGET neograph_sqlite)
            target_compile_definitions(neograph_program_tests PRIVATE
                NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
            )
            target_link_libraries(neograph_program_tests PRIVATE
                neograph::sqlite
            )
        endif()
    endif()
    if(TARGET neograph_program_postgres)
        target_compile_definitions(neograph_program_tests PRIVATE
            NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
        )
        target_link_libraries(neograph_program_tests PRIVATE
            neograph::program_postgres
            PostgreSQL::PostgreSQL
        )
        if(TARGET neograph_postgres)
            target_compile_definitions(neograph_program_tests PRIVATE NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES_CHECKPOINT)
            target_link_libraries(neograph_program_tests PRIVATE neograph::postgres)
        endif()
    endif()
endif()

if(TARGET neograph_quickjs)
    add_executable(neograph_quickjs_tests
        test_quickjs_runtime.cpp
    )
    target_compile_features(neograph_quickjs_tests PRIVATE cxx_std_17)
    _neograph_attach_quickjs_runtime(neograph_quickjs_tests)
    target_link_libraries(neograph_quickjs_tests PRIVATE
        GTest::gtest_main
    )

    add_executable(neograph_quickjs_c_smoke
        quickjs_c_smoke.c
    )
    set_target_properties(neograph_quickjs_c_smoke PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)
    _neograph_attach_quickjs_runtime(neograph_quickjs_c_smoke)
endif()

if(NEOGRAPH_BUILD_MCP_CLIENT)
    # A stdio MCP tool used from inside a graph run — the combination nothing
    # covered, and a use-after-free until the session stopped depending on an
    # io_context it does not own.
    target_sources(neograph_tests PRIVATE test_mcp_stdio_in_graph.cpp)
    target_sources(neograph_tests PRIVATE
        test_mcp_client_async.cpp
        test_mcp_client_conformance.cpp
        test_mcp_stdio_async.cpp
    )
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_MCP_CLIENT)

    add_executable(neograph_mcp_hook_rpc_tests
        test_hook_rpc_mcp.cpp
    )
    target_link_libraries(neograph_mcp_hook_rpc_tests PRIVATE
        neograph::mcp
        GTest::gtest_main
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(neograph_mcp_stdio_fd_fallback test_mcp_stdio_fd_fallback.cpp)
        target_link_libraries(neograph_mcp_stdio_fd_fallback PRIVATE neograph::mcp)
    endif()
endif()

if(NEOGRAPH_BUILD_MCP_SERVER)
    target_sources(neograph_tests PRIVATE test_mcp_server.cpp test_mcp_json_schema.cpp)
endif()

if(TARGET neograph_harness)
    target_sources(neograph_tests PRIVATE
        test_harness_contract.cpp
        test_harness_host_agent.cpp
        test_mcp_adoption.cpp
    )
    if(NEOGRAPH_BUILD_QUICKJS_CONTROL)
        target_sources(neograph_tests PRIVATE
            test_harness_program_translator.cpp
            test_harness_program_cutover.cpp
        )
    endif()
endif()
if(TARGET neograph_mcp_sqlite)
    target_sources(neograph_tests PRIVATE
        test_harness_program_store.cpp
        test_harness_tenant_persistence.cpp
    )
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTING)

    add_executable(neograph_harness_program_store_tests
        test_harness_program_store.cpp
    )
    target_compile_definitions(neograph_harness_program_store_tests PRIVATE NEOGRAPH_TESTING)
    target_link_libraries(neograph_harness_program_store_tests PRIVATE
        neograph::mcp_sqlite
        SQLite::SQLite3
        GTest::gtest_main
    )
endif()

if(NEOGRAPH_BUILD_MCP_HTTP_SERVER)
    target_sources(neograph_tests PRIVATE test_mcp_http_server.cpp)

    # The NeoGraph MCP server the official-SDK interop suites
    # (bindings/python/tests/interop/mcp) talk to over stdio and HTTP.
    add_executable(neograph_mcp_interop_probe fixtures/mcp_interop_probe_server.cpp)
    target_link_libraries(neograph_mcp_interop_probe PRIVATE neograph::mcp_http_server)
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
        test_a2a_conformance.cpp
    )

    add_executable(neograph_a2a_collaboration_tests
        test_a2a_collaboration.cpp
    )
    target_compile_definitions(neograph_a2a_collaboration_tests PRIVATE
        NEOGRAPH_A2A_TESTING)
    target_link_libraries(neograph_a2a_collaboration_tests PRIVATE
        neograph::a2a
        httplib
        OpenSSL::SSL
        OpenSSL::Crypto
        GTest::gtest_main
    )
    add_test(NAME neograph.a2a_collaboration
             COMMAND neograph_a2a_collaboration_tests)
    add_executable(neograph_a2a_graph_contract_tests
        test_graph_host_a2a_contract.cpp)
    target_compile_definitions(neograph_a2a_graph_contract_tests PRIVATE
        NEOGRAPH_A2A_TESTING)
    target_link_libraries(neograph_a2a_graph_contract_tests PRIVATE
        neograph::a2a SchemaProvider::transport GTest::gtest_main)
    add_test(NAME neograph.a2a_graph_contract
             COMMAND neograph_a2a_graph_contract_tests)

    # Deterministic native A2A agent driven by the official A2A SDKs in
    # bindings/python/tests/interop/a2a_suite (no provider, no keys).
    add_executable(neograph_a2a_probe_server fixtures/a2a_probe_server.cpp)
    target_link_libraries(neograph_a2a_probe_server PRIVATE neograph::a2a)
endif()

# The Agent Card Copy Ninja PoC intentionally has no LLM, Harness, or remote
# agent dependency. Keep its end-to-end loopback test independently runnable
# in the minimal A2A configuration.
if(NEOGRAPH_BUILD_A2A)
    add_executable(neograph_a2a_agent_card_candidate_tests
        test_a2a_agent_card_candidate.cpp
    )
    target_link_libraries(neograph_a2a_agent_card_candidate_tests PRIVATE
        neograph::a2a
        httplib
        OpenSSL::SSL
        OpenSSL::Crypto
        GTest::gtest_main
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
        SchemaProvider::transport
        GTest::gtest_main
    )

    add_executable(neograph_acp_graph_contract_tests
        test_graph_host_acp_contract.cpp)
    target_link_libraries(neograph_acp_graph_contract_tests PRIVATE
        neograph::acp SchemaProvider::transport GTest::gtest_main)
    add_test(NAME neograph.acp_graph_contract
             COMMAND neograph_acp_graph_contract_tests)

    if(NEOGRAPH_BUILD_MCP_CLIENT)
        # ACPServer starts client-supplied stdio MCP servers in this build.
        target_compile_definitions(neograph_acp_tests PRIVATE NEOGRAPH_ACP_MCP_TESTS)
        target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_ACP_MCP_TESTS)
    endif()

    # Deterministic stdio ACP agent driven by the official ACP SDKs in
    # bindings/python/tests/interop/acp_suite/ (no provider, no keys).
    add_executable(neograph_acp_probe_agent fixtures/acp_probe_agent.cpp)
    target_link_libraries(neograph_acp_probe_agent PRIVATE
        neograph::core neograph::acp SchemaProvider::transport)
endif()

if(NEOGRAPH_BUILD_GRPC)
    add_executable(neograph_grpc_graph_contract_tests
        test_graph_host_grpc_contract.cpp)
    target_link_libraries(neograph_grpc_graph_contract_tests PRIVATE
        neograph::grpc SchemaProvider::transport GTest::gtest_main)
    target_compile_definitions(neograph_grpc_graph_contract_tests PRIVATE
        NEOGRAPH_EXPECTED_VERSION="${NEOGRAPH_VERSION}")
    add_test(NAME neograph.grpc_graph_contract
             COMMAND neograph_grpc_graph_contract_tests)

    # Each public header on its own, in a translation unit that includes
    # nothing else of gRPC: the service factories return a pointer to
    # ::grpc::Service, which has to be complete wherever it is destroyed.
    foreach(_header tool_service checkpoint)
        add_executable(neograph_grpc_${_header}_header_tests
            test_grpc_${_header}_header.cpp)
        target_link_libraries(neograph_grpc_${_header}_header_tests PRIVATE
            neograph::grpc SchemaProvider::transport GTest::gtest_main)
        add_test(NAME neograph.grpc_${_header}_header
                 COMMAND neograph_grpc_${_header}_header_tests)
    endforeach()
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
    target_sources(neograph_tests PRIVATE test_sqlite_checkpoint.cpp test_evidence_ledger.cpp test_sqlite_runtime_stores.cpp)
    target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_TESTS_HAVE_SQLITE)
    # The runtime-store tests read the schemas under schemas/.
    set_source_files_properties(test_sqlite_runtime_stores.cpp PROPERTIES
        COMPILE_DEFINITIONS NEOGRAPH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()

target_sources(neograph_tests PRIVATE test_provider_constructor_exception_safety.cpp)

target_link_libraries(neograph_tests PRIVATE
    neograph::core
    GTest::gtest_main
)

# Reaches into core internals (`detail::` binding scopes) that a shared library
# does not export, so it is built only against the static core.
if(NOT BUILD_SHARED_LIBS)
    target_sources(neograph_tests PRIVATE test_run_context_runtime_registry.cpp)
    if(NEOGRAPH_BUILD_SQLITE)
        target_sources(neograph_tests PRIVATE test_tool_effect_not_started.cpp)
    endif()
endif()

# A program that links neograph::core alone must not load libcurl (only neograph::llm links the
# SchemaProvider transport). Reads /proc/self/maps, so Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(neograph_core_footprint_probe core_footprint_probe.cpp)
    target_link_libraries(neograph_core_footprint_probe PRIVATE neograph::core)
    add_test(NAME core_links_no_libcurl COMMAND neograph_core_footprint_probe)
endif()

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
    target_link_libraries(neograph_tests PRIVATE neograph::sqlite SQLite::SQLite3)
    if(TARGET neograph_mcp_sqlite)
        target_link_libraries(neograph_tests PRIVATE neograph::mcp_sqlite SQLite::SQLite3)
    endif()
endif()

include(GoogleTest)

# gtest_discover_tests lists each test binary's tests after it is built and
# gives up after 5 s by default. On a loaded CI runner (seen on the Windows
# Visual Studio lane) listing a large binary can take longer, which fails the
# build, so every test binary is discovered through this wrapper with a 120 s
# limit. Extra arguments are passed through unchanged.
function(neograph_discover_tests target)
    gtest_discover_tests(${target} DISCOVERY_TIMEOUT 120 ${ARGN})
endfunction()

# A suite instantiated over tests/fixtures/checkpoint_backends.h also runs on
# PostgreSQL when it is built. Those cases drop the shared checkpoint tables,
# so they hold the postgres_test_db lock; the other cases stay parallel. The
# lock comes first: gtest_discover_tests flattens a list-valued property.
function(neograph_discover_backend_tests target labels)
    if(TARGET neograph_postgres)
        target_link_libraries(${target} PRIVATE neograph::postgres)
        target_compile_definitions(${target} PRIVATE NEOGRAPH_TESTS_HAVE_POSTGRES)
        neograph_discover_tests(${target} TEST_FILTER "*/Postgres*"
            PROPERTIES RESOURCE_LOCK postgres_test_db LABELS ${labels})
        neograph_discover_tests(${target} TEST_FILTER "-*/Postgres*"
            PROPERTIES LABELS ${labels})
    else()
        neograph_discover_tests(${target} PROPERTIES LABELS ${labels})
    endif()
endfunction()

if(TARGET neograph_mcp_hook_rpc_tests)
    neograph_discover_tests(neograph_mcp_hook_rpc_tests)
endif()

# Keep durable runtime-contract verification independent from optional provider
# transports so canonical identity tests remain runnable in minimal Core builds.
add_executable(neograph_runtime_context_tests
    test_runtime_context.cpp
    test_context_transform.cpp
    test_context_stability.cpp
)
# The stability tests check the stored values against the schemas under schemas/.
set_source_files_properties(test_context_stability.cpp PROPERTIES
    COMPILE_DEFINITIONS NEOGRAPH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(neograph_runtime_context_tests PRIVATE
    neograph::core
    GTest::gtest_main
)
neograph_discover_tests(neograph_runtime_context_tests)

# Hook planning is Core-only and intentionally does not depend on providers,
# model tool calls, or optional transport libraries.
add_executable(neograph_hook_tests
    test_hook.cpp
    test_checkpoint_hook.cpp
)
target_link_libraries(neograph_hook_tests PRIVATE
    neograph::core
    GTest::gtest_main
)
neograph_discover_tests(neograph_hook_tests)

add_executable(neograph_hook_rpc_tests
    test_hook_rpc.cpp
)
target_link_libraries(neograph_hook_rpc_tests PRIVATE
    neograph::core
    GTest::gtest_main
)
neograph_discover_tests(neograph_hook_rpc_tests)

add_executable(neograph_context_store_tests
    test_context_store.cpp
)
target_link_libraries(neograph_context_store_tests PRIVATE
    neograph::core
    GTest::gtest_main
)
neograph_discover_tests(neograph_context_store_tests)

if(NEOGRAPH_BUILD_SQLITE)
    # Keep runtime-store migration and outcome lifecycle tests independently
    # buildable on Windows; unrelated checkpoint fixtures include POSIX-only
    # test helpers in some developer configurations.
    add_executable(neograph_sqlite_runtime_store_tests
        test_sqlite_runtime_stores.cpp
    )
    target_link_libraries(neograph_sqlite_runtime_store_tests PRIVATE
        neograph::core
        neograph::sqlite
        SQLite::SQLite3
        GTest::gtest_main
    )
    neograph_discover_tests(neograph_sqlite_runtime_store_tests)
    if(TARGET neograph::llm)
        target_link_libraries(neograph_sqlite_runtime_store_tests PRIVATE neograph::llm)
        target_compile_definitions(neograph_sqlite_runtime_store_tests PRIVATE NEOGRAPH_SQLITE_TOOL_TESTS_HAVE_LLM)
        target_compile_definitions(neograph_tests PRIVATE NEOGRAPH_SQLITE_TOOL_TESTS_HAVE_LLM)
    endif()
endif()

add_executable(neograph_runtime_turn_tests
    test_runtime_turn_assembler.cpp
    test_controlled_provider.cpp
)
target_link_libraries(neograph_runtime_turn_tests PRIVATE
    neograph::core
    SchemaProvider::transport
    GTest::gtest_main
)
if(TARGET neograph::llm)
    # llm::Agent runs the same tool loop outside a graph (#444).
    target_link_libraries(neograph_runtime_turn_tests PRIVATE neograph::llm)
    target_compile_definitions(neograph_runtime_turn_tests PRIVATE NEOGRAPH_RUNTIME_TURN_TESTS_HAVE_LLM)
endif()
neograph_discover_tests(neograph_runtime_turn_tests)

if(TARGET neograph::llm)
    # Live end-to-end checks with real provider keys. Built so that they compile with every change, run only by
    # scripts/live_e2e.py (tests/live/README.md), never by CTest.
    add_executable(neograph_live_e2e_tests live_e2e_test.cpp)
    target_link_libraries(neograph_live_e2e_tests PRIVATE neograph::llm GTest::gtest_main)
endif()

# Keep the live safe-point boundary independently runnable from optional
# backend tests in the aggregate Core binary.
add_executable(neograph_graph_safe_point_tests
    test_graph_safe_point.cpp
)
target_link_libraries(neograph_graph_safe_point_tests PRIVATE
    neograph::core
    GTest::gtest_main
)
neograph_discover_tests(neograph_graph_safe_point_tests)

# The fail-closed cross-repository compatibility check runs when its metadata files are
# present; a clean checkout does not depend on them.
set(NEOGRAPH_COMPATIBILITY_METADATA
    ${PROJECT_SOURCE_DIR}/spec/cross-repository-compatibility-v1.json)
set(NEOGRAPH_COMPATIBILITY_SCHEMA
    ${PROJECT_SOURCE_DIR}/spec/cross-repository-compatibility-v1.schema.json)
if(EXISTS "${NEOGRAPH_COMPATIBILITY_METADATA}" AND EXISTS "${NEOGRAPH_COMPATIBILITY_SCHEMA}")
    add_test(NAME CrossRepositoryCompatibility.Metadata
        COMMAND Python3::Interpreter
            ${PROJECT_SOURCE_DIR}/scripts/check_cross_repository_compatibility.py
            --metadata ${NEOGRAPH_COMPATIBILITY_METADATA}
            --schema ${NEOGRAPH_COMPATIBILITY_SCHEMA})
    set_tests_properties(CrossRepositoryCompatibility.Metadata
        PROPERTIES LABELS "compatibility;metadata")
endif()

# Q7 uses a frozen-storage audit rather than a claim inferred from source
# availability. Keep its Python contract runnable in minimal configurations.
add_test(NAME QuickJS.LegacyDrainAudit
    COMMAND Python3::Interpreter
        ${PROJECT_SOURCE_DIR}/tests/test_legacy_drain_audit.py)
set_tests_properties(QuickJS.LegacyDrainAudit
    PROPERTIES LABELS "quickjs;migration;metadata")

# Public-text hygiene: scripts/check_public_text.py keeps internal trivia (names of earlier
# projects, private links, narrated decisions and reviews, home paths, addresses, secrets) out of
# the public text. The tree scan covers the documents too: Markdown, docs/, README* and CHANGELOG*.
add_test(NAME public_text_guard
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/scripts/check_public_text.py
        --tree --scope all --root ${PROJECT_SOURCE_DIR})
add_test(NAME public_text_guard_selftest
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test_public_text_guard.py)
set_tests_properties(public_text_guard public_text_guard_selftest
    PROPERTIES LABELS "hygiene" TIMEOUT 300)

# The benchmark A/B gate runner, the script that checks the absolute limits, and their data file
# (benchmarks/ci_ab_gates.json) against stand-in benchmarks.
add_test(NAME ci_ab_gates_selftest
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test_ci_ab_gates.py)
set_tests_properties(ci_ab_gates_selftest PROPERTIES LABELS "hygiene" TIMEOUT 120)

# Engine code waits on async::HostTimer (src/core/host_timer.h), never on a raw Asio timer: a HostLoop cannot see Asio's
# timer queue, so a timer that it was not told about never fires on a WebAssembly host (#567). The scan covers src/ and
# include/; scripts/host_timer_allowlist.txt lists the code that never runs on a host loop, each with its reason.
add_test(NAME host_timer_guard
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/scripts/check_host_timers.py --root ${PROJECT_SOURCE_DIR})
add_test(NAME host_timer_guard_selftest
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test_host_timer_guard.py)
set_tests_properties(host_timer_guard host_timer_guard_selftest PROPERTIES LABELS "hygiene" TIMEOUT 120)

if(TARGET neograph_program_tests)
    if(NEOGRAPH_BUILD_POSTGRES)
        # Keep the Program PostgreSQL store integration cases off the shared
        # checkpoint fixture while other test cases remain parallelizable.
        set(NEOGRAPH_PROGRAM_PG_LOCKED_TESTS
            "ProgramCatalogTest.PostgreSQLProgramStoreReopensActivationAndOwnerVisibility:ProgramTransitionStoreTest.PostgreSQL*:*ProgramChildSynthesisPersistence.*/*PostgreSQL*:*ProgramChildSynthesisCrash.*/*PostgreSQL*:ProgramChildSynthesisPostgreSQL.*:*StoreConformance.*/PostgreSQL")
        neograph_discover_tests(neograph_program_tests
            TEST_FILTER "${NEOGRAPH_PROGRAM_PG_LOCKED_TESTS}"
            PROPERTIES RESOURCE_LOCK postgres_test_db)
        neograph_discover_tests(neograph_program_tests
            TEST_FILTER "-${NEOGRAPH_PROGRAM_PG_LOCKED_TESTS}")
    else()
        neograph_discover_tests(neograph_program_tests)
    endif()
endif()
if(TARGET neograph_harness_program_store_tests)
    neograph_discover_tests(neograph_harness_program_store_tests)
endif()
if(TARGET neograph_a2a_agent_card_candidate_tests)
    neograph_discover_tests(neograph_a2a_agent_card_candidate_tests)
endif()
if(TARGET neograph_acp_tests)
    neograph_discover_tests(neograph_acp_tests)
endif()
if(TARGET neograph_quickjs_tests)
    neograph_discover_tests(neograph_quickjs_tests)
endif()
if(TARGET neograph_quickjs)
    add_test(NAME QuickJS.MsvcPortSource
        COMMAND ${CMAKE_COMMAND}
            -DNEOGRAPH_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DNEOGRAPH_BINARY_DIR=${PROJECT_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/verify_quickjs_msvc_port.cmake)
    set_tests_properties(QuickJS.MsvcPortSource PROPERTIES LABELS "quickjs;portability")
endif()


if(TARGET neograph_quickjs_c_smoke)
    add_test(NAME QuickJS.CEmbeddingSmoke COMMAND neograph_quickjs_c_smoke)
    set_tests_properties(QuickJS.CEmbeddingSmoke PROPERTIES LABELS "quickjs;abi")
endif()
if(TARGET neograph_mcp_stdio_fd_fallback)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME MCPStdioFallback.ClosesHighDescriptors
            COMMAND neograph_mcp_stdio_fd_fallback ${Python3_EXECUTABLE})
        set_tests_properties(MCPStdioFallback.ClosesHighDescriptors PROPERTIES
            TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "mcp;isolation")
    endif()
endif()
if(TARGET neograph_a2a_agent_card_candidate_tests)
    neograph_discover_tests(neograph_a2a_agent_card_candidate_tests)
endif()

if(NEOGRAPH_BUILD_POSTGRES)
    add_test(NAME PostgresCheckpointAbiTest.OldHeaderNewLibrary
        COMMAND neograph_postgres_old_header_abi)
    set_tests_properties(PostgresCheckpointAbiTest.OldHeaderNewLibrary
        PROPERTIES RESOURCE_LOCK postgres_test_db)
endif()

if(TARGET cookbook_program_chatbot AND NEOGRAPH_BUILD_SQLITE)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_executable(program_chat_policy_fixture fixtures/program_chat_policy.cpp)
        target_link_libraries(program_chat_policy_fixture PRIVATE neograph::core)
        add_test(NAME ProgramChatbot.SQLite
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/examples/cookbook/self_evolving_chatbot/test_program_chat.py
                $<TARGET_FILE:cookbook_program_chatbot>
                $<TARGET_FILE:program_chat_policy_fixture>)
        set_tests_properties(ProgramChatbot.SQLite PROPERTIES
            TIMEOUT 240 LABELS "program;example;integration"
            ENVIRONMENT "NEOGRAPH_CHAT_POSTGRES_URL=")
    endif()
endif()

if(TARGET neograph_harness_mcp)
    set(NEOGRAPH_HARNESS_CLI_TEST_TARGET neograph_harness_mcp)
elseif(TARGET example_harness_mcp_server)
    set(NEOGRAPH_HARNESS_CLI_TEST_TARGET example_harness_mcp_server)
endif()
if(NEOGRAPH_HARNESS_CLI_TEST_TARGET)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME HarnessCLI.CompileStartGet
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_harness_cli.py
                $<TARGET_FILE:${NEOGRAPH_HARNESS_CLI_TEST_TARGET}>)
        set_tests_properties(HarnessCLI.CompileStartGet PROPERTIES
            TIMEOUT 60 LABELS "program;cli;integration")
    endif()
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
    # Channel-write and subgraph persistence cases use the same checkpoint
    # tables; the Postgres suite's drop_schema() must not race those fixtures.
    set(NEOGRAPH_PG_LOCKED_TESTS
        "PostgresCheckpointTest.*:ChannelWriteModeTest.OverwriteSurvivesReplayThroughPostgres:SubgraphPersistence.PostgreSQL*")
    neograph_discover_tests(neograph_tests
        TEST_FILTER "${NEOGRAPH_PG_LOCKED_TESTS}"
        PROPERTIES RESOURCE_LOCK postgres_test_db
    )
    neograph_discover_tests(neograph_tests
        TEST_FILTER "-${NEOGRAPH_PG_LOCKED_TESTS}"
    )
else()
    neograph_discover_tests(neograph_tests)
endif()

# History preservation and summary trust, including authentic native fixtures.
add_executable(neograph_history_compaction_tests test_history_compaction.cpp)
target_link_libraries(neograph_history_compaction_tests PRIVATE
    neograph::core SchemaProvider::transport GTest::gtest_main)
neograph_discover_tests(neograph_history_compaction_tests)

if(NEOGRAPH_BUILD_PROGRAM)
    add_executable(neograph_program_join_signal_tests test_program_join_signal.cpp)
    target_include_directories(neograph_program_join_signal_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/program)
    target_compile_features(neograph_program_join_signal_tests PRIVATE cxx_std_20)
    target_link_libraries(neograph_program_join_signal_tests PRIVATE neograph::core asio GTest::gtest_main)
    neograph_discover_tests(neograph_program_join_signal_tests)
endif()

if(TARGET neograph::llm AND TARGET httplib AND TARGET OpenSSL::SSL)
    add_executable(neograph_tool_result_grouping_tests test_tool_result_grouping.cpp)
    target_link_libraries(neograph_tool_result_grouping_tests PRIVATE
        neograph::core neograph::llm httplib OpenSSL::SSL OpenSSL::Crypto GTest::gtest_main)
    neograph_discover_tests(neograph_tool_result_grouping_tests)
endif()

if(TARGET neograph::llm)
    add_executable(neograph_example_support_tests
        test_example_utf8_prefix.cpp test_jarvis_provider_support.cpp)
    target_link_libraries(neograph_example_support_tests PRIVATE
        neograph::core neograph::llm GTest::gtest_main)
    neograph_discover_tests(neograph_example_support_tests)
endif()

if(TARGET neograph::llm)
    # The engine on a host's event loop (#535): async::HostLoop pumps the io_context from tasks the host runs, and a
    # SchemaProvider built over the SDK's host seam sends through the host's transport. A manual executor stands for
    # the host; no network, no thread.
    add_executable(neograph_host_loop_tests test_host_loop.cpp)
    target_link_libraries(neograph_host_loop_tests PRIVATE neograph::core neograph::llm GTest::gtest_main)
    neograph_discover_tests(neograph_host_loop_tests PROPERTIES TIMEOUT 60 LABELS "host;model-free")
endif()

if(TARGET neograph_program_sqlite AND TARGET neograph_sqlite AND TARGET neograph::llm)
    add_executable(neograph_program_provider_journal_runtime_tests
        test_program_provider_journal_runtime.cpp)
    target_link_libraries(neograph_program_provider_journal_runtime_tests PRIVATE
        neograph::program neograph::program_sqlite neograph::sqlite
        neograph::llm SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_program_provider_journal_runtime_tests)

    # A host-bound provider for llm_call nodes and monetary budgets (#372).
    add_executable(neograph_program_provider_binding_tests test_program_provider_binding.cpp)
    target_include_directories(neograph_program_provider_binding_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/program)
    target_link_libraries(neograph_program_provider_binding_tests PRIVATE
        neograph::program neograph::program_sqlite neograph::sqlite
        neograph::llm SchemaProvider::transport httplib OpenSSL::SSL OpenSSL::Crypto
        SQLite::SQLite3 GTest::gtest_main)
    if(WIN32)
        target_link_libraries(neograph_program_provider_binding_tests PRIVATE advapi32)
    endif()
    target_include_directories(neograph_program_provider_binding_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/deps)
    neograph_discover_tests(neograph_program_provider_binding_tests)

    # Bounded-exhaustive resume/replay harness at the Program level: the
    # runtime as LocalProgramHost wires it, against a reference of the
    # join/interrupt and once-per-run charging contract.
    # NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended runs the larger bound.
    add_executable(neograph_program_resume_model_tests
        test_program_resume_model.cpp test_program_join_recovery.cpp
        test_program_command_recovery.cpp)
    target_include_directories(neograph_program_resume_model_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/program)
    target_link_libraries(neograph_program_resume_model_tests PRIVATE
        neograph::program neograph::program_sqlite neograph::sqlite
        neograph::llm SQLite::SQLite3 GTest::gtest_main)
    if(NEOGRAPH_BUILD_QUICKJS_CONTROL)
        target_compile_definitions(neograph_program_resume_model_tests PRIVATE
            NEOGRAPH_PROGRAM_RESUME_MODEL_HAVE_QUICKJS)
    endif()
    neograph_discover_tests(neograph_program_resume_model_tests PROPERTIES LABELS "program;resume-model" TIMEOUT 300)
endif()

if(NEOGRAPH_BUILD_SQLITE)
    add_executable(neograph_tool_effect_integration_tests test_tool_effects_standalone.cpp)
    target_link_libraries(neograph_tool_effect_integration_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    if(TARGET neograph::llm)
        target_link_libraries(neograph_tool_effect_integration_tests PRIVATE neograph::llm)
        target_compile_definitions(neograph_tool_effect_integration_tests PRIVATE
            NEOGRAPH_TOOL_EFFECT_TESTS_HAVE_LLM)
    endif()
    if(TARGET neograph_program_sqlite)
        target_sources(neograph_tool_effect_integration_tests PRIVATE
            test_program_tool_effects_runtime.cpp)
        target_compile_definitions(neograph_tool_effect_integration_tests PRIVATE
            NEOGRAPH_TOOL_EFFECT_TESTS_HAVE_PROGRAM)
        target_link_libraries(neograph_tool_effect_integration_tests PRIVATE neograph::program_sqlite)
    endif()
    neograph_discover_tests(neograph_tool_effect_integration_tests)
endif()

if(TARGET neograph::llm)
    add_executable(neograph_schema_provider_interface_tests
        test_schema_provider_interface.cpp)
    target_link_libraries(neograph_schema_provider_interface_tests PRIVATE
        neograph::core neograph::llm SchemaProvider::transport
        httplib OpenSSL::SSL OpenSSL::Crypto GTest::gtest_main)
    if(WIN32)
        target_link_libraries(neograph_schema_provider_interface_tests PRIVATE advapi32)
    endif()
    target_include_directories(neograph_schema_provider_interface_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/deps)
    neograph_discover_tests(neograph_schema_provider_interface_tests
        PROPERTIES TIMEOUT 60 LABELS "provider;interface;model-free")

    # What a provider failure costs the token bank: a completed HTTP 4xx answer
    # releases the reservation, every other failure keeps it, and the usage
    # labels (missing, partial, derived) decide what a call is settled on.
    add_executable(neograph_provider_rejection_accounting_tests
        test_provider_rejection_accounting.cpp)
    target_link_libraries(neograph_provider_rejection_accounting_tests PRIVATE
        neograph::core neograph::llm SchemaProvider::transport
        httplib OpenSSL::SSL OpenSSL::Crypto GTest::gtest_main)
    if(WIN32)
        target_link_libraries(neograph_provider_rejection_accounting_tests PRIVATE advapi32)
    endif()
    target_include_directories(neograph_provider_rejection_accounting_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/deps)
    neograph_discover_tests(neograph_provider_rejection_accounting_tests
        PROPERTIES TIMEOUT 60 LABELS "provider;accounting;model-free")
endif()

# Program API v2 guards (#363-#365). Each suite is its own executable so the
# work packages that change Core and Program files never edit these lists.
add_executable(neograph_text_safety_tests test_text_safety.cpp)
target_link_libraries(neograph_text_safety_tests PRIVATE neograph::core GTest::gtest_main)
neograph_discover_tests(neograph_text_safety_tests PROPERTIES LABELS "core;text-safety")

# One classifier for node and tool failures, and effect-aware node retry
# (#364). Reaches into core internals (tool_failure.h), so it is built only
# against the static core, like test_run_context_runtime_registry.cpp.
if(NOT BUILD_SHARED_LIBS)
    add_executable(neograph_node_failure_tests test_node_failure_classifier.cpp)
    target_link_libraries(neograph_node_failure_tests PRIVATE neograph::core GTest::gtest_main)
    neograph_discover_tests(neograph_node_failure_tests PROPERTIES LABELS "core;node-failure")
    # #383: every checkpoint save path writes the same run metadata for the
    # same run state; also checks the one helper they share (core internals).
    add_executable(neograph_checkpoint_metadata_tests test_checkpoint_run_metadata.cpp)
    target_link_libraries(neograph_checkpoint_metadata_tests PRIVATE neograph::core GTest::gtest_main)
    neograph_discover_tests(neograph_checkpoint_metadata_tests PROPERTIES LABELS "core;checkpoint")
endif()
if(NEOGRAPH_BUILD_SQLITE)
    # #364 failure routing runtime: handled failures on every dispatch path,
    # retry timing, the ceiling, max_visits, failure exits, crash-resume.
    add_executable(neograph_failure_routing_tests test_failure_routing_runtime.cpp)
    target_link_libraries(neograph_failure_routing_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_failure_routing_tests PROPERTIES LABELS "core;failure-routing")
    add_executable(neograph_node_failure_effect_tests test_node_failure_effects.cpp)
    target_link_libraries(neograph_node_failure_effect_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_node_failure_effect_tests PROPERTIES LABELS "core;node-failure")
    # #382: only a resume that answers an interrupt skips its interrupt_before
    # gate, including recovery from a Completed checkpoint on SQLite.
    add_executable(neograph_interrupt_gate_tests test_interrupt_resume_gate.cpp)
    target_link_libraries(neograph_interrupt_gate_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_interrupt_gate_tests PROPERTIES LABELS "core;interrupt")
    # #364 attempt timeouts and deadline-aware backoff (WP12) on the
    # dispatched-attempt execution model (#367, #368): the late completion is
    # discarded on every dispatch path, the stop deadline, crash-resume.
    add_executable(neograph_attempt_timeout_tests test_attempt_timeout.cpp)
    target_link_libraries(neograph_attempt_timeout_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_attempt_timeout_tests PROPERTIES LABELS "core;failure-routing")
    # #385: a Send target that interrupts pauses the run like any other node,
    # on every entry point, in memory and on SQLite.
    add_executable(neograph_send_interrupt_tests test_send_interrupt.cpp)
    target_link_libraries(neograph_send_interrupt_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_send_interrupt_tests PROPERTIES LABELS "core;interrupt")
    # #390: a fork of a NodeInterrupt checkpoint carries its pending writes, so
    # the forked run replays the finished siblings as a resume does.
    add_executable(neograph_fork_pending_writes_tests test_fork_pending_writes.cpp)
    target_link_libraries(neograph_fork_pending_writes_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_fork_pending_writes_tests PROPERTIES LABELS "core;interrupt")
    # #389: one Send and many Sends follow one contract: a payload is visible
    # only to its own Send, live, retried and replayed.
    add_executable(neograph_send_isolation_tests test_send_isolation.cpp)
    target_link_libraries(neograph_send_isolation_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_send_isolation_tests PROPERTIES LABELS "core;send")
    # #424: RunResult::execution_trace lists every node execution, Send-started
    # ones included, in one order, on every entry point.
    add_executable(neograph_execution_trace_tests test_execution_trace.cpp)
    target_link_libraries(neograph_execution_trace_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_execution_trace_tests PROPERTIES LABELS "core;send")
    # #388: several NodeInterrupts of one super-step are answered one at a
    # time, by the interrupt a resume names.
    add_executable(neograph_interrupt_identity_tests test_interrupt_identity.cpp)
    target_link_libraries(neograph_interrupt_identity_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_interrupt_identity_tests PROPERTIES LABELS "core;interrupt")
    # #393: a resume value reaches only the invocation the resume answers, in
    # the step it re-enters; later steps ask again, on every checkpoint store.
    add_executable(neograph_resume_value_scope_tests test_resume_value_scope.cpp)
    target_link_libraries(neograph_resume_value_scope_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_backend_tests(neograph_resume_value_scope_tests "core;interrupt")
    # #392: a fan-out step and the Sends of a step end by one rule, whatever
    # the index of the sibling that failed or paused, with one worker or many.
    add_executable(neograph_step_outcome_tests test_step_outcome.cpp)
    target_link_libraries(neograph_step_outcome_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_step_outcome_tests PROPERTIES LABELS "core;interrupt")
    # #396: each interrupt_before gate of a step is answered on its own; one
    # resume no longer passes every gate of the step it re-enters.
    add_executable(neograph_gate_identity_tests test_gate_identity.cpp)
    target_link_libraries(neograph_gate_identity_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_backend_tests(neograph_gate_identity_tests "core;interrupt")
endif()

if(NEOGRAPH_BUILD_PROGRAM)
    # Golden identity corpus: pins definition hashes, compiled-plan identities
    # and bundle ids. Regenerating it is opt-in; see test_identity_golden.cpp.
    add_executable(neograph_identity_golden_tests test_identity_golden.cpp)
    target_link_libraries(neograph_identity_golden_tests PRIVATE
        neograph::program GTest::gtest_main)
    target_compile_definitions(neograph_identity_golden_tests PRIVATE
        NEOGRAPH_IDENTITY_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/identity_corpus")
    if(NEOGRAPH_BUILD_QUICKJS_CONTROL)
        target_compile_definitions(neograph_identity_golden_tests PRIVATE
            NEOGRAPH_IDENTITY_CORPUS_HAVE_QUICKJS)
    endif()
    neograph_discover_tests(neograph_identity_golden_tests PROPERTIES LABELS "program;identity")
endif()

# Bounded-exhaustive join harness: the real Scheduler and GraphEngine against
# a reference interpreter of the #363 join semantics (tests/model/ has the TLA+
# model). NEOGRAPH_JOIN_MODEL_BOUND=extended runs the larger bound.
add_executable(neograph_join_model_tests test_join_exhaustive.cpp)
target_link_libraries(neograph_join_model_tests PRIVATE neograph::core GTest::gtest_main)
neograph_discover_tests(neograph_join_model_tests PROPERTIES LABELS "core;join-model" TIMEOUT 300)

# Bounded-exhaustive resume/replay harness: the real GraphEngine against a
# reference interpreter of the interrupt/resume contract, through every answer
# order, a crash at every durable point and a fork of every pause.
# NEOGRAPH_RESUME_MODEL_BOUND=extended runs the larger bound.
if(NEOGRAPH_BUILD_SQLITE)
    add_executable(neograph_resume_model_tests test_resume_model.cpp)
    target_link_libraries(neograph_resume_model_tests PRIVATE
        neograph::core neograph::sqlite SQLite::SQLite3 GTest::gtest_main)
    neograph_discover_tests(neograph_resume_model_tests PROPERTIES LABELS "core;resume-model" TIMEOUT 300)
endif()

# Opt-in soundness check (X3): findings on hand-written graphs, the Program
# compiler path, and agreement with the join harness's reference interpreter on
# every graph of its family. NEOGRAPH_JOIN_MODEL_BOUND=extended runs the larger
# bound here too.
add_executable(neograph_graph_soundness_tests test_graph_soundness.cpp)
target_link_libraries(neograph_graph_soundness_tests PRIVATE neograph::core GTest::gtest_main)
if(NEOGRAPH_BUILD_PROGRAM)
    target_include_directories(neograph_graph_soundness_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/program)
    target_link_libraries(neograph_graph_soundness_tests PRIVATE neograph::program)
    target_compile_definitions(neograph_graph_soundness_tests PRIVATE NEOGRAPH_SOUNDNESS_HAVE_PROGRAM)
    if(NEOGRAPH_BUILD_QUICKJS_CONTROL)
        target_compile_definitions(neograph_graph_soundness_tests PRIVATE
            NEOGRAPH_SOUNDNESS_HAVE_QUICKJS)
    endif()
endif()
neograph_discover_tests(neograph_graph_soundness_tests PROPERTIES LABELS "core;soundness" TIMEOUT 300)

if(NEOGRAPH_BUILD_PROGRAM)
    # Program API v2 (#362-#365): diagnostics help, the capability manifest,
    # graph metadata, any_of join slots, witness paths and failure routing keys.
    add_executable(neograph_program_v2_tests
        test_program_diagnostics_help.cpp
        test_capability_manifest.cpp
        test_graph_metadata.cpp
        test_any_of_slots.cpp
        test_witness_paths.cpp
        test_failure_routing_keys.cpp
    )
    target_include_directories(neograph_program_v2_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/program)
    target_link_libraries(neograph_program_v2_tests PRIVATE
        neograph::program GTest::gtest_main)
    target_compile_definitions(neograph_program_v2_tests PRIVATE
        NEOGRAPH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    if(NEOGRAPH_BUILD_QUICKJS_CONTROL)
        target_compile_definitions(neograph_program_v2_tests PRIVATE
            NEOGRAPH_V2_TESTS_HAVE_QUICKJS)
    endif()
    neograph_discover_tests(neograph_program_v2_tests PROPERTIES LABELS "program;program-v2")
endif()

# Program pricing from the SchemaProvider model catalogue (#520): parity with
# the 0.15 price catalog, reservation bounds, refusals and settlement. No
# network. Built with the LLM component, whose provider types it prices.
if(NEOGRAPH_BUILD_PROGRAM AND TARGET neograph::llm)
    add_executable(neograph_pricing_tests test_pricing.cpp)
    target_include_directories(neograph_pricing_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/program)
    target_link_libraries(neograph_pricing_tests PRIVATE
        neograph::program neograph::llm SchemaProvider::transport httplib OpenSSL::SSL OpenSSL::Crypto
        GTest::gtest_main)
    if(WIN32)
        target_link_libraries(neograph_pricing_tests PRIVATE advapi32)
    endif()
    target_compile_definitions(neograph_pricing_tests PRIVATE
        NEOGRAPH_MODEL_PRICES="${PROJECT_SOURCE_DIR}/tests/fixtures/pricing/model_prices.json"
        NEOGRAPH_MONETARY_SOURCE="${PROJECT_SOURCE_DIR}/src/program/monetary.cpp")
    neograph_discover_tests(neograph_pricing_tests PROPERTIES LABELS pricing)
endif()
