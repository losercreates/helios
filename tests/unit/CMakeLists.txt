add_executable(helios_unit_tests
    test_foundation.cpp
    test_ring_engine.cpp
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
