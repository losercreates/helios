#include <benchmark/benchmark.h>
#include "foundation.hpp"
#include <vector>

static void BM_GetSystemInfo(benchmark::State& state) {
    for (auto _ : state) {
        auto info = helios::GetSystemInfo();
        benchmark::DoNotOptimize(info);
    }
}
BENCHMARK(BM_GetSystemInfo);

static void BM_ValidateBufferSpan(benchmark::State& state) {
    const auto size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> buffer(size, 0xAA);
    for (auto _ : state) {
        bool result = helios::ValidateBufferSpan(buffer);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_ValidateBufferSpan)->Range(64, 65536);

BENCHMARK_MAIN();
