<!--
# Sync Impact Report
- **Version Change**: Template Initial → 1.0.0
- **Modified Principles**: N/A (Initial instantiation)
- **Added Principles**:
  - I. Correctness and Protocol Compliance
  - II. Measured Performance
  - III. Explicit Asynchronous Ownership
  - IV. Backpressure and Bounded Resources
  - V. Testable Concurrency
  - VI. Failure Containment
  - VII. Linux-Native Engineering
  - VIII. Incremental Complexity
  - IX. Honest Zero-Copy Terminology
  - X. Observability
  - XI. Security by Default
  - XII. Reproducible Benchmarking
  - XIII. Minimal Complexity
  - XIV. Documentation and Architectural Reasoning
  - XV. Production-Like Engineering
- **Added Sections**:
  - Core Principles
  - Technology Stack & Systems Constraints
  - Verification & Quality Gates
  - Governance
- **Removed Sections**: None
- **Follow-up TODOs**: None
-->

# Helios Constitution

## Core Principles

### I. Correctness and Protocol Compliance
Correct behavior MUST take precedence over throughput under all circumstances. Developers MUST NOT introduce undefined behavior, data races, use-after-free, double-free, stale completion handling, or protocol violations to optimize performance. HTTP parsing state machines and connection lifecycle transitions MUST have explicitly defined, deterministic semantics.

*Rationale*: A high-performance proxy that corrupts state, drops connection invariant safety, or violates protocol specifications is unsafe and unusable in production environments.

### II. Measured Performance
All performance claims MUST be experimentally verified under controlled, reproducible conditions. Systems design decisions MUST NOT assume io_uring is inherently faster than epoll, nor that advanced io_uring features (such as registered buffers, SQPOLL, or multishot ring operations) improve performance for a given workload without empirical proof. All performance targets MUST be treated as unverified hypotheses until measured.

*Rationale*: Optimization without measurement leads to premature complexity, misleading architectural decisions, and undetected performance regressions.

### III. Explicit Asynchronous Ownership
Every asynchronous operation MUST enforce explicit ownership and lifetime rules for buffers, file descriptors, and control structures. `io_uring` submission ring entries (SQEs) and completion ring entries (CQEs) MUST NEVER reference state that has been freed, modified, or re-allocated prior to completion processing. Buffer lifecycles MUST be explicitly modeled from acquisition/pool checkout through submission, completion, and reuse. Ring completion ordering and operation cancellations MUST be handled deliberately.

*Rationale*: Asynchronous kernel rings decoupled from user-space call stacks cause severe memory corruption or silent data corruption when references outlive their buffers or handlers.

### IV. Backpressure and Bounded Resources
Every system resource—including socket read/write buffers, application queues, fixed buffer pools, concurrent connections, pending io_uring submissions, and inflight requests—MUST enforce explicit, configurable upper bounds. When resource limits are reached, backpressure MUST propagate immediately upstream (e.g., pausing socket reads). Resource exhaustion MUST produce deterministic, non-crashing behavior (e.g., gracefully rejecting new connections or shedding load). Unbounded queueing or dynamic memory growth under load is strictly forbidden.

*Rationale*: Unbounded resource allocation under heavy load leads to catastrophic out-of-memory crashes, bufferbloat, and unpredictable tail latencies.

### V. Testable Concurrency
Helios SHOULD prefer simple, deterministic concurrency architectures, specifically thread-per-core / shared-nothing models, to eliminate cross-thread synchronization overhead. Shared mutable state and hot-path locking MUST be avoided wherever possible. Any remaining concurrency mechanisms MUST be testable (e.g., via thread sanitizers, stress tests, and deterministic scheduling tests). Thread safety MUST be empirically demonstrated rather than assumed.

*Rationale*: Complex lock hierarchies and shared mutable state cause race conditions, lock contention bottlenecks, and intractable production failures.

### VI. Failure Containment
All failure paths—including backend server disconnects, client resets, malformed HTTP payloads, upstream/downstream timeouts, resource exhaustion, io_uring submission/completion errors, worker thread failures, and shutdown race conditions—MUST be explicitly handled and contained. A failure on one client connection or backend socket MUST NOT corrupt, delay, or terminate unrelated connections or workers.

*Rationale*: Fault isolation ensures high availability and prevents single bad actors, protocol anomalies, or backend faults from cascading across the entire load balancer.

### VII. Linux-Native Engineering
Helios MUST directly utilize Linux-native system primitives (e.g., `io_uring`, `epoll`, `splice`, `eventfd`) when they materially improve latency/throughput or provide foundational engineering value. `io_uring` MUST be treated as a core system technology rather than hidden behind general-purpose asynchronous networking frameworks (e.g., Boost.Asio, libuv). Avoid unnecessary external abstraction frameworks on the core event loop path.

*Rationale*: Abstracting away kernel I/O primitives behind generic frameworks introduces unnecessary overhead, obscures execution semantics, and defeats the performance and systems-learning goals of a native io_uring architecture.

### VIII. Incremental Complexity
System capabilities MUST be constructed incrementally, starting from a minimal, fully correct baseline (e.g., single-threaded L4 TCP proxy) before advancing to complex features (e.g., multi-worker execution, L7 HTTP parsing, registered buffers, SQPOLL, or eBPF integration). Advanced optimizations MUST NOT be introduced until simpler baselines are verified for correctness and benchmarked. Experimental or research features MUST remain strictly isolated from the core stable path.

*Rationale*: Layering complex optimizations onto an unverified architecture makes root-cause analysis impossible and masks fundamental design defects.

