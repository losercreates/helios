# Prometheus Telemetry Metrics Schema: Helios

**Feature Branch**: `001-helios-initial-spec`  
**Created**: 2026-09-19  
**Status**: Completed  

---

## 1. Metrics Endpoint Output Specification (`GET /metrics`)

### 1. Throughput & Connections
```text
# HELP helios_requests_total Total number of HTTP requests processed
# TYPE helios_requests_total counter
helios_requests_total{worker="0", protocol="http1"} 104523

# HELP helios_bytes_read_total Total bytes read from client sockets
# TYPE helios_bytes_read_total counter
helios_bytes_read_total{worker="0"} 540239042

# HELP helios_bytes_written_total Total bytes written to client sockets
# TYPE helios_bytes_written_total counter
helios_bytes_written_total{worker="0"} 1240239042

# HELP helios_active_connections Current active client connections
# TYPE helios_active_connections gauge
helios_active_connections{worker="0"} 412
```

### 2. Latency Histograms
```text
# HELP helios_added_latency_microseconds Added proxy latency in microseconds
# TYPE helios_added_latency_microseconds histogram
helios_added_latency_microseconds_bucket{worker="0", le="50"} 84210
helios_added_latency_microseconds_bucket{worker="0", le="100"} 99420
helios_added_latency_microseconds_bucket{worker="0", le="300"} 104100
helios_added_latency_microseconds_bucket{worker="0", le="+Inf"} 104523
helios_added_latency_microseconds_sum{worker="0"} 4210940
helios_added_latency_microseconds_count{worker="0"} 104523
```

### 3. `io_uring` Kernel Ring Metrics
```text
# HELP helios_iouring_sqe_submitted_total Total io_uring SQEs submitted
# TYPE helios_iouring_sqe_submitted_total counter
helios_iouring_sqe_submitted_total{worker="0"} 209046

# HELP helios_iouring_cqe_reaped_total Total io_uring CQEs reaped
# TYPE helios_iouring_cqe_reaped_total counter
helios_iouring_cqe_reaped_total{worker="0"} 209046

# HELP helios_iouring_sq_overflow_total io_uring kernel SQ ring overflow count
# TYPE helios_iouring_sq_overflow_total counter
helios_iouring_sq_overflow_total{worker="0"} 0
```

### 4. Buffer & Resource Backpressure
```text
# HELP helios_buffer_pool_utilization Buffer pool utilization ratio (0.0 - 1.0)
# TYPE helios_buffer_pool_utilization gauge
helios_buffer_pool_utilization{worker="0"} 0.35

# HELP helios_backpressure_pause_events_total Number of times read/accept SQEs paused due to backpressure
# TYPE helios_backpressure_pause_events_total counter
helios_backpressure_pause_events_total{worker="0"} 4
```
