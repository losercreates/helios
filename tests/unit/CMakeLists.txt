add_executable(helios_unit_tests
    test_foundation.cpp
    test_ring_engine.cpp
    test_tcp_net.cpp
    test_connection_pair.cpp
    test_buffer_pool.cpp
    test_round_robin.cpp
    test_weighted_round_robin.cpp
    test_least_connections.cpp
    test_consistent_hash.cpp
    test_health_management.cpp
    test_circuit_breaker.cpp
    test_backend_selection_matrix.cpp
    test_http_parser.cpp
    test_backend_pool.cpp
    test_epoll_baseline.cpp
    test_prometheus_server.cpp
    test_config_loader.cpp
    test_access_logger.cpp
    test_l7_router.cpp
    test_http_rewriter.cpp
)

target_link_libraries(helios_unit_tests PRIVATE
    helios_lib
    helios_epoll_baseline
    helios_compiler_flags
    helios_sanitizers
    gtest
    gtest_main
    uring
)

target_compile_definitions(helios_unit_tests PRIVATE
    HELIOS_TEST_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
)

include(GoogleTest)
gtest_discover_tests(helios_unit_tests)
