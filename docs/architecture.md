# Helios Core Architecture & Build Foundation

## Overview

Helios is a Linux-native high-performance reverse proxy and load balancer implemented in C++20 built directly around `io_uring`.

## Build System Requirements

- **C++ Compiler**: GCC 11+ or Clang 13+ with C++20 standard support.
- **Build Generator**: CMake 3.22+ and Make/Ninja.
- **Dependencies**: Linux Kernel 5.19+ (for `io_uring`), `liburing-dev`, `GoogleTest` (fetched automatically via CMake), `Google Benchmark` (fetched automatically via CMake).

## Repository Structure

```text
helios/
├── CMakeLists.txt              # Root CMake configuration
├── config/                     # Declarative configuration presets
├── docs/                       # Architectural documentation
├── src/                        # Modular C++ implementation
│   ├── main.cpp                # Main executable entry point
│   ├── io/                     # io_uring Ring Engine abstraction
│   ├── net/                    # Socket handling & ConnectionPair state machine
│   ├── buffer/                 # Fixed-capacity BufferPool memory manager
│   ├── http/                   # Zero-copy HTTP/1.1 streaming parser
│   ├── lb/                     # Load balancing algorithms
│   ├── backend/                # Backend endpoint management
│   ├── health/                 # Health checking & CircuitBreaker
│   ├── config/                 # YAML configuration parser
│   └── metrics/                # Lockless telemetry & Prometheus export
├── tests/
│   ├── unit/                   # Unit test targets (GoogleTest)
│   ├── integration/            # Multi-component integration test targets
│   └── benchmarks/             # Google Benchmark & comparative epoll baseline
└── fuzz/                       # libFuzzer targets for parser security
```

## Build Configurations & Commands

### 1. Prerequisites (WSL / Ubuntu)
```bash
sudo apt update && sudo apt install -y build-essential cmake clang g++ liburing-dev
```

### 2. Standard Debug Build & Test
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### 3. Release Build & Benchmarks
```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
./build-release/tests/benchmarks/helios_benchmarks
```

### 4. Sanitizer Builds
- **AddressSanitizer (ASan) & UndefinedBehaviorSanitizer (UBSan)**:
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_ENABLE_ASAN=ON -DHELIOS_ENABLE_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

- **ThreadSanitizer (TSan)**:
```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_ENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure
```
