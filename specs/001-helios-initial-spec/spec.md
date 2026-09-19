# Feature Specification: Helios Core Reverse Proxy & Load Balancer

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Draft  
**Input**: User description: "Create the initial product specification for Helios."

---

## 1. Problem Statement

Traditional event-driven reverse proxies and load balancers rely primarily on `epoll(7)` or `kqueue` for I/O multiplexing. While highly scalable compared to blocking or thread-per-connection models, `epoll`-based architectures still incur non-trivial overhead under high connection counts and high packet rates:
- **Syscall Overhead**: Frequent context switches required to register events (`epoll_ctl`), wait for readiness (`epoll_wait`), and execute non-blocking read/write operations (`read`, `write`, `recv`, `send`).
- **Scheduling & Buffer Overhead**: Frequent transitions between user space and kernel space during data transfer, necessitating manual memory buffer management and potential redundant data copies across user-kernel boundaries.
- **Abstraction Layering**: Overhead from general-purpose event-loop abstractions and framework wrappers hiding kernel primitives.

`io_uring` introduces an asynchronous ring-buffer interface between kernel space and user space, enabling batching, submission without context switching (SQPOLL), fixed buffer registration, and multishot operations. **Helios** explores whether a Linux-native proxy designed from the ground up around `io_uring` can deliver a simpler, safer, and higher-throughput architecture for L4 and L7 load balancing while maintaining strict resource bounds and failure containment.

---

## Clarifications

### Session 2026-09-19

- Q: How should Helios handle TCP half-close (FIN received from client or backend) during L4 proxying? → A: Timeout-Bounded Half-Close: When receiving a FIN from side A, Helios forwards `shutdown(SHUT_WR)` to side B, continuing to forward remaining bytes from B to A. A configurable half-close timeout (default 5.0 seconds) is initiated; if side B does not send FIN within the timeout, both sockets are forcefully closed.
- Q: How should io_uring buffer ownership and lifetime be handled when a connection closes while I/O operations are outstanding in the kernel ring? → A: Reference-Counted Buffer Life with Deferred Cleanup: Connection and buffer resources retain ownership until all in-flight CQEs for that connection tag return (pending CQE count reaches 0), avoiding use-after-free and memory corruption races.
- Q: What specific backpressure policy should Helios execute when a worker's buffer pool is fully checked out? → A: Read Pause & Accept Suspension: When a worker's fixed buffer pool reaches 100% checked out, downstream read SQEs and multishot accept SQEs are suspended until checked-out buffers drop below an 80% watermark.
- Q: How strict should the L7 HTTP/1.1 parsing engine be when encountering ambiguous or malformed request framing headers? → A: Strict Reject / HTTP 400: If both `Content-Length` and `Transfer-Encoding` are present, or invalid header formatting is detected, the request is immediately rejected with HTTP 400 Bad Request to prevent smuggling.
- Q: During graceful shutdown (SIGTERM), how should Helios drain idle vs active client and backend connections? → A: Immediate Idle Close + Active Drain Window: Upon receiving `SIGTERM`/`SIGINT`, stop accepting new connections, close idle connections immediately, and allow active requests up to a configurable drain timeout (default 15.0 seconds) to complete before exiting cleanly.

---

## 2. Users & Use Cases

### Primary User Personas
1. **Infrastructure & Platform Engineers**: Operating high-throughput edge proxies or service-mesh ingress nodes requiring high request density, minimal tail latency, and predictable memory bounds.
2. **Backend Developers**: Running high-QPS microservices requiring low-latency load balancing with reliable backpressure and health detection.
3. **Systems & Linux Kernel Engineers**: Investigating asynchronous Linux I/O primitives (`io_uring`), thread-per-core scalability, and benchmarking proxy architectures against legacy `epoll` models.

