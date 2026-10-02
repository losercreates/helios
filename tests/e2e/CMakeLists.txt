set(E2E_TESTS
    test_e2e
    test_data_integrity
    test_backpressure
    test_connection_lifecycle
    test_fault_injection
)

foreach(test_name IN LISTS E2E_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE GTest::gtest_main helios_lib helios_test_common)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/../common)
    
    # Add to CTest
    add_test(NAME e2e_${test_name} COMMAND ${test_name})
    set_tests_properties(e2e_${test_name} PROPERTIES LABELS "e2e")
endforeach()
