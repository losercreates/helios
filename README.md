# Helios

Helios is a high-performance Linux reverse proxy and load balancer
built in C++ using io_uring.

The project explores:

- asynchronous Linux I/O
- io_uring
- buffer ownership and lifetime
- zero-copy-oriented networking
- L4 TCP proxying
- L7 HTTP proxying
- thread-per-core concurrency
- backpressure
- load balancing
- health checking
- fault tolerance
- performance engineering
- kernel-level observability