### Primary Use Cases
- **L4 TCP Load Balancing**: High-throughput layer-4 forwarding for raw TCP streams (databases, gRPC, custom RPC protocols) with minimal CPU and latency overhead.
- **L7 HTTP/1.1 Reverse Proxying**: Layer-7 routing, connection reuse, and header/payload validation for HTTP/1.1 traffic.
- **Backend Service Health & Traffic Management**: Automatic detection and circuit breaking of degraded backend servers using configurable health checks and failover policies.

---

## 3. Goals

- **G-1 (Correctness First)**: Provide deterministic, spec-compliant L4 TCP forwarding and HTTP/1.1 L7 proxying that prevents data corruption, state leakage, or connection desynchronization under all concurrency levels.
- **G-2 (Linux-Native io_uring Architecture)**: Implement a clean C++20 thread-per-core / shared-nothing core engine that directly interfaces with `io_uring` for all network I/O, timer events, and worker notifications.
- **G-3 (Strict Resource Bounding & Backpressure)**: Ensure all buffer pools, socket queues, connection tables, and inflight submissions are strictly bounded, propagating backpressure upstream to prevent OOM panics under extreme load.
- **G-4 (Empirical Performance Verification)**: Establish an automated, reproducible benchmarking harness to rigorously validate latency, throughput, CPU efficiency, and io_uring behavior against epoll baselines and established proxies (Nginx/HAProxy).
- **G-5 (Production Observability & Operability)**: Expose real-time operational telemetry (requests/sec, latency histograms, io_uring ring stats, health state) and support graceful connection draining and reload.

---

## 4. Non-Goals

- **NG-1 (HTTP/2 or HTTP/3 Protocol Support)**: Initial scope excludes multiplexed HTTP/2 or QUIC/HTTP/3 framing state machines.
- **NG-2 (Non-Linux Platform Support)**: Helios will not support BSD, macOS, Windows, or non-Linux kernel abstractions.
- **NG-3 (Arbitrary Scripting/Plugin System)**: No embedded Lua, WebAssembly, or dynamic runtime scripting engine on the network hot path.
- **NG-4 (GUI / Web Dashboard)**: No integrated graphical frontend; metrics are exposed via standard telemetry endpoints (Prometheus-compatible format).
- **NG-5 (Unverified Claims of "Zero-Copy" or Superiority)**: No unqualified assertion that io_uring is faster than epoll without empirical evidence from the benchmark suite.

---

## 5. User Scenarios & Testing *(mandatory)*

### User Story 1 - Single-Threaded L4 TCP Load Balancing Baseline (Priority: P1 - MVP)

As an infrastructure engineer, I want to route client TCP connections to a pool of backend servers using a single-threaded `io_uring` proxy worker so that I can evaluate baseline correctness, throughput, and latency for raw TCP streams.

**Why this priority**: Core foundation of the proxy. Proves asynchronous operation ownership, ring buffer lifecycle, and basic forwarding correctness before adding multi-threading or L7 parsing.

**Independent Test**: Can be tested by launching Helios with a single worker forwarding incoming TCP connections on port 8080 across two echo servers on ports 9001 and 9002, verifying data integrity and load distribution using `netcat` or `tcpbench`.

**Acceptance Scenarios**:
1. **Given** a running Helios L4 proxy configured with 2 healthy backends and a Round-Robin policy, **When** multiple client TCP connections are established and send payloads, **Then** traffic is distributed evenly across backends and all bytes are forwarded without corruption or truncation.
2. **Given** an active client-proxy-backend TCP connection stream, **When** the client sends data at maximum rate, **Then** downstream reads pause when upstream write buffers fill, enforcing backpressure without dropping data or crashing.
3. **Given** an established client connection, **When** either the client or the backend closes the TCP connection gracefully, **Then** Helios detects the close CQE, flushes remaining inflight write buffers, forwards `shutdown(SHUT_WR)` to the opposite socket with a 5s fallback timeout, and cleans up connection resources without leaking file descriptors or memory.

---

### User Story 2 - Multi-Worker Thread-per-Core Execution (Priority: P2)

As a platform engineer operating multi-core servers, I want Helios to run independent worker threads pinned to CPU cores, each managing its own `io_uring` ring, so that system throughput scales horizontally without lock contention.

