cmake_minimum_required(VERSION 3.20)

project(saturnbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SATURNBOX_BUILD_TESTS "Build SaturnBox unit tests" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(cmake/Musashi.cmake)

option(SATURNBOX_IPO "Enable interprocedural optimization in Release builds" OFF)
if(SATURNBOX_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SATURNBOX_IPO_SUPPORTED OUTPUT SATURNBOX_IPO_ERROR LANGUAGES CXX)
    if(NOT SATURNBOX_IPO_SUPPORTED)
        message(FATAL_ERROR "Release IPO is unavailable: ${SATURNBOX_IPO_ERROR}")
    endif()
    # Apply to our targets below, leaving the external 68000 core and its
    # code generator on their separately validated compiler options.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if(ENABLE_SANITIZERS)
    if(MSVC)
        message(STATUS "Enabling AddressSanitizer for MSVC (UndefinedBehaviorSanitizer is unavailable)")
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/DEBUG /INCREMENTAL:NO)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
        set(SANITIZER_FLAGS
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
            -g2
        )
        add_compile_options(${SANITIZER_FLAGS})
        add_link_options(${SANITIZER_FLAGS})
    else()
        message(FATAL_ERROR
            "ENABLE_SANITIZERS is supported only with MSVC, Clang, or GCC")
    endif()
endif()

add_subdirectory(libs/core)
add_subdirectory(libs/sir)
add_subdirectory(libs/sh2)
add_subdirectory(libs/saturn)
add_subdirectory(tools/saturn-sh2)
add_subdirectory(tools/saturn-aot)
if(SATURNBOX_MUSASHI_68K)
    add_subdirectory(tools/saturn-scsp-probe)
endif()

if(SATURNBOX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
