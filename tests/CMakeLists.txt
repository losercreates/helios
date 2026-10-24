if(HELIOS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(HELIOS_BUILD_TESTS)
    # Generate test self-signed certificates dynamically inside build directory
    set(HELIOS_TEST_CERTS_DIR "${CMAKE_BINARY_DIR}/test_certs")
    file(MAKE_DIRECTORY ${HELIOS_TEST_CERTS_DIR})

    find_program(OPENSSL_EXECUTABLE openssl)
    if(OPENSSL_EXECUTABLE AND (NOT EXISTS "${HELIOS_TEST_CERTS_DIR}/server.crt" OR NOT EXISTS "${HELIOS_TEST_CERTS_DIR}/server.key"))
        execute_process(
            COMMAND ${OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048 -nodes
                    -keyout "${HELIOS_TEST_CERTS_DIR}/server.key"
                    -out "${HELIOS_TEST_CERTS_DIR}/server.crt"
                    -days 365
                    -subj "/CN=localhost"
                    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
            RESULT_VARIABLE OPENSSL_GEN_RES
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(NOT OPENSSL_GEN_RES EQUAL 0)
            message(WARNING "Failed to generate test certificates with openssl command")
        endif()
    endif()

    add_compile_definitions(HELIOS_TEST_CERTS_DIR="${HELIOS_TEST_CERTS_DIR}")

    add_subdirectory(common)
    add_subdirectory(unit)
    add_subdirectory(integration)
    add_subdirectory(e2e)
    add_subdirectory(stress)
endif()
