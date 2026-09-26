if(HELIOS_BUILD_TESTS)
    add_subdirectory(unit)
    add_subdirectory(integration)
endif()

if(HELIOS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
