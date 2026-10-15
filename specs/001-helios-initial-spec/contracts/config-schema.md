# Configuration Schema Specification: Helios

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Active Specification  

---

## 1. Declarative YAML Configuration Specification

Helios accepts declarative YAML configuration files specifying server lifecycle options, worker concurrency, backend load balancing pools, timeouts, active health checking, and Prometheus telemetry exposition.

```yaml
# Helios Configuration Specification
version: "1.0"

server:
  host: "0.0.0.0"                 # Listening IP address (0.0.0.0 for all interfaces)
  port: 8080                      # Listening TCP port (1-65535)
  workers: 0                      # Number of worker threads (0 = auto-detect hardware CPU cores)
  cpu_affinity: true             # Pin each worker thread to a dedicated physical CPU core
  max_connections: 50000          # Maximum concurrent connections allowed
  buffer_pool_blocks: 4096       # Fixed-capacity memory pool buffer blocks per worker
  buffer_block_size: 16384        # Byte capacity per buffer block (default 16 KB)
  io_uring_entries: 512          # Submission / Completion ring depth per worker (power of 2)

backend_pools:
  - name: "default_pool"          # Unique identifier for the backend pool
    algorithm: "round_robin"      # round_robin | weighted_round_robin | least_connections | consistent_hash
    endpoints:                    # List of upstream target endpoints
      - host: "127.0.0.1"
        port: 9001
        weight: 3
      - host: "127.0.0.1"
        port: 9002
        weight: 1

timeouts:
  idle_timeout_ms: 30000          # Inactivity timeout before closing idle connections
  header_read_timeout_ms: 5000    # Slowloris defense timeout for HTTP header parsing
  connect_timeout_ms: 2000        # Timeout for establishing upstream backend TCP connections
  half_close_fallback_ms: 5000    # Fallback timer for half-closed connections before force close
  drain_timeout_ms: 15000         # Maximum time allowed for active connections to drain upon shutdown

health_check:
  enabled: true                   # Enable asynchronous io_uring active probing
  interval_ms: 5000               # Frequency between consecutive health probes
  timeout_ms: 1000                # Connect probe timeout before failure
  unhealthy_threshold: 3          # Consecutive failures required to transition to Unhealthy
  healthy_threshold: 2            # Consecutive successes required to recover to Healthy

metrics:
  enabled: true                   # Expose Prometheus HTTP telemetry endpoint
  port: 9090                      # Telemetry server listening port
  path: "/metrics"                # Telemetry scraping endpoint path
```

---

## 2. Schema Model & Type Definitions

### 2.1 Server Configuration (`server`)

| Field | Type | Default | Constraints | Description |
| :--- | :--- | :--- | :--- | :--- |
| `host` | `string` | `"0.0.0.0"` | Valid IPv4/IPv6 address | Listening host interface |
| `port` | `uint16` | `8080` | `1..65535` (or `0` for test binding) | Listening TCP port |
| `workers` | `uint32` | `0` | `0..256` (`0` = auto CPU count) | Thread-per-core worker concurrency |
| `cpu_affinity` | `bool` | `true` | `true`/`false` | Whether to pin workers to CPU cores |
| `max_connections` | `uint32` | `10000` | `1..1000000` | Connection table capacity limit |
| `buffer_pool_blocks`| `uint32` | `1024` | `64..131072` | Number of fixed buffers in pool |
| `buffer_block_size` | `uint32` | `16384` | `1024..65536` | Memory block size in bytes |
| `io_uring_entries` | `uint32` | `512` | `64..16384` (power of 2) | Ring SQ/CQ depth |

### 2.2 Backend Pools Configuration (`backend_pools`)

| Field | Type | Default | Constraints | Description |
| :--- | :--- | :--- | :--- | :--- |
| `name` | `string` | Required | Non-empty alphanumeric string | Backend pool name |
| `algorithm` | `string` | `"round_robin"` | `round_robin`, `weighted_round_robin`, `least_connections`, `consistent_hash` | Load balancing policy |
| `endpoints` / `backends` | `array` | Required | At least 1 endpoint | Upstream server list |

#### Endpoint Object (`endpoints[]`)

| Field | Type | Default | Constraints | Description |
| :--- | :--- | :--- | :--- | :--- |
| `host` / `address` | `string` | Required | Non-empty IP or hostname | Upstream host IP |
| `port` | `uint16` | Required | `1..65535` | Upstream TCP port |
| `weight` | `uint32` | `1` | `>= 1` | Scheduling weight for weighted algorithms |

### 2.3 Timeouts Configuration (`timeouts`)

| Field | Type | Default | Constraints | Description |
| :--- | :--- | :--- | :--- | :--- |
| `idle_timeout_ms` | `uint32` | `30000` | `100..600000` | Connection idle timeout |
| `header_read_timeout_ms` | `uint32` | `5000` | `100..60000` | HTTP header read timeout |
| `connect_timeout_ms` | `uint32` | `2000` | `50..30000` | Backend connect timeout |
| `half_close_fallback_ms` | `uint32` | `5000` | `100..60000` | Half-close grace period |
| `drain_timeout_ms` | `uint32` | `15000` | `500..300000` | Shutdown drain grace timeout |

### 2.4 Health Checking Configuration (`health_check`)

| Field | Type | Default | Constraints | Description |
| :--- | :--- | :--- | :--- | :--- |
| `enabled` | `bool` | `false` | `true`/`false` | Enable active health probes |
| `interval_ms` | `uint32` | `5000` | `>= 100` | Time between consecutive probes |
| `timeout_ms` | `uint32` | `1000` | `>= 50` | Probe timeout threshold |
| `unhealthy_threshold` | `uint32` | `3` | `>= 1` | Failures to mark Unhealthy |
| `healthy_threshold` | `uint32` | `2` | `>= 1` | Successes to mark Healthy |

### 2.5 Metrics & Observability Configuration (`metrics`)

| Field | Type | Default | Constraints | Description |
| :--- | :--- | :--- | :--- | :--- |
| `enabled` | `bool` | `true` | `true`/`false` | Enable Prometheus HTTP telemetry |
| `port` | `uint16` | `9090` | `1..65535` (or `0` in tests) | Telemetry HTTP listening port |
| `path` | `string` | `"/metrics"` | Must start with `'/'` | Prometheus endpoint path |

---

## 3. Strict Validation & Error Handling (CFG-002)

1. **Syntax Errors**: If the YAML file contains malformed indentation, invalid tokens, or unclosed quotes, parsing MUST fail immediately, reporting the file path, line number, and a descriptive syntax message.
2. **Schema & Range Violations**:
   - `server.port` must be between `1` and `65535` (or `0` when explicitly permitted in unit/integration test harnesses).
   - `server.buffer_block_size` must be at least 1024 bytes and power-of-two aligned.
   - `server.io_uring_entries` must be a power of two.
   - At least one backend pool must be defined, and each backend pool must define at least one valid endpoint.
   - `algorithm` must be one of the recognized algorithms (`round_robin`, `weighted_round_robin`, `least_connections`, `consistent_hash`).
   - Endpoint weights must be `>= 1`.
3. **Fail-Fast Semantics**: Any configuration validation failure MUST prevent worker pool and proxy initialization, returning a non-zero exit code (`1`) with clear console error diagnostics.
