cmake_minimum_required(VERSION 3.22)

project(helios
    VERSION 0.1.0
    DESCRIPTION "Linux-native high-performance reverse proxy and load balancer"
    LANGUAGES CXX C
)

# Enforce C++20 Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Debug build if non specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(HELIOS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(HELIOS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(HELIOS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(HELIOS_BUILD_TESTS "Build unit and integration tests" ON)
option(HELIOS_BUILD_BENCHMARKS "Build benchmark targets" ON)

# Strict Compiler Warnings Target
add_library(helios_compiler_flags INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(helios_compiler_flags INTERFACE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )
elseif(MSVC)
    target_compile_options(helios_compiler_flags INTERFACE
        /W4
        /WX
        /permissive-
    )
endif()

# Sanitizers Configuration
add_library(helios_sanitizers INTERFACE)

if(HELIOS_ENABLE_ASAN AND HELIOS_ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled simultaneously.")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(SANITIZER_FLAGS "")
    if(HELIOS_ENABLE_ASAN)
        list(APPEND SANITIZER_FLAGS "-fsanitize=address")
        message(STATUS "AddressSanitizer (ASan) enabled")
    endif()
    if(HELIOS_ENABLE_UBSAN)
        list(APPEND SANITIZER_FLAGS "-fsanitize=undefined")
        message(STATUS "UndefinedBehaviorSanitizer (UBSan) enabled")
    endif()
    if(HELIOS_ENABLE_TSAN)
        list(APPEND SANITIZER_FLAGS "-fsanitize=thread")
        message(STATUS "ThreadSanitizer (TSan) enabled")
    endif()

    if(SANITIZER_FLAGS)
        target_compile_options(helios_sanitizers INTERFACE ${SANITIZER_FLAGS} -fno-omit-frame-pointer)
        target_link_options(helios_sanitizers INTERFACE ${SANITIZER_FLAGS})
    endif()
endif()

# CTest Integration
if(HELIOS_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif()

# External Dependencies via FetchContent
include(FetchContent)

if(HELIOS_BUILD_TESTS)
    message(STATUS "Configuring GoogleTest dependency...")
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    # Prevent overriding parent project's compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(HELIOS_BUILD_BENCHMARKS)
    message(STATUS "Configuring Google Benchmark dependency...")
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
endif()

# Add Subdirectories
add_subdirectory(src)

if(HELIOS_BUILD_TESTS OR HELIOS_BUILD_BENCHMARKS)
    add_subdirectory(tests)
endif()
