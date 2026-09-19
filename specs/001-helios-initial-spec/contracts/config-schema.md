# Configuration Schema Specification: Helios

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Completed  

---

## 1. Declarative YAML Configuration Specification

```yaml
version: "1.0"

server:
  workers: 4                      # Number of worker threads (0 = auto-detect CPU cores)
  cpu_affinity: true             # Pin workers to dedicated physical CPU cores
  io_uring_entries: 4096         # SQ/CQ ring size per worker
  metrics_port: 9090             # HTTP Prometheus metrics port

listeners:
  - name: "l4_tcp_frontend"
    address: "0.0.0.0"
    port: 8080
    protocol: "L4_TCP"
    backend_pool: "tcp_backends"
    tcp_nodelay: true
    so_reuseport: true
    rcvbuf_bytes: 131072
    sndbuf_bytes: 131072

  - name: "l7_http_frontend"
    address: "0.0.0.0"
    port: 80
    protocol: "L7_HTTP1"
    backend_pool: "web_backends"
    strict_http_parsing: true

backend_pools:
  - name: "tcp_backends"
    algorithm: "round_robin"      # round_robin | weighted_round_robin | least_connections | consistent_hashing
    backends:
      - address: "127.0.0.1"
        port: 9001
        weight: 1
      - address: "127.0.0.1"
        port: 9002
        weight: 2

  - name: "web_backends"
    algorithm: "least_connections"
    backends:
      - address: "10.0.1.10"
        port: 8080
        weight: 1
      - address: "10.0.1.11"
        port: 8080
        weight: 1

health_checks:
  interval_seconds: 5.0
  timeout_seconds: 2.0
  unhealthy_threshold: 3
  healthy_threshold: 2
  http_path: "/health"

limits:
  max_connections_per_worker: 10000
  buffer_pool_mb_per_worker: 64
  buffer_block_bytes: 16384
  client_header_timeout_seconds: 5.0
  half_close_timeout_seconds: 5.0
  graceful_drain_timeout_seconds: 15.0
  backend_connect_timeout_seconds: 1.0
```