**Why this priority**: Unlocks full CPU core utilization while maintaining a shared-nothing architecture free of hot-path mutexes.

**Independent Test**: Can be verified by running Helios across 4 worker threads pinned to 4 CPU cores using `SO_REUSEPORT`, sending load via `wrk2`, and confirming linear throughput scaling without cross-thread lock acquisition.

**Acceptance Scenarios**:
1. **Given** a multi-core system with 4 pinned Helios worker threads, **When** client connections hit the listening port, **Then** the kernel balances incoming connection accepts across worker rings via `SO_REUSEPORT` / multishot accept.
2. **Given** 4 active worker threads, **When** observing core utilization under load, **Then** no worker accesses another worker's memory or ring, and CPU performance scales predictably with core count.

---

### User Story 3 - L7 HTTP/1.1 Reverse Proxying & Health Checking (Priority: P3)

As a backend developer, I want Helios to parse HTTP/1.1 requests, route requests to healthy backend servers, automatically eject failing backends, and maintain HTTP keep-alive connection pools.

**Why this priority**: Provides layer-7 intelligence, request-level routing, connection pooling, and fault handling necessary for modern web applications.

**Independent Test**: Tested by launching HTTP backends, simulating a backend failure (500 errors or connection drop), and observing that Helios stops sending traffic to the unhealthy backend within the configured health check interval.

**Acceptance Scenarios**:
1. **Given** HTTP/1.1 client traffic, **When** valid HTTP requests are received, **Then** Helios parses headers, validates `Content-Length`/`Transfer-Encoding`, rejects ambiguous framing with HTTP 400, forwards the request to an available backend, and streams the response back to the client.
2. **Given** a pool of 3 backends where 1 backend begins failing health checks, **When** a client sends subsequent HTTP requests, **Then** Helios routes requests only to the 2 remaining healthy backends and increments the backend error metric.

---

### Edge Cases

- **Partial Reads/Writes**: How does the system handle an `io_uring` read or write completion (`CQE`) returning fewer bytes than submitted? (Must track payload offsets and resubmit remaining byte ranges without reallocating buffers).
- **Abrupt Client Disconnect / TCP Reset (`RST`)**: What happens when a client forcibly drops a TCP connection while a write to the backend is inflight? (Must mark connection state `Pending-Cancellation` and retain buffer reference counts until pending in-flight CQE count reaches 0 before freeing resources).
- **Backend Slowloris / Stalled Writes**: How does Helios react when a backend accepts connections but stops reading data? (Must trigger write timeout timers, close the backend socket, and report a backend timeout fault).
- **Buffer Pool Exhaustion**: How does Helios behave when all fixed buffers in a worker's pool are currently checked out? (Must suspend downstream client socket read SQEs and multishot accept SQEs until checked-out buffers drop below an 80% watermark).
- **Ring Submission Overflow (`EBUSY` / `EAGAIN`)**: What occurs when the kernel SQ ring is full? (Must buffer unsubmitted SQEs in user-space ring fallback queue and flush on subsequent ring loop iterations).

---

## 6. Functional Requirements

### L4 TCP Proxying
- **FR-001**: System MUST accept incoming IPv4 and IPv6 TCP connections on configured listening addresses and ports.
- **FR-002**: System MUST forward raw TCP byte streams bi-directionally between client sockets and selected backend sockets without modifying payload bytes, implementing timeout-bounded half-close (`shutdown(SHUT_WR)` with a 5s fallback timeout).
- **FR-003**: System MUST support configurable TCP socket options, including `TCP_NODELAY`, `SO_REUSEPORT`, and socket read/write buffer sizes (`SO_RCVBUF`, `SO_SNDBUF`).

