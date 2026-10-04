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
    lb/backend.cpp
    lb/round_robin.cpp
    lb/weighted_round_robin.cpp
)

target_include_directories(helios_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/io
    ${CMAKE_CURRENT_SOURCE_DIR}/buffer
    ${CMAKE_CURRENT_SOURCE_DIR}/net
    ${CMAKE_CURRENT_SOURCE_DIR}/lb
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
