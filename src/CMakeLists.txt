add_library(helios_lib STATIC
    foundation.cpp
)

target_include_directories(helios_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(helios_lib PUBLIC
    helios_compiler_flags
    helios_sanitizers
)

add_executable(helios
    main.cpp
)

target_link_libraries(helios PRIVATE
    helios_lib
    helios_compiler_flags
    helios_sanitizers
)