### L7 HTTP/1.1 Proxying
- **FR-004**: System MUST parse incoming HTTP/1.1 request headers and enforce strict protocol compliance according to RFC 9110 / RFC 9112.
- **FR-005**: System MUST validate `Content-Length` and `Transfer-Encoding: chunked` headers, strictly rejecting ambiguous framing (both headers present or malformed header syntax) with HTTP 400 Bad Request to prevent request smuggling attacks.
- **FR-006**: System MUST maintain HTTP/1.1 keep-alive backend connection pools per worker, reusing existing backend connections for subsequent client requests when available.
- **FR-007**: System MUST inject or append mandatory HTTP forwarding headers (`X-Forwarded-For`, `X-Forwarded-Proto`, `Via`) when configured.

### Load-Balancing Algorithms
- **FR-008**: System MUST support Round-Robin load balancing across backends within a pool.
- **FR-009**: System MUST support Weighted Round-Robin load balancing based on assigned backend weight values.
- **FR-010**: System MUST support Least-Connections load balancing, prioritizing backends with the lowest number of active inflight connections.
- **FR-011**: System MUST support Consistent Hashing (Ketama or bounded-loads hashing) based on client IP or specified HTTP request headers for session affinity.

### Connection Management & Backpressure
- **FR-012**: System MUST enforce an explicit configurable upper limit on maximum concurrent connections per worker thread.
- **FR-013**: System MUST track active byte counts and fixed buffer checkout levels per worker, pausing downstream socket read SQEs and multishot accept SQEs when buffer pool utilization hits 100%, resuming when utilization drops below 80%.
- **FR-014**: System MUST enforce configurable idle connection timeouts, downstream read/write timeouts, upstream backend connect/response timeouts, and half-close fallback timeouts (default 5.0s).

---

## 7. Key Entities

- **Worker**: An independent execution unit bound to a single CPU core, owning a dedicated `io_uring` instance, buffer pool, connection map, and telemetry collector.
- **Connection Pair / Session**: Encapsulates the explicit state machine of a client socket and its associated backend socket, including state (Connecting, Reading, Writing, Closing, Pending-Cancellation), pending io_uring operation reference count, and byte offsets.
- **Buffer Pool**: A fixed-capacity pool of pre-allocated memory buffers managed by a worker, checked out during socket reads and returned immediately upon write completion or after deferred reference-counted cleanup.
- **Backend Pool**: A logical group of backend endpoints sharing a load-balancing strategy, health check configuration, and connection limits.
- **Backend Endpoint**: Represents a remote server destination (`IP:Port`), tracking health status (Healthy, Degraded, Unhealthy), active connection count, latency metrics, and failure history.

---

## 8. Non-Functional Requirements

- **NFR-001 (Language & Standard)**: Core engine MUST be written in modern C++ (C++20 or newer) adhering to strict compiler warning levels (`-Wall -Wextra -Werror -Wpedantic`).
- **NFR-002 (System Dependencies)**: Engine MUST interface directly with the Linux kernel via `io_uring` / `liburing`. External event-loop libraries (Boost.Asio, libuv, libevent) are strictly prohibited on the network data path.
- **NFR-003 (Memory Management)**: Hot-path network processing MUST NOT invoke dynamic heap allocations (`malloc`/`free`/`new`/`delete`). All buffers, SQE metadata contexts, and connection state structs MUST be pre-allocated or pooled. Buffer lifecycle MUST enforce reference-counted deferred cleanup to prevent use-after-free races with kernel rings.
- **NFR-004 (Concurrency Model)**: System MUST follow a strict thread-per-core, shared-nothing architecture. No hot-path mutexes, condition variables, or atomic operations across workers during request forwarding.

---

## 9. Reliability Requirements

- **RL-001 (Failure Containment)**: A malformed request, socket error, or crash on one connection MUST NOT impact or terminate unrelated connections or worker threads.
- **RL-002 (Active Health Checking)**: System MUST perform configurable background health checks (TCP probe or HTTP `GET /health`) against backends, marking unresponsive backends as Unhealthy.
- **RL-003 (Passive Failure Detection & Circuit Breaking)**: System MUST track consecutive backend connection/response failures. If failures exceed a configured threshold, the backend MUST be temporarily removed from the active pool (Circuit Open) with exponential backoff before retry probes (Half-Open).
- **RL-004 (Graceful Draining & Shutdown)**: Upon receiving `SIGTERM` or `SIGINT`, Helios MUST stop accepting new connections, immediately close idle connections, signal active workers to drain existing connections up to a configurable drain timeout (default 15.0s), flush telemetry, and exit cleanly with code 0.

