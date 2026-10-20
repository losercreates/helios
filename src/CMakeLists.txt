find_package(PkgConfig QUIET)

add_library(helios_lib STATIC
    foundation.cpp
    io/ring_engine.cpp
    buffer/buffer_pool.cpp
    net/socket_utils.cpp
    net/tcp_listener.cpp
    net/tcp_connection.cpp
    net/connection_pair.cpp
    net/l4_proxy_server.cpp
    net/listener.cpp
    worker.cpp
    lb/backend.cpp
    lb/round_robin.cpp
    lb/weighted_round_robin.cpp
    lb/least_connections.cpp
    lb/consistent_hash.cpp
    lb/health_checker.cpp
    lb/circuit_breaker.cpp
    http/http_parser.cpp
    backend/backend_pool.cpp
    metrics/metrics_collector.cpp
    metrics/prometheus_server.cpp
    config/config_loader.cpp
    logging/access_logger.cpp
    routing/l7_router.cpp
    http/http_rewriter.cpp
)

target_include_directories(helios_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/io
    ${CMAKE_CURRENT_SOURCE_DIR}/buffer
    ${CMAKE_CURRENT_SOURCE_DIR}/net
    ${CMAKE_CURRENT_SOURCE_DIR}/lb
    ${CMAKE_CURRENT_SOURCE_DIR}/http
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/logging
    ${CMAKE_CURRENT_SOURCE_DIR}/routing
)

target_link_libraries(helios_lib PUBLIC
    helios_compiler_flags
    helios_sanitizers
    uring
)

add_executable(helios
    main.cpp
)

target_link_libraries(helios PRIVATE
    helios_lib
    helios_compiler_flags
    helios_sanitizers
)
