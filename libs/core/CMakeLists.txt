add_library(saturn_core
    src/memory_image.cpp
)

add_library(saturn::core ALIAS saturn_core)

target_include_directories(saturn_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(saturn_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(saturn_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(saturn_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