---

## 10. Security Requirements

- **SEC-001 (Strict Protocol Validation)**: L7 HTTP parser MUST reject invalid HTTP header syntax, malformed URIs, null bytes in headers, invalid line endings (`LF` without `CR` if strict mode enabled), and ambiguous framing (`Content-Length` + `Transfer-Encoding`) with HTTP 400 Bad Request.
- **SEC-002 (Slowloris & Resource Exhaustion Protection)**: System MUST enforce a minimum header read rate and maximum header duration timer. Connections failing to complete HTTP request headers within the timeout MUST be immediately closed.
- **SEC-003 (Memory Bounds & Overflow Safety)**: All header, URI, and buffer parsing MUST enforce hard bounds on string lengths, maximum header counts (e.g., max 100 headers), and body limits, preventing stack/heap buffer overflows.
- **SEC-004 (Sanitizer Verification)**: Codebase MUST pass all unit and integration test suites cleanly under AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan).

---

## 11. Observability Requirements

- **OBS-001 (Metrics Telemetry)**: Workers MUST collect and expose key performance counters and histograms:
  - Throughput: Requests/sec, Bytes Read/sec, Bytes Written/sec.
  - Latency: p50, p90, p95, p99, p999 added proxy latency histograms.
  - Connections: Active client connections, active backend connections, total accepted, total closed.
  - Errors: Backend timeouts, connection resets, HTTP 4xx/5xx responses, buffer exhaustion events.
- **OBS-002 (io_uring Telemetry)**: Engine MUST track SQE submission counts, CQE completion counts, kernel SQ overflow count, drop counts, and SQPOLL thread wakeups.
- **OBS-003 (Prometheus Endpoint)**: Metrics MUST be exposed over a designated HTTP metrics endpoint (e.g., `:9090/metrics`) in standard Prometheus text format.

---

## 12. Configuration Requirements

- **CFG-001 (Declarative File Format)**: System MUST load configuration from a structured file (YAML or JSON) validating all fields upon startup.
- **CFG-002 (Configuration Schema Validation)**: Invalid configuration options (e.g., negative timeouts, missing backend pools, malformed IP addresses) MUST cause startup validation failure with descriptive error messages.
- **CFG-003 (Hot Configuration Reload)**: System MUST support reloading backend pool definitions and weights upon receiving `SIGHUP` without dropping active connections.

---

## 13. Resource and Capacity Limits

| Resource Dimension | Default Limit | Maximum Hard Cap | Behavior at Limit |
| :--- | :--- | :--- | :--- |
| Max Connections / Worker | 10,000 | 50,000 | Reject new connection accepts (`EMFILE`/pause accept SQE) |
| Max Buffer Pool Memory / Worker | 64 MB | 512 MB | Pause downstream socket reads & multishot accept until pool drops <80% |
| Max Pending io_uring SQEs | 4,096 | 16,384 | Queue SQE in user-space ring buffer fallback |
| Max HTTP Header Size | 8 KB | 64 KB | Return HTTP 431 Request Header Fields Too Large |
| Max HTTP Request Body Size | 10 MB | 1 GB (Configurable) | Return HTTP 413 Payload Too Large |
| Client Header Read Timeout | 5.0 seconds | 60.0 seconds | Close connection (Slowloris defense) |
| Half-Close Fallback Timeout | 5.0 seconds | 30.0 seconds | Force close remaining socket pair |
| Graceful Drain Timeout | 15.0 seconds | 120.0 seconds | Force close remaining connections and exit process |
| Backend Connect Timeout | 1.0 second | 10.0 seconds | Mark backend attempt failed, retry/failover |

---

## 14. Failure Behavior

