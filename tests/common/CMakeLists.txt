add_library(helios_test_common STATIC
    test_backend.cpp
    test_client.cpp
    test_helpers.cpp
)

target_include_directories(helios_test_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(helios_test_common PUBLIC helios_lib)
