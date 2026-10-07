add_executable(test_stress test_stress.cpp test_health_checker_stress.cpp)
target_link_libraries(test_stress PRIVATE GTest::gtest_main helios_lib helios_test_common)
target_include_directories(test_stress PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_test(NAME stress_test_multi_conn COMMAND test_stress)
set_tests_properties(stress_test_multi_conn PROPERTIES LABELS "stress")
