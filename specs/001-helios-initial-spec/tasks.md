# Tasks: Helios Core Reverse Proxy & Load Balancer

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Completed  
**Plan Reference**: [plan.md](file:///home/mayukh/projects/helios/specs/001-helios-initial-spec/plan.md)  
**Spec Reference**: [spec.md](file:///home/mayukh/projects/helios/specs/001-helios-initial-spec/spec.md)  

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and basic structure

- [ ] T001 Initialize CMake project build system with C++20 compiler flags (`-Wall -Wextra -Werror -Wpedantic`) in `CMakeLists.txt`
- [ ] T002 [P] Create module directory structure (`src/io`, `src/net`, `src/buffer`, `src/http`, `src/lb`, `src/backend`, `src/health`, `src/config`, `src/metrics`, `tests/unit`, `tests/integration`, `tests/benchmarks`, `fuzz`) per plan
- [ ] T003 [P] Configure GoogleTest, Google Benchmark, and Clang sanitizers (ASan, UBSan, TSan) in `CMakeLists.txt`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

- [ ] T004 Implement `liburing` ring abstraction engine in `src/io/ring_engine.hpp` and `src/io/ring_engine.cpp`
- [ ] T005 [P] Implement fixed-capacity `BufferPool` allocator and 100% pause / 80% resume watermark tracking in `src/buffer/buffer_pool.hpp` and `src/buffer/buffer_pool.cpp`
- [ ] T006 [P] Implement declarative YAML configuration parser and schema validator in `src/config/config_loader.hpp` and `src/config/config_loader.cpp`
- [ ] T007 Implement `OpContext` user-data tagging and reference-counted deferred operation cleanup in `src/io/op_context.hpp`
- [ ] T008 [P] Implement lockless thread-local metric accumulators in `src/metrics/metrics_collector.hpp` and `src/metrics/metrics_collector.cpp`

---

## Phase 3: User Story 1 - Single-Threaded L4 TCP Load Balancing Baseline (Priority: P1) 🎯 MVP

**Goal**: Deliver a correct, single-threaded L4 TCP reverse proxy forwarding traffic to a backend pool using Round-Robin load balancing.

**Independent Test**: Can be tested by launching Helios forwarding port 8080 across echo servers on 9001 and 9002, verifying data integrity via MD5 checksums.

- [ ] T009 [US1] Create unit tests for `BufferPool` acquisition and watermark calculation in `tests/unit/test_buffer_pool.cpp`
- [ ] T010 [US1] Create unit tests for `RingEngine` SQE preparation and CQE reaping in `tests/unit/test_ring_engine.cpp`
- [ ] T011 [US1] Implement `ConnectionPair` state machine and bi-directional L4 payload forwarding in `src/net/connection_pair.hpp` and `src/net/connection_pair.cpp`
- [ ] T012 [US1] Implement Round-Robin load balancer in `src/lb/round_robin.hpp` and `src/lb/round_robin.cpp`
- [ ] T013 [US1] Implement timeout-bounded half-close (`shutdown(SHUT_WR)` with 5s fallback timer) in `src/net/connection_pair.cpp`
- [ ] T014 [US1] Build single-worker L4 proxy engine and main entrypoint in `src/main.cpp`
- [ ] T015 [US1] Create end-to-end integration test verifying 100% MD5 data integrity through L4 proxy in `tests/integration/test_l4_proxy.cpp`

---

## Phase 4: User Story 2 - Multi-Worker Thread-per-Core Execution (Priority: P2)

**Goal**: Scale throughput horizontally across CPU cores without hot-path lock contention using thread-per-core isolation and `SO_REUSEPORT`.

**Independent Test**: Verified by launching Helios across 4 worker threads, measuring linear QPS scaling without lock contention.

- [ ] T016 [US2] Implement CPU affinity binding (`pthread_setaffinity_np`) and worker initialization in `src/worker.hpp` and `src/worker.cpp`
- [ ] T017 [US2] Implement `SO_REUSEPORT` listening socket setup for multi-worker connection distribution in `src/net/listener.hpp` and `src/net/listener.cpp`
- [ ] T018 [US2] Create multi-worker concurrency integration test in `tests/integration/test_multi_worker.cpp`

---

## Phase 5: User Story 3 - L7 HTTP/1.1 Reverse Proxying & Health Checking (Priority: P3)

**Goal**: Provide L7 HTTP/1.1 request routing, smuggling validation, backend connection pooling, and circuit breaking.

**Independent Test**: Tested by sending HTTP/1.1 requests, simulating backend failures, and confirming automatic eject within health interval.

- [ ] T019 [US3] Implement zero-copy HTTP/1.1 streaming parser and RFC 9112 request smuggling validator (`Content-Length` + `Transfer-Encoding` check) in `src/http/http_parser.hpp` and `src/http/http_parser.cpp`
- [ ] T020 [US3] Implement HTTP keep-alive backend connection pool in `src/backend/backend_pool.hpp` and `src/backend/backend_pool.cpp`
- [ ] T021 [US3] Implement active TCP/HTTP health probes and passive failure circuit breaker in `src/health/circuit_breaker.hpp` and `src/health/circuit_breaker.cpp`
- [ ] T022 [US3] Implement Weighted Round-Robin, Least-Connections, and Ketama Consistent Hashing algorithms in `src/lb/load_balancers.cpp`
- [ ] T023 [US3] Create libFuzzer fuzzing harness for HTTP request parsing in `fuzz/fuzz_http_parser.cpp`
- [ ] T024 [US3] Create L7 reverse proxy end-to-end integration test in `tests/integration/test_l7_proxy.cpp`

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: System optimization, telemetry, graceful shutdown, and comparative benchmarking

- [ ] T025 Implement Prometheus `/metrics` text endpoint in `src/metrics/prometheus_server.cpp`
- [ ] T026 Implement 15s graceful connection draining on `SIGTERM` in `src/main.cpp`
- [ ] T027 Build parallel 1:1 `epoll` baseline proxy in `tests/benchmarks/epoll_baseline.cpp`
- [ ] T028 Create comparative Google Benchmark suite (Helios vs epoll) in `tests/benchmarks/bench_proxy.cpp`
- [ ] T029 Execute `quickstart.md` validation scenarios and update operational documentation in `docs/architecture.md`
