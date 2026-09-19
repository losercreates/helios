# Implementation Readiness Checklist: Helios Core Reverse Proxy & Load Balancer

**Purpose**: Validate requirement completeness, architectural consistency, and technical readiness before starting implementation  
**Created**: 2026-09-19  
**Feature**: [spec.md](file:///home/mayukh/projects/helios/specs/001-helios-initial-spec/spec.md) | [plan.md](file:///home/mayukh/projects/helios/specs/001-helios-initial-spec/plan.md) | [tasks.md](file:///home/mayukh/projects/helios/specs/001-helios-initial-spec/tasks.md)  

**Review Ownership**: This checklist is a reviewer-owned requirements-quality review artifact. Mark an item `[x]` only when the reviewer determines the requirements-quality criterion is satisfied.  
**Marker Semantics**: `[x]` means the criterion has been reviewed and satisfied for requirements quality. It does not mean implementation work is complete.  

---

## 1. Required Before Implementation

- [ ] CHK001 Are all Functional Requirements (FR-001 through FR-014) mapped to actionable, file-specific tasks in `tasks.md`? [Requirements Completeness, Spec §FR]
- [ ] CHK002 Are compiler options (`C++20`, `-Wall -Wextra -Werror -Wpedantic`) and sanitizer build flags specified in CMake requirements? [Architecture Consistency, Spec §NFR-001]
- [ ] CHK003 Is the `io_uring` ring submission (`SQE`) and completion (`CQE`) lifecycle deterministic with no dynamic heap allocations on the hot path? [io_uring Lifecycle, Spec §NFR-003]
- [ ] CHK004 Does the `user_data` SQE payload tag a reference-counted `OpContext` ensuring connection resources outlive kernel completion events? [Asynchronous Operation Ownership, Data Model §2.3]
- [ ] CHK005 Is the `BufferPool` memory contiguous, fixed-capacity, and pre-allocated per worker at startup? [Buffer Ownership, Data Model §2.2]
- [ ] CHK006 Is the connection state machine explicitly defined with `Pending-Cancellation` tracking `pending_cqe_count` to zero before socket cleanup? [Connection Lifecycle, Data Model §2.4]
- [ ] CHK007 Are partial read and write CQEs (`res < payload_len`) handled by resubmitting byte slices with offset tracking without buffer reallocation? [Partial I/O, Spec §Edge Cases]
- [ ] CHK008 Is a two-tier watermark backpressure policy defined (100% pause read/accept SQEs; 80% resume watermark)? [Backpressure, Spec §FR-013]
- [ ] CHK009 Are hard capacity limits established for maximum connections (10K/50K per worker) and buffer memory (64MB per worker)? [Resource Limits, Spec §13]
- [ ] CHK0010 Are all system timeouts (idle, header read, backend connect, half-close fallback) explicitly specified with default values? [Timeout Behavior, Spec §13]
- [ ] CHK011 Is the thread-per-core, shared-nothing concurrency model free of hot-path cross-worker mutexes or atomics? [Concurrency Model, Constitution §V]
- [ ] CHK012 Are declarative YAML configuration schema fields and validation rules completely documented? [Configuration, Contract §config-schema]

---

## 2. Required Before MVP (Single-Worker L4 TCP Proxy Baseline)

- [ ] CHK013 Are unit tests defined for `BufferPool` acquisition, release, and zero-allocation enforcement? [Testing Strategy, Tasks §Phase 3]
- [ ] CHK014 Are unit tests defined for `RingEngine` SQE preparation, CQE reaping, and kernel ring submission overflow? [Testing Strategy, Tasks §Phase 3]
- [ ] CHK015 Is Round-Robin load balancing specified with deterministic backend selection logic? [Load Balancing, Spec §FR-008]
- [ ] CHK016 Is bi-directional raw TCP stream forwarding verified with 100% data checksum matching? [L4 Proxying, Spec §AC-001]
- [ ] CHK017 Is timeout-bounded TCP half-close (`shutdown(SHUT_WR)` with a 5.0s fallback timer) specified for both client and backend socket closes? [Connection Lifecycle, Spec §FR-002]
- [ ] CHK018 Are backend connection refusal and connect timeout failover behaviors explicitly defined? [Backend Failures, Spec §14]
- [ ] CHK019 Is basic stdout/JSON structured logging defined for connection creation, bytes transferred, and socket errors? [Operational Behavior, Spec §15]
- [ ] CHK020 Is a runnable quickstart validation scenario documented for launching and testing the L4 TCP proxy MVP? [Documentation, Quickstart §3]

---

## 3. Required Before Production-Like Validation

- [ ] CHK021 Is `SO_REUSEPORT` listening socket setup specified for kernel connection distribution across pinned worker threads? [Worker Ownership, Spec §FR-003]
- [ ] CHK022 Does the HTTP/1.1 streaming parser strictly reject ambiguous framing headers (`Content-Length` + `Transfer-Encoding`) with HTTP 400 Bad Request? [HTTP Framing & Security, Spec §SEC-001]
- [ ] CHK023 Are HTTP keep-alive backend connection pools managed independently per worker thread? [L7 Proxying, Spec §FR-006]
- [ ] CHK024 Are active TCP/HTTP health checking probes and passive error circuit breaking with exponential backoff specified? [Health Checking & Reliability, Spec §RL-002/003]
- [ ] CHK025 Is a 15.0-second graceful connection drain window specified upon receiving `SIGTERM`/`SIGINT`? [Shutdown/Draining, Spec §RL-004]
- [ ] CHK026 Is a libFuzzer fuzzing harness configured to run 100,000+ iterations against the HTTP parser? [Fuzzing, Tasks §Phase 5]
- [ ] CHK027 Are mandatory Clang sanitizers (ASan, UBSan, TSan) integrated into CTest automated verification gates? [Sanitizers, Constitution §Verification]
- [ ] CHK028 Is a parallel 1:1 `epoll` baseline proxy implementation specified for honest comparative benchmarking? [Benchmark Methodology, Spec §H-1]
- [ ] CHK029 Are Prometheus metrics (`requests_total`, latency histograms, ring stats) specified for HTTP `/metrics` export? [Observability, Contract §metrics-schema]

---

## 4. Experimental / Research Scope

- [ ] CHK030 Is SQPOLL kernel thread polling (`IORING_SETUP_SQPOLL`) isolated behind experimental configuration flags and benchmark suites? [Research-Feature Isolation, Spec §17]
- [ ] CHK031 Is fixed buffer registration (`IORING_REGISTER_BUFFERS`) isolated as a benchmark hypothesis (H-2) for page-table lookup profiling? [Research-Feature Isolation, Spec §17]
- [ ] CHK032 Is `io_uring` provided buffer ring (`IORING_OP_PROVIDE_BUFFERS`) isolated as an experimental alternative allocator? [Research-Feature Isolation, Spec §17]
- [ ] CHK033 Are zero-copy kernel transfer mechanisms (`splice` / `MSG_ZEROCOPY`) isolated as payload threshold hypotheses (H-4) requiring byte-tracing proof? [Research-Feature Isolation, Spec §17]

---

## Notes

- Mark items `[x]` only after reviewer evaluation confirms that requirements quality criteria are satisfied.
- `/speckit-implement` reads checklist checkbox states as operational quality gates.
