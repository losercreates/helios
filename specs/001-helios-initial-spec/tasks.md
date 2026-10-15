# Tasks: Helios Core Reverse Proxy & Load Balancer

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: In Progress (L4 Core, Load Balancing, Health, Circuit Breaking & Lifecycle Completed)  
**Plan Reference**: [plan.md](file:///home/mayukh/projects/helios/specs/001-helios-initial-spec/plan.md)  
**Spec Reference**: [spec.md](file:///home/mayukh/projects/helios/specs/001-helios-initial-spec/spec.md)  

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project build system, compiler flags, dependencies, and project structure.

- [x] T001 Initialize CMake project build system with C++20 compiler flags (`-Wall -Wextra -Werror -Wpedantic`) in [CMakeLists.txt](file:///home/mayukh/projects/helios/CMakeLists.txt)
- [x] T002 [P] Create modular directory structure (`src/io`, `src/net`, `src/buffer`, `src/lb`, `tests/unit`, `tests/integration`, `tests/e2e`, `tests/stress`, `tests/benchmarks`)
- [x] T003 [P] Configure GoogleTest, Google Benchmark, and Clang sanitizers (ASan, UBSan, TSan) in [CMakeLists.txt](file:///home/mayukh/projects/helios/CMakeLists.txt)

---

## Phase 2: Core Asynchronous I/O & Memory Engine (Blocking Prerequisites)

**Purpose**: Core engine infrastructure that powers all network forwarding and proxy operations.

- [x] T004 Implement `liburing` ring abstraction engine in [src/io/ring_engine.hpp](file:///home/mayukh/projects/helios/src/io/ring_engine.hpp) and [src/io/ring_engine.cpp](file:///home/mayukh/projects/helios/src/io/ring_engine.cpp)
- [x] T005 [P] Implement fixed-capacity `BufferPool` allocator and 100% pause / 80% resume watermark tracking in [src/buffer/buffer_pool.hpp](file:///home/mayukh/projects/helios/src/buffer/buffer_pool.hpp) and [src/buffer/buffer_pool.cpp](file:///home/mayukh/projects/helios/src/buffer/buffer_pool.cpp)
- [x] T006 [P] Implement proxy configuration structures (`L4ProxyServer::Config`) in [src/net/l4_proxy_server.hpp](file:///home/mayukh/projects/helios/src/net/l4_proxy_server.hpp)
- [x] T007 Implement `OpContext` / `ProxyOpContext` user-data tagging and reference-counted deferred operation cleanup in [src/net/l4_proxy_server.hpp](file:///home/mayukh/projects/helios/src/net/l4_proxy_server.hpp) and [src/net/connection_pair.hpp](file:///home/mayukh/projects/helios/src/net/connection_pair.hpp)

---

## Phase 3: Single-Threaded L4 TCP Load Balancing Baseline 🎯 MVP

**Goal**: Deliver a correct, single-threaded L4 TCP reverse proxy forwarding traffic across a backend pool using Round-Robin load balancing.

- [x] T008 [US1] Create unit tests for `BufferPool` acquisition and watermark calculation in [tests/unit/test_buffer_pool.cpp](file:///home/mayukh/projects/helios/tests/unit/test_buffer_pool.cpp)
- [x] T009 [US1] Create unit tests for `RingEngine` SQE preparation and CQE reaping in [tests/unit/test_ring_engine.cpp](file:///home/mayukh/projects/helios/tests/unit/test_ring_engine.cpp)
- [x] T010 [US1] Create unit tests for `SocketUtils` and TCP connection fundamentals in [tests/unit/test_tcp_net.cpp](file:///home/mayukh/projects/helios/tests/unit/test_tcp_net.cpp) and [tests/unit/test_foundation.cpp](file:///home/mayukh/projects/helios/tests/unit/test_foundation.cpp)
- [x] T011 [US1] Implement `ConnectionPair` state machine and bi-directional L4 payload forwarding in [src/net/connection_pair.hpp](file:///home/mayukh/projects/helios/src/net/connection_pair.hpp) and [src/net/connection_pair.cpp](file:///home/mayukh/projects/helios/src/net/connection_pair.cpp)
- [x] T012 [US1] Implement Round-Robin load balancer in [src/lb/round_robin.hpp](file:///home/mayukh/projects/helios/src/lb/round_robin.hpp) and [src/lb/round_robin.cpp](file:///home/mayukh/projects/helios/src/lb/round_robin.cpp)
- [x] T013 [US1] Implement timeout-bounded half-close (`shutdown(SHUT_WR)` with 5s fallback timer) in [src/net/connection_pair.cpp](file:///home/mayukh/projects/helios/src/net/connection_pair.cpp)
- [x] T014 [US1] Build single-worker L4 proxy engine and main entrypoint in [src/net/l4_proxy_server.cpp](file:///home/mayukh/projects/helios/src/net/l4_proxy_server.cpp) and [src/main.cpp](file:///home/mayukh/projects/helios/src/main.cpp)
- [x] T015 [US1] Create end-to-end integration test verifying 100% MD5 data integrity through L4 proxy in [tests/integration/test_l4_proxy_integration.cpp](file:///home/mayukh/projects/helios/tests/integration/test_l4_proxy_integration.cpp) and [tests/e2e/test_data_integrity.cpp](file:///home/mayukh/projects/helios/tests/e2e/test_data_integrity.cpp)

---

## Phase 4: Advanced Load Balancing Algorithms

**Goal**: Support diverse traffic distribution strategies tailored to backend capacities and connection profiles.

- [x] T016 Implement Weighted Round-Robin load balancer with smooth interleaved scheduling in [src/lb/weighted_round_robin.hpp](file:///home/mayukh/projects/helios/src/lb/weighted_round_robin.hpp) and [src/lb/weighted_round_robin.cpp](file:///home/mayukh/projects/helios/src/lb/weighted_round_robin.cpp)
- [x] T017 Implement Least-Connections load balancer prioritizing backends with minimal active client connections in [src/lb/least_connections.hpp](file:///home/mayukh/projects/helios/src/lb/least_connections.hpp) and [src/lb/least_connections.cpp](file:///home/mayukh/projects/helios/src/lb/least_connections.cpp)
- [x] T018 Implement Consistent Hashing load balancer using virtual nodes (Ketama ring) and 64-bit hash rings in [src/lb/consistent_hash.hpp](file:///home/mayukh/projects/helios/src/lb/consistent_hash.hpp) and [src/lb/consistent_hash.cpp](file:///home/mayukh/projects/helios/src/lb/consistent_hash.cpp)
- [x] T019 Add comprehensive unit test suites for all 4 load balancing algorithms in [tests/unit/test_round_robin.cpp](file:///home/mayukh/projects/helios/tests/unit/test_round_robin.cpp), [tests/unit/test_weighted_round_robin.cpp](file:///home/mayukh/projects/helios/tests/unit/test_weighted_round_robin.cpp), [tests/unit/test_least_connections.cpp](file:///home/mayukh/projects/helios/tests/unit/test_least_connections.cpp), and [tests/unit/test_consistent_hash.cpp](file:///home/mayukh/projects/helios/tests/unit/test_consistent_hash.cpp)

---

## Phase 5: Health Management & Active Probing

**Goal**: Automatic detection of unhealthy and recovering backend servers with flapping hysteresis.

- [x] T020 Implement backend health state machine (`Healthy`, `Degraded`, `Unhealthy`, `Recovering`) and configurable success/failure thresholds in [src/lb/backend.hpp](file:///home/mayukh/projects/helios/src/lb/backend.hpp)
- [x] T021 Implement asynchronous TCP health checker engine using `io_uring` connect probes with configurable check intervals and probe timeouts in [src/lb/health_checker.hpp](file:///home/mayukh/projects/helios/src/lb/health_checker.hpp) and [src/lb/health_checker.cpp](file:///home/mayukh/projects/helios/src/lb/health_checker.cpp)
- [x] T022 Add unit test suite for backend health transitions and threshold hysteresis in [tests/unit/test_health_management.cpp](file:///home/mayukh/projects/helios/tests/unit/test_health_management.cpp)
- [x] T023 Add integration test suite (15 test scenarios) for active probing, startup delays, flapping hysteresis, and shutdown safety in [tests/integration/test_health_checker_integration.cpp](file:///home/mayukh/projects/helios/tests/integration/test_health_checker_integration.cpp)

---

## Phase 6: Backend Circuit Breaking & Resilience

**Goal**: Passive fault containment preventing continuous client requests from overwhelming failing backends.

- [x] T024 Implement 3-state circuit breaker (`CLOSED`, `OPEN`, `HALF_OPEN`) with legal transitions, failure counters, cooldown timers, and half-open probe quotas in [src/lb/circuit_breaker.hpp](file:///home/mayukh/projects/helios/src/lb/circuit_breaker.hpp) and [src/lb/circuit_breaker.cpp](file:///home/mayukh/projects/helios/src/lb/circuit_breaker.cpp)
- [x] T025 Integrate circuit breaker into `Backend` entity with per-backend configuration (`ConfigureCircuitBreaker`) and request attempt gating (`CanAttemptRequest`) in [src/lb/backend.hpp](file:///home/mayukh/projects/helios/src/lb/backend.hpp)
- [x] T026 Add unit test suite verifying state transitions, trip thresholds, cooldown recovery, and probe quotas in [tests/unit/test_circuit_breaker.cpp](file:///home/mayukh/projects/helios/tests/unit/test_circuit_breaker.cpp)
- [x] T027 Add integration test suite (5 test scenarios) verifying live TCP traffic tripping, cooldown recovery, probe limits, and resource safety in [tests/integration/test_circuit_breaker_integration.cpp](file:///home/mayukh/projects/helios/tests/integration/test_circuit_breaker_integration.cpp)

---

## Phase 7: Unified Backend Selection, Draining & Lifecycle Integration

**Goal**: Establish one deterministic backend-selection policy across all load balancers with connection-safe draining.

- [x] T028 Implement deterministic 4-tier backend selection precedence rule (`CanAcceptNewTraffic()`) enforcing Draining -> Administrative Eligibility -> Health State -> Circuit Breaker in [src/lb/backend.hpp](file:///home/mayukh/projects/helios/src/lb/backend.hpp)
- [x] T029 Integrate `CanAcceptNewTraffic()` selection policy across all 4 load balancing algorithms ([src/lb/round_robin.cpp](file:///home/mayukh/projects/helios/src/lb/round_robin.cpp), [src/lb/weighted_round_robin.cpp](file:///home/mayukh/projects/helios/src/lb/weighted_round_robin.cpp), [src/lb/least_connections.cpp](file:///home/mayukh/projects/helios/src/lb/least_connections.cpp), [src/lb/consistent_hash.cpp](file:///home/mayukh/projects/helios/src/lb/consistent_hash.cpp))
- [x] T030 Implement active connection tracking and administrative draining (`SetDraining(true)`) preserving existing active client connections without force-closing in [src/lb/backend.hpp](file:///home/mayukh/projects/helios/src/lb/backend.hpp) and [src/net/l4_proxy_server.cpp](file:///home/mayukh/projects/helios/src/net/l4_proxy_server.cpp)
- [x] T031 Implement comprehensive unit test matrix (16 tests) verifying all 12 operational state combinations, 4-tier precedence hierarchy, and exhaustive 48-state Cartesian permutations in [tests/unit/test_backend_selection_matrix.cpp](file:///home/mayukh/projects/helios/tests/unit/test_backend_selection_matrix.cpp)
- [x] T032 Implement real TCP integration test suite (6 tests) verifying dynamic failure redistribution, circuit trip/recovery, graceful draining, and backend removal in [tests/integration/test_backend_lifecycle_integration.cpp](file:///home/mayukh/projects/helios/tests/integration/test_backend_lifecycle_integration.cpp)

---

## Phase 8: End-to-End Stress & Reliability Verification

**Goal**: Validate full system reliability, backpressure, data integrity, and sanitizer clean execution under heavy load.

- [x] T033 Implement end-to-end proxy verification tests (`e2e_test_e2e`, `e2e_test_connection_lifecycle`, `e2e_test_fault_injection`) in `tests/e2e/`
- [x] T034 Implement backpressure and buffer saturation verification test in [tests/e2e/test_backpressure.cpp](file:///home/mayukh/projects/helios/tests/e2e/test_backpressure.cpp)
- [x] T035 Implement high-concurrency multi-connection stress test in [tests/stress/test_stress.cpp](file:///home/mayukh/projects/helios/tests/stress/test_stress.cpp)
- [x] T036 Verify zero memory leaks (ASan), zero undefined behavior (UBSan), and zero data races (TSan) across all 162 CTest test cases (**100% pass rate**)

---

## Phase 9: Future Roadmap (Multi-Worker, L7 Protocol & Benchmarking)

**Goal**: Horizontal core scaling, layer-7 HTTP reverse proxying, telemetry, and epoll comparative benchmarks.

- [x] T037 [P] Implement CPU affinity binding (`pthread_setaffinity_np`) and worker thread isolation in `src/worker.hpp` and `src/worker.cpp`
- [x] T038 [P] Implement `SO_REUSEPORT` multi-worker listening socket distribution in `src/net/listener.hpp` and `src/net/listener.cpp`
- [x] T039 [P] Implement zero-copy HTTP/1.1 streaming parser and RFC 9112 request smuggling validator in `src/http/http_parser.hpp`
- [x] T040 [P] Implement HTTP keep-alive connection pooling in `src/backend/backend_pool.hpp`
- [x] T041 [P] Build parallel 1:1 `epoll` baseline proxy in `tests/benchmarks/epoll_baseline.cpp`
- [x] T042 [P] Create comparative Google Benchmark suite (Helios io_uring vs epoll) in `tests/benchmarks/bench_proxy.cpp`
- [ ] T043 [P] Implement Prometheus `/metrics` telemetry endpoint in `src/metrics/prometheus_server.cpp`
