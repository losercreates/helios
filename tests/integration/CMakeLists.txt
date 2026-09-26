add_executable(helios_integration_tests
    test_foundation_integration.cpp
)

target_compile_definitions(helios_integration_tests PRIVATE
    HELIOS_EXECUTABLE_PATH="$<TARGET_FILE:helios>"
)

target_link_libraries(helios_integration_tests PRIVATE
    helios_compiler_flags
    helios_sanitizers
    gtest
    gtest_main
)

# Explicitly declare dependency so main binary is built before integration tests run
add_dependencies(helios_integration_tests helios)

include(GoogleTest)
gtest_discover_tests(helios_integration_tests)