- **Backend Connection Refused**: Immediately fail over to next backend in pool if retries remain; otherwise return HTTP 502 Bad Gateway to client.
- **Backend Connection Timeout**: Abort connect SQE, increment backend timeout metric, trigger passive health failure counter, return HTTP 504 Gateway Timeout.
- **Client Unscheduled Close**: Cancel inflight read/write SQEs for connection pair, mark connection state `Pending-Cancellation`, reclaim checked-out buffers upon final CQE reap (pending count = 0).
- **io_uring SQE Submission Failure (`ENOMEM` / `EBUSY`)**: Yield worker loop, submit pending ring batch to kernel, retry submission on next tick.
- **Kernel Unsupported `io_uring` Opcode**: Abort worker initialization with clear log detailing missing kernel feature requirements.

---

## 15. MVP Scope

The Minimum Viable Product (MVP) provides a fully verifiable foundation focused strictly on correctness and basic forwarding:
- **Core Engine**: Single-worker thread with dedicated `io_uring` ring.
- **Protocol**: Raw L4 TCP proxying with timeout-bounded half-close.
- **Backend Selection**: Static backend pool using Round-Robin load balancing.
- **Buffer Management**: Ring-buffer based allocation pool with explicit checked-out tracking and reference-counted cleanup on close.
- **Backpressure**: Pause client reads and accept SQEs when buffer pool utilization reaches 100%.
- **Observability**: Basic stdout/JSON logging of connection counts, bytes transferred, and errors.
- **Verification**: Unit tests and end-to-end integration tests verifying byte-perfect payload transmission.

---

## 16. Advanced Scope

Features to be added after MVP baseline correctness is verified:
- **Multi-Worker Execution**: `SO_REUSEPORT` multishot accept across CPU-pinned workers.
- **L7 HTTP/1.1 Engine**: Zero-copy header parsing, keep-alive backend pooling, strict request smuggling protection (HTTP 400).
- **Advanced Load Balancing**: Weighted Round-Robin, Least Connections, Consistent Hashing.
- **Health Checking & Reliability**: Active TCP/HTTP probes, circuit breaking with exponential backoff.
- **Advanced io_uring Features**: Registered/fixed buffers (`IORING_REGISTER_BUFFERS`), fixed file descriptors (`IORING_REGISTER_FILES`).
- **Telemetry**: Full Prometheus endpoint exposing latency histograms and io_uring ring stats.

---

## 17. Experimental & Research Scope (Hypothesis-Driven)

The following items are explicit experimental hypotheses to be measured and validated rather than assumed:

- **Hypothesis H-1 (io_uring vs epoll)**: *An io_uring-native L4/L7 proxy will achieve higher throughput and lower p99 latency than an equivalent epoll-based proxy under high concurrency.*
  - **Measurement Plan**: Benchmark Helios against an epoll implementation built on identical data structures under `wrk2` load (10K to 100K concurrent connections).
- **Hypothesis H-2 (Fixed Buffer Registration)**: *Pre-registering fixed buffers with `io_uring` (`IORING_REGISTER_BUFFERS`) will measurably reduce kernel page-mapping overhead.*
  - **Measurement Plan**: Compare CPU cycles per 1M requests with fixed buffers enabled vs standard buffer submission using `perf stat`.
- **Hypothesis H-3 (SQPOLL Performance)**: *Enabling `IORING_SETUP_SQPOLL` kernel thread polling will increase peak throughput for I/O bound workloads at the expense of higher idle CPU utilization.*
  - **Measurement Plan**: Profile QPS vs CPU power/utilization with and without SQPOLL across varying QPS load levels.
- **Hypothesis H-4 (Zero-Copy Transfer)**: *Utilizing `splice` or `MSG_ZEROCOPY` in io_uring will yield throughput gains only for payload sizes exceeding 64 KB due to setup overhead.*
  - **Measurement Plan**: Benchmark payload sizes from 512 bytes to 1MB comparing copy-based ring buffers vs `splice`/`MSG_ZEROCOPY`.
