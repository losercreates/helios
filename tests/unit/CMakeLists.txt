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
)

target_link_libraries(helios_unit_tests PRIVATE
    helios_lib
    helios_compiler_flags
    helios_sanitizers
    gtest
    gtest_main
    uring
)

include(GoogleTest)
gtest_discover_tests(helios_unit_tests)
