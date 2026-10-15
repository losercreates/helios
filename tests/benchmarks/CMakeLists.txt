add_library(helios_epoll_baseline STATIC
    epoll_baseline.cpp
)

target_include_directories(helios_epoll_baseline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(helios_epoll_baseline PUBLIC
    helios_lib
    helios_compiler_flags
    helios_sanitizers
)

add_executable(helios_benchmarks
    bench_foundation.cpp
)

target_link_libraries(helios_benchmarks PRIVATE
    helios_epoll_baseline
    helios_lib
    helios_compiler_flags
    helios_sanitizers
    benchmark::benchmark
)