- **Hypothesis H-5 (Target QPS & Latency Limits)**: *Helios can achieve 200,000+ HTTP reqs/sec per node with p99 added proxy latency under 300 microseconds.*
  - **Measurement Plan**: Run automated load test using `wrk2` on bare-metal hardware with dual 10GbE interfaces.

---

## 18. Acceptance Criteria

- **AC-001 (Data Integrity)**: 100GB of random binary data transferred through L4 TCP proxy matches MD5 checksum on client and backend sides exactly.
- **AC-002 (Protocol Compliance)**: HTTP/1.1 parser passes 100% of invalid/smuggling test vectors from standard HTTP compliance test suites (e.g., HTTP Garden / smuggling suites).
- **AC-003 (Resource Safety)**: Zero memory leaks reported by ASan and zero data races reported by TSan under a 24-hour continuous stress test at 50,000 active connections.
- **AC-004 (Backpressure Effectiveness)**: Under simulated backend stalls (0 bytes/sec read rate), memory consumption of Helios worker remains constant at fixed buffer pool cap without growth.
- **AC-005 (Graceful Shutdown)**: When `SIGTERM` is issued during active 10,000 QPS load, zero existing connections suffer abrupt resets; all active requests complete within the 15s drain window before exit.
- **AC-006 (Reproducible Benchmark Suite)**: Executing `make benchmark` automatically generates comparison plots (QPS, p50/p95/p99 latency, CPU usage) comparing Helios against epoll baseline with statistically significant confidence intervals.

---

## 19. Constraints

- **CON-001 (OS Kernel)**: Requires Linux kernel version 5.19 or newer (to support multishot accept and stable ring buffer capabilities).
- **CON-002 (Compiler)**: Requires GCC 11+ or Clang 13+ with C++20 standard support.
- **CON-003 (No External Loop Dependencies)**: Core data path must rely strictly on Linux system calls and `liburing` without external runtime event frameworks.
- **CON-004 (Deterministic Execution)**: All asynchronous state transitions must be driven strictly by `io_uring` CQEs or explicit timer ring events.

---

## 20. Assumptions

- **ASM-001**: Target deployment environment is Linux on x86_64 or ARM64 architectures.
- **ASM-002**: Network interface cards (NICs) support standard Linux socket options and multi-queue RSS for effective CPU core distribution.
- **ASM-003**: System administrators have appropriate permissions (`CAP_NET_BIND_SERVICE`, `RLIMIT_MEMLOCK` adjustments for fixed buffers) when running performance configurations.
- **ASM-004**: Initial backend pools communicate over cleartext TCP or HTTP/1.1 (TLS passthrough supported at L4; TLS termination deferred to advanced scope).

---

## 21. Open Questions

- **OQ-001**: Should ring buffer management use `io_uring`'s native provided buffer feature (`IORING_OP_PROVIDE_BUFFERS` / buffer rings) for multishot accept/read, or a custom user-space ring buffer array?
  - *Status*: Open for initial investigation during MVP benchmarking.
- **OQ-002**: How should cross-worker control messages (e.g., dynamic weight updates or shared health status updates) be communicated given the shared-nothing principle?
  - *Status*: Proposed solution uses `eventfd` or atomic ring queues polled outside hot-path data loops.
- **OQ-003**: What is the optimal fixed buffer block size (e.g., 4KB vs 16KB vs 64KB) for balancing memory footprint against I/O throughput across mixed payload sizes?
  - *Status*: To be resolved empirically in optimization phase.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: L4 TCP baseline forwards 100% of data correctly across 50,000 concurrent client connections per worker without memory growth beyond memory cap.
- **SC-002**: Measurable added p99 latency introduced by Helios is quantified and benchmarked against an epoll baseline across 1K, 10K, and 50K concurrent connection tiers.
- **SC-003**: Under backend failover, 100% of new client requests are re-routed to healthy backends within 1 second of failure detection.
- **SC-004**: Zero memory leaks (ASan), zero undefined behavior (UBSan), and zero data races (TSan) across entire test suite.
