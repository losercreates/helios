add_executable(helios_benchmarks
    bench_foundation.cpp
)

target_link_libraries(helios_benchmarks PRIVATE
    helios_lib
    helios_compiler_flags
    helios_sanitizers
    benchmark::benchmark
)
