add_library(saturn_sir
    src/interpreter.cpp
    src/module.cpp
    src/static_module.cpp
    src/win64_jit.cpp
)

add_library(saturn::sir ALIAS saturn_sir)

target_include_directories(saturn_sir
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(saturn_sir PUBLIC saturn::core)
target_compile_features(saturn_sir PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(saturn_sir PRIVATE /W4 /permissive-)
else()
    target_compile_options(saturn_sir PRIVATE -Wall -Wextra -Wpedantic)
endif()
