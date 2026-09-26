# Helios: High-Performance Linux `io_uring` Reverse Proxy & Load Balancer

**Helios** is a high-performance, Linux-native L4/L7 reverse proxy and load balancer implemented in modern **C++20** and built directly on **`io_uring`**. 

Helios follows a strict **thread-per-core, shared-nothing architecture** designed to eliminate lock contention, minimize context switching, and enforce zero dynamic heap allocations on the networking data path.

---

## Technical Context & Architectural Invariants

* **Language Standard**: C++20 (GCC 11+ / Clang 13+) compiled with strict flags (`-Wall -Wextra -Werror -Wpedantic`).
* **Kernel & Async I/O**: Linux Kernel 5.19+ direct `io_uring` integration via `liburing`. No generic event-loop wrappers (Boost.Asio / libuv).
* **Concurrency Model**: Shared-nothing execution per worker thread. No hot-path cross-thread mutexes or shared atomic variables.
* **Operation Ownership**: Explicit asynchronous operation context tagging (`OpContext`) in `SQE.user_data`. Resources and buffer memory remain pinned until kernel CQE completion events are reaped.
* **Verification Gates**: 100% CTest pass rate, 0 AddressSanitizer (ASan) / UndefinedBehaviorSanitizer (UBSan) / ThreadSanitizer (TSan) errors.

---

## Repository Structure

```text
helios/
├── CMakeLists.txt              # Root CMake build configuration
├── README.md                   # Project overview & build guide
├── config/                     # Declarative YAML presets (helios_mvp.yaml, helios_production.yaml)
├── docs/                       # Architecture specifications & design records
├── src/                        # Core source code
│   ├── main.cpp                # Main executable entry point
│   ├── foundation.hpp/.cpp     # System metadata & C++20 primitives
│   └── io/                     # Linux io_uring RingEngine & OpContext tracking
│       ├── op_context.hpp      # Operation lifecycle state & CompletionEvent
│       └── ring_engine.hpp/.cpp# liburing SQE submission / CQE reaping engine
├── tests/
│   ├── unit/                   # GoogleTest unit test suites (test_foundation, test_ring_engine)
│   ├── integration/            # Real kernel I/O tests (test_ring_engine_integration)
│   └── benchmarks/             # Google Benchmark suites (bench_foundation)
└── .github/
    └── workflows/ci.yml        # GitHub Actions CI matrix (Debug, Release, ASan, UBSan, TSan)
```

---

## Prerequisites (Linux / WSL2)

System dependencies required to build and run Helios:

```bash
sudo apt update && sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  g++ \
  clang \
  liburing-dev
```

---

## Build & Test Quickstart

### 1. Standard Debug Build & Test Suite
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### 2. Sanitizer Verification (ASan + UBSan)
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_ENABLE_ASAN=ON -DHELIOS_ENABLE_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

### 3. ThreadSanitizer (TSan)
```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_ENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure
```

### 4. Release Build & Benchmarks
```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
./build-release/bin/helios_benchmarks
```

---

## Verification Summary

* **Unit Tests**: 9 unit tests passing (`FoundationTest.*`, `RingEngineUnitTest.*`).
* **Integration Tests**: 4 integration tests passing (`FoundationIntegrationTest.*`, `RingEngineIntegrationTest.*` verifying real Linux kernel `io_uring` I/O over anonymous pipes).
* **Sanitizers**: Passed cleanly under AddressSanitizer and UndefinedBehaviorSanitizer with zero memory leaks or undefined behavior warnings.
