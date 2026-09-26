add_executable(helios_integration_tests
    test_foundation_integration.cpp
    test_ring_engine_integration.cpp
)

target_compile_definitions(helios_integration_tests PRIVATE
    HELIOS_EXECUTABLE_PATH="$<TARGET_FILE:helios>"
)

target_link_libraries(helios_integration_tests PRIVATE
    helios_lib
    helios_compiler_flags
    helios_sanitizers
    gtest
    gtest_main
    uring
)

add_dependencies(helios_integration_tests helios)

include(GoogleTest)
gtest_discover_tests(helios_integration_tests)
