# Helios Quickstart & Validation Guide

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Active / Verified  

---

## 1. Environment Setup & Prerequisites

### Prerequisites
- **Operating System**: Linux kernel 5.19 or newer (`uname -r`)
- **Compiler**: GCC 11+ or Clang 13+ with C++20 support
- **Build System**: CMake 3.22+, `ninja` or `make`
- **Libraries**: `liburing-dev` (version 2.2+)

### Installing Dependencies (Debian / Ubuntu 22.04+)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build liburing-dev \
    clang llvm libgoogle-gtest-dev wrk
```

---

## 2. Building Helios & Running Test Suites

### Standard Debug Build
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### AddressSanitizer (ASan) & UndefinedBehaviorSanitizer (UBSan)
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_ENABLE_ASAN=ON -DHELIOS_ENABLE_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

### ThreadSanitizer (TSan)
```bash
# On Linux/WSL2, setarch disables ASLR to prevent TSan shadow memory mapping collisions
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_ENABLE_TSAN=ON
setarch x86_64 --addr-no-randomize cmake --build build-tsan -j$(nproc)
setarch x86_64 --addr-no-randomize ctest --test-dir build-tsan --output-on-failure
```

### Release Build
```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
```

---

## 3. Direct Test Executable Invocations

All tests are partitioned into dedicated, domain-specific executables:

```bash
# Unit Tests (96 tests: Foundation, RingEngine, BufferPool, Load Balancers, Health, Selection Matrix)
./build/bin/helios_unit_tests

# Integration Tests (60 tests: Proxy, Health Checker, Circuit Breaker, Backend Lifecycle)
./build/bin/helios_integration_tests

# End-to-End Tests (5 tests: MD5 Data Integrity, Backpressure, Connection Lifecycle, Fault Injection)
./build/bin/helios_e2e_tests

# Stress Tests (1 test: Multi-Connection Concurrent Saturation)
./build/bin/helios_stress_tests
```

---

## 4. Launching & Validating L4 TCP Proxy

```bash
# 1. Start two simple TCP echo backends
nc -l -k 127.0.0.1 9001 &
nc -l -k 127.0.0.1 9002 &

# 2. Run Helios binary
./build/bin/helios &

# 3. Connect client via netcat and verify bidirectional forwarding
echo "Helios L4 Forwarding Test" | nc 127.0.0.1 8080
```
