add_executable(helios_unit_tests
    test_foundation.cpp
)

target_link_libraries(helios_unit_tests PRIVATE
    helios_lib
    helios_compiler_flags
    helios_sanitizers
    gtest
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(helios_unit_tests)
