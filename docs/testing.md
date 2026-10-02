# Helios Testing & Validation Infrastructure

This document outlines the testing architecture for the Helios Phase 6 (L4 Proxy) and provides commands for running the various test suites, stress testing, fault injection, and manual validation.

## Test Architecture & Directory Structure

The test infrastructure is separated into distinct layers to ensure components work correctly both in isolation and when integrated end-to-end.

```text
tests/
├── unit/            # Isolated unit tests for internal components (buffer, parsing, RR logic)
├── integration/     # Integration tests verifying interactions between 2+ internal components
├── e2e/             # Comprehensive End-to-End L4 tests (Black-box style via real TCP)
├── stress/          # High-load concurrency and connection storm tests
└── common/          # Reusable test utilities (TestBackend, TestClient, Helpers)
```

**Common Test Library (`tests/common/`)**: Provides a purpose-built TCP TestBackend capable of echoing, fragmenting, delaying, or dropping traffic. It also provides a robust TestClient for precise timing and fragmented sends.

## How to Build

We recommend creating separate build directories for standard testing and sanitizer runs.

### Build (Standard Debug)
```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DHELIOS_BUILD_TESTS=ON -DHELIOS_BUILD_BENCHMARKS=ON
cmake --build build/debug -j$(nproc)
```

## How to Run Automated Tests

Helios uses CTest to orchestrate and categorize tests.

### Run All Tests
```bash
ctest --test-dir build/debug --output-on-failure
```

### Run Unit Tests Only
```bash
ctest --test-dir build/debug -L unit --output-on-failure
```

### Run Integration Tests Only
```bash
ctest --test-dir build/debug -L integration --output-on-failure
```

### Run End-to-End (E2E) Tests Only
```bash
ctest --test-dir build/debug -L e2e --output-on-failure
```

### Run Stress Tests
Stress tests simulate high loads and connection storms. They are separated from standard CI.
```bash
ctest --test-dir build/debug -L stress --output-on-failure
```

## Running with Sanitizers

Sanitizer builds detect memory errors, leaks, and race conditions. Due to `io_uring` architectural constraints, TSan may flag false positives inside kernel boundary interactions, but ASan and UBSan are fully supported.

### ASan (AddressSanitizer) Build & Run
```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_BUILD_TESTS=ON -DHELIOS_ENABLE_ASAN=ON
cmake --build build/asan -j$(nproc)
ctest --test-dir build/asan --output-on-failure
```

### UBSan (UndefinedBehaviorSanitizer) Build & Run
```bash
cmake -S . -B build/ubsan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_BUILD_TESTS=ON -DHELIOS_ENABLE_UBSAN=ON
cmake --build build/ubsan -j$(nproc)
ctest --test-dir build/ubsan --output-on-failure
```

### TSan (ThreadSanitizer) Build & Run
*(Note: May conflict with certain `io_uring` multithreading patterns)*
```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DHELIOS_BUILD_TESTS=ON -DHELIOS_ENABLE_TSAN=ON
cmake --build build/tsan -j$(nproc)
ctest --test-dir build/tsan --output-on-failure
```

## Manual L4 Testing Commands

You can manually test Helios using standard Linux utilities (`nc`, `socat`).

1. **Start a Test Backend (Echo Server)**
   We can use `socat` to create a simple echo server on port 9001:
   ```bash
   socat -v TCP-LISTEN:9001,fork,reuseaddr EXEC:"/bin/cat"
   ```

2. **Start Helios**
   Assuming you have a valid `helios.yaml` configuring port 8080 to route to `127.0.0.1:9001`:
   ```bash
   ./build/debug/bin/helios --config config/helios.yaml
   ```

3. **Connect a TCP Client**
   Use `nc` (netcat) to connect to Helios:
   ```bash
   nc 127.0.0.1 8080
   ```

4. **Send Data & Verify**
   Type into the `nc` terminal and press enter. The data should echo back immediately.

5. **Test Bidirectional Traffic & Half-Close**
   You can pipe data to `nc` and let it close the write side when finished:
   ```bash
   echo "Hello World" | nc -N 127.0.0.1 8080
   ```
   Helios should forward the FIN, the backend will echo the data and close, and Helios will forward the backend FIN to the client.

6. **Observe Backend Failure**
   Stop the `socat` server (`Ctrl+C`) while `nc` is still connected, or before connecting. Helios should cleanly terminate the client connection or refuse it.

7. **Cleanly Stop Helios**
   Send a `SIGTERM` to gracefully shutdown:
   ```bash
   kill -TERM $(pidof helios)
   ```

## Known Platform Limitations
- Requires Linux Kernel 5.19+ for advanced `io_uring` features.
- Memory constraints are tightly bound by the `BufferPool` size configured at startup.
