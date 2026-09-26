find_package(PkgConfig QUIET)

add_library(helios_lib STATIC
    foundation.cpp
    io/ring_engine.cpp
)

target_include_directories(helios_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/io
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
