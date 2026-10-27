add_library(saturn_sh2
    src/analysis.cpp
    src/block_lifter.cpp
    src/decoder.cpp
    src/differential.cpp
    src/format.cpp
    src/cfg.cpp
    src/lifter.cpp
    src/module_lifter.cpp
    src/runtime.cpp
)

add_library(saturn::sh2 ALIAS saturn_sh2)

target_include_directories(saturn_sh2
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(saturn_sh2 PUBLIC saturn::core saturn::sir)
target_compile_features(saturn_sh2 PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(saturn_sh2 PRIVATE /W4 /permissive-)
else()
    target_compile_options(saturn_sh2 PRIVATE -Wall -Wextra -Wpedantic)
endif()
