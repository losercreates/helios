# Technical Research & Architecture Decisions: Helios

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Completed  

---

## 1. Executive Summary

Helios is a Linux-native high-performance reverse proxy and load balancer implemented in modern C++ (C++20) using `io_uring` and `liburing`. This document details the research findings, design trade-offs, and architectural choices that satisfy the requirements of the Helios Specification and Constitution.

---

## 2. Technical Context & Trade-Off Analysis

### Decision 1: `io_uring` SQE User-Data Tagging and Operation Context Lifetime

- **Context**: Every `io_uring` SQE accepts a 64-bit `user_data` payload returned in the corresponding CQE upon completion. Operations can complete out-of-order, be canceled, or complete after a connection has initiated teardown.
- **Decision**: Use a raw pointer to a reference-counted `OpContext` structure (`reinterpret_cast<uint64_t>(op_context_ptr)`). The `OpContext` holds a `std::shared_ptr` or intrusively ref-counted handle to the `ConnectionPair`.
- **Rationale**:
  - `user_data` pointing to an intrusively reference-counted `OpContext` guarantees that the connection state and memory buffer cannot be deleted or returned to the pool while the kernel has a pending SQE.
  - When a CQE arrives, the worker decrements `pending_cqe_count`. Only when `pending_cqe_count == 0` and state is `Closing` or `Pending-Cancellation` are connection resources reclaimed.
- **Alternatives Considered**:
  - *32-bit Connection Index + Generation Counter*: Requires a global array lookup per CQE. Adds cache miss overhead without safety benefits over intrusive reference counting.
  - *Direct Socket Descriptor Tagging*: Fails when multiple ops (e.g., concurrent Read and Write) are outstanding on the same socket.

---

### Decision 2: Fixed Buffer Management and Allocation Strategy

- **Context**: High-QPS networking engines cannot invoke heap allocators (`malloc`/`new`) on the packet forwarding hot path.
- **Decision**: Pre-allocate a contiguous `BufferPool` per worker thread at initialization. Each pool contains $N$ fixed-size memory slots (e.g., 4,096 blocks of 16 KB).
- **Rationale**:
  - Direct pointer indexing allows $O(1)$ checkout and checkin.
  - Enforces hard memory bounds per worker (e.g., 64 MB max per worker).
  - Enables seamless integration with `io_uring` fixed buffers (`IORING_REGISTER_BUFFERS`) in advanced performance experimentation phases.
- **Alternatives Considered**:
  - *Dynamic `std::vector<char>` Allocation*: Violates Principle IV (Bounded Resources) and NFR-003 (Zero Heap Allocations on Hot Path).
  - *Native `IORING_OP_PROVIDE_BUFFERS` Ring*: Excellent kernel feature, but adds kernel complexity for MVP. Kept as an experimental feature (Hypothesis H-2).

---

### Decision 3: Backpressure & Buffer Pool Exhaustion Policy

- **Context**: When downstream backends slow down, write queues back up. If buffer pool utilization reaches 100%, new socket reads cannot acquire buffers.
- **Decision**: Two-level watermark backpressure system:
  1. **Per-Connection High Watermark**: Pause submitting `IORING_OP_READ` SQEs on downstream client socket when unwritten byte count for that connection exceeds 64 KB.
  2. **Worker-Wide Buffer Exhaustion**: If overall worker `BufferPool` utilization hits 100%, suspend submitting read SQEs on ALL client sockets and temporarily suspend `multishot_accept` on the listening socket. Resume reads and accepts when buffer usage drops below 80% watermark.
- **Rationale**:
  - Allows TCP windowing to naturally throttle the client stack.
  - Prevents memory buffer allocation panics or dynamic allocation fallbacks.
- **Alternatives Considered**:
  - *Dropping Packets / Connection Reset*: Violates Principle I (Correctness First) under temporary load spikes.
  - *Unbounded User-Space Buffering*: Violates Principle IV (Bounded Resources).

---

### Decision 4: Concurrency Architecture & Thread-per-Core Model

- **Context**: Cross-thread mutexes, atomic reference counting on packet paths, and lock contention severely limit multi-core network scaling.
- **Decision**: Strict shared-nothing thread-per-core model. Each worker owns:
  - 1 dedicated `io_uring` instance (`struct io_uring ring`).
  - 1 isolated `BufferPool`.
  - 1 isolated `ConnectionMap`.
  - 1 isolated `MetricsCollector`.
  Incoming TCP connections are load-balanced across worker rings by the Linux kernel using `SO_REUSEPORT` socket options with `multishot_accept`.
- **Rationale**:
  - Eliminates all hot-path locking and atomic operations.
  - Maximizes L1/L2 CPU cache locality.
- **Alternatives Considered**:
  - *Single Acceptor Thread + Worker Task Queue*: Requires lock-free queues and cross-thread connection transfer, introducing cache bouncing and latency jitter.

---

### Decision 5: L7 HTTP/1.1 Parsing Engine & Request Smuggling Defenses

- **Context**: L7 reverse proxies must parse HTTP headers safely without introducing request smuggling vulnerabilities (RFC 9112 compliance).
- **Decision**: Implement an incremental zero-copy streaming HTTP state machine (based on `picohttpparser` or custom zero-allocation state machine).
- **Security Policy**:
  - Reject requests containing BOTH `Content-Length` and `Transfer-Encoding` with HTTP 400 Bad Request.
  - Reject requests with invalid whitespace, control characters, or malformed header line endings (`LF` without `CR`).
  - Hard limits: Max 8 KB header size, max 100 headers per request.
- **Rationale**:
  - Protects upstream backends from HTTP smuggling attacks (CL.TE / TE.CL).
  - Zero allocation: parses header token slices directly pointing into `BufferPool` memory blocks.

---

### Decision 6: Benchmark & Verification Baseline

- **Context**: Principle II (Measured Performance) mandates empirical proof before making performance claims.
- **Decision**: Build a parallel, functionally identical `epoll(7)` event-loop proxy baseline in `tests/benchmarks/epoll_baseline.cpp`.
- **Rationale**:
  - Provides a 1:1 architectural baseline sharing the exact same HTTP parser, buffer pool, and load-balancing algorithms.
  - Allows isolating `io_uring` ring overhead vs `epoll_wait`/`epoll_ctl` syscall overhead under identical test conditions.