### IX. Honest Zero-Copy Terminology
Engineers MUST NOT casually claim "zero-copy" without precise technical characterization. Every data path MUST explicitly document where memory copies occur—distinguishing application-level copies, kernel-to-userspace transfers, user-to-kernel transfers, buffer reuse strategies, and true zero-copy mechanisms (e.g., `MSG_ZEROCOPY`, `splice`). Data path copy counts MUST be measured and documented.

*Rationale*: Imprecise terminology obscures performance realities, creates false assumptions, and hides memory bandwidth bottlenecks.

### X. Observability
Essential performance, resource utilization, and failure behaviors MUST be observable in real-time. Helios MUST expose fine-grained metrics—including throughput (bytes/sec, reqs/sec), latency histograms (p50, p99, p999), active connection counts, error counters, backend health state, io_uring queue depth and drop rates, buffer pool usage, and resource exhaustion events. Debugging production-like failures MUST be possible via telemetry and non-invasive logging without modifying core logic or recompiling.

*Rationale*: Unobservable networking systems cannot be reliably tuned, debugged, or operated in production environments.

### XI. Security by Default
All incoming traffic parsers (specifically L7 HTTP engines) MUST strictly validate input to prevent request smuggling, HTTP header injection, malformed payload abuse, resource exhaustion, Slowloris attacks, and connection abuse. Parser validation MUST NOT be weakened for performance gains. TLS configurations MUST enforce secure defaults (strong cipher suites, modern protocol versions) and MUST NOT permit accidental insecure fallbacks or downgrades.

*Rationale*: As an edge or core reverse proxy, Helios is a primary attack target; sacrificing protocol security for throughput compromises the protected application infrastructure.

### XII. Reproducible Benchmarking
All performance benchmarks MUST document the full experimental environment: hardware specifications, OS kernel version, sysctl network tunings, compiler version and build flags, workload characteristics, concurrency levels, and exact benchmark client configurations (`wrk2`, `vegeta`, etc.). Comparative benchmarks against `epoll`, Nginx, or HAProxy MUST use equivalent workloads and resource limits, explicitly stating architectural limitations. Benchmark results MUST NEVER be manufactured or cherry-picked.

*Rationale*: Irreproducible or biased benchmarks lead to invalid technical decisions and obscure actual performance characteristics.

### XIII. Minimal Complexity
Every abstraction, wrapper, or design pattern MUST justify its complexity with measurable clarity or performance benefits. Engineers MUST prefer explicit data structures, linear memory layouts, and direct ownership over clever abstractions or deep inheritance hierarchies. Hot paths MUST remain straightforward enough to profile with standard tools (`perf`, `eBPF`) and reason about directly.

*Rationale*: Over-engineered abstractions impair CPU cache locality, complicate profiling, and increase maintenance overhead.

### XIV. Documentation and Architectural Reasoning
Significant architectural design decisions, optimization attempts, and protocol tradeoffs MUST be documented in Architecture Decision Records (ADRs). Each ADR MUST state the problem context, evaluated alternatives, decision rationale, and empirical measurements or technical justifications.

*Rationale*: Unrecorded architectural decisions lead to redundant technical debates, accidental regressions, and loss of core design intent over time.

### XV. Production-Like Engineering
Non-functional operational requirements—including declarative configuration parsing/validation, graceful worker shutdown, zero-downtime reconfiguration capability, active and passive backend health checking, comprehensive test suites (unit, integration, sanitizer, stress), and detailed operational documentation—MUST be engineered as first-class system capabilities alongside core proxying logic.

*Rationale*: High-performance networking software that cannot be safely reconfigured, gracefully drained, or reliably monitored fails in real-world deployment scenarios.

## Technology Stack & Systems Constraints

Helios is implemented in modern C++ (C++20/C++23) targeting modern Linux kernels supporting `io_uring`.

- **Kernel Requirements**: Linux kernel version 5.19+ (or specified LTS release with required `io_uring` features).
- **Core Dependencies**: Native C++ standard library and Linux system headers (`liburing` permitted for raw ring interaction). External event loop or async frameworks (e.g., Boost.Asio, libuv) are explicitly forbidden on the network data path.
- **Memory Management**: Custom buffer pools, fixed buffer arrays, or arenas with strict deterministic allocation limits. No dynamic `malloc`/`new` on the per-packet or per-request hot path.
- **Compiler Requirements**: Clang/GCC with strict warning flags (`-Wall -Wextra -Werror -Wpedantic`) and sanitizer support (ASan, TSan, UBSan).

## Verification & Quality Gates

All contributions to Helios MUST pass mandatory quality and verification gates prior to merging:

1. **Sanitizer Cleanliness**: Zero errors under AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan).
2. **Protocol Conformance**: Automated compliance tests for HTTP/1.x parsing, connection framing, and state machine transitions.
3. **Regression Benchmarking**: Automated baseline performance tests ensuring no regressions in latency or throughput.
4. **Static Analysis & Formatting**: Clean passes on `clang-tidy` and `clang-format`.

## Governance

This Constitution is the supreme engineering authority for the Helios project. All specifications (`spec.md`), implementation plans (`plan.md`), task lists (`tasks.md`), pull requests, and code reviews MUST conform to these principles.

- **Amendments**: Amending this constitution requires an explicit proposal specifying rationale, an Architecture Decision Record (ADR), and empirical benchmark data (if modifying performance or resource rules).
- **Versioning Policy**:
  - **MAJOR**: Incompatible changes to governance rules, removal of core principles, or fundamental shifts in system goals.
  - **MINOR**: Addition of new principles, standards, or non-breaking expansions to existing sections.
  - **PATCH**: Non-semantic edits, formatting improvements, or wording clarifications.
- **Compliance Review**: All pull requests MUST be reviewed against this document. Architectural deviations MUST be rejected unless accompanied by an approved constitution amendment.

**Version**: 1.0.0 | **Ratified**: 2026-09-19 | **Last Amended**: 2026-09-19
