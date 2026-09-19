# Helios Quickstart & Validation Guide

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Completed  

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
    clang-14 llvm-14 libgoogle-gtest-dev wrk
```

---

## 2. Building Helios

```bash
# Clone and configure build directory with ASan & UBSan enabled by default in Debug
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON ..
make -j$(nproc)
```

### Running Unit & Integration Tests
```bash
# Run unit tests
ctest --output-on-failure

# Run ASan/TSan sanitizer test suite
./bin/helios_test
```

---

## 3. Launching & Validating MVP L4 TCP Proxy

```bash
# 1. Start two simple TCP echo backends using netcat
nc -l -k 127.0.0.1 9001 &
nc -l -k 127.0.0.1 9002 &

# 2. Run Helios with sample MVP configuration
./bin/helios --config ../config/helios_mvp.yaml &

# 3. Connect client via telnet / nc and send data
echo "Helios L4 Forwarding Test" | nc 127.0.0.1 8080
```

---

## 4. Running Comparative Benchmarks (Helios vs Epoll Baseline)

```bash
# Build production optimized release bundle
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF ..
make -j$(nproc)

# Run automated benchmarking suite (generates throughput/latency report)
./bin/helios_benchmark --benchmark_out=benchmark_results.json
```
